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

    // 2.2 audio: Audio = the audio player card (audiocard.h), for every Audio-kind entry while Windows
    // can play media (mf::available()); otherwise audio files stay plain file cards (Still).
    enum class Mode { Still, Animated, Video, Audio };

    // What ChatIntegration should draw for key right now.
    Mode            mode(const QString& key) const;  // Video for every Video-kind entry, Animated while a movie exists
    QImage          frame(const QString& key) const; // current animation/video frame (device pixels), may be null
    PlaybackOverlay overlay(const QString& key) const;

    // Keys whose previews are currently on screen in a visible chat tab. Animations run only while
    // visible (and, when autoplayGifs or Windows animations are off, only while hovered). Playing
    // videos keep playing while scrolled away or in a hidden tab.
    void setVisibleKeys(const QSet<QString>& keys);
    // Keys that still have a preview in some chat document (visible or not). A video whose preview
    // is gone (chat tab closed, chat cleared) for about a second is stopped and its player closed:
    // nothing would be left to pause it with.
    void setPresentKeys(const QSet<QString>& keys);
    // Device-pixel size the preview of key is drawn at (frames are produced at this size).
    void setFrameSize(const QString& key, const QSize& devicePixels);

    // Input on a preview. For videos: Body/PlayPause toggle playback (downloading the file first if
    // needed), Seek jumps to seekFraction, Mute toggles mute, Expand emits openRequested.
    // For audio cards (Mode::Audio): Body/PlayPause toggle playback, Seek jumps there (before the file
    // is open: downloads / opens it and starts playing from there).
    // The zone is applied as given; pointer input on hidden controls is mapped to Body by the caller.
    // For other kinds only hover matters (GIF hover-to-play); clicks are handled by ChatIntegration.
    // Only one video or audio file plays at a time: starting one pauses the others.
    void click(const QString& key, VideoZone zone, double seekFraction);
    void hover(const QString& key, VideoZone zone); // empty key = mouse left all previews

    void pauseAll();
    void pauseVideos(); // 2.2 audio: videos only (opening the viewer on a picture lets audio play on)
    void stopAll();
    void settingsChanged(); // after the settings dialog: picks up a changed "start videos muted"

    // Session mute shared by all inline video players (the mute button, "start videos muted", the
    // viewer). Audio cards never follow it: they have no mute button and always play out loud.
    bool isMuted() const { return m_muted; }
    void setMuted(bool muted);

  signals:
    void frameChanged(const QString& key);
    void openRequested(const QString& key);   // expand button: open in MediaViewer
    void playbackStarted(const QString& key); // an inline video or audio file starts playing (other players should pause)

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
    double controlsOpacity(const Video* video) const; // < 1 while the bar fades out
    bool   isVideo(const QString& key) const;
    bool   isInlineAudio(const QString& key) const; // 2.2 audio: drawn as the audio card
    bool   isPlayable(const QString& key) const;    // isVideo || isInlineAudio
    void   repaintAudioProgress(Video* video);      // 2.2 audio: redraw when the played pixel or second moves
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
