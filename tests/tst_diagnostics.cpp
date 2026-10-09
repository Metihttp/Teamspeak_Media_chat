// Unit tests for "Copy diagnostic info" (src/diagnostics.*): the redaction of names and paths, the
// legacy log wording, the log parsers, the Windows name, the report text and the bug report link.
// Runs inside tsmedia_tests (tests/testmain.h).

#include <QDir>
#include <QUrlQuery>
#include <QtTest>

#include "diagnostics.h"
#include "medialink.h"
#include "testmain.h"

// settings.cpp keeps its ini file in the plugin's data folder (ts3::dataDir() is stubbed in
// tst_tsmedia.cpp); these tests never load or save it.

namespace {

QString marked(char kind, const char* value)
{
    return diag::mark(kind, QString::fromUtf8(value));
}

// What a legacy (unmarked) line looks like in the report with names hidden.
QString redactLegacy(const QString& line)
{
    diag::Redactor redactor(false, QString::fromLatin1("C:/Users/Bob"));
    return redactor.apply(diag::markSafetyNet(diag::markLegacy(line)));
}

diag::Facts sampleFacts()
{
    diag::Facts f;
    f.created                 = QDateTime(QDate(2026, 11, 2), QTime(17, 5), Qt::OffsetFromUTC, 3 * 3600 + 30 * 60);
    f.pluginName              = QString::fromLatin1("TS Media chat");
    f.pluginVersion           = QString::fromLatin1("2.2.0");
    f.pluginBits              = 64;
    f.qtRuntime               = QString::fromLatin1("5.15.2");
    f.qtBuilt                 = QString::fromLatin1("5.15.2");
    f.pluginApi               = 26;
    f.teamSpeakVersion        = QString::fromLatin1("3.6.2 [Build: 1695203293]");
    f.configFolder            = 1;
    f.windowsProduct          = QString::fromLatin1("Windows 10 Pro");
    f.windowsDisplayVersion   = QString::fromLatin1("25H2");
    f.windowsEdition          = QString::fromLatin1("Professional");
    f.windowsBuild            = 26200;
    f.windowsUbr              = 9457;
    f.nativeArch              = QString::fromLatin1("x64");
    f.mediaFoundationPresent  = true;
    f.mediaFoundationStarted  = true;
    f.codecs.checked          = true;
    f.codecs.videoDecoders    = {{QString::fromLatin1("H.264"), true, true}, {QString::fromLatin1("HEVC"), false, false}};
    f.codecs.audioDecoders    = {{QString::fromLatin1("AAC"), true, false}};
    f.codecs.encoders         = {{QString::fromLatin1("H.264"), true, true}, {QString::fromLatin1("AAC"), true, false}};
    f.codecs.elapsedMs        = 84;
    f.videoDevice             = 1;
    f.scalePercent            = 150;
    f.devicePixelRatio        = 1.5;
    f.screens                 = 2;
    f.animations              = true;
    f.theme                   = 2;
    f.connections             = 1;
    f.serverVersions          = QStringList{QString::fromLatin1("3.13.7 [Build: 1655727713] on Linux")};
    f.chatViews               = 3;
    f.chatInputs              = 2;
    f.session.sessionMs       = (2 * 60 + 14) * 60 * 1000;
    f.session.downloadsOk     = 41;
    f.session.downloadsFailed = 2;
    f.session.downloadErrors.insert(static_cast<int>(MediaError::NotFound), 1);
    f.session.downloadErrors.insert(static_cast<int>(MediaError::Permission), 1);
    f.session.uploadsOk       = 6;
    f.session.uploadsFailed   = 1;
    f.cacheKnown              = true;
    f.cacheBytes              = 412ull * 1024 * 1024;
    f.cacheFiles              = 238;
    f.log.source              = diag::LogTail::Source::PluginLog;
    f.log.marked              = true;
    f.log.total               = 212;
    f.log.lines = {
        {QString::fromLatin1("11-02 17:02:11"), QString::fromLatin1("INFO"), QString::fromLatin1("TS Media chat 2.2.0 loaded")},
        {QString::fromLatin1("11-02 17:03:40"), QString::fromLatin1("WARN"),
         QString::fromLatin1("Download of ") + marked('f', "/tsmedia/holiday_3f9a1c2e.jpg") + QString::fromLatin1(" failed: You don't have permission.")},
        {QString::fromLatin1("11-02 17:04:02"), QString::fromLatin1("INFO"),
         QString::fromLatin1("Uploaded ") + marked('f', "/tsmedia/clip_0011aa22.mp4") + QString::fromLatin1(" (4.5 MB)")},
        {QString::fromLatin1("11-02 17:04:05"), QString::fromLatin1("INFO"),
         QString::fromLatin1("Seeded ") + marked('l', "C:/Users/Bob/Videos/clip.mp4") + QString::fromLatin1(" as ") + marked('f', "/tsmedia/clip_0011aa22.mp4")},
    };
    return f;
}

} // namespace

