#pragma once

// Drives everything that moves inside the chat: animated GIFs/WebPs (QMovie) and inline video
// players (mf::VideoPlayer). ChatIntegration asks it what to draw for a key and forwards input;
// it emits frameChanged(key) whenever that key must be redrawn (cheap path: same layout size,
// only the pixels change).

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>

#include "previewrenderer.h"

class Core;
class QMovie;
class QTimer;
namespace mf {
class VideoPlayer;
}

class InlineMediaController : public QObject
{
    Q_OBJECT

  public:
    explicit InlineMediaController(Core* core, QObject* parent = nullptr);
    ~InlineMediaController() override; // destroys all players/movies synchronously

    enum class Mode { Still, Animated, Video };

    // What ChatIntegration should draw for key right now.
    Mode            mode(const QString& key) const;  // Video for every Video-kind entry, Animated while a movie exists
    QImage          frame(const QString& key) const; // current animation/video frame (device pixels), may be null
    PlaybackOverlay overlay(const QString& key) const;

    // Keys whose previews are currently on screen in a visible chat tab. Animations run only while
    // visible (and, when autoplayGifs is off, only while hovered). Playing videos keep playing while
    // scrolled away or in a hidden tab.
    void setVisibleKeys(const QSet<QString>& keys);
    // Keys that still have a preview in some chat document (visible or not). A video whose preview
    // is gone (chat tab closed, chat cleared) for about a second is stopped and its player closed:
    // nothing would be left to pause it with.
    void setPresentKeys(const QSet<QString>& keys);
    // Device-pixel size the preview of key is drawn at (frames are produced at this size).
    void setFrameSize(const QString& key, const QSize& devicePixels);

    // Input on a preview. For videos: Body/PlayPause toggle playback (downloading the file first if
    // needed), Seek jumps to seekFraction, Mute toggles mute, Expand emits openRequested.
    // The zone is applied as given; pointer input on hidden controls is mapped to Body by the caller.
    // For other kinds only hover matters (GIF hover-to-play); clicks are handled by ChatIntegration.
    void click(const QString& key, VideoZone zone, double seekFraction);
    void hover(const QString& key, VideoZone zone); // empty key = mouse left all previews

    void pauseAll();
    void stopAll();
    void settingsChanged(); // after the settings dialog: picks up a changed "start videos muted"

    // Session mute shared by all inline players (the mute button, "start videos muted", the viewer).
    bool isMuted() const { return m_muted; }
    void setMuted(bool muted);

  signals:
    void frameChanged(const QString& key);
    void openRequested(const QString& key);   // expand button: open in MediaViewer
    void playbackStarted(const QString& key); // an inline video starts playing (other players should pause)

    // ---- implementation (owned by inlinemedia.cpp; may be reorganised freely) ------------------
  private:
    struct Video;
    struct Animation;

    void onEntryChanged(const QString& key);
    void startVideo(const QString& key);
    void ensureAnimation(const QString& key);
    void updateAnimations();
    void tick();

    Video* videoRecord(const QString& key);
    void   play(Video* video);
    void   destroyPlayer(Video* video);
    void   destroyAnimation(const QString& key);
    void   makeRoomForPlayer(const Video* keep);
    void   applySettings(Video* video);
    bool   controlsShown(const Video* video) const;
    bool   isVideo(const QString& key) const;
    bool   isAnimatable(const QString& key);
    QSize  videoFrameSize(const Video* video) const;
    void   updateTimer();

    Core*                          m_core;
    QHash<QString, Video*>         m_videos;
    QHash<QString, Animation*>     m_animations;
    QHash<QString, QSize>          m_frameSizes;
    QSet<QString>                  m_visible;
    QString                        m_hoverKey;
    VideoZone                      m_hoverZone = VideoZone::None;
    QTimer*                        m_timer     = nullptr;

    QHash<QString, bool> m_animatable;     // key -> file is an animation we are allowed to play
    quint64              m_useClock   = 0; // LRU counter for players
    qint64               m_lastMoveMs = 0; // last mouse activity over a preview (controls auto-hide)
    bool                 m_muted      = false; // session mute state shared by inline players
    bool                 m_startMuted = false; // Settings::videosStartMuted that m_muted was last reset to
    int                  m_ticks      = 0;
};
