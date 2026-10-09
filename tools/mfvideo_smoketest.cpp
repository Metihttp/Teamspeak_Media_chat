// mfvideo_smoketest <video> <outdir> [<format-change clip>]
// mfvideo_smoketest --audio <audio file> <outdir>
// mfvideo_smoketest --writer <outdir>
//
// Exercises src/video/mfvideo.* outside TeamSpeak: probes the file on a worker thread (as the plugin
// does) and saves the poster, then plays it headless through mf::VideoPlayer, saving frames while it
// checks pause, seek, resize, resume, end of stream, loop, close and teardown.
// --audio plays an audio file through the audio-only engine (OpenMode::AudioOnly, the inline audio
// card's player): no graphics device, no frames, pause / seek / end / replay, and it measures which
// playback rates the engine accepts (the pitch at other rates can only be judged by listening, which
// this tool never does). --writer encodes 2 s of a tone to .m4a through mf::detail's MPEG-4 sink
// writer on a worker thread and probes and plays the result.
// The optional clip changes resolution mid-stream: 640x360, then 480x360, then 640x360 again, each part
// a blue fill inside a 24 px red border, for example (ffmpeg, then concatenate the parts with -c copy):
//   ffmpeg -f lavfi -i color=c=blue:s=640x360:r=30:d=2 -vf drawbox=x=0:y=0:w=iw:h=ih:color=red:t=24
//          -c:v libx264 -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f mpegts a.ts   (b.ts: s=480x360)
//   ffmpeg -i "concat:a.ts|b.ts|a.ts" -c copy fmtchange.mp4
// It checks that videoSizeChanged() reports each change and that no frame crops the picture.
// Completely silent: every player is set to volume 0 and muted before it opens a file, and nothing
// here ever unmutes one or raises its volume (the volume/mute API is checked on a player without a file).
// Exit code: 0 all checks passed, 1 usage, 2 Media Foundation unavailable, 3 probe failed,
// 4 playback failed, 5 some checks failed.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <vector>

#include "video/mfcommon.h"
#include "video/mfvideo.h"

namespace {

int g_failures = 0;

void print(const QString& text)
{
    std::fputs(text.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void check(bool ok, const QString& what)
{
    print(QStringLiteral("  [%1] %2").arg(ok ? QStringLiteral("ok") : QStringLiteral("FAIL"), what));
    if (!ok)
        ++g_failures;
}

QString seconds(qint64 ms)
{
    return QString::number(ms / 1000.0, 'f', 3) + QStringLiteral(" s");
}

QString sizeText(const QSize& size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

// Runs the event loop until condition() holds or timeoutMs has passed.
bool waitUntil(const std::function<bool()>& condition, int timeoutMs)
{
    if (condition())
        return true;
    QEventLoop    loop;
    QElapsedTimer clock;
    clock.start();
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (condition() || clock.elapsed() >= timeoutMs)
            loop.quit();
    });
    poll.start();
    loop.exec();
    return condition();
}

void wait(int ms)
{
    waitUntil([] { return false; }, ms);
}

// Grey levels on a 24x24 grid.
QVector<int> samples(const QImage& image)
{
    QVector<int> grey;
    if (image.isNull())
        return grey;
    constexpr int kGrid = 24;
    grey.reserve(kGrid * kGrid);
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const int x = (2 * gx + 1) * image.width() / (2 * kGrid);
            const int y = (2 * gy + 1) * image.height() / (2 * kGrid);
            grey.append(qGray(image.pixel(x, y)));
        }
    }
    return grey;
}

// A real decoded picture, not a flat (black/green) surface.
bool hasDetail(const QImage& image)
{
    const QVector<int> grey = samples(image);
    if (grey.isEmpty())
        return false;
    const auto range = std::minmax_element(grey.cbegin(), grey.cend());
    return *range.second - *range.first > 40;
}

bool isOpaque(const QImage& image)
{
    for (int y = 0; y < image.height(); y += qMax(1, image.height() / 16)) {
        for (int x = 0; x < image.width(); x += qMax(1, image.width() / 16)) {
            if (qAlpha(image.pixel(x, y)) != 255)
                return false;
        }
    }
    return true;
}

// Fraction of grid samples whose grey level differs by more than 16.
double difference(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull())
        return 1.0;
    const QImage       bb = b.size() == a.size() ? b : b.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QVector<int> ga = samples(a);
    const QVector<int> gb = samples(bb);
    int                differing = 0;
    for (int i = 0; i < ga.size(); ++i) {
        if (qAbs(ga.at(i) - gb.at(i)) > 16)
            ++differing;
    }
    return static_cast<double>(differing) / ga.size();
}

QString pct(double fraction)
{
    return QString::number(fraction * 100.0, 'f', 1) + QLatin1Char('%');
}

bool save(const QImage& image, const QString& path)
{
    const bool ok = !image.isNull() && image.save(path);
    check(ok, QStringLiteral("saved %1 (%2)").arg(QFileInfo(path).fileName(), sizeText(image.size())));
    return ok;
}

// Called on every player before it opens a file.
void silence(mf::VideoPlayer& player)
{
    player.setVolume(0.0);
    player.setMuted(true);
}

bool isSilent(const mf::VideoPlayer& player)
{
    return player.isMuted() && player.volume() == 0.0;
}

