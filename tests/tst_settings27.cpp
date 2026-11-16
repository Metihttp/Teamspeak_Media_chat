// 2.7 Part 0: the three new settings keys (settings.h / settings.cpp): autoLoadLinkPreviewsOnlyFriends (Only friends),
// chatZoom (the saved chat zoom, tenths of a point) and chatLineColor (the colour of TS Media's own chat lines, a preset
// name or "" for Automatic). Defaults, missing keys, round trips, garbage, the choice normalisation and forServer().

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include "serversettings.h"
#include "settings.h"
#include "testmain.h"

class TestSettings27 : public QObject {
    Q_OBJECT

    // Writes raw values into an ini file the way a hand edit (or an older version) would.
    static void writeRaw(const QString& file, const QString& name, const QVariant& value)
    {
        QSettings ini(file, QSettings::IniFormat);
        ini.setValue(name, value);
        ini.sync();
    }

    static QVariant readRaw(const QString& file, const QString& name)
    {
        const QSettings ini(file, QSettings::IniFormat);
        return ini.value(name);
    }

private slots:
    void defaults()
    {
        const Settings s;
        QCOMPARE(s.autoLoadLinkPreviewsOnlyFriends, false);
        QCOMPARE(s.chatZoom, 0);
        QVERIFY(s.chatLineColor.isEmpty());
        QCOMPARE(Settings::chatZoomRange.min, -40);
        QCOMPARE(Settings::chatZoomRange.max, 240);
        QCOMPARE(Settings::chatZoomStep, 10);
        const QStringList expected = {QStringLiteral("blue"),   QStringLiteral("teal"), QStringLiteral("green"),
                                      QStringLiteral("orange"), QStringLiteral("pink"), QStringLiteral("text")};
        QStringList choices;
        for (const char* name : Settings::lineColorChoices)
            choices << QString::fromLatin1(name);
        QCOMPARE(choices, expected);
    }

    void missingKeysReadTheDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        writeRaw(file, QStringLiteral("hdEmoji"), false); // a 2.6 file: none of the new keys
        Settings s;
        s.autoLoadLinkPreviewsOnlyFriends = true;
        s.chatZoom                        = 30;
        s.chatLineColor                   = QStringLiteral("teal");
        s.load(file);
        QVERIFY(!s.hdEmoji);
        QCOMPARE(s.autoLoadLinkPreviewsOnlyFriends, false);
        QCOMPARE(s.chatZoom, 0);
        QVERIFY(s.chatLineColor.isEmpty());

