// Unit tests for the 2.2 drag-out and per-server settings logic: local / export file names
// (filenames.cpp), the [server_<id>] groups of settings.ini and the data saver rules
// (serversettings.cpp, Settings::forServer / load(file) / save(file)).
// Runs inside tsmedia_tests (tests/testmain.h).

#include <QCryptographicHash>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "filenames.h"
#include "medialink.h"
#include "serversettings.h"
#include "settings.h"
#include "testmain.h"

// settings.cpp keeps its ini file in the plugin's data folder (a temporary one in the tests, see
// tst_tsmedia.cpp); these tests use load(file) / save(file).

namespace {

MediaLink tsMediaLink(const QString& fileName)
{
    MediaLink link;
    link.serverUid = QStringLiteral("uid");
    link.channelId = 1;
    link.fileName  = fileName;
    link.protocol  = MediaLink::kProtocol;
    return link;
}

QString serverGroup(const QString& uid)
{
    return serversettings::groupName(serversettings::serverKey(uid));
}

} // namespace

class TestDragOutServers : public QObject
{
    Q_OBJECT

  private slots:
    // ---- filenames -------------------------------------------------------------------------
    void deviceNames_data();
    void deviceNames();
    void safeLocalFileName_data();
    void safeLocalFileName();
    void longNamesKeepTheirExtension();
    void exportFileName_data();
    void exportFileName();
    void exportFileNameFitsThePath();

    // ---- serversettings --------------------------------------------------------------------
    void serverKeyMatchesTheCacheFolder();
    void groupNames();
    void sanitizeName();
    void applyOverrides();
    void toggledOverride();
    void parseDataSaverCommand();
    void dataSaverTexts();
    void forServer();

    // ---- settings.ini ----------------------------------------------------------------------
    void iniRoundTrip();
    void iniHostileValues();
    void iniCapsServers();
    void iniForgetRemovesTheGroup();
    void iniGlobalDataSaver();
};

void TestDragOutServers::deviceNames_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("reserved");
    QTest::newRow("CON.txt") << QStringLiteral("CON.txt") << true;
    QTest::newRow("nul") << QStringLiteral("nul") << true;
    QTest::newRow("Com1.tar.gz") << QStringLiteral("Com1.tar.gz") << true;
    QTest::newRow("LPT9") << QStringLiteral("LPT9.log") << true;
    QTest::newRow("COM0") << QStringLiteral("com0") << true;
    QTest::newRow("LPT superscript 1") << (QStringLiteral("LPT") + QChar(0x00B9) + QStringLiteral(".log")) << true;
    QTest::newRow("COM superscript 3") << (QStringLiteral("COM") + QChar(0x00B3)) << true;
    QTest::newRow("CONIN$") << QStringLiteral("conin$.txt") << true;
    QTest::newRow("CONOUT$") << QStringLiteral("CONOUT$") << true;
    QTest::newRow("CON space") << QStringLiteral("CON .txt") << true;
    QTest::newRow("console.txt") << QStringLiteral("console.txt") << false;
    QTest::newRow("COM10") << QStringLiteral("COM10.txt") << false;
    QTest::newRow("xCON") << QStringLiteral("xCON.txt") << false;
    QTest::newRow("empty") << QString() << false;
}

void TestDragOutServers::deviceNames()
{
    QFETCH(QString, name);
    QFETCH(bool, reserved);
    QCOMPARE(filenames::isReservedDeviceName(name), reserved);
}