// Waits for loaded()/failed() of a freshly opened player. Returns the error text (empty on success).
QString waitForLoad(mf::VideoPlayer& player, int timeoutMs)
{
    bool    loaded = false;
    QString error;
    const auto loadedConnection = QObject::connect(&player, &mf::VideoPlayer::loaded, [&] { loaded = true; });
    const auto failedConnection = QObject::connect(&player, &mf::VideoPlayer::failed, [&](const QString& message) { error = message; });
    if (!waitUntil([&] { return loaded || !error.isEmpty(); }, timeoutMs) && error.isEmpty())
        error = QStringLiteral("timed out waiting for loaded()/failed()");
    QObject::disconnect(loadedConnection);
    QObject::disconnect(failedConnection);
    return error;
}

int runPlayback(const QString& video, const QString& outDir, const mf::ProbeResult& info)
{
    print(QStringLiteral("playback:"));
    mf::VideoPlayer player;
    int             frames          = 0;
    int             stateChanges    = 0;
    int             positionSignals = 0;
    int             sizeSignals     = 0;
    QString         error;
    QObject::connect(&player, &mf::VideoPlayer::frameReady, [&] { ++frames; });
    QObject::connect(&player, &mf::VideoPlayer::stateChanged, [&] { ++stateChanges; });
    QObject::connect(&player, &mf::VideoPlayer::positionChanged, [&](qint64) { ++positionSignals; });
    QObject::connect(&player, &mf::VideoPlayer::videoSizeChanged, [&] { ++sizeSignals; });
    QObject::connect(&player, &mf::VideoPlayer::failed, [&](const QString& message) {
        error = message;
        print(QStringLiteral("  failed(): ") + message);
    });

    QElapsedTimer clock;
    clock.start();
    silence(player);
    player.open(video);
    const QString loadError = waitForLoad(player, 15000);
    if (!loadError.isEmpty()) {
        print(QStringLiteral("  playback failed: ") + loadError);
        return 4;
    }
    const qint64 total    = player.duration();
    const QSize  native   = player.videoSize();
    const bool   hasVideo = !native.isEmpty();
    print(QStringLiteral("  loaded in %1 ms: video %2, duration %3").arg(clock.elapsed()).arg(sizeText(native), seconds(total)));
    check(player.isLoaded(), QStringLiteral("isLoaded()"));
    check(isSilent(player), QStringLiteral("volume 0 and mute set before open() are kept"));
    check(total > 0 && qAbs(total - info.durationMs) <= 150, QStringLiteral("duration matches the probe (%1 vs %2)").arg(seconds(total), seconds(info.durationMs)));
    check(hasVideo == info.hasVideo, QStringLiteral("video stream presence matches the probe"));
    if (hasVideo) {
        const bool close = qAbs(native.width() - info.size.width()) <= 2 && qAbs(native.height() - info.size.height()) <= 2;
        check(close, QStringLiteral("display size matches the probe (%1 vs %2)").arg(sizeText(native), sizeText(info.size)));
    }

    QSize frameSize;
    if (hasVideo) {
        frameSize = native.scaled(480, 270, Qt::KeepAspectRatio);
        player.setFrameSize(frameSize);
        clock.restart();
        const bool first = waitUntil([&] { return player.currentFrame().size() == frameSize && hasDetail(player.currentFrame()); }, 3000);
        check(first, QStringLiteral("first frame appears while paused, at %1 (%2 ms)").arg(sizeText(frameSize)).arg(clock.elapsed()));
        if (first)
            save(player.currentFrame(), outDir + QStringLiteral("/frame_0_paused.png"));
    }

    // ---- play and grab frames ----
    player.play();
    check(player.isPlaying(), QStringLiteral("isPlaying() right after play()"));
    qint64 targets[3] = {500, 1500, 3000};
    if (total < 3600) {
        targets[0] = total * 15 / 100;
        targets[1] = total * 40 / 100;
        targets[2] = total * 70 / 100;
    }
    QImage shots[3];
    for (int i = 0; i < 3; ++i) {
        const bool   reached = waitUntil([&] { return player.position() >= targets[i]; }, static_cast<int>(targets[i]) + 5000);
        const int    before  = frames;
        const bool   fresh   = !hasVideo || waitUntil([&] { return frames > before; }, 1000);
        const qint64 at      = player.position();
        check(reached && fresh, QStringLiteral("playback reached %1 (position %2)").arg(seconds(targets[i]), seconds(at)));
        if (!hasVideo)
            continue;
        shots[i] = player.currentFrame();
        save(shots[i], outDir + QStringLiteral("/frame_%1.png").arg(i + 1));
        check(shots[i].size() == frameSize && shots[i].format() == QImage::Format_ARGB32_Premultiplied,
              QStringLiteral("frame is %1 ARGB32_Premultiplied").arg(sizeText(frameSize)));
        check(hasDetail(shots[i]) && isOpaque(shots[i]), QStringLiteral("frame has picture content and is opaque"));
    }
    if (hasVideo) {
        const double d12 = difference(shots[0], shots[1]);
        const double d23 = difference(shots[1], shots[2]);
        check(d12 > 0.02 && d23 > 0.02, QStringLiteral("frames differ over time (%1, %2 of samples changed)").arg(pct(d12), pct(d23)));
        print(QStringLiteral("  %1 frames in %2 ms of playback").arg(frames).arg(clock.elapsed()));
    }
    check(positionSignals >= 3 && positionSignals <= 40, QStringLiteral("positionChanged throttled (%1 signals)").arg(positionSignals));

    // ---- pause ----
    const int statesBeforePause = stateChanges;
    player.pause();
    check(!player.isPlaying() && !player.isEnded(), QStringLiteral("paused"));
    check(stateChanges > statesBeforePause, QStringLiteral("stateChanged on pause"));
    wait(200);
    const qint64 pausedAt     = player.position();
    const int    framesPaused = frames;
    wait(800);
    check(qAbs(player.position() - pausedAt) <= 20,
          QStringLiteral("position stays while paused (%1 -> %2)").arg(seconds(pausedAt), seconds(player.position())));
    check(frames - framesPaused <= 1, QStringLiteral("no frames while paused (%1)").arg(frames - framesPaused));

    // ---- seek while paused ----
    // 50%, unless playback was paused close to it (short clips): then 25%, leaving room to resume.
    const qint64 seekTarget  = qAbs(total / 2 - pausedAt) < 500 ? total / 4 : total / 2;
    const int    beforeSeek  = frames;
    player.seek(seekTarget);
    const bool seekedFrame = !hasVideo || waitUntil([&] { return frames > beforeSeek; }, 3000);
    wait(400); // let the engine settle on the target frame
    check(seekedFrame, QStringLiteral("frame delivered after seeking while paused"));
    check(qAbs(player.position() - seekTarget) <= 60, QStringLiteral("position after seek %1 (target %2)").arg(seconds(player.position()), seconds(seekTarget)));
    check(!player.isPlaying(), QStringLiteral("still paused after seek"));
    QImage seekShot;
    if (hasVideo) {
        seekShot = player.currentFrame();
        save(seekShot, outDir + QStringLiteral("/frame_seek_%1ms.png").arg(seekTarget));
        const double changed = difference(seekShot, shots[2]);
        check(changed > 0.02, QStringLiteral("seek frame differs from the last playing frame (%1)").arg(pct(changed)));

        // ---- resize while paused ----
        const QSize smaller = (frameSize / 2).expandedTo(QSize(16, 16));
        const int   before  = frames;
        player.setFrameSize(smaller);
        const bool resized = waitUntil([&] { return frames > before && player.currentFrame().size() == smaller; }, 1500);
        check(resized, QStringLiteral("resize while paused re-renders at %1").arg(sizeText(smaller)));
        if (resized) {
            const double same = difference(player.currentFrame(), seekShot);
            check(same < 0.15, QStringLiteral("resized frame shows the same picture (%1 differs)").arg(pct(same)));
            save(player.currentFrame(), outDir + QStringLiteral("/frame_seek_small.png"));
        }

        // Full resolution (the viewer's case); the burned-in timer of test clips stays readable here.
        const QSize large = native.boundedTo(QSize(1280, 1280)) == native ? native : native.scaled(1280, 1280, Qt::KeepAspectRatio);
        const int   beforeLarge = frames;
        player.setFrameSize(large);
        const bool enlarged = waitUntil([&] { return frames > beforeLarge && player.currentFrame().size() == large; }, 1500);
        check(enlarged, QStringLiteral("resize while paused re-renders at %1").arg(sizeText(large)));
        if (enlarged) {
            const double same = difference(player.currentFrame(), seekShot);
            check(same < 0.15, QStringLiteral("enlarged frame shows the same picture (%1 differs)").arg(pct(same)));
            save(player.currentFrame(), outDir + QStringLiteral("/frame_seek_full.png"));
        }
        player.setFrameSize(frameSize);
    }

    // ---- resume ----
    player.play();
    const qint64 resumeFrom = player.position();
    wait(800);
    check(player.isPlaying() && player.position() >= resumeFrom + 400,
          QStringLiteral("playback resumes (%1 -> %2)").arg(seconds(resumeFrom), seconds(player.position())));

    // ---- end of stream ----
    const int statesBeforeEnd = stateChanges;
    player.seek(qMax<qint64>(0, total - 400));
    const bool ended = waitUntil([&] { return player.isEnded(); }, 5000);
    check(ended && !player.isPlaying(), QStringLiteral("isEnded() at the end of the file"));
    check(stateChanges > statesBeforeEnd, QStringLiteral("stateChanged on end"));
    check(player.position() >= total - 150, QStringLiteral("position at the end (%1)").arg(seconds(player.position())));

    player.play();
    wait(500);
    check(player.isPlaying() && !player.isEnded() && player.position() < 1500,
          QStringLiteral("play() after the end restarts (position %1)").arg(seconds(player.position())));

    // ---- loop ----
    player.setLoop(true);
    player.seek(qMax<qint64>(0, total - 300));
    wait(1200);
    check(player.isPlaying() && !player.isEnded() && player.position() < 1500,
          QStringLiteral("loop wraps around (position %1)").arg(seconds(player.position())));
    player.setLoop(false);

    // ---- mute (never undone: see controlsTest() for the volume/mute API) ----
    const int statesBeforeMute = stateChanges;
    player.setMuted(true);
    check(stateChanges == statesBeforeMute && isSilent(player), QStringLiteral("setMuted() without a change emits nothing; still silent"));

    // ---- close ----
    // A size reported again by the engine (load events, format changes of the same size) is no change.
    check(sizeSignals == 0 && player.videoSize() == native,
          QStringLiteral("no videoSizeChanged() for a clip of one size (%1 signals)").arg(sizeSignals));

    player.close();
    check(!player.isLoaded() && !player.isPlaying() && player.currentFrame().isNull() && player.duration() == 0,
          QStringLiteral("close() resets the player"));
    wait(300);
    check(error.isEmpty(), QStringLiteral("no failed() during playback"));

    // ---- reopen the same player ----
    player.open(video);
    const QString reopenError = waitForLoad(player, 15000);
    check(reopenError.isEmpty() && player.duration() == total, QStringLiteral("the same player opens a file again"));
    check(isSilent(player), QStringLiteral("volume 0 and mute survive close() / open()"));
    if (hasVideo) {
        player.play();
        check(waitUntil([&] { return !player.currentFrame().isNull(); }, 3000), QStringLiteral("reopened player delivers frames"));
    }
    return 0;
}

