// Unit tests for the 2.4 compression planner (src/videocompress.*): when a video is compressed or
// converted, the preset, size and frame rate it gets, fitting the upload limit, the failure texts, the
// send window's Quality entries, and the compression settings' clamping.

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "settings.h"
#include "testmain.h"
#include "videocompress.h"

using namespace videocompress;

namespace {

constexpr quint64 kMB = 1024ull * 1024;

VideoFacts video(quint64 bytes, qint64 durationMs, QSize display, double fps = 30.0, const char* codec = "h264", const char* ext = "mp4")
{
    VideoFacts f;
    f.bytes         = bytes;
    f.durationMs    = durationMs;
    f.display       = display;
    f.fps           = fps;
    f.probed        = true;
    f.hasVideo      = true;
    f.decodable     = true;
    f.hasAudio      = true;
    f.audioChannels = 2;
    f.videoCodec    = QString::fromLatin1(codec);
    f.extension     = QString::fromLatin1(ext);
    f.is32Bit       = false;
    return f;
}

Options defaults()
{
    Options o; // on, 25 MB, 720p, convert on, 100 MB limit, Auto
    return o;
}

} // namespace

class TestVideoCompress : public QObject
{
    Q_OBJECT

  private slots:
    void smallVideoGoesAsItIs()
    {
        const Plan plan = planCompression(video(10 * kMB, 30000, QSize(1920, 1080)), defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::SmallEnough);
    }

