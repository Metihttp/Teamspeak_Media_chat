// Tests for the 2.2 voice-message parts that need Windows: the AAC encoder (generated sound -> .m4a ->
// Media Foundation probe), the recorder driven by FakeCapture (never a real microphone), and the chat's
// voice card (geometry, zones, states, colours). Target tsmedia_voice_tests; runs its classes through
// tests/testmain.h with a QGuiApplication (the card needs fonts).

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QtTest>

#include <QSet>

#include <windows.h>

#include <objbase.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "audio/aacwriter.h"
#include "audio/fakecapture.h"
#include "audio/voicerecorder.h"
#include "audio/wasapicapture.h"
#include "audio/waveform.h"
#include "audiocard.h"
#include "previewrenderer.h"
#include "testmain.h"
#include "uiutil.h"
#include "video/mfvideo.h"
#include "voicecard.h"

namespace {

std::vector<int16_t> tone(int rate, qint64 ms, double hz, double db)
{
    std::vector<int16_t> out(static_cast<size_t>(rate * ms / 1000));
    const double         amplitude = 32767.0 * std::pow(10.0, db / 20.0);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate)));
    return out;
}

voice::VoiceRecorder::BackendFactory fake(const voice::FakeCapture::Options& options)
{
    return [options]() -> std::unique_ptr<voice::CaptureBackend> { return std::make_unique<voice::FakeCapture>(options); };
}

using State = voice::VoiceRecorder::State;

} // namespace