class TestDiagnostics : public QObject
{
    Q_OBJECT

  private slots:
    void markStripsStrayMarkers();
    void redactNumbersByFirstAppearance();
    void redactIncludeNames();
    void redactFailsClosed();
    void legacyLines_data();
    void legacyLines();
    void safetyNetOnMarkedLines();
    void pluginLogLine();
    void teamSpeakLog();
    void windowsVersion();
    void cleanValues();
    void reportText();
    void reportLimits();
    void reportNeverShowsServerOrPeople();
    void bugReportLink();
};

void TestDiagnostics::markStripsStrayMarkers()
{
    const QString value = QString::fromLatin1("a") + QChar(diag::kMarkBegin) + QString::fromLatin1("b") + QChar(diag::kMarkEnd) + QString::fromLatin1("c");
    const QString m     = diag::mark('f', value);
    QCOMPARE(m, QString(QChar(diag::kMarkBegin)) + QString::fromLatin1("fabc") + QChar(diag::kMarkEnd));
    diag::Redactor redactor(true, QString());
    QCOMPARE(redactor.apply(m), QString::fromLatin1("abc"));
}

void TestDiagnostics::redactNumbersByFirstAppearance()
{
    diag::Redactor redactor(false, QString::fromLatin1("C:/Users/Bob"));
    QCOMPARE(redactor.apply(QString::fromLatin1("Got ") + marked('f', "a.jpg")), QString::fromLatin1("Got <file 1>"));
    QCOMPARE(redactor.apply(marked('f', "b.jpg") + QString::fromLatin1(" and ") + marked('f', "a.jpg")), QString::fromLatin1("<file 2> and <file 1>"));
    QCOMPARE(redactor.apply(marked('l', "C:/Users/Bob/a.jpg")), QString::fromLatin1("<path 1>"));
    QCOMPARE(redactor.apply(marked('n', "Lobby")), QString::fromLatin1("<name 1>"));
    QCOMPARE(redactor.apply(marked('x', "unknown kind")), QString::fromLatin1("<name 2>"));
    // The same text of another kind is another value.
    QCOMPARE(redactor.apply(marked('l', "a.jpg")), QString::fromLatin1("<path 2>"));
}

void TestDiagnostics::redactIncludeNames()
{
    diag::Redactor redactor(true, QString::fromLatin1("C:/Users/Bob"));
    QCOMPARE(redactor.apply(marked('f', "/tsmedia/a b.jpg")), QString::fromLatin1("/tsmedia/a b.jpg"));
    QCOMPARE(redactor.apply(marked('l', "c:/users/BOB/Pictures/a.png")), QString::fromLatin1("%USERPROFILE%/Pictures/a.png"));
    QCOMPARE(redactor.apply(marked('l', "C:\\Users\\Bob\\Pictures\\a.png")), QString::fromLatin1("%USERPROFILE%\\Pictures\\a.png"));
    // Other names are never shown.
    QCOMPARE(redactor.apply(marked('n', "Mehdi")), QString::fromLatin1("<name 1>"));
    // A name can't start a new report line or reorder it.
    QCOMPARE(redactor.apply(marked('f', "a\nb\xE2\x80\xAE" "c.jpg")), QString::fromLatin1("a bc.jpg"));
}

