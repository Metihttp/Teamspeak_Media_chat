// 2.6 chat proxy, I2b (design 9.4): the Chat proxy window and its Settings group, the Plugins menu pair, the hotkey and
// /tsmedia proxy, the lines and notices (with their links), the failure strips and the card menu, the route-change
// and health rules of the chat, the player switch's label, the Privacy & updates tab's access keys and height, and the
// CRASH RULE in the new files. Offscreen; a real proxy::Control on a temporary settings.ini (its bridge on 127.0.0.1
// only when a test turns the chat proxy on, nothing contacted), the fake TeamSpeak for the printed lines.

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStyle>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

#include <memory>

#include "fakets3.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkstate.h"
#include "privacysection.h"
#include "proxy/bridge.h"
#include "proxy/control.h"
#include "proxy/linkparse.h"
#include "proxy/protect.h"
#include "proxy/proxycommands.h"
#include "proxy/proxygroup.h"
#include "proxy/proxytexts.h"
#include "proxy/proxywindow.h"
#include "proxy/router.h"
#include "proxy/windowsproxy.h"
#include "settings.h"
#include "settingstheme.h"
#include "testmain.h"
#include "ts3api.h"
#include "uiutil.h"
#include "update/sessionplan.h"
#include "ytplayer/ytpage.h"

using namespace proxy;
namespace lp = linkpreview;

