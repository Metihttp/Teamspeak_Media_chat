#pragma once

// 2.2 voice: a generated "microphone" for unit tests, harnesses and test hooks: a tone, noise or
// silence, optionally failing to open or "unplugged" after a while. Never touches a real device.

#include <atomic>

#include "audio/capture.h"

namespace voice {

class FakeCapture : public CaptureBackend
{
  public:
    enum class Signal { Tone, Noise, Silence };

    struct Options {
        Signal       signal       = Signal::Tone;
        double       toneHz       = 440.0;
        double       levelDb      = -12.0; // peak level of the tone / noise
        int          sampleRate   = 48000;
        int          packetMs     = 10;
        bool         realtime     = true;  // paced like a microphone (sleeps); false: as fast as read() is called
        qint64       endAfterMs   = -1;    // >= 0: EndOfStream once this much sound was delivered
        qint64       unplugAfterMs = -1;   // >= 0: Disconnected once this much sound was delivered
        CaptureError openError    = CaptureError::None; // open() fails with this
        QString      deviceName   = QStringLiteral("Test microphone");
    };

    explicit FakeCapture(const Options& options);
    ~FakeCapture() override;

    CaptureResult open(CaptureFormat* format, QString* deviceName) override;
    CaptureResult read(std::vector<int16_t>& out, int timeoutMs) override;
    void          close() override;
    void          interrupt() override;

  private:
    Options           m_options;
    qint64            m_delivered = 0; // frames
    qint64            m_startMs   = 0;
    quint32           m_noise     = 0x12345678u;
    void*             m_wake      = nullptr; // event handle
    std::atomic<bool> m_open{false};
};

} // namespace voice