// Volume clamping and the mute signal, on a player that never opens a file (no engine, no audio).
void controlsTest()
{
    print(QStringLiteral("volume / mute:"));
    mf::VideoPlayer player;
    player.setVolume(0.0);
    int states = 0;
    QObject::connect(&player, &mf::VideoPlayer::stateChanged, [&] { ++states; });
    check(player.volume() == 0.0, QStringLiteral("volume 0"));
    player.setVolume(0.25);
    check(qFuzzyCompare(player.volume(), 0.25), QStringLiteral("volume 0.25"));
    player.setVolume(7.0);
    check(qFuzzyCompare(player.volume(), 1.0), QStringLiteral("volume is clamped to 1"));
    player.setVolume(-3.0);
    check(player.volume() == 0.0, QStringLiteral("volume is clamped to 0"));
    player.setMuted(true);
    check(states == 1 && player.isMuted(), QStringLiteral("stateChanged on mute (%1)").arg(states));
    player.setMuted(true);
    check(states == 1, QStringLiteral("no stateChanged when the mute state does not change"));
}

void errorPathTest(const QString& outDir)
{
    print(QStringLiteral("error path:"));
    mf::VideoPlayer player;
    silence(player);
    player.open(outDir + QStringLiteral("/does-not-exist.mp4"));
    const QString error = waitForLoad(player, 10000);
    check(!error.isEmpty() && !error.startsWith(QStringLiteral("timed out")), QStringLiteral("missing file emits failed(): ") + error);
    check(!player.isLoaded() && !player.isPlaying(), QStringLiteral("failed player is not loaded"));
    const mf::ProbeResult info = mf::probe(outDir + QStringLiteral("/does-not-exist.mp4"));
    check(!info.ok && !info.error.isEmpty(), QStringLiteral("probe of a missing file: ") + info.error);
}

