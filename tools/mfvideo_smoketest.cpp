// mfvideo_smoketest <video> <outdir> [<format-change clip>]
//
// Exercises src/video/mfvideo.* outside TeamSpeak: probes the file on a worker thread (as the plugin
// does) and saves the poster, then plays it headless through mf::VideoPlayer, saving frames while it
// checks pause, seek, resize, resume, end of stream, loop, close and teardown.
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
#include <QFileInfo>
#include <QImage>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>

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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    SetConsoleOutputCP(CP_UTF8);

    const QStringList args = QCoreApplication::arguments();
    if (args.size() != 3 && args.size() != 4) {
        print(QStringLiteral("usage: mfvideo_smoketest <video> <outdir> [<format-change clip>]"));
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