void TestDragOutServers::safeLocalFileName_data()
{
    QTest::addColumn<QString>("in");
    QTest::addColumn<QString>("out");
    QTest::newRow("plain") << QStringLiteral("holiday.jpg") << QStringLiteral("holiday.jpg");
    QTest::newRow("device") << QStringLiteral("CON.txt") << QStringLiteral("_CON.txt");
    QTest::newRow("device no ext") << QStringLiteral("nul") << QStringLiteral("_nul");
    QTest::newRow("device double ext") << QStringLiteral("Com1.tar.gz") << QStringLiteral("_Com1.tar.gz");
    QTest::newRow("device superscript") << (QStringLiteral("LPT") + QChar(0x00B9) + QStringLiteral(".log"))
                                        << (QStringLiteral("_LPT") + QChar(0x00B9) + QStringLiteral(".log"));
    QTest::newRow("not a device") << QStringLiteral("console.txt") << QStringLiteral("console.txt");
    QTest::newRow("trailing dot space") << QStringLiteral("report. ") << QStringLiteral("report");
    QTest::newRow("exe trailing dots") << QStringLiteral("x.exe...") << QStringLiteral("x.exe");
    QTest::newRow("reserved chars") << QStringLiteral("a:b?.pdf") << QStringLiteral("a_b_.pdf");
    QTest::newRow("all reserved") << QStringLiteral("<>:\"/\\|?*.txt") << QStringLiteral("_________.txt");
    QTest::newRow("controls") << (QStringLiteral("a") + QChar(0x01) + QChar(0x7f) + QStringLiteral("b\t.txt")) << QStringLiteral("a__b_.txt");
    QTest::newRow("double extension stays") << QStringLiteral("photo.jpg.exe") << QStringLiteral("photo.jpg.exe");
    QTest::newRow("empty") << QString() << QStringLiteral("file");
    QTest::newRow("only dots") << QStringLiteral("...") << QStringLiteral("file");
    QTest::newRow("persian") << QStringLiteral("عکس تعطیلات.jpg") << QStringLiteral("عکس تعطیلات.jpg");
}

void TestDragOutServers::safeLocalFileName()
{
    QFETCH(QString, in);
    QFETCH(QString, out);
    QCOMPARE(filenames::safeLocalFileName(in), out);
}

void TestDragOutServers::longNamesKeepTheirExtension()
{
    const QString name = filenames::safeLocalFileName(QString(300, QLatin1Char('a')) + QStringLiteral(".mp4"));
    QVERIFY(name.length() <= 120);
    QVERIFY(name.endsWith(QLatin1String(".mp4")));
    QVERIFY(name.startsWith(QLatin1String("aaaa")));

    // A cut never leaves half a surrogate pair (or dots / spaces) at its end.
    QString emoji;
    for (int i = 0; i < 100; ++i)
        emoji += QString::fromUcs4(U"\U0001F600");
    const QString cut = filenames::safeLocalFileName(emoji + QStringLiteral(".png"), 50);
    QVERIFY(cut.length() <= 50);
    QVERIFY(cut.endsWith(QLatin1String(".png")));
    const QString base = cut.left(cut.length() - 4);
    QVERIFY(!base.at(base.length() - 1).isHighSurrogate());
    QCOMPARE(QString::fromUcs4(base.toUcs4().constData(), base.toUcs4().size()), base); // valid UTF-16

    const QString dots = filenames::safeLocalFileName(QString(60, QLatin1Char('b')) + QString(60, QLatin1Char('.')) + QStringLiteral("x.zip"), 100);
    QVERIFY(!dots.contains(QLatin1String("..zip")));
    QVERIFY(dots.endsWith(QLatin1String(".zip")));
}

void TestDragOutServers::exportFileName_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("protocol");
    QTest::addColumn<QString>("out");
    QTest::newRow("random part") << QStringLiteral("holiday_3f9a1c2e.jpg") << MediaLink::kProtocol << QStringLiteral("holiday.jpg");
    QTest::newRow("pasted") << QStringLiteral("new_photo_1a2b3c4d.png") << MediaLink::kProtocol << QStringLiteral("Pasted image.png");
    QTest::newRow("plain link keeps its name") << QStringLiteral("holiday_3f9a1c2e.jpg") << 0 << QStringLiteral("holiday_3f9a1c2e.jpg");
    QTest::newRow("program") << QStringLiteral("photo.jpg.exe") << 0 << QStringLiteral("photo.jpg.exe");
    QTest::newRow("device") << QStringLiteral("CON_3f9a1c2e.txt") << MediaLink::kProtocol << QStringLiteral("_CON.txt");
    QTest::newRow("bidi override") << (QStringLiteral("invoice_") + QChar(0x202E) + QStringLiteral("fdp.exe")) << 0 << QStringLiteral("invoice_fdp.exe");
    QTest::newRow("trailing dots") << QStringLiteral("report. ") << 0 << QStringLiteral("report");
}