// Destroying players while the engine is still loading or playing must not leave callbacks behind.
void teardownTest(const QString& video)
{
    print(QStringLiteral("teardown:"));
    QElapsedTimer clock;
    clock.start();
    for (int round = 0; round < 4; ++round) {
        auto* player = new mf::VideoPlayer;
        silence(*player);
        player->open(video);
        player->play();
        wait(round * 120); // round 0: destroyed before the engine reported anything
        delete player;
    }
    wait(500); // a queued event reaching a destroyed player would crash here
    check(true, QStringLiteral("4 players destroyed while loading/playing (%1 ms)").arg(clock.elapsed()));
}

// How the viewer and the chat size their frame requests: the picture fitted into their box, even sides.
QSize evenFit(const QSize& picture, const QSize& box)
{
    const QSize size = picture.scaled(box, Qt::KeepAspectRatio);
    return QSize(qMax(2, size.width() & ~1), qMax(2, size.height() & ~1));
}

bool isBorder(QRgb c)
{
    return qRed(c) >= 150 && qGreen(c) <= 100 && qBlue(c) <= 100;
}

// Red pixels from (x, y) inwards along (dx, dy).
int borderRun(const QImage& image, int x, int y, int dx, int dy)
{
    int run = 0;
    while (x >= 0 && y >= 0 && x < image.width() && y < image.height() && isBorder(image.pixel(x, y))) {
        ++run;
        x += dx;
        y += dy;
    }
    return run;
}

// The format-change clip in full: its red border on all four sides, equally thick everywhere (not
// stretched), and its blue fill in the centre. A picture cropped to another aspect ratio loses the
// border on two sides; a letterboxed one starts with black.
bool wholePicture(const QImage& image)
{
    if (image.width() < 32 || image.height() < 32)
        return false;
    const int w = image.width(), h = image.height();
    const int runs[] = {borderRun(image, w / 2, 0, 0, 1), borderRun(image, w / 2, h - 1, 0, -1), borderRun(image, 0, h / 2, 1, 0),
                        borderRun(image, w - 1, h / 2, -1, 0)};
    const auto range = std::minmax_element(std::begin(runs), std::end(runs));
    const QRgb centre = image.pixel(w / 2, h / 2);
    return *range.first >= 4 && *range.second - *range.first <= 2 && qBlue(centre) >= 150 && qRed(centre) <= 100;
}

