#include "inlinemedia.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QMovie>
#include <QTimer>

#include "core.h"
#include "settings.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"

namespace {

constexpr int    kMaxPlayers         = 3;
constexpr qint64 kControlsHideMs     = 2500;
constexpr qint64 kControlsFadeMs     = 200; // the bar fades out over the last part of kControlsHideMs
constexpr qint64 kMaxAnimationPixels = 80LL * 1000 * 1000; // all frames together
constexpr int    kTickMs             = 50;
constexpr qint64 kOrphanGraceMs      = 1000; // a preview missing this long is gone (not a rescan)
constexpr int    kMaxLateFailures    = 3;    // failures while playing before a file is given up on

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// GIFs, and WebPs (which may or may not be animated; checked once per file).
bool mayBeAnimated(const MediaEntry& e)
{
    if (e.kind == MediaKind::AnimatedImage)
        return true;
    return e.kind == MediaKind::Image && QFileInfo(e.link.fileName).suffix().compare(QLatin1String("webp"), Qt::CaseInsensitive) == 0;
}

// Pointer on one of the player's controls (not just on the picture).
bool isControlZone(VideoZone zone)
{
    return zone == VideoZone::PlayPause || zone == VideoZone::Seek || zone == VideoZone::Mute || zone == VideoZone::Expand;
}

// GIFs play by themselves only when the setting allows it and Windows animations are on;
// otherwise they play while hovered (the user starts them).
bool gifsAutoplay()
{
    return Settings::instance().autoplayGifs && ui::animationsEnabled();
}

// Aspect-preserving fit of natural into box; never more than maxUpscale times the natural size.
QSize fitWithin(const QSize& natural, const QSize& box, qreal maxUpscale)
{
    if (natural.isEmpty())
        return box;
    if (box.isEmpty())
        return natural;
    QSize       size = natural.scaled(box, Qt::KeepAspectRatio);
    const QSize cap(qRound(natural.width() * maxUpscale), qRound(natural.height() * maxUpscale));
    if (size.width() > cap.width() || size.height() > cap.height())
        size = natural.scaled(cap, Qt::KeepAspectRatio);
    return size.expandedTo(QSize(1, 1));
}

// Video frames: even dimensions keep the video scaler happy (4:2:0 formats).
QSize evenVideoSize(const QSize& size)
{
    return QSize(qMax(2, size.width() & ~1), qMax(2, size.height() & ~1));
}

#ifdef TSMEDIA_TESTHOOKS
QElapsedTimer& testClock()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return clock;
}
void testLog(const QString& text)
{
    ts3::log(QStringLiteral("[test] t=%1ms ").arg(testClock().elapsed()) + text);
}
#else
void testLog(const QString&) {}
#endif

} // namespace

// A player record: an inline video, or (2.2) an audio card's audio-only player.
struct InlineMediaController::Video {
    QString          key;
    bool             audio            = false;   // 2.2 audio: audio-only engine, no mute / loop / frames
    double           pendingSeek      = -1.0;    // 2.2 audio: 0..1 to seek to once the file is open
    int              drawnStep        = -1;      // 2.2 audio: played pixels last drawn
    qint64           drawnSecond      = -1;      // 2.2 audio: time label last drawn (s)
    mf::VideoPlayer* player           = nullptr; // owned; deleted synchronously
    quint64          playerGeneration = 0;       // identifies player across deferred callbacks
    bool             loaded           = false;
    bool             wantPlay         = false; // start as soon as the file / player is ready
    bool             failed           = false; // this file cannot be played inline (open externally)
    int              lateFailures     = 0;     // failures after the file had opened (device reset, ...)
    bool             controlsShown    = false; // last drawn state, to notice the auto-hide
    quint64          lastUsed         = 0;
    int              appliedLoop      = -1;
    qint64           absentSinceMs    = 0; // its preview is in no chat document since then (0 = present)
};

struct InlineMediaController::Animation {
    QMovie* movie = nullptr; // owned
    QSize   natural;
    QImage  frame;
};