class TestVoiceMedia : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QVERIFY(m_dir.isValid());
    }

    // ---- encoder ---------------------------------------------------------------------------------
    void encoderRoundTrip_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<int>("ms");
        QTest::newRow("48 kHz, 3 s") << 48000 << 3000;
        QTest::newRow("44.1 kHz, 1.25 s") << 44100 << 1250;
        QTest::newRow("48 kHz, 0.5 s (shortest)") << 48000 << 500;
    }
    void encoderRoundTrip()
    {
        QFETCH(int, rate);
        QFETCH(int, ms);
        const std::vector<int16_t> pcm  = tone(rate, ms, 440.0, -12.0);
        const QString              path = m_dir.filePath(QStringLiteral("round_%1_%2.m4a").arg(rate).arg(ms));
        QElapsedTimer              clock;
        clock.start();
        const voice::AacResult r = voice::writeAac(path, pcm.data(), static_cast<qint64>(pcm.size()), rate, 1.0);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(clock.elapsed() < 5000);
        const qint64 size = QFileInfo(path).size();
        // 96 kbps = 12 000 bytes a second, plus the container.
        QVERIFY2(size > ms * 12 * 8 / 10 && size < ms * 12 + 8000, qPrintable(QString::number(size)));
        const mf::ProbeResult probe = mf::probe(path, 0);
        QVERIFY2(probe.ok, qPrintable(probe.error));
        QVERIFY(probe.hasAudio);
        QVERIFY(!probe.hasVideo);
        QVERIFY2(qAbs(probe.durationMs - ms) <= 60, qPrintable(QString::number(probe.durationMs)));
    }

    void encoderGain()
    {
        // A quiet recording is lifted (at most +12 dB): the file is valid either way.
        const std::vector<int16_t> pcm  = tone(48000, 1000, 300.0, -30.0);
        const QString              path = m_dir.filePath(QStringLiteral("gain.m4a"));
        const double               gain = waveform::normalizeGain(waveform::peakDb(pcm.data(), static_cast<int>(pcm.size())));
        QVERIFY(gain > 3.9 && gain < 4.0); // +12 dB
        QVERIFY(voice::writeAac(path, pcm.data(), static_cast<qint64>(pcm.size()), 48000, gain).ok);
        QVERIFY(mf::probe(path, 0).ok);
    }

    void encoderRefusesAndCancels()
    {
        const std::vector<int16_t> pcm = tone(48000, 2000, 440.0, -12.0);
        // Rates the encoder doesn't take, and nothing to encode.
        const QString bad = m_dir.filePath(QStringLiteral("bad.m4a"));
        QVERIFY(!voice::writeAac(bad, pcm.data(), 1000, 22050, 1.0).ok);
        QVERIFY(!voice::writeAac(bad, pcm.data(), 0, 48000, 1.0).ok);
        QVERIFY(!QFile::exists(bad));
        // An unwritable place.
        const voice::AacResult nowhere = voice::writeAac(m_dir.filePath(QStringLiteral("missing/dir/x.m4a")), pcm.data(), 48000, 48000, 1.0);
        QVERIFY(!nowhere.ok);
        QVERIFY(!nowhere.error.isEmpty());
        // Canceled: no file left.
        std::atomic<bool> cancel{true};
        const QString     canceled = m_dir.filePath(QStringLiteral("canceled.m4a"));
        const voice::AacResult r   = voice::writeAac(canceled, pcm.data(), static_cast<qint64>(pcm.size()), 48000, 1.0, &cancel);
        QVERIFY(!r.ok);
        QVERIFY(r.canceled);
        QVERIFY(!QFile::exists(canceled));
    }

    // ---- recorder with generated sound ---------------------------------------------------------------
    void recorderRecordsAndEncodes()
    {
        voice::FakeCapture::Options o;
        o.realtime   = false;
        o.endAfterMs = 3000;
        voice::VoiceRecorder rec;
        QSignalSpy           changed(&rec, &voice::VoiceRecorder::stateChanged);
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 1000); // the poll reports it on the GUI thread
        QCOMPARE(rec.elapsedMs(), qint64(3000));
        QVERIFY(qAbs(rec.binCount() - 60) <= 1); // 50 ms bins
        QVERIFY(rec.loudestDb() > -13.0f && rec.loudestDb() < -11.0f);
        QCOMPARE(rec.format().sampleRate, 48000);
        QCOMPARE(rec.deviceName(), QStringLiteral("Test microphone"));
        QVERIFY(!rec.limitReached());
        QVERIFY(!rec.deviceLost());
        const QByteArray levels = rec.waveformLevels();
        QCOMPARE(levels.size(), 64);
        for (const char level : levels)
            QCOMPARE(int(level), 15); // a steady tone: loud everywhere (scaled up to 15)

        const QString path = m_dir.filePath(QStringLiteral("recorded.m4a"));
        rec.encode(path);
        QCOMPARE(rec.state(), State::Encoding);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Encoded, 10000);
        QCOMPARE(rec.encodedPath(), path);
        QCOMPARE(rec.encodedBytes(), QFileInfo(path).size());
        const mf::ProbeResult probe = mf::probe(path, 0);
        QVERIFY(probe.ok && probe.hasAudio);
        QVERIFY2(qAbs(probe.durationMs - 3000) <= 60, qPrintable(QString::number(probe.durationMs)));
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 2, 1000); // (Recording,) Captured, Encoded: Opening / Encoding are set by the calls

        // Encoding again from memory (the "Try again" after a failed save) gives the same length.
        const QString again = m_dir.filePath(QStringLiteral("recorded_again.m4a"));
        rec.encode(again);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Encoded, 10000);
        QVERIFY(qAbs(mf::probe(again, 0).durationMs - 3000) <= 60);

        rec.cancel();
        QCOMPARE(rec.state(), State::Idle);
        QCOMPARE(rec.binCount(), 0);
    }

    void recorderStopsAtTheLimit()
    {
        voice::FakeCapture::Options o;
        o.realtime = false; // endless
        voice::VoiceRecorder rec;
        rec.start(fake(o), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 10000);
        QVERIFY(rec.limitReached());
        QCOMPARE(rec.elapsedMs(), qint64(1000));
    }

    void recorderKeepsSoundWhenUnplugged()
    {
        voice::FakeCapture::Options o;
        o.realtime      = false;
        o.unplugAfterMs = 1500;
        voice::VoiceRecorder rec;
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 10000);
        QVERIFY(rec.deviceLost());
        QCOMPARE(rec.captureError().error, voice::CaptureError::Disconnected);
        QCOMPARE(rec.elapsedMs(), qint64(1500));
    }

    void recorderReportsOpenErrors_data()
    {
        QTest::addColumn<int>("error");
        QTest::newRow("privacy") << int(voice::CaptureError::PrivacyBlocked);
        QTest::newRow("no device") << int(voice::CaptureError::NoDevice);
        QTest::newRow("busy") << int(voice::CaptureError::Busy);
        QTest::newRow("format") << int(voice::CaptureError::UnsupportedFormat);
        QTest::newRow("generic") << int(voice::CaptureError::Generic);
    }
    void recorderReportsOpenErrors()
    {
        QFETCH(int, error);
        voice::FakeCapture::Options o;
        o.openError = static_cast<voice::CaptureError>(error);
        voice::VoiceRecorder rec;
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::CaptureFailed, 5000);
        QCOMPARE(int(rec.captureError().error), error);
        QCOMPARE(rec.elapsedMs(), qint64(0));
    }

    void recorderStopsInRealTime()
    {
        voice::FakeCapture::Options o; // paced like a microphone
        voice::VoiceRecorder        rec;
        QSignalSpy                  ticks(&rec, &voice::VoiceRecorder::tick);
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Recording, 5000);
        QTest::qWait(400);
        rec.stop();
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 2000);
        QVERIFY2(rec.elapsedMs() >= 250 && rec.elapsedMs() <= 1200, qPrintable(QString::number(rec.elapsedMs())));
        QVERIFY(ticks.count() >= 3); // the window's 33 ms updates while recording
    }

    void recorderSilence()
    {
        voice::FakeCapture::Options o;
        o.realtime   = false;
        o.signal     = voice::FakeCapture::Signal::Silence;
        o.endAfterMs = 4000;
        voice::VoiceRecorder rec;
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 10000);
        QCOMPARE(rec.loudestDb(), waveform::kSilenceDb); // "No sound from the microphone"
        QCOMPARE(rec.waveformLevels(), QByteArray(64, '\0'));
    }

    // Lists Windows' microphones (reading names only: nothing is opened or recorded).
    void listsMicrophones()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const voice::EndpointList list = voice::listCaptureEndpoints();
        QSet<QString> ids;
        for (const voice::Endpoint& e : list.endpoints) {
            QVERIFY(!e.id.isEmpty());
            QVERIFY(!ids.contains(e.id));
            ids.insert(e.id);
            QCOMPARE(voice::endpointIdIn(e.id), e.id); // capture endpoint ids have the expected shape
        }
        if (!list.defaultCommunications.isEmpty())
            QVERIFY(ids.contains(list.defaultCommunications));
        // The setting picks an endpoint by id; an unknown id falls back.
        if (!list.endpoints.isEmpty()) {
            const voice::DeviceChoice c = voice::chooseCaptureDevice(list.endpoints.first().id, {}, list);
            QCOMPARE(c.source, voice::DeviceChoice::Source::Setting);
        }
        qInfo("%d capture endpoint(s)", list.endpoints.size());
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x80070005L)), voice::CaptureError::PrivacyBlocked);
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x8889000AL)), voice::CaptureError::Busy);         // AUDCLNT_E_DEVICE_IN_USE
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x88890004L)), voice::CaptureError::Disconnected); // AUDCLNT_E_DEVICE_INVALIDATED
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x88890008L)), voice::CaptureError::UnsupportedFormat);
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x80070490L)), voice::CaptureError::NoDevice);
        QCOMPARE(voice::captureErrorFor(static_cast<long>(0x80004005L)), voice::CaptureError::Generic);
        if (SUCCEEDED(com))
            CoUninitialize();
    }

    void recorderCancelJoinsQuickly()
    {
        voice::FakeCapture::Options o;
        o.realtime   = false;
        o.endAfterMs = 300000; // the longest message
        voice::VoiceRecorder rec;
        rec.start(fake(o), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(rec.state(), State::Captured, 20000);
        const QString path = m_dir.filePath(QStringLiteral("canceled_encode.m4a"));
        rec.encode(path);
        QTest::qWait(50);
        QElapsedTimer clock;
        clock.start();
        rec.cancel(); // during the encode
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(rec.state(), State::Idle);
        QVERIFY(!QFile::exists(path));

        // Cancel while recording (and destruction while recording) returns at once too.
        voice::FakeCapture::Options live;
        auto*                       other = new voice::VoiceRecorder;
        other->start(fake(live), 300000);
        QTRY_COMPARE_WITH_TIMEOUT(other->state(), State::Recording, 5000);
        clock.restart();
        delete other;
        QVERIFY(clock.elapsed() < 500);
    }

  private:
    QTemporaryDir m_dir;
};

