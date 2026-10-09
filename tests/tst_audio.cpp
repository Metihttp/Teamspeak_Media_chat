// Unit tests for inline audio (2.2): the audio player card (audiocard.*: size, zones, seek mapping,
// tooltips, every state renders at the same size) and the audio auto-download rule (audioplayback.*).
// Runs inside tsmedia_tests (tests/testmain.h, which creates a QGuiApplication for the fonts).

#include <QDir>
#include <QtTest>

#include <limits>

#include "audiocard.h"
#include "audioplayback.h"
#include "medialink.h"
#include "settings.h"
#include "testmain.h"
#include "uiutil.h"

// settings.cpp keeps its ini file in the plugin's data folder (ts3::dataDir() is stubbed in
// tst_tsmedia.cpp); these tests never load or save it.

namespace {

constexpr quint64 kMB = 1024 * 1024;

MediaEntry audioEntry(MediaState state, qint64 durationMs = 205000, const QString& name = QStringLiteral("song_3f9a1c2e.mp3"))
{
    MediaEntry e;
    e.link.host       = QStringLiteral("voice.example.org");
    e.link.port       = 9987;
    e.link.serverUid  = QStringLiteral("uid=");
    e.link.channelId  = 3;
    e.link.path       = QStringLiteral("/tsmedia");
    e.link.fileName   = name;
    e.link.size       = 4404019;
    e.link.protocol   = MediaLink::kProtocol;
    e.link.durationMs = durationMs;
    e.kind            = kindForFileName(name);
    e.state           = state;
    e.progress        = state == MediaState::Ready ? 1.0 : 0.0;
    return e;
}

PreviewStyle style(int maxWidth = 400, qreal dpr = 1.0)
{
    PreviewStyle st;
    st.font = QFont(QStringLiteral("Segoe UI"));
    st.font.setPointSizeF(9.0);
    st.maxWidth = maxWidth;
    st.dpr      = dpr;
    return st;
}

PlaybackOverlay rest(const MediaEntry& e)
{
    PlaybackOverlay o;
    o.durationMs      = e.link.durationMs;
    o.controlsVisible = true;
    return o;
}

// The centre of the seek band: halfway along the bar, on its line (y = 44).
QPointF seekPoint(const MediaEntry& e, const PlaybackOverlay& o, const PreviewStyle& st, double fraction)
{
    const QSize size = audioCardSize(e, st);
    // Find the bar's ends by scanning for the Seek zone along its line.
    qreal left = -1, right = -1;
    for (qreal x = 0; x < size.width(); x += 0.5) {
        if (audioZoneAt(e, o, st, size, QPointF(x, 44)) == VideoZone::Seek) {
            if (left < 0)
                left = x;
            right = x;
        }
    }
    return QPointF(left + (right - left) * fraction, 44);
}

} // namespace

class TestAudio : public QObject
{
    Q_OBJECT

  private slots:
    void autoDownloadRule_data();
    void autoDownloadRule();
    void kinds_data();
    void kinds();
    void sizeMatchesFileCard_data();
    void sizeMatchesFileCard();
    void everyStateHasTheSameBox();
    void zones();
    void seekOnlyWhereTheBarIsShown();
    void seekFraction();
    void narrowCards();
    void toolTips();
    void hostileValuesDoNotBreakIt();
    void contrast();
};

void TestAudio::autoDownloadRule_data()
{
    QTest::addColumn<bool>("voice");
    QTest::addColumn<bool>("images");
    QTest::addColumn<int>("imageMB");
    QTest::addColumn<int>("videoMB");
    QTest::addColumn<quint64>("limit");

    QTest::newRow("plain audio, video rule off (default): on play") << false << true << 15 << 0 << quint64(0);
    QTest::newRow("plain audio follows the video limit") << false << true << 15 << 25 << quint64(25 * kMB);
    QTest::newRow("plain audio ignores the image setting") << false << false << 15 << 25 << quint64(25 * kMB);
    QTest::newRow("voice: image limit") << true << true << 15 << 0 << quint64(15 * kMB);
    QTest::newRow("voice: never above 16 MB") << true << true << 100 << 0 << quint64(16 * kMB);
    QTest::newRow("voice: images off means on play") << true << false << 15 << 500 << quint64(0);
}