InlineMediaController::InlineMediaController(Core* core, QObject* parent)
    : QObject(parent)
    , m_core(core)
{
    m_startMuted = Settings::instance().videosStartMuted;
    m_muted      = m_startMuted;

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &InlineMediaController::tick);
    connect(m_core, &Core::entryChanged, this, &InlineMediaController::onEntryChanged, Qt::QueuedConnection);

#ifdef TSMEDIA_TESTHOOKS
    // Test builds: report GUI-thread stalls (anything that blocks TeamSpeak's UI for > 400 ms).
    auto* watchdog = new QTimer(this);
    watchdog->setInterval(100);
    auto* last = new QElapsedTimer;
    last->start();
    connect(watchdog, &QTimer::timeout, this, [last] {
        const qint64 gap = last->restart();
        if (gap > 400)
            testLog(QStringLiteral("GUI stall %1 ms").arg(gap));
    });
    connect(this, &QObject::destroyed, [last] { delete last; });
    watchdog->start();
#endif
}

InlineMediaController::~InlineMediaController()
{
    // Runs at plugin shutdown: no signals and no Core calls, just stop every decoder/engine now.
    m_timer->stop();
    for (Video* v : qAsConst(m_videos)) {
        if (v->player) {
            disconnect(v->player, nullptr, this, nullptr);
            delete v->player;
        }
        delete v;
    }
    m_videos.clear();
    for (Animation* a : qAsConst(m_animations)) {
        disconnect(a->movie, nullptr, this, nullptr);
        delete a->movie;
        delete a;
    }
    m_animations.clear();
}

// ============================================================================================
// What to draw
// ============================================================================================

InlineMediaController::Mode InlineMediaController::mode(const QString& key) const
{
    if (isVideo(key))
        return Mode::Video;
    if (isInlineAudio(key)) // 2.2 audio
        return Mode::Audio;
    if (m_animations.contains(key))
        return Mode::Animated;
    return Mode::Still;
}

QImage InlineMediaController::frame(const QString& key) const
{
    if (const Video* v = m_videos.value(key))
        return v->player && v->loaded ? v->player->currentFrame() : QImage();
    if (const Animation* a = m_animations.value(key))
        return a->frame;
    return {};
}

PlaybackOverlay InlineMediaController::overlay(const QString& key) const
{
    PlaybackOverlay   o;
    const MediaEntry* e = m_core->entry(key);
    if (e)
        o.durationMs = e->link.durationMs;
    o.muted = m_muted;
    o.hover = key == m_hoverKey ? m_hoverZone : VideoZone::None;
    // 2.2 audio: the card's controls are always shown, and it never plays muted.
    const bool audio = e && e->kind == MediaKind::Audio;
    if (audio) {
        o.controlsVisible = true;
        o.muted           = false;
    }

    const Video* v = m_videos.value(key);
    if (!v)
        return o;
    o.externalOnly = v->failed; // a click opens it in the default app
    if (audio && v->pendingSeek >= 0.0 && o.durationMs > 0 && !v->loaded) // 2.2 audio: where it will start
        o.positionMs = qRound64(v->pendingSeek * static_cast<double>(o.durationMs));
    if (!v->player) {
        if (v->wantPlay && e && (e->state == MediaState::Idle || e->state == MediaState::Queued || e->state == MediaState::Downloading)) {
            o.busy         = true;
            o.busyProgress = e->state == MediaState::Downloading ? shownDownloadProgress(*e) : -1.0; // 2.2 sha: the check's, when shown
        }
        return o;
    }
    if (!v->loaded) {
        o.busy         = true; // opening the file
        o.busyProgress = -1.0;
        return o;
    }
    o.playing    = v->player->isPlaying();
    o.ended      = v->player->isEnded();
    o.muted      = v->player->isMuted();
    o.positionMs = v->player->position();
    if (v->player->duration() > 0)
        o.durationMs = v->player->duration();
    if (audio)
        return o;
    o.controlsVisible = !o.playing || controlsShown(v);
    if (o.playing && o.controlsVisible)
        o.controlsOpacity = controlsOpacity(v);
    return o;
}

bool InlineMediaController::controlsShown(const Video* video) const
{
    if (video->key != m_hoverKey)
        return false;
    // A pointer resting on a control keeps the bar: it must not vanish under the button about to be
    // clicked (the click would then toggle playback instead). Only the picture itself times out.
    if (isControlZone(m_hoverZone))
        return true;
    return nowMs() - m_lastMoveMs < kControlsHideMs;
}

