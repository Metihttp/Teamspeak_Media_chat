#pragma once

// 2.3 live music: what the music tests share (tst_music.cpp, tst_musicbar.cpp): a generated track (never a
// file, never played), a clock the tests move, and TeamSpeak's voice variables on the fake connection.

#include <QByteArray>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fakets3.h"
#include "music/musicengine.h"
#include "ts3api.h"

namespace musicfakes {

// Where the source's calls ran (the worker, never the GUI thread).
struct SourceLog {
    std::mutex      lock;
    std::thread::id openThread;
    std::thread::id destroyThread;
    std::atomic<int> reads{0};
    std::atomic<int> seeks{0};
    std::atomic<qint64> lastSeek{-1}; // the frame the last seek asked for
    std::atomic<bool> destroyed{false};
};

// 2.3 polish: holds the worker inside Source::seek while held, so a test sees a seek that is still pending
// for as long as it looks (before, a test raced the worker: TestMusic::seekWithAnAbsurdDuration read the
// place right after seek() and once found the refused seek already undone). Never longer than 10 s.
struct Gate {
    std::mutex              lock;
    std::condition_variable wake;
    bool                    held = false;
    void hold()
    {
        std::lock_guard<std::mutex> guard(lock);
        held = true;
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> guard(lock);
            held = false;
        }
        wake.notify_all();
    }
    void pass()
    {
        std::unique_lock<std::mutex> guard(lock);
        wake.wait_for(guard, std::chrono::seconds(10), [this] { return !held; });
    }
};

// A ramp the tests can check sample by sample: frame f is L = 0.5 * (f % 1000) / 1000, R = -L.
class GenSource : public music::Source
{
  public:
    struct Options {
        qint64  frames   = 5 * 48000;
        int     chunk    = 1152; // like MP3 frames
        int     coarse   = 1152; // a seek lands on a multiple of this, at or before the target
        bool    canSeek  = true; // false: seek() fails (the track plays on where it was)
        qint64  claimedMs = -1;  // the duration info() reports (-1: that of frames), like a file's header
        bool    failOpen = false;
        qint64  failAt   = -1;   // read() fails from this frame on
        QString title;
        QString artist;
        std::shared_ptr<SourceLog> log;
        std::shared_ptr<Gate>      seekGate; // seek() waits here (after it is counted) while the gate is held
    };
    static float left(qint64 frame) { return 0.5f * static_cast<float>(frame % 1000) / 1000.0f; }

    explicit GenSource(Options options)
        : m_o(std::move(options))
    {
    }
    ~GenSource() override
    {
        if (m_o.log) {
            std::lock_guard<std::mutex> lock(m_o.log->lock);
            m_o.log->destroyThread = std::this_thread::get_id();
            m_o.log->destroyed     = true;
        }
    }
    bool open(QString* error, long* code) override
    {
        if (m_o.log) {
            std::lock_guard<std::mutex> lock(m_o.log->lock);
            m_o.log->openThread = std::this_thread::get_id();
        }
        if (m_o.failOpen) {
            *error = QStringLiteral("This file has no sound to play.");
            *code  = 0x0123;
            return false;
        }
        return true;
    }
    mf::MusicInfo info() const override
    {
        mf::MusicInfo i;
        i.title      = m_o.title;
        i.artist     = m_o.artist;
        i.durationMs = m_o.claimedMs >= 0 ? m_o.claimedMs : m_o.frames * 1000 / 48000;
        return i;
    }
    Read read(std::vector<float>* out, qint64* first) override
    {
        if (m_o.log)
            ++m_o.log->reads;
        if (m_o.failAt >= 0 && m_next >= m_o.failAt) {
            m_error = QStringLiteral("The file is damaged.");
            return Read::Error;
        }
        if (m_next >= m_o.frames)
            return Read::End;
        const qint64 n = qMin<qint64>(m_o.chunk, m_o.frames - m_next);
        *first         = m_next;
        for (qint64 f = 0; f < n; ++f) {
            out->push_back(left(m_next + f));
            out->push_back(-left(m_next + f));
        }
        m_next += n;
        return Read::Data;
    }
    bool seek(qint64 frame) override
    {
        if (m_o.log) {
            m_o.log->lastSeek = frame;
            ++m_o.log->seeks;
        }
        if (m_o.seekGate)
            m_o.seekGate->pass();
        if (!m_o.canSeek)
            return false;
        m_next = frame / m_o.coarse * m_o.coarse;
        return true;
    }
    QString error() const override { return m_error; }
    long    code() const override { return m_error.isEmpty() ? 0 : 0x0456; }

  private:
    Options m_o;
    qint64  m_next = 0;
    QString m_error;
};

inline std::unique_ptr<music::Source> gen(GenSource::Options options = {})
{
    return std::make_unique<GenSource>(std::move(options));
}

// A clock the test moves by hand (the engine's "alive" rule).
struct Clock {
    std::shared_ptr<std::atomic<qint64>> ms = std::make_shared<std::atomic<qint64>>(100000);
    std::function<qint64()> fn() const
    {
        auto at = ms;
        return [at] { return at->load(); };
    }
    void advance(qint64 by) { *ms += by; }
};