namespace {

QString a(const char* text)
{
    return QString::fromUtf8(text);
}

// What setPluginMenuEnabled was told (id -> enabled), the last call per id.
QHash<int, int> g_menu;
void fakeMenuEnabled(const char*, int id, int enabled)
{
    g_menu.insert(id, enabled);
}

QString sourceText(const QString& relative)
{
    QFile f(QString::fromUtf8(TSMEDIA_SOURCE_DIR) + QLatin1Char('/') + relative);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// Every access key (the character after a single '&') of texts.
QList<QChar> keysOf(const QStringList& texts)
{
    QList<QChar> keys;
    for (const QString& t : texts) {
        for (int i = 0; i + 1 < t.size(); ++i) {
            if (t.at(i) == QLatin1Char('&') && t.at(i + 1) != QLatin1Char('&')) {
                keys << t.at(i + 1).toLower();
                break;
            }
        }
    }
    return keys;
}

windowsproxy::IeConfig manual(const char* proxyText)
{
    windowsproxy::IeConfig c;
    c.ok    = true;
    c.proxy = QString::fromLatin1(proxyText);
    return c;
}

void publishWindows()
{
    auto s        = std::make_shared<Snapshot>();
    s->mode       = Mode::System;
    s->generation = 1;
    s->refused    = Failure::None;
    publish(s);
}

// A proxy::Control on its own settings.ini, the plugin's instance for the test (Control::current()), a clock the test
// moves, and the window opener's calls.
struct Env {
    QTemporaryDir            dir;
    qint64                   clock = 10'000'000;
    std::unique_ptr<Control> control;
    QVector<Control::WindowRequest> opened;

    explicit Env(const Config* saved = nullptr)
    {
        fakets3::install(dir.path());
        ts3::pluginId                   = QStringLiteral("{proxy-ui-test}");
        ts3::funcs.setPluginMenuEnabled = &fakeMenuEnabled;
        g_menu.clear();
        Control::Options o;
        o.settingsFile = dir.path() + QStringLiteral("/settings.ini");
        o.clockMs      = [this] { return clock; };
        if (saved)
            saveConfig(o.settingsFile, *saved);
        control = std::make_unique<Control>(o);
        Control::setInstanceForTests(control.get());
        control->setNoticeNonceForTests(QStringLiteral("00112233aabbccdd"));
        control->setWindowOpener([this](Control::WindowRequest r) { opened << r; });
        control->start();
    }
    ~Env()
    {
        control.reset(); // stops the bridge; the snapshot goes back to the refusing one (the cleanup republishes)
        ts3::pluginId.clear();
    }
    // The lines TS Media printed (the fake TeamSpeak's log: "[chat] ..."), plain text without the prefix's BBCode.
    QStringList lines() const
    {
        QStringList out;
        for (const QString& l : fakets3::logLines()) {
            if (!l.startsWith(QStringLiteral("[chat] ")))
                continue;
            QString t = l.mid(7);
            t.remove(QRegularExpression(QStringLiteral("\\[/?[a-z]+(=[^\\]]*)?\\]")));
            out << t;
        }
        return out;
    }
    QString lastLine() const { return lines().isEmpty() ? QString() : lines().last(); }
};

Config savedProxy(const char* host = "127.0.0.1", quint16 port = 2080, bool on = false)
{
    Config c;
    c.on   = on;
    c.type = Type::Socks5;
    c.host = normalizeHost(QString::fromLatin1(host));
    c.port = port;
    return c;
}

QPushButton* buttonWithText(QWidget* root, const QString& text)
{
    for (QPushButton* b : root->findChildren<QPushButton*>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

} // namespace

class TestProxyUi : public QObject
{
    Q_OBJECT

  private slots:
    void init()
    {
        windowsproxy::setIeConfigProviderForTests([] { return manual(""); }); // Windows names no proxy
    }
    void cleanup()
    {
        windowsproxy::setIeConfigProviderForTests({});
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QLatin1String("tsmediaProxyDialog"))
                delete w;
        }
        publishWindows(); // the suite's route (testmain's setup)
        markProxyRouteChange(0);
        Settings::instance() = Settings();
    }

    // ---- the lines (design 1.4) ---------------------------------------------------------------------------------------
    void proxyTexts()
    {
        const Config here = savedProxy();
        Config       lan  = savedProxy("192.168.1.5", 1080);
        lan.type          = Type::Http;
        Config remote     = savedProxy("proxy.example.net", 3128);
        remote.user       = QStringLiteral("secret-user");
        QCOMPARE(texts::toggleLine(Toggle::TurnedOn, here, 0),
                 a("Chat proxy on: SOCKS5 proxy 127.0.0.1:2080. Link previews, the YouTube player and update checks use it; TeamSpeak itself doesn't."));
        // Chat lines (TeamSpeak keeps chat logs) name host:port only for a proxy on this PC (F24), never the user name.
        QCOMPARE(texts::toggleLine(Toggle::TurnedOn, lan, 0),
                 a("Chat proxy on: HTTP proxy on the local network. Link previews, the YouTube player and update checks use it; TeamSpeak itself doesn't."));
        QCOMPARE(texts::toggleLine(Toggle::AlreadyOn, remote, 0), a("The chat proxy is already on: SOCKS5 proxy on another computer."));
        QVERIFY(!texts::toggleLine(Toggle::AlreadyOn, remote, 0).contains(a("proxy.example.net")));
        QVERIFY(!texts::toggleLine(Toggle::AlreadyOn, remote, 0).contains(a("secret-user")));
        QCOMPARE(texts::toggleLine(Toggle::TurnedOff, here, 0), a("Chat proxy off. TS Media uses your Windows proxy settings again."));
        Config direct  = here;
        direct.whenOff = WhenOff::Direct;
        QCOMPARE(texts::toggleLine(Toggle::TurnedOff, direct, 0), a("Chat proxy off. TS Media connects directly again."));
        QCOMPARE(texts::toggleLine(Toggle::TurnedOff, here, 4),
                 a("Chat proxy off. TS Media uses your Windows proxy settings again. Previews that waited for the proxy load when you click them."));
        QCOMPARE(texts::toggleLine(Toggle::AlreadyOff, here, 0), a("The chat proxy is already off."));
        QCOMPARE(texts::toggleLine(Toggle::NotSetUp, Config(), 0), a("No chat proxy is set up yet. Enter one in the Chat proxy window."));
        // /tsmedia proxy
        Config on = here;
        on.on     = true;
        QCOMPARE(texts::statusLine(on, Health::State::Unknown, Failure::None), a("Chat proxy on: SOCKS5 proxy 127.0.0.1:2080 (not checked yet)."));
        QCOMPARE(texts::statusLine(on, Health::State::Working, Failure::None), a("Chat proxy on: SOCKS5 proxy 127.0.0.1:2080 (working)."));
        QCOMPARE(texts::statusLine(on, Health::State::Down, Failure::Unreachable),
                 a("Chat proxy on: SOCKS5 proxy 127.0.0.1:2080 (not working: can't reach the proxy at 127.0.0.1:2080)."));
        Config remoteOn = remote;
        remoteOn.on     = true;
        QCOMPARE(texts::statusLine(remoteOn, Health::State::Down, Failure::Unreachable),
                 a("Chat proxy on: SOCKS5 proxy on another computer (not working: can't reach the proxy)."));
        QCOMPARE(texts::statusLine(Config(), Health::State::Unknown, Failure::None), a("Chat proxy off. TS Media uses your Windows proxy settings."));
        QCOMPARE(texts::statusLine(direct, Health::State::Unknown, Failure::None),
                 a("Chat proxy off. TS Media connects directly. Saved: SOCKS5 proxy 127.0.0.1:2080."));
        QCOMPARE(texts::usageLine(), a("Use /tsmedia proxy on, /tsmedia proxy off or /tsmedia proxy."));
        QCOMPARE(texts::helpLine(), a("[b]/tsmedia proxy[/b] on | off — turn the chat proxy for link previews, the YouTube player and update checks on or off"));
        QCOMPARE(texts::menuOnText(), a("Turn on chat proxy"));
        QCOMPARE(texts::menuOffText(), a("Turn off chat proxy"));
        QCOMPARE(texts::hotkeyDescription(), a("Turn the chat proxy on or off"));
        // Notices (design 1.5).
        QCOMPARE(texts::noticeFor(Mode::Custom, Failure::Unreachable), texts::Notice::ChatDown);
        QCOMPARE(texts::noticeFor(Mode::Custom, Failure::PasswordUnreadable), texts::Notice::ChatPassword);
        QCOMPARE(texts::noticeFor(Mode::Custom, Failure::Intercepted), texts::Notice::ChatIntercepted);
        QCOMPARE(texts::noticeFor(Mode::System, Failure::CantReachSite), texts::Notice::WindowsFailed);
        QCOMPARE(texts::noticeFor(Mode::System, Failure::SocksOnlyWindows), texts::Notice::WindowsSocksOnly);
        QCOMPARE(texts::noticeFor(Mode::System, Failure::UnsupportedWindowsProxy), texts::Notice::WindowsScript);
        QCOMPARE(texts::noticeFor(Mode::Custom, Failure::SiteRefused), texts::Notice::None); // not route-level
        QCOMPARE(texts::noticeText(texts::Notice::ChatDown, texts::chatShortText(Failure::Unreachable, here)),
                 a("The chat proxy isn't working: can't reach the proxy at 127.0.0.1:2080. Link previews wait until it works."));
        QCOMPARE(texts::noticeText(texts::Notice::ChatDown, texts::chatShortText(Failure::Unreachable, remote)),
                 a("The chat proxy isn't working: can't reach the proxy. Link previews wait until it works."));
        QCOMPARE(texts::noticeLinkText(texts::Notice::ChatDown), a("Turn off chat proxy"));
        QCOMPARE(texts::noticeLinkText(texts::Notice::ChatIntercepted), a("Turn off chat proxy"));
        QCOMPARE(texts::noticeLinkText(texts::Notice::ChatPassword), a("Proxy settings"));
        QCOMPARE(texts::noticeLinkText(texts::Notice::WindowsFailed), a("Proxy settings"));
        for (const texts::Notice n : {texts::Notice::ChatDown, texts::Notice::ChatPassword, texts::Notice::ChatIntercepted, texts::Notice::WindowsFailed,
                                      texts::Notice::WindowsSocksOnly, texts::Notice::WindowsScript})
            QVERIFY(!texts::noticeText(n, a("x")).isEmpty());
    }

    // ---- the status row (design 1.3.4) -------------------------------------------------------------------------------
    void statusRowTexts()
    {
        texts::StatusInput in;
        in.nowMs = 1'000'000;
        // Windows mode, off.
        in.summary.kind     = windowsproxy::Summary::Kind::HttpProxy;
        in.summary.hostPort = QStringLiteral("127.0.0.1:10809");
        texts::StatusRow row = texts::statusRow(in);
        QCOMPARE(row.text, a("TS Media uses the proxy in your Windows settings (127.0.0.1:10809)."));
        QCOMPARE(row.icon, texts::StatusRow::Icon::None);
        in.summary.kind = windowsproxy::Summary::Kind::Script;
        QCOMPARE(texts::statusRow(in).text, a("TS Media follows your Windows proxy setup script or automatic detection."));
        in.summary.kind = windowsproxy::Summary::Kind::None;
        QCOMPARE(texts::statusRow(in).text, a("Windows names no proxy, so TS Media connects directly."));
        in.summary.kind     = windowsproxy::Summary::Kind::SocksOnly;
        in.summary.hostPort = QStringLiteral("127.0.0.1:2080");
        row                 = texts::statusRow(in);
        QCOMPARE(row.text, a("Your Windows settings name only a SOCKS proxy (127.0.0.1:2080). TS Media can't use it this way, so previews and the YouTube player "
                             "don't load."));
        QCOMPARE(row.icon, texts::StatusRow::Icon::Warning);
        QVERIFY(row.useIt);
        // A Windows-proxy failure in the last 10 minutes (no site answer since).
        in.summary.kind        = windowsproxy::Summary::Kind::HttpProxy;
        in.summary.hostPort    = QStringLiteral("127.0.0.1:10809");
        in.windows.lastFailure = Failure::Unreachable;
        in.windows.atMs        = in.nowMs - 60'000;
        row                    = texts::statusRow(in);
        QCOMPARE(row.text, a("The proxy in your Windows settings didn't work for the last preview: can't reach the proxy at 127.0.0.1:10809."));
        QCOMPARE(row.icon, texts::StatusRow::Icon::Warning);
        in.windows.atMs = in.nowMs - 11 * 60'000; // older: the summary again
        QCOMPARE(texts::statusRow(in).text, a("TS Media uses the proxy in your Windows settings (127.0.0.1:10809)."));
        in.windows = WindowsStatus();
        // Direct.
        in.saved.whenOff = WhenOff::Direct;
        QCOMPARE(texts::statusRow(in).text, a("TS Media connects directly. Sites your network blocks won't load."));
        // A proxy saved while off (F17b): the suffix with host:port (our own window).
        in.saved = savedProxy("192.168.1.5", 1080);
        QCOMPARE(texts::statusRow(in).text,
                 a("TS Media uses the proxy in your Windows settings (127.0.0.1:10809). Saved chat proxy: SOCKS5 proxy 192.168.1.5:1080 (off)."));
        // The group, nothing saved.
        texts::StatusInput none;
        none.group = true;
        QVERIFY(texts::statusRow(none).text.startsWith(a("No chat proxy is set up. ")));
        // On.
        texts::StatusInput on;
        on.saved      = savedProxy("127.0.0.1", 2080, true);
        on.mode       = Mode::Custom;
        row           = texts::statusRow(on);
        QCOMPARE(row.text, a("In use: SOCKS5 proxy 127.0.0.1:2080. Not checked yet."));
        on.health = Health::State::Working;
        row       = texts::statusRow(on);
        QCOMPARE(row.text, a("In use: SOCKS5 proxy 127.0.0.1:2080 · working"));
        QCOMPARE(row.icon, texts::StatusRow::Icon::Check);
        on.lastTestWhen = QStringLiteral("today at 12:40");
        QCOMPARE(texts::statusRow(on).text, a("In use: SOCKS5 proxy 127.0.0.1:2080 · working (last test: today at 12:40)"));
        on.health      = Health::State::Down;
        on.lastFailure = Failure::Unreachable;
        row            = texts::statusRow(on);
        QCOMPARE(row.text, a("In use: SOCKS5 proxy 127.0.0.1:2080 · not working: can't reach the proxy at 127.0.0.1:2080. Previews wait until it works."));
        QCOMPARE(row.icon, texts::StatusRow::Icon::Warning);
        on.lastFailure = Failure::PasswordUnreadable;
        QCOMPARE(texts::statusRow(on).text, a("In use: SOCKS5 proxy 127.0.0.1:2080 · the saved password can't be read on this PC. Enter it again."));
        // IPv6 with brackets.
        on.saved       = savedProxy("::1", 8080, true);
        on.saved.type  = Type::Http;
        on.lastFailure = Failure::None;
        on.health      = Health::State::Unknown;
        QCOMPARE(texts::statusRow(on).text, a("In use: HTTP proxy [::1]:8080. Not checked yet."));
    }

    // ---- the Plugins menu pair, the hotkey, /tsmedia proxy (design 1.4, 9.4 test 5) -------------------------------------
    void commandsAndMenu()
    {
        Env env;
        const QVector<commands::MenuItem> items = commands::menuItems();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).id, 8);
        QCOMPARE(items.at(0).text, a("Turn on chat proxy"));
        QCOMPARE(items.at(1).id, 9);
        QCOMPARE(items.at(1).text, a("Turn off chat proxy"));
        // Exactly one is enabled: "Turn on" while off (also with nothing saved).
        commands::updateMenu();
        QCOMPARE(g_menu.value(8), 1);
        QCOMPARE(g_menu.value(9), 0);
        // The hotkey with nothing saved: the line only, never a window (F17d).
        QVERIFY(commands::handleHotkey(a("tsmedia_proxy"), fakets3::kConnection));
        QCOMPARE(env.lastLine(), a("TS Media chat No chat proxy is set up yet. Enter one in the Chat proxy window."));
        QVERIFY(env.opened.isEmpty());
        QVERIFY(!commands::handleHotkey(a("tsmedia_send"), fakets3::kConnection));
        // The menu and the command with nothing saved: the line, and the window to set one up.
        QVERIFY(commands::handleMenu(8, fakets3::kConnection));
        QCOMPARE(env.opened, QVector<Control::WindowRequest>({Control::WindowRequest::SetUp}));
        commands::runCommand(fakets3::kConnection, a("on"));
        QCOMPARE(env.opened.size(), 2);
        QVERIFY(!commands::handleMenu(5, fakets3::kConnection)); // not ours
        // Status, off, usage.
        commands::runCommand(fakets3::kConnection, QString());
        QCOMPARE(env.lastLine(), a("TS Media chat Chat proxy off. TS Media uses your Windows proxy settings."));
        commands::runCommand(fakets3::kConnection, a("off"));
        QCOMPARE(env.lastLine(), a("TS Media chat The chat proxy is already off."));
        commands::runCommand(fakets3::kConnection, a("sideways"));
        QVERIFY(env.lastLine().endsWith(a("Use /tsmedia proxy on, /tsmedia proxy off or /tsmedia proxy.")));
        QVERIFY(env.lastLine().contains(QChar(0x26A0))); // the warning prefix
        // A saved proxy: the menu turns it on (the line names host:port: it is on this PC), the hotkey off again.
        QVERIFY(env.control->save(savedProxy()));
        QVERIFY(commands::handleMenu(8, fakets3::kConnection));
        QCOMPARE(env.lastLine(), a("TS Media chat Chat proxy on: SOCKS5 proxy 127.0.0.1:2080. Link previews, the YouTube player and update checks use "
                                   "it; TeamSpeak itself doesn't."));
        QVERIFY(env.control->config().on);
        QCOMPARE(env.control->mode(), Mode::Custom);
        QCOMPARE(g_menu.value(8), 0);
        QCOMPARE(g_menu.value(9), 1);
        commands::runCommand(fakets3::kConnection, a("on"));
        QCOMPARE(env.lastLine(), a("TS Media chat The chat proxy is already on: SOCKS5 proxy 127.0.0.1:2080."));
        commands::runCommand(fakets3::kConnection, QString());
        QCOMPARE(env.lastLine(), a("TS Media chat Chat proxy on: SOCKS5 proxy 127.0.0.1:2080 (not checked yet)."));
        QVERIFY(commands::handleHotkey(a("tsmedia_proxy"), fakets3::kConnection));
        QCOMPARE(env.lastLine(), a("TS Media chat Chat proxy off. TS Media uses your Windows proxy settings again."));
        QCOMPARE(env.control->mode(), Mode::System);
        QCOMPARE(g_menu.value(8), 1);
        QCOMPARE(g_menu.value(9), 0);
        // A proxy elsewhere: its place, never its host or user name (F24).
        Config remote = savedProxy("proxy.example.net", 3128);
        remote.user   = QStringLiteral("secret-user");
        remote.type   = Type::Http;
        QVERIFY(env.control->save(remote));
        commands::runCommand(fakets3::kConnection, a("on"));
        QVERIFY(env.lastLine().contains(a("HTTP proxy on another computer")));
        for (const QString& line : env.lines()) {
            QVERIFY2(!line.contains(a("proxy.example.net")) && !line.contains(a("secret-user")) && !line.contains(a("3128")), qPrintable(line));
        }
        commands::runCommand(fakets3::kConnection, a("off"));
        // plugin.cpp: the pair right after the data saver pair, the hotkey, the command and its help line.
        const QString plugin = sourceText(a("src/plugin.cpp"));
        QVERIFY(plugin.contains(a("{MenuResumeDownloads, i18n::t(\"Resume automatic downloads on this server\")},\n"
                                  "        // 2.6 chat proxy: an enable/disable pair too (design 1.4): exactly one of them is enabled.\n"
                                  "        {MenuProxyOn, proxy::texts::menuOnText()},\n"
                                  "        {MenuProxyOff, proxy::texts::menuOffText()},")));
        QVERIFY(plugin.contains(a("{proxy::commands::kHotkey, proxy::texts::hotkeyDescription()}")));
        QVERIFY(plugin.contains(a("cmd == QLatin1String(\"proxy\") || cmd.startsWith(QLatin1String(\"proxy \"))")));
        QVERIFY(plugin.contains(a("\"[b]/tsmedia proxy[/b] on | off — turn the chat proxy for link previews, the YouTube player and update checks on or off\"")));
        QVERIFY(plugin.contains(a("proxy::commands::updateMenu();")));
        QCOMPARE(QString::fromLatin1(commands::kHotkey), a("tsmedia_proxy"));
    }

    // ---- the notices and their links (design 1.5, 9.4 test 7) -----------------------------------------------------------
    void notices()
    {
        Env env;
        QVERIFY(env.control->save(savedProxy("127.0.0.1", 2080, true)));
        QCOMPARE(env.control->mode(), Mode::Custom);
        const quint64 gen = env.control->generation();
        const auto    down = [&](Failure f, const char* host) {
            RouteReport r;
            r.failure    = f;
            r.step       = Step::Connect;
            r.host       = QString::fromLatin1(host);
            r.generation = gen;
            r.via        = Via::Bridge;
            env.control->report(r);
        };
        QVERIFY(fakets3::channelPrints().isEmpty());
        down(Failure::Unreachable, "www.youtube.com"); // decisive: Down at once
        QCOMPARE(env.control->state(), Health::State::Down);
        QCOMPARE(fakets3::channelPrints().size(), 1);
        const QString first = fakets3::channelPrints().first();
        QVERIFY(first.contains(a("The chat proxy isn't working: can't reach the proxy at 127.0.0.1:2080. Link previews wait until it works.")));
        QVERIFY(first.contains(a("[url=https://github.com/Metihttp/Teamspeak_Media_chat/blob/main/docs/FAQ.md#tsmupd-proxy-00112233aabbccdd-off]Turn off chat proxy[/url]")));
        // The same episode, and a new one of the same kind within 10 minutes: no second line.
        down(Failure::Unreachable, "i.ytimg.com");
        env.control->reportOk(gen);
        QCOMPARE(env.control->state(), Health::State::Working);
        env.clock += 5 * 60'000;
        down(Failure::Unreachable, "www.youtube.com");
        QCOMPARE(env.control->state(), Health::State::Down);
        QCOMPARE(fakets3::channelPrints().size(), 1);
        // Another kind prints at once.
        env.control->reportOk(gen);
        down(Failure::LoginRefused, "www.youtube.com");
        QCOMPARE(fakets3::channelPrints().size(), 2);
        QVERIFY(fakets3::channelPrints().last().contains(a("the proxy refused the login")));
        // Ten minutes later the first kind may print again (a new episode).
        env.control->reportOk(gen);
        env.clock += 11 * 60'000;
        down(Failure::Unreachable, "www.youtube.com");
        QCOMPARE(fakets3::channelPrints().size(), 3);
        // Review fix: ONE notice per Down episode. A long outage: Probation, its probe fails, Down again (the same
        // episode), each time more than 10 minutes later and once with another kind: no new line.
        const qint64 episodeAt = env.control->downSinceMs();
        for (int i = 0; i < 3; ++i) {
            env.clock += 11 * 60'000;
            QVERIFY(env.control->admits(false)); // the clock: Probation, and this load holds the probe's slot
            QCOMPARE(env.control->state(), Health::State::Probation);
            down(i == 1 ? Failure::NotSocks5 : Failure::Unreachable, "www.youtube.com"); // the probe fails at the route
            QCOMPARE(env.control->state(), Health::State::Down);
        }
        QCOMPARE(fakets3::channelPrints().size(), 3);
        QCOMPARE(env.control->downSinceMs(), episodeAt);
        // The password and interception notices.
        env.control->reportOk(gen);
        down(Failure::PasswordUnreadable, "www.youtube.com");
        QVERIFY(fakets3::channelPrints().last().contains(a("The chat proxy's saved password can't be read on this PC.")));
        QVERIFY(fakets3::channelPrints().last().contains(a("-set]Proxy settings[/url]")));
        // The link handler: this load's nonce only; the updater's links are left to it.
        const QString offUrl = env.control->noticeUrl(true), setUrl = env.control->noticeUrl(false);
        QVERIFY(env.control->handleNoticeLink(setUrl, false));
        QVERIFY(env.opened.isEmpty()); // not activated: only "it's ours"
        QVERIFY(!env.control->handleNoticeLink(a("https://github.com/Metihttp/Teamspeak_Media_chat/blob/main/docs/FAQ.md#tsmupd-proxy-ffffffffffffffff-off"), true));
        QVERIFY(!env.control->handleNoticeLink(a("https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest#tsmupd-0011-2.6.0"), true));
        QVERIFY(env.control->config().on);
        QVERIFY(env.control->handleNoticeLink(setUrl, true));
        QCOMPARE(env.opened, QVector<Control::WindowRequest>({Control::WindowRequest::Open}));
        // Turn off chat proxy: off, and the line says the paused previews wait for a click (S4).
        QObject::connect(env.control.get(), &Control::routeChanged, env.control.get(), [&env] { env.control->noteWaitingForClick(5); });
        QVERIFY(env.control->handleNoticeLink(offUrl, true));
        QVERIFY(!env.control->config().on);
        QCOMPARE(env.lastLine(),
                 a("TS Media chat Chat proxy off. TS Media uses your Windows proxy settings again. Previews that waited for the proxy load when you click them."));
        QObject::disconnect(env.control.get(), &Control::routeChanged, env.control.get(), nullptr);
        // Windows mode: at most one notice per kind in 10 minutes; no health, no pausing (F2).
        const int before   = fakets3::channelPrints().size();
        const auto windows = [&](Failure f) {
            RouteReport r;
            r.failure    = f;
            r.generation = env.control->generation();
            r.via        = f == Failure::SocksOnlyWindows ? Via::Refused : Via::System;
            env.control->report(r);
        };
        // Review fix: the chat rule (1.4, F24) for the Windows proxy too: a proxy on another computer is never named.
        windowsproxy::setIeConfigProviderForTests([] { return manual("proxy.corp.example:8080"); });
        windows(Failure::Unreachable);
        windows(Failure::CantReachSite);
        QCOMPARE(fakets3::channelPrints().size(), before + 1);
        QVERIFY(fakets3::channelPrints().last().contains(
            a("The proxy in your Windows settings isn't working: can't reach the proxy. Previews that need it load when you click them.")));
        QVERIFY(!fakets3::channelPrints().last().contains(a("corp.example")));
        QVERIFY(fakets3::channelPrints().last().contains(a("Proxy settings[/url]")));
        QVERIFY(env.control->admits(false));
        windows(Failure::SocksOnlyWindows);
        QCOMPARE(fakets3::channelPrints().size(), before + 2);
        QVERIFY(fakets3::channelPrints().last().contains(a("Your Windows settings name only a SOCKS proxy")));
        env.clock += 11 * 60'000;
        // ... and one on this PC is (its address helps, and it says nothing about the user).
        windowsproxy::setIeConfigProviderForTests([] { return manual("127.0.0.1:10809"); });
        windows(Failure::Unreachable);
        QCOMPARE(fakets3::channelPrints().size(), before + 3);
        QVERIFY(fakets3::channelPrints().last().contains(a("can't reach the proxy at 127.0.0.1:10809")));
        QCOMPARE(texts::windowChatShortText(Failure::Unreachable, a("[::1]:8080")), a("can't reach the proxy at [::1]:8080"));
        QCOMPARE(texts::windowChatShortText(Failure::Unreachable, a("192.168.1.2:3128")), a("can't reach the proxy"));
        QCOMPARE(texts::windowChatShortText(Failure::Unreachable, QString()), a("can't reach the proxy"));
        // Not route-level: nothing.
        windows(Failure::SiteRefused);
        QCOMPARE(fakets3::channelPrints().size(), before + 3);
    }

    // ---- the Chat proxy window (design 1.3, 9.4 test 1) -----------------------------------------------------------------
    void windowTextsAndForm()
    {
        Config bridgeOnly;
        bridgeOnly.bridgePort = 47123; // TS Media's own port of an earlier session (nothing else saved)
        Env             env(&bridgeOnly);
        QCOMPARE(env.control->config().bridgePort, quint16(47123));
        proxy::ui::ProxyWindow w(env.control.get());
        QCOMPARE(w.objectName(), a("tsmediaProxyDialog"));
        QCOMPARE(w.windowTitle(), a("Chat proxy"));
        QCOMPARE(w.useBox()->text(), a("&Use a chat proxy for link previews, the YouTube player and update checks"));
        QCOMPARE(w.typeBox()->count(), 2);
        QCOMPARE(w.typeBox()->itemText(0), a("SOCKS5"));
        QCOMPARE(w.typeBox()->itemText(1), a("HTTP"));
        QCOMPARE(w.typeBox()->accessibleName(), a("Proxy type"));
        QCOMPARE(w.findButton()->text(), a("&Find my VPN app"));
        QCOMPARE(w.pasteButton()->text(), a("&Paste from clipboard"));
        QCOMPARE(w.testButton()->text(), a("&Test"));
        QCOMPARE(w.systemRadio()->text(), a("Use &Windows proxy settings"));
        QCOMPARE(w.directRadio()->text(), a("No proxy (&direct)"));
        QCOMPARE(w.useItButton()->text(), a("Use &it as the chat proxy"));
        QStringList labels;
        for (QLabel* l : w.findChildren<QLabel*>())
            labels << l->text();
        for (const char* t : {"T&ype:", "Ser&ver:", "P&ort:", "Us&er name:", "Pa&ssword:", "When the chat proxy is off:"})
            QVERIFY2(labels.contains(a(t)), t);
        QVERIFY(labels.contains(a("For TS Media's own web traffic: link previews, the YouTube player and update checks. TeamSpeak itself (servers, voice, "
                                  "chat, file transfers) never uses it, so servers in your country stay reachable while previews go through a VPN app.")));
        QVERIFY(labels.contains(a("Your VPN app shows its local port: v2rayN 10808, Throne and Nekoray 2080, Clash Verge Rev 7897, Hiddify 12334 (SOCKS5 and "
                                  "HTTP both work there). Find my VPN app tries these on this PC. The proxy looks up website names itself, so filtered "
                                  "addresses on your network don't matter.")));
        QVERIFY(labels.contains(a("The proxy Windows is set to use, if any. A VPN app's \"system proxy\" option sets one.")));
        QCOMPARE(w.statusLabel()->textFormat(), Qt::PlainText);
        QCOMPARE(w.resultLabel()->textFormat(), Qt::PlainText);
        QCOMPARE(w.serverField()->placeholderText(), a("127.0.0.1"));
        QVERIFY(w.portField()->placeholderText().isEmpty());
        QCOMPARE(w.userField()->placeholderText(), a("optional"));
        QCOMPARE(w.passwordField()->placeholderText(), a("optional"));
        QCOMPARE(w.passwordField()->echoMode(), QLineEdit::Password);
        QCOMPARE(w.findButton()->accessibleDescription(), a("Looks for a VPN app's proxy on this PC at its usual ports. Nothing leaves this PC."));
        QCOMPARE(w.testButton()->accessibleDescription(), a("Sends one request to www.youtube.com through this proxy."));
        QCOMPARE(w.pasteButton()->accessibleDescription(), a("Fills in the fields from a proxy link you copied, such as socks5://127.0.0.1:10808."));
        QCOMPARE(w.portField()->accessibleName(), a("Port"));
        for (QPushButton* b : {w.findButton(), w.pasteButton(), w.testButton()})
            QVERIFY(!b->autoDefault());
        // Nothing saved: everything off, the Windows radio, every field enabled (also while off, F17a).
        QVERIFY(!w.useBox()->isChecked());
        QVERIFY(w.systemRadio()->isChecked());
        for (QWidget* f : {static_cast<QWidget*>(w.typeBox()), static_cast<QWidget*>(w.serverField()), static_cast<QWidget*>(w.portField()),
                           static_cast<QWidget*>(w.userField()), static_cast<QWidget*>(w.passwordField()), static_cast<QWidget*>(w.testButton())})
            QVERIFY(f->isEnabled());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        // Checking the box with an empty server: 127.0.0.1 and the port gets the focus (no ":" jump, F19).
        w.useBox()->click();
        QCOMPARE(w.serverField()->text(), a("127.0.0.1"));
        QCOMPARE(w.focusWidget(), static_cast<QWidget*>(w.portField()));
        // OK with no port: the message under the port, with the warning icon; nothing saved.
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.portField()), a("Enter the port, a number from 1 to 65535."));
        QVERIFY(w.errorIconShown(w.portField()));
        QCOMPARE(w.portField()->accessibleDescription(), a("Enter the port, a number from 1 to 65535."));
        QVERIFY(!env.control->config().saved());
        // A server typed as host:port is read when the field is left (exactly one ':').
        w.serverField()->setText(a("127.0.0.1:10808"));
        emit w.serverField()->editingFinished();
        QCOMPARE(w.serverField()->text(), a("127.0.0.1"));
        QCOMPARE(w.portField()->text(), a("10808"));
        // An IPv6 host stays a host (two colons: no port).
        w.serverField()->setText(a("::1"));
        emit w.serverField()->editingFinished();
        QCOMPARE(w.serverField()->text(), a("::1"));
        QVERIFY(w.errorOf(w.serverField()).isEmpty());
        // A link the paste parser refuses: its message under the field.
        w.serverField()->setText(a("socks4://127.0.0.1:1080"));
        emit w.serverField()->editingFinished();
        QCOMPARE(w.errorOf(w.serverField()), a("TS Media needs SOCKS5. Use the proxy's SOCKS5 or HTTP port."));
        w.serverField()->setText(a("http://proxy.example.net"));
        emit w.serverField()->editingFinished();
        QCOMPARE(w.errorOf(w.serverField()), linkMessage(parseProxyLink(a("http://proxy.example.net")).result));
        QVERIFY(!w.errorOf(w.serverField()).isEmpty());
        w.serverField()->setText(a("a b"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.serverField()), a("Enter only the proxy's address, like 127.0.0.1 or proxy.example.net, without http:// or a port."));
        QVERIFY(w.errorIconShown(w.serverField()));
        w.serverField()->setText(a("224.0.0.1"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.serverField()), a("Enter only the proxy's address, like 127.0.0.1 or proxy.example.net, without http:// or a port."));
        w.serverField()->clear();
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.serverField()), a("Enter the proxy's address, for example 127.0.0.1."));
        // The bridge's own port on this PC (S17).
        w.serverField()->setText(a("127.0.0.1"));
        w.portField()->setText(a("47123"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.portField()), a("This is TS Media's own local port. Enter your VPN app's port."));
        QVERIFY(w.errorIconShown(w.portField()));

        w.portField()->setText(a("70000"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.portField()), a("Enter the port, a number from 1 to 65535."));
        w.portField()->setText(a("2080"));
        // Logins: ':' in an HTTP user name, SOCKS5 needs a password, the 255-byte limits.
        w.typeBox()->setCurrentIndex(1);
        w.userField()->setText(a("a:b"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.userField()), a("An HTTP proxy's user name can't contain \":\"."));
        QVERIFY(w.errorIconShown(w.userField()));
        w.userField()->setText(QString(256, QLatin1Char('u')));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.userField()), a("The user name is too long (at most 255 bytes)."));
        w.typeBox()->setCurrentIndex(0);
        w.userField()->setText(a("tester"));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.passwordField()), a("SOCKS5 needs a password with the user name."));
        w.passwordField()->setText(QString(256, QLatin1Char('p')));
        QVERIFY(!w.accept_());
        QCOMPARE(w.errorOf(w.passwordField()), a("The password is too long (at most 255 bytes)."));
        // The LAN hint: a user name for a proxy on another computer.
        QVERIFY(w.lanHint()->isHidden());
        w.serverField()->setText(a("192.168.1.5"));
        w.userField()->setText(a("tester"));
        w.userField()->textEdited(w.userField()->text());
        QVERIFY(!w.lanHint()->isHidden());
        w.serverField()->setText(a("127.0.0.1"));
        w.userField()->textEdited(w.userField()->text());
        QVERIFY(w.lanHint()->isHidden());
        // OK: saved, the password sealed for this endpoint, the field cleared.
        w.passwordField()->setText(a("secret-pass"));
        w.directRadio()->click();
        QVERIFY(w.accept_());
        const Config saved = env.control->config();
        QVERIFY(saved.on);
        QCOMPARE(saved.type, Type::Socks5);
        QCOMPARE(saved.host, a("127.0.0.1"));
        QCOMPARE(saved.port, quint16(2080));
        QCOMPARE(saved.user, a("tester"));
        QCOMPARE(saved.whenOff, WhenOff::Direct);
        QVERIFY(saved.passwordBlob.startsWith("dpapi1:"));
        protect::SecretBytes back;
        QCOMPARE(protect::open(saved.passwordBlob, saved.endpoint(), &back), protect::OpenResult::Ok);
        QCOMPARE(QByteArray(back.data(), back.size()), QByteArray("secret-pass"));
        QVERIFY(w.passwordField()->text().isEmpty());
        // The settings.ini never holds the plain password.
        QFile ini(env.dir.path() + a("/settings.ini"));
        QVERIFY(ini.open(QIODevice::ReadOnly));
        QVERIFY(!ini.readAll().contains("secret-pass"));
        QVERIFY2(env.lastLine().startsWith(a("TS Media chat Chat proxy on: SOCKS5 proxy 127.0.0.1:2080.")), qPrintable(env.lines().join(a(" | "))));
    }

    void windowPasswordFollowsEndpoint()
    {
        // S7: the saved password follows its endpoint.
        Config c = savedProxy("127.0.0.1", 2080, false);
        c.user   = QStringLiteral("tester");
        QVERIFY(protect::seal(QStringLiteral("secret-pass"), c.endpoint(), &c.passwordBlob));
        Env env(&c);
        QCOMPARE(env.control->config().passwordBlob, c.passwordBlob);
        {
            proxy::ui::ProxyWindow w(env.control.get());
            QCOMPARE(w.passwordField()->placeholderText(), a("saved (type to replace)"));
            QVERIFY(w.passwordField()->text().isEmpty()); // never decrypted into the field
            QVERIFY(w.droppedHint()->isHidden());
            // Another server: the placeholder and the hint say it goes; back: both restored.
            w.serverField()->setText(a("127.0.0.2"));
            w.serverField()->textEdited(w.serverField()->text());
            QCOMPARE(w.passwordField()->placeholderText(), a("optional"));
            QVERIFY(!w.droppedHint()->isHidden());
            QCOMPARE(w.droppedHint()->text(), a("The saved password will be removed because the server changed. Type it again if this proxy needs it."));
            w.serverField()->setText(a("127.0.0.1"));
            w.serverField()->textEdited(w.serverField()->text());
            QCOMPARE(w.passwordField()->placeholderText(), a("saved (type to replace)"));
            QVERIFY(w.droppedHint()->isHidden());
            // The same for the type, the port and the user name.
            w.portField()->setText(a("2081"));
            w.portField()->textEdited(w.portField()->text());
            QVERIFY(!w.droppedHint()->isHidden());
            w.portField()->setText(a("2080"));
            w.portField()->textEdited(w.portField()->text());
            w.typeBox()->setCurrentIndex(1);
            QVERIFY(!w.droppedHint()->isHidden());
            w.typeBox()->setCurrentIndex(0);
            QVERIFY(w.droppedHint()->isHidden());
            // Untouched: OK keeps the blob (no seal, no new blob).
            QVERIFY(w.accept_());
            QCOMPARE(env.control->config().passwordBlob, c.passwordBlob);
        }
        {
            // A changed server without a typed password: the blob is dropped (and SOCKS5 with a user then needs one).
            proxy::ui::ProxyWindow w(env.control.get());
            w.serverField()->setText(a("127.0.0.2"));
            w.userField()->clear();
            QVERIFY(w.accept_());
            QVERIFY(env.control->config().passwordBlob.isEmpty());
            QCOMPARE(env.control->config().host, a("127.0.0.2"));
        }
        {
            // A typed password replaces it, sealed for the new endpoint.
            proxy::ui::ProxyWindow w(env.control.get());
            w.userField()->setText(a("other"));
            w.passwordField()->setText(a("new-pass"));
            QVERIFY(w.accept_());
            const Config now = env.control->config();
            protect::SecretBytes back;
            QCOMPARE(protect::open(now.passwordBlob, now.endpoint(), &back), protect::OpenResult::Ok);
            QCOMPARE(QByteArray(back.data(), back.size()), QByteArray("new-pass"));
        }
        {
            // Cancel saves nothing (and clears the typed password).
            proxy::ui::ProxyWindow w(env.control.get());
            const Config before = env.control->config();
            w.serverField()->setText(a("10.0.0.9"));
            w.passwordField()->setText(a("typed"));
            w.useBox()->setChecked(!before.on);
            w.reject();
            QCOMPARE(env.control->config().host, before.host);
            QCOMPARE(env.control->config().on, before.on);
            QCOMPARE(env.control->config().passwordBlob, before.passwordBlob);
            QVERIFY(w.passwordField()->text().isEmpty());
        }
    }

    void windowJobs()
    {
        Env                           env;
        int                           tests = 0, finds = 0, cancels = 0;
        Config                        testedForm;
        QByteArray                    testedBlob, testedTyped;
        std::function<void(const TestResult&)>       testDone;
        std::function<void(const QVector<quint16>&)> findDone;
        proxy::ui::ProxyWindow::Jobs         jobs;
        jobs.test = [&](const Config& form, protect::SecretBytes typed, const QByteArray& blob, std::function<void(const TestResult&)> done) {
            ++tests;
            testedForm  = form;
            testedBlob  = blob;
            testedTyped = QByteArray(typed.data(), typed.size());
            testDone    = done;
            return true;
        };
        jobs.find = [&](quint16, std::function<void(const QVector<quint16>&)> done) {
            ++finds;
            findDone = done;
            return true;
        };
        jobs.cancel = [&] { ++cancels; };
        proxy::ui::ProxyWindow w(env.control.get(), nullptr, jobs);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.resultRow()->isHidden());
        // Test needs a server and a port.
        w.testButton()->click();
        QCOMPARE(tests, 0);
        QCOMPARE(w.errorOf(w.serverField()), a("Enter the proxy's address, for example 127.0.0.1."));
        w.serverField()->setText(a("127.0.0.1"));
        w.portField()->setText(a("2080"));
        w.passwordField()->setText(a("typed-pass"));
        w.testButton()->click();
        QCOMPARE(tests, 1);
        QCOMPARE(testedForm.host, a("127.0.0.1"));
        QCOMPARE(testedForm.port, quint16(2080));
        QCOMPARE(testedTyped, QByteArray("typed-pass"));
        QVERIFY(testedBlob.isEmpty());
        QCOMPARE(w.testButton()->text(), a("Testing…"));
        QVERIFY(!w.testButton()->isEnabled());
        QVERIFY(!w.findButton()->isEnabled());
        QCOMPARE(w.resultLabel()->text(), a("Testing www.youtube.com through the proxy…"));
        QVERIFY(!w.resultRow()->isHidden());
        TestResult works;
        works.outcome = TestResult::Outcome::Works;
        works.ms      = 612;
        testDone(works);
        QCOMPARE(w.resultLabel()->text(), a("Works · 612 ms"));
        QCOMPARE(w.resultIcon(), a("check"));
        QCOMPARE(w.testButton()->text(), a("&Test"));
        QVERIFY(w.testButton()->isEnabled());
        QVERIFY(env.control->lastTest().ran && env.control->lastTest().ok);
        // Each failure kind's text (1.3.5), with the warning icon.
        const auto failWith = [&](TestResult r, const QString& want) {
            w.testButton()->click();
            testDone(r);
            QCOMPARE(w.resultLabel()->text(), want);
            QCOMPARE(w.resultIcon(), a("warning"));
        };
        TestResult r;
        r.outcome = TestResult::Outcome::Failed;
        r.failure = Failure::Unreachable;
        failWith(r, a("Can't reach the proxy at 127.0.0.1:2080. Is your VPN app running, and is 2080 its port?"));
        r.failure = Failure::NoNameConnect;
        failWith(r, a("The proxy can't connect to websites by name. TS Media needs a proxy that looks up names itself."));
        r.failure = Failure::ProxyNameFiltered;
        failWith(r, a("The proxy's name leads to a filtered address. Use the proxy's IP address, or 127.0.0.1 for a VPN app on this PC."));
        r.failure = Failure::LoginRefused;
        failWith(r, a("The proxy refused the login. Check the user name and password."));
        TestResult unexpected;
        unexpected.outcome = TestResult::Outcome::Unexpected;
        unexpected.status  = 418;
        failWith(unexpected, a("Unexpected answer from the test site (HTTP 418)."));
        // An edit while it runs cancels it: its result is dropped.
        w.testButton()->click();
        QCOMPARE(w.testButton()->text(), a("Testing…"));
        w.portField()->setText(a("2081"));
        w.portField()->textEdited(w.portField()->text());
        QVERIFY(cancels >= 1);
        QCOMPARE(w.testButton()->text(), a("&Test"));
        testDone(works); // too late
        QVERIFY(w.resultRow()->isHidden());
        // Find my VPN app: busy, Test disabled, then the form from the first port that answered (user name emptied).
        w.userField()->setText(a("someone"));
        w.findButton()->click();
        QCOMPARE(finds, 1);
        QCOMPARE(w.findButton()->text(), a("Looking…"));
        QVERIFY(!w.findButton()->isEnabled());
        QVERIFY(!w.testButton()->isEnabled());
        w.typeBox()->setCurrentIndex(1);
        QVERIFY(w.findButton()->isEnabled()); // an edit canceled it
        w.findButton()->click();
        findDone({2080, 7897});
        QCOMPARE(w.typeBox()->currentIndex(), 0);
        QCOMPARE(w.serverField()->text(), a("127.0.0.1"));
        QCOMPARE(w.portField()->text(), a("2080"));
        QVERIFY(w.userField()->text().isEmpty()); // pasteClearsUser for Find too
        QCOMPARE(w.resultLabel()->text(), a("Found proxies on this PC at ports 2080, 7897. TS Media chose 2080. Press Test, then OK."));
        QCOMPARE(w.resultIcon(), QString());
        w.findButton()->click();
        findDone({});
        QCOMPARE(w.resultLabel()->text(),
                 a("No VPN app answered at its usual ports. Open your VPN app's settings and look for its local SOCKS or HTTP port."));
        QCOMPARE(w.resultIcon(), a("warning"));
        // Paste from clipboard: a link fills the fields (the user name from the link, empty without one, S7).
        QClipboard* clip = QGuiApplication::clipboard();
        clip->setText(a("socks5://u%40x:p%3Ay@127.0.0.1:10808"));
        w.pasteButton()->click();
        QCOMPARE(w.portField()->text(), a("10808"));
        QCOMPARE(w.userField()->text(), a("u@x"));
        QCOMPARE(w.passwordField()->text(), a("p:y"));
        QCOMPARE(w.resultLabel()->text(), a("Filled in from the clipboard. Press Test, then OK."));
        clip->setText(a("http://127.0.0.1:10809"));
        w.pasteButton()->click();
        QCOMPARE(w.typeBox()->currentIndex(), 1);
        QVERIFY(w.userField()->text().isEmpty());
        clip->setText(a("vless://abc@example.com:443"));
        w.pasteButton()->click();
        QCOMPARE(w.resultLabel()->text(), a("This is a server link for a VPN app. Add it to your VPN app, then enter the app's local port here."));
        QCOMPARE(w.resultIcon(), a("warning"));
        clip->setText(QString());
        w.pasteButton()->click();
        QCOMPARE(w.resultLabel()->text(), a("The clipboard has no text."));
        clip->setText(a("tg://socks?server=1.2.3.4&port=1080"));
        w.pasteButton()->click();
        QCOMPARE(w.resultLabel()->text(), a("This is a Telegram proxy link. Use your VPN app's local port instead."));
        // Review fix: a bare word or host is no proxy link (1.3.6): refused, the fields untouched.
        const QString serverBefore = w.serverField()->text(), portBefore = w.portField()->text();
        w.userField()->setText(a("someone"));
        for (const char* word : {"v2rayN", "Proxy", "proxy.example.net", "::1"}) {
            clip->setText(QString::fromLatin1(word));
            w.pasteButton()->click();
            QCOMPARE(w.resultLabel()->text(), a("This isn't a proxy link. Copy a link like socks5://127.0.0.1:10808."));
            QCOMPARE(w.resultIcon(), a("warning"));
            QCOMPARE(w.serverField()->text(), serverBefore);
            QCOMPARE(w.portField()->text(), portBefore);
            QCOMPARE(w.userField()->text(), a("someone"));
        }
        w.userField()->clear();
        // Test with the saved values: the saved blob, not a typed password; a passed test marks the route working.
        Config s = savedProxy("127.0.0.1", 2080, true);
        s.user   = QStringLiteral("tester");
        QVERIFY(protect::seal(QStringLiteral("pw"), s.endpoint(), &s.passwordBlob));
        QVERIFY(env.control->save(s));
        proxy::ui::ProxyWindow again(env.control.get(), nullptr, jobs);
        again.testButton()->click();
        QCOMPARE(testedBlob, s.passwordBlob);
        QVERIFY(testedTyped.isEmpty());
        QCOMPARE(env.control->state(), Health::State::Unknown);
        testDone(works);
        QCOMPARE(env.control->state(), Health::State::Working);
        // ... and with another server: no saved password goes there (and SOCKS5 with a user name then needs one typed).
        again.serverField()->setText(a("127.0.0.3"));
        again.testButton()->click();
        QCOMPARE(again.errorOf(again.passwordField()), a("SOCKS5 needs a password with the user name."));
        again.userField()->clear();
        again.testButton()->click();
        QVERIFY(testedBlob.isEmpty());
        testDone(works);
        clip->clear();
    }

    void windowStatusUseItAndRouteChanges()
    {
        windowsproxy::setIeConfigProviderForTests([] { return manual("socks=127.0.0.1:2080"); });
        Env env;
        proxy::ui::ProxyWindow w(env.control.get());
        // Windows names only a SOCKS proxy: the warning, and Use it.
        QVERIFY(w.statusLabel()->text().startsWith(a("Your Windows settings name only a SOCKS proxy (127.0.0.1:2080).")));
        QCOMPARE(w.statusIcon(), a("warning"));
        QVERIFY(!w.useItButton()->isHidden());
        w.useItButton()->click();
        QVERIFY(w.useBox()->isChecked());
        QCOMPARE(w.typeBox()->currentIndex(), 0);
        QCOMPARE(w.serverField()->text(), a("127.0.0.1"));
        QCOMPARE(w.portField()->text(), a("2080"));
        QVERIFY(w.userField()->text().isEmpty());
        QVERIFY(!env.control->config().saved()); // nothing saved until OK
        // A route change elsewhere: the checkbox follows only while untouched (here it was set by Use it: touched).
        windowsproxy::setIeConfigProviderForTests([] { return manual(""); });
        QVERIFY(env.control->save(savedProxy("127.0.0.1", 2080, false)));
        QVERIFY(w.useBox()->isChecked());
        proxy::ui::ProxyWindow fresh(env.control.get());
        QVERIFY(!fresh.useBox()->isChecked());
        QVERIFY(fresh.statusLabel()->text().endsWith(a("Saved chat proxy: SOCKS5 proxy 127.0.0.1:2080 (off).")));
        env.control->turnOn(Control::Origin::Menu, fakets3::kConnection);
        QVERIFY(fresh.useBox()->isChecked());
        QCOMPARE(fresh.statusLabel()->text(), a("In use: SOCKS5 proxy 127.0.0.1:2080. Not checked yet."));
        fresh.directRadio()->click(); // touched
        env.control->turnOff(Control::Origin::Menu, fakets3::kConnection);
        QVERIFY(!fresh.useBox()->isChecked());
        QVERIFY(fresh.directRadio()->isChecked()); // the touched radio stays
        // The endpoint fields are never overwritten by a route change.
        fresh.serverField()->setText(a("10.1.2.3"));
        env.control->turnOn(Control::Origin::Menu, fakets3::kConnection);
        QCOMPARE(fresh.serverField()->text(), a("10.1.2.3"));
        env.control->turnOff(Control::Origin::Menu, fakets3::kConnection);
        // showProxyWindow: one window; a second open raises it.
        QWidget* one = proxy::ui::showProxyWindow(nullptr, proxy::ui::Request::SetUp, env.control.get());
        QVERIFY(one);
        QCOMPARE(proxy::ui::showProxyWindow(nullptr, proxy::ui::Request::Open, env.control.get()), one);
        auto* setUp = qobject_cast<proxy::ui::ProxyWindow*>(one);
        QVERIFY(setUp && setUp->useBox()->isChecked());
        QVERIFY(setUp->testAttribute(Qt::WA_DeleteOnClose));
        delete one;
    }

    void windowAccessibility()
    {
        Env             env;
        proxy::ui::ProxyWindow w(env.control.get());
        // The window's own access keys: exactly u y v o e s f p t w d i, each once (design 1.3.2). OK and Cancel are the
        // style's own texts.
        const QDialogButtonBox* buttons = w.findChild<QDialogButtonBox*>();
        QStringList             texts;
        for (QAbstractButton* b : w.findChildren<QAbstractButton*>()) {
            if (!buttons->buttons().contains(b))
                texts << b->text();
        }
        for (QLabel* l : w.findChildren<QLabel*>()) {
            if (l->buddy())
                texts << l->text();
        }
        const QList<QChar> keys = keysOf(texts);
        QCOMPARE(keys.size(), QSet<QChar>(keys.begin(), keys.end()).size());
        QCOMPARE(QSet<QChar>(keys.begin(), keys.end()), QSet<QChar>({'u', 'y', 'v', 'o', 'e', 's', 'f', 'p', 't', 'w', 'd', 'i'}));
        // Tab order: checkbox, type, server, port, user, password, Find, Paste, Test, the radios, Use it, OK, Cancel.
        const QDialogButtonBox* box = w.findChild<QDialogButtonBox*>();
        QVERIFY(box);
        QWidget* const chain[] = {w.useBox(), w.typeBox(), w.serverField(), w.portField(), w.userField(), w.passwordField(), w.findButton(),
                                  w.pasteButton(), w.testButton(), w.systemRadio(), w.directRadio(), w.useItButton(),
                                  box->button(QDialogButtonBox::Ok), box->button(QDialogButtonBox::Cancel)};
        // The focus chain from the checkbox, the containers between them skipped.
        QSet<QWidget*> inChain;
        for (QWidget* c : chain)
            inChain.insert(c);
        QVector<QWidget*> order;
        QWidget*          at = chain[0];
        for (int step = 0; step < 200 && order.size() < int(sizeof(chain) / sizeof(chain[0])); ++step) {
            if (inChain.contains(at) && !order.contains(at))
                order << at;
            at = at->nextInFocusChain();
        }
        QCOMPARE(order.size(), int(sizeof(chain) / sizeof(chain[0])));
        for (int i = 0; i < order.size(); ++i)
            QVERIFY2(order.at(i) == chain[i], qPrintable(QString::number(i)));
        // Accessible names and descriptions.
        for (QWidget* f : {static_cast<QWidget*>(w.useBox()), static_cast<QWidget*>(w.serverField()), static_cast<QWidget*>(w.userField()),
                           static_cast<QWidget*>(w.passwordField()), static_cast<QWidget*>(w.systemRadio()), static_cast<QWidget*>(w.directRadio())})
            QVERIFY(!f->accessibleDescription().isEmpty());
        // The ok, error and hint colours read at 4.5:1 on light and dark palettes (1.3.8).
        for (const bool dark : {false, true}) {
            QPalette pal;
            pal.setColor(QPalette::Window, dark ? QColor(0x2b, 0x2d, 0x31) : QColor(0xf0, 0xf0, 0xf0)); // TeamSpeak's light skin
            pal.setColor(QPalette::WindowText, dark ? QColor(0xe6, 0xe6, 0xe6) : QColor(0x1e, 0x1f, 0x22));
            const settingstheme::Colors c = settingstheme::roleColors(pal);
            QCOMPARE(c.dark, dark);
            for (const QColor& color : {c.ok, c.error, c.hint})
                QVERIFY2(::ui::contrastRatio(color, pal.color(QPalette::Window)) >= 4.5, qPrintable(color.name()));
            QCOMPARE(c.ok, dark ? QColor(0x57, 0xd3, 0x8c) : QColor(0x15, 0x70, 0x32)); // live test fix: #1a7f37 read 4.46:1 on #f0f0f0
        }
        // A background the green can't read on: the text colour instead.
        QPalette grey;
        grey.setColor(QPalette::Window, QColor(0x60, 0x80, 0x60));
        grey.setColor(QPalette::WindowText, QColor(0xff, 0xff, 0xff));
        QCOMPARE(settingstheme::roleColors(grey).ok, QColor(0xff, 0xff, 0xff));
        QVERIFY(!settingstheme::checkMark(QColor(0x1a, 0x7f, 0x37), 16, 2.0).isNull());
        QVERIFY(settingstheme::roleSheet(a("tsmediaProxyDialog"), settingstheme::roleColors(grey)).startsWith(a("#tsmediaProxyDialog QLabel[role=\"hint\"]")));
    }

    // Review fix: the painted check mark follows the palette TeamSpeak's skin gives the window and the Settings group after
    // they were built (a dark skin: the dark ok colour #57d38c, not the light #1a7f37 of before the polish).
    void checkMarkFollowsSkin()
    {
        const Config on = savedProxy("127.0.0.1", 2080, true);
        Env          env(&on);
        env.control->reportOk(env.control->generation());
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(0x2b, 0x2d, 0x31));
        dark.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
        const QColor darkOk(0x57, 0xd3, 0x8c);
        const auto   imageOf = [](QLabel* label) { return label->pixmap(Qt::ReturnByValue).toImage(); };
        proxy::ui::ProxyWindow w(env.control.get());
        QCOMPARE(w.statusIcon(), a("check"));
        const int size = w.style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, &w);
        QCOMPARE(imageOf(w.statusIconLabel()), settingstheme::checkMark(settingstheme::roleColors(w.palette()).ok, size, w.devicePixelRatioF()).toImage());
        w.setPalette(dark);
        QCOMPARE(settingstheme::roleColors(w.palette()).ok, darkOk);
        QCOMPARE(imageOf(w.statusIconLabel()), settingstheme::checkMark(darkOk, size, w.devicePixelRatioF()).toImage());
        proxy::ui::ProxyGroup group(nullptr, env.control.get());
        QCOMPARE(group.statusIcon(), a("check"));
        group.setPalette(dark);
        QCOMPARE(imageOf(group.statusIconLabel()), settingstheme::checkMark(darkOk, size, group.devicePixelRatioF()).toImage());
    }

    // Live test fix: TeamSpeak's skin is an application style sheet, so the window gets a StyleChange while it is still
    // being built (setWindowFlags re-parents it under the styled main window), before its status and result icons exist.
    // The live run crashed there (QLabel::setPixmap on a null label) on every open: a card's Set up proxy…, the menu, Settings.
    void windowBuiltUnderStyleSheet()
    {
        const Config  on = savedProxy("127.0.0.1", 2080, true);
        Env           env(&on);
        env.control->reportOk(env.control->generation());
        const QString before = qApp->styleSheet();
        qApp->setStyleSheet(a("QWidget { color: #e6e6e6; } QDialog { background: #2b2d31; }"));
        {
            QWidget main;
            main.setObjectName(a("tsmediaTestMainWindow"));
            main.ensurePolished();
            QVERIFY(main.testAttribute(Qt::WA_StyleSheet));
            proxy::ui::ProxyWindow w(env.control.get(), &main);
            QCOMPARE(w.statusIcon(), a("check"));
            QVERIFY(!w.statusIconLabel()->pixmap(Qt::ReturnByValue).isNull());
            w.show();
            QVERIFY(!w.statusIconLabel()->pixmap(Qt::ReturnByValue).isNull());
            QWidget* shown = proxy::ui::showProxyWindow(&main, proxy::ui::Request::SetUp, env.control.get()); // the card's path
            QVERIFY(shown && shown != &w);
            delete shown;
        }
        qApp->setStyleSheet(before);
    }

    // Live test fix: the result row (and an error under a field) made the form taller while the window kept its height, so
    // the word-wrapped ports hint slid under the Find / Paste / Test buttons. The window now grows: nothing overlaps.
    void windowGrowsForRows()
    {
        Env                                          env;
        std::function<void(const QVector<quint16>&)> findDone;
        std::function<void(const TestResult&)>       testDone;
        proxy::ui::ProxyWindow::Jobs                 jobs;
        jobs.test = [&](const Config&, protect::SecretBytes, const QByteArray&, std::function<void(const TestResult&)> done) {
            testDone = done;
            return true;
        };
        jobs.find = [&](quint16, std::function<void(const QVector<quint16>&)> done) {
            findDone = done;
            return true;
        };
        jobs.cancel = [] {};
        proxy::ui::ProxyWindow w(env.control.get(), nullptr, jobs);
        w.prepare(proxy::ui::Request::SetUp);
        w.resize(440, w.sizeHint().height()); // the width TeamSpeak's skin gave it live
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(20);
        const auto bottomOf = [&w](QWidget* x) { return x->mapTo(&w, QPoint(0, x->height())).y(); };
        const auto topOf    = [&w](QWidget* x) { return x->mapTo(&w, QPoint(0, 0)).y(); };
        const auto clear    = [&] {
            QTest::qWait(20); // the layout request
            QVERIFY2(bottomOf(w.portsHint()) <= topOf(w.findButton()),
                     qPrintable(QString::fromLatin1("hint bottom %1, buttons top %2, window %3").arg(bottomOf(w.portsHint())).arg(topOf(w.findButton())).arg(w.height())));
            QVERIFY(bottomOf(w.resultRow()) <= w.height());
            QVERIFY(w.portsHint()->height() >= w.portsHint()->heightForWidth(w.portsHint()->width()));
        };
        clear();
        const int before = w.height();
        w.findButton()->click();
        findDone({2080});
        QCOMPARE(w.resultLabel()->text(), a("Found a proxy on this PC at port 2080. Press Test, then OK."));
        clear();
        QVERIFY(w.height() > before);
        w.testButton()->click();
        TestResult r;
        r.outcome = TestResult::Outcome::Failed;
        r.failure = Failure::Unreachable;
        testDone(r);
        clear();
        w.portField()->setText(a("99999"));
        QVERIFY(!w.accept_());
        QVERIFY(!w.errorOf(w.portField()).isEmpty());
        clear();
    }

    // Live test fix: under TeamSpeak's dark skin the group box's own palette had the skin's light text on a light Window (the
    // skin paints backgrounds by style sheet), so the check mark fell back to the text colour (white) while the status text,
    // coloured from the Settings dialog's palette, was green. The group now takes the dialog's palette for both.
    void groupCheckMarkUsesDialogPalette()
    {
        const Config on = savedProxy("127.0.0.1", 2080, true);
        Env          env(&on);
        env.control->reportOk(env.control->generation());
        QPalette dialogPalette;
        dialogPalette.setColor(QPalette::Window, QColor(0x36, 0x39, 0x3e));
        dialogPalette.setColor(QPalette::WindowText, QColor(0xdc, 0xdd, 0xde));
        QPalette mixed; // what the skin left on the group box itself
        mixed.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
        mixed.setColor(QPalette::WindowText, QColor(0xdc, 0xdd, 0xde));
        QWidget dialog;
        dialog.setPalette(dialogPalette);
        auto* group = new proxy::ui::ProxyGroup(&dialog, env.control.get());
        group->setPalette(mixed);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(group->statusIcon(), a("check"));
        const int    size = group->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, group);
        const QColor darkOk(0x57, 0xd3, 0x8c);
        QCOMPARE(group->statusIconLabel()->pixmap(Qt::ReturnByValue).toImage(), settingstheme::checkMark(darkOk, size, group->devicePixelRatioF()).toImage());
        QCOMPARE(group->statusLabel()->palette().color(QPalette::WindowText), darkOk);
    }

    // ---- the Settings group (design 1.3.10, 9.4 test 3) and the Privacy & updates tab (test 4) ---------------------------
    void proxyGroup()
    {
        Env            env;
        proxy::ui::ProxyGroup group(nullptr, env.control.get());
        QCOMPARE(group.objectName(), a("tsmediaProxyGroup"));
        QCOMPARE(group.title(), a("Chat proxy"));
        QVERIFY(group.statusLabel()->text().startsWith(a("No chat proxy is set up. Windows names no proxy")));
        QVERIFY(group.toggleButton()->isHidden()); // nothing to turn on
        QCOMPARE(group.settingsButton()->text(), a("&Chat proxy settings…"));
        QVERIFY(group.useItButton()->isHidden());
        QVERIFY(env.control->save(savedProxy()));
        QVERIFY(!group.toggleButton()->isHidden());
        QCOMPARE(group.toggleButton()->text(), a("Turn on chat pro&xy"));
        QVERIFY(group.statusLabel()->text().endsWith(a("Saved chat proxy: SOCKS5 proxy 127.0.0.1:2080 (off).")));
        // At once, with the line.
        group.toggleButton()->click();
        QVERIFY(env.control->config().on);
        QCOMPARE(group.toggleButton()->text(), a("Turn off chat pro&xy"));
        QCOMPARE(group.statusLabel()->text(), a("In use: SOCKS5 proxy 127.0.0.1:2080. Not checked yet."));
        QVERIFY(env.lastLine().startsWith(a("TS Media chat Chat proxy on: SOCKS5 proxy 127.0.0.1:2080.")));
        env.control->reportOk(env.control->generation());
        QCOMPARE(group.statusLabel()->text(), a("In use: SOCKS5 proxy 127.0.0.1:2080 · working"));
        QCOMPARE(group.statusIcon(), a("check"));
        QCOMPARE(group.statusLabel()->property("role").toString(), a("ok"));
        group.toggleButton()->click();
        QVERIFY(!env.control->config().on);
        QCOMPARE(env.lastLine(), a("TS Media chat Chat proxy off. TS Media uses your Windows proxy settings again."));
        // Windows names only a SOCKS proxy: Use it.
        windowsproxy::setIeConfigProviderForTests([] { return manual("socks=127.0.0.1:2080"); });
        group.refresh();
        QVERIFY(!group.useItButton()->isHidden());
        QCOMPARE(group.statusIcon(), a("warning"));
        // Chat proxy settings… opens the window.
        group.settingsButton()->click();
        QWidget* window = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QLatin1String("tsmediaProxyDialog") && w->isVisible())
                window = w;
        }
        QVERIFY(window);
        QVERIFY(!qobject_cast<proxy::ui::ProxyWindow*>(window)->useBox()->isChecked()); // saved and off: as saved
        delete window;
    }

    // Review fix: with nothing saved, Chat proxy settings… (and a card's Set up proxy… / Proxy settings…) opens the set-up
    // form, the box ticked and the server 127.0.0.1, so Find, Test and OK turn the new proxy on, as USAGE and the FAQ say.
    void setUpFromSettingsAndCards()
    {
        QCOMPARE(proxy::ui::settingsRequest(false), proxy::ui::Request::SetUp);
        QCOMPARE(proxy::ui::settingsRequest(true), proxy::ui::Request::Open);
        Env                   env;
        proxy::ui::ProxyGroup group(nullptr, env.control.get());
        group.settingsButton()->click();
        proxy::ui::ProxyWindow* window = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QLatin1String("tsmediaProxyDialog") && w->isVisible())
                window = qobject_cast<proxy::ui::ProxyWindow*>(w);
        }
        QVERIFY(window);
        QVERIFY(window->useBox()->isChecked());
        QCOMPARE(window->serverField()->text(), a("127.0.0.1"));
        // Find filled the port (the window's own Find is tested in windowJobs); OK saves it on, with the line.
        window->portField()->setText(a("2080"));
        QVERIFY(window->accept_());
        QVERIFY(env.control->config().on);
        QCOMPARE(env.control->mode(), Mode::Custom);
        QVERIFY(env.lastLine().startsWith(a("TS Media chat Chat proxy on: SOCKS5 proxy 127.0.0.1:2080.")));
        // The card's path (ChatLinkPreviews isn't in the tests): the same request.
        QVERIFY(sourceText(a("src/chatlinkpreviews.cpp")).contains(a("proxy::ui::settingsRequest(proxySaved())")));
    }

    void privacyTabKeysAndHeight()
    {
        windowsproxy::setIeConfigProviderForTests([] { return manual("socks=127.0.0.1:2080"); }); // the Use it button shown
        Env           env;
        QWidget       page;
        auto*         layout  = new QVBoxLayout(&page);
        auto*         privacy = new PrivacySection(&page);
        auto*         group   = new proxy::ui::ProxyGroup(&page, env.control.get());
        layout->addWidget(privacy);
        layout->addWidget(group);
        privacy->load(Settings());
        QVERIFY(!group->useItButton()->isHidden());
        QStringList texts;
        for (QAbstractButton* b : page.findChildren<QAbstractButton*>())
            texts << b->text();
        for (QLabel* l : page.findChildren<QLabel*>()) {
            if (l->buddy())
                texts << l->text();
        }
        // The Updates group (built in) and the dialog's Apply and Restore defaults (their sources).
        const QString updates = sourceText(a("src/update/updatesettingsgroup.cpp"));
        QVERIFY(updates.contains(a("i18n::t(\"Check for &updates automatically\")")) && updates.contains(a("i18n::t(\"Check &now\")")));
        texts << a("Check for &updates automatically") << a("Check &now");
        const QString dialog = sourceText(a("src/settingsdialog.cpp"));
        QVERIFY(dialog.contains(a("i18n::t(\"&Apply\")")) && dialog.contains(a("i18n::t(\"&Restore defaults\")")));
        texts << a("&Apply") << a("&Restore defaults");
        const QList<QChar> keys = keysOf(texts);
        QCOMPARE(keys.size(), QSet<QChar>(keys.begin(), keys.end()).size());
        QCOMPARE(QSet<QChar>(keys.begin(), keys.end()),
                 QSet<QChar>({'h', 'y', 'o', 'w', 'l', 'm', 'p', 'x', 'c', 'i', 'u', 'n', 'a', 'r'}));
        // The player switch keeps p.
        QVERIFY(texts.contains(a("&Play YouTube videos in a TS Media window")));
        // The group is about three rows (F9): the title, the status line, the buttons. The dialog grows by at most 100 px
        // with the default settings (the Windows settings, nothing saved), whose status is one line at the Settings dialog's
        // width with TeamSpeak's font (about 520 px of Segoe UI 9 pt). The offscreen platform's fallback font is far wider
        // (17 px per character), so the line is counted as one here: everything but the text, plus one line.
        windowsproxy::setIeConfigProviderForTests([] { return manual(""); });
        group->refresh();
        page.resize(640, 900);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        const int height    = group->hasHeightForWidth() ? group->heightForWidth(group->width()) : group->sizeHint().height();
        const int textLines = group->statusLabel()->heightForWidth(group->statusLabel()->width());
        const int oneLine   = group->statusLabel()->fontMetrics().height();
        QVERIFY2(height - textLines + oneLine <= 100, qPrintable(QString::number(height - textLines + oneLine)));
        // It sits between the Privacy section and Updates in the dialog (its constructor, before UpdateSettingsGroup).
        const int at = dialog.indexOf(a("new proxy::ui::ProxyGroup(this)"));
        QVERIFY(at > 0 && at < dialog.indexOf(a("new upd::UpdateSettingsGroup(this)")));
    }

    void playerSwitchLabel()
    {
        QCOMPARE(PrivacySection::playYouTubeLabel(false), a("&Play YouTube videos in a TS Media window"));
        QCOMPARE(PrivacySection::playYouTubeLabel(true), a("&Play YouTube videos in the chat"));
        QCOMPARE(PrivacySection::playYouTubeLabel(ytpage::kAnchoredPlayer), a("&Play YouTube videos in a TS Media window")); // 2.6.0's gate
        PrivacySection section;
        QCheckBox* box = section.findChild<QCheckBox*>(a("tsmediaPlayYouTube"));
        QVERIFY(box);
        QCOMPARE(box->text(), a("&Play YouTube videos in a TS Media window"));
    }

    // ---- the chat's strips, menu and rules (design 1.5, 5.3, 6; 9.4 tests 6 and 8) ---------------------------------------
    void cardStrips()
    {
        using linkcard::ProxyStrip;
        linkcard::Card c;
        c.kind = lp::Kind::Image;
        c.host = a("example.com");
        // routedown: the Click look ("Paused"), Load preview, the normal tool tip, never the error colour.
        c.look       = linkcard::Look::Click;
        c.proxyStrip = ProxyStrip::PausedChatProxy;
        QCOMPARE(linkcard::statusText(c), a("Paused · the proxy isn't working"));
        QCOMPARE(linkcard::actionLabel(c), a("Load preview"));
        QCOMPARE(linkcard::primaryZone(c), linkcard::Zone::Load);
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Load), a("Loading the preview connects your PC to example.com, which sees your IP address."));
        // The Windows proxy, automatic: the quiet click look with its own tool tip.
        c.proxyStrip  = ProxyStrip::PausedWindows;
        c.proxyReason = a("can't reach the proxy");
        QCOMPARE(linkcard::statusText(c), a("Couldn't load through your Windows proxy"));
        QCOMPARE(linkcard::actionLabel(c), a("Load preview"));
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Load), a("Your Windows proxy didn't work: can't reach the proxy. Click to try again."));
        // Errors with Retry.
        c.look        = linkcard::Look::Error;
        c.proxyStrip  = ProxyStrip::ChatProxy;
        c.proxyReason = a("can't reach the proxy at 127.0.0.1:2080");
        c.error       = linkcard::proxyErrorText(c.proxyStrip);
        QCOMPARE(linkcard::statusText(c), a("Couldn't load through the chat proxy"));
        QCOMPARE(linkcard::actionLabel(c), a("Retry"));
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry), a("Load it again through the chat proxy. It didn't work: can't reach the proxy at 127.0.0.1:2080."));
        c.proxyStrip = ProxyStrip::Windows;
        c.error      = linkcard::proxyErrorText(c.proxyStrip);
        QCOMPARE(linkcard::statusText(c), a("Couldn't load through your Windows proxy"));
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry),
                 a("Load it again through your Windows proxy. It didn't work: can't reach the proxy at 127.0.0.1:2080."));
        // Review fix: no reason (our own link that ended routedown made no request): never "It didn't work: .".
        c.proxyReason.clear();
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry), a("Load it again through your Windows proxy."));
        c.proxyStrip = ProxyStrip::ChatProxy;
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry), a("Load it again through the chat proxy."));
        {
            linkcard::Card q = c;
            q.look           = linkcard::Look::Click;
            q.proxyStrip     = ProxyStrip::PausedWindows;
            QCOMPARE(linkcard::toolTip(q, linkcard::Zone::Load), a("Your Windows proxy didn't work. Click to try again."));
        }
        c.proxyReason = a("can't reach the proxy at 127.0.0.1:2080");
        // Filtered (F10): no Retry on the strip; the chat proxy or its setup instead.
        c.proxyStrip = ProxyStrip::FilteredUseProxy;
        c.error      = linkcard::proxyErrorText(c.proxyStrip);
        QCOMPARE(linkcard::statusText(c), a("Your network blocks this site"));
        QCOMPARE(linkcard::actionLabel(c), a("Use chat proxy"));
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry),
                 a("Your network's DNS gave this site a filtered address. The chat proxy loads it through your VPN app."));
        c.proxyStrip = ProxyStrip::FilteredSetUp;
        QCOMPARE(linkcard::actionLabel(c), a("Set up proxy…"));
        QCOMPARE(linkcard::toolTip(c, linkcard::Zone::Retry),
                 a("Your network's DNS gave this site a filtered address. A chat proxy (your VPN app's local port) can load it."));
        // A different strip draws again (the signature).
        linkcard::Card d = c;
        d.proxyStrip     = ProxyStrip::FilteredUseProxy;
        PreviewStyle style;
        QVERIFY(linkcard::signature(c, style, {}) != linkcard::signature(d, style, {}));
        // The local state a route failure gives (design 1.5).
        using linkstate::Local;
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        const QString proxyCode = QString::fromLatin1(lp::errors::kProxy), filtered = QString::fromLatin1(lp::errors::kFiltered),
                      routeDown = QString::fromLatin1(lp::errors::kRouteDown);
        linkstate::LocalState l = linkstate::afterRouteFailure(loading, routeDown, Via::Unset, Failure::None, false, 3, true);
        QCOMPARE(l.state, Local::Paused);
        QCOMPARE(l.pause, linkstate::Pause::ChatProxy);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::PausedChatProxy);
        linkstate::Decision dec = linkstate::decide(lp::Kind::Image, false, l, linkstate::ServerState());
        QCOMPARE(dec.look, linkcard::Look::Click);
        // The chat proxy, automatic and decisive while Down: Paused; ambiguous, before Down or a click: Failed.
        l = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::Unreachable, true, 3, true);
        QCOMPARE(l.state, Local::Paused);
        l = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::CantReachSite, false, 3, true);
        QCOMPARE(l.state, Local::Failed);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::ChatProxy);
        l = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::Unreachable, true, 3, false);
        QCOMPARE(l.state, Local::Failed);
        linkstate::LocalState clicked = loading;
        clicked.clicked               = true;
        l = linkstate::afterRouteFailure(clicked, proxyCode, Via::Bridge, Failure::Unreachable, true, 3, true);
        QCOMPARE(l.state, Local::Failed);
        // A refused custom route is the chat proxy's; Windows' refusals are Windows'.
        l = linkstate::afterRouteFailure(clicked, proxyCode, Via::Refused, Failure::PasswordUnreadable, true, 3, false);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::ChatProxy);
        l = linkstate::afterRouteFailure(clicked, proxyCode, Via::Refused, Failure::SocksOnlyWindows, true, 3, false);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::Windows);
        // Review fix: a setup script that didn't answer in time (only Windows mode refuses with Slow) is Windows' too.
        QVERIFY(!linkstate::viaChatProxy(Via::Refused, Failure::Slow));
        QVERIFY(linkstate::viaChatProxy(Via::Refused, Failure::BridgeUnavailable));
        l = linkstate::afterRouteFailure(loading, proxyCode, Via::Refused, Failure::Slow, false, 3, false);
        QCOMPARE(l.state, Local::Paused);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::PausedWindows);
        // The Windows proxy: automatic -> its quiet click look; clicked -> the error.
        l = linkstate::afterRouteFailure(loading, proxyCode, Via::System, Failure::Unreachable, true, 3, false);
        QCOMPARE(l.state, Local::Paused);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::PausedWindows);
        l = linkstate::afterRouteFailure(clicked, proxyCode, Via::System, Failure::Unreachable, true, 3, false);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::Windows);
        // Filtered: Use chat proxy (a proxy is saved) or Set up proxy… (none).
        l = linkstate::afterRouteFailure(loading, filtered, Via::Direct, Failure::None, false, 3, false);
        QCOMPARE(l.state, Local::Failed);
        QCOMPARE(linkstate::proxyStripOf(l, true), ProxyStrip::FilteredUseProxy);
        QCOMPARE(linkstate::proxyStripOf(l, false), ProxyStrip::FilteredSetUp);
        // A paused slot is never an automatic candidate and says why.
        linkstate::LocalState paused;
        paused.state = Local::Paused;
        paused.pause = linkstate::Pause::Click;
        QCOMPARE(linkstate::proxyStripOf(paused, true), ProxyStrip::None); // the plain Load preview strip
        QCOMPARE(linkstate::holdReason(linkstate::decide(lp::Kind::Image, false, paused, linkstate::ServerState()), paused, linkstate::Context(),
                                       true, true, true, lp::HostGate::Verdict::Go),
                 linkstate::HoldReason::Paused);
        QCOMPARE(QString::fromLatin1(linkstate::holdReasonName(linkstate::HoldReason::Paused)), a("paused"));
    }

    void downAndResume()
    {
        // healthNotTriggeredBySender (F11): entering Down pauses only automatic decisive failures of this generation.
        using linkstate::Local;
        const QString proxyCode = QString::fromLatin1(lp::errors::kProxy);
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        const linkstate::LocalState decisiveAuto = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::Unreachable, true, 4, false);
        QCOMPARE(decisiveAuto.state, Local::Failed);
        QVERIFY(linkstate::pausesOnDown(decisiveAuto, 4));
        QVERIFY(!linkstate::pausesOnDown(decisiveAuto, 5)); // another generation
        const linkstate::LocalState ambiguous = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::CantReachSite, false, 4, false);
        QVERIFY(!linkstate::pausesOnDown(ambiguous, 4));
        linkstate::LocalState clicked = loading;
        clicked.clicked               = true;
        QVERIFY(!linkstate::pausesOnDown(linkstate::afterRouteFailure(clicked, proxyCode, Via::Bridge, Failure::Unreachable, true, 4, false), 4));
        const linkstate::LocalState windows = linkstate::afterRouteFailure(clicked, proxyCode, Via::System, Failure::Unreachable, true, 4, false);
        QVERIFY(!linkstate::pausesOnDown(windows, 4));
        // Working or probation: the links paused for the chat proxy resume; Windows' quiet ones and route-change clicks don't.
        linkstate::LocalState paused = linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kRouteDown), Via::Unset, Failure::None, false, 4, true);
        QVERIFY(linkstate::resumesOnHealth(paused));
        QVERIFY(!linkstate::resumesOnHealth(linkstate::afterRouteFailure(loading, proxyCode, Via::System, Failure::Unreachable, true, 4, false)));
        linkstate::LocalState click;
        click.state = Local::Paused;
        click.pause = linkstate::Pause::Click;
        QVERIFY(!linkstate::resumesOnHealth(click));
    }

    void routeChangeRules()
    {
        using linkstate::AfterRoute;
        using linkstate::Local;
        const QString proxyCode = QString::fromLatin1(lp::errors::kProxy);
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        // turnOffDoesNotRefetch (S4): five paused links, the chat proxy turned off -> each waits for a click.
        int waiting = 0;
        for (int i = 0; i < 5; ++i) {
            const linkstate::LocalState p = linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kRouteDown), Via::Unset, Failure::None, false, 2, true);
            const AfterRoute next = linkstate::afterRouteChange(p, false);
            QCOMPARE(next, AfterRoute::Click);
            waiting += next == AfterRoute::Click ? 1 : 0;
        }
        QCOMPARE(waiting, 5);
        // Toward the chat proxy: paused, proxy-failed, filtered and walled links go back to the normal rules.
        const linkstate::LocalState failed = linkstate::afterRouteFailure(loading, proxyCode, Via::Bridge, Failure::CantReachSite, false, 2, false);
        QCOMPARE(linkstate::afterRouteChange(failed, true), AfterRoute::Idle);
        const linkstate::LocalState filtered = linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kFiltered), Via::Direct, Failure::None, false, 2, false);
        QCOMPARE(linkstate::afterRouteChange(filtered, true), AfterRoute::Idle);
        QCOMPARE(linkstate::afterRouteChange(filtered, false), AfterRoute::Keep); // filtered elsewhere: as it is
        linkstate::LocalState walled;
        walled.state = Local::Nothing;
        walled.code  = QString::fromLatin1(lp::errors::kWalled);
        QCOMPARE(linkstate::afterRouteChange(walled, true), AfterRoute::Idle);
        QCOMPARE(linkstate::afterRouteChange(walled, false), AfterRoute::Click);
        linkstate::LocalState nothing;
        nothing.state = Local::Nothing;
        nothing.code  = QString::fromLatin1(lp::errors::kNothing);
        QCOMPARE(linkstate::afterRouteChange(nothing, true), AfterRoute::Keep); // a content fact stays
        // Windows <-> direct: proxy-failed automatic links wait for a click; a clicked one keeps its error.
        const linkstate::LocalState windowsPaused = linkstate::afterRouteFailure(loading, proxyCode, Via::System, Failure::Unreachable, true, 2, false);
        QCOMPARE(linkstate::afterRouteChange(windowsPaused, false), AfterRoute::Click);
        linkstate::LocalState clicked = loading;
        clicked.clicked               = true;
        QCOMPARE(linkstate::afterRouteChange(linkstate::afterRouteFailure(clicked, proxyCode, Via::System, Failure::Unreachable, true, 2, false), false),
                 AfterRoute::Keep);
        // Loaded, too large and plain idle links are never touched.
        linkstate::LocalState loaded;
        loaded.state = Local::Loaded;
        QCOMPARE(linkstate::afterRouteChange(loaded, true), AfterRoute::Keep);
        QCOMPARE(linkstate::afterRouteChange(loaded, false), AfterRoute::Keep);
        QCOMPARE(linkstate::afterRouteChange(linkstate::LocalState(), false), AfterRoute::Keep);
        // Live test fix (S4): a link the chat proxy's probation let go (back to the normal rules: no entry, or its automatic
        // probe running) that hasn't loaded yet waits for a click away from the chat proxy; a click, a finished load or the
        // chat proxy itself: the normal rules.
        QCOMPARE(linkstate::afterRouteChangeResumed(linkstate::LocalState(), false), AfterRoute::Click);
        QCOMPARE(linkstate::afterRouteChangeResumed(loading, false), AfterRoute::Click);
        QCOMPARE(linkstate::afterRouteChangeResumed(linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kRouteDown), Via::Unset,
                                                                                 Failure::None, false, 2, true),
                                                    false),
                 AfterRoute::Click);
        QCOMPARE(linkstate::afterRouteChangeResumed(linkstate::LocalState(), true), AfterRoute::Keep);
        QCOMPARE(linkstate::afterRouteChangeResumed(clicked, false), AfterRoute::Keep);
        QCOMPARE(linkstate::afterRouteChangeResumed(loaded, false), AfterRoute::Keep);
        QCOMPARE(linkstate::afterRouteChangeResumed(nothing, false), AfterRoute::Keep);
        // Review fix: a busy site's wait (Cooling: a 429, or the gate's wait) hasn't loaded it either; one that waits for a
        // click already is kept as it is (counted once).
        const linkstate::LocalState gateBusy = linkstate::afterBusy(loading, 0, false, true, true, 1000);
        QCOMPARE(gateBusy.state, Local::Cooling);
        QCOMPARE(linkstate::afterRouteChangeResumed(gateBusy, false), AfterRoute::Click);
        const linkstate::LocalState limited = linkstate::afterBusy(loading, 5000, false, false, true, 1000);
        QCOMPARE(limited.state, Local::Cooling);
        QCOMPARE(linkstate::afterRouteChangeResumed(limited, false), AfterRoute::Click);
        QCOMPARE(linkstate::afterRouteChangeResumed(limited, true), AfterRoute::Keep);
        linkstate::LocalState waitsForClick;
        waitsForClick.state = Local::Paused;
        waitsForClick.pause = linkstate::Pause::Click;
        QCOMPARE(linkstate::afterRouteChangeResumed(waitsForClick, false), AfterRoute::Keep);
        // Remembered until it loads, ends or is clicked.
        QVERIFY(linkstate::resumedStillWaits(linkstate::LocalState()));
        QVERIFY(linkstate::resumedStillWaits(loading));
        QVERIFY(linkstate::resumedStillWaits(gateBusy));
        QVERIFY(linkstate::resumedStillWaits(waitsForClick));
        QVERIFY(!linkstate::resumedStillWaits(clicked));
        QVERIFY(!linkstate::resumedStillWaits(loaded));
        QVERIFY(!linkstate::resumedStillWaits(nothing));
        QVERIFY(!linkstate::resumedStillWaits(failed));
        linkstate::LocalState tooLarge;
        tooLarge.state = Local::TooLarge;
        QVERIFY(!linkstate::resumedStillWaits(tooLarge));
        QVERIFY(!linkstate::resumedStillWaits(linkstate::afterBusy(loading, 5000, false, false, false, 1000))); // a click: the busy line
        // ChatLinkPreviews keeps the keys its health pass let go, turns them into clicks on a move away from the proxy
        // (routeChangePass), and forgets one only when its load ended (not on a busy answer: review fix).
        const QString source = sourceText(a("src/chatlinkpreviews.cpp"));
        QVERIFY(source.contains(a("m_resumedFromPause.insert(it.key());")));
        QVERIFY(source.contains(a("linkstate::routeChangePass(&m_local, &m_resumedFromPause, m_pipelineWaits, toChatProxy, &idled)")));
        QVERIFY(source.contains(a("if (!linkstate::resumedStillWaits(m_local.value(key)))")));
        QCOMPARE(source.count(a("forgetResumedIfDone(key);")), 3); // a failure, a load, a video's poster
        QVERIFY(!source.contains(a("m_resumedFromPause.clear()")));
        QCOMPARE(source.count(a("m_resumedFromPause.remove(")), 2); // a click, forgetResumedIfDone
    }

    // Live test fix (S4, the L6 re-run): while the chat proxy is down the automatic pass pauses a link on screen at once
    // (the state its routedown answer would give), instead of holding it for its sender's turn as a plain click card that
    // then loaded by itself through the Windows proxy after the notice's Turn off chat proxy.
    void downPausesLinksWaitingTheirTurn()
    {
        using linkstate::AfterRoute;
        using linkstate::Local;
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        const linkstate::LocalState routedown = linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kRouteDown), Via::Unset, Failure::None, false, 7, true);
        const linkstate::LocalState paused    = linkstate::pausedForChatProxy(7);
        QCOMPARE(paused.state, Local::Paused);
        QCOMPARE(paused.pause, linkstate::Pause::ChatProxy);
        QCOMPARE(paused.state, routedown.state);
        QCOMPARE(paused.code, routedown.code);
        QVERIFY(paused.via == routedown.via);
        QCOMPARE(paused.routeGeneration, routedown.routeGeneration);
        QVERIFY(!paused.clicked);
        QCOMPARE(linkstate::proxyStripOf(paused, true), linkcard::ProxyStrip::PausedChatProxy); // "Paused · the proxy isn't working"
        QVERIFY(linkstate::resumesOnHealth(paused));                                   // probation lets it go
        QCOMPARE(linkstate::afterRouteChange(paused, false), AfterRoute::Click);        // a turn-off: Load preview
        QCOMPARE(linkstate::afterRouteChange(paused, true), AfterRoute::Idle);
        // Only while the chat proxy is down, once the cache was looked at, and never our own pipeline's link.
        QVERIFY(linkstate::pausesForChatProxy(true, true, false));
        QVERIFY(!linkstate::pausesForChatProxy(false, true, false));
        QVERIFY(!linkstate::pausesForChatProxy(true, false, false));
        QVERIFY(!linkstate::pausesForChatProxy(true, true, true));
        // Eight links on screen while Down: two started (routedown), six would have waited for their turn. All paused, then
        // probation lets them go, one loads, and Turn off chat proxy: the other seven wait for a click.
        QHash<QString, linkstate::LocalState> local;
        QSet<QString>                         resumed;
        for (int i = 0; i < 8; ++i)
            local.insert(a("d%1").arg(i), i < 2 ? routedown : paused);
        for (auto it = local.begin(); it != local.end();) { // onHealthChanged (Probation)
            QVERIFY(linkstate::resumesOnHealth(it.value()));
            resumed.insert(it.key());
            it = local.erase(it);
        }
        linkstate::LocalState loaded;
        loaded.state = Local::Loaded;
        local.insert(a("d0"), loaded);
        if (!linkstate::resumedStillWaits(local.value(a("d0"))))
            resumed.remove(a("d0"));
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, QSet<QString>(), false, nullptr), 7);
        for (int i = 1; i < 8; ++i)
            QCOMPARE(local.value(a("d%1").arg(i)).pause, linkstate::Pause::Click);
        // ChatLinkPreviews asks before the sender's turn and budget, and counts it for the status row.
        const QString source = sourceText(a("src/chatlinkpreviews.cpp"));
        const int     pause  = source.indexOf(a("linkstate::pausesForChatProxy(chatProxyDown(), m_cacheMiss.contains(ref.target.key), "
                                                 "m_pipeline && m_pipeline->pending(ref.target.key))"));
        const int     turn   = source.indexOf(a("if (!freeTurn(sender) || !m_budget.take(sender, wallMs(), ref.target.expectedRequests))"));
        QVERIFY(pause > 0 && turn > pause);
        QVERIFY(source.indexOf(a("void ChatLinkPreviews::runAutoLoads()")) < pause);
        QVERIFY(source.contains(a("m_local.insert(target.key, linkstate::pausedForChatProxy(m_proxy->generation()));")));
        QVERIFY(source.contains(a("m_proxy->notePausedLoad();")));
        Env env;
        const int before = env.control->pausedLoads();
        env.control->notePausedLoad();
        QCOMPARE(env.control->pausedLoads(), before + 1);
    }

    // The links that waited for a proxy, over several route changes (review fixes of the S4 live test fix). Loop 1 of
    // ChatLinkPreviews::onRouteChanged (running automatic loads back to the normal rules) is modelled by erasing them.
    void routeChangePassSequences()
    {
        using linkstate::Local;
        using linkstate::LocalState;
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        const LocalState paused = linkstate::afterRouteFailure(loading, QString::fromLatin1(lp::errors::kRouteDown), Via::Unset, Failure::None, false, 2, true);
        QCOMPARE(paused.state, Local::Paused);
        const auto loopOne = [](QHash<QString, LocalState>* local, const QSet<QString>& pipelineWaits) {
            for (auto it = local->begin(); it != local->end();) {
                if (it->state == Local::Loading && !it->clicked && !pipelineWaits.contains(it.key()))
                    it = local->erase(it);
                else
                    ++it;
            }
        };
        const auto forgetIfDone = [](const QHash<QString, LocalState>& local, QSet<QString>* resumed, const QString& key) {
            if (!linkstate::resumedStillWaits(local.value(key)))
                resumed->remove(key);
        };
        // Would load by itself on the route now in use: no entry (Idle) or a busy site's wait.
        const auto autoLoads = [](const QHash<QString, LocalState>& local, const QStringList& keys) {
            int n = 0;
            for (const QString& k : keys) {
                const Local s = local.value(k).state;
                n += s == Local::Idle || s == Local::Cooling ? 1 : 0;
            }
            return n;
        };
        QStringList keys;
        for (int i = 0; i < 7; ++i)
            keys << a("k%1").arg(i);
        const QSet<QString> noPipeline;

        // Down, then Turn off chat proxy, then on again, then off again before they load: 0 automatic loads (finding 5a).
        QHash<QString, LocalState> local;
        QSet<QString>              resumed;
        for (const QString& k : qAsConst(keys))
            local.insert(k, paused);
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, false, nullptr), 7);
        for (const QString& k : qAsConst(keys))
            QVERIFY(local.value(k).state == Local::Paused && local.value(k).pause == linkstate::Pause::Click);
        QVERIFY(resumed.isEmpty());
        QStringList idled;
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, true, &idled), 0); // back on: the normal rules
        QCOMPARE(idled.size(), 7);
        QVERIFY(local.isEmpty());
        QCOMPARE(resumed.size(), 7);
        // The sender's budget lets two start and one meets a busy site; one loads; three wait their turn.
        local.insert(keys.at(0), loading);
        local.insert(keys.at(1), loading);
        local.insert(keys.at(2), linkstate::afterBusy(loading, 0, false, true, true, 1000));
        LocalState loaded;
        loaded.state = Local::Loaded;
        local.insert(keys.at(3), loaded);
        for (const QString& k : qAsConst(keys))
            forgetIfDone(local, &resumed, k);
        QCOMPARE(resumed.size(), 6); // the loaded one is the normal rules' now
        loopOne(&local, noPipeline);
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, false, nullptr), 6);
        QCOMPARE(autoLoads(local, keys), 0);
        QCOMPARE(local.value(keys.at(3)).state, Local::Loaded);
        QVERIFY(resumed.isEmpty());

        // Probation let them go, the site said "busy" (429 / gate), then Turn off chat proxy: no load by itself (findings 1, 3).
        local.clear();
        resumed.clear();
        local.insert(keys.at(0), paused);
        local.insert(keys.at(1), paused);
        for (auto it = local.begin(); it != local.end();) { // onHealthChanged (Probation)
            QVERIFY(linkstate::resumesOnHealth(it.value()));
            resumed.insert(it.key());
            it = local.erase(it);
        }
        local.insert(keys.at(0), linkstate::afterBusy(loading, 30000, false, false, true, 1000)); // a 429
        local.insert(keys.at(1), linkstate::afterBusy(loading, 0, false, true, true, 1000));      // the gate's wait
        forgetIfDone(local, &resumed, keys.at(0));
        forgetIfDone(local, &resumed, keys.at(1));
        QCOMPARE(resumed.size(), 2);
        loopOne(&local, noPipeline);
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, false, nullptr), 2);
        QCOMPARE(autoLoads(local, keys.mid(0, 2)), 0);

        // Custom -> custom (a port changed in the window) while they wait their turn: still remembered (finding 5b).
        local.clear();
        resumed = QSet<QString>{keys.at(0), keys.at(1), keys.at(2)};
        local.insert(keys.at(1), loading);
        local.insert(keys.at(2), loaded);
        loopOne(&local, noPipeline);
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, true, nullptr), 0);
        QCOMPARE(resumed, (QSet<QString>{keys.at(0), keys.at(1)}));
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, noPipeline, false, nullptr), 2);
        QCOMPARE(autoLoads(local, keys.mid(0, 3)), 0);
        QCOMPARE(local.value(keys.at(2)).state, Local::Loaded);

        // Our own pipeline loads the same link (the peer's slot waits on it): never canceled or made a click; it stays
        // remembered (finding 2). A paused one is counted once.
        local.clear();
        resumed = QSet<QString>{keys.at(0), keys.at(1)};
        local.insert(keys.at(0), loading);
        local.insert(keys.at(1), paused);
        const QSet<QString> pipeline{keys.at(0)};
        loopOne(&local, pipeline);
        QCOMPARE(linkstate::routeChangePass(&local, &resumed, pipeline, false, nullptr), 1);
        QCOMPARE(local.value(keys.at(0)).state, Local::Loading);
        QCOMPARE(local.value(keys.at(1)).pause, linkstate::Pause::Click);
        QCOMPARE(resumed, QSet<QString>{keys.at(0)});
    }

    void cardMenuItems()
    {
        using linkstate::Local;
        const QString proxyCode = QString::fromLatin1(lp::errors::kProxy);
        linkstate::LocalState clicked;
        clicked.state   = Local::Loading;
        clicked.clicked = true;
        const linkstate::LocalState chat     = linkstate::afterRouteFailure(clicked, proxyCode, Via::Bridge, Failure::Unreachable, true, 2, false);
        const linkstate::LocalState windows  = linkstate::afterRouteFailure(clicked, proxyCode, Via::System, Failure::Unreachable, true, 2, false);
        const linkstate::LocalState filtered = linkstate::afterRouteFailure(clicked, QString::fromLatin1(lp::errors::kFiltered), Via::Direct, Failure::None, false, 2, false);
        linkstate::ProxyMenu m = linkstate::proxyMenuOf(chat, true, true);
        QVERIFY(m.turnOff && !m.turnOn && m.settings);
        m = linkstate::proxyMenuOf(windows, false, true);
        QVERIFY(!m.turnOff && m.turnOn && m.settings);
        m = linkstate::proxyMenuOf(windows, false, false); // nothing saved: no Turn on
        QVERIFY(!m.turnOn && m.settings);
        m = linkstate::proxyMenuOf(filtered, false, true);
        QVERIFY(m.turnOn && m.settings && !m.turnOff);
        linkstate::LocalState click;
        click.state = Local::Paused;
        click.pause = linkstate::Pause::Click;
        m           = linkstate::proxyMenuOf(click, false, true);
        QVERIFY(!m.turnOn && !m.turnOff && m.settings);
        m = linkstate::proxyMenuOf(linkstate::LocalState(), true, true);
        QVERIFY(!m.turnOn && !m.turnOff && !m.settings);
        // Their access keys (o, x) are free in the card menu (L t S b a S w C H p R) and unique.
        const QString source = sourceText(a("src/chatlinkpreviews.cpp"));
        QVERIFY(source.contains(a("i18n::t(\"Turn &off chat proxy\")")));
        QVERIFY(source.contains(a("i18n::t(\"Turn &on chat proxy\")")));
        QVERIFY(source.contains(a("i18n::t(\"Pro&xy settings…\")")));
        const QList<QChar> used = keysOf({a("&Load preview"), a("Re&try"), a("&Show preview"), a("Open in &browser"), a("Try a&gain"), a("&Stop video"),
                                          a("Play in a &window"), a("&Copy link"), a("&Hide preview"), a("&Play"), a("&Reply")});
        for (const QChar k : {QChar('o'), QChar('x')})
            QVERIFY(!used.contains(k));
    }

    // ---- the updater (design 3.5, 9.4 test 9) -------------------------------------------------------------------------
    void updaterWiring()
    {
        QCOMPARE(upd::proxyShortError(Failure::Unreachable, Via::Bridge).isEmpty(), false);
        QVERIFY(upd::proxyShortError(Failure::Unreachable, Via::Bridge).contains(a("chat proxy")));
        QVERIFY(upd::proxyShortError(Failure::Unreachable, Via::System).contains(a("Windows proxy")));
        // A route change never cancels or restarts a check (F8): the Updater doesn't listen to the route; it reports to
        // route health (5.2) and reads the route per request.
        const QString updater = sourceText(a("src/update/updater.cpp"));
        QVERIFY(!updater.contains(a("routeChanged")));
        QVERIFY(updater.contains(a("control->report(report);")));
        QVERIFY(updater.contains(a("control->reportOk(outcome.routeGeneration);")));
    }

    // ---- CRASH RULE in the new files (design 3.9) ---------------------------------------------------------------------
    void sourceRulesUi()
    {
        for (const char* name : {"src/proxy/proxywindow.cpp", "src/proxy/proxygroup.cpp", "src/proxy/proxytexts.cpp", "src/proxy/proxycommands.cpp",
                                 "src/settingstheme.cpp", "src/linkpreview/routereport.h", "src/proxy/control.cpp"}) {
            const QString text = sourceText(a(name));
            QVERIFY2(!text.isEmpty(), name);
            for (const char* banned : {"QTimer::singleShot", "qRegisterMetaType", "Q_DECLARE_METATYPE", "QStringLiteral", "exec("})
                QVERIFY2(!text.contains(QLatin1String(banned)), qPrintable(QString::fromLatin1(name) + a(": ") + QString::fromLatin1(banned)));
        }
        // Nothing of the window ever reads the saved password into a field.
        const QString window = sourceText(a("src/proxy/proxywindow.cpp"));
        QVERIFY(!window.contains(a("protect::open(")));
    }
};

TSMEDIA_REGISTER_TEST(TestProxyUi)
#include "tst_proxyui.moc"