double InlineMediaController::controlsOpacity(const Video* video) const
{
    // Exit is quicker than entry: the bar appears at once and fades out over the last 200 ms.
    if (video->key != m_hoverKey || isControlZone(m_hoverZone) || !ui::animationsEnabled())
        return 1.0;
    const qint64 left = kControlsHideMs - (nowMs() - m_lastMoveMs);
    return left >= kControlsFadeMs ? 1.0 : qBound(0.0, static_cast<double>(left) / kControlsFadeMs, 1.0);
}

bool InlineMediaController::isVideo(const QString& key) const
{
    const MediaEntry* e = m_core->entry(key);
    return e && e->kind == MediaKind::Video;
}

// 2.2 audio: without Media Foundation (Windows N without the Media Feature Pack) audio files stay
// plain file cards that open in the default app.
bool InlineMediaController::isInlineAudio(const QString& key) const
{
    const MediaEntry* e = m_core->entry(key);
    return e && e->kind == MediaKind::Audio && mf::available();
}

bool InlineMediaController::isPlayable(const QString& key) const
{
    return isVideo(key) || isInlineAudio(key);
}

// ============================================================================================
// Layout / visibility input from ChatIntegration
// ============================================================================================

void InlineMediaController::setVisibleKeys(const QSet<QString>& keys)
{
    m_visible = keys;
    updateAnimations();
}

void InlineMediaController::setPresentKeys(const QSet<QString>& keys)
{
    const qint64 now = nowMs();
    QStringList  gone;
    for (Video* v : qAsConst(m_videos)) {
        if (keys.contains(v->key) || (!v->player && !v->wantPlay)) {
            v->absentSinceMs = 0;
            continue;
        }
        if (v->absentSinceMs == 0)
            v->absentSinceMs = now;
        else if (now - v->absentSinceMs >= kOrphanGraceMs)
            gone.append(v->key);
    }
    for (const QString& key : qAsConst(gone)) {
        Video* v = m_videos.value(key);
        testLog(QStringLiteral("preview of %1 is gone: closing its player").arg(key));
        v->wantPlay      = false;
        v->absentSinceMs = 0;
        destroyPlayer(v);
        emit frameChanged(key);
    }
    if (!gone.isEmpty())
        updateTimer();
}

void InlineMediaController::setFrameSize(const QString& key, const QSize& devicePixels)
{
    if (devicePixels.isEmpty() || m_frameSizes.value(key) == devicePixels)
        return;
    m_frameSizes.insert(key, devicePixels);

    if (Video* v = m_videos.value(key); v && v->player && v->loaded && !v->audio) // 2.2 audio: no frames
        v->player->setFrameSize(videoFrameSize(v));
    if (Animation* a = m_animations.value(key); a && !a->natural.isEmpty())
        a->movie->setScaledSize(fitWithin(a->natural, devicePixels, 4.0));
}

QSize InlineMediaController::videoFrameSize(const Video* video) const
{
    QSize natural = video->player ? video->player->videoSize() : QSize();
    if (natural.isEmpty()) {
        if (const MediaEntry* e = m_core->entry(video->key))
            natural = QSize(e->link.width, e->link.height);
    }
    const QSize box = m_frameSizes.value(video->key);
    if (box.isEmpty())
        return natural.isEmpty() ? QSize(640, 360) : evenVideoSize(fitWithin(natural, QSize(1280, 720), 1.0));
    return evenVideoSize(fitWithin(natural.isEmpty() ? box : natural, box, 4.0));
}

// ============================================================================================
// Input
// ============================================================================================

