#include "audio/voicerecorder.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimer>

#include <windows.h>

#include <objbase.h>

#include <cmath>
#include <limits>

#include "audio/aacwriter.h"
#include "audio/waveform.h"

namespace voice {

namespace {

constexpr int   kPollMs        = 33;
constexpr int   kReadTimeoutMs = 100;
constexpr int   kGrowSeconds   = 10;    // the recording grows in 10 s steps (never reserved up front)
constexpr float kReleaseDbPerMs = 0.2f; // the meter falls 60 dB in 300 ms

// COM for the worker thread (WASAPI wants it; the encoder sets up its own scope as well).
class ComThread
{
  public:
    ComThread()
        : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }
    ~ComThread()
    {
        if (SUCCEEDED(m_hr))
            CoUninitialize();
    }
    ComThread(const ComThread&)            = delete;
    ComThread& operator=(const ComThread&) = delete;

  private:
    HRESULT m_hr;
};

} // namespace

VoiceRecorder::VoiceRecorder(QObject* parent)
    : QObject(parent)
{
    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &VoiceRecorder::poll);
}

VoiceRecorder::~VoiceRecorder()
{
    cancel();
}

void VoiceRecorder::joinWorker()
{
    if (m_worker.joinable())
        m_worker.join();
}

void VoiceRecorder::setState(State state)
{
    m_state.store(state);
}

void VoiceRecorder::start(BackendFactory factory, qint64 maxMs)
{
    cancel();
    {
        QMutexLocker lock(&m_mutex);
        m_pcm.clear();
        m_pcm.shrink_to_fit();
        m_bins.clear();
        m_format       = CaptureFormat();
        m_deviceName.clear();
        m_captureError = CaptureResult();
        m_encodedPath.clear();
        m_encodedBytes = 0;
        m_encodeError.clear();
        m_encodeCode   = 0;
    }
    m_elapsedMs    = 0;
    m_levelDb      = waveform::kSilenceDb;
    m_loudestDb    = waveform::kSilenceDb;
    m_limitReached = false;
    m_deviceLost   = false;
    m_stop         = false;
    m_cancel       = false;
    setState(State::Opening);
    m_reported   = State::Opening;
    m_workerDone = false;
    m_worker     = std::thread(&VoiceRecorder::captureLoop, this, std::move(factory), maxMs);
    m_poll->start();
}

void VoiceRecorder::stop()
{
    m_stop = true;
    std::shared_ptr<CaptureBackend> backend;
    {
        QMutexLocker lock(&m_mutex);
        backend = m_backend;
    }
    if (backend)
        backend->interrupt();
}

void VoiceRecorder::encode(const QString& path)
{
    const State s = state();
    if (s != State::Captured && s != State::EncodeFailed && s != State::Encoded)
        return;
    joinWorker();
    {
        QMutexLocker lock(&m_mutex);
        m_encodedPath.clear();
        m_encodedBytes = 0;
        m_encodeError.clear();
        m_encodeCode = 0;
    }
    m_cancel = false;
    setState(State::Encoding);
    m_reported   = State::Encoding;
    m_workerDone = false;
    m_worker     = std::thread(&VoiceRecorder::encodeRun, this, path);
    m_poll->start();
}

void VoiceRecorder::cancel()
{
    // An encode still running now is canceled: whatever it writes goes, even if it gets to the end (its
    // last step, the sink writer's Finalize, can't be interrupted). A file of an encode that had already
    // finished is the caller's.
    const bool encoding = state() == State::Encoding;
    m_cancel            = true;
    stop();
    joinWorker();
    m_workerDone = true;
    m_poll->stop();
    QString finished;
    {
        QMutexLocker lock(&m_mutex);
        m_pcm.clear();
        m_pcm.shrink_to_fit();
        m_bins.clear();
        m_backend.reset();
        if (encoding) {
            finished = m_encodedPath;
            m_encodedPath.clear();
            m_encodedBytes = 0;
        }
    }
    if (!finished.isEmpty())
        QFile::remove(finished);
    m_elapsedMs = 0;
    setState(State::Idle);
    m_reported = State::Idle;
    m_cancel   = false;
}

