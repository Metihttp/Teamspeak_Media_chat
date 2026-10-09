#pragma once

// Video support built on Windows Media Foundation (part of Windows, nothing extra to ship).
// TeamSpeak does not bundle QtMultimedia, so decoding/playback goes through IMFMediaEngine
// (frame-server mode, D3D11 with WARP fallback) and IMFSourceReader for probing.

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

struct ProbeResult {
    bool    ok = false;
    QSize   size;            // display size in pixels (pixel aspect ratio applied)
    qint64  durationMs = 0;
    bool    hasVideo   = false;
    bool    hasAudio   = false;
    QImage  poster;          // a representative frame (around 1 s, or 10% for short clips), max side posterMaxSide
    QString error;
};

// Synchronous. Safe to call from any thread (initializes COM/MF for that thread as needed).
ProbeResult probe(const QString& path, int posterMaxSide = 960);

// Plays one local file. Video frames are delivered as QImages at setFrameSize() (device pixels),
// audio goes to the default Windows output device. All methods and signals are GUI-thread only.
class VideoPlayer : public QObject
{
    Q_OBJECT

  public:
    explicit VideoPlayer(QObject* parent = nullptr);
    ~VideoPlayer() override; // shuts the engine down synchronously; no callbacks after this returns

    void open(const QString& path); // asynchronous: emits loaded() or failed()
    void close();
    bool isLoaded() const;

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