        // A file that doesn't exist at all (a fresh install).
        Settings fresh;
        fresh.chatZoom = -20;
        fresh.load(dir.filePath(QStringLiteral("none.ini")));
        QCOMPARE(fresh.autoLoadLinkPreviewsOnlyFriends, false);
        QCOMPARE(fresh.chatZoom, 0);
        QVERIFY(fresh.chatLineColor.isEmpty());
    }

    void roundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));

        Settings s;
        s.autoLoadLinkPreviewsOnlyFriends = true;
        s.chatZoom                        = 20;
        s.chatLineColor                   = QStringLiteral("orange");
        s.save(file);
        QCOMPARE(readRaw(file, QStringLiteral("autoLoadLinkPreviewsOnlyFriends")).toBool(), true);
        QCOMPARE(readRaw(file, QStringLiteral("chatZoom")).toInt(), 20);
        QCOMPARE(readRaw(file, QStringLiteral("chatLineColor")).toString(), QStringLiteral("orange"));

        Settings loaded;
        loaded.load(file);
        QCOMPARE(loaded.autoLoadLinkPreviewsOnlyFriends, true);
        QCOMPARE(loaded.chatZoom, 20);
        QCOMPARE(loaded.chatLineColor, QStringLiteral("orange"));

        // And back to the defaults.
        loaded.autoLoadLinkPreviewsOnlyFriends = false;
        loaded.chatZoom                        = 0;
        loaded.chatLineColor.clear();
        loaded.save(file);
        Settings again;
        again.autoLoadLinkPreviewsOnlyFriends = true;
        again.chatZoom                        = 50;
        again.chatLineColor                   = QStringLiteral("pink");
        again.load(file);
        QCOMPARE(again.autoLoadLinkPreviewsOnlyFriends, false);
        QCOMPARE(again.chatZoom, 0);
        QVERIFY(again.chatLineColor.isEmpty());

        // Every step of the range, the bounds and every preset survive a round trip.
        for (int tenths = Settings::chatZoomRange.min; tenths <= Settings::chatZoomRange.max;
             tenths += Settings::chatZoomStep) {
            Settings z;
            z.chatZoom = tenths;
            z.save(file);
            Settings r;
            r.load(file);
            QCOMPARE(r.chatZoom, tenths);
        }
        for (const char* name : Settings::lineColorChoices) {
            Settings c;
            c.chatLineColor = QString::fromLatin1(name);
            c.save(file);
            Settings r;
            r.load(file);
            QCOMPARE(r.chatLineColor, QString::fromLatin1(name));
        }
    }

    void chatZoomGarbage_data()
    {
        QTest::addColumn<QVariant>("raw");
        QTest::addColumn<int>("expected");
        QTest::newRow("above the range") << QVariant(999) << 240;
        QTest::newRow("below the range") << QVariant(-999) << -40;
        QTest::newRow("not a number") << QVariant(QStringLiteral("x")) << 0;
        QTest::newRow("empty") << QVariant(QString()) << 0;
        QTest::newRow("upper bound") << QVariant(240) << 240;
        QTest::newRow("lower bound") << QVariant(-40) << -40;
        QTest::newRow("in range") << QVariant(QStringLiteral("30")) << 30;
    }

    void chatZoomGarbage()
    {
        QFETCH(QVariant, raw);
        QFETCH(int, expected);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        writeRaw(file, QStringLiteral("chatZoom"), raw);
        Settings s;
        s.chatZoom = 70;
        s.load(file);
        QCOMPARE(s.chatZoom, expected);
    }

    void chatZoomSavedClamped()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        Settings s;
        s.chatZoom = 999;
        s.save(file);
        QCOMPARE(readRaw(file, QStringLiteral("chatZoom")).toInt(), 240);
        s.chatZoom = -999;
        s.save(file);
        QCOMPARE(readRaw(file, QStringLiteral("chatZoom")).toInt(), -40);
    }

    void normalizeLineColor_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("trimmed, any case") << QStringLiteral(" Teal ") << QStringLiteral("teal");
        QTest::newRow("upper case") << QStringLiteral("TEXT") << QStringLiteral("text");
        QTest::newRow("exact") << QStringLiteral("blue") << QStringLiteral("blue");
        QTest::newRow("tab and mixed case") << QStringLiteral("\tOrAnGe") << QStringLiteral("orange");
        QTest::newRow("a hex colour") << QStringLiteral("#ff0000") << QString();
        QTest::newRow("not a preset") << QStringLiteral("red") << QString();
        QTest::newRow("a preset inside a word") << QStringLiteral("blueish") << QString();
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("spaces only") << QStringLiteral("   ") << QString();
    }

    void normalizeLineColor()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const QString normalized = Settings::normalizeLineColor(input);
        QCOMPARE(normalized, expected);
        if (expected.isEmpty())
            QVERIFY(normalized.isEmpty());

        // The same through the ini file.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        writeRaw(file, QStringLiteral("chatLineColor"), input);
        Settings s;
        s.chatLineColor = QStringLiteral("green");
        s.load(file);
        QCOMPARE(s.chatLineColor, expected);
    }

    void invalidChoiceSavedAsAutomatic()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        for (const QString& bad : {QStringLiteral("#ff0000"), QStringLiteral("red"), QStringLiteral("blue green")}) {
            Settings s;
            s.chatLineColor = bad;
            s.save(file);
            QVERIFY2(readRaw(file, QStringLiteral("chatLineColor")).toString().isEmpty(), qPrintable(bad));
            Settings r;
            r.chatLineColor = QStringLiteral("pink");
            r.load(file);
            QVERIFY2(r.chatLineColor.isEmpty(), qPrintable(bad));
        }
        // A valid choice in another case is saved in lower case.
        Settings s;
        s.chatLineColor = QStringLiteral(" Green ");
        s.save(file);
        QCOMPARE(readRaw(file, QStringLiteral("chatLineColor")).toString(), QStringLiteral("green"));
    }

    void forServerKeepsTheNewKeys()
    {
        Settings s;
        s.autoLoadLinkPreviewsOnlyFriends = true;
        s.chatZoom                        = -30;
        s.chatLineColor                   = QStringLiteral("text");
        ServerOverrides own;
        own.dataSaver   = true;
        own.uploadMaxMB = 50;
        s.servers.insert(serversettings::serverKey(QStringLiteral("mine")), own);

        for (const QString& uid : {QStringLiteral("mine"), QStringLiteral("someone else's"), QString()}) {
            const Settings server = s.forServer(uid);
            QCOMPARE(server.autoLoadLinkPreviewsOnlyFriends, true);
            QCOMPARE(server.chatZoom, -30);
            QCOMPARE(server.chatLineColor, QStringLiteral("text"));
        }
        QCOMPARE(s.forServer(QStringLiteral("mine")).dataSaver, true); // the overrides still apply
    }

    // 2.7 dark mode: the darkMode key (design_dark 3.3). Global; written at once by the switch, the menu and the hotkey.
    void darkModeKey()
    {
        QCOMPARE(Settings().darkMode, false);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("settings.ini"));
        writeRaw(file, QStringLiteral("hdEmoji"), false); // a 2.6 file: no darkMode
        Settings missing;
        missing.darkMode = true;
        missing.load(file);
        QCOMPARE(missing.darkMode, false);

        Settings on;
        on.darkMode = true;
        on.save(file);
        QCOMPARE(readRaw(file, QStringLiteral("darkMode")).toBool(), true);
        Settings loaded;
        loaded.load(file);
        QCOMPARE(loaded.darkMode, true);
        loaded.darkMode = false;
        loaded.save(file);
        Settings again;
        again.darkMode = true;
        again.load(file);
        QCOMPARE(again.darkMode, false);

        // A hand-edited value reads by QVariant's rule (readBool): "false", "0" and empty are off, any other text is on.
        const QList<QPair<QString, bool>> raw = {{QStringLiteral("false"), false}, {QStringLiteral("0"), false}, {QString(), false},
                                                 {QStringLiteral("true"), true},   {QStringLiteral("1"), true},  {QStringLiteral("x"), true}};
        for (const auto& r : raw) {
            writeRaw(file, QStringLiteral("darkMode"), r.first);
            Settings s;
            s.darkMode = !r.second;
            s.load(file);
            QVERIFY2(s.darkMode == r.second, qPrintable(r.first));
        }

        Settings s;
        s.darkMode = true;
        for (const QString& uid : {QStringLiteral("mine"), QString()})
            QCOMPARE(s.forServer(uid).darkMode, true);
    }
};

TSMEDIA_REGISTER_TEST(TestSettings27)
#include "tst_settings27.moc"
