#include "music/musicengine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#include "music/musicmix.h"

namespace music {

namespace {

// ---- the file source -------------------------------------------------------------------------------------

class FileSource : public Source
{
  public:
    explicit FileSource(const QString& path)
        : m_path(path)
    {
    }
    ~FileSource() override
    {
        m_reader.reset(); // its COM objects go before the thread's COM / Media Foundation scope
        m_scope.reset();
    }
    bool open(QString* error, long* code) override
    {
        m_scope  = std::make_unique<mf::ThreadScope>();
        m_reader = std::make_unique<mf::MusicReader>();
        if (m_reader->open(m_path))
            return true;
        *error = m_reader->error();
        *code  = m_reader->code();
        return false;
    }
    mf::MusicInfo info() const override { return m_reader ? m_reader->info() : mf::MusicInfo(); }
    Read          read(std::vector<float>* out, qint64* first) override
    {
        switch (m_reader->read(out, first)) {
        case mf::MusicReader::Read::Data:
            return Read::Data;
        case mf::MusicReader::Read::End:
            return Read::End;
        case mf::MusicReader::Read::Error:
            break;
        }
        return Read::Error;
    }
    bool    seek(qint64 frame) override { return m_reader->seek(frame); }
    QString error() const override { return m_reader ? m_reader->error() : QString(); }
    long    code() const override { return m_reader ? m_reader->code() : 0; }

  private:
    QString                          m_path;
    std::unique_ptr<mf::ThreadScope> m_scope;
    std::unique_ptr<mf::MusicReader> m_reader;
};

qint64 steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 2.3 polish: frames / 48, not frames * 1000 / 48000: the same for every frame number, without the
// overflow beyond about 6 years of frames (a file claiming any duration, and a seek to its end, is shown).
static_assert(kRate % 1000 == 0, "whole frames per millisecond");
constexpr qint64 framesToMs(qint64 frames)
{
    return frames / (kRate / 1000);
}

// ---- dispatch to the active engine ---------------------------------------------------------------------------

std::atomic<Engine*> g_engine{nullptr};
std::atomic<int>     g_inside{0};

} // namespace

std::unique_ptr<Source> fileSource(const QString& path)
{
    return std::make_unique<FileSource>(path);
}

// ---- Engine ----------------------------------------------------------------------------------------------

Engine::Engine(quint64 sch, std::unique_ptr<Source> source, std::function<qint64()> clock)
    : m_sch(sch)
    , m_clock(std::move(clock))
    , m_source(std::move(source))
{
    m_ring.assign(static_cast<size_t>(kRingFrames * kChannels), 0.0f);
    m_sendBlock.assign(static_cast<size_t>(kMaxBlock * kChannels), 0.0f);
    m_monitorBlock.assign(static_cast<size_t>(kMaxBlock * kChannels), 0.0f);
}

Engine::~Engine()
{
    if (g_engine.load() == this)
        setActive(nullptr); // no callback is inside it any more
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    m_source.reset(); // never started: nothing of it ran on a worker
}

qint64 Engine::now() const
{
    return m_clock ? m_clock() : steadyMs();
}

void Engine::start()
{
    if (m_worker.joinable() || !m_source)
        return;
    m_worker = std::thread([this] { run(); });
}

void Engine::pause()
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_state == State::Playing)
        m_state = State::Paused;
    else if (m_state == State::Opening)
        m_pauseWanted = true;
}

void Engine::resume()
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_state == State::Paused)
        m_state = State::Playing;
    m_pauseWanted = false;
}

void Engine::seek(qint64 ms)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_state == State::Failed || m_state == State::Ended)
            return;
        // Clamped in milliseconds first: a file may claim any duration, and a frame number must never be a
        // signed overflow (a negative place: the ring would be written before its start).
        static_assert(kRate % 1000 == 0, "whole frames per millisecond");
        constexpr qint64 kFramesPerMs = kRate / 1000;
        qint64           targetMs     = qBound<qint64>(0, ms, std::numeric_limits<qint64>::max() / kFramesPerMs);
        if (m_info.durationMs > 0)
            targetMs = qMin(targetMs, m_info.durationMs);
        const qint64 target = targetMs * kFramesPerMs;
        m_seekTarget  = target;
        m_seekPending = true;
        m_live[Send] = m_live[Monitor] = false; // nothing of the old place plays meanwhile
    }
    m_wake.notify_all();
}