// TeamSpeak's own voice variables on the fake connection, the visible server tab, the own channel, talk
// power and the channel's codec, as the controller reads them through ts3::funcs.
struct FakeVoice {
    static inline int     inputMuted   = MUTEINPUT_NONE;
    static inline int     outputMuted  = MUTEOUTPUT_NONE;
    static inline int     deactivated  = INPUT_ACTIVE;
    static inline int     hardware     = 1;
    static inline int     talkPower    = 0;
    static inline int     neededPower  = 0;
    static inline int     talker       = 0;
    static inline int     codec        = CODEC_OPUS_MUSIC;
    static inline quint64 visible      = fakets3::kConnection;
    static inline quint64 channel      = fakets3::kChannel;
    static inline int     deactivateSets = 0; // CLIENT_INPUT_DEACTIVATED written by the plugin
    static inline int     lastDeactivatedSet = -1;
    // TeamSpeak's preprocessor "vad" (voice activation): "true", "false", or empty for "can't be read".
    static inline QByteArray vad;
    static inline int        vadOffSets = 0; // 2.3 live test: "vad" set to "false" by the plugin
    static inline int        talking    = 1; // CLIENT_FLAG_TALKING of the own client: TeamSpeak sends our voice
    // Called as the plugin writes a variable, before it is taken (a capture block arriving right then).
    static inline std::function<void(size_t flag, int value)> onSet;

    static unsigned int getSelf(uint64 sch, size_t flag, int* result)
    {
        if (sch != fakets3::kConnection || !ts3::isConnected(sch))
            return ERROR_not_connected;
        switch (flag) {
        case CLIENT_INPUT_MUTED:
            *result = inputMuted;
            return ERROR_ok;
        case CLIENT_OUTPUT_MUTED:
            *result = outputMuted;
            return ERROR_ok;
        case CLIENT_INPUT_DEACTIVATED:
            *result = deactivated;
            return ERROR_ok;
        case CLIENT_INPUT_HARDWARE:
            *result = hardware;
            return ERROR_ok;
        case CLIENT_FLAG_TALKING:
            *result = talking;
            return ERROR_ok;
        default:
            return ERROR_parameter_invalid;
        }
    }
    static unsigned int setSelf(uint64 sch, size_t flag, int value)
    {
        if (sch != fakets3::kConnection || !ts3::isConnected(sch))
            return ERROR_not_connected;
        if (onSet)
            onSet(flag, value);
        if (flag == CLIENT_INPUT_DEACTIVATED) {
            deactivated        = value;
            lastDeactivatedSet = value;
            ++deactivateSets;
        } else if (flag == CLIENT_INPUT_MUTED) {
            inputMuted = value;
        }
        return ERROR_ok;
    }
    static unsigned int flush(uint64 sch, const char*) { return ts3::isConnected(sch) ? ERROR_ok : ERROR_not_connected; }
    static unsigned int clientInt(uint64 sch, anyID, size_t flag, int* result)
    {
        if (sch != fakets3::kConnection)
            return ERROR_parameter_invalid;
        if (flag == CLIENT_TALK_POWER)
            *result = talkPower;
        else if (flag == CLIENT_IS_TALKER)
            *result = talker;
        else
            *result = 0;
        return ERROR_ok;
    }
    static unsigned int channelInt(uint64, uint64, size_t flag, int* result)
    {
        if (flag == CHANNEL_NEEDED_TALK_POWER)
            *result = neededPower;
        else if (flag == CHANNEL_CODEC)
            *result = codec;
        else
            *result = 0; // no password
        return ERROR_ok;
    }
    static unsigned int channelOf(uint64, anyID, uint64* result)
    {
        *result = channel;
        return ERROR_ok;
    }
    static uint64 current() { return visible; }
    static unsigned int preProcessor(uint64 sch, const char* ident, char** result)
    {
        if (sch != fakets3::kConnection || qstrcmp(ident, "vad") != 0 || vad.isEmpty())
            return ERROR_parameter_invalid;
        *result = static_cast<char*>(std::malloc(static_cast<size_t>(vad.size()) + 1)); // freeMemory: std::free
        std::memcpy(*result, vad.constData(), static_cast<size_t>(vad.size()) + 1);
        return ERROR_ok;
    }

    static unsigned int setPreProcessor(uint64 sch, const char* ident, const char* value)
    {
        if (sch != fakets3::kConnection || qstrcmp(ident, "vad") != 0)
            return ERROR_parameter_invalid;
        vad = QByteArray(value);
        if (vad == "false")
            ++vadOffSets;
        return ERROR_ok;
    }

    static void install()
    {
        inputMuted = MUTEINPUT_NONE;
        outputMuted = MUTEOUTPUT_NONE;
        deactivated = INPUT_ACTIVE;
        hardware    = 1;
        talkPower = neededPower = talker = 0;
        codec              = CODEC_OPUS_MUSIC;
        visible            = fakets3::kConnection;
        channel            = fakets3::kChannel;
        deactivateSets     = 0;
        lastDeactivatedSet = -1;
        vad.clear();
        vadOffSets         = 0;
        talking            = 1;
        onSet              = nullptr;
        ts3::funcs.getPreProcessorConfigValue           = &preProcessor;
        ts3::funcs.setPreProcessorConfigValue           = &setPreProcessor;
        ts3::funcs.getClientSelfVariableAsInt           = &getSelf;
        ts3::funcs.setClientSelfVariableAsInt           = &setSelf;
        ts3::funcs.flushClientSelfUpdates               = &flush;
        ts3::funcs.getClientVariableAsInt               = &clientInt;
        ts3::funcs.getChannelVariableAsInt              = &channelInt;
        ts3::funcs.getChannelOfClient                   = &channelOf;
        ts3::funcs.getCurrentServerConnectionHandlerID  = &current;
    }
};

} // namespace musicfakes