void TestDragOutServers::exportFileName()
{
    QFETCH(QString, fileName);
    QFETCH(int, protocol);
    QFETCH(QString, out);
    MediaLink link = tsMediaLink(fileName);
    link.protocol  = protocol;
    QCOMPARE(filenames::exportFileName(link), out);
}

void TestDragOutServers::exportFileNameFitsThePath()
{
    const MediaLink link = tsMediaLink(QString(200, QLatin1Char('n')) + QStringLiteral("_3f9a1c2e.mkv"));
    const QString   deep = filenames::exportFileName(link, 200);
    QVERIFY(200 + deep.length() <= 240);
    QVERIFY(deep.endsWith(QLatin1String(".mkv")));
    QVERIFY(filenames::exportFileName(link, 60).length() <= 120);
    // However deep the folder, a usable name remains.
    QVERIFY(filenames::exportFileName(link, 1000).length() >= 8);
}

void TestDragOutServers::serverKeyMatchesTheCacheFolder()
{
    const QString uid = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    // Core::cachePathFor uses this expression for cache/<id>/.
    const QString expected = QString::fromLatin1(QCryptographicHash::hash(uid.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    QCOMPARE(serversettings::serverKey(uid), expected);
    QCOMPARE(serversettings::serverKey(uid).size(), 12);
    QCOMPARE(serversettings::serverKey(QStringLiteral("abc")), QStringLiteral("a9993e364706")); // SHA-1("abc")
    QVERIFY(serversettings::serverKey(QString()).isEmpty());
}

void TestDragOutServers::groupNames()
{
    QString key;
    QVERIFY(serversettings::isGroupName(QStringLiteral("server_0123456789ab"), &key));
    QCOMPARE(key, QStringLiteral("0123456789ab"));
    QVERIFY(!serversettings::isGroupName(QStringLiteral("server_XYZ")));
    QVERIFY(!serversettings::isGroupName(QStringLiteral("server_0123456789AB"))); // the id is lower case
    QVERIFY(!serversettings::isGroupName(QStringLiteral("server_0123456789abc")));
    QVERIFY(!serversettings::isGroupName(QStringLiteral("server_0123456789a")));
    QVERIFY(!serversettings::isGroupName(QStringLiteral("General")));
    QCOMPARE(serversettings::groupName(QStringLiteral("0123456789ab")), QStringLiteral("server_0123456789ab"));
}

void TestDragOutServers::sanitizeName()
{
    const QString hostile = QStringLiteral("  Mehdi's ") + QChar(0x202E) + QStringLiteral("revreS\n\t") + QChar(0x200F) + QStringLiteral(" x  ");
    QCOMPARE(serversettings::sanitizeName(hostile), QStringLiteral("Mehdi's revreS x"));
    QCOMPARE(serversettings::sanitizeName(QString(100, QLatin1Char('s'))).size(), 64);
}

void TestDragOutServers::applyOverrides()
{
    Settings global;
    global.dataSaver         = false;
    global.uploadDirectory   = QStringLiteral("/tsmedia");
    global.uploadMaxMB       = 100;
    global.addRequiredNotice = true;

    Settings none = global;
    serversettings::applyOverrides(none, ServerOverrides());
    QCOMPARE(none.dataSaver, false);
    QCOMPARE(none.uploadDirectory, QStringLiteral("/tsmedia"));
    QCOMPARE(none.uploadMaxMB, 100);
    QCOMPARE(none.addRequiredNotice, true);

    ServerOverrides partial;
    partial.dataSaver = true;
    Settings some     = global;
    serversettings::applyOverrides(some, partial);
    QCOMPARE(some.dataSaver, true);
    QCOMPARE(some.uploadMaxMB, 100);

    ServerOverrides all;
    all.dataSaver         = true;
    all.uploadDirectory   = QStringLiteral("media\\");
    all.uploadMaxMB       = 50;
    all.addRequiredNotice = false;
    Settings everything   = global;
    serversettings::applyOverrides(everything, all);
    QCOMPARE(everything.dataSaver, true);
    QCOMPARE(everything.uploadDirectory, QStringLiteral("/media"));
    QCOMPARE(everything.uploadMaxMB, 50);
    QCOMPARE(everything.addRequiredNotice, false);

    QVERIFY(ServerOverrides().isEmpty());
    ServerOverrides named;
    named.name = QStringLiteral("only a name");
    QVERIFY(named.isEmpty());
    QVERIFY(!partial.isEmpty());
}

void TestDragOutServers::toggledOverride()
{
    QCOMPARE(serversettings::toggledOverride(false, false), std::optional<bool>());
    QCOMPARE(serversettings::toggledOverride(false, true), std::optional<bool>(true));
    QCOMPARE(serversettings::toggledOverride(true, false), std::optional<bool>(false));
    QCOMPARE(serversettings::toggledOverride(true, true), std::optional<bool>());
}

void TestDragOutServers::parseDataSaverCommand()
{
    using C = serversettings::DataSaverCommand;
    QCOMPARE(serversettings::parseDataSaverCommand(QString()), C::Status);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral("  ")), C::Status);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral("on")), C::On);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral(" OFF ")), C::Off);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral("Default")), C::Default);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral("maybe")), C::Invalid);
    QCOMPARE(serversettings::parseDataSaverCommand(QStringLiteral("on off")), C::Invalid);
}