void Engine::setGain(float gain)
{
    m_gain.store(qBound(0.0f, gain, 1.0f));
}

void Engine::setVoiceAllowed(bool allowed)
{
    m_voiceAllowed.store(allowed);
}

void Engine::setSendAllowed(bool allowed)
{
    m_sendAllowed.store(allowed);
}

void Engine::setMonitorAllowed(bool allowed)
{
    m_monitorAllowed.store(allowed);
}

void Engine::fadeOut(int timeoutMs)
{
    pause();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(qMax(0, timeoutMs));
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_state != State::Paused || m_seekPending)
                return; // opening, failed, or nothing of the old place left to fade out
            const qint64 at      = now();
            bool         waiting = false;
            for (int side = 0; side < 2; ++side) // a side TeamSpeak still calls, still playing: its fade out comes
                waiting = waiting || (m_live[side] && m_last[side] >= 0 && at - m_last[side] <= kFreshMs);
            if (!waiting)
                return;
        }
        if (std::chrono::steady_clock::now() >= until)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool Engine::alive(int side, qint64 at) const
{
    return m_last[side] >= 0 && at - m_last[side] <= kAliveMs;
}

int Engine::leader(qint64 at) const
{
    if (alive(Send, at))
        return Send;
    if (alive(Monitor, at))
        return Monitor;
    return -1;
}

Engine::Snapshot Engine::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    const qint64 at = now();
    Snapshot     s;
    s.state      = m_state;
    s.info       = m_info;
    s.error      = m_error;
    s.code       = m_code;
    s.sending    = alive(Send, at);
    s.monitoring = alive(Monitor, at);
    s.mixing     = m_mixedAt >= 0 && at - m_mixedAt <= kAliveMs;
    s.underruns  = m_underruns;
    const int lead = leader(at);
    qint64    pos  = lead >= 0 ? m_cursor[lead] : qMax(m_cursor[Send], m_cursor[Monitor]);
    if (m_seekPending)
        pos = m_seekTarget;
    s.positionMs = framesToMs(qMax<qint64>(0, pos));
    s.durationMs = m_info.durationMs > 0 ? m_info.durationMs : (m_eof ? framesToMs(m_ringEnd) : 0);
    // The end: everything decoded was played by the side that leads (or by both, when neither is called).
    if (m_state == State::Playing && m_eof && !m_seekPending) {
        const bool done = lead >= 0 ? m_cursor[lead] >= m_ringEnd : (m_cursor[Send] >= m_ringEnd && m_cursor[Monitor] >= m_ringEnd);
        if (done)
            s.state = m_errorAtEnd ? State::Failed : State::Ended; // a damaged file: played up to the damage
    }
    if (s.state == State::Ended && s.durationMs > 0)
        s.positionMs = s.durationMs;
    return s;
}

qint64 Engine::decodedFrames(bool* all) const
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (all)
        *all = m_eof;
    return m_ringEnd;
}

int Engine::copyOut(qint64 from, float* out, int frames) const
{
    const qint64 have = m_ringEnd - from;
    const int    n    = static_cast<int>(qBound<qint64>(0, have, frames));
    for (int done = 0; done < n;) {
        const qint64 index = (from + done) % kRingFrames;
        const int    run   = static_cast<int>(qMin<qint64>(n - done, kRingFrames - index));
        std::memcpy(out + static_cast<size_t>(done) * kChannels, m_ring.data() + index * kChannels, static_cast<size_t>(run) * kChannels * sizeof(float));
        done += run;
    }
    return n;
}

