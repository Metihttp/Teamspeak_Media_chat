// 2.6 chat proxy, I3 (design 9.5): the diagnostic info's "Chat proxy" section for every state, and that neither the whole
// report (its log tail included) nor the plugin's log lines ever carry the proxy's host, port, user name or password,
// the sealed blob, or the bridge's token, realm or port (S18); the live-test driver's echo of a proxy.set line; the
// update consent text (design 8.4). Offscreen; a real proxy::Control and bridge on 127.0.0.1 only, whose upstream name
// lookup is injected (nothing leaves this PC).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "diagnostics.h"
#include "fakets3.h"
#include "pluginlog.h"
#include "proxy/bridge.h"
#include "proxy/control.h"
#include "proxy/findrun.h"
#include "proxy/protect.h"
#include "proxy/proxydiag.h"
#include "proxy/router.h"
#include "proxy/testrun.h"
#include "proxy/windowsproxy.h"
#include "proxyfakes.h"
#include "selftestecho.h"
#include "testmain.h"
#include "ts3api.h"

using namespace proxy;

namespace {

QString a(const char* text)
{
    return QString::fromUtf8(text);
}

QString sourceText(const QString& relative)
{
    QFile f(QString::fromUtf8(TSMEDIA_SOURCE_DIR) + QLatin1Char('/') + relative);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

windowsproxy::IeConfig manual(const char* proxyText)
{
    windowsproxy::IeConfig c;
    c.ok    = true;
    c.proxy = QString::fromLatin1(proxyText);
    return c;
}

Config proxyConfig(const char* host, quint16 port, bool on, Type type = Type::Socks5, const char* user = "")
{
    Config c;
    c.on   = on;
    c.type = type;
    c.host = normalizeHost(QString::fromLatin1(host));
    c.port = port;
    c.user = QString::fromLatin1(user);
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

} // namespace

class TestProxyDiag : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        windowsproxy::setIeConfigProviderForTests({});
        Control::setInstanceForTests(nullptr);
        publishWindows(); // the suite's route (testmain's setup)
        markProxyRouteChange(0);
    }

