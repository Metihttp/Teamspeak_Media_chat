#include "audio/fakecapture.h"

#include <QElapsedTimer>

#include <windows.h>

#include <cmath>

namespace voice {

namespace {

qint64 nowMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

} // namespace

FakeCapture::FakeCapture(const Options& options)
    : m_options(options)
{
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FakeCapture::~FakeCapture()
{
    close();
    if (m_wake)
        CloseHandle(static_cast<HANDLE>(m_wake));
}

CaptureResult FakeCapture::open(CaptureFormat* format, QString* deviceName)
{
    if (m_options.openError != CaptureError::None) {
        CaptureResult r;
        r.error  = m_options.openError;
        r.code   = static_cast<long>(0x80004005L); // E_FAIL, shown for Generic
        r.detail = QStringLiteral("fake open failure");
        return r;
    }
    format->sampleRate = m_options.sampleRate;
    format->channels   = 1;
    if (deviceName)
        *deviceName = m_options.deviceName;
    m_delivered = 0;
    m_startMs   = nowMs();
    m_open      = true;
    return {};
}

CaptureResult FakeCapture::read(std::vector<int16_t>& out, int timeoutMs)
{
    if (!m_open)
        return {CaptureError::Generic, static_cast<long>(0x80004005L), QStringLiteral("not open")};
    const int    rate      = m_options.sampleRate;
    const qint64 packet    = qMax(1, rate * m_options.packetMs / 1000);
    qint64       available = packet;
    if (m_options.realtime) {
        // As a microphone: only what "happened" since the start, waiting for the next packet if needed.
        qint64 due = (nowMs() - m_startMs) * rate / 1000 - m_delivered;
        if (due < packet) {
            const qint64 waitMs = qMin<qint64>(timeoutMs, (packet - due) * 1000 / rate + 1);
            if (WaitForSingleObject(static_cast<HANDLE>(m_wake), static_cast<DWORD>(qMax<qint64>(0, waitMs))) == WAIT_OBJECT_0)
                return {}; // interrupted
            due = (nowMs() - m_startMs) * rate / 1000 - m_delivered;
        }
        available = due >= packet ? due : 0;
    }
    auto limitTo = [&](qint64 ms) {
        if (ms < 0)
            return false;
        const qint64 frames = ms * rate / 1000;
        if (m_delivered >= frames)
            return true;
        available = qMin(available, frames - m_delivered);
        return false;
    };
    if (limitTo(m_options.endAfterMs))
        return {CaptureError::EndOfStream, 0, QStringLiteral("end of the generated sound")};
    if (limitTo(m_options.unplugAfterMs))
        return {CaptureError::Disconnected, static_cast<long>(0x88890004L), QStringLiteral("fake unplug")};

    const double amplitude = 32767.0 * std::pow(10.0, m_options.levelDb / 20.0);
    out.reserve(out.size() + static_cast<size_t>(available));
    for (qint64 i = 0; i < available; ++i) {
        const qint64 n     = m_delivered + i;
        double       value = 0.0;
        switch (m_options.signal) {
        case Signal::Tone:
            value = amplitude * std::sin(2.0 * 3.14159265358979323846 * m_options.toneHz * static_cast<double>(n) / rate);
            break;
        case Signal::Noise:
            m_noise = m_noise * 1664525u + 1013904223u;
            value   = amplitude * (static_cast<double>(m_noise >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0);
            break;
        case Signal::Silence:
            break;
        }
        out.push_back(static_cast<int16_t>(std::lround(value)));
    }
    m_delivered += available;
    return {};
}

void FakeCapture::close()
{
    m_open = false;
}

void FakeCapture::interrupt()
{
    if (m_wake)
        SetEvent(static_cast<HANDLE>(m_wake));
}

} // namespace voice