void TestAudio::autoDownloadRule()
{
    QFETCH(bool, voice);
    QFETCH(bool, images);
    QFETCH(int, imageMB);
    QFETCH(int, videoMB);
    QFETCH(quint64, limit);

    Settings s;
    s.autoDownloadImages  = images;
    s.autoDownloadMaxMB   = imageMB;
    s.videoAutoDownloadMB = videoMB;
    QCOMPARE(audioplayback::autoDownloadLimit(s, voice), limit);
    QCOMPARE(audioplayback::downloadsAutomatically(s, voice, 1), limit > 0);
    QCOMPARE(audioplayback::downloadsAutomatically(s, voice, 0), limit > 0); // unknown size: checked once it starts
    if (limit > 0) {
        QVERIFY(audioplayback::downloadsAutomatically(s, voice, limit));
        QVERIFY(!audioplayback::downloadsAutomatically(s, voice, limit + 1));
    }
}

void TestAudio::kinds_data()
{
    QTest::addColumn<QString>("name");
    for (const char* name : {"a.mp3", "a.M4A", "a.wav", "a.flac", "a.ogg", "a.opus", "a.aac", "a.wma", "a.mka", "a.weba"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void TestAudio::kinds()
{
    QFETCH(QString, name);
    QCOMPARE(int(kindForFileName(name)), int(MediaKind::Audio));
}

void TestAudio::sizeMatchesFileCard_data()
{
    QTest::addColumn<int>("maxWidth");
    QTest::addColumn<int>("width");
    QTest::newRow("default") << 400 << 340;
    QTest::newRow("340") << 340 << 340;
    QTest::newRow("narrow chat") << 220 << 220;
    QTest::newRow("smallest") << 120 << 160;
}

void TestAudio::sizeMatchesFileCard()
{
    QFETCH(int, maxWidth);
    QFETCH(int, width);
    const MediaEntry   e  = audioEntry(MediaState::Idle);
    const PreviewStyle st = style(maxWidth);
    QCOMPARE(audioCardSize(e, st), QSize(width, 64));
    QCOMPARE(audioCardSize(e, st), previewLogicalSize(e, st)); // what the chat reserves for the file card
}

// No layout jump: idle, downloading, opening, playing, paused, ended and failed all use one box.
void TestAudio::everyStateHasTheSameBox()
{
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        const PreviewStyle st       = style(400, dpr);
        const QSize        expected = audioCardSize(audioEntry(MediaState::Idle), st);
        QVector<QPair<MediaEntry, PlaybackOverlay>> states;
        MediaEntry idle = audioEntry(MediaState::Idle);
        states.append({idle, rest(idle)});
        MediaEntry downloading = audioEntry(MediaState::Downloading);
        downloading.progress   = 0.45;
        PlaybackOverlay busy   = rest(downloading);
        busy.busy              = true;
        busy.busyProgress      = 0.45;
        states.append({downloading, busy});
        MediaEntry      ready   = audioEntry(MediaState::Ready);
        PlaybackOverlay playing = rest(ready);
        playing.playing         = true;
        playing.positionMs      = 42000;
        states.append({ready, playing});
        PlaybackOverlay ended = rest(ready);
        ended.ended           = true;
        ended.positionMs      = 205000;
        states.append({ready, ended});
        PlaybackOverlay external = rest(ready);
        external.externalOnly    = true;
        states.append({ready, external});
        MediaEntry failed = audioEntry(MediaState::Failed);
        failed.error      = MediaError::NotFound;
        states.append({failed, rest(failed)});
        for (const auto& state : qAsConst(states)) {
            QSize        logical;
            const QImage image = renderAudioCard(state.first, state.second, st, &logical);
            QCOMPARE(logical, expected);
            QCOMPARE(image.size(), QSize(qRound(expected.width() * dpr), qRound(expected.height() * dpr)));
            QCOMPARE(image.devicePixelRatio(), dpr);
        }
    }
}

void TestAudio::zones()
{
    const MediaEntry      e    = audioEntry(MediaState::Ready);
    const PlaybackOverlay o    = rest(e);
    const PreviewStyle    st   = style();
    const QSize           size = audioCardSize(e, st);
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(32, 32)), VideoZone::PlayPause); // the button's centre
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(10, 10)), VideoZone::PlayPause); // its 4 px margin
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(150, 44)), VideoZone::Seek);     // on the bar
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(150, 20)), VideoZone::Body);     // the name
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(size.width() - 4, 44)), VideoZone::Body); // the time column
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(-1, 10)), VideoZone::None);
    QCOMPARE(audioZoneAt(e, o, st, size, QPointF(size.width() + 1, 10)), VideoZone::None);
    // Never the video-only controls.
    for (int x = 0; x < size.width(); x += 3) {
        for (int y = 0; y < size.height(); y += 3) {
            const VideoZone zone = audioZoneAt(e, o, st, size, QPointF(x, y));
            QVERIFY(zone != VideoZone::Mute && zone != VideoZone::Expand);
        }
    }
    // The zone's rectangle holds the point (tooltips are narrowed to it).
    QVERIFY(audioZoneRect(e, o, st, size, QPointF(150, 44)).contains(QPointF(150, 44)));
    QVERIFY(audioZoneRect(e, o, st, size, QPointF(32, 32)).contains(QPointF(32, 32)));
    QVERIFY(audioZoneRect(e, o, st, size, QPointF(-5, -5)).isEmpty());
}