    // design 2.4: the section's lines per state (categories only)
    void diagnosticsChatProxy()
    {
        QCOMPARE(diagnosticsTitle(), a("Chat proxy"));
        QCOMPARE(diagnosticsDuration(-5), a("0 s"));
        QCOMPARE(diagnosticsDuration(45'000), a("45 s"));
        QCOMPARE(diagnosticsDuration(3 * 60'000 + 59'000), a("3 min"));
        QCOMPARE(diagnosticsDuration(125 * 60'000), a("2 h 5 min"));

        // Windows mode, Windows names an HTTP proxy on this PC; nothing else happened.
        DiagnosticsInput in;
        in.mode = Mode::System;
        in.ie   = manual("127.0.0.1:10809");
        QCOMPARE(diagnosticLines(in), QStringList({a("Mode: Windows proxy settings"), a("Windows proxy: HTTP proxy on this PC"),
                                                   a("Windows proxy health: none"), a("Last test: none"), a("Bridge: not started")}));
        in.ie = manual("proxy.corp.example:3128");
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: HTTP proxy elsewhere")));
        in.ie = manual("http=192.168.1.2:8080");
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: HTTP proxy on the local network")));
        // Windows names only a SOCKS proxy (W1).
        in.ie = manual("socks=127.0.0.1:2080");
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: SOCKS only, on this PC")));
        // A setup script or automatic detection (no script is run for the summary).
        windowsproxy::IeConfig script;
        script.ok            = true;
        script.autoConfigUrl = a("http://wpad.example/proxy.pac");
        in.ie                = script;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: setup script")));
        script.autoConfigUrl.clear();
        script.autoDetect = true;
        in.ie             = script;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: automatic detection")));
        // Review fix: detection on (Windows' default) with a manual proxy: the manual one applies wherever WPAD finds
        // nothing, so it is named.
        script.proxy = a("127.0.0.1:10809");
        in.ie        = script;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: HTTP proxy on this PC; automatic detection on")));
        script.proxy = a("socks=127.0.0.1:10808");
        in.ie        = script;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: SOCKS only, on this PC; automatic detection on")));
        script.autoConfigUrl = a("http://wpad.example/proxy.pac");
        in.ie                = script;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: setup script and automatic detection")));
        in.ie = windowsproxy::IeConfig(); // nothing could be read
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy: none")));
        // A Windows-proxy failure two minutes ago.
        in.ie                  = manual("127.0.0.1:10809");
        in.nowMs               = 10'000'000;
        in.windows.lastFailure = Failure::Unreachable;
        in.windows.atMs        = in.nowMs - 2 * 60'000;
        QVERIFY(diagnosticLines(in).contains(a("Windows proxy health: last failure 2 min ago (can't reach the proxy)")));

        // Direct: no route and no Windows health line.
        in         = DiagnosticsInput();
        in.mode    = Mode::Direct;
        in.ie      = manual("");
        QCOMPARE(diagnosticLines(in), QStringList({a("Mode: No proxy (direct)"), a("Windows proxy: none"), a("Last test: none"), a("Bridge: not started")}));

        // A proxy saved and off: one more line.
        in.saved = proxyConfig("192.168.1.5", 8080, false, Type::Http);
        QCOMPARE(diagnosticLines(in).at(1), a("Saved chat proxy: HTTP on the local network, login: no, off"));
        in.saved = proxyConfig("127.0.0.1", 2080, false, Type::Socks5, "someone");
        QCOMPARE(diagnosticLines(in).at(1), a("Saved chat proxy: SOCKS5 on this PC, login: yes, off"));

        // Custom: unknown, working, down, probation, refused.
        in             = DiagnosticsInput();
        in.mode        = Mode::Custom;
        in.saved       = proxyConfig("127.0.0.1", 2080, true, Type::Socks5, "someone");
        in.ie          = manual("");
        in.bridge      = DiagnosticsInput::BridgeState::Running;
        in.counters.tunnelsOpen     = 2;
        in.counters.accepted        = 143;
        in.counters.unauthenticated = 31;
        QCOMPARE(diagnosticLines(in),
                 QStringList({a("Mode: SOCKS5 proxy on this PC, login: yes"), a("Windows proxy: none"), a("Route: not checked yet"), a("Last test: none"),
                              a("Bridge: running, tunnels open 2, since start 143 (refused 0, busy 0, unauthenticated 31); failures: none")}));
        in.health = Health::State::Working;
        in.counters.failures.insert(static_cast<int>(Failure::CantReachSite), 1);
        in.counters.failures.insert(static_cast<int>(Failure::Unreachable), 3);
        in.counters.failures.insert(static_cast<int>(Failure::Slow), 0);
        QStringList lines = diagnosticLines(in);
        QVERIFY(lines.contains(a("Route: working")));
        QCOMPARE(lines.last(), a("Bridge: running, tunnels open 2, since start 143 (refused 0, busy 0, unauthenticated 31); failures: unreachable 3, cantReachSite 1"));
        in.health      = Health::State::Down;
        in.lastFailure = Failure::Unreachable;
        in.downForMs   = 3 * 60'000 + 5'000;
        in.pausedLoads = 4;
        QVERIFY(diagnosticLines(in).contains(a("Route: down for 3 min (can't reach the proxy), 4 automatic loads paused")));
        in.pausedLoads = 1;
        in.health      = Health::State::Probation;
        QVERIFY(diagnosticLines(in).contains(a("Route: trying again after 3 min down (can't reach the proxy), 1 automatic load paused")));
        in.refused = Failure::PasswordUnreadable;
        QVERIFY(diagnosticLines(in).contains(a("Route: password unreadable")));
        in.refused = Failure::BridgeUnavailable;
        in.bridge  = DiagnosticsInput::BridgeState::Failed;
        lines      = diagnosticLines(in);
        QVERIFY(lines.contains(a("Route: refused (TS Media couldn't open its local connection)")));
        QCOMPARE(lines.last(), a("Bridge: couldn't start"));
        // A proxy elsewhere and on the local network, HTTP without a login.
        in.saved = proxyConfig("proxy.example.net", 3128, true, Type::Http);
        QCOMPARE(diagnosticLines(in).first(), a("Mode: HTTP proxy elsewhere, login: no"));
        in.saved = proxyConfig("fd00::5", 1080, true);
        QCOMPARE(diagnosticLines(in).first(), a("Mode: SOCKS5 proxy on the local network, login: no"));

        // The last Test.
        in.nowWallMs        = 1'800'000'000'000;
        in.lastTest.ran     = true;
        in.lastTest.ok      = true;
        in.lastTest.ms      = 612;
        in.lastTest.wallMs  = in.nowWallMs - 14 * 60'000;
        QVERIFY(diagnosticLines(in).contains(a("Last test: works, 612 ms, 14 min ago")));
        in.lastTest.ok      = false;
        in.lastTest.failure = Failure::Unreachable;
        in.lastTest.wallMs  = in.nowWallMs - 2 * 60'000;
        QVERIFY(diagnosticLines(in).contains(a("Last test: can't reach the proxy, 2 min ago")));
        in.lastTest.failure = Failure::None; // another status from the test site
        QVERIFY(diagnosticLines(in).contains(a("Last test: didn't work, 2 min ago")));

        // collectDiagnostics without a Control (before start, after shutdown): not started.
        windowsproxy::setIeConfigProviderForTests([] { return manual(""); });
        QCOMPARE(collectDiagnostics(nullptr).mode, Mode::Unset);
        QCOMPARE(diagnosticLines(collectDiagnostics(nullptr)).first(), a("Mode: not started"));

        // The plugin registers the section (GUI thread, nothing once proxy::shutdown() ran).
        const QString plugin = sourceText(a("src/plugin.cpp"));
        QVERIFY(plugin.contains(a("diag::Section{proxy::diagnosticsTitle(), proxy::currentDiagnosticLines()}")));
        QVERIFY(plugin.contains(a("return proxy::Control::current() ?")));
    }

    // design 2.4, 9.5 tests 2 and 3 (S18): a custom proxy with a login whose upstream fails; then the whole report (the
    // log tail included), TeamSpeak's log, the plugin log and the chat lines carry none of the secrets.
    void diagnosticsNoSecrets()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fakets3::install(dir.path());
        ts3::pluginId = QStringLiteral("{proxy-diag-test}");
        windowsproxy::setIeConfigProviderForTests([] { return manual("127.0.0.1:10809"); });
        plog::start(dir.path() + a("/logs"));

        // The saved proxy: a login, a sealed password, on.
        Config cfg = proxyConfig("secret-host.example", 47123, true, Type::Socks5, "secret-user");
        QVERIFY(protect::seal(a("secret-pass"), cfg.endpoint(), &cfg.passwordBlob));
        QVERIFY(cfg.passwordBlob.startsWith("dpapi1:"));
        const QString file = dir.path() + a("/settings.ini");
        saveConfig(file, cfg);

        // The proxy's own name is never found (injected: no DNS query leaves this PC).
        const auto notFound = [](const QString&, QVector<linkpreview::net::Address>*, const HANDLE*, int, int) { return 1; };
        qint64           clock = 50'000'000;
        Control::Options o;
        o.settingsFile  = file;
        o.clockMs       = [&clock] { return clock; };
        o.bridgeFactory = [notFound](quint16 port, int* error) {
            Bridge::Options b;
            b.preferredPort     = port;
            b.upstream.resolver = notFound;
            return Bridge::create(b, error);
        };
        auto control = std::make_unique<Control>(o);
        Control::setInstanceForTests(control.get());
        control->start();
        QCOMPARE(control->mode(), Mode::Custom);
        const std::shared_ptr<Bridge> bridge = control->bridge();
        QVERIFY(bridge);
        const QByteArray token = bridge->token(), realm = bridge->realm();
        const quint16    bridgePort = bridge->port();
        QVERIFY(token.size() == 43 && realm.startsWith("tsmedia-"));

        // A Fetcher's request through the bridge: the proxy's name isn't found (502), a tokenless one gets the 407.
        {
            proxyfakes::Client client(bridgePort);
            QVERIFY(client.send(proxyfakes::connectRequest("www.example.com:443", "tsm-f1", token)));
            const QByteArray head = client.readHead();
            QVERIFY2(head.startsWith("HTTP/1.1 502") && head.contains("nameNotFound"), head.constData());
        }
        {
            proxyfakes::Client client(bridgePort);
            QVERIFY(client.send(proxyfakes::connectRequest("www.example.com:443", QByteArray(), QByteArray())));
            QVERIFY(client.readHead().startsWith("HTTP/1.1 407"));
        }
        // The Fetcher's report: decisive, so the route is down at once (a notice line in the chat).
        RouteReport r;
        r.failure    = Failure::NameNotFound;
        r.step       = Step::Resolve;
        r.decisive   = true;
        r.host       = a("www.example.com");
        r.generation = control->generation();
        r.via        = Via::Bridge;
        control->report(r);
        QCOMPARE(control->state(), Health::State::Down);
        QVERIFY(!control->admits(false));
        QVERIFY(!control->admits(false));
        QCOMPARE(control->pausedLoads(), 2);
        QCOMPARE(fakets3::channelPrints().size(), 1);
        clock += 3 * 60'000;

        // The window's Test with the saved values (its own temporary bridge, the same failing name).
        {
            TestRun::Options t;
            t.upstream.resolver = notFound;
            TestRun    test(t, nullptr);
            QSignalSpy spy(&test, &TestRun::finished);
            QVERIFY(test.start(cfg, protect::SecretBytes(), cfg.passwordBlob));
            QVERIFY(spy.wait(25000));
            QCOMPARE(test.result().failure, Failure::NameNotFound);
            control->noteTest(false, test.result().failure, test.result().ms, true);
        }

        // The section names categories only.
        const QStringList lines = currentDiagnosticLines();
        QCOMPARE(lines.value(0), a("Mode: SOCKS5 proxy elsewhere, login: yes"));
        QCOMPARE(lines.value(1), a("Windows proxy: HTTP proxy on this PC"));
        QCOMPARE(lines.value(2), a("Route: down for 3 min (can't find the proxy's address), 2 automatic loads paused"));
        QCOMPARE(lines.value(3), a("Last test: can't find the proxy's address, 0 s ago"));
        QVERIFY2(lines.value(4).startsWith(a("Bridge: running, tunnels open ")) && lines.value(4).contains(a("failures: nameNotFound 1")),
                 qPrintable(lines.value(4)));

        // The whole report, names included or not, with the plugin log's tail.
        diag::Facts f;
        f.created       = QDateTime::currentDateTime();
        f.pluginName    = a("TS Media chat");
        f.pluginVersion = a("2.6.0");
        f.extra << diag::Section{diagnosticsTitle(), lines};
        const QStringList pluginLog = plog::tail(100000);
        QVERIFY(!pluginLog.isEmpty());
        f.log = diag::pluginLogTail(pluginLog, pluginLog.size());

        const QByteArray  blob = cfg.passwordBlob;
        const QStringList secrets{a("secret-host"), a("47123"), a("secret-user"), a("secret-pass"), QString::fromLatin1(token),
                                  QString::fromLatin1(realm), QString::fromLatin1(realm.mid(8)), QString::fromLatin1(blob),
                                  QString::fromLatin1(blob.mid(7)), QString::fromLatin1(blob.mid(7, 24)), a("dpapi1:"), a("Basic"),
                                  a("Proxy-Authorization")};
        const QRegularExpression portWord(QString::fromLatin1("\\b%1\\b").arg(bridgePort));
        const auto clean = [&](const QString& where, const QString& text) {
            for (const QString& s : secrets)
                QVERIFY2(!text.contains(s, Qt::CaseInsensitive), qPrintable(where + a(": ") + s));
            QVERIFY2(!portWord.match(text).hasMatch(), qPrintable(where + a(": the bridge's port")));
        };
        for (const bool includeNames : {false, true}) {
            const QString report = diag::format(f, includeNames, dir.path());
            QVERIFY(report.contains(a("\nChat proxy\n  Mode: SOCKS5 proxy elsewhere, login: yes\n")));
            QVERIFY(report.contains(a("[proxy] mode custom (socks5, elsewhere, login yes)")));
            clean(a("report"), report);
        }
        // logNoSecrets: TeamSpeak's log, the plugin log file and the chat lines of this run.
        QVERIFY(fakets3::logContains(a("[proxy] route down: nameNotFound (custom)")));
        QVERIFY(fakets3::logContains(a("[proxy] test: nameNotFound")));
        clean(a("TeamSpeak log"), fakets3::logLines().join(QLatin1Char('\n')));
        clean(a("plugin log"), pluginLog.join(QLatin1Char('\n')));
        clean(a("chat lines"), fakets3::channelPrints().join(QLatin1Char('\n')));
        QVERIFY(fakets3::channelPrints().first().contains(a("The chat proxy isn't working: can't find the proxy's address.")));
        // settings.ini holds the blob, never the plain password.
        QFile ini(file);
        QVERIFY(ini.open(QIODevice::ReadOnly));
        const QByteArray iniBytes = ini.readAll();
        QVERIFY(!iniBytes.contains("secret-pass"));
        QVERIFY(iniBytes.contains("proxyPassword"));

        control.reset();
        plog::shutdown();
        ts3::pluginId.clear();
    }

    // design 2.4 (S18): the live-test driver's echo of a script line never shows a password.
    void selftestEchoMasksPassword()
    {
        const auto echo = [](const char* line) { return selftest::echoLine(QString::fromLatin1(line)); };
        QCOMPARE(echo("{\"cmd\":\"proxy.set\",\"host\":\"127.0.0.1\",\"port\":18080,\"user\":\"tsmtest\",\"password\":\"secret-pass\"}"),
                 a("{\"cmd\":\"proxy.set\",\"host\":\"127.0.0.1\",\"port\":18080,\"user\":\"tsmtest\",\"password\":\"***\"}"));
        QCOMPARE(echo("{\"cmd\":\"proxy.set\", \"Password\" : \"a\\\"b,c}\"}"), a("{\"cmd\":\"proxy.set\", \"Password\" : \"***\"}"));
        QCOMPARE(echo("proxy.set user=tsmtest password=secret-pass on=true"), a("proxy.set user=tsmtest password=*** on=true"));
        // A line cut off inside the value (the driver masks before cutting, and this still holds).
        QCOMPARE(echo("{\"cmd\":\"proxy.set\",\"password\":\"secret-pa"), a("{\"cmd\":\"proxy.set\",\"password\":\"***"));
        QCOMPARE(echo("{\"cmd\":\"proxy.toggle\",\"to\":\"on\"}"), a("{\"cmd\":\"proxy.toggle\",\"to\":\"on\"}"));
        // The driver uses it, masking the whole line before it is cut to 400 characters.
        const QString driver = sourceText(a("src/selftest.cpp"));
        QVERIFY(driver.contains(a("say(QString::fromLatin1(\"> \") + selftest::echoLine(line).left(400));")));
        for (const char* command : {"proxy.state", "proxy.set", "proxy.toggle", "proxy.test", "proxy.find", "proxy.records", "ytplayer.netlog"})
            QVERIFY2(driver.contains(QString::fromLatin1("QLatin1String(\"%1\")").arg(QString::fromLatin1(command))), command);
    }

    // design 8.4: the update consent names the YouTube player too.
    void updateConsentText()
    {
        const QString dialog = sourceText(a("src/update/updatedialog.cpp"));
        QVERIFY(dialog.contains(a("\"Apart from link previews and the YouTube player, this is the only internet connection the plugin makes. GitHub \"")));
        QVERIFY(!dialog.contains(a("Apart from link previews, this is the only")));
    }

    // design 7, 9.5 test 5: the docs' exact claims.
    void docsChatProxy()
    {
        const QString readme = sourceText(a("README.md")), changelog = sourceText(a("CHANGELOG.md")), faq = sourceText(a("docs/FAQ.md")),
                      settings = sourceText(a("docs/SETTINGS.md")), usage = sourceText(a("docs/USAGE.md")),
                      privacy = sourceText(a("docs/PRIVACY-SECURITY.md")), architecture = sourceText(a("docs/ARCHITECTURE.md"));
        QVERIFY(!readme.isEmpty() && !faq.isEmpty() && !settings.isEmpty() && !usage.isEmpty() && !privacy.isEmpty() && !architecture.isEmpty());

        // The FAQ's VPN-app recipe names exactly the verified default ports, and Find my VPN app tries the same ones.
        QVERIFY(faq.contains(a("### How do I use TS Media with my VPN app?")));
        for (const char* line : {"- v2rayN: 10808 (one port for SOCKS5 and HTTP)", "- Throne and Nekoray: 2080 (SOCKS5 and HTTP)",
                                 "- Clash Verge Rev: 7897 (mixed port)",
                                 "- Hiddify: 12334; set its service mode to *Proxy service only* or *Set system proxy*, not *VPN*",
                                 "- Outline has no local proxy port (it is always a full VPN), so it can't be used this way"})
            QVERIFY2(faq.contains(QString::fromUtf8(line)), line);
        QCOMPARE(FindRun::Options().ports, QVector<quint16>({10808, 2080, 7897, 12334}));
        for (const char* question : {"### Will the chat proxy break my connection to my TeamSpeak server?",
                                     "### Link previews say \"Paused · the proxy isn't working\"", "### A link says \"Your network blocks this site\"",
                                     "### The Test says \"The proxy can't reach the website\"", "### Iranian sites don't load with the chat proxy on"})
            QVERIFY2(faq.contains(QString::fromUtf8(question)), question);

        // SETTINGS: every key of design 2.1, the DPAPI wording, Restore defaults, the window's group.
        for (const char* key : {"proxyOn", "proxyWhenOff", "proxyType", "proxyHost", "proxyPort", "proxyUser", "proxyPassword", "proxyBridgePort"})
            QVERIFY2(settings.contains(QString::fromLatin1("`%1`").arg(QString::fromLatin1(key))), key);
        QVERIFY(settings.contains(a("Windows DPAPI-protected for your Windows account and this proxy server, never plain text")));
        QVERIFY(settings.contains(a("TS Media's local port for its proxy connection")));
        QVERIFY(settings.contains(a("### Chat proxy")));
        QVERIFY(settings.contains(a("**OK**, **Apply** and **Restore defaults** never change the chat proxy")));

        // PRIVACY-SECURITY: the section, and what never goes through the chat proxy (S19).
        const int section = privacy.indexOf(a("\n## Chat proxy\n"));
        QVERIFY(section > 0);
        const QString chatProxy = privacy.mid(section, privacy.indexOf(a("\n## "), section + 5) - section);
        const int     never     = chatProxy.indexOf(a("**What never goes through it:**"));
        QVERIFY(never > 0);
        const QString neverLine = chatProxy.mid(never, chatProxy.indexOf(QLatin1Char('\n'), never) - never);
        for (const char* item : {"`[img]` tags", "certificate chain", "links you open in your browser", "file transfers", "voice"})
            QVERIFY2(neverLine.contains(QString::fromUtf8(item)), item);
        for (const char* item : {"unless you installed the proxy's own certificate", "DPAPI", "only when that proxy asks for it", "Fail closed",
                                 "127.0.0.1 only", "WebRTC", "as safe as your trust in its certificate"})
            QVERIFY2(privacy.contains(QString::fromUtf8(item)), item);

        // No doc says "Couldn't load the preview" is the only error text any more.
        const QStringList docs{readme, faq, settings, usage, privacy};
        for (const QString& doc : docs) {
            QVERIFY(!doc.contains(a("whatever the reason")));
            QVERIFY(!doc.contains(a("Couldn't load the preview* with **Retry**, whatever")));
        }

        // The YouTube switch's new name everywhere; the old one only where SETTINGS says it was renamed (and in the
        // CHANGELOG's history).
        int oldName = 0;
        for (const QString& doc : docs + QStringList{architecture})
            oldName += doc.count(a("Play YouTube videos in the chat"));
        QCOMPARE(oldName, 1);
        QVERIFY(settings.contains(a("(before 2.6 this switch was called *Play YouTube videos in the chat*)")));
        QVERIFY(settings.contains(a("| Play YouTube videos in a TS Media window | on |")));

        // README: the feature bullet and the privacy link; CHANGELOG: 2.6.0 above 2.5.0, dated by the release.
        QVERIFY(readme.contains(a("- **Chat proxy (new in 2.6).** Link previews, the YouTube player and update checks can go through an HTTP or SOCKS5 proxy")));
        QVERIFY(readme.contains(a("docs/PRIVACY-SECURITY.md#chat-proxy")));
        QVERIFY(changelog.indexOf(a("## [2.6.0] - ")) >= 0 && changelog.indexOf(a("## [2.6.0] - ")) < changelog.indexOf(a("## [2.5.0] - ")));
        QVERIFY(usage.contains(a("| `/tsmedia proxy` |")) && usage.contains(a("| Turn the chat proxy on or off |")));
        // Review fix: after a turn-off only the links that waited for or failed through a proxy wait for a click (design 6
        // step 3); links that never started load through the new route, and the docs say so instead of promising more.
        for (const QString& doc : docs)
            QVERIFY(!doc.contains(a("never loaded without it unless you ask")));
        QVERIFY(usage.contains(a("links that hadn't started loading yet (off screen, or waiting for their turn) load through it by themselves")));
        QVERIFY(privacy.contains(a("links that hadn't started loading yet load through the new route as usual")));
        QVERIFY(architecture.contains(a("## Chat proxy")) && architecture.contains(a("`proxy::shutdown()`")));
    }

    // Every link to a heading inside the docs ("FILE.md#anchor", "#anchor") names a heading that exists (GitHub's anchors).
    void docsAnchors()
    {
        QStringList files{a("README.md"), a("CHANGELOG.md")};
        for (const QString& f : QDir(QString::fromUtf8(TSMEDIA_SOURCE_DIR) + a("/docs")).entryList({a("*.md")}, QDir::Files))
            files << a("docs/") + f;
        const auto slug = [](QString heading) {
            heading.remove(QLatin1Char('`'));
            heading.replace(QRegularExpression(a("\\[([^\\]]*)\\]\\([^)]*\\)")), a("\\1"));
            heading = heading.trimmed().toLower();
            heading.remove(QRegularExpression(a("[^\\w\\- ]"), QRegularExpression::UseUnicodePropertiesOption));
            return heading.replace(QLatin1Char(' '), QLatin1Char('-'));
        };
        QHash<QString, QSet<QString>> anchors;
        for (const QString& f : files) {
            QString text = sourceText(f);
            text.remove(QRegularExpression(a("```.*?```"), QRegularExpression::DotMatchesEverythingOption));
            QHash<QString, int> seen;
            QSet<QString>       out;
            auto                it = QRegularExpression(a("^#{1,6} (.+)$"), QRegularExpression::MultilineOption).globalMatch(text);
            while (it.hasNext()) {
                const QString s = slug(it.next().captured(1));
                const int     n = seen.value(s);
                seen.insert(s, n + 1);
                out.insert(n == 0 ? s : s + a("-%1").arg(n));
            }
            anchors.insert(QDir::cleanPath(f), out);
        }
        int links = 0;
        for (const QString& f : files) {
            auto it = QRegularExpression(a("\\]\\(([^)\\s]*?)#([^)\\s]+)\\)")).globalMatch(sourceText(f));
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                const QString target = m.captured(1), anchor = m.captured(2);
                if (target.startsWith(a("http")))
                    continue;
                const QString path = target.isEmpty() ? f : QDir::cleanPath(QFileInfo(f).path() + QLatin1Char('/') + target);
                if (!anchors.contains(path))
                    continue;
                ++links;
                QVERIFY2(anchors.value(path).contains(anchor), qPrintable(f + a(" -> ") + target + QLatin1Char('#') + anchor));
            }
        }
        QVERIFY(links > 100);
    }
};

TSMEDIA_REGISTER_TEST(TestProxyDiag)
#include "tst_proxydiag.moc"
