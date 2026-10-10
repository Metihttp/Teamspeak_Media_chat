#pragma once

// 2.4 compress: turns a video into an H.264 (High) + AAC-LC MP4 with Windows Media Foundation, Source
// Reader to Sink Writer, synchronously on the calling (worker) thread. The graphics card is tried first
// (D3D11 device manager, hardware decoder and encoder), then the processor once. The output is checked
// (opens, length, size, upright, sound) before it counts as done. No network, no files other than the
// target. Safe on any thread; cancel and progress go through TranscodeControl's atomics.

#include <QSize>
#include <QString>
#include <QStringList>

#include <atomic>

namespace mf {

struct TranscodeRequest {
    QString source;              // the original, read in place
    QString target;              // the MP4 to write (replaced if it exists)
    QSize   frameSize;           // even, upright display orientation (the planner's)
    int     fps           = 30;  // frames above this rate are dropped
    int     videoKbps     = 2500;
    int     audioKbps     = 128; // 0: no sound
    int     audioChannels = 2;   // 1 | 2
    bool    allowHardware = true;
    quint64 abortAboveBytes = 0; // stop (exceeded) when the projected output is 3% above this; 0: never
    qint64  durationMs      = 0; // of the source: progress and the length check
    int     maxQueuedFrames = sizeof(void*) == 4 ? 4 : 8; // frames waiting for the encoder at most
    int     encoderThreads  = 0;                          // software encoder threads; 0: half the cores (at most 8, 4 in 32-bit)
};

struct TranscodeControl {
    std::atomic<bool> cancel{false};
    std::atomic<int>  permille{0};      // 0..1000 of the source's length written
    std::atomic<bool> finishing{false}; // writing the index and checking the result
    std::atomic<int>  encoder{0};       // 0 not known yet, 1 processor, 2 graphics card
    std::atomic<int>  attempts{0};      // attempts started (2: the processor took over); each starts at permille 0
};

struct TranscodeResult {
    enum class Stage { None, Source, Encoder, Disk, Memory, Stall, Verify };

    bool    ok        = false;
    bool    canceled  = false;
    bool    exceeded  = false; // the projected size passed abortAboveBytes (projectedBytes)
    bool    hardware  = false; // the graphics card encoded the result
    bool    gpuFailed = false; // the graphics card was tried first and failed (the processor took over)
    bool    shorter   = false; // Verify: the result is shorter than the source claims (a cut-off file); not retried
    quint64 bytes          = 0;
    quint64 projectedBytes = 0;
    QSize   frameSize;          // of the result (verified)
    qint64  durationMs = 0;     // of the result (verified)
    qint64  elapsedMs  = 0;
    quint32 hr         = 0;     // the failing HRESULT
    Stage   stage      = Stage::None;
    QString encoderName;        // "NVIDIA H.264 Encoder MFT", "H264 Encoder MFT"
    QString gpuError;           // what failed on the graphics card (log only)
    QString detail;             // English, for the log only
};

// Synchronous; any thread (initializes COM and Media Foundation for it). Returns within about one decoded
// frame after control->cancel is set (no Finalize then; the partial target is deleted).
TranscodeResult transcodeToMp4(const TranscodeRequest& request, TranscodeControl* control);

// The real frame rate of a video file, from the presentation times of its first compressed frames (no
// decoding). Containers sometimes report half of it (WebM and MKV written by ffmpeg). 0 if unknown.
// Synchronous; call on a worker thread with COM and Media Foundation started (mf::probe's thread is fine
// once it returned: this starts both itself).
double measuredFrameRate(const QString& path);

// "source", "encoder", "disk", "memory", "stall", "verify" (log lines and diagnostics).
const char* stageName(TranscodeResult::Stage stage);

// The H.264 encoders of this computer (MFTEnumEx), by their names; hardware ones need a graphics card.
struct EncoderList {
    bool        queried = false; // Media Foundation answered
    QStringList hardware;
    QStringList software;
};
EncoderList h264Encoders(); // synchronous, any thread; takes a few milliseconds

} // namespace mf