int Engine::take(Side side, float* out, int frames)
{
    if (!out || frames <= 0)
        return 0;
    const qint64                at = now();
    std::lock_guard<std::mutex> lock(m_lock);
    const int                   other      = 1 - side;
    const bool                  wasAlive   = alive(side, at);
    const bool                  otherAlive = alive(other, at);
    // 2.3 review: away for kBackMs or more (push-to-talk let go and opened again on the next tick, a device
    // hiccup) and now more than kSnapFrames from the other side counts as coming back too. Before, only
    // kAliveMs did: a 200 ms capture gap left the send side 200 ms behind, and the monitor then snapped back
    // to it and played those 200 ms here again.
    const bool                  wasAway    = m_last[side] >= 0 && at - m_last[side] >= kBackMs;
    m_last[side]                           = at;
    if (m_state != State::Playing || m_seekPending) {
        // Right after a pause: one short block fading out from where it stopped (the place doesn't move,
        // so resuming fades in from there again). Otherwise nothing.
        int n = 0;
        if (m_live[side] && m_state == State::Paused && !m_seekPending && m_cursor[side] >= m_ringStart) {
            n = copyOut(m_cursor[side], out, qMin(frames, kFadeFrames));
            fade(out, n, false);
        }
        m_live[side] = false;
        return n;
    }
    // Both sides play the same moment: one coming (back) starts where the other is; the monitor follows
    // the send side when they drift apart, while TeamSpeak still captures (a send side that just stopped
    // is not followed back to where it stopped).
    const bool otherFresh = m_last[other] >= 0 && at - m_last[other] <= kFreshMs;
    const bool apart      = std::llabs(m_cursor[side] - m_cursor[other]) > kSnapFrames;
    if (otherAlive && (!wasAlive || (wasAway && apart) || (side == Monitor && otherFresh && apart))) {
        if (m_cursor[side] != m_cursor[other])
            m_live[side] = false;
        m_cursor[side] = m_cursor[other];
    }
    if (m_cursor[side] < m_ringStart) {
        m_cursor[side] = m_ringStart;
        m_live[side]   = false;
    }
    const qint64 have = m_ringEnd - m_cursor[side];
    if (!m_live[side] && have < kPrebuffer && !m_eof)
        return 0; // a start or a seek: a little decoded first
    const int n = copyOut(m_cursor[side], out, frames);
    if (n <= 0)
        return 0;
    if (!m_live[side]) {
        fade(out, qMin(n, kFadeFrames), true);
        m_live[side] = true;
    }
    m_cursor[side] += n;
    if (n < frames && !m_eof) { // the decoder fell behind: fade in again once there is more
        ++m_underruns;
        m_live[side] = false;
    }
    return n;
}

void Engine::captured(qint16* samples, int frames, int channels, int* edited)
{
    if (!samples || !edited || frames <= 0 || channels <= 0 || channels > 8)
        return;
    const float gain  = m_gain.load();
    const bool  voice = m_voiceAllowed.load();
    const bool  send  = m_sendAllowed.load(); // false: TeamSpeak's microphone or sound is muted
    const int   entry = *edited;
    bool        mixed = false, silenced = false;
    for (int done = 0; done < frames;) {
        const int part = qMin(kMaxBlock, frames - done);
        // The send side isn't called while sending isn't allowed (the monitor leads meanwhile). Muted music
        // (gain 0) still moves the place but isn't music for the voice: nothing is sent for it. 2.3 review:
        // the gain moves from the last block's to the new one across the block, so a volume change, mute or
        // unmute never steps (a click for the whole channel); muting sends that one fading block.
        const int   got  = send ? take(Send, m_sendBlock.data(), part) : 0;
        const float from = m_sideGain[Send] < 0.0f ? gain : m_sideGain[Send];
        m_sideGain[Send] = gain;
        const bool music = got > 0 && (gain > 0.0f || from > 0.0f);
        if (music)
            ramp(m_sendBlock.data(), got, from, gain);
        const CaptureDecision d     = decideCapture(entry, music, voice);
        qint16* const         block = samples + static_cast<size_t>(done) * channels;
        if (d.mix) {
            // The microphone's ramp starts from the last block's share; never from the voice when it isn't allowed.
            mixCapture(block, part, channels, m_sendBlock.data(), got, 1.0f, voice ? m_micGain : 0.0f, d.micGain);
            m_micGain = d.micGain;
            mixed     = true;
        } else if (d.silence) {
            std::fill(block, block + static_cast<size_t>(part) * channels, qint16(0));
            m_micGain = 0.0f;
            silenced  = true;
        } else {
            m_micGain = 1.0f;
        }
        done += part;
    }
    // One answer for the whole block: sent with the music in it, else silence not sent, else TeamSpeak's.
    const int result = mixed ? entry | kEditedChanged | kEditedSend : silenced ? (entry | kEditedChanged) & ~kEditedSend : entry;
    if (result != entry)
        *edited = result;
    if (mixed) { // (also when *edited already said so on entry: the bar's badge follows what was mixed)
        std::lock_guard<std::mutex> lock(m_lock);
        m_mixedAt = now();
    }
}