void TestDiagnostics::redactFailsClosed()
{
    diag::Redactor redactor(true, QString());
    // An unbalanced marker hides the rest of the line, names included or not.
    const QString open = QString::fromLatin1("Upload of ") + QChar(diag::kMarkBegin) + QString::fromLatin1("fsecret.jpg failed");
    QCOMPARE(redactor.apply(open), QString::fromLatin1("Upload of <file 1>"));
    // A stray end marker is dropped.
    QCOMPARE(redactor.apply(QString::fromLatin1("x") + QChar(diag::kMarkEnd) + QString::fromLatin1("y")), QString::fromLatin1("xy"));
    // A marker without a kind letter counts as a name.
    const QString noKind = QString(QChar(diag::kMarkBegin)) + QString::fromLatin1(" x") + QChar(diag::kMarkEnd);
    QCOMPARE(redactor.apply(noKind), QString::fromLatin1("<name 1>"));
}

void TestDiagnostics::legacyLines_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("expected");

    const auto row = [](const char* name, const char* line, const char* expected) {
        QTest::newRow(name) << QString::fromUtf8(line) << QString::fromUtf8(expected);
    };
    // The wording of the 2.1 log calls (core.cpp, inlinemedia.cpp, plugin.cpp, printWarning).
    row("download failed", "Download of /tsmedia/holiday_3f9a1c2e.jpg failed: You don't have permission to download files in this channel.",
        "Download of <file 1> failed: You don't have permission to download files in this channel.");
    row("downloaded, parentheses in the name", "Downloaded /tsmedia/a (1)_3f9a1c2e.jpg (4.5 MB)", "Downloaded <file 1> (4.5 MB)");
    row("uploaded", "Uploaded /tsmedia/x y_1234abcd.png (120 KB)", "Uploaded <file 1> (120 KB)");
    row("preview not loaded", "Preview /tsmedia/previews/clip_1.jpg of /tsmedia/clip_1.mp4 not loaded: Not found",
        "Preview <file 1> of <file 2> not loaded: Not found");
    row("probed", "Probed holiday_3f9a.jpg: 1920x1080, 0 ms, blurhash yes, preview 24.1 KB",
        "Probed <file 1>: 1920x1080, 0 ms, blurhash yes, preview 24.1 KB");
    row("automatic download stopped", "Automatic download of /tsmedia/big.mp4 stopped: the file is larger than the auto-download limit",
        "Automatic download of <file 1> stopped: the file is larger than the auto-download limit");
    row("damaged cache", "Discarding damaged cached copy of /tsmedia/a.png; it is downloaded again",
        "Discarding damaged cached copy of <file 1>; it is downloaded again");
    row("still writing", "holiday.jpg was still open for writing 30 s after its transfer completed",
        "<file 1> was still open for writing 30 s after its transfer completed");
    row("waited", "Waited 400 ms for TeamSpeak to finish writing holiday.jpg", "Waited 400 ms for TeamSpeak to finish writing <file 1>");
    row("resuming", "Resuming the download of /tsmedia/a.mp4", "Resuming the download of <file 1>");
    row("preview staging", "Could not stage the preview of a_1.mp4; sending without it", "Could not stage the preview of <file 1>; sending without it");
    row("preview upload", "Preview upload for a_1.mp4 failed (quota); sending without it", "Preview upload for <file 1> failed (quota); sending without it");
    row("renamed", "The name of /tsmedia/a.jpg (or of its preview) is taken on the server; uploading as a_2.jpg",
        "The name of <file 1> (or of its preview) is taken on the server; uploading as <file 2>");
    row("remote delete", "Could not remove /tsmedia/a.jpg from the file browser: not connected",
        "Could not remove <file 1> from the file browser: not connected");
    row("inline video", "Inline video 0123abcd cannot be played: Couldn't decode this file. It may be damaged.",
        "Inline video 0123abcd cannot be played: Couldn't decode this file. It may be damaged.");
    row("not animating", "Not animating cat.gif: 500x500, 900 frames is too large", "Not animating <file 1>: 500x500, 900 frames is too large");
    row("missing local file", "Couldn't find C:\\Users\\Bob\\Pictures\\a.png. It may have been moved or deleted.",
        "Couldn't find <path 1>. It may have been moved or deleted.");
    row("missing local files", "Couldn't find 2 files: C:\\a.png, D:\\b.png. They may have been moved or deleted.",
        "Couldn't find 2 files: <path 1>. They may have been moved or deleted.");
    row("send failed", "Couldn't send “holiday.jpg”: The server's file storage is full.", "Couldn't send “<file 1>”: The server's file storage is full.");
    row("unknown command", "Unknown command “foo”.", "Unknown command “<name 1>”.");
    row("quoted name in kept text", "Download of /a.jpg failed: “Lobby” is locked", "Download of <file 1> failed: “<name 1>” is locked");
    row("path in kept text", "Download of /a.jpg failed: no /tsmedia/b c.jpg here", "Download of <file 1> failed: no <file 2>");
    row("file name in kept text", "Download of /a.jpg failed: bad name x.png, retry", "Download of <file 1> failed: bad name <file 2>, retry");
    // Lines without names stay as they are.
    row("loaded", "TS Media chat 2.2.0 loaded", "TS Media chat 2.2.0 loaded");
    row("cache limit", "Media cache above its 1.0 GB limit: 1.2 GB -> 1.0 GB", "Media cache above its 1.0 GB limit: 1.2 GB -> 1.0 GB");
    row("no media foundation", "Media Foundation is not available: videos can't be played inside the chat",
        "Media Foundation is not available: videos can't be played inside the chat");
    // Unknown wording: fail closed.
    row("unknown, local paths", "[test] self-test upload of C:\\x\\a.mp4, C:\\y.mp4", "[test] self-test upload of <path 1>");
    row("unknown, remote path", "Something about /tsmedia/new file.jpg here", "Something about <file 1>");
    row("unknown, file name", "Saw holiday.jpg twice", "Saw <file 1> twice");
    row("unknown, address", "Connected to 192.168.1.20:9987 fine", "Connected to <name 1> fine");
    row("unknown, IPv6", "Host ::1 answered", "Host <name 1> answered");
    row("unknown, curly quotes", "Sent to the channel “Lobby”", "Sent to the channel “<name 1>”");
    row("unknown, unbalanced quote", "Sent to “Lobby", "Sent to “<name 1>");
    row("unknown, versions stay", "Version 2.2.0 and 4.5 MB, 1/2 done", "Version 2.2.0 and 4.5 MB, 1/2 done");
    row("unknown, double colon in code", "mf::startup failed", "mf::startup failed");
}