// ---- the voice card -------------------------------------------------------------------------------------

class TestVoiceCard : public QObject
{
    Q_OBJECT

    static MediaEntry entry(MediaState state = MediaState::Ready)
    {
        MediaEntry e;
        e.link.host       = QStringLiteral("127.0.0.1");
        e.link.port       = 9987;
        e.link.serverUid  = QStringLiteral("uid");
        e.link.channelId  = 1;
        e.link.path       = QStringLiteral("/tsmedia");
        e.link.fileName   = QStringLiteral("voice_message_3f9a1c2e.m4a");
        e.link.size       = 146320;
        e.link.protocol   = MediaLink::kProtocol;
        e.link.durationMs = 12040;
        e.link.voice      = true;
        QByteArray levels(64, '\0');
        for (int i = 0; i < 64; ++i)
            levels[i] = static_cast<char>((i * 7) % 16);
        e.link.waveform = levels;
        e.kind          = kindForFileName(e.link.fileName);
        e.state         = state;
        e.progress      = state == MediaState::Ready ? 1.0 : 0.0;
        return e;
    }
    static PreviewStyle style(int maxWidth = 400, qreal dpr = 1.0, bool dark = false)
    {
        PreviewStyle s;
        s.maxWidth = maxWidth;
        s.dpr      = dpr;
        s.dark     = dark;
        s.font.setPixelSize(13);
        return s;
    }
    static PlaybackOverlay overlay(const MediaEntry& e)
    {
        PlaybackOverlay o;
        o.durationMs = e.link.durationMs;
        return o;
    }