void TestAudio::seekOnlyWhereTheBarIsShown()
{
    const PreviewStyle st = style();
    const QPointF      onBar(150, 44);

    MediaEntry idle = audioEntry(MediaState::Idle);
    QCOMPARE(audioZoneAt(idle, rest(idle), st, audioCardSize(idle, st), onBar), VideoZone::Seek); // seeking starts it there

    MediaEntry downloading = audioEntry(MediaState::Downloading); // the row says "Downloading… 45%"
    downloading.progress   = 0.45;
    QCOMPARE(audioZoneAt(downloading, rest(downloading), st, audioCardSize(downloading, st), onBar), VideoZone::Body);

    MediaEntry queued = audioEntry(MediaState::Queued);
    QCOMPARE(audioZoneAt(queued, rest(queued), st, audioCardSize(queued, st), onBar), VideoZone::Body);

    MediaEntry failed = audioEntry(MediaState::Failed);
    failed.error      = MediaError::Permission;
    QCOMPARE(audioZoneAt(failed, rest(failed), st, audioCardSize(failed, st), onBar), VideoZone::Body);

    MediaEntry      ready    = audioEntry(MediaState::Ready);
    PlaybackOverlay external = rest(ready);
    external.externalOnly    = true;
    QCOMPARE(audioZoneAt(ready, external, st, audioCardSize(ready, st), onBar), VideoZone::Body);

    // A plain TeamSpeak link has no duration: nothing to seek in until the player knows it.
    MediaEntry plain      = audioEntry(MediaState::Idle, 0, QStringLiteral("song.mp3"));
    plain.link.protocol   = 0;
    PlaybackOverlay unknown = rest(plain);
    QCOMPARE(audioZoneAt(plain, unknown, st, audioCardSize(plain, st), onBar), VideoZone::Body);
    unknown.durationMs = 60000; // loaded: the player reports it
    QCOMPARE(audioZoneAt(plain, unknown, st, audioCardSize(plain, st), onBar), VideoZone::Seek);
}

void TestAudio::seekFraction()
{
    const MediaEntry      e    = audioEntry(MediaState::Ready);
    const PlaybackOverlay o    = rest(e);
    const PreviewStyle    st   = style();
    const QSize           size = audioCardSize(e, st);
    QCOMPARE(audioSeekFractionAt(e, o, st, size, QPointF(0, 44)), 0.0);
    QCOMPARE(audioSeekFractionAt(e, o, st, size, QPointF(size.width(), 44)), 1.0);
    const double half = audioSeekFractionAt(e, o, st, size, seekPoint(e, o, st, 0.5));
    QVERIFY2(qAbs(half - 0.5) < 0.06, qPrintable(QString::number(half)));
    // Rising along the bar.
    double last = -1.0;
    for (int x = 0; x <= size.width(); x += 4) {
        const double f = audioSeekFractionAt(e, o, st, size, QPointF(x, 44));
        QVERIFY(f >= last && f >= 0.0 && f <= 1.0);
        last = f;
    }
    // Drawing and hit testing agree in every playback state with the same duration: the bar does not
    // move while the time counts up.
    PlaybackOverlay playing = o;
    playing.playing         = true;
    playing.positionMs      = 100000;
    QCOMPARE(audioSeekFractionAt(e, playing, st, size, QPointF(150, 44)), audioSeekFractionAt(e, o, st, size, QPointF(150, 44)));
}

void TestAudio::narrowCards()
{
    for (const int width : {120, 160, 200, 240}) {
        const PreviewStyle st = style(width);
        for (const qint64 duration : {qint64(9000), qint64(205000), qint64(3723000)}) {
            const MediaEntry      e    = audioEntry(MediaState::Ready, duration, QStringLiteral("a_really_long_file_name_that_never_fits_3f9a1c2e.flac"));
            const PlaybackOverlay o    = rest(e);
            const QSize           size = audioCardSize(e, st);
            QSize                 logical;
            QVERIFY(!renderAudioCard(e, o, st, &logical).isNull());
            QCOMPARE(logical, size);
            // The bar is still there and still maps to 0..1.
            const QPointF p = seekPoint(e, o, st, 0.5);
            QVERIFY2(p.x() > 64 && p.x() < size.width(), qPrintable(QStringLiteral("width %1 duration %2").arg(width).arg(duration)));
            QCOMPARE(audioZoneAt(e, o, st, size, p), VideoZone::Seek);
        }
    }
}