void TestDiagnostics::legacyLines()
{
    QFETCH(QString, line);
    QFETCH(QString, expected);
    QCOMPARE(redactLegacy(line), expected);
}

void TestDiagnostics::safetyNetOnMarkedLines()
{
    // A structured line trusts its markers, but a path or an address passed as public is still caught.
    diag::Redactor redactor(false, QString());
    const QString  line = QString::fromLatin1("Uploaded ") + marked('f', "/x.jpg") + QString::fromLatin1(" from C:\\Users\\Bob\\x.jpg");
    QCOMPARE(redactor.apply(diag::markSafetyNet(line)), QString::fromLatin1("Uploaded <file 1> from <path 1>"));
    QCOMPARE(redactor.apply(diag::markSafetyNet(QString::fromLatin1("peer 10.0.0.7 said hi"))), QString::fromLatin1("peer <name 1> said hi"));
    // A file name in public text of a marked source is trusted (only the plugin's own log is marked).
    QCOMPARE(redactor.apply(diag::markSafetyNet(QString::fromLatin1("Loaded mfplat.dll"))), QString::fromLatin1("Loaded mfplat.dll"));
}

void TestDiagnostics::pluginLogLine()
{
    const QString raw = QString::fromLatin1("2026-11-02T17:03:40.123+03:30 WARN  Download of ") + marked('f', "/tsmedia/a.jpg") + QString::fromLatin1(" failed");
    const diag::LogLine line = diag::parsePluginLogLine(raw);
    QCOMPARE(line.time, QString::fromLatin1("11-02 17:03:40"));
    QCOMPARE(line.level, QString::fromLatin1("WARN"));
    QCOMPARE(line.text, QString::fromLatin1("Download of ") + marked('f', "/tsmedia/a.jpg") + QString::fromLatin1(" failed"));

    const diag::LogLine error = diag::parsePluginLogLine(QString::fromLatin1("2026-11-02T17:03:41.000Z ERROR x"));
    QCOMPARE(error.level, QString::fromLatin1("ERROR"));
    QCOMPARE(error.text, QString::fromLatin1("x"));

    const diag::LogLine garbage = diag::parsePluginLogLine(QString::fromLatin1("not a log line"));
    QVERIFY(garbage.time.isEmpty());
    QCOMPARE(garbage.text, QString::fromLatin1("not a log line"));

    // The provider's adapter: marked lines from the plugin log, or nothing (then TeamSpeak's log).
    const diag::LogTail tail = diag::pluginLogTail({raw, QString(), QString::fromLatin1("2026-11-02T17:03:42.000Z INFO  y")}, 212);
    QCOMPARE(tail.source, diag::LogTail::Source::PluginLog);
    QVERIFY(tail.marked);
    QCOMPARE(tail.total, 212);
    QCOMPARE(tail.lines.size(), 2);
    QCOMPARE(diag::pluginLogTail({}, 0).source, diag::LogTail::Source::None);

    // A legacy line (one unclassified span) gets the legacy wording rules instead of being hidden whole.
    const QString legacy = QString::fromLatin1("2026-11-02T17:03:43.000Z INFO  ") + marked('u', "Could not stage the preview of C:/Users/Bob/x.png; sending without it");
    const diag::LogTail unwrapped = diag::pluginLogTail({legacy}, 1);
    QCOMPARE(unwrapped.lines.size(), 1);
    const QString shown = diag::Redactor(false, QString::fromLatin1("C:/Users/Bob")).apply(diag::markSafetyNet(unwrapped.lines.first().text));
    QVERIFY2(shown.contains(QString::fromLatin1("sending without it")), qPrintable(shown));
    QVERIFY2(!shown.contains(QString::fromLatin1("Bob")) && !shown.contains(QString::fromLatin1("x.png")), qPrintable(shown));
}