    void phoneClipBecomes720p()
    {
        // The design's example: 240 MB, 62 s, 1080 x 1920 portrait at 30 fps.
        const Plan plan = planCompression(video(240 * kMB, 62000, QSize(1080, 1920)), defaults());
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.reason, Reason::Shrink); // over the limit, but the preset fits anyway
        QCOMPARE(plan.frameSize, QSize(720, 1280));
        QCOMPARE(plan.fps, 30);
        QCOMPARE(plan.fpsCap, 60);
        QCOMPARE(plan.videoKbps, 2500);
        QCOMPARE(plan.audioKbps, 128);
        QCOMPARE(plan.audioChannels, 2);
        QVERIFY(plan.estimatedBytes > 19 * kMB && plan.estimatedBytes < 21 * kMB);
    }

    void neverUpscales()
    {
        const Plan plan = planCompression(video(60 * kMB, 120000, QSize(640, 360)), defaults());
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.frameSize, QSize(640, 360));
        QVERIFY(plan.videoKbps < 2500); // scaled down for the smaller picture
        QVERIFY(plan.videoKbps >= 250);
    }

    void scaledEvenSizes_data()
    {
        QTest::addColumn<QSize>("display");
        QTest::addColumn<int>("shortSide");
        QTest::addColumn<QSize>("expected");
        QTest::newRow("1080p to 720p") << QSize(1920, 1080) << 720 << QSize(1280, 720);
        QTest::newRow("portrait") << QSize(1080, 1920) << 720 << QSize(720, 1280);
        QTest::newRow("odd source") << QSize(1366, 769) << 720 << QSize(1278, 720);
        QTest::newRow("odd small source") << QSize(641, 361) << 720 << QSize(642, 362);
        QTest::newRow("square") << QSize(1440, 1440) << 480 << QSize(480, 480);
        QTest::newRow("to 480p") << QSize(1920, 1080) << 480 << QSize(854, 480);
        QTest::newRow("tiny") << QSize(1, 1) << 720 << QSize(2, 2);
        QTest::newRow("panorama capped") << QSize(16000, 1000) << 1080 << QSize(4096, 256);
        QTest::newRow("empty") << QSize() << 720 << QSize();
    }
    void scaledEvenSizes()
    {
        QFETCH(QSize, display);
        QFETCH(int, shortSide);
        QFETCH(QSize, expected);
        const QSize out = scaledEven(display, shortSide);
        QCOMPARE(out, expected);
        if (!out.isEmpty()) {
            QCOMPARE(out.width() % 2, 0);
            QCOMPARE(out.height() % 2, 0);
            QVERIFY(qMax(out.width(), out.height()) <= kMaxOutputSide);
        }
    }

    void sixtyFpsKeepsItsRate()
    {
        const VideoFacts game = video(400 * kMB, 120000, QSize(1920, 1080), 60.0);
        Plan             plan = planCompression(game, defaults());
        QCOMPARE(plan.fps, 60);
        QCOMPARE(plan.videoKbps, 3750); // 1.5x above 30 fps
        Options smaller   = defaults();
        smaller.shortSide = 480;
        plan              = planCompression(game, smaller);
        QCOMPARE(plan.fps, 30); // 480p is capped at 30 fps
        QCOMPARE(plan.fpsCap, 30);
        QCOMPARE(plan.frameSize, QSize(854, 480));
        QCOMPARE(plan.videoKbps, 1200);
    }

    void unknownRateIs30()
    {
        const Plan plan = planCompression(video(100 * kMB, 60000, QSize(1280, 720), 0.0), defaults());
        QCOMPARE(plan.fps, 30);
    }

    void notWorthIt()
    {
        // 720p at about 2 Mbit/s, 60 MB: the preset would save too little.
        const VideoFacts efficient = video(60 * kMB, 240000, QSize(1280, 720));
        Plan             plan      = planCompression(efficient, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::NotWorthIt);
        // Picked in the send window: done anyway.
        Options asked = defaults();
        asked.request = Request::P720;
        plan          = planCompression(efficient, asked);
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.reason, Reason::Chosen);
        QVERIFY(plan.videoKbps <= static_cast<int>((60.0 * kMB * 8 / 240.0 / 1000.0 - 128) * kSourceBitrateCap) + 1);
    }

    void lowBitrateVideoThatFitsGoesAsItIs()
    {
        // 60 min of 640 x 360 H.264 at 95 MB, 100 MB limit: even the bitrate floor would be about 172 MB.
        // It fits, so it goes as it is (not "too long to fit").
        const VideoFacts lecture = video(95 * kMB, 60 * 60 * 1000, QSize(640, 360));
        Plan             plan    = planCompression(lecture, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::NotWorthIt);
        // 25 min of 720p at 90 MB: 360p would fit the limit but be larger than the original.
        const VideoFacts talk = video(90 * kMB, 25 * 60 * 1000, QSize(1280, 720));
        plan                  = planCompression(talk, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::NotWorthIt);
        // The send window: Original, the default; no entry that isn't smaller.
        for (const VideoFacts& facts : {lecture, talk}) {
            const QVector<Choice> list = choices(facts, defaults());
            QVERIFY(!list.isEmpty());
            QVERIFY(list.at(0).selected);
            QVERIFY(list.at(0).automatic);
            for (const Choice& choice : list) {
                if (choice.request != Request::Original)
                    QVERIFY2(choice.plan.estimatedBytes < facts.bytes, qPrintable(choice.label));
            }
        }
        // A picked preset that can't fit, for an original that fits: the original, not a failure.
        Options asked = defaults();
        asked.request = Request::P720;
        plan          = planCompression(talk, asked);
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QVERIFY(!plan.mayGrow);
    }

    void convertingMayGrow()
    {
        // 5 min of 720p VP9 at 50 MB, 100 MB limit: converted to H.264, made to fit at about 92 MB. Core
        // takes the larger result (mayGrow) instead of sending the VP9 after all.
        const Plan plan = planCompression(video(50 * kMB, 5 * 60 * 1000, QSize(1280, 720), 30.0, "vp9", "webm"), defaults());
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.reason, Reason::FitToLimit);
        QVERIFY(plan.mayGrow);
        QVERIFY(plan.estimatedBytes > 50 * kMB);
        QVERIFY(plan.estimatedBytes <= static_cast<quint64>(100 * kMB * kFitTarget));
        // 3 min at 30 MB (over the 25 MB threshold): 2500 kbps, about 60 MB.
        const Plan shrink = planCompression(video(30 * kMB, 3 * 60 * 1000, QSize(1280, 720), 30.0, "vp9", "webm"), defaults());
        QCOMPARE(shrink.reason, Reason::Shrink);
        QVERIFY(shrink.mayGrow);
        QVERIFY(shrink.estimatedBytes > 30 * kMB);
        // H.264 never grows.
        QVERIFY(!planCompression(video(240 * kMB, 62000, QSize(1080, 1920)), defaults()).mayGrow);
    }

    void longVideoFitsTheLimit()
    {
        // 20 min of 1080p, 1.5 GB, 100 MB limit: 720p can't get its floor, 480p at about 508 kbps can.
        const Plan plan = planCompression(video(1536 * kMB, 20 * 60 * 1000, QSize(1920, 1080)), defaults());
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.reason, Reason::FitToLimit);
        QCOMPARE(plan.frameSize, QSize(854, 480));
        QVERIFY(plan.videoKbps >= 500 && plan.videoKbps <= 515);
        QVERIFY(plan.estimatedBytes <= static_cast<quint64>(100 * kMB * kFitTarget));
        QVERIFY(plan.estimatedBytes > 90 * kMB);
    }

    void fitsAtTheSameSizeWhenItCan()
    {
        // 5 min 1080p at 40 Mbit/s, 100 MB limit: 720p at a lower bitrate.
        const Plan plan = planCompression(video(1500 * kMB, 5 * 60 * 1000, QSize(1920, 1080)), defaults());
        QCOMPARE(plan.reason, Reason::FitToLimit);
        QCOMPARE(plan.frameSize, QSize(1280, 720));
        QVERIFY(plan.videoKbps >= 800 && plan.videoKbps < 2500);
        QVERIFY(plan.estimatedBytes <= static_cast<quint64>(100 * kMB * kFitTarget));
    }

    void tooLongToFit()
    {
        const Plan plan = planCompression(video(3072 * kMB, 2 * 3600 * 1000, QSize(1920, 1080)), defaults());
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::CannotFit);
        QVERIFY(failureText(plan, 100).contains(QStringLiteral("100 MB")));
        QVERIFY(failureText(plan, 100).contains(QStringLiteral("too long")));
    }

    void pickedPresetDoesNotStepDown()
    {
        // At 1080p the floor doesn't fit: the picked preset fails rather than becoming 480p.
        Options asked = defaults();
        asked.request = Request::P1080;
        const Plan plan = planCompression(video(1536 * kMB, 20 * 60 * 1000, QSize(1920, 1080)), asked);
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::CannotFit);
    }

    void compressionOff()
    {
        Options off       = defaults();
        off.compressLarge = false;
        // Over the limit: the user is told how to send it.
        Plan plan = planCompression(video(240 * kMB, 62000, QSize(1920, 1080)), off);
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::DisabledOverLimit);
        QVERIFY(failureText(plan, 100).contains(QStringLiteral("Turn on video compression")));
        // Under it: as it is.
        plan = planCompression(video(60 * kMB, 62000, QSize(1920, 1080)), off);
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::Disabled);
        // A format that may not play elsewhere is still converted (its own switch).
        plan = planCompression(video(8 * kMB, 10000, QSize(1920, 1080), 30.0, "hevc", "mov"), off);
        QCOMPARE(plan.decision, Decision::Compress);
        QCOMPARE(plan.reason, Reason::Convert);
        // ... and made to fit when it is over the limit.
        plan = planCompression(video(300 * kMB, 10 * 60 * 1000, QSize(3840, 2160), 30.0, "hevc", "mov"), off);
        QCOMPARE(plan.decision, Decision::Compress);
        QVERIFY(plan.estimatedBytes <= 100 * kMB);
    }

    void convertsWhatMayNotPlay_data()
    {
        QTest::addColumn<QString>("codec");
        QTest::addColumn<QString>("ext");
        QTest::addColumn<bool>("converted");
        QTest::newRow("iPhone HEVC .mov") << QStringLiteral("hevc") << QStringLiteral("mov") << true;
        QTest::newRow("AV1 .mkv") << QStringLiteral("av1") << QStringLiteral("mkv") << true;
        QTest::newRow("VP9 .webm") << QStringLiteral("vp9") << QStringLiteral("webm") << true;
        QTest::newRow("VP8 .webm") << QStringLiteral("vp8") << QStringLiteral("webm") << true;
        QTest::newRow("MPEG-2 .mpg") << QStringLiteral("mpeg2") << QStringLiteral("mpg") << true;
        QTest::newRow("camcorder H.264 .mts") << QStringLiteral("h264") << QStringLiteral("mts") << true;
        QTest::newRow("phone .3gp") << QStringLiteral("h264") << QStringLiteral("3gp") << true;
        QTest::newRow("H.264 .mp4") << QStringLiteral("h264") << QStringLiteral("mp4") << false;
        QTest::newRow("H.264 .mov") << QStringLiteral("h264") << QStringLiteral("mov") << false;
        QTest::newRow("H.264 .mkv") << QStringLiteral("h264") << QStringLiteral("mkv") << false;
    }
    void convertsWhatMayNotPlay()
    {
        QFETCH(QString, codec);
        QFETCH(QString, ext);
        QFETCH(bool, converted);
        const VideoFacts small = video(5 * kMB, 10000, QSize(1920, 1080), 30.0, codec.toLatin1().constData(), ext.toLatin1().constData());
        QCOMPARE(mayNotPlayForOthers(small), converted);
        Plan plan = planCompression(small, defaults());
        QCOMPARE(plan.decision, converted ? Decision::Compress : Decision::SendOriginal);
        if (converted) {
            QCOMPARE(plan.reason, Reason::Convert);
        }
        // The switch off: as it is.
        Options off           = defaults();
        off.convertUnplayable = false;
        plan                  = planCompression(small, off);
        QCOMPARE(plan.decision, Decision::SendOriginal);
    }

    void convertingMaySpendMoreThanTheSource()
    {
        // A low-bitrate VP9 clip: H.264 may use up to twice its rate (but at most the preset's).
        const VideoFacts vp9  = video(3 * kMB, 15000, QSize(1280, 720), 30.0, "vp9", "webm");
        const Plan       plan = planCompression(vp9, defaults());
        QCOMPARE(plan.reason, Reason::Convert);
        QCOMPARE(plan.videoKbps, 2500);
    }

    void onlyWhenThisComputerCanDecode()
    {
        VideoFacts hevc = video(5 * kMB, 10000, QSize(1920, 1080), 30.0, "hevc", "mov");
        hevc.decodable  = false;
        hevc.undecodableText = QStringLiteral("No HEVC (H.265) video decoder is installed on this computer.");
        Plan plan = planCompression(hevc, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::Undecodable);
        hevc.bytes = 300 * kMB;
        plan       = planCompression(hevc, defaults());
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::Undecodable);
        const QString text = failureText(plan, 100);
        QVERIFY(text.startsWith(hevc.undecodableText));
        QVERIFY(text.contains(QStringLiteral("100 MB")));
    }

    void unknownLength()
    {
        VideoFacts f = video(60 * kMB, 0, QSize(1920, 1080));
        Plan       plan = planCompression(f, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QCOMPARE(plan.reason, Reason::UnknownLength);
        f.bytes = 300 * kMB;
        plan    = planCompression(f, defaults());
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::UnknownLength);
        f.probed = false;
        f.durationMs = 60000;
        plan = planCompression(f, defaults());
        QCOMPARE(plan.reason, Reason::UnknownLength);
    }

    void thirtyTwoBitLimit()
    {
        VideoFacts uhd = video(300 * kMB, 60000, QSize(3840, 2160));
        uhd.is32Bit    = true;
        Plan plan      = planCompression(uhd, defaults());
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::TooLarge32Bit);
        QVERIFY(failureText(plan, 100).contains(QStringLiteral("32-bit")));
        uhd.bytes = 60 * kMB;
        plan      = planCompression(uhd, defaults());
        QCOMPARE(plan.decision, Decision::SendOriginal);
        uhd.display = QSize(2560, 1440);
        uhd.bytes   = 300 * kMB;
        plan        = planCompression(uhd, defaults());
        QCOMPARE(plan.decision, Decision::Compress);
    }

    void sound()
    {
        VideoFacts f = video(100 * kMB, 60000, QSize(1280, 720));
        f.hasAudio   = false;
        f.audioChannels = 0;
        Plan plan = planCompression(f, defaults());
        QCOMPARE(plan.audioKbps, 0);
        QCOMPARE(plan.audioChannels, 0);
        f.hasAudio      = true;
        f.audioChannels = 1;
        plan            = planCompression(f, defaults());
        QCOMPARE(plan.audioKbps, 96);
        QCOMPARE(plan.audioChannels, 1);
        f.audioChannels = 6; // 5.1 is mixed down to stereo
        plan            = planCompression(f, defaults());
        QCOMPARE(plan.audioKbps, 128);
        QCOMPARE(plan.audioChannels, 2);
    }

    void withoutMediaFoundation()
    {
        Options none         = defaults();
        none.mediaFoundation = false;
        Plan plan            = planCompression(video(300 * kMB, 60000, QSize(1920, 1080)), none);
        QCOMPARE(plan.decision, Decision::Fail);
        QCOMPARE(plan.reason, Reason::NoMediaFoundation);
        QVERIFY(failureText(plan, 100).contains(QStringLiteral("Media Feature Pack")));
        plan = planCompression(video(60 * kMB, 60000, QSize(1920, 1080)), none);
        QCOMPARE(plan.decision, Decision::SendOriginal);
        QVERIFY(choices(video(60 * kMB, 60000, QSize(1920, 1080)), none).isEmpty());
    }

    void originalAsked()
    {
        Options original = defaults();
        original.request = Request::Original;
        const Plan plan  = planCompression(video(300 * kMB, 60000, QSize(1920, 1080)), original);
        QCOMPARE(plan.decision, Decision::SendOriginal); // Core's limit check then says it's too large
        QCOMPARE(plan.reason, Reason::OriginalAsked);
    }

    void soundOnlyMp4()
    {
        VideoFacts f = video(60 * kMB, 60000, QSize());
        f.hasVideo   = false;
        QCOMPARE(planCompression(f, defaults()).reason, Reason::NotVideo);
        QVERIFY(choices(f, defaults()).isEmpty());
    }

    void compressibleNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("compressible");
        for (const char* yes : {"a.mp4", "a.MOV", "a.mkv", "a.webm", "a.m4v", "a.avi", "a.wmv", "a.3gp", "a.3g2", "a.MTS", "a.m2ts", "a.mpg", "a.mpeg"})
            QTest::newRow(yes) << QString::fromLatin1(yes) << true;
        for (const char* no : {"a.ts", "a.txt", "a.mp3", "a.gif", "a", "a.mp4.exe"})
            QTest::newRow(no) << QString::fromLatin1(no) << false;
    }
    void compressibleNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, compressible);
        QCOMPARE(isCompressibleVideoName(name), compressible);
    }

    void codecIds()
    {
        QCOMPARE(codecId(QStringLiteral("H.264")), QStringLiteral("h264"));
        QCOMPARE(codecId(QStringLiteral("HEVC (H.265)")), QStringLiteral("hevc"));
        QCOMPARE(codecId(QStringLiteral("VP9")), QStringLiteral("vp9"));
        QCOMPARE(codecId(QStringLiteral("AV1")), QStringLiteral("av1"));
        QCOMPARE(codecId(QStringLiteral("MPEG-2")), QStringLiteral("mpeg2"));
        QCOMPARE(codecId(QStringLiteral("Apple ProRes")), QStringLiteral("other"));
        QCOMPARE(codecId(QString()), QString());
    }

    void estimates()
    {
        // (2500 + 128) kbps for 62 s, plus 1% muxing and 64 KB of index.
        QCOMPARE(estimateBytes(2500, 128, 62000), static_cast<quint64>(20570670 + 65536));
        QCOMPARE(estimateBytes(1000, 0, 0), static_cast<quint64>(65536));
    }

    void texts()
    {
        QVERIFY(compressFailedText(QStringLiteral("error 0xC00D36B4"), 100).contains(QStringLiteral("error 0xC00D36B4")));
        QVERIFY(compressFailedText(QStringLiteral("x"), 250).contains(QStringLiteral("250 MB")));
        QVERIFY(compressFailedNote(QStringLiteral("it stopped responding")).contains(QStringLiteral("original was sent")));
        QVERIFY(diskSpaceText(1536 * kMB).contains(QStringLiteral("1.5 GB")));
        QCOMPARE(resolutionLabel(QSize(1280, 720)), QStringLiteral("720p"));
        QCOMPARE(resolutionLabel(QSize(720, 1280)), QStringLiteral("720p"));
        QCOMPARE(resolutionLabel(QSize()), QString());
    }

    // ---- the send window's Quality combo ----------------------------------------------------

    void choicesForALargeVideo()
    {
        const QVector<Choice> list = choices(video(97 * kMB, 20000, QSize(1920, 1080), 60.0), defaults());
        QCOMPARE(list.size(), 4); // Original, 1080p, 720p, 480p
        QCOMPARE(list.at(0).request, Request::Original);
        QVERIFY(list.at(0).enabled); // 97 MB fits the 100 MB limit
        QCOMPARE(list.at(1).plan.frameSize, QSize(1920, 1080));
        QCOMPARE(list.at(2).plan.frameSize, QSize(1280, 720));
        QCOMPARE(list.at(3).plan.frameSize, QSize(854, 480));
        // The default is what Auto does (720p), and sending it asks Core for Auto.
        int selected = -1;
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).selected) {
                QCOMPARE(selected, -1);
                selected = i;
            }
        }
        QCOMPARE(selected, 2);
        QCOMPARE(requestFor(list, 2), Request::Auto);
        QCOMPARE(requestFor(list, 0), Request::Original);
        QCOMPARE(requestFor(list, 1), Request::P1080);
        QCOMPARE(requestFor(list, 3), Request::P480);
        QVERIFY(list.at(2).label.contains(QStringLiteral("720p")));
        QVERIFY(list.at(2).label.contains(QStringLiteral("about")));
        QVERIFY(list.at(2).longLabel.contains(QStringLiteral("Balanced")));
        // Larger entries first.
        QVERIFY(list.at(1).plan.estimatedBytes > list.at(2).plan.estimatedBytes);
        QVERIFY(list.at(2).plan.estimatedBytes > list.at(3).plan.estimatedBytes);
    }

    void choicesOverTheLimit()
    {
        // Original over the limit: listed, but it can't be picked; the default is a size that fits.
        const QVector<Choice> list = choices(video(1536 * kMB, 20 * 60 * 1000, QSize(1920, 1080)), defaults());
        QVERIFY(!list.isEmpty());
        QVERIFY(!list.at(0).enabled);
        QVERIFY(list.at(0).label.contains(QStringLiteral("over")));
        int selected = -1;
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).selected)
                selected = i;
            if (list.at(i).request != Request::Original)
                QVERIFY(list.at(i).plan.estimatedBytes <= 100 * kMB);
        }
        QVERIFY(selected > 0);
        QCOMPARE(list.at(selected).plan.frameSize, QSize(854, 480));
    }

    void choicesWhenNothingFits()
    {
        const QVector<Choice> list = choices(video(3072 * kMB, 2 * 3600 * 1000, QSize(1920, 1080)), defaults());
        QCOMPARE(list.size(), 1); // Original only, disabled
        QVERIFY(!list.at(0).enabled);
        QVERIFY(!list.at(0).selected);
    }

    void choicesForASmallSource()
    {
        // 720p source: "High" would give the same picture as "Balanced", so it isn't listed twice.
        const QVector<Choice> list = choices(video(60 * kMB, 60000, QSize(1280, 720)), defaults());
        QSet<QString> sizes;
        for (const Choice& choice : list) {
            if (choice.request == Request::Original)
                continue;
            const QString key = QStringLiteral("%1x%2").arg(choice.plan.frameSize.width()).arg(choice.plan.frameSize.height());
            QVERIFY2(!sizes.contains(key), qPrintable(key));
            sizes.insert(key);
        }
        QVERIFY(sizes.contains(QStringLiteral("1280x720")));
        QVERIFY(sizes.contains(QStringLiteral("854x480")));
    }

    void choicesWhenCompressionIsOff()
    {
        // Off and over the limit: Auto would fail, so the largest entry that fits is the default, and
        // sending it asks for that preset explicitly.
        Options off       = defaults();
        off.compressLarge = false;
        const QVector<Choice> list = choices(video(240 * kMB, 62000, QSize(1920, 1080)), off);
        int selected = -1;
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).selected)
                selected = i;
        }
        QVERIFY(selected > 0);
        QVERIFY(!list.at(selected).automatic);
        QCOMPARE(requestFor(list, selected), list.at(selected).request);
        QVERIFY(list.at(selected).request != Request::Auto);
    }

    void smallVideoDefaultsToOriginal()
    {
        const QVector<Choice> list = choices(video(10 * kMB, 30000, QSize(1920, 1080)), defaults());
        QVERIFY(!list.isEmpty());
        QVERIFY(list.at(0).selected);
        QCOMPARE(requestFor(list, 0), Request::Auto);
    }

    void noChoicesWithoutFacts()
    {
        VideoFacts f = video(60 * kMB, 60000, QSize(1920, 1080));
        f.decodable  = false;
        QVERIFY(choices(f, defaults()).isEmpty());
        f.decodable  = true;
        f.durationMs = 0;
        QVERIFY(choices(f, defaults()).isEmpty());
    }

    // ---- settings ---------------------------------------------------------------------------

    void settingsClamping()
    {
        QCOMPARE(Settings::normalizeVideoQuality(480), 480);
        QCOMPARE(Settings::normalizeVideoQuality(1080), 1080);
        QCOMPARE(Settings::normalizeVideoQuality(999), 720);
        QCOMPARE(Settings::normalizeVideoQuality(0), 720);

        // A settings.ini with garbage, in a folder of its own (Settings::load(file), as the other 2.2 tests).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("settings.ini"));
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("compressVideoQuality"), 999);
            ini.setValue(QStringLiteral("compressVideosOverMB"), 0);
            ini.setValue(QStringLiteral("compressVideos"), QStringLiteral("false"));
            ini.sync();
        }
        Settings s;
        s.load(path);
        QCOMPARE(s.compressVideoQuality, 720);
        QCOMPARE(s.compressVideosOverMB, 1);
        QCOMPARE(s.compressVideos, false);
        QCOMPARE(s.convertUnplayableVideos, true); // missing: the default
        QCOMPARE(s.compressUseGpu, true);
        {
            QSettings ini(path, QSettings::IniFormat);
            ini.setValue(QStringLiteral("compressVideosOverMB"), QStringLiteral("lots"));
            ini.sync();
        }
        s.load(path);
        QCOMPARE(s.compressVideosOverMB, 25);
        const Settings d;
        QVERIFY(d.compressVideos);
        QCOMPARE(d.compressVideosOverMB, 25);
        QCOMPARE(d.compressVideoQuality, 720);
        QVERIFY(d.convertUnplayableVideos);
    }
};

TSMEDIA_REGISTER_TEST(TestVideoCompress)
#include "tst_videocompress.moc"