int VoiceRecorder::binCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_bins.size();
}

QVector<float> VoiceRecorder::bins(int from) const
{
    QMutexLocker lock(&m_mutex);
    if (from <= 0)
        return m_bins;
    return from >= m_bins.size() ? QVector<float>() : m_bins.mid(from);
}

CaptureFormat VoiceRecorder::format() const
{
    QMutexLocker lock(&m_mutex);
    return m_format;
}

QString VoiceRecorder::deviceName() const
{
    QMutexLocker lock(&m_mutex);
    return m_deviceName;
}

CaptureResult VoiceRecorder::captureError() const
{
    QMutexLocker lock(&m_mutex);
    return m_captureError;
}

QByteArray VoiceRecorder::waveformLevels() const
{
    return waveform::levelsFromBins(bins());
}

QString VoiceRecorder::encodedPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_encodedPath;
}

qint64 VoiceRecorder::encodedBytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_encodedBytes;
}

QString VoiceRecorder::encodeError() const
{
    QMutexLocker lock(&m_mutex);
    return m_encodeError;
}

long VoiceRecorder::encodeCode() const
{
    QMutexLocker lock(&m_mutex);
    return m_encodeCode;
}

void VoiceRecorder::poll()
{
    if (m_workerDone.load())
        joinWorker();
    const State s = state();
    if (s != m_reported) {
        m_reported = s;
        emit stateChanged();
    }
    if (state() == State::Recording)
        emit tick();
    // A slot above may have started the next run (encode after stop): only stop once nothing runs.
    if (m_workerDone.load() && !m_worker.joinable() && state() != State::Opening && state() != State::Recording && state() != State::Encoding)
        m_poll->stop();
}

// ---- worker thread -------------------------------------------------------------------------------