// The owner's box is square, so a request fitted to one picture and a refit for the next one differ.
void formatChangeTest(const QString& clip, const QString& outDir)
{
    print(QStringLiteral("format change:"));
    mf::VideoPlayer player;
    QVector<QSize>  reported; // videoSize() at each videoSizeChanged()
    QString         error;
    QObject::connect(&player, &mf::VideoPlayer::videoSizeChanged, [&] { reported.append(player.videoSize()); });
    QObject::connect(&player, &mf::VideoPlayer::failed, [&](const QString& message) { error = message; });
    silence(player);
    player.open(clip);
    const QString loadError = waitForLoad(player, 15000);
    check(loadError.isEmpty(), QStringLiteral("format-change clip loads %1").arg(loadError));
    if (!loadError.isEmpty())
        return;

    const QSize box(400, 400);
    QSize       picture = player.videoSize();
    QSize       request = evenFit(picture, box);
    player.setFrameSize(request);
    const bool first = waitUntil([&] { return player.currentFrame().size() == request && wholePicture(player.currentFrame()); }, 3000);
    check(first, QStringLiteral("first picture %1: whole picture at %2").arg(sizeText(picture), sizeText(request)));
    if (first)
        save(player.currentFrame(), outDir + QStringLiteral("/fmt_0.png"));

    player.play();
    for (int change = 1; change <= 2; ++change) {
        const bool signalled = waitUntil([&] { return reported.size() >= change || !error.isEmpty(); }, 6000) && reported.size() >= change;
        const QSize previous = picture;
        picture              = signalled ? reported.at(change - 1) : QSize();
        check(signalled && !picture.isEmpty() && picture != previous,
              QStringLiteral("change %1: videoSizeChanged() reports %2 -> %3").arg(change).arg(sizeText(previous), sizeText(picture)));
        if (!signalled || picture.isEmpty())
            return;

        // Before the owner reacts: the new picture fitted into the old request, never cropped to it.
        const QSize interim = evenFit(picture, request);
        const bool  fitted  = waitUntil([&] { return player.currentFrame().size() == interim && wholePicture(player.currentFrame()); }, 1500);
        check(fitted, QStringLiteral("change %1: until the owner reacts, the whole picture fits the old request %2 at %3")
                          .arg(change)
                          .arg(sizeText(request), sizeText(interim)));
        if (fitted)
            save(player.currentFrame(), outDir + QStringLiteral("/fmt_%1_interim.png").arg(change));

        // The owner's reaction (viewer, chat): a request fitted to its box for the new picture.
        request = evenFit(picture, box);
        player.setFrameSize(request);
        const bool refit = waitUntil([&] { return player.currentFrame().size() == request && wholePicture(player.currentFrame()); }, 1500);
        check(refit, QStringLiteral("change %1: whole picture at the new request %2").arg(change).arg(sizeText(request)));
        if (refit)
            save(player.currentFrame(), outDir + QStringLiteral("/fmt_%1.png").arg(change));
    }

    const bool ended = waitUntil([&] { return player.isEnded() || !error.isEmpty(); }, 8000);
    check(ended && error.isEmpty(), QStringLiteral("plays to the end without failed() %1").arg(error));
    check(reported.size() == 2, QStringLiteral("exactly one videoSizeChanged() per change (%1)").arg(reported.size()));
}

// ---- audio-only engine (--audio) ------------------------------------------------------------------

// How fast the position moves at the current rate: ms of media per ms of wall time over about 1.2 s.
double measuredSpeed(mf::VideoPlayer& player)
{
    const qint64  from = player.position();
    QElapsedTimer clock;
    clock.start();
    wait(1200);
    const qint64 moved = player.position() - from;
    return clock.elapsed() > 0 ? static_cast<double>(moved) / static_cast<double>(clock.elapsed()) : 0.0;
}