void TestDiagnostics::teamSpeakLog()
{
    const QByteArray log = "2026-10-09 19:22:30.100000|INFO    |ClientUI      |   |Connect to server: SecretServer 127.0.0.1\r\n"
                           "2026-10-09 19:22:31.200000|INFO    |TSMedia       |   |TS Media chat 2.2.0 loaded\r\n"
                           "2026-10-09 19:22:32.300000|INFO    |ClientUI      |1  |Nickname Mehdi joined\r\n"
                           "2026-10-09 19:22:33.400000|WARNING |TSMedia       |1  |Download of /tsmedia/a.jpg failed: x | y\r\n"
                           "2026-10-09 19:22:34.500000|DEBUG   |TSMedia       |   |Probed a.jpg: 1x1, 0 ms, blurhash no, preview none\r\n"
                           "garbage line without columns\r\n"
                           "2026-10-09 19:22:35.600000|INFO    |Plugins       |   |TSMedia: not our channel\r\n";
    diag::LogTail tail = diag::parseTeamSpeakLog(log, 40);
    QCOMPARE(tail.source, diag::LogTail::Source::TeamSpeakLog);
    QVERIFY(!tail.marked);
    QCOMPARE(tail.total, 3);
    QCOMPARE(tail.lines.size(), 3);
    QCOMPARE(tail.lines.at(0).time, QString::fromLatin1("10-09 19:22:31"));
    QCOMPARE(tail.lines.at(0).level, QString::fromLatin1("INFO"));
    QCOMPARE(tail.lines.at(0).text, QString::fromLatin1("TS Media chat 2.2.0 loaded"));
    QCOMPARE(tail.lines.at(1).level, QString::fromLatin1("WARN"));
    QCOMPARE(tail.lines.at(1).text, QString::fromLatin1("Download of /tsmedia/a.jpg failed: x | y")); // a '|' in the message stays
    for (const diag::LogLine& line : qAsConst(tail.lines)) {
        QVERIFY(!line.text.contains(QLatin1String("SecretServer")));
        QVERIFY(!line.text.contains(QLatin1String("Mehdi")));
    }

    tail = diag::parseTeamSpeakLog(log, 2);
    QCOMPARE(tail.total, 3);
    QCOMPARE(tail.lines.size(), 2);
    QCOMPARE(tail.lines.at(1).level, QString::fromLatin1("DEBUG")); // the newest ones

    QCOMPARE(diag::parseTeamSpeakLog(QByteArray(), 40).total, 0);
}