void InlineMediaController::click(const QString& key, VideoZone zone, double seekFraction)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e || !isPlayable(key) || m_core->isSpoilerHidden(key)) // 2.2 audio: audio cards too; 2.2 spoiler: revealed first (ChatIntegration)
        return;
    const bool audio = e->kind == MediaKind::Audio;
    if (audio && (zone == VideoZone::Mute || zone == VideoZone::Expand))
        zone = VideoZone::Body; // the card has neither

    Video*     v      = m_videos.value(key);
    const bool active = v && v->player && v->loaded;
    m_lastMoveMs      = nowMs();

    if (zone == VideoZone::Expand && active) {
        if (v->player->isPlaying())
            v->player->pause();
        emit frameChanged(key);
        emit openRequested(key);
        return;
    }

    if (!active) {
        v           = videoRecord(key);
        v->lastUsed = ++m_useClock;
        if (v->failed) {
            m_core->openExternally(key); // e.g. a codec Media Foundation does not have
            return;
        }
        // 2.2 audio: a seek on a card that isn't open yet starts it from there (never cancels).
        const bool seekStart = audio && zone == VideoZone::Seek;
        if (seekStart)
            v->pendingSeek = qBound(0.0, seekFraction, 1.0);
        if (v->player) {
            v->wantPlay = seekStart || !v->wantPlay; // still opening: a second click cancels the autoplay
        } else if (v->wantPlay) {
            v->wantPlay = seekStart; // pressed again while downloading
        } else if (e->state == MediaState::Ready) {
            v->wantPlay = true;
            startVideo(key);
        } else if (e->state == MediaState::Failed) {
            if (isRetryableDownload(*e)) { // not for deleted files or password-protected channels
                v->wantPlay = true;
                m_core->retry(key);
            }
        } else {
            v->wantPlay = true;
            m_core->download(key, false);
        }
        if (!v->wantPlay)
            v->pendingSeek = -1.0; // canceled: the next play starts from the beginning
        emit frameChanged(key);
        updateTimer();
        return;
    }

    v->lastUsed = ++m_useClock;
    switch (zone) {
    case VideoZone::Seek: {
        const qint64 duration = v->player->duration();
        if (duration > 0)
            v->player->seek(qRound64(qBound(0.0, seekFraction, 1.0) * static_cast<double>(duration)));
        break;
    }
    case VideoZone::Mute:
        m_muted = !v->player->isMuted();
        for (Video* other : qAsConst(m_videos)) {
            if (other->player && !other->audio) // 2.2 audio: cards are never muted
                other->player->setMuted(m_muted);
        }
        break;
    case VideoZone::None:
    case VideoZone::Body:
    case VideoZone::PlayPause:
    case VideoZone::Expand:
        if (v->player->isPlaying())
            v->player->pause();
        else
            play(v);
        break;
    }
    emit frameChanged(key);
    updateTimer();
}

void InlineMediaController::hover(const QString& key, VideoZone zone)
{
    const QString   oldKey  = m_hoverKey;
    const VideoZone oldZone = m_hoverZone;
    m_hoverKey              = key;
    m_hoverZone             = key.isEmpty() ? VideoZone::None : zone;
    m_lastMoveMs            = nowMs();

    if (oldKey != key) {
        if (!oldKey.isEmpty() && isPlayable(oldKey)) { // 2.2 audio: cards show hover too
            if (Video* old = m_videos.value(oldKey))
                old->controlsShown = false;
            emit frameChanged(oldKey);
        }
        if (!gifsAutoplay())
            updateAnimations();
    }

    if (!key.isEmpty() && isPlayable(key)) {
        bool changed = oldKey != key || oldZone != m_hoverZone;
        if (Video* v = m_videos.value(key); v && v->player && v->loaded && !v->audio) {
            const bool shown = controlsShown(v);
            if (shown != v->controlsShown) {
                v->controlsShown = shown;
                changed          = true;
            }
        }
        if (changed)
            emit frameChanged(key);
    }
    updateTimer();
}

void InlineMediaController::pauseAll()
{
    for (Video* v : qAsConst(m_videos)) {
        v->wantPlay = false;
        if (v->player && v->player->isPlaying()) {
            v->player->pause();
            emit frameChanged(v->key);
        }
    }
    updateTimer();
}

void InlineMediaController::pauseVideos()
{
    for (Video* v : qAsConst(m_videos)) {
        if (v->audio)
            continue;
        v->wantPlay = false;
        if (v->player && v->player->isPlaying()) {
            v->player->pause();
            emit frameChanged(v->key);
        }
    }
    updateTimer();
}

