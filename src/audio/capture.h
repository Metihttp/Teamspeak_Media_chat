#pragma once

// 2.2 voice: where a voice message's sound comes from. The recorder (voicerecorder.h) pulls 16-bit mono
// samples from a CaptureBackend on its worker thread: WasapiCapture (the microphone, wasapicapture.h)
// or FakeCapture (generated sound for tests and harnesses, fakecapture.h). Every method except
// interrupt() is called on that one worker thread, which has COM initialized (multithreaded).

#include <QString>

#include <cstdint>
#include <vector>

namespace voice {

// Why the microphone can't be used (or stopped). Each has its own text and fix in the recorder window.
enum class CaptureError {
    None,
    PrivacyBlocked,    // Windows' microphone privacy switch blocks desktop apps
    NoDevice,          // no microphone
    Busy,              // another app uses it in exclusive mode
    Disconnected,      // unplugged / disabled while open
    UnsupportedFormat, // a mix format we can't convert (not 44.1 / 48 kHz)
    Generic,           // anything else (see the code)
    EndOfStream,       // FakeCapture only: the generated sound is over (a normal stop)
};

struct CaptureFormat {
    int sampleRate = 48000; // 48000 or 44100 (what the AAC encoder takes)
    int channels   = 1;     // what read() delivers is always mono
};

struct CaptureResult {
    CaptureError error = CaptureError::None;
    long         code  = 0; // HRESULT for Generic (shown as "error 0x...")
    QString      detail;    // for logs only (no names)
};

class CaptureBackend
{
  public:
    virtual ~CaptureBackend() = default;

    // Opens the device and starts capturing. On success fills format and deviceName (the friendly name,
    // for the window; may be empty).
    virtual CaptureResult open(CaptureFormat* format, QString* deviceName) = 0;
    // Appends what was captured since the last call (mono, 16 bit, format.sampleRate) to out. Waits up
    // to timeoutMs for sound to arrive; returns early when interrupt() is called.
    virtual CaptureResult read(std::vector<int16_t>& out, int timeoutMs) = 0;
    virtual void          close() = 0;
    // Any thread: wakes a read() that is waiting (stop / cancel).
    virtual void interrupt() = 0;
};

} // namespace voice