void TestAudio::toolTips()
{
    MediaEntry      e = audioEntry(MediaState::Ready);
    PlaybackOverlay o = rest(e);
    QCOMPARE(audioZoneToolTip(e, o, VideoZone::PlayPause, 0), QStringLiteral("Play"));
    o.playing = true;
    QCOMPARE(audioZoneToolTip(e, o, VideoZone::PlayPause, 0), QStringLiteral("Pause"));
    o.playing = false;
    o.ended   = true;
    QCOMPARE(audioZoneToolTip(e, o, VideoZone::PlayPause, 0), QStringLiteral("Replay"));
    QCOMPARE(audioZoneToolTip(e, rest(e), VideoZone::Seek, 0.5), QString::fromUtf8("Jump to 1:42"));
    QVERIFY(audioZoneToolTip(e, rest(e), VideoZone::Body, 0).isEmpty()); // name and size from the caller
    PlaybackOverlay external = rest(e);
    external.externalOnly    = true;
    QCOMPARE(audioZoneToolTip(e, external, VideoZone::PlayPause, 0), QStringLiteral("Open in default app"));
    QVERIFY(!audioToolTipDetail(e, external).isEmpty());

    MediaEntry downloading = audioEntry(MediaState::Downloading);
    PlaybackOverlay busy   = rest(downloading);
    busy.busy              = true;
    QCOMPARE(audioZoneToolTip(downloading, busy, VideoZone::Body, 0), QString::fromUtf8("Downloading… Click to cancel autoplay."));

    MediaEntry gone = audioEntry(MediaState::Failed);
    gone.error      = MediaError::NotFound;
    QVERIFY(audioZoneToolTip(gone, rest(gone), VideoZone::PlayPause, 0).isEmpty()); // nothing to press
    QVERIFY(!audioToolTipDetail(gone, rest(gone)).contains(QStringLiteral("retry")));
    MediaEntry quota = audioEntry(MediaState::Failed);
    quota.error      = MediaError::Quota;
    QCOMPARE(audioZoneToolTip(quota, rest(quota), VideoZone::PlayPause, 0), QStringLiteral("Retry download"));
    QVERIFY(audioToolTipDetail(quota, rest(quota)).endsWith(QStringLiteral("Click to retry.")));
}

// Values from someone else's link or a misbehaving player must not break drawing or hit testing.
void TestAudio::hostileValuesDoNotBreakIt()
{
    const PreviewStyle st = style();
    MediaEntry         e  = audioEntry(MediaState::Ready, std::numeric_limits<qint64>::max() / 4);
    e.link.fileName       = QStringLiteral("x") + QChar(0x202E) + QStringLiteral("3pm.exe.mp3");
    PlaybackOverlay o     = rest(e);
    o.positionMs          = -5000;
    QVERIFY(!renderAudioCard(e, o, st, nullptr).isNull());
    o.positionMs = std::numeric_limits<qint64>::max();
    o.playing    = true;
    QVERIFY(!renderAudioCard(e, o, st, nullptr).isNull());
    const QSize size = audioCardSize(e, st);
    const double f   = audioSeekFractionAt(e, o, st, size, QPointF(1e9, 44));
    QVERIFY(f >= 0.0 && f <= 1.0);
    QVERIFY(!audioZoneToolTip(e, o, VideoZone::Seek, 7.0).isEmpty()); // clamped
    e.link.durationMs = -1;
    e.link.size       = 0;
    QVERIFY(!renderAudioCard(e, rest(e), st, nullptr).isNull());
    QVERIFY(audioZoneToolTip(e, rest(e), VideoZone::Seek, 0.5).isEmpty());
}

void TestAudio::contrast()
{
    for (const bool dark : {false, true}) {
        for (const PreviewColorPair& pair : audioCardColorPairs(dark)) {
            const double ratio = ui::contrastRatio(pair.foreground, pair.background);
            QVERIFY2(ratio + 0.005 >= pair.minimum,
                     qPrintable(QStringLiteral("%1 (%2): %3 < %4").arg(pair.name, dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(ratio).arg(pair.minimum)));
        }
    }
}

TSMEDIA_REGISTER_TEST(TestAudio)

#include "tst_audio.moc"