void TestDiagnostics::windowsVersion()
{
    QCOMPARE(diag::windowsName(QString::fromLatin1("Windows 10 Pro"), 26200, QString::fromLatin1("25H2")), QString::fromLatin1("Windows 11 Pro 25H2"));
    QCOMPARE(diag::windowsName(QString::fromLatin1("Windows 10 Pro"), 19045, QString::fromLatin1("22H2")), QString::fromLatin1("Windows 10 Pro 22H2"));
    QCOMPARE(diag::windowsName(QString::fromLatin1("Windows 10 Pro N"), 22631, QString()), QString::fromLatin1("Windows 11 Pro N"));
    QCOMPARE(diag::windowsName(QString(), 0, QString()), QString::fromLatin1("Windows"));
    QVERIFY(diag::isNEdition(QString::fromLatin1("ProfessionalN")));
    QVERIFY(diag::isNEdition(QString::fromLatin1("EnterpriseSN")));
    QVERIFY(!diag::isNEdition(QString::fromLatin1("EnterpriseS")));
    QVERIFY(!diag::isNEdition(QString::fromLatin1("Professional")));
    QVERIFY(!diag::isNEdition(QString::fromLatin1("N")));
}

void TestDiagnostics::cleanValues()
{
    QCOMPARE(diag::cleanValue(QString::fromUtf8("3.6.2\n[Build: 1]\xE2\x80\xAE"), 60), QString::fromLatin1("3.6.2 [Build: 1]"));
    QCOMPARE(diag::cleanValue(QString(100, QLatin1Char('a')), 10), QString::fromLatin1("aaaaaaa..."));
}