void InlineMediaController::settingsChanged()
{
    // Only a change of the setting resets the session mute state (the mute button sets it too).
    const bool startMuted = Settings::instance().videosStartMuted;
    if (startMuted == m_startMuted)
        return;
    m_startMuted = startMuted;
    setMuted(startMuted);
}

void InlineMediaController::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    for (Video* v : qAsConst(m_videos)) {
        if (v->audio) // 2.2 audio: cards don't follow the session mute
            continue;
        if (v->player && v->player->isMuted() != m_muted)
            v->player->setMuted(m_muted);
        emit frameChanged(v->key); // the mute icon
    }
}

void InlineMediaController::stopAll()
{
    QStringList keys = m_videos.keys();
    for (Video* v : qAsConst(m_videos)) {
        destroyPlayer(v);
        delete v;
    }
    m_videos.clear();
    const QStringList animated = m_animations.keys();
    for (const QString& key : animated)
        destroyAnimation(key);
    keys += animated;
    m_hoverKey.clear();
    m_hoverZone = VideoZone::None;
    updateTimer();
    for (const QString& key : qAsConst(keys))
        emit frameChanged(key);
}

// ============================================================================================
// Videos
// ============================================================================================

InlineMediaController::Video* InlineMediaController::videoRecord(const QString& key)
{
    Video* v = m_videos.value(key);
    if (!v) {
        v      = new Video;
        v->key = key;
        if (const MediaEntry* e = m_core->entry(key))
            v->audio = e->kind == MediaKind::Audio; // 2.2 audio
        m_videos.insert(key, v);
    }
    return v;
}

void InlineMediaController::startVideo(const QString& key)
{
    const MediaEntry* e = m_core->entry(key);
    if (!e || !isPlayable(key) || e->state != MediaState::Ready || e->localPath.isEmpty()) // 2.2 audio: audio too
        return;

    Video* v = videoRecord(key);
    if (v->player) {
        if (v->loaded && v->wantPlay) {
            v->wantPlay = false;
            play(v);
            emit frameChanged(key);
        }
        return;
    }

    makeRoomForPlayer(v);
    testLog(QStringLiteral("creating player for ") + key);
    auto* player   = new mf::VideoPlayer;
    testLog(QStringLiteral("player created"));
    v->player      = player;
    v->loaded      = false;
    v->appliedLoop = -1;
    const quint64 generation = ++v->playerGeneration;
    m_core->setInUse(key, true);

    connect(player, &mf::VideoPlayer::loaded, this, [this, key] {
        Video* video = m_videos.value(key);
        if (!video || !video->player)
            return;
        video->loaded = true;
        testLog(QStringLiteral("player loaded ") + key);
        if (!video->audio)
            video->player->setFrameSize(videoFrameSize(video));
        applySettings(video);
        // 2.2 audio: a click on the seek bar before the file was open starts it there.
        if (video->pendingSeek >= 0.0 && video->player->duration() > 0)
            video->player->seek(qRound64(video->pendingSeek * static_cast<double>(video->player->duration())));
        video->pendingSeek = -1.0;
        if (video->wantPlay) {
            video->wantPlay = false;
            play(video);
        }
        emit frameChanged(key);
        updateTimer();
    });
    connect(player, &mf::VideoPlayer::failed, this, [this, key, generation](const QString& error) {
        Video* video = m_videos.value(key);
        if (!video || !video->player || video->playerGeneration != generation)
            return;
        ts3::log((video->audio ? QStringLiteral("Inline audio %1 cannot be played: %2") : QStringLiteral("Inline video %1 cannot be played: %2")).arg(key, error),
                 LogLevel_WARNING);
        const bool beforeStart = !video->loaded;
        video->wantPlay        = false;
        // A file that does not even open (missing decoder, unsupported format) is played externally
        // from now on. A failure while playing (graphics device reset, the file became unreadable)
        // is not final: the next click opens a fresh player. Only repeated failures make it final.
        if (beforeStart || ++video->lateFailures >= kMaxLateFailures)
            video->failed = true;
        // We are inside the player's own signal: tear it down once it has returned.
        QTimer::singleShot(0, this, [this, key, generation] {
            Video* failedVideo = m_videos.value(key);
            if (!failedVideo || !failedVideo->player || failedVideo->playerGeneration != generation)
                return;
            destroyPlayer(failedVideo);
            emit frameChanged(key);
            updateTimer();
        });
        if (beforeStart)
            m_core->openExternally(key); // the default player may have the codec
    });
    connect(player, &mf::VideoPlayer::frameReady, this, [this, key] {
#ifdef TSMEDIA_TESTHOOKS
        static int frames = 0;
        if (++frames % 30 == 1)
            testLog(QStringLiteral("frame %1 of %2").arg(frames).arg(key));
#endif
        emit frameChanged(key);
    });
    connect(player, &mf::VideoPlayer::stateChanged, this, [this, key] {
        emit frameChanged(key);
        updateTimer();
    });
    if (v->audio) // 2.2 audio: no frames; a finished seek (also while paused) moves the bar and the time
        connect(player, &mf::VideoPlayer::positionChanged, this, [this, key](qint64) { emit frameChanged(key); });
    connect(player, &mf::VideoPlayer::videoSizeChanged, this, [this, key] {
        // The stream switched resolution / aspect: refit the frames to the new picture. The preview
        // box itself stays as the link's metadata says (the chat never relayouts for this).
        Video* video = m_videos.value(key);
        if (!video || !video->player || !video->loaded)
            return;
        video->player->setFrameSize(videoFrameSize(video));
        emit frameChanged(key);
    });

    // 2.2 audio: audio files use the audio-only engine (no graphics device), never muted. The volume
    // is set before opening, so nothing can ever start at the engine's default full volume.
    player->setVolume(qBound(0, Settings::instance().videoVolume, 100) / 100.0);
    player->setMuted(v->audio ? false : m_muted);
    player->open(e->localPath, v->audio ? mf::OpenMode::AudioOnly : mf::OpenMode::Auto);
    testLog(QStringLiteral("open() returned"));
    emit frameChanged(key);
    updateTimer();
}

