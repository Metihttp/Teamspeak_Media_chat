#pragma once

// 2.3 live music: one track playing into one connection's voice.
//
// A worker thread decodes ahead (a Source: mf::MusicReader for files, generated sound in the tests) into a
// ring of 48 kHz stereo, about two seconds ahead of what is played. TeamSpeak's audio threads take from it:
//   Send     ts3plugin_onEditCapturedVoiceDataEvent: mixed into the outgoing voice (what the channel hears);
//   Monitor  ts3plugin_onEditMixedPlaybackVoiceDataEvent: added to what this computer plays.
// Each side has its own place in the track. The send side leads while TeamSpeak captures; the monitor
// follows it (snapped back when they drift apart by more than 100 ms). When one side stops being called
// (push-to-talk released, the capture device closed), the other leads, and the side coming back starts
// where the leader is (2.3 review: also after a short gap that left it behind). Nothing is called on the
// GUI thread's behalf: the controller polls snapshot().
//
// The audio callbacks never allocate and hold the engine's lock only to copy one block. setActive() is how
// the callbacks reach the engine; clearing it waits until no callback is inside it, so the engine can then
// be deleted. Deleting it joins the worker.

#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "video/mfmusicreader.h"

namespace music {

// What the worker decodes. Every call happens on the worker thread (open first), and it is destroyed there.
class Source
{
  public:
    enum class Read { Data, End, Error };
    virtual ~Source() = default;
    virtual bool          open(QString* error, long* code) = 0;
    virtual mf::MusicInfo info() const = 0;
    // The next piece appended to out (interleaved stereo float at 48 kHz); *first: its first frame, -1 if
    // unknown. On Error, error() says why.
    virtual Read    read(std::vector<float>* out, qint64* first) = 0;
    virtual bool    seek(qint64 frame) = 0;
    virtual QString error() const = 0;
    virtual long    code() const = 0;
};

// A music file through Media Foundation (COM and Media Foundation for the worker thread inside).
std::unique_ptr<Source> fileSource(const QString& path);

class Engine
{
  public:
    enum class State { Opening, Playing, Paused, Ended, Failed };
    enum Side { Send = 0, Monitor = 1 };

    static constexpr int    kAliveMs       = 300;          // a side not called for this long is not playing
    static constexpr int    kBackMs        = 100;          // a side away this long, now kSnapFrames apart, rejoins the other
    static constexpr int    kFreshMs       = 60;           // ... the send side called this recently is followed
    static constexpr qint64 kSnapFrames    = 4800;         // 100 ms apart: the monitor jumps to the send side
    static constexpr qint64 kPrebuffer     = 4800;         // after a start or a seek, 100 ms decoded first
    static constexpr int    kFadeFrames    = 240;          // 5 ms fades: no clicks at start, seek, pause
    static constexpr qint64 kAheadFrames   = 2 * 48000;    // decoded ahead of the leader
    static constexpr qint64 kRingFrames    = 6 * 48000;    // what the ring holds
    static constexpr int    kMaxBlock      = 9600;         // larger callback blocks are mixed in parts

    struct Snapshot {
        State         state       = State::Opening;
        qint64        positionMs  = 0;
        qint64        durationMs  = 0; // from the file; 0 if it doesn't say (then known at its end)
        bool          sending     = false; // the send side is being called (TeamSpeak captures)
        bool          monitoring  = false;
        bool          mixing      = false; // music went into the voice within the last kAliveMs
        mf::MusicInfo info;
        QString       error;      // Failed: why, for the user
        long          code        = 0;
        int           underruns   = 0; // blocks the decoder didn't have ready (diagnostics)
    };