  private slots:
    void isVoice()
    {
        MediaEntry e = entry();
        QVERIFY(isVoiceCard(e));
        e.link.voice = false;
        QVERIFY(!isVoiceCard(e));
        MediaEntry pic = entry();
        pic.kind = MediaKind::Image;
        QVERIFY(!isVoiceCard(pic));
    }

    void sizeNeverJumps()
    {
        // The same box in every state, at every width and scale; previewLogicalSize agrees.
        const QList<MediaState> states{MediaState::Idle, MediaState::Queued, MediaState::Downloading, MediaState::Ready, MediaState::Failed};
        for (const int width : {120, 160, 200, 320, 400, 1200}) {
            for (const qreal dpr : {1.0, 1.5, 2.0}) {
                const PreviewStyle st       = style(width, dpr);
                const QSize        expected = voiceCardSize(st);
                QCOMPARE(expected.height(), 56);
                QCOMPARE(expected.width(), qBound(160, width, 320));
                for (const MediaState state : states) {
                    const MediaEntry e = entry(state);
                    QCOMPARE(previewLogicalSize(e, st), expected);
                    QCOMPARE(audioCardSize(e, st), expected);
                    for (int variant = 0; variant < 4; ++variant) {
                        PlaybackOverlay o = overlay(e);
                        o.playing         = variant == 1;
                        o.positionMs      = variant >= 1 ? 4200 : 0;
                        o.busy            = variant == 2;
                        o.externalOnly    = variant == 3;
                        QSize        logical;
                        const QImage img = renderAudioCard(e, o, st, &logical); // audiocard hands it over
                        QCOMPARE(logical, expected);
                        QCOMPARE(img.size(), QSize(qRound(expected.width() * dpr), qRound(expected.height() * dpr)));
                    }
                    QSize logical;
                    renderPreview(e, MediaStill(), st, &logical); // no inline player (no Media Foundation)
                    QCOMPARE(logical, expected);
                }
            }
        }
    }

    void zones()
    {
        const MediaEntry      e  = entry();
        const PreviewStyle    st = style();
        const PlaybackOverlay o  = overlay(e);
        const QSize           size = voiceCardSize(st);
        QCOMPARE(audioZoneAt(e, o, st, size, QPointF(28, 28)), VideoZone::PlayPause); // the button
        QCOMPARE(audioZoneAt(e, o, st, size, QPointF(8, 28)), VideoZone::PlayPause);  // its 4 px margin
        const QRectF wave = voiceWaveRect(size);
        QCOMPARE(audioZoneAt(e, o, st, size, wave.center()), VideoZone::Seek);
        QCOMPARE(audioZoneAt(e, o, st, size, QPointF(wave.center().x(), 7)), VideoZone::Seek); // full height
        QCOMPARE(audioZoneAt(e, o, st, size, QPointF(size.width() - 20, 28)), VideoZone::Body); // the time
        QCOMPARE(audioZoneAt(e, o, st, size, QPointF(-1, 28)), VideoZone::None);
        QCOMPARE(audioZoneRect(e, o, st, size, wave.center()).toRect(), audioZoneRect(e, o, st, size, wave.center() + QPointF(5, 0)).toRect());
        // Seek fractions: 0 at the left edge, 1 at the right, monotonic.
        QCOMPARE(audioSeekFractionAt(e, o, st, size, QPointF(wave.left(), 28)), 0.0);
        QCOMPARE(audioSeekFractionAt(e, o, st, size, QPointF(wave.right(), 28)), 1.0);
        double last = -1;
        for (qreal x = wave.left(); x <= wave.right(); x += 3) {
            const double f = audioSeekFractionAt(e, o, st, size, QPointF(x, 28));
            QVERIFY(f >= last);
            last = f;
        }
        // A failed or external card has nothing to seek.
        MediaEntry failedEntry = entry(MediaState::Failed);
        failedEntry.error      = MediaError::NotConnected;
        QCOMPARE(audioZoneAt(failedEntry, overlay(failedEntry), st, size, wave.center()), VideoZone::Body);
        PlaybackOverlay external = o;
        external.externalOnly    = true;
        QCOMPARE(audioZoneAt(e, external, st, size, wave.center()), VideoZone::Body);
        // Without a duration there is nothing to seek either.
        MediaEntry noLength      = entry();
        noLength.link.durationMs = 0;
        QCOMPARE(audioZoneAt(noLength, overlay(noLength), st, size, wave.center()), VideoZone::Body);
    }

