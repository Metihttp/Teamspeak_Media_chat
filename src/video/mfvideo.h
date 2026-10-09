#pragma once

// Video support built on Windows Media Foundation (part of Windows, nothing extra to ship).
// TeamSpeak does not bundle QtMultimedia, so decoding/playback goes through IMFMediaEngine
// (frame-server mode, D3D11 with WARP fallback) and IMFSourceReader for probing. Audio files play
// through the same engine in its audio-only mode, without any graphics device.

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

namespace mf {

// Reference-counted MFStartup/MFShutdown. Call startup() once from the GUI thread at plugin init
// and shutdown() at plugin shutdown after every VideoPlayer has been destroyed.
bool startup();
void shutdown();

// Media Foundation can be used on this computer (false on Windows N editions without the Media
// Feature Pack). Cheap after the first call; safe on any thread.
bool available();
// 2.2 diagnostics: the Direct3D device the most recent VideoPlayer got (any thread).
enum class VideoDevice { NotUsedYet = 0, Hardware = 1, Warp = 2, Failed = 3 };
VideoDevice lastVideoDevice();

struct ProbeResult {
    bool    ok = false;
    QSize   size;            // display size in pixels (pixel aspect ratio applied)
    qint64  durationMs = 0;
    bool    hasVideo   = false;
    bool    hasAudio   = false;
    QImage  poster;          // a representative frame (around 1 s, or 10% for short clips), max side posterMaxSide
    QString error;
    // 2.4 compress: what the compression planner needs to know.
    double  frameRate       = 0.0; // nominal frames per second of the video stream, 0 if unknown
    int     rotation        = 0;   // clockwise degrees that turn the stored picture upright (0/90/180/270)
    QString videoCodec;            // readable codec name ("H.264", "HEVC (H.265)", "VP9"), empty if unknown
    int     audioChannels   = 0;
    int     audioSampleRate = 0;
};

// Synchronous. Safe to call from any thread (initializes COM/MF for that thread as needed).
ProbeResult probe(const QString& path, int posterMaxSide = 960);

// How VideoPlayer::open() sets up the engine.
//  Auto:      video and audio (a D3D11 device renders the frames).
//  AudioOnly: sound only, no graphics device at all and no frames; for audio files. A video stream
//             in the file is ignored. Error texts talk about audio.
enum class OpenMode { Auto, AudioOnly };

// Plays one local file. Video frames are delivered as QImages at setFrameSize() (device pixels),
// audio goes to the default Windows output device. All methods and signals are GUI-thread only.
class VideoPlayer : public QObject
{
    Q_OBJECT

  public:
    explicit VideoPlayer(QObject* parent = nullptr);
    ~VideoPlayer() override; // shuts the engine down synchronously; no callbacks after this returns

    void open(const QString& path, OpenMode mode = OpenMode::Auto); // asynchronous: emits loaded() or failed()
    void close();
    bool isLoaded() const;
    bool isAudioOnly() const;       // the current file was opened with OpenMode::AudioOnly
    bool hasGraphicsDevice() const; // a D3D11 device exists for the current file (tests, diagnostics)

    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const;
    bool isEnded() const;

    void   seek(qint64 ms);
    qint64 position() const; // ms
    qint64 duration() const; // ms, 0 if unknown
    QSize  videoSize() const;

    void   setMuted(bool muted);
    bool   isMuted() const;
    void   setVolume(double volume); // 0..1
    double volume() const;
    void   setLoop(bool loop);

    // Playback speed (1.0 = normal), after loaded(). setPlaybackRate() returns false (and changes
    // nothing) when the engine does not support the rate for this file. open() resets it to 1.0.
    bool   isPlaybackRateSupported(double rate) const;
    bool   setPlaybackRate(double rate);
    double playbackRate() const;

    // Frames are scaled to exactly this size (callers keep the aspect ratio). Default: video size.
    // When the picture changes size later (videoSizeChanged()), frames show the new picture fitted
    // into this size, at its own aspect ratio, until the next call.
    void   setFrameSize(const QSize& pixels);
    QImage currentFrame() const; // Format_ARGB32_Premultiplied, null until the first frame

  signals:
    void loaded();
    void failed(const QString& error);
    void frameReady();               // currentFrame() changed
    void stateChanged();             // playing / paused / ended / muted changed
    void positionChanged(qint64 ms); // throttled to a few times per second
    void videoSizeChanged();         // videoSize() changed after loaded() (the stream switched resolution or aspect)

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace mf