    // sch: the connection whose voice gets the music. clock (tests): milliseconds; empty: a steady clock.
    Engine(quint64 sch, std::unique_ptr<Source> source, std::function<qint64()> clock = {});
    ~Engine(); // the worker stopped and joined (the source goes on it)
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    void start(); // the worker opens the source and decodes; plays once something is decoded
    void pause();
    void resume();
    void seek(qint64 ms);
    void setGain(float gain);           // the music's level in both sides
    void setVoiceAllowed(bool allowed); // false: none of the microphone goes out, with or without music
    // false: the music doesn't go into the voice and TeamSpeak's decision stands (its microphone or sound is
    // muted: the plugin never makes a muted client send); the monitor side plays on.
    void setSendAllowed(bool allowed);
    // 2.3 review: false while TeamSpeak's sound is muted: the monitor side keeps the place but adds nothing
    // (faded out), so "nobody hears it, not even you" holds whatever TeamSpeak's mixer still runs.
    void setMonitorAllowed(bool allowed);
    // 2.3 review, GUI thread: pauses, then waits (at most timeoutMs) until each side TeamSpeak is calling has
    // played its short fade out, so a stop, or a pause before a push-to-talk input is given back, never
    // cuts the music off mid-wave (a click for the channel and here). Returns at once when nothing plays.
    void fadeOut(int timeoutMs);
    quint64  connection() const { return m_sch; }
    Snapshot snapshot() const;
    // How far the worker has decoded (the ring's end, in frames); *all: the source's end was reached.
    qint64 decodedFrames(bool* all = nullptr) const;

    // ---- TeamSpeak's audio threads (through onCaptured / onPlayback) -------------------------------------
    void captured(qint16* samples, int frames, int channels, int* edited);
    void playback(qint16* samples, int frames, int channels, const unsigned* speakers, unsigned* fillMask);

    // Up to frames frames of the track for side (tests, and the two above): what was copied (faded in after
    // a start, a seek or a resume; faded out right after a pause).
    int take(Side side, float* out, int frames);

  private:
    void   run();
    qint64 now() const;
    bool   alive(int side, qint64 at) const; // under m_lock
    int    leader(qint64 at) const;          // under m_lock: -1 if no side is alive
    void   trim();                           // under m_lock
    int    copyOut(qint64 from, float* out, int frames) const; // under m_lock

    const quint64            m_sch;
    std::function<qint64()>  m_clock;
    std::unique_ptr<Source>  m_source; // until the worker takes it
    std::thread              m_worker;

    mutable std::mutex      m_lock;
    std::condition_variable m_wake;
    std::vector<float>      m_ring; // kRingFrames x 2
    qint64                  m_ringStart = 0; // absolute frames held: [m_ringStart, m_ringEnd)
    qint64                  m_ringEnd   = 0;
    qint64                  m_cursor[2] = {0, 0};
    qint64                  m_last[2]   = {-1, -1}; // when each side was last called
    bool                    m_live[2]   = {false, false}; // its output is faded in
    qint64                  m_mixedAt   = -1;
    State                   m_state     = State::Opening;
    bool                    m_pauseWanted = false; // pause() while still opening
    bool                    m_eof       = false;
    bool                    m_errorAtEnd = false; // the decoder failed: what came before plays, then Failed
    bool                    m_stop      = false;
    bool                    m_seekPending = false;
    qint64                  m_seekTarget  = 0;
    mf::MusicInfo           m_info;
    QString                 m_error;
    long                    m_code      = 0;
    int                     m_underruns = 0;

    std::atomic<float> m_gain{0.25f};
    std::atomic<bool>  m_voiceAllowed{true};
    std::atomic<bool>  m_sendAllowed{true};
    std::atomic<bool>  m_monitorAllowed{true};
    // Audio-thread scratch (each used by its own side only).
    std::vector<float> m_sendBlock;
    std::vector<float> m_monitorBlock;
    float              m_micGain = 1.0f; // the capture side's last microphone share
    float              m_sideGain[2] = {-1.0f, -1.0f}; // each side's last music gain (-1: none yet), ramped from
};

// ---- the TeamSpeak callbacks -------------------------------------------------------------------------------

// GUI thread: the engine the callbacks reach (nullptr: none). Returns once no callback is inside the
// previous one, which may then be deleted.
void setActive(Engine* engine);
Engine* active(); // any thread, an atomic read (plugin.cpp's event filter, the tests): never dereferenced there

// TeamSpeak's audio threads (plugin.cpp). Nothing happens unless sch is the active engine's connection.
void onCaptured(quint64 sch, short* samples, int frames, int channels, int* edited);
void onPlayback(quint64 sch, short* samples, int frames, int channels, const unsigned int* speakers, unsigned int* fillMask);

} // namespace music