void Engine::playback(qint16* samples, int frames, int channels, const unsigned* speakers, unsigned* fillMask)
{
    if (!samples || frames <= 0 || channels <= 0 || channels > 32)
        return;
    // 2.3 review: TeamSpeak's sound muted, nothing is added here (faded out); the place moves on all the same.
    const float gain = m_monitorAllowed.load() ? m_gain.load() : 0.0f;
    for (int done = 0; done < frames;) {
        const int   part = qMin(kMaxBlock, frames - done);
        const int   got  = take(Monitor, m_monitorBlock.data(), part);
        const float from = m_sideGain[Monitor] < 0.0f ? gain : m_sideGain[Monitor];
        m_sideGain[Monitor] = gain;
        if (got > 0 && (gain > 0.0f || from > 0.0f)) { // ramped like the send side: no click on a change
            ramp(m_monitorBlock.data(), got, from, gain);
            mixPlayback(samples + static_cast<size_t>(done) * channels, part, channels, speakers, fillMask, m_monitorBlock.data(), got, 1.0f);
        }
        done += part;
    }
}

void Engine::trim()
{
    const qint64 at   = now();
    qint64       keep = -1;
    for (int side = 0; side < 2; ++side) {
        if (alive(side, at))
            keep = keep < 0 ? m_cursor[side] : qMin(keep, m_cursor[side]);
    }
    if (keep < 0) // neither side plays: keep what either still needs
        keep = qMin(m_cursor[Send], m_cursor[Monitor]);
    m_ringStart = qBound(m_ringStart, keep, m_ringEnd);
    if (m_ringEnd - m_ringStart > kRingFrames) // never: the producer only writes into free room
        m_ringStart = m_ringEnd - kRingFrames;
}