    void layout()
    {
        // 320 px: waveform from 56 to 320 - 10 - 40 - 8 = 262, 3 px bars with 2 px gaps.
        const QRectF wave = voiceWaveRect(QSize(320, 56));
        QCOMPARE(wave.left(), 56.0);
        QCOMPARE(wave.right(), 262.0);
        QCOMPARE(voiceBarCount(wave.width()), 41);
        QCOMPARE(voiceBarCount(3), 1);
        QCOMPARE(voiceBarCount(2), 0);
        // Under 180 px the time goes and the waveform takes its room.
        QCOMPARE(voiceWaveRect(QSize(170, 56)).right(), 160.0);
        QCOMPARE(voiceWaveRect(QSize(180, 56)).right(), 122.0);
    }

    void tooltips()
    {
        const MediaEntry e = entry();
        PlaybackOverlay  o = overlay(e);
        QCOMPARE(audioZoneToolTip(e, o, VideoZone::PlayPause, 0), QStringLiteral("Play"));
        o.playing = true;
        QCOMPARE(audioZoneToolTip(e, o, VideoZone::PlayPause, 0), QStringLiteral("Pause"));
        QCOMPARE(audioZoneToolTip(e, o, VideoZone::Seek, 0.5), QStringLiteral("Jump to 0:06"));
        QCOMPARE(voiceToolTipName(e), QStringLiteral("Voice message · 0:12"));
        QVERIFY(audioToolTipDetail(e, overlay(e)).isEmpty());
        MediaEntry failedEntry = entry(MediaState::Failed);
        failedEntry.error      = MediaError::Permission;
        QVERIFY(!audioToolTipDetail(failedEntry, overlay(failedEntry)).isEmpty());
    }

    void contrast()
    {
        for (const bool dark : {false, true}) {
            for (const PreviewColorPair& pair : voiceCardColorPairs(dark)) {
                const double ratio = ui::contrastRatio(pair.foreground, pair.background);
                QVERIFY2(ratio + 0.005 >= pair.minimum, qPrintable(QStringLiteral("%1 %2: %3").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), pair.name).arg(ratio)));
            }
        }
    }

    void playedBarsChangeColour()
    {
        // Idle vs half played: the played half differs, the rest does not.
        const MediaEntry   e  = entry();
        const PreviewStyle st = style(320, 1.0, true);
        PlaybackOverlay    idle = overlay(e);
        PlaybackOverlay    half = idle;
        half.playing            = true;
        half.positionMs         = e.link.durationMs / 2;
        const QImage a          = renderAudioCard(e, idle, st, nullptr);
        const QImage b          = renderAudioCard(e, half, st, nullptr);
        const QRectF wave       = voiceWaveRect(voiceCardSize(st));
        int          changedLeft = 0, changedRight = 0;
        for (int x = qRound(wave.left()); x < qRound(wave.right()); ++x) {
            for (int y = 10; y < 46; ++y) {
                if (a.pixel(x, y) == b.pixel(x, y))
                    continue;
                if (x < wave.center().x() - 3)
                    ++changedLeft;
                else if (x > wave.center().x() + 3)
                    ++changedRight;
            }
        }
        QVERIFY(changedLeft > 50);
        QCOMPARE(changedRight, 0);
    }
};

TSMEDIA_REGISTER_TEST(TestVoiceCard)

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    TestVoiceMedia tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return testmain::run(&tc, argc, argv);
}

#include "tst_voicemedia.moc"