int runAudioPlayback(const QString& file, const mf::ProbeResult& info)
{
    print(QStringLiteral("audio-only playback:"));
    mf::VideoPlayer player;
    int             frames          = 0;
    int             positionSignals = 0;
    QString         error;
    QObject::connect(&player, &mf::VideoPlayer::frameReady, [&] { ++frames; });
    QObject::connect(&player, &mf::VideoPlayer::positionChanged, [&](qint64) { ++positionSignals; });
    QObject::connect(&player, &mf::VideoPlayer::failed, [&](const QString& message) {
        error = message;
        print(QStringLiteral("  failed(): ") + message);
    });

    QElapsedTimer clock;
    clock.start();
    silence(player);
    player.open(file, mf::OpenMode::AudioOnly);
    const QString loadError = waitForLoad(player, 15000);
    if (!loadError.isEmpty()) {
        print(QStringLiteral("  playback failed: ") + loadError);
        return 4;
    }
    const qint64 total = player.duration();
    print(QStringLiteral("  loaded in %1 ms: duration %2").arg(clock.elapsed()).arg(seconds(total)));
    check(player.isLoaded() && player.isAudioOnly(), QStringLiteral("isLoaded() and isAudioOnly()"));
    check(!player.hasGraphicsDevice(), QStringLiteral("no D3D11 device was created"));
    check(isSilent(player), QStringLiteral("volume 0 and mute set before open() are kept"));
    check(total > 0 && qAbs(total - info.durationMs) <= 150, QStringLiteral("duration matches the probe (%1 vs %2)").arg(seconds(total), seconds(info.durationMs)));
    check(player.videoSize().isEmpty(), QStringLiteral("no video size"));

    player.play();
    check(player.isPlaying(), QStringLiteral("isPlaying() right after play()"));
    const qint64 target  = qMin<qint64>(total * 40 / 100, 8000);
    QElapsedTimer played;
    played.start();
    const bool reached = waitUntil([&] { return player.position() >= target; }, static_cast<int>(target) + 5000);
    check(reached, QStringLiteral("playback reached %1 (position %2)").arg(seconds(target), seconds(player.position())));
    const qint64 allowed = played.elapsed() / 250 + 4; // at most one every 250 ms
    check(positionSignals >= 1 && positionSignals <= allowed, QStringLiteral("positionChanged throttled (%1 signals, at most %2)").arg(positionSignals).arg(allowed));

    // Playback rates: a measurement, not a check (the speed pill would need 1.5 and 2 on every format,
    // with the pitch kept). The speed is how fast the position moves in about 1.2 s after the change.
    for (const double rate : {0.5, 1.5, 2.0}) {
        const bool supported = player.isPlaybackRateSupported(rate);
        QString    line      = QStringLiteral("  IsPlaybackRateSupported(%1) = %2").arg(rate).arg(supported ? QStringLiteral("yes") : QStringLiteral("no"));
        if (supported && total >= 6000) {
            player.seek(total / 10);
            const bool   set = player.setPlaybackRate(rate);
            const double got = set ? measuredSpeed(player) : 0.0;
            line += QStringLiteral(", set %1, position moves at %2x").arg(set ? QStringLiteral("yes") : QStringLiteral("no")).arg(got, 0, 'f', 2);
        }
        print(line);
    }
    if (player.playbackRate() != 1.0)
        check(player.setPlaybackRate(1.0) && qFuzzyCompare(player.playbackRate(), 1.0), QStringLiteral("back to rate 1"));
    print(QStringLiteral("  pitch at other rates: not measured (only by listening; this tool keeps everything silent)"));

    player.pause();
    wait(200);
    const qint64 pausedAt = player.position();
    wait(600);
    check(!player.isPlaying() && qAbs(player.position() - pausedAt) <= 20, QStringLiteral("position stays while paused (%1)").arg(seconds(pausedAt)));

    const qint64 seekTarget = total / 4;
    player.seek(seekTarget);
    wait(300);
    check(qAbs(player.position() - seekTarget) <= 80, QStringLiteral("seek while paused to %1 (position %2)").arg(seconds(seekTarget), seconds(player.position())));

    player.play();
    player.seek(qMax<qint64>(0, total - 400));
    const bool ended = waitUntil([&] { return player.isEnded(); }, 5000);
    check(ended && !player.isPlaying(), QStringLiteral("isEnded() at the end of the file"));
    player.play();
    wait(400);
    check(player.isPlaying() && player.position() < 1500, QStringLiteral("play() after the end restarts (position %1)").arg(seconds(player.position())));
    player.pause();

    // The card's seek bar after the end: a seek leaves the ended state, and play() continues from there.
    player.play();
    player.seek(qMax<qint64>(0, total - 300));
    check(waitUntil([&] { return player.isEnded(); }, 5000), QStringLiteral("ended again"));
    const qint64 third = total / 3;
    player.seek(third);
    wait(250);
    check(!player.isEnded() && qAbs(player.position() - third) <= 80, QStringLiteral("seek after the end leaves the ended state (position %1)").arg(seconds(player.position())));
    player.play();
    wait(400);
    check(player.isPlaying() && player.position() >= third, QStringLiteral("play() after that continues from the seek (position %1)").arg(seconds(player.position())));
    player.pause();

    check(frames == 0 && player.currentFrame().isNull(), QStringLiteral("no frames from the audio-only engine (%1)").arg(frames));
    check(error.isEmpty(), QStringLiteral("no failed() during playback"));

    player.close();
    check(!player.isLoaded() && !player.isAudioOnly(), QStringLiteral("close() resets the player"));
    player.open(file, mf::OpenMode::AudioOnly);
    check(waitForLoad(player, 15000).isEmpty() && !player.hasGraphicsDevice(), QStringLiteral("the same player opens the file again, still without a device"));
    player.close();

    // The default mode, for comparison: it does create a device for the same file.
    mf::VideoPlayer full;
    silence(full);
    full.open(file);
    const QString fullError = waitForLoad(full, 15000);
    print(QStringLiteral("  Auto mode on the same file: %1, graphics device %2")
              .arg(fullError.isEmpty() ? QStringLiteral("loaded") : fullError, full.hasGraphicsDevice() ? QStringLiteral("yes") : QStringLiteral("no")));
    return 0;
}

void audioErrorTest(const QString& outDir)
{
    print(QStringLiteral("audio error path:"));
    const QString fake = outDir + QStringLiteral("/not_audio.mp3");
    QFile         f(fake);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("this is not an mp3 file\n");
        f.close();
    }
    for (const QString& path : {fake, outDir + QStringLiteral("/does-not-exist.m4a")}) {
        mf::VideoPlayer player;
        silence(player);
        player.open(path, mf::OpenMode::AudioOnly);
        const QString error = waitForLoad(player, 10000);
        check(!error.isEmpty() && !error.startsWith(QStringLiteral("timed out")) && !error.contains(QLatin1String("video")),
              QStringLiteral("%1: failed() with audio wording: %2").arg(QFileInfo(path).fileName(), error));
        check(!player.hasGraphicsDevice(), QStringLiteral("%1: no graphics device").arg(QFileInfo(path).fileName()));
    }
}

void audioTeardownTest(const QString& file)
{
    print(QStringLiteral("audio teardown:"));
    QElapsedTimer clock;
    clock.start();
    for (int round = 0; round < 4; ++round) {
        auto* player = new mf::VideoPlayer;
        silence(*player);
        player->open(file, mf::OpenMode::AudioOnly);
        player->play();
        wait(round * 120);
        delete player;
    }
    wait(500);
    check(true, QStringLiteral("4 audio players destroyed while loading/playing (%1 ms)").arg(clock.elapsed()));
}