void TestDiagnostics::reportText()
{
    const QString expected = QString::fromUtf8(
        "TS Media chat diagnostic info\n"
        "Created: 2026-11-02 17:05 (UTC+03:30)\n"
        "Privacy: file names hidden. No server addresses, server or channel names, unique IDs or nicknames.\n"
        "\n"
        "Versions\n"
        "  Plugin: TS Media chat 2.2.0, 64-bit, Qt 5.15.2 (built with 5.15.2), plugin API 26\n"
        "  TeamSpeak: 3.6.2 [Build: 1695203293], 64-bit, config folder: standard\n"
        "  Windows: Windows 11 Pro 25H2, build 26200.9457, x64, N edition: no\n"
        "Media\n"
        "  Media Foundation: available\n"
        "  Video decoders: H.264 yes, HEVC no\n"
        "  Audio decoders: AAC yes\n"
        "  Encoders: H.264 yes (hardware), AAC yes [checked in 84 ms]\n"
        "  Video output: Direct3D 11 hardware\n"
        "  Display: 150% scaling (device pixel ratio 1.5), 2 screens, animations on, TeamSpeak theme dark\n"
        "Connections\n"
        "  Connected servers: 1 (3.13.7 [Build: 1655727713] on Linux)\n"
        "  Chat hooks: 3 chat views, 2 input lines\n"
        "Settings\n"
        "  Show media in chat: on. Images and GIFs: automatic up to 15 MB. Videos: when played. Autoplay GIFs: on. Preview size: 400 × 300\n"
        "  Volume 80%, start muted off, loop off\n"
        "  Send by drop on, by paste on, PNG to JPEG on, previews on\n"
        "  Note on, link default. Upload limit 100 MB, folder default. Cache limit 1024 MB\n"
        "  Data saver off, 0 servers with their own settings\n"
        "Activity this session (2 h 14 min)\n"
        "  Downloads: 41 finished, 2 failed (no permission 1, not found 1)\n"
        "  Uploads: 6 finished, 1 failed, 0 canceled\n"
        "  Cache: 412 MB of 1024 MB, 238 files\n"
        "Recent log (last 4 of 212 lines, from the plugin log)\n"
        "  11-02 17:02:11 INFO  TS Media chat 2.2.0 loaded\n"
        "  11-02 17:03:40 WARN  Download of <file 1> failed: You don't have permission.\n"
        "  11-02 17:04:02 INFO  Uploaded <file 2> (4.5 MB)\n"
        "  11-02 17:04:05 INFO  Seeded <path 1> as <file 2>");
    const QString report = diag::format(sampleFacts(), false, QString::fromLatin1("C:/Users/Bob"));
    if (report != expected)
        qWarning().noquote() << "\n" << report;
    QCOMPARE(report, expected);

    // With names: file names and paths, the user folder as %USERPROFILE%.
    const QString withNames = diag::format(sampleFacts(), true, QString::fromLatin1("C:/Users/Bob"));
    QVERIFY(withNames.contains(QLatin1String("Privacy: file names included")));
    QVERIFY(withNames.contains(QLatin1String("Download of /tsmedia/holiday_3f9a1c2e.jpg failed")));
    QVERIFY(withNames.contains(QLatin1String("Seeded %USERPROFILE%/Videos/clip.mp4 as /tsmedia/clip_0011aa22.mp4")));
    QVERIFY(!withNames.contains(QLatin1String("Bob")));
}

void TestDiagnostics::reportLimits()
{
    diag::Facts f = sampleFacts();
    f.log.lines.clear();
    for (int i = 0; i < 41; ++i)
        f.log.lines.append({QString::fromLatin1("11-02 17:00:00"), QString::fromLatin1("INFO"), QString::fromLatin1("line %1 ").arg(i)});
    f.log.lines[40].text += QString(1000, QLatin1Char('x')); // cut to 300 characters
    f.log.total = 41;
    const QString     report = diag::format(f, false, QString());
    const QStringList lines  = report.split(QLatin1Char('\n'));
    QVERIFY(report.size() <= diag::kMaxReportLength);
    int logLines = 0;
    for (const QString& line : lines) {
        QVERIFY2(line.size() <= diag::kMaxLineLength, qPrintable(line.left(60)));
        if (line.contains(QLatin1String(" INFO  line ")))
            ++logLines;
    }
    QCOMPARE(logLines, diag::kMaxLogLines);
    QVERIFY(!report.contains(QLatin1String("line 0 "))); // the oldest one is left out
    QVERIFY(report.contains(QLatin1String("line 40 ")));
    QVERIFY(report.contains(QLatin1String("Recent log (last 40 of 41 lines")));

    // Far more text than fits: the newest lines that fit, and the header says how many.
    f.log.lines.clear();
    for (int i = 0; i < 40; ++i)
        f.log.lines.append({QString(), QString::fromLatin1("INFO"), QString(400, QLatin1Char('y'))});
    for (int i = 0; i < 30; ++i)
        f.extra.append({QString::fromLatin1("Extra %1").arg(i), {QString(250, QLatin1Char('z'))}});
    const QString full = diag::format(f, false, QString());
    QVERIFY(full.size() <= diag::kMaxReportLength);
    QVERIFY(full.contains(QLatin1String("Recent log (last ")));

    // No log at all.
    f       = sampleFacts();
    f.log   = diag::LogTail();
    f.log.problem = QString::fromLatin1("no TeamSpeak log found");
    QVERIFY(diag::format(f, false, QString()).endsWith(QLatin1String("Recent log: not available (no TeamSpeak log found)")));
}

