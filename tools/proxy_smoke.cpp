// 2.6 chat proxy: proxy_smoke, a standalone check of the chat proxy against a real proxy (design 9.2). Never run by the
// unit tests; for the live run and the developer.
//
//   proxy_smoke --socks5|--http host:port [--user u --password p] [--test] [--find] [--fetch URL] [--verbose]
//
//   --test       the Test button's job: one GET https://www.youtube.com/generate_204 through a temporary bridge
//   --find       Find my VPN app: a SOCKS5 greeting to 127.0.0.1 at 10808, 2080, 7897, 12334 (nothing leaves the PC)
//   --fetch URL  one link-preview load (a click) through a bridge to the proxy, with the real WinHTTP transport
//   --verbose    the bridge's records of the load (host, step, failure, bytes)
//
// It prints the classification and the bridge's record; never the password, the bridge's token or its realm.

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "linkpreview/fetcher.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/transport.h"
#include "proxy/bridge.h"
#include "proxy/findrun.h"
#include "proxy/protect.h"
#include "proxy/proxyconfig.h"
#include "proxy/router.h"
#include "proxy/testrun.h"
#include "video/mfvideo.h"

using namespace proxy;

namespace {

void say(const QString& line)
{
    std::fprintf(stdout, "%s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

QString recordLine(const Record& r)
{
    return QStringLiteral("  record %1%2 host=%3 step=%4 failure=%5 code=%6 loginUnused=%7 up=%8 down=%9")
        .arg(QChar::fromLatin1(r.client ? r.client : '-'))
        .arg(r.id)
        .arg(r.host.isEmpty() ? QStringLiteral("-") : r.host)
        .arg(QString::fromLatin1(stepName(r.step)), QString::fromLatin1(failureName(r.failure)))
        .arg(r.code)
        .arg(r.loginUnused ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(r.up)
        .arg(r.down);
}

int usage()
{
    say(QStringLiteral("usage: proxy_smoke --socks5|--http host:port [--user u --password p] [--test] [--find] [--fetch URL] [--verbose]"));
    return 3;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication   app(argc, argv); // picture formats come from Qt's image plugins
    const QStringList args = app.arguments();
    Config            form;
    bool              haveProxy = false, test = false, find = false, verbose = false;
    QString           fetchUrl, password;
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args.at(i);
        if ((a == QLatin1String("--socks5") || a == QLatin1String("--http")) && i + 1 < args.size()) {
            form.type          = a == QLatin1String("--http") ? Type::Http : Type::Socks5;
            const QString link = args.at(++i);
            const int     colon = link.lastIndexOf(QLatin1Char(':'));
            form.host          = normalizeHost(link.left(colon));
            form.port          = static_cast<quint16>(link.mid(colon + 1).toUInt());
            haveProxy          = !form.host.isEmpty() && form.port != 0;
        } else if (a == QLatin1String("--user") && i + 1 < args.size()) {
            form.user = args.at(++i);
        } else if (a == QLatin1String("--password") && i + 1 < args.size()) {
            password = args.at(++i);
        } else if (a == QLatin1String("--test")) {
            test = true;
        } else if (a == QLatin1String("--find")) {
            find = true;
        } else if (a == QLatin1String("--fetch") && i + 1 < args.size()) {
            fetchUrl = args.at(++i);
        } else if (a == QLatin1String("--verbose")) {
            verbose = true;
        } else {
            return usage();
        }
    }
    if (!find && !haveProxy)
        return usage();
    // The transport refuses everything until a route is published: Windows settings by default, the bridge for --fetch.
    {
        auto snap        = std::make_shared<Snapshot>();
        snap->mode       = Mode::System;
        snap->generation = 1;
        snap->refused    = Failure::None;
        publish(snap);
    }
    int exitCode = 0;

    if (find) {
        FindRun run;
        QObject::connect(&run, &FindRun::finished, &app, &QCoreApplication::quit);
        run.start();
        app.exec();
        say(QStringLiteral("find: ") + findResultText(run.found()));
        exitCode = run.found().isEmpty() ? 1 : exitCode;
    }

    if (test && haveProxy) {
        TestRun run;
        QObject::connect(&run, &TestRun::finished, &app, &QCoreApplication::quit);
        QByteArray           utf8 = password.toUtf8();
        protect::SecretBytes typed(utf8.constData(), utf8.size());
        utf8.fill('\0');
        password.fill(QLatin1Char(' '));
        run.start(form, std::move(typed), QByteArray());
        app.exec();
        const TestResult r = run.result();
        say(QStringLiteral("test: %1 (failure %2, step %3, status %4, %5 ms)")
                .arg(testResultText(r, hostPort(form.host, form.port), form.port), QString::fromLatin1(failureName(r.failure)),
                     QString::fromLatin1(stepName(r.step)))
                .arg(r.status)
                .arg(r.ms));
        exitCode = testResultOk(r) ? exitCode : 1;
    }

    if (!fetchUrl.isEmpty() && haveProxy) {
        Bridge::Options options;
        int             error  = 0;
        auto            bridge = Bridge::create(options, &error);
        if (!bridge) {
            say(QStringLiteral("fetch: the bridge couldn't start (%1)").arg(error));
            return 2;
        }
        auto spec        = std::make_unique<UpstreamSpec>();
        spec->type       = form.type;
        spec->host       = form.host;
        spec->port       = form.port;
        spec->user       = form.user;
        spec->generation = 2;
        if (!password.isEmpty()) {
            QByteArray utf8 = password.toUtf8();
            spec->opened    = std::make_shared<protect::SecretBytes>(utf8.constData(), utf8.size());
            utf8.fill('\0');
        }
        bridge->setUpstream(std::move(spec));
        auto snap        = std::make_shared<Snapshot>();
        snap->mode       = Mode::Custom;
        snap->generation = 2;
        snap->refused    = Failure::None;
        snap->bridgePort = bridge->port();
        snap->token      = bridge->token();
        snap->realm      = bridge->realm();
        publish(snap);
        setLiveRecords(bridge);

        mf::startup();
        QTemporaryDir                dir;
        linkpreview::Fetcher::Options fetchOptions;
        fetchOptions.records = bridge;
        auto* fetcher        = new linkpreview::Fetcher(linkpreview::http::winHttpTransport(), fetchOptions, &app);
        fetcher->setLogger([](const QString& line) { say(QStringLiteral("log: ") + line); });
        linkpreview::Caps caps;
        caps.media     = 25 * 1024 * 1024;
        caps.userAsked = true;
        QElapsedTimer clock;
        clock.start();
        QObject::connect(fetcher, &linkpreview::Fetcher::finished, &app, [&](const QString&, const linkpreview::Result& r) {
            say(QStringLiteral("fetch: %1%2 via=%3 proxyFailure=%4/%5 routeOk=%6 %7 ms")
                    .arg(r.ok ? QStringLiteral("ok") : QStringLiteral("error ") + r.error)
                    .arg(r.ok ? QStringLiteral(" (%1)").arg(QString::fromLatin1(linkpreview::kindName(r.kind))) : QString())
                    .arg(r.via == Via::Bridge ? QStringLiteral("chat proxy") : QString::number(static_cast<int>(r.via)))
                    .arg(QString::fromLatin1(failureName(r.proxyFailure)), QString::fromLatin1(stepName(r.proxyStep)))
                    .arg(r.routeOk ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(clock.elapsed()));
            exitCode = r.ok ? exitCode : 1;
            app.quit();
        });
        fetcher->load(linkpreview::classify(linkpreview::trimLinkEnd(fetchUrl)), caps, dir.path());
        app.exec();
        if (verbose) {
            for (const Record& r : bridge->recent('f', 0))
                say(recordLine(r));
        }
        const Bridge::Counters c = bridge->counters();
        say(QStringLiteral("bridge: accepted %1, refused %2, busy %3, unauthenticated %4").arg(c.accepted).arg(c.refused).arg(c.busy).arg(c.unauthenticated));
        fetcher->shutdown();
        delete fetcher;
        bridge->stop(2000);
        setLiveRecords(nullptr);
        mf::shutdown();
    }
    return exitCode;
}