// ---- MPEG-4 sink writer (--writer) ---------------------------------------------------------------

// 2 s of a 440 Hz tone at -18 dBFS, 48 kHz mono, AAC 96 kbps, written in 100 ms samples. Runs on the
// calling thread with its own COM and Media Foundation scopes, like an encoder worker would.
QString writeToneM4a(const QString& path, qint64* writtenMs)
{
    using Microsoft::WRL::ComPtr;
    namespace detail = mf::detail;
    *writtenMs = 0;
    if (!detail::mediaFoundationPresent())
        return QStringLiteral("Media Foundation is not available");
    detail::ComScope com(COINIT_MULTITHREADED);
    if (!com.usable())
        return QStringLiteral("CoInitializeEx failed");
    detail::PlatformScope platform;
    if (FAILED(platform.result()))
        return QStringLiteral("MFStartup failed");

    constexpr UINT32 kRate = 48000;
    ComPtr<IMFSinkWriter> writer;
    HRESULT               hr = detail::createMpeg4SinkWriter(path, nullptr, false, &writer);
    ComPtr<IMFMediaType>  out;
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr))
        hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED(hr))
        hr = out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    DWORD stream = 0;
    if (SUCCEEDED(hr))
        hr = writer->AddStream(out.Get(), &stream);
    ComPtr<IMFMediaType> in;
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr))
        hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr))
        hr = in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    if (SUCCEEDED(hr))
        hr = in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    if (SUCCEEDED(hr))
        hr = in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
    if (SUCCEEDED(hr))
        hr = in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 2);
    if (SUCCEEDED(hr))
        hr = writer->SetInputMediaType(stream, in.Get(), nullptr);
    if (SUCCEEDED(hr))
        hr = writer->BeginWriting();
    if (FAILED(hr))
        return QStringLiteral("writer setup failed: ") + detail::hexCode(hr);

    constexpr UINT32 kFrames = kRate / 10; // 100 ms
    const double     amplitude = 32767.0 * std::pow(10.0, -18.0 / 20.0);
    for (int chunk = 0; chunk < 20 && SUCCEEDED(hr); ++chunk) {
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(kFrames * 2, &buffer);
        BYTE* data = nullptr;
        if (SUCCEEDED(hr))
            hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            auto* pcm = reinterpret_cast<qint16*>(data);
            for (UINT32 i = 0; i < kFrames; ++i) {
                const double t = static_cast<double>(chunk * kFrames + i) / kRate;
                pcm[i]         = static_cast<qint16>(std::lround(amplitude * std::sin(2.0 * 3.14159265358979 * 440.0 * t)));
            }
            buffer->Unlock();
            hr = buffer->SetCurrentLength(kFrames * 2);
        }
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr))
            hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
            hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr))
            hr = sample->SetSampleTime(static_cast<LONGLONG>(chunk) * kFrames * 10000000LL / kRate);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleDuration(static_cast<LONGLONG>(kFrames) * 10000000LL / kRate);
        if (SUCCEEDED(hr))
            hr = writer->WriteSample(stream, sample.Get());
        if (SUCCEEDED(hr))
            *writtenMs += 100;
    }
    if (SUCCEEDED(hr))
        hr = writer->Finalize();
    return SUCCEEDED(hr) ? QString() : QStringLiteral("writing failed: ") + detail::hexCode(hr);
}