void InlineMediaController::play(Video* video)
{
    // Only one inline video plays at a time.
    for (Video* other : qAsConst(m_videos)) {
        if (other != video && other->player && other->player->isPlaying()) {
            other->player->pause();
            emit frameChanged(other->key);
        }
    }
    applySettings(video);
    if (video->player->isEnded())
        video->player->seek(0);
    video->player->play();
    video->lastUsed = ++m_useClock;
    emit playbackStarted(video->key);
}

void InlineMediaController::destroyPlayer(Video* video)
{
    if (!video->player)
        return;
    mf::VideoPlayer* player = video->player;
    video->player           = nullptr;
    video->loaded           = false;
    video->controlsShown    = false;
    disconnect(player, nullptr, this, nullptr);
    delete player; // shuts the engine down synchronously
    m_core->setInUse(video->key, false);
}

void InlineMediaController::makeRoomForPlayer(const Video* keep)
{
    for (;;) {
        int    players = 0;
        Video* victim  = nullptr;
        for (Video* v : qAsConst(m_videos)) {
            if (!v->player)
                continue;
            ++players;
            if (v == keep || v->player->isPlaying())
                continue;
            if (!victim || v->lastUsed < victim->lastUsed)
                victim = v;
        }
        if (players < kMaxPlayers || !victim)
            return;
        // A victim may still be opening with autoplay pending; that request is superseded by the
        // newer one (only one video plays anyway). Left set, it would wait for a player forever.
        victim->wantPlay = false;
        destroyPlayer(victim);
        emit frameChanged(victim->key); // back to the poster
    }
}

void InlineMediaController::applySettings(Video* video)
{
    if (!video->player)
        return;
    const Settings& s      = Settings::instance();
    const double    volume = qBound(0, s.videoVolume, 100) / 100.0;
    if (qAbs(video->player->volume() - volume) > 0.001)
        video->player->setVolume(volume);
    // 2.2 audio: audio cards never loop and never follow the video session mute.
    const int loop = s.loopVideos && !video->audio ? 1 : 0;
    if (video->appliedLoop != loop) {
        video->player->setLoop(loop == 1);
        video->appliedLoop = loop;
    }
    const bool muted = video->audio ? false : m_muted;
    if (video->player->isMuted() != muted)
        video->player->setMuted(muted);
}