void TestDiagnostics::reportNeverShowsServerOrPeople()
{
    diag::Facts f                = sampleFacts();
    f.settings.uploadDirectory   = QString::fromLatin1("/SecretServer uploads");
    f.settings.pluginDownloadUrl = QString::fromLatin1("https://example.com/Mehdi");
    f.serverVersions             = QStringList{QString::fromUtf8("3.13.7\n  Plugin: fake line")};
    f.log.marked                 = false;
    f.log.source                 = diag::LogTail::Source::TeamSpeakLog;
    f.log.lines = {
        {QString(), QString::fromLatin1("WARN"), QString::fromUtf8("Couldn't send “Mehdi's photo.jpg”: denied")},
        {QString(), QString::fromLatin1("INFO"), QString::fromUtf8("Sent to the channel “SecretServer”")},
        {QString(), QString::fromLatin1("INFO"), QString::fromLatin1("uid abc+def/ghi= at 127.0.0.1:9987")},
    };
    for (const bool includeNames : {false, true}) {
        const QString report = diag::format(f, includeNames, QString());
        QVERIFY2(!report.contains(QLatin1String("SecretServer")), qPrintable(report));
        QVERIFY2(!report.contains(QLatin1String("abc+def/ghi=")), qPrintable(report));
        QVERIFY2(!report.contains(QLatin1String("127.0.0.1")), qPrintable(report));
        QVERIFY2(!report.contains(QLatin1String("example.com")), qPrintable(report));
        QVERIFY2(!report.contains(QLatin1String("\n  Plugin: fake line")), qPrintable(report));
        if (!includeNames)
            QVERIFY2(!report.contains(QLatin1String("Mehdi")), qPrintable(report));
        QVERIFY(report.contains(QLatin1String("folder changed")));
        QVERIFY(report.contains(QLatin1String("link changed")));
    }
}

void TestDiagnostics::bugReportLink()
{
    const diag::Facts f   = sampleFacts();
    const QUrl        url = diag::bugReportUrl(f);
    QVERIFY(url.isValid());
    QCOMPARE(url.scheme(), QString::fromLatin1("https"));
    QCOMPARE(url.host(), QString::fromLatin1("github.com"));
    QCOMPARE(url.path(), QString::fromLatin1("/Metihttp/Teamspeak_Media_chat/issues/new"));
    const QByteArray encoded = url.toEncoded();
    QVERIFY(encoded.size() < 2000);
    QVERIFY(!encoded.contains(' '));
    QVERIFY(!encoded.contains('+'));

    const QUrlQuery   query(url);
    QStringList       keys;
    for (const auto& item : query.queryItems(QUrl::FullyDecoded))
        keys << item.first;
    QCOMPARE(keys, (QStringList{QString::fromLatin1("template"), QString::fromLatin1("plugin-version"), QString::fromLatin1("teamspeak-version"),
                                QString::fromLatin1("client-arch"), QString::fromLatin1("windows-version"), QString::fromLatin1("log-line")}));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("template"), QUrl::FullyDecoded), QString::fromLatin1("bug_report.yml"));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("plugin-version"), QUrl::FullyDecoded), QString::fromLatin1("TS Media chat 2.2.0"));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("teamspeak-version"), QUrl::FullyDecoded), QString::fromLatin1("3.6.2 [Build: 1695203293]"));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("client-arch"), QUrl::FullyDecoded), QString::fromLatin1("64-bit"));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("windows-version"), QUrl::FullyDecoded), QString::fromLatin1("Windows 11 Pro 25H2 (build 26200.9457)"));
    QCOMPARE(query.queryItemValue(QString::fromLatin1("log-line"), QUrl::FullyDecoded), QString::fromLatin1("TS Media chat 2.2.0 loaded"));
    QVERIFY(!encoded.contains("holiday")); // no log text
}

TSMEDIA_REGISTER_TEST(TestDiagnostics)

#include "tst_diagnostics.moc"