void writerTest(const QString& outDir)
{
    print(QStringLiteral("MPEG-4 sink writer (AAC):"));
    const QString path = outDir + QStringLiteral("/tone_writer.m4a");
    QFile::remove(path);
    QString       error;
    qint64        written = 0;
    QElapsedTimer clock;
    clock.start();
    QThreadPool pool;
    pool.start([&] { error = writeToneM4a(path, &written); });
    pool.waitForDone();
    check(error.isEmpty() && QFileInfo(path).size() > 1000,
          QStringLiteral("encoded %1 ms to %2 (%3 bytes, %4 ms) %5").arg(written).arg(QFileInfo(path).fileName()).arg(QFileInfo(path).size()).arg(clock.elapsed()).arg(error));
    if (!error.isEmpty())
        return;
    const mf::ProbeResult info = mf::probe(path, 0);
    check(info.ok && info.hasAudio && !info.hasVideo && qAbs(info.durationMs - 2000) <= 120,
          QStringLiteral("the result probes as 2 s of audio (ok=%1 audio=%2 duration %3)").arg(info.ok).arg(info.hasAudio).arg(seconds(info.durationMs)));
    mf::VideoPlayer player;
    silence(player);
    player.open(path, mf::OpenMode::AudioOnly);
    check(waitForLoad(player, 10000).isEmpty() && player.duration() > 1800, QStringLiteral("the result plays in the audio-only engine"));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    SetConsoleOutputCP(CP_UTF8);

    const QStringList args = QCoreApplication::arguments();
    if (args.size() == 3 && args.at(1) == QLatin1String("--writer")) {
        const QString outDir = QFileInfo(args.at(2)).absoluteFilePath();
        QDir().mkpath(outDir);
        if (!mf::startup()) {
            print(QStringLiteral("FAIL: Media Foundation could not be started"));
            return 2;
        }
        writerTest(outDir);
        mf::shutdown();
        print(g_failures == 0 ? QStringLiteral("PASS") : QStringLiteral("FAILED (%1 failed checks)").arg(g_failures));
        return g_failures == 0 ? 0 : 5;
    }
    if (args.size() == 4 && args.at(1) == QLatin1String("--audio")) {
        const QString file   = QFileInfo(args.at(2)).absoluteFilePath();
        const QString outDir = QFileInfo(args.at(3)).absoluteFilePath();
        QDir().mkpath(outDir);
        print(QStringLiteral("file: ") + QDir::toNativeSeparators(file));
        if (!mf::startup()) {
            print(QStringLiteral("FAIL: Media Foundation could not be started"));
            return 2;
        }
        mf::ProbeResult info;
        QElapsedTimer   clock;
        clock.start();
        QThreadPool pool;
        pool.start([&] { info = mf::probe(file, 0); });
        pool.waitForDone();
        print(QStringLiteral("probe (%1 ms): ok=%2 duration=%3 video=%4 audio=%5%6")
                  .arg(clock.elapsed())
                  .arg(info.ok ? QStringLiteral("yes") : QStringLiteral("no"), seconds(info.durationMs),
                       info.hasVideo ? QStringLiteral("yes") : QStringLiteral("no"), info.hasAudio ? QStringLiteral("yes") : QStringLiteral("no"),
                       info.error.isEmpty() ? QString() : QStringLiteral(" error=\"%1\"").arg(info.error)));
        int result = 0;
        if (!info.ok) {
            mf::VideoPlayer player;
            silence(player);
            player.open(file, mf::OpenMode::AudioOnly);
            print(QStringLiteral("player: ") + waitForLoad(player, 15000));
            result = 3;
        } else {
            result = runAudioPlayback(file, info);
            if (result == 0) {
                audioErrorTest(outDir);
                audioTeardownTest(file);
            }
        }
        mf::shutdown();
        if (result == 0 && g_failures > 0)
            result = 5;
        print(result == 0 ? QStringLiteral("PASS") : QStringLiteral("FAILED (exit code %1, %2 failed checks)").arg(result).arg(g_failures));
        return result;
    }
    if (args.size() != 3 && args.size() != 4) {
        print(QStringLiteral("usage: mfvideo_smoketest <video> <outdir> [<format-change clip>]\n"
                             "       mfvideo_smoketest --audio <audio file> <outdir>\n"
                             "       mfvideo_smoketest --writer <outdir>"));
        return 1;
    }
    const QString video  = QFileInfo(args.at(1)).absoluteFilePath();
    const QString outDir = QFileInfo(args.at(2)).absoluteFilePath();
    QDir().mkpath(outDir);
    print(QStringLiteral("file: ") + QDir::toNativeSeparators(video));

    if (!mf::startup()) {
        print(QStringLiteral("FAIL: Media Foundation could not be started"));
        return 2;
    }

    // Probe on a pool thread, the way the plugin does it.
    mf::ProbeResult info;
    QElapsedTimer   clock;
    clock.start();
    QThreadPool pool;
    pool.start([&] { info = mf::probe(video, 960); });
    pool.waitForDone();
    print(QStringLiteral("probe (%1 ms): ok=%2 size=%3 duration=%4 video=%5 audio=%6 poster=%7%8")
              .arg(clock.elapsed())
              .arg(info.ok ? QStringLiteral("yes") : QStringLiteral("no"), sizeText(info.size), seconds(info.durationMs),
                   info.hasVideo ? QStringLiteral("yes") : QStringLiteral("no"), info.hasAudio ? QStringLiteral("yes") : QStringLiteral("no"),
                   info.poster.isNull() ? QStringLiteral("none") : sizeText(info.poster.size()),
                   info.error.isEmpty() ? QString() : QStringLiteral(" error=\"%1\"").arg(info.error)));

    int result = 0;
    if (!info.ok) {
        // Show what the player reports for the same file: it must fail cleanly, too.
        mf::VideoPlayer player;
        silence(player);
        player.open(video);
        print(QStringLiteral("player: ") + waitForLoad(player, 15000));
        result = 3;
    } else {
        if (info.hasVideo) {
            check(!info.poster.isNull(), QStringLiteral("poster decoded"));
            if (!info.poster.isNull()) {
                save(info.poster, outDir + QStringLiteral("/poster.png"));
                check(hasDetail(info.poster), QStringLiteral("poster has picture content"));
                check(qMax(info.poster.width(), info.poster.height()) <= 960, QStringLiteral("poster fits 960 px"));
            }
        }

        clock.restart();
        const mf::ProbeResult quick = mf::probe(video, 0);
        check(quick.ok && quick.size == info.size && quick.durationMs == info.durationMs && quick.poster.isNull(),
              QStringLiteral("metadata-only probe on the main thread agrees (%1 ms)").arg(clock.elapsed()));
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        check(hr == S_OK, QStringLiteral("probe left the thread's COM state balanced"));
        if (SUCCEEDED(hr))
            CoUninitialize();

        result = runPlayback(video, outDir, info);
        if (result == 0) {
            controlsTest();
            errorPathTest(outDir);
            teardownTest(video);
            if (args.size() == 4)
                formatChangeTest(QFileInfo(args.at(3)).absoluteFilePath(), outDir);
        }
    }

    mf::shutdown();
    if (result == 0 && g_failures > 0)
        result = 5;
    print(result == 0 ? QStringLiteral("PASS") : QStringLiteral("FAILED (exit code %1, %2 failed checks)").arg(result).arg(g_failures));
    return result;
}