void InlineMediaController::onEntryChanged(const QString& key)
{
    const MediaEntry* e = m_core->entry(key);

    // 2.2 spoiler: covered again (Hide spoiler, or the "without blurring" setting turned off): a video
    // stops where it is and doesn't start by itself once its download is done.
    if (Video* v = m_videos.value(key); v && m_core->isSpoilerHidden(key)) {
        v->wantPlay = false;
        if (v->player && v->player->isPlaying()) {
            v->player->pause();
            emit frameChanged(key);
        }
    }

    if (Video* v = m_videos.value(key)) {
        if (!e || e->state != MediaState::Ready) {
            // The file went away (cache cleared, entry reset) or the download failed. A fresh copy
            // gets a fresh inline attempt.
            v->failed       = false;
            v->lateFailures = 0;
            if (v->player) {
                destroyPlayer(v);
                emit frameChanged(key);
            }
            if (!e || e->state == MediaState::Failed) {
                v->wantPlay    = false;
                v->pendingSeek = -1.0;
            }
            if (!e) {
                m_videos.remove(key);
                delete v;
            }
        } else if (v->wantPlay && !v->player) {
            startVideo(key);
        }
        updateTimer();
    }

    if (m_animations.contains(key) && (!e || e->state != MediaState::Ready)) {
        destroyAnimation(key);
        emit frameChanged(key);
    }
    if (!e || e->state != MediaState::Ready)
        m_animatable.remove(key);
    else if (m_visible.contains(key) && mayBeAnimated(*e))
        updateAnimations();
}

void InlineMediaController::tick()
{
    ++m_ticks;
    const bool syncSettings = m_ticks % 20 == 0; // about once a second
    const bool animate      = ui::animationsEnabled(); // off: spinners are static, nothing to redraw
    for (Video* v : qAsConst(m_videos)) {
        if (!v->player) {
            if (v->wantPlay && animate) {
                // Indeterminate spinner while waiting for the download to start (overlay() draws
                // the download's own progress, and nothing in the other states).
                const MediaEntry* e = m_core->entry(v->key);
                if (e && (e->state == MediaState::Idle || e->state == MediaState::Queued))
                    emit frameChanged(v->key);
            }
            continue;
        }
        if (!v->loaded) {
            if (animate)
                emit frameChanged(v->key); // spinner while opening
            continue;
        }
        if (v->audio) { // 2.2 audio: no frames; the card follows the position
            repaintAudioProgress(v);
            if (syncSettings)
                applySettings(v);
            continue;
        }
        const bool shown = controlsShown(v);
        if (shown != v->controlsShown) {
            v->controlsShown = shown;
            emit frameChanged(v->key);
        } else if (shown && v->player->isPlaying() && controlsOpacity(v) < 1.0) {
            emit frameChanged(v->key); // fading out: smooth even when the video has few frames
        }
        if (syncSettings)
            applySettings(v);
    }
    updateTimer();
}

// 2.2 audio: while an audio card plays, redraw it only when something visible moves: the played
// part of the seek bar by a device pixel (the card's width bounds the bar's), or the time by a second.
void InlineMediaController::repaintAudioProgress(Video* video)
{
    if (!video->player || !video->player->isPlaying())
        return;
    const qint64 position = video->player->position();
    const qint64 duration = video->player->duration();
    const int    pixels   = qMax(1, m_frameSizes.value(video->key).width());
    const int    step     = duration > 0 ? static_cast<int>(static_cast<double>(position) / static_cast<double>(duration) * pixels) : 0;
    const qint64 second   = position / 1000;
    if (step == video->drawnStep && second == video->drawnSecond)
        return;
    video->drawnStep   = step;
    video->drawnSecond = second;
    emit frameChanged(video->key);
}

void InlineMediaController::updateTimer()
{
    // Waiting for a download only needs the timer for the spinner.
    const bool animate = ui::animationsEnabled();
    bool       needed  = false;
    for (const Video* v : qAsConst(m_videos)) {
        if (v->player || (v->wantPlay && animate)) {
            needed = true;
            break;
        }
    }
    if (needed && !m_timer->isActive())
        m_timer->start();
    else if (!needed && m_timer->isActive())
        m_timer->stop();
}