void TestDragOutServers::dataSaverTexts()
{
    QCOMPARE(serversettings::dataSaverStatusText(true, true), QStringLiteral("Data saver is on for this server (its own setting)."));
    QCOMPARE(serversettings::dataSaverStatusText(false, false), QStringLiteral("Data saver is off for this server (same as all servers)."));
    QVERIFY(serversettings::dataSaverChangedText(true).startsWith(QLatin1String("Data saver is on")));
    QVERIFY(serversettings::dataSaverChangedText(false).startsWith(QLatin1String("Data saver is off")));
    QVERIFY(serversettings::dataSaverConnectText().contains(QStringLiteral("Plugins → TS Media chat")));
    QVERIFY(!serversettings::dataSaverUsageText().isEmpty());
    QVERIFY(!serversettings::dataSaverNotConnectedText().isEmpty());
}

void TestDragOutServers::forServer()
{
    Settings s;
    s.dataSaver   = false;
    s.uploadMaxMB = 100;
    ServerOverrides own;
    own.dataSaver   = true;
    own.uploadMaxMB = 50;
    s.servers.insert(serversettings::serverKey(QStringLiteral("mine")), own);

    const Settings unknown = s.forServer(QStringLiteral("someone else's"));
    QCOMPARE(unknown.dataSaver, false);
    QCOMPARE(unknown.uploadMaxMB, 100);
    QCOMPARE(unknown.uploadDirectory, s.uploadDirectory);
    QCOMPARE(unknown.addRequiredNotice, s.addRequiredNotice);
    QVERIFY(!s.overridesFor(QStringLiteral("someone else's")));
    QVERIFY(!s.overridesFor(QString()));
    QCOMPARE(s.forServer(QString()).dataSaver, false);

    const Settings mine = s.forServer(QStringLiteral("mine"));
    QCOMPARE(mine.dataSaver, true);
    QCOMPARE(mine.uploadMaxMB, 50);
    QVERIFY(s.overridesFor(QStringLiteral("mine")));
    // The global values are untouched.
    QCOMPARE(s.dataSaver, false);
    QCOMPARE(s.uploadMaxMB, 100);
}