void VoiceRecorder::captureLoop(BackendFactory factory, qint64 maxMs)
{
    ComThread                       com;
    std::shared_ptr<CaptureBackend> backend(factory ? factory().release() : nullptr);
    // Stopped or canceled while the microphone was being chosen (there was nothing to interrupt yet):
    // it is never opened. Checked again right before open().
    const auto stoppedEarly = [this] {
        if (!m_stop.load() && !m_cancel.load())
            return false;
        {
            QMutexLocker lock(&m_mutex);
            m_backend.reset();
        }
        setState(m_cancel.load() ? State::Idle : State::Captured); // Captured with nothing: "Too short"
        m_workerDone = true;
        return true;
    };
    if (stoppedEarly())
        return;
    if (!backend) {
        QMutexLocker lock(&m_mutex);
        m_captureError        = CaptureResult();
        m_captureError.error  = CaptureError::Generic;
        m_captureError.detail = QStringLiteral("no capture backend");
        setState(State::CaptureFailed);
        m_workerDone = true;
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_backend = backend;
    }
    // A stop from here on also reaches the backend (interrupt()).
    if (stoppedEarly())
        return;

    CaptureFormat       fmt;
    QString             name;
    const CaptureResult opened = backend->open(&fmt, &name);
    if (opened.error != CaptureError::None) {
        backend->close();
        QMutexLocker lock(&m_mutex);
        m_backend.reset();
        m_captureError = opened;
        setState(m_cancel.load() ? State::Idle : State::CaptureFailed);
        m_workerDone = true;
        return;
    }
    const int    rate         = fmt.sampleRate > 0 ? fmt.sampleRate : 48000;
    const qint64 maxFrames    = qMax<qint64>(1, maxMs) * rate / 1000;
    const int    binFrames    = qMax(1, rate * waveform::kBinMs / 1000);
    {
        QMutexLocker lock(&m_mutex);
        m_format     = fmt;
        m_deviceName = name;
    }
    setState(State::Recording);

    std::vector<int16_t> chunk;
    int                  binFill = 0;
    int                  binPeak = 0;
    float                level   = waveform::kSilenceDb;
    float                loudest = waveform::kSilenceDb;
    bool                 failed  = false;
    qint64               frames  = 0;
    while (!m_stop.load()) {
        chunk.clear();
        const CaptureResult r = backend->read(chunk, kReadTimeoutMs);
        // What arrived before an error or the end still counts.
        qint64 take = qMin<qint64>(static_cast<qint64>(chunk.size()), maxFrames - frames);
        if (take > 0) {
            if (m_pcm.capacity() < m_pcm.size() + static_cast<size_t>(take))
                m_pcm.reserve(m_pcm.size() + static_cast<size_t>(qMax<qint64>(take, static_cast<qint64>(rate) * kGrowSeconds)));
            QVector<float> newBins;
            for (qint64 i = 0; i < take; ++i) {
                const int16_t s = chunk[static_cast<size_t>(i)];
                m_pcm.push_back(s);
                const int a = s < 0 ? -static_cast<int>(s) : s;
                binPeak     = qMax(binPeak, a);
                if (++binFill >= binFrames) {
                    const float db = binPeak > 0 ? qMax(waveform::kSilenceDb, static_cast<float>(20.0 * std::log10(binPeak / 32768.0))) : waveform::kSilenceDb;
                    newBins.append(db);
                    loudest = qMax(loudest, db);
                    binFill = 0;
                    binPeak = 0;
                }
            }
            frames += take;
            const float chunkDb = waveform::peakDb(chunk.data(), static_cast<int>(take));
            const float fall    = kReleaseDbPerMs * static_cast<float>(take * 1000 / rate);
            level               = qMax(chunkDb, qMax(waveform::kSilenceDb, level - fall));
            m_levelDb           = level;
            m_loudestDb         = loudest;
            m_elapsedMs         = frames * 1000 / rate;
            if (!newBins.isEmpty()) {
                QMutexLocker lock(&m_mutex);
                m_bins += newBins;
            }
        }
        if (frames >= maxFrames) {
            m_limitReached = true;
            break;
        }
        if (r.error == CaptureError::None)
            continue;
        if (r.error == CaptureError::EndOfStream)
            break;
        QMutexLocker lock(&m_mutex);
        m_captureError = r;
        if (r.error == CaptureError::Disconnected)
            m_deviceLost = true; // the sound so far is kept
        else
            failed = true;
        break;
    }
    backend->close();
    {
        QMutexLocker lock(&m_mutex);
        m_backend.reset();
        if (binFill > 0 && binPeak > 0) // the last partial bin
            m_bins.append(qMax(waveform::kSilenceDb, static_cast<float>(20.0 * std::log10(binPeak / 32768.0))));
    }
    m_levelDb = waveform::kSilenceDb;
    if (m_cancel.load())
        setState(State::Idle);
    else
        setState(failed ? State::CaptureFailed : State::Captured);
    m_workerDone = true;
}

void VoiceRecorder::encodeRun(QString path)
{
    // The recording is not touched by anyone else while this runs (capture has ended).
    const int    rate   = m_format.sampleRate;
    const qint64 frames = static_cast<qint64>(m_pcm.size());
    const float  peak   = waveform::peakDb(m_pcm.data(), static_cast<int>(qMin<qint64>(frames, std::numeric_limits<int>::max())));
    const double gain   = waveform::normalizeGain(peak);
    const AacResult r   = writeAac(path, m_pcm.data(), frames, rate, gain, &m_cancel);
    const bool      canceled = m_cancel.load() || r.canceled;
    if (r.ok && canceled)
        QFile::remove(path); // canceled while the writer finalized: the finished file goes too
    {
        QMutexLocker lock(&m_mutex);
        if (r.ok && !canceled) {
            m_encodedPath  = path;
            m_encodedBytes = QFileInfo(path).size();
        } else if (!r.ok) {
            m_encodeError = r.error;
            m_encodeCode  = r.code;
        }
    }
    if (canceled)
        setState(State::Idle);
    else
        setState(r.ok ? State::Encoded : State::EncodeFailed);
    m_workerDone = true;
}

} // namespace voice