void Engine::run()
{
    std::unique_ptr<Source> source;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        source = std::move(m_source);
    }
    QString error;
    long    code = 0;
    if (!source || !source->open(&error, &code)) {
        std::lock_guard<std::mutex> lock(m_lock);
        m_state = State::Failed;
        m_error = error;
        m_code  = code;
        source.reset(); // on this thread
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_info  = source->info();
        m_state = m_pauseWanted ? State::Paused : State::Playing;
    }

    std::vector<float> pending; // decoded, not in the ring yet
    size_t             pendingAt = 0; // floats of pending already moved
    qint64             next      = 0; // the frame the next decoded piece starts at
    qint64             skipUntil = 0; // after a seek: decoded frames before this are dropped
    bool               afterSeek = false;
    pending.reserve(static_cast<size_t>(kRate) * kChannels);

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_lock);
            if (m_stop)
                break;
            if (m_seekPending) {
                const qint64 target = m_seekTarget;
                lock.unlock();
                const bool moved = source->seek(target);
                lock.lock();
                if (m_stop)
                    break;
                if (m_seekPending && m_seekTarget != target)
                    continue; // another seek came meanwhile
                pending.clear();
                pendingAt = 0;
                if (moved) {
                    m_ringStart = m_ringEnd = target;
                    m_cursor[Send] = m_cursor[Monitor] = target;
                    next      = target;
                    skipUntil = target;
                    afterSeek = true;
                    m_eof     = false;
                    m_errorAtEnd = false; // back before a damaged part: it plays again up to there
                } else { // the file can't seek: it plays on where it was
                    m_cursor[Send] = m_cursor[Monitor] = qMax(m_cursor[Send], m_cursor[Monitor]);
                }
                m_live[Send] = m_live[Monitor] = false;
                m_seekPending                  = false;
                continue;
            }
            trim();
            if (pendingAt < pending.size()) { // what was decoded goes in as far as there is room
                const qint64 room   = kRingFrames - (m_ringEnd - m_ringStart);
                const qint64 frames = qMin<qint64>(room, static_cast<qint64>((pending.size() - pendingAt) / kChannels));
                for (qint64 f = 0; f < frames; ++f) {
                    const qint64 index                   = (m_ringEnd + f) % kRingFrames;
                    m_ring[static_cast<size_t>(index * 2)]     = pending[pendingAt + static_cast<size_t>(f) * 2];
                    m_ring[static_cast<size_t>(index * 2 + 1)] = pending[pendingAt + static_cast<size_t>(f) * 2 + 1];
                }
                m_ringEnd += frames;
                pendingAt += static_cast<size_t>(frames) * kChannels;
                if (pendingAt < pending.size()) {
                    m_wake.wait_for(lock, std::chrono::milliseconds(10));
                    continue;
                }
                pending.clear();
                pendingAt = 0;
            }
            const int    lead  = leader(now());
            const qint64 front = lead >= 0 ? m_cursor[lead] : qMax(m_cursor[Send], m_cursor[Monitor]);
            if (m_eof || m_state == State::Failed || m_ringEnd - front >= kAheadFrames) {
                m_wake.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
        }
        // Decoding happens outside the lock: the audio threads never wait for it.
        std::vector<float> piece;
        qint64             first = -1;
        const Source::Read r     = source->read(&piece, &first);
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_stop)
            break;
        if (m_seekPending)
            continue; // decoded for the old place
        if (r == Source::Read::Error) { // what was decoded still plays; then it fails (snapshot)
            m_eof        = true;
            m_errorAtEnd = true;
            m_error      = source->error();
            m_code       = source->code();
            continue;
        }
        if (r == Source::Read::End) {
            m_eof = true;
            continue;
        }
        qint64 at = next;
        if (afterSeek && first >= 0)
            at = first;
        const qint64 frames = static_cast<qint64>(piece.size() / kChannels);
        qint64       skip   = 0;
        if (afterSeek) {
            if (at < skipUntil) {
                skip = qMin(frames, skipUntil - at);
            } else if (at > m_ringEnd) { // the seek landed a little later: the track goes on from there
                m_ringStart = m_ringEnd = at;
                m_cursor[Send] = m_cursor[Monitor] = at;
            }
            if (skip < frames)
                afterSeek = false;
        }
        next = at + frames;
        if (skip < frames) {
            pending.assign(piece.begin() + static_cast<std::ptrdiff_t>(skip * kChannels), piece.end());
            pendingAt = 0;
        }
    }
    source.reset(); // on this thread: its COM / Media Foundation scope
}

// ---- dispatch ----------------------------------------------------------------------------------------------

void setActive(Engine* engine)
{
    Engine* previous = g_engine.exchange(engine);
    if (previous == engine)
        return;
    while (g_inside.load() != 0) // a callback that took the previous one finishes with it
        std::this_thread::yield();
}

Engine* active()
{
    return g_engine.load();
}

void onCaptured(quint64 sch, short* samples, int frames, int channels, int* edited)
{
    ++g_inside;
    Engine* engine = g_engine.load();
    if (engine && engine->connection() == sch)
        engine->captured(samples, frames, channels, edited);
    --g_inside;
}

void onPlayback(quint64 sch, short* samples, int frames, int channels, const unsigned int* speakers, unsigned int* fillMask)
{
    ++g_inside;
    Engine* engine = g_engine.load();
    if (engine && engine->connection() == sch)
        engine->playback(samples, frames, channels, speakers, fillMask);
    --g_inside;
}

} // namespace music