void TestDragOutServers::iniRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("settings.ini"));

    Settings written;
    written.dataSaver = true;
    ServerOverrides a;
    a.name            = QStringLiteral("Mehdi's Server");
    a.dataSaver       = false;
    a.uploadDirectory = QStringLiteral("/media");
    a.uploadMaxMB     = 50;
    a.addRequiredNotice = false;
    ServerOverrides b;
    b.name      = QStringLiteral("Big public server");
    b.dataSaver = true;
    written.servers.insert(serversettings::serverKey(QStringLiteral("uidA")), a);
    written.servers.insert(serversettings::serverKey(QStringLiteral("uidB")), b);
    written.save(file);

    Settings read;
    read.load(file);
    QCOMPARE(read.dataSaver, true);
    QCOMPARE(read.servers.size(), 2);
    const ServerOverrides ra = read.servers.value(serversettings::serverKey(QStringLiteral("uidA")));
    QCOMPARE(ra.name, a.name);
    QVERIFY(ra.sameValues(a));
    const ServerOverrides rb = read.servers.value(serversettings::serverKey(QStringLiteral("uidB")));
    QVERIFY(rb.sameValues(b));
    QVERIFY(!rb.uploadMaxMB);

    // The groups are in the file under their ids.
    const QSettings raw(file, QSettings::IniFormat);
    QVERIFY(raw.childGroups().contains(serverGroup(QStringLiteral("uidA"))));
    QCOMPARE(raw.value(serverGroup(QStringLiteral("uidA")) + QStringLiteral("/uploadMaxMB")).toInt(), 50);
}

void TestDragOutServers::iniHostileValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(file, QSettings::IniFormat);
        const QString g1 = serverGroup(QStringLiteral("one"));
        raw.setValue(g1 + QStringLiteral("/dataSaver"), QStringLiteral("maybe"));
        raw.setValue(g1 + QStringLiteral("/uploadMaxMB"), 999999);
        raw.setValue(g1 + QStringLiteral("/uploadDirectory"), QStringLiteral("..\\x\\"));
        raw.setValue(g1 + QStringLiteral("/name"), QStringLiteral("Evil") + QChar(0x202E) + QStringLiteral("name\x01") + QString(100, QLatin1Char('z')));
        const QString g2 = serverGroup(QStringLiteral("two"));
        raw.setValue(g2 + QStringLiteral("/uploadMaxMB"), -5);
        raw.setValue(g2 + QStringLiteral("/addRequiredNotice"), QStringLiteral("1"));
        raw.setValue(g2 + QStringLiteral("/uploadDirectory"), QString(300, QLatin1Char('d'))); // too long: ignored
        const QString g3 = serverGroup(QStringLiteral("three"));
        raw.setValue(g3 + QStringLiteral("/uploadMaxMB"), QStringLiteral("lots"));
        raw.setValue(g3 + QStringLiteral("/name"), QStringLiteral("only garbage"));
        raw.setValue(QStringLiteral("server_XYZ/dataSaver"), true); // not a server id
        raw.setValue(QStringLiteral("server_0123456789AB/dataSaver"), true);
        raw.sync();
    }

    Settings s;
    s.load(file);
    QCOMPARE(s.servers.size(), 2); // "three" has no valid value, the bad ids are ignored
    const ServerOverrides one = s.servers.value(serversettings::serverKey(QStringLiteral("one")));
    QVERIFY(!one.dataSaver);
    QCOMPARE(one.uploadMaxMB, std::optional<int>(Settings::uploadMaxMBRange.max));
    QCOMPARE(one.uploadDirectory, std::optional<QString>(QStringLiteral("/../x")));
    QCOMPARE(one.name.size(), 64);
    QVERIFY(!one.name.contains(QChar(0x202E)));
    QVERIFY(!one.name.contains(QChar(0x01)));
    const ServerOverrides two = s.servers.value(serversettings::serverKey(QStringLiteral("two")));
    QCOMPARE(two.uploadMaxMB, std::optional<int>(Settings::uploadMaxMBRange.min));
    QCOMPARE(two.addRequiredNotice, std::optional<bool>(true));
    QVERIFY(!two.uploadDirectory);

    // Saving writes back only valid groups.
    s.save(file);
    const QSettings raw(file, QSettings::IniFormat);
    const QStringList groups = raw.childGroups();
    QVERIFY(!groups.contains(QStringLiteral("server_XYZ")));
    QVERIFY(!groups.contains(QStringLiteral("server_0123456789AB")));
    QVERIFY(!groups.contains(serverGroup(QStringLiteral("three"))));
    QVERIFY(groups.contains(serverGroup(QStringLiteral("one"))));
    QCOMPARE(raw.value(serverGroup(QStringLiteral("one")) + QStringLiteral("/uploadDirectory")).toString(), QStringLiteral("/../x"));
}