// ============================================================================================
// GIF / animated WebP
// ============================================================================================

bool InlineMediaController::isAnimatable(const QString& key)
{
    const auto cached = m_animatable.constFind(key);
    if (cached != m_animatable.constEnd())
        return cached.value();

    const MediaEntry* e = m_core->entry(key);
    if (!e || e->state != MediaState::Ready || !mayBeAnimated(*e))
        return false;

    bool         ok = false;
    QImageReader reader(e->localPath);
    reader.setDecideFormatFromContent(true);
    if (reader.supportsAnimation()) {
        const QSize  size   = reader.size();
        const int    count  = reader.imageCount(); // 0 = unknown
        const qint64 pixels = size.isValid() ? static_cast<qint64>(size.width()) * size.height() : 0;
        ok                  = pixels > 0 && count != 1 && pixels * qMax(1, count) <= kMaxAnimationPixels;
        if (!ok && pixels > 0 && count != 1)
            ts3::log(QStringLiteral("Not animating %1: %2x%3, %4 frames is too large").arg(e->link.fileName).arg(size.width()).arg(size.height()).arg(count));
    }
    m_animatable.insert(key, ok);
    return ok;
}

void InlineMediaController::ensureAnimation(const QString& key)
{
    if (m_animations.contains(key))
        return;
    const MediaEntry* e = m_core->entry(key);
    if (!e || e->state != MediaState::Ready)
        return;

    auto* movie = new QMovie(e->localPath);
    if (!movie->isValid()) {
        delete movie;
        m_animatable.insert(key, false);
        return;
    }
    movie->setCacheMode(QMovie::CacheNone);

    auto* a    = new Animation;
    a->movie   = movie;
    a->natural = QImageReader(e->localPath).size();
    const QSize box = m_frameSizes.value(key);
    if (!a->natural.isEmpty() && !box.isEmpty())
        movie->setScaledSize(fitWithin(a->natural, box, 4.0));

    connect(movie, &QMovie::frameChanged, this, [this, key](int) {
        Animation* an = m_animations.value(key);
        if (!an)
            return;
        an->frame = an->movie->currentImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        emit frameChanged(key);
    });
    connect(movie, &QMovie::error, this, [this, key](QImageReader::ImageReaderError) {
        m_animatable.insert(key, false);
        QTimer::singleShot(0, this, [this, key] {
            if (!m_animations.contains(key))
                return;
            destroyAnimation(key);
            emit frameChanged(key);
        });
    });

    m_animations.insert(key, a);
    m_core->setInUse(key, true); // counted by Core, so the viewer's own mark is unaffected
}

void InlineMediaController::destroyAnimation(const QString& key)
{
    Animation* a = m_animations.take(key);
    if (!a)
        return;
    disconnect(a->movie, nullptr, this, nullptr);
    delete a->movie;
    delete a;
    m_core->setInUse(key, false);
}

void InlineMediaController::updateAnimations()
{
    const bool autoplay = gifsAutoplay();

    // Animations that scrolled out of view (or whose chat was hidden) are dropped. 2.2 spoiler: so are
    // those covered again; a hidden spoiler never animates (not even on hover).
    const QStringList running = m_animations.keys();
    for (const QString& key : running) {
        if (!m_visible.contains(key) || m_core->isSpoilerHidden(key)) {
            destroyAnimation(key);
            emit frameChanged(key);
        }
    }

    for (const QString& key : qAsConst(m_visible)) {
        const MediaEntry* e = m_core->entry(key);
        if (!e || e->state != MediaState::Ready || !mayBeAnimated(*e) || m_core->isSpoilerHidden(key))
            continue;
        const bool run = autoplay || key == m_hoverKey;
        Animation* a   = m_animations.value(key);
        if (!a) {
            if (!run || !isAnimatable(key))
                continue;
            ensureAnimation(key);
            a = m_animations.value(key);
            if (!a)
                continue;
        }
        if (run) {
            if (a->movie->state() == QMovie::NotRunning)
                a->movie->start();
            else if (a->movie->state() == QMovie::Paused)
                a->movie->setPaused(false);
        } else if (a->movie->state() == QMovie::Running) {
            a->movie->setPaused(true);
        }
    }
}