void TestDragOutServers::iniCapsServers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings raw(file, QSettings::IniFormat);
        for (int i = 0; i < 250; ++i)
            raw.setValue(serverGroup(QStringLiteral("uid%1").arg(i)) + QStringLiteral("/dataSaver"), true);
        raw.sync();
    }
    Settings s;
    s.load(file);
    QCOMPARE(s.servers.size(), serversettings::kMaxServers);

    // Writing more than the cap (e.g. built in code) keeps the file bounded too.
    for (int i = 0; i < 250; ++i) {
        ServerOverrides o;
        o.uploadMaxMB = 10;
        s.servers.insert(serversettings::serverKey(QStringLiteral("more%1").arg(i)), o);
    }
    s.save(file);
    const QSettings raw(file, QSettings::IniFormat);
    int             groups = 0;
    for (const QString& group : raw.childGroups())
        groups += serversettings::isGroupName(group) ? 1 : 0;
    QCOMPARE(groups, serversettings::kMaxServers);
}

void TestDragOutServers::iniForgetRemovesTheGroup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("settings.ini"));

    Settings s;
    ServerOverrides own;
    own.dataSaver = true;
    own.name      = QStringLiteral("Forget me");
    s.servers.insert(serversettings::serverKey(QStringLiteral("gone")), own);
    ServerOverrides nameOnly;
    nameOnly.name = QStringLiteral("Only a name");
    s.servers.insert(serversettings::serverKey(QStringLiteral("nameonly")), nameOnly);
    s.save(file);
    {
        const QSettings raw(file, QSettings::IniFormat);
        QVERIFY(raw.childGroups().contains(serverGroup(QStringLiteral("gone"))));
        QVERIFY(!raw.childGroups().contains(serverGroup(QStringLiteral("nameonly")))); // no own value: not stored
    }

    s.servers.remove(serversettings::serverKey(QStringLiteral("gone")));
    s.save(file);
    const QSettings raw(file, QSettings::IniFormat);
    QVERIFY(!raw.childGroups().contains(serverGroup(QStringLiteral("gone"))));
    Settings again;
    again.load(file);
    QVERIFY(again.servers.isEmpty());
}

void TestDragOutServers::iniGlobalDataSaver()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath(QStringLiteral("settings.ini"));
    Settings fresh;
    fresh.load(file); // no file yet: the defaults
    QCOMPARE(fresh.dataSaver, false);
    QVERIFY(fresh.servers.isEmpty());
    {
        QSettings raw(file, QSettings::IniFormat);
        raw.setValue(QStringLiteral("dataSaver"), true);
        raw.sync();
    }
    Settings s;
    s.load(file);
    QCOMPARE(s.dataSaver, true);
}

TSMEDIA_REGISTER_TEST(TestDragOutServers)

#include "tst_dragout_servers.moc"
