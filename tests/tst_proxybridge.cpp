// 2.6 chat proxy, sockets and integration (design 9.2, tests 13-31): the bridge against in-test fake SOCKS5 and HTTP
// proxies on 127.0.0.1 (client side, upstream, limits, epochs, bounded stop, relay), the real WinHTTP transport through
// the bridge and its route guards, the Windows proxy's setup-script answers, the Fetcher's routes and proxy
// classification, proxy::Control, the Test and Find jobs, the updater's plan and the source rules. Nothing leaves
// 127.0.0.1.

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <atomic>
#include <thread>

#include "linkpreview/fetcher.h"
#include "linkpreview/hostgate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/transport.h"
#include "proxy/bridge.h"
#include "proxy/control.h"
#include "proxy/findrun.h"
#include "proxy/protect.h"
#include "proxy/router.h"
#include "proxy/testrun.h"
#include "proxy/upstream.h"
#include "proxy/windowsproxy.h"
#include "linkfixtures.h"
#include "proxyfakes.h"
#include "testmain.h"
#include "update/sessionplan.h"

using namespace proxy;
using namespace proxyfakes;
namespace lp = linkpreview;

namespace {

constexpr unsigned long kFlagInvalidCa       = 0x00000010; // WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA
constexpr unsigned long kFlagCertDateInvalid = 0x00000002; // ..._CERT_DATE_INVALID

void publishMode(Mode mode, quint64 generation = 1, quint16 bridgePort = 0, const QByteArray& token = QByteArray(), Failure refused = Failure::None)
{
    auto s        = std::make_shared<Snapshot>();
    s->mode       = mode;
    s->generation = generation;
    s->refused    = refused;
    s->bridgePort = bridgePort;
    s->token      = token;
    s->realm      = QByteArray("tsmedia-0011223344556677");
    publish(s);
}

void publishWindowsRoute()
{
    publishMode(Mode::System);
}

Bridge::Options bridgeOptions()
{
    Bridge::Options o;
    o.localSuffixes = [] { return QStringList{QStringLiteral("corp.example.net")}; };
    return o;
}

std::unique_ptr<UpstreamSpec> upstream(Type type, quint16 port, const char* user = "", const char* password = nullptr, const char* host = "127.0.0.1",
                                       quint64 generation = 1)
{
    auto spec        = std::make_unique<UpstreamSpec>();
    spec->type       = type;
    spec->host       = QString::fromLatin1(host);
    spec->port       = port;
    spec->user       = QString::fromLatin1(user);
    spec->generation = generation;
    if (password)
        spec->opened = std::make_shared<protect::SecretBytes>(password, static_cast<int>(strlen(password)));
    return spec;
}

QByteArray statusLine(const QByteArray& head)
{
    return head.left(head.indexOf("\r\n"));
}

// Waits (pumping events) until fn or the time-out.
template <typename Fn>
bool waitFor(Fn fn, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!fn()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return true;
}

// A thread that calls send() until it fails, at most 8 s, and is always joined (also when a check returns early).
class Flood
{
  public:
    explicit Flood(std::function<bool()> send)
        : m_thread([this, send]() {
            QElapsedTimer t;
            t.start();
            while (!m_done.load() && t.elapsed() < 8000 && send()) {
            }
        })
    {
    }
    ~Flood()
    {
        m_done = true;
        m_thread.join();
    }

  private:
    std::atomic_bool m_done{false};
    std::thread      m_thread;
};

Record recordOf(const std::shared_ptr<Bridge>& bridge, char client, quint64 id)
{
    Record r;
    waitFor([&]() { return bridge->outcome(client, id, &r) && (r.step == Step::Done || r.step == Step::Relay || r.failure != Failure::None); }, 3000);
    return r;
}

// One CONNECT through the bridge: the status line and the client (kept open).
struct Tunnel {
    std::unique_ptr<Client> client;
    QByteArray              head;
    QByteArray              status() const { return statusLine(head); }
};

Tunnel connectThrough(const std::shared_ptr<Bridge>& bridge, const QByteArray& authority, quint64 id, const QByteArray& extra = QByteArray())
{
    Tunnel t;
    t.client.reset(new Client(bridge->port()));
    t.client->send(connectRequest(authority, "tsm-f" + QByteArray::number(id), bridge->token()) + extra);
    t.head = t.client->readHead();
    return t;
}

// A link-preview transport that speaks to the bridge itself with a plain socket: CONNECT with the route's login, then
// the test site's answer (204 when the tunnel stands). For the Test job without TLS.
class BridgeSocketTransport : public lp::http::Transport
{
  public:
    lp::http::Error resolve(const QString&, QVector<lp::net::Address>*, const lp::http::Cancel&, quint64) override { return lp::http::Error::Unavailable; }
    std::unique_ptr<lp::http::Exchange> open(const lp::http::HopOptions& options, const lp::http::Cancel& cancel, lp::http::Error* error) override
    {
        *error = lp::http::Error::None;
        const quint16 port = static_cast<quint16>(options.route.proxy.section(QLatin1Char(':'), 1).toInt());
        auto          c    = std::make_shared<Client>(port, 10000);
        c->send(connectRequest(options.url.host().toLatin1() + ":443", options.route.bridgeUser, options.route.bridgeToken));
        if (cancel.isSet()) {
            *error = lp::http::Error::Canceled;
            return nullptr;
        }
        const QByteArray head = c->readHead();
        if (head.isEmpty()) {
            *error = lp::http::Error::Offline;
            return nullptr;
        }
        int  status = statusLine(head).mid(9, 3).toInt();
        bool proxy  = true;
        if (status == 200) {
            c->send("PING");
            const QByteArray echoed = head.mid(head.indexOf("\r\n\r\n") + 4) + c->readCount(4 - (head.size() - head.indexOf("\r\n\r\n") - 4));
            if (!echoed.endsWith("PING")) {
                *error = lp::http::Error::Offline; // the tunnel fell over (an optimistic proxy)
                return nullptr;
            }
            status = 204;
            proxy  = false;
        }
        return std::unique_ptr<lp::http::Exchange>(new Answer(c, status, proxy));
    }

  private:
    class Answer : public lp::http::Exchange
    {
      public:
        Answer(std::shared_ptr<Client> c, int status, bool proxy)
            : m_client(std::move(c))
            , m_status(status)
            , m_proxy(proxy)
        {
        }
        lp::http::Error head(lp::http::Head* out) override
        {
            out->status               = m_status;
            out->proxyConnectResponse = m_proxy;
            out->peer                 = lp::net::Address::fromIPv4(0x7F000001u);
            return lp::http::Error::None;
        }
        lp::http::Error read(char*, int, int* got) override
        {
            *got = 0;
            return lp::http::Error::None;
        }

      private:
        std::shared_ptr<Client> m_client;
        int                     m_status;
        bool                    m_proxy;
    };
};

// A tiny PAC server: GET /<case> answers FindProxyForURL returning the case's string.
class PacServer : public FakeServer
{
  public:
    PacServer() { start(); }
    ~PacServer() override { stop(); }
    QHash<QByteArray, QByteArray> answers; // path -> the returned string
    QSet<QByteArray>              slow;    // paths that are never answered (until the server stops)

  protected:
    void serve(uintptr_t socket) override
    {
        const SOCKET s = static_cast<SOCKET>(socket);
        QByteArray   head;
        char         c = 0;
        while (!head.endsWith("\r\n\r\n") && head.size() < 8192 && ::recv(s, &c, 1, 0) == 1)
            head.append(c);
        const QByteArray target = head.split(' ').value(1);
        const QByteArray path   = target.left(target.indexOf('?') >= 0 ? target.indexOf('?') : target.size());
        if (slow.contains(path)) {
            while (!m_stop.load())
                Sleep(20);
            return;
        }
        const QByteArray answer = answers.value(path, "DIRECT");
        const QByteArray body   = "function FindProxyForURL(url, host) { return \"" + answer + "\"; }\n";
        const QByteArray reply  = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nContent-Length: " + QByteArray::number(body.size())
                                 + "\r\nConnection: close\r\n\r\n" + body;
        ::send(s, reply.constData(), reply.size(), 0);
    }
};

// ---- Fetcher harness -------------------------------------------------------------------------------------------------

lp::Caps clickCaps()
{
    lp::Caps caps;
    caps.media     = 25 * 1024 * 1024;
    caps.picture   = 2 * 1024 * 1024;
    caps.page      = 512 * 1024;
    caps.userAsked = true;
    return caps;
}

std::shared_ptr<lp::HostGate> permissiveGate()
{
    auto                 clock = std::make_shared<std::atomic<qint64>>(0);
    lp::HostGate::Limits limits;
    limits.pageRunning = limits.mediaRunning = 1000;
    return std::make_shared<lp::HostGate>([clock] { return clock->fetch_add(3600000) + 3600000; }, limits);
}

struct FetchHarness {
    explicit FetchHarness(lp::Fetcher::Options options = lp::Fetcher::Options())
    {
        options.totalTimeoutMs = 15000;
        if (!options.gate)
            options.gate = permissiveGate();
        if (!options.localSuffixes)
            options.localSuffixes = [] { return QStringList{QStringLiteral("corp.example.net")}; };
        options.records = records;
        fetcher.reset(new lp::Fetcher(http, options));
        fetcher->setLogSink([this](const lp::LogLine& line) { logs << line.text(); });
    }
    lp::Result load(const QString& href, const lp::Caps& caps = clickCaps())
    {
        QSignalSpy spy(fetcher.get(), &lp::Fetcher::finished);
        fetcher->load(lp::classify(href), caps, dir.path());
        if (!spy.wait(20000) && spy.isEmpty())
            return lp::Result();
        return spy.first().at(1).value<lp::Result>();
    }
    // A bridge hop's id from its route (tsm-f<id>).
    static quint64 hopId(const lp::http::HopOptions& options) { return options.route.bridgeUser.mid(5).toULongLong(); }

    QTemporaryDir                      dir;
    std::shared_ptr<ScriptedTransport> http    = std::make_shared<ScriptedTransport>();
    std::shared_ptr<FakeRecords>       records = std::make_shared<FakeRecords>();
    std::unique_ptr<lp::Fetcher>       fetcher;
    QStringList                        logs;
};

ScriptedHop png()
{
    ScriptedHop hop;
    hop.body        = pngBytes();
    hop.contentType = "image/png";
    return hop;
}

const char kPng[] = "https://cdn.example.com/a.png";

QString sourceText(const QString& relative)
{
    QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/") + relative);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TestProxyBridge : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        protect::setUnprotectErrorForTests(0);
        windowsproxy::setIeConfigProviderForTests({});
        lp::http::setAnyLoopbackBridgeForTests(false);
        publishWindowsRoute();
        markProxyRouteChange(0);
    }

    // 13. what the bridge accepts (S17)
    void bridgeClientSide()
    {
        FakeSocks fake;
        auto      bridge = Bridge::create(bridgeOptions());
        QVERIFY(bridge);
        QVERIFY(bridge->port() >= 1024);
        QCOMPARE(bridge->token().size(), 43);
        QVERIFY(bridge->realm().startsWith("tsmedia-"));
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));

        const auto ask = [&](const QByteArray& request) {
            Client c(bridge->port());
            c.send(request);
            bool             closed = false;
            const QByteArray answer = c.readToClose(&closed);
            return qMakePair(answer, closed);
        };
        // no login, a wrong token: 407 with the realm, closed
        auto r = ask(connectRequest("www.youtube.com:443", "", ""));
        QVERIFY2(r.first.startsWith("HTTP/1.1 407"), r.first.constData());
        QVERIFY(r.first.contains("Proxy-Authenticate: Basic realm=\"" + bridge->realm() + "\""));
        QVERIFY(r.second);
        r = ask(connectRequest("www.youtube.com:443", "tsm-f1", "not-the-token"));
        QVERIFY(r.first.startsWith("HTTP/1.1 407"));
        // GET: 405 without a login prompt
        r = ask("GET http://www.youtube.com/ HTTP/1.1\r\nHost: www.youtube.com\r\n\r\n");
        QVERIFY(r.first.startsWith("HTTP/1.1 405"));
        QVERIFY(!r.first.contains("Proxy-Authenticate"));
        // garbage, a head over 8 KB, a body: 400
        QVERIFY(ask("hello there\r\n\r\n").first.startsWith("HTTP/1.1 400"));
        QVERIFY(ask(QByteArray("CONNECT www.youtube.com:443 HTTP/1.1\r\n") + QByteArray(9000, 'x')).first.startsWith("HTTP/1.1 400"));
        QVERIFY(ask(connectRequest("www.youtube.com:443", "tsm-f1", bridge->token()).replace("\r\n\r\n", "\r\nContent-Length: 5\r\n\r\nhello"))
                    .first.startsWith("HTTP/1.1 400"));
        // every error answer: no body, Content-Length: 0, Connection: close, no echo of the request
        for (const QByteArray& request : {connectRequest("www.youtube.com:443", "", ""), QByteArray("hello there\r\n\r\n"),
                                          connectRequest("127.0.0.1:443", "tsm-f1", bridge->token())}) {
            const QByteArray answer = ask(request).first;
            QVERIFY(answer.endsWith("\r\n\r\n"));
            QVERIFY(answer.contains("Content-Length: 0\r\n"));
            QVERIFY(answer.contains("Connection: close\r\n"));
            QVERIFY(!answer.contains("youtube") && !answer.contains("hello") && !answer.contains(bridge->token()));
        }
        // a grammar or policy refusal never connects upstream
        QCOMPARE(fake.connections(), 0);
        QVERIFY(ask(connectRequest("127.0.0.1:443", "tsm-f1", bridge->token())).first.startsWith("HTTP/1.1 403"));
        QVERIFY(ask(connectRequest("router.lan:443", "tsm-f1", bridge->token())).first.contains("X-TSMedia-Proxy: refused"));
        QCOMPARE(fake.connections(), 0);
        // a head slower than 2 s: closed without an answer
        {
            QElapsedTimer t;
            t.start();
            Client c(bridge->port());
            c.send("CONNECT www.yout");
            bool closed = false;
            QVERIFY(c.readToClose(&closed).isEmpty());
            QVERIFY(closed);
            QVERIFY(t.elapsed() >= 1500 && t.elapsed() < 4500);
        }
        // pipelined bytes are forwarded after the 200
        Tunnel t = connectThrough(bridge, "www.youtube.com:443", 7, "PIPELINED");
        QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
        QByteArray after = t.head.mid(t.head.indexOf("\r\n\r\n") + 4);
        after += t.client->readCount(9 - after.size());
        QCOMPARE(after, QByteArray("PIPELINED"));
        QCOMPARE(fake.seen().last().target, QByteArray("www.youtube.com"));
        QVERIFY(bridge->stop(2000));
    }

    // 14. SOCKS5 upstream
    void bridgeSocksUpstream()
    {
        // ATYP 3 with the validated, normalized name; relay both ways
        {
            FakeSocks fake;
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel t = connectThrough(bridge, "WWW.YouTube.COM.:443", 1);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            t.client->send("hello");
            QCOMPARE(t.client->readCount(5), QByteArray("hello"));
            QCOMPARE(fake.seen().value(0).atyp, 3);
            QCOMPARE(fake.seen().value(0).target, QByteArray("www.youtube.com"));
            QCOMPARE(fake.seen().value(0).greeting, QByteArray("\x05\x01\x00", 3));
            const Record r = recordOf(bridge, 'f', 1);
            QCOMPARE(r.host, QStringLiteral("www.youtube.com"));
            QVERIFY(r.established());
            bridge->stop(2000);
        }
        // a login offered [00, 02]; RFC 1929 with the password
        {
            SocksScript script;
            script.method = 0x02;
            FakeSocks fake(script);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port(), "tester", "proxy-pass-1"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 2);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QCOMPARE(fake.seen().value(0).greeting, QByteArray("\x05\x02\x00\x02", 4));
            QCOMPARE(fake.seen().value(0).loginUser, QByteArray("tester"));
            QCOMPARE(fake.seen().value(0).loginPass, QByteArray("proxy-pass-1"));
            QVERIFY(!recordOf(bridge, 'f', 2).loginUnused);
            bridge->stop(2000);
        }
        // 00 chosen with a login: success, loginUnused
        {
            FakeSocks fake;
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port(), "tester", "pw"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 3);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QVERIFY(recordOf(bridge, 'f', 3).loginUnused);
            bridge->stop(2000);
        }
        // the failures: each gets 502 + X-TSMedia-Proxy and its record
        struct Case {
            const char* name;
            SocksScript script;
            const char* user;
            Failure     failure;
            Step        step;
        };
        const auto script = [](quint8 method, int rep, bool loginOk = true, quint8 atyp = 1) {
            SocksScript s;
            s.method  = method;
            s.rep     = rep;
            s.loginOk = loginOk;
            s.atyp    = atyp;
            return s;
        };
        const Case cases[] = {
            {"FF with a login", script(0xFF, 0), "tester", Failure::LoginMethod, Step::Greeting},
            {"FF without", script(0xFF, 0), "", Failure::NeedsLogin, Step::Greeting},
            {"02 without a login", script(0x02, 0), "", Failure::NeedsLogin, Step::Greeting},
            {"login refused", script(0x02, 0, false), "tester", Failure::LoginRefused, Step::Login},
            {"REP 1", script(0, 1), "", Failure::CantReachSite, Step::Request},
            {"REP 2", script(0, 2), "", Failure::NotAllowed, Step::Request},
            {"REP 3", script(0, 3), "", Failure::CantReachSite, Step::Request},
            {"REP 4", script(0, 4), "", Failure::CantReachSite, Step::Request},
            {"REP 5", script(0, 5), "", Failure::SiteRefused, Step::Request},
            {"REP 6", script(0, 6), "", Failure::CantReachSite, Step::Request},
            {"REP 7", script(0, 7), "", Failure::NoNameConnect, Step::Request},
            {"REP 8", script(0, 8), "", Failure::NoNameConnect, Step::Request},
            {"unknown ATYP", script(0, 0, true, 9), "", Failure::NotSocks5, Step::Request},
        };
        quint64 id = 100;
        for (const Case& c : cases) {
            FakeSocks fake(c.script);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port(), c.user, *c.user ? "pw" : nullptr));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", ++id);
            QVERIFY2(t.status().startsWith("HTTP/1.1 502"), c.name);
            QVERIFY2(t.head.contains(QByteArray("X-TSMedia-Proxy: ") + failureName(c.failure) + "\r\n"), c.name);
            const Record r = recordOf(bridge, 'f', id);
            QVERIFY2(r.failure == c.failure, c.name);
            QVERIFY2(r.step == c.step, c.name);
            bridge->stop(2000);
        }
        // BND in the domain form; bytes after the reply forwarded after our 200 (S17)
        {
            SocksScript s;
            s.atyp            = 3;
            s.earlyAfterReply = "EARLY";
            FakeSocks fake(s);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 200);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QByteArray after = t.head.mid(t.head.indexOf("\r\n\r\n") + 4);
            after += t.client->readCount(5 - after.size());
            QCOMPARE(after, QByteArray("EARLY"));
            bridge->stop(2000);
        }
        // success, then EOF after the client's bytes: CantReachSite, recorded, the client closed
        {
            SocksScript s;
            s.after = After::CloseOnClientData;
            FakeSocks fake(s);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 201);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            t.client->send("\x16\x03\x01 client hello");
            QVERIFY(t.client->closedByPeer(3000));
            QVERIFY(waitFor([&]() {
                Record r;
                return bridge->outcome('f', 201, &r) && r.failure == Failure::CantReachSite;
            }));
            bridge->stop(2000);
        }
        // an unreadable blob: 502 passwordUnreadable and no connect to the fake
        {
            FakeSocks fake;
            auto      bridge = Bridge::create(bridgeOptions());
            auto      spec   = upstream(Type::Socks5, fake.port(), "tester");
            Endpoint elsewhere;
            elsewhere.host = QStringLiteral("10.9.9.9");
            elsewhere.port = 1;
            elsewhere.user = QStringLiteral("x");
            QVERIFY(protect::seal(QStringLiteral("pw"), elsewhere, &spec->passwordBlob)); // sealed for another endpoint
            bridge->setUpstream(std::move(spec));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 202);
            QVERIFY(t.status().startsWith("HTTP/1.1 502"));
            QVERIFY(t.head.contains("X-TSMedia-Proxy: passwordUnreadable"));
            QCOMPARE(fake.connections(), 0);
            bridge->stop(2000);
        }
        // a proxy name that resolves only into the sinkhole: refused before any connect (S8)
        {
            FakeSocks       fake;
            Bridge::Options o = bridgeOptions();
            std::atomic_int lookups{0};
            o.upstream.resolver = [&lookups](const QString&, QVector<lp::net::Address>* out, const HANDLE*, int, int) {
                ++lookups;
                *out = {lp::net::parseAddress(QStringLiteral("10.10.34.36"))};
                return 0;
            };
            auto bridge = Bridge::create(o);
            bridge->setUpstream(upstream(Type::Socks5, fake.port(), "", nullptr, "proxy.example.net"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 203);
            QVERIFY(t.head.contains("X-TSMedia-Proxy: proxyNameFiltered"));
            QCOMPARE(recordOf(bridge, 'f', 203).failure, Failure::ProxyNameFiltered);
            QCOMPARE(fake.connections(), 0);
            QCOMPARE(lookups.load(), 1);
            bridge->stop(2000);
        }
    }

    // 15. HTTP CONNECT upstream (S8)
    void bridgeHttpUpstream()
    {
        // on this PC: the CONNECT line with pre-emptive Basic
        {
            FakeHttpProxy fake;
            auto          bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "proxy-pass-1"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 1);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            const Seen s = fake.seen().value(0);
            QVERIFY(s.greeting.startsWith("CONNECT www.youtube.com:443 HTTP/1.1\r\nHost: www.youtube.com:443\r\n"));
            QVERIFY(s.authHeader);
            QCOMPARE(s.loginUser, QByteArray("tester"));
            QCOMPARE(s.loginPass, QByteArray("proxy-pass-1"));
            QVERIFY(!s.greeting.contains("User-Agent"));
            bridge->stop(2000);
        }
        Bridge::Options remote = bridgeOptions();
        remote.upstream.treatAsRemote = true;
        // elsewhere, a proxy that never challenges: success and no Proxy-Authorization ever sent
        {
            FakeHttpProxy fake;
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "proxy-pass-1"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 2);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QCOMPARE(fake.seen().size(), 1);
            QVERIFY(!fake.seen().value(0).authHeader);
            QVERIFY(recordOf(bridge, 'f', 2).loginUnused);
            bridge->stop(2000);
        }
        // a challenging proxy: one resend with Basic on the same connection
        {
            HttpScript script;
            script.challenge  = true;
            script.expectUser = "tester";
            script.expectPass = "proxy-pass-1";
            FakeHttpProxy fake(script);
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "proxy-pass-1"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 3);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QCOMPARE(fake.seen().size(), 2);
            QVERIFY(!fake.seen().value(0).authHeader);
            QVERIFY(fake.seen().value(1).authHeader);
            QCOMPARE(fake.connections(), 1);
            bridge->stop(2000);
        }
        // a second 407: LoginRefused
        {
            HttpScript script;
            script.challenge  = true;
            script.expectUser = "tester";
            script.expectPass = "the-right-one";
            FakeHttpProxy fake(script);
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "wrong"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 4);
            QVERIFY(t.head.contains("X-TSMedia-Proxy: loginRefused"));
            QCOMPARE(recordOf(bridge, 'f', 4).step, Step::Login);
            bridge->stop(2000);
        }
        // a 407 with Connection: close: the resend on a new connection
        {
            HttpScript script;
            script.challenge        = true;
            script.closeOnChallenge = true;
            script.expectUser       = "tester";
            script.expectPass       = "pw";
            FakeHttpProxy fake(script);
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "pw"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 5);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            QCOMPARE(fake.connections(), 2);
            bridge->stop(2000);
        }
        // Review fix: 407s that close the connection without saying so, and a long 407 page. Each ends in the tunnel, the
        // login sent once on a working connection; never "not HTTP".
        struct Challenge {
            const char* name;
            QByteArray  answer;
            bool        closes;
            int         connections;
        };
        const QByteArray basic = "Proxy-Authenticate: Basic realm=\"fake\"\r\n";
        const Challenge  challenges[] = {
            // gost / goproxy: HTTP/1.1, Content-Length 0, no Connection header, then closed: the resend finds it closed
            {"closes silently", "HTTP/1.1 407 Proxy Authentication Required\r\n" + basic + "Content-Length: 0\r\n\r\n", true, 2},
            // HTTP/1.0 without keep-alive: a new connection at once
            {"HTTP/1.0", "HTTP/1.0 407 Proxy Authentication Required\r\n" + basic + "Content-Length: 4\r\n\r\nnope", true, 2},
            // Proxy-Connection: close
            {"Proxy-Connection: close", "HTTP/1.1 407 Proxy Authentication Required\r\n" + basic + "Proxy-Connection: close\r\nContent-Length: 0\r\n\r\n", true, 2},
            // HTTP/1.0 with keep-alive keeps it
            {"HTTP/1.0 keep-alive", "HTTP/1.0 407 Proxy Authentication Required\r\n" + basic + "Proxy-Connection: keep-alive\r\nContent-Length: 0\r\n\r\n", false, 1},
            // a 20 KB page (inline styles and logos): drained by count, the resend on the same connection
            {"20 KB page", "HTTP/1.1 407 Proxy Authentication Required\r\n" + basic + "Content-Type: text/html\r\nContent-Length: 20000\r\n\r\n" + QByteArray(20000, 'p'),
             false, 1},
        };
        quint64 challengeId = 30;
        for (const Challenge& c : challenges) {
            HttpScript script;
            script.challenge           = true;
            script.challengeAnswer     = c.answer;
            script.closeAfterChallenge = c.closes;
            script.expectUser          = "tester";
            script.expectPass          = "pw";
            FakeHttpProxy fake(script);
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), "tester", "pw"));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", ++challengeId);
            QVERIFY2(t.status() == QByteArray("HTTP/1.1 200 Connection established"), c.name);
            QVERIFY2(fake.connections() == c.connections, c.name);
            QVERIFY2(fake.seen().size() == 2 && !fake.seen().value(0).authHeader && fake.seen().value(1).authHeader, c.name);
            t.client->send("ping");
            QVERIFY2(t.client->readCount(4) == QByteArray("ping"), c.name); // the tunnel relays (nothing of the 407 left)
            bridge->stop(2000);
        }
        // Review fix: each of a proxy name's addresses gets its share of the 6 s connect budget (one that drops the
        // packets, such as a broken IPv6 path, can't starve the working IPv4 address behind it).
        QCOMPARE(connectSliceMs(6000, 1), quint64(6000));
        QCOMPARE(connectSliceMs(6000, 2), quint64(3000));
        QCOMPARE(connectSliceMs(6000, 4), quint64(1500));
        QCOMPARE(connectSliceMs(6000, 8), quint64(1500)); // at least 1.5 s each
        QCOMPARE(connectSliceMs(1000, 3), quint64(1000)); // never more than what is left
        QCOMPARE(connectSliceMs(0, 2), quint64(0));
        // 200 with early bytes; NTLM only; 403; 502; garbage; an immediate close after 200 with the client's bytes
        {
            HttpScript early;
            early.earlyAfter200 = "SITE";
            FakeHttpProxy fake(early);
            auto          bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Http, fake.port()));
            Tunnel     t     = connectThrough(bridge, "www.youtube.com:443", 6);
            QByteArray after = t.head.mid(t.head.indexOf("\r\n\r\n") + 4);
            after += t.client->readCount(4 - after.size());
            QCOMPARE(after, QByteArray("SITE"));
            bridge->stop(2000);
        }
        struct Case {
            HttpScript  script;
            const char* user;
            Failure     failure;
        };
        const auto make = [](bool ntlm, int status, bool garbage) {
            HttpScript s;
            s.ntlmOnly = ntlm;
            s.status   = status;
            s.garbage  = garbage;
            return s;
        };
        const Case cases[] = {{make(true, 200, false), "tester", Failure::LoginMethod},
                              {make(false, 403, false), "", Failure::NotAllowed},
                              {make(false, 502, false), "", Failure::CantReachSite},
                              {make(false, 200, true), "", Failure::NotHttp}};
        quint64 id = 10;
        for (const Case& c : cases) {
            FakeHttpProxy fake(c.script);
            auto          bridge = Bridge::create(remote);
            bridge->setUpstream(upstream(Type::Http, fake.port(), c.user, *c.user ? "pw" : nullptr));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", ++id);
            QVERIFY2(t.head.contains(QByteArray("X-TSMedia-Proxy: ") + failureName(c.failure)), failureName(c.failure));
            QCOMPARE(recordOf(bridge, 'f', id).failure, c.failure);
            bridge->stop(2000);
        }
        {
            HttpScript s;
            s.after = After::CloseOnClientData;
            FakeHttpProxy fake(s);
            auto          bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Http, fake.port()));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 20);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            t.client->send("client hello");
            QVERIFY(t.client->closedByPeer(3000));
            QVERIFY(waitFor([&]() {
                Record r;
                return bridge->outcome('f', 20, &r) && r.failure == Failure::CantReachSite;
            }));
            bridge->stop(2000);
        }
    }

    // 16. caps and time-outs (S17)
    void bridgeLimits()
    {
        SocksScript hold;
        hold.after = After::Hold;
        FakeSocks fake(hold);
        {
            Bridge::Options o = bridgeOptions();
            o.headTimeoutMs   = 3000;
            auto bridge       = Bridge::create(o);
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            // eight connections that send nothing; the ninth is closed at once without an answer
            std::vector<std::unique_ptr<Client>> quiet;
            for (int i = 0; i < 8; ++i)
                quiet.emplace_back(new Client(bridge->port()));
            QThread::msleep(200);
            Client        ninth(bridge->port());
            QElapsedTimer t;
            t.start();
            bool closed = false;
            QVERIFY(ninth.readToClose(&closed).isEmpty());
            QVERIFY(closed);
            QVERIFY(t.elapsed() < 1500);
            quiet.clear();
            bridge->stop(2000);
        }
        // the 65th authenticated tunnel: 503 busy
        {
            auto bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            std::vector<Tunnel> tunnels;
            for (int i = 0; i < 64; ++i) {
                tunnels.push_back(connectThrough(bridge, "www.youtube.com:443", 1000 + i));
                QCOMPARE(tunnels.back().status(), QByteArray("HTTP/1.1 200 Connection established"));
            }
            Tunnel busy = connectThrough(bridge, "www.youtube.com:443", 2000);
            QVERIFY(busy.status().startsWith("HTTP/1.1 503"));
            QVERIFY(busy.head.contains("X-TSMedia-Proxy: busy"));
            QCOMPARE(recordOf(bridge, 'f', 2000).failure, Failure::Busy);
            QCOMPARE(bridge->counters().tunnelsOpen, 64);
            // setUpstream closes every open tunnel
            bridge->setUpstream(upstream(Type::Socks5, fake.port(), "", nullptr, "127.0.0.1", 2));
            for (Tunnel& t : tunnels)
                QVERIFY(t.client->closedByPeer(2000));
            QVERIFY(waitFor([&]() { return bridge->counters().tunnelsOpen == 0; }));
            bridge->stop(2000);
        }
        // an idle relayed tunnel closes (a short idle time here)
        {
            Bridge::Options o = bridgeOptions();
            o.idleMs          = 300;
            auto bridge       = Bridge::create(o);
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel        t = connectThrough(bridge, "www.youtube.com:443", 1);
            QElapsedTimer clock;
            clock.start();
            QVERIFY(t.client->closedByPeer(3000));
            QVERIFY(clock.elapsed() < 2000);
            bridge->stop(2000);
        }
    }

    // 17. the epoch closes idle tunnels at once (F4); after none, 502 off
    void bridgeEpochClose()
    {
        SocksScript hold;
        hold.after = After::Hold;
        FakeSocks fake(hold);
        auto      bridge = Bridge::create(bridgeOptions());
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));
        Tunnel t = connectThrough(bridge, "www.youtube.com:443", 1);
        QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
        QThread::msleep(100);
        QElapsedTimer clock;
        clock.start();
        bridge->setUpstream(upstream(Type::Socks5, fake.port(), "", nullptr, "127.0.0.1", 2));
        QVERIFY(t.client->closedByPeer(1000));
        QVERIFY2(clock.elapsed() < 100, qPrintable(QString::number(clock.elapsed())));
        bridge->setUpstream(nullptr);
        QVERIFY(!bridge->hasUpstream());
        Tunnel off = connectThrough(bridge, "www.youtube.com:443", 2);
        QVERIFY(off.status().startsWith("HTTP/1.1 502"));
        QVERIFY(off.head.contains("X-TSMedia-Proxy: off"));
        QCOMPARE(recordOf(bridge, 'f', 2).failure, Failure::Off);
        bridge->stop(2000);
    }

    // 18. a bounded stop, the listener first (F5); the saved port; an exclusive port elsewhere
    void bridgeStopBounded()
    {
        SocksScript hold;
        hold.after = After::Hold;
        FakeSocks fake(hold);
        auto      bridge = Bridge::create(bridgeOptions());
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));
        std::vector<Tunnel> tunnels;
        for (int i = 0; i < 10; ++i)
            tunnels.push_back(connectThrough(bridge, "www.youtube.com:443", i + 1));
        QCOMPARE(bridge->liveThreads(), 11);
        const quint16 port = bridge->port();
        QElapsedTimer clock;
        clock.start();
        QVERIFY(bridge->stop(2000));
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(bridge->liveThreads(), 0);
        QCOMPARE(bridge->openHandles(), 0);
        bridge.reset();
        // the saved port again
        Bridge::Options o = bridgeOptions();
        o.preferredPort   = port;
        auto again        = Bridge::create(o);
        QVERIFY(again);
        QCOMPARE(again->port(), port);
        // a stop racing accepts leaves no unjoined tunnel
        std::atomic_bool stopClients{false};
        std::thread      flood([&]() {
            while (!stopClients.load()) {
                Client c(again->port(), 500);
                c.send(connectRequest("www.youtube.com:443", "", ""));
            }
        });
        QThread::msleep(100);
        again->stop(2000);
        stopClients = true;
        flood.join();
        QVERIFY(waitFor([&]() { return again->liveThreads() == 0; }, 3000));
        QCOMPARE(again->openHandles(), 0);
        // the saved port held exclusively elsewhere: another port
        SOCKET      holder = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL        yes    = TRUE;
        sockaddr_in a{};
        a.sin_family           = AF_INET;
        a.sin_port             = htons(port);
        a.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
        QCOMPARE(::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        ::listen(holder, 1);
        auto other = Bridge::create(o);
        QVERIFY(other);
        QVERIFY(other->port() != port);
        other->stop(2000);
        closesocket(holder);
    }

    // 19. a stop while tunnels are in the handshake (F14)
    void bridgeStopMidHandshake()
    {
        SocksScript silent;
        silent.silent = true;
        FakeSocks       fake(silent); // accepts and never answers: tunnels wait in the greeting
        Bridge::Options o = bridgeOptions();
        std::atomic_int lookups{0};
        o.upstream.resolver = [&lookups](const QString&, QVector<lp::net::Address>*, const HANDLE* cancel, int count, int timeoutMs) {
            ++lookups; // a slow resolver for the proxy's name: until the stop or epoch event
            WaitForMultipleObjects(static_cast<DWORD>(count), cancel, FALSE, static_cast<DWORD>(timeoutMs));
            return 2;
        };
        auto greeting = Bridge::create(o);
        greeting->setUpstream(upstream(Type::Socks5, fake.port()));
        auto resolving = Bridge::create(o);
        resolving->setUpstream(upstream(Type::Socks5, fake.port(), "", nullptr, "proxy.example.net"));
        std::vector<std::unique_ptr<Client>> clients;
        for (int i = 0; i < 4; ++i) {
            clients.emplace_back(new Client(greeting->port()));
            clients.back()->send(connectRequest("www.youtube.com:443", "tsm-f" + QByteArray::number(i), greeting->token()));
            clients.emplace_back(new Client(resolving->port()));
            clients.back()->send(connectRequest("www.youtube.com:443", "tsm-f" + QByteArray::number(i), resolving->token()));
        }
        QVERIFY(waitFor([&]() { return fake.connections() >= 4 && lookups.load() >= 4; }));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(greeting->stop(2000));
        QVERIFY(resolving->stop(2000));
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(greeting->liveThreads(), 0);
        QCOMPARE(resolving->liveThreads(), 0);
    }

    // 20. a long session holds only live handles (F5)
    void bridgeHandlePrune()
    {
        FakeSocks fake;
        auto      bridge = Bridge::create(bridgeOptions());
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));
        int worst = 0;
        for (int i = 0; i < 1000; ++i) {
            {
                Tunnel t = connectThrough(bridge, "www.youtube.com:443", static_cast<quint64>(i + 1));
                QVERIFY(t.status().startsWith("HTTP/1.1 200"));
            }
            worst = qMax(worst, bridge->openHandles() - bridge->liveThreads());
        }
        QVERIFY2(worst <= 1, qPrintable(QString::number(worst)));
        bridge->stop(2000);
    }

    // 21. volume, backpressure and half-close (F14)
    void bridgeRelayVolume()
    {
        FakeSocks fake; // echo
        auto      bridge = Bridge::create(bridgeOptions());
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));
        Tunnel t = connectThrough(bridge, "www.youtube.com:443", 1);
        QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
        constexpr int total = 64 * 1024 * 1024;
        QByteArray    chunk(256 * 1024, '\0');
        for (int i = 0; i < chunk.size(); ++i)
            chunk[i] = static_cast<char>((i * 131 + 7) & 0xff);
        quint64     sentSum = 0;
        std::thread writer([&]() {
            for (int sent = 0; sent < total; sent += chunk.size()) {
                if (!t.client->send(chunk))
                    return;
            }
            t.client->send("TRAILING");
            t.client->shutdownSend(); // the half-close passes through after the last byte
        });
        for (const char c : chunk)
            sentSum += static_cast<quint8>(c);
        sentSum *= static_cast<quint64>(total / chunk.size());
        QThread::msleep(300); // a slow reader first: the bridge must hold back, not buffer
        quint64    gotSum = 0;
        qint64     got    = 0;
        QByteArray tail;
        bool       closed = false;
        while (!closed) {
            const QByteArray part = t.client->readCount(64 * 1024);
            if (part.isEmpty())
                break;
            for (const char c : part)
                gotSum += static_cast<quint8>(c);
            got += part.size();
            tail = (tail + part).right(8);
            if (got >= total + 8)
                closed = t.client->closedByPeer(3000);
        }
        writer.join();
        QCOMPARE(got, qint64(total) + 8);
        QCOMPARE(tail, QByteArray("TRAILING"));
        QCOMPARE(gotSum - (static_cast<quint64>('T') + 'R' + 'A' + 'I' + 'L' + 'I' + 'N' + 'G'), sentSum);
        QVERIFY(closed);
        bridge->stop(2000);
    }

    // 22. 407s never reach route health (F14)
    void bridgeAuthRefusalNotRouteLevel()
    {
        FakeSocks fake;
        auto      bridge = Bridge::create(bridgeOptions());
        bridge->setUpstream(upstream(Type::Socks5, fake.port()));
        for (int i = 0; i < 5; ++i) {
            Client c(bridge->port());
            c.send(connectRequest("www.youtube.com:443", "", ""));
            QVERIFY(statusLine(c.readHead()).startsWith("HTTP/1.1 407"));
        }
        QVERIFY(waitFor([&]() { return bridge->counters().unauthenticated >= 5; }));
        const QVector<Record> unauth = bridge->recent(0, 0);
        QVERIFY(unauth.size() >= 5);
        for (const Record& r : unauth)
            QVERIFY(!isRouteLevel(r.failure));
        for (const auto& count : bridge->counters().failures)
            QCOMPARE(count, quint64(0));
        QCOMPARE(fake.connections(), 0);
        bridge->stop(2000);
    }

    // Review fix: a client that keeps sending can't hold a tunnel thread. After an error answer the drain ends at its time
    // and byte cap; an established tunnel that never runs dry still closes at once at a new generation and at the bridge's
    // stop. The updater's tunnel ('u') is the one a new generation leaves alone (its download finishes, design 3.5).
    void bridgeStreamingClientsBounded()
    {
        const QByteArray chunk(64 * 1024, 'x');
        // a 405, then a flood: the thread ends at its cap, not when the client stops sending
        {
            FakeSocks fake;
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Client c(bridge->port());
            c.send("POST / HTTP/1.1\r\nHost: x\r\n\r\n");
            QVERIFY(statusLine(c.readHead()).startsWith("HTTP/1.1 405"));
            {
                Flood flood([&]() { return c.send(chunk); });
                QVERIFY(waitFor([&]() { return bridge->liveThreads() == 1; }, 2000)); // the listener alone
            }
            bridge->stop(2000);
        }
        // an established tunnel that streams without a pause: a new generation, then the stop, reach it at once
        for (const bool byStop : {false, true}) {
            SocksScript sink;
            sink.after = After::Sink;
            FakeSocks fake(sink);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 1);
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
            {
                Flood flood([&]() { return t.client->send(chunk); });
                QVERIFY(waitFor([&]() { return fake.sunk() > 4 * 1024 * 1024; }));
                QElapsedTimer clock;
                clock.start();
                if (byStop) {
                    QVERIFY(bridge->stop(2000));
                } else {
                    bridge->setUpstream(upstream(Type::Socks5, fake.port(), "", nullptr, "127.0.0.1", 2));
                    QVERIFY(waitFor([&]() { return bridge->counters().tunnelsOpen == 0; }, 2000));
                }
                QVERIFY2(clock.elapsed() < 1000, qPrintable(QString::number(clock.elapsed())));
            }
            bridge->stop(2000);
        }
        // the updater's tunnel lives through a route change (the chat proxy turned off); the Fetcher's closes at once
        {
            FakeSocks fake; // echo
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Client updater(bridge->port());
            updater.send(connectRequest("api.github.com:443", "tsm-u7", bridge->token()));
            QCOMPARE(statusLine(updater.readHead()), QByteArray("HTTP/1.1 200 Connection established"));
            Tunnel fetcher = connectThrough(bridge, "www.youtube.com:443", 8);
            QCOMPARE(fetcher.status(), QByteArray("HTTP/1.1 200 Connection established"));
            bridge->setUpstream(nullptr);
            QVERIFY(fetcher.client->closedByPeer(1000));
            updater.send("still here");
            QCOMPARE(updater.readCount(10), QByteArray("still here"));
            QVERIFY(bridge->stop(2000)); // the stop still ends it
            QVERIFY(updater.closedByPeer(1000));
        }
        // a failed attach: the socket is attach()'s from the call on (no second closesocket of a reused handle value)
        QVERIFY(!sourceText(QStringLiteral("src/proxy/bridge.cpp")).contains(QStringLiteral("closesocket(arg.socket)")));
    }

    // 23. the real WinHTTP transport through the bridge (asynchronous mode)
    void winhttpThroughBridge()
    {
        const QString name = QStringLiteral("nx-%1.example.com").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
        const auto    hop  = [&](const std::shared_ptr<Bridge>& bridge, quint64 id, lp::http::Head* head) {
            auto                  transport = lp::http::winHttpTransport();
            lp::http::HopOptions  o;
            o.url               = QUrl(QStringLiteral("https://") + name + QStringLiteral("/a.png"));
            o.route.via         = lp::http::Route::Via::Bridge;
            o.route.proxy       = QStringLiteral("127.0.0.1:%1").arg(bridge->port());
            o.route.bridgeToken = bridge->token();
            o.route.bridgeUser  = "tsm-f" + QByteArray::number(id);
            o.deadline          = lp::http::nowMs() + 10000;
            o.connectTimeoutMs  = 5000;
            o.receiveTimeoutMs  = 5000;
            o.allowPeer         = [](const lp::net::Address& a) { return a == lp::net::Address::fromIPv4(0x7F000001u); };
            lp::http::Cancel cancel;
            lp::http::Error  error    = lp::http::Error::None;
            auto             exchange = transport->open(o, cancel, &error);
            if (!exchange)
                return error;
            return exchange->head(head);
        };
        // the name goes by ATYP 3, the first CONNECT carried the login (pre-authentication: no 407 needed)
        {
            FakeSocks fake;
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            lp::http::Head head;
            hop(bridge, 11, &head); // the echo "site" breaks the TLS handshake: whatever error
            QVERIFY(waitFor([&]() { return !fake.seen().isEmpty(); }));
            QCOMPARE(fake.seen().value(0).atyp, 3);
            QCOMPARE(QString::fromLatin1(fake.seen().value(0).target), name);
            Record r;
            QVERIFY(bridge->outcome('f', 11, &r));
            QCOMPARE(r.client, 'f');
            QCOMPARE(bridge->counters().unauthenticated, quint64(0));
            bridge->stop(2000);
        }
        // the bridge's own 502 comes with IS_PROXY_CONNECT_RESPONSE
        {
            SocksScript s;
            s.rep = 1;
            FakeSocks fake(s);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            lp::http::Head head;
            QCOMPARE(hop(bridge, 12, &head), lp::http::Error::None);
            QCOMPARE(head.status, 502);
            QVERIFY(head.proxyConnectResponse);
            QCOMPARE(head.proxyHeader, QStringLiteral("cantReachSite"));
            bridge->stop(2000);
        }
        // an optimistic proxy (success, then EOF after the ClientHello): CantReachSite in the record
        {
            SocksScript s;
            s.after = After::CloseOnClientData;
            FakeSocks fake(s);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            lp::http::Head head;
            QVERIFY(hop(bridge, 13, &head) != lp::http::Error::None);
            QCOMPARE(recordOf(bridge, 'f', 13).failure, Failure::CantReachSite);
            bridge->stop(2000);
        }
        // 2.6 I2a finding (the player through Throne): a client that sends its ClientHello and closes first (Chromium
        // drops a spare connection) is no failure of the proxy, although the site never sent a byte.
        {
            SocksScript s;
            s.after = After::Sink;
            FakeSocks fake(s);
            auto      bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, fake.port()));
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", 15);
            QVERIFY(t.status().contains(" 200 "));
            QVERIFY(t.client->send(QByteArray(517, '\x16')));
            t.client->shutdownSend();
            bool closed = false;
            t.client->readToClose(&closed);
            QVERIFY(closed);
            Record r;
            QVERIFY(waitFor([&]() { return bridge->outcome('f', 15, &r) && r.step == Step::Done; }, 3000));
            QCOMPARE(r.failure, Failure::None);
            QVERIFY(r.up >= 517);
            bridge->stop(2000);
        }
        // the proxy's port closed: Unreachable
        {
            auto bridge = Bridge::create(bridgeOptions());
            bridge->setUpstream(upstream(Type::Socks5, closedPort()));
            lp::http::Head head;
            QCOMPARE(hop(bridge, 14, &head), lp::http::Error::None);
            QCOMPARE(head.status, 502);
            QVERIFY(head.proxyConnectResponse);
            QCOMPARE(recordOf(bridge, 'f', 14).failure, Failure::Unreachable);
            bridge->stop(2000);
        }
    }

    // 24. the transport's guards (S1, F1)
    void transportRouteGuards()
    {
        auto       transport = lp::http::winHttpTransport();
        const auto tryOpen   = [&](const lp::http::Route& route) {
            lp::http::HopOptions o;
            o.url       = QUrl(QStringLiteral("https://www.example.com/a.png"));
            o.route     = route;
            o.deadline  = lp::http::nowMs() + 5000;
            o.allowPeer = [](const lp::net::Address&) { return true; };
            lp::http::Cancel cancel;
            lp::http::Error  error = lp::http::Error::None;
            auto             e     = transport->open(o, cancel, &error);
            return e ? lp::http::Error::None : error;
        };
        const auto route = [](lp::http::Route::Via via, const char* proxy) {
            lp::http::Route r;
            r.via   = via;
            r.proxy = QString::fromLatin1(proxy);
            return r;
        };
        const quint64 before = lp::http::sessionsOpened();
        QCOMPARE(tryOpen(route(lp::http::Route::Via::System, "")), lp::http::Error::Blocked); // emptyProxyRouteBlocked
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Bridge, "127.0.0.1:0")), lp::http::Error::Blocked);
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Bridge, "10.0.0.1:8080")), lp::http::Error::Blocked);
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Bridge, "127.0.0.1:080")), lp::http::Error::Blocked);
        QCOMPARE(tryOpen(lp::http::Route()), lp::http::Error::Blocked); // Unset
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Refused, "")), lp::http::Error::Proxy);
        // custom (and not ready): nothing but the bridge
        publishMode(Mode::Custom, 2, 40000, "t");
        QCOMPARE(tryOpen(route(lp::http::Route::Via::System, "127.0.0.1:3128")), lp::http::Error::Blocked);
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Direct, "")), lp::http::Error::Blocked);
        init();
        QCOMPARE(tryOpen(route(lp::http::Route::Via::Direct, "")), lp::http::Error::Blocked);
        // a Windows-proxy route only in Windows mode
        publishMode(Mode::Direct, 3);
        QCOMPARE(tryOpen(route(lp::http::Route::Via::System, "127.0.0.1:3128")), lp::http::Error::Blocked);
        QCOMPARE(lp::http::sessionsOpened(), before);
        QVERIFY(lp::http::isBridgeProxyString(QStringLiteral("127.0.0.1:40000")));
        QVERIFY(!lp::http::isBridgeProxyString(QStringLiteral("127.0.0.1:1023")));
        QVERIFY(!lp::http::isBridgeProxyString(QStringLiteral("localhost:40000")));
        publishWindowsRoute();
    }

    void bridgePeerPinned()
    {
        closedPort(); // Winsock started
        // a listener on 127.0.0.2 that a "bridge" string names (a test hook lets the string through): the peer pin closes it
        SOCKET      listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.2", &a.sin_addr);
        QCOMPARE(::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
        ::listen(listener, 4);
        int length = sizeof(a);
        getsockname(listener, reinterpret_cast<sockaddr*>(&a), &length);
        const quint16 port = ntohs(a.sin_port);
        lp::http::setAnyLoopbackBridgeForTests(true);
        auto                 transport = lp::http::winHttpTransport();
        lp::http::HopOptions o;
        o.url               = QUrl(QStringLiteral("https://www.example.com/a.png"));
        o.route.via         = lp::http::Route::Via::Bridge;
        o.route.proxy       = QStringLiteral("127.0.0.2:%1").arg(port);
        o.route.bridgeToken = "token";
        o.route.bridgeUser  = "tsm-f1";
        o.deadline          = lp::http::nowMs() + 5000;
        o.allowPeer         = [](const lp::net::Address&) { return true; };
        lp::http::Cancel cancel;
        lp::http::Error  error    = lp::http::Error::None;
        auto             exchange = transport->open(o, cancel, &error);
        QVERIFY(exchange);
        lp::http::Head head;
        QCOMPARE(exchange->head(&head), lp::http::Error::Blocked);
        // nothing was sent to it
        u_long nonBlocking = 1;
        ioctlsocket(listener, FIONBIO, &nonBlocking);
        const SOCKET accepted = ::accept(listener, nullptr, nullptr);
        if (accepted != INVALID_SOCKET) {
            char buffer[64];
            QVERIFY(::recv(accepted, buffer, sizeof(buffer), 0) <= 0);
            closesocket(accepted);
        }
        closesocket(listener);
        lp::http::setAnyLoopbackBridgeForTests(false);
    }

    // 25. a Windows proxy's answers, only with IS_PROXY_CONNECT_RESPONSE
    void winhttpSystemProxyClassify()
    {
        class SystemRoute : public lp::http::Transport
        {
          public:
            explicit SystemRoute(quint16 port)
                : m_port(port)
            {
            }
            lp::http::Error resolve(const QString& h, QVector<lp::net::Address>* out, const lp::http::Cancel& c, quint64 d) override
            {
                return m_inner->resolve(h, out, c, d);
            }
            std::unique_ptr<lp::http::Exchange> open(const lp::http::HopOptions& o, const lp::http::Cancel& c, lp::http::Error* e) override
            {
                return m_inner->open(o, c, e);
            }
            lp::http::Route route(const QUrl&, const lp::http::Cancel&, quint64) override
            {
                lp::http::Route r;
                r.via   = lp::http::Route::Via::System;
                r.proxy = QStringLiteral("127.0.0.1:%1").arg(m_port);
                return r;
            }

          private:
            std::shared_ptr<lp::http::Transport> m_inner = lp::http::winHttpTransport();
            quint16                              m_port;
        };
        const auto load = [](quint16 port) {
            lp::Fetcher::Options o;
            o.gate          = permissiveGate();
            o.localSuffixes = [] { return QStringList(); };
            QTemporaryDir dir;
            lp::Fetcher   fetcher(std::make_shared<SystemRoute>(port), o);
            QSignalSpy    spy(&fetcher, &lp::Fetcher::finished);
            fetcher.load(lp::classify(QStringLiteral("https://cdn.example.com/a.png")), clickCaps(), dir.path());
            spy.wait(20000);
            return spy.isEmpty() ? lp::Result() : spy.first().at(1).value<lp::Result>();
        };
        {
            HttpScript s;
            s.challenge = true; // 407 to everything without a login
            FakeHttpProxy fake(s);
            const lp::Result r = load(fake.port());
            QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
            QCOMPARE(r.proxyFailure, Failure::NeedsLogin);
            QCOMPARE(r.via, Via::System);
        }
        {
            HttpScript s;
            s.status = 502;
            FakeHttpProxy fake(s);
            const lp::Result r = load(fake.port());
            QCOMPARE(r.proxyFailure, Failure::CantReachSite);
        }
        {
            const lp::Result r = load(closedPort());
            QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
            QCOMPARE(r.proxyFailure, Failure::Unreachable);
        }
    }

    // 26. setup scripts on Windows 8+: entries with their schemes (S2)
    void pacSocksRefused()
    {
        PacServer pac;
        pac.answers.insert("/socks5.pac", "SOCKS5 127.0.0.1:2080");
        pac.answers.insert("/socks.pac", "SOCKS 127.0.0.1:2080");
        pac.answers.insert("/proxy-direct.pac", "PROXY 127.0.0.1:1; DIRECT");
        pac.answers.insert("/direct.pac", "DIRECT");
        const QString base  = QStringLiteral("http://127.0.0.1:%1/").arg(pac.port());
        const QString nonce = QString::number(QRandomGenerator::global()->generate());
        const auto    route = [&](const char* file) {
            windowsproxy::setIeConfigProviderForTests([&, file]() {
                windowsproxy::IeConfig c;
                c.ok            = true;
                c.autoConfigUrl = base + QString::fromLatin1(file) + QStringLiteral("?") + nonce; // a new URL per run
                return c;
            });
            lp::http::Cancel cancel;
            return lp::http::winHttpTransport()->route(QUrl(QStringLiteral("https://www.example.com/a.png")), cancel, lp::http::nowMs() + 9000);
        };
        const lp::http::Route socks5 = route("socks5.pac");
        const lp::http::Route socks  = route("socks.pac");
        const lp::http::Route mixed  = route("proxy-direct.pac");
        const lp::http::Route direct = route("direct.pac");
        if (pac.connections() == 0)
            QSKIP("WinHTTP's setup-script service didn't fetch the loopback script on this PC");
        const auto show = [](const lp::http::Route& r) {
            return qPrintable(QStringLiteral("via %1 refused %2 proxy %3").arg(int(r.via)).arg(QLatin1String(failureName(r.refused))).arg(r.proxy));
        };
        QVERIFY2(socks5.via == lp::http::Route::Via::Refused && socks5.refused == Failure::UnsupportedWindowsProxy, show(socks5));
        QVERIFY2(socks.via == lp::http::Route::Via::Refused && socks.refused == Failure::SocksOnlyWindows, show(socks));
        QVERIFY2(mixed.via == lp::http::Route::Via::System && mixed.proxy == QStringLiteral("127.0.0.1:1"), show(mixed));
        QVERIFY2(direct.via == lp::http::Route::Via::Direct, show(direct));
        lp::http::Route r;
        // a manual SOCKS-only setting through the real route()
        windowsproxy::setIeConfigProviderForTests([]() {
            windowsproxy::IeConfig c;
            c.ok    = true;
            c.proxy = QStringLiteral("socks=127.0.0.1:2080");
            return c;
        });
        lp::http::Cancel cancel;
        r = lp::http::winHttpTransport()->route(QUrl(QStringLiteral("https://www.example.com/a.png")), cancel, lp::http::nowMs() + 9000);
        QCOMPARE(r.via, lp::http::Route::Via::Refused);
        QCOMPARE(r.refused, Failure::SocksOnlyWindows);
    }

    // 27. the Fetcher's routes and proxy classification
    void failClosedDefaultRoute()
    {
        // custom refused: no route(), no lookup, no request
        {
            FetchHarness h;
            h.http->hops.insert(QString::fromLatin1(kPng), png());
            publishMode(Mode::Custom, 5, 0, QByteArray(), Failure::BridgeUnavailable);
            const lp::Result r = h.load(QString::fromLatin1(kPng));
            QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
            QCOMPARE(r.proxyFailure, Failure::BridgeUnavailable);
            QCOMPARE(h.http->resolves.load(), 0);
            QCOMPARE(h.http->opens.load(), 0);
            QCOMPARE(h.http->routes.load(), 0);
        }
        // custom: the bridge, never route(), never a lookup
        {
            FetchHarness h;
            h.http->hops.insert(QString::fromLatin1(kPng), png());
            h.http->routeAnswer = lp::http::Route(); // a fake that never decides
            publishMode(Mode::Custom, 6, 40001, "tok");
            const lp::Result r = h.load(QString::fromLatin1(kPng));
            QVERIFY2(r.ok, qPrintable(r.error));
            QCOMPARE(h.http->routes.load(), 0);
            QCOMPARE(h.http->resolves.load(), 0);
            QCOMPARE(h.http->opened.value(0).route.via, lp::http::Route::Via::Bridge);
            QCOMPARE(h.http->opened.value(0).route.proxy, QStringLiteral("127.0.0.1:40001"));
            QVERIFY(h.http->opened.value(0).route.bridgeUser.startsWith("tsm-f"));
            QCOMPARE(r.via, Via::Bridge);
            QVERIFY(r.routeOk);
            QVERIFY(h.logs.join(QLatin1Char('\n')).contains(QStringLiteral("(via the chat proxy)")));
            QVERIFY(!h.logs.join(QLatin1Char('\n')).contains(QStringLiteral("40001")));
        }
        // Windows mode with route() undecided (WinHTTP missing): refused, nothing sent
        {
            FetchHarness h;
            h.http->hops.insert(QString::fromLatin1(kPng), png());
            h.http->routeAnswer = lp::http::Route();
            publishWindowsRoute();
            const lp::Result r = h.load(QString::fromLatin1(kPng));
            QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
            QCOMPARE(r.proxyFailure, Failure::Internal);
            QCOMPARE(h.http->opens.load(), 0);
            QCOMPARE(h.http->resolves.load(), 0);
        }
    }

    void snapshotBeforeStart()
    {
        FetchHarness h;
        h.http->hops.insert(QString::fromLatin1(kPng), png());
        init();
        const lp::Result r = h.load(QString::fromLatin1(kPng));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
        QCOMPARE(r.proxyFailure, Failure::NotReady);
        QCOMPARE(h.http->resolves.load(), 0);
        QCOMPARE(h.http->opens.load(), 0);
    }

    void fetchThroughBridgeNoDns()
    {
        FetchHarness h;
        h.http->hops.insert(QString::fromLatin1(kPng), png());
        h.http->hops.insert(QStringLiteral("https://nas.corp.example.net/a.png"), png());
        publishMode(Mode::Custom, 7, 40002, "tok");
        QVERIFY(h.load(QString::fromLatin1(kPng)).ok);
        QCOMPARE(h.http->resolves.load(), 0);
        // the local-suffix rule still refuses
        const lp::Result r = h.load(QStringLiteral("https://nas.corp.example.net/a.png"));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kBlocked));
        QCOMPARE(h.http->opens.load(), 1);
    }

    void poisonedDnsAndFakeIp()
    {
        const auto addresses = [](std::initializer_list<const char*> list) {
            QVector<lp::net::Address> v;
            for (const char* a : list)
                v << lp::net::parseAddress(QString::fromLatin1(a));
            return v;
        };
        // poisonedDnsWindowsProxy: the Windows proxy resolves, so a poisoned local answer doesn't matter
        {
            FetchHarness h;
            h.http->hops.insert(QString::fromLatin1(kPng), png());
            h.http->dns.insert(QStringLiteral("cdn.example.com"), addresses({"10.10.34.35"}));
            h.http->routeAnswer.via   = lp::http::Route::Via::System;
            h.http->routeAnswer.proxy = QStringLiteral("127.0.0.1:10809");
            QVERIFY(h.load(QString::fromLatin1(kPng)).ok);
            QCOMPARE(h.http->resolves.load(), 0);
        }
        publishMode(Mode::Direct, 9);
        const auto direct = [&](std::initializer_list<const char*> answer, const char* url = kPng, const char* peer = nullptr) {
            FetchHarness h;
            ScriptedHop  hop = png();
            if (peer)
                hop.peer = QString::fromLatin1(peer);
            h.http->hops.insert(QString::fromLatin1(url), hop);
            h.http->dns.insert(QUrl(QString::fromLatin1(url)).host(), addresses(answer));
            const lp::Result r = h.load(QString::fromLatin1(url));
            return qMakePair(r, h.http->opens.load());
        };
        auto r = direct({"10.10.34.35"});
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kFiltered)); // poisonedDnsDirect
        QCOMPARE(r.second, 0);
        r = direct({"192.168.1.10"});
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kBlocked)); // lanAnswerBlockedNotFiltered (S6)
        r = direct({"127.0.0.1"});
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kBlocked));
        r = direct({"198.18.0.5", "198.18.0.6"}, kPng, "198.18.0.5");
        QVERIFY2(r.first.ok, qPrintable(r.first.error)); // fakeIpDirect: the peer is accepted too
        r = direct({"198.18.0.5", "10.0.0.2"});
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kBlocked));
        r = direct({"fc00::5", "198.18.0.5"}, kPng, "198.18.0.5");
        QVERIFY(r.first.ok);
        r = direct({"198.18.0.5", "2606:2800:220:1:248:1893:25c8:1946"}, kPng, "198.18.0.5");
        QVERIFY(r.first.ok); // fakeIpPlusPublicV6
        r = direct({"198.18.0.5"}, "https://nas.corp.example.net/a.png", "198.18.0.5");
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kBlocked)); // fakeIpLocalSuffix (S5)
        // a fake-IP answer but the response came from a private address: refused after the response (rebinding)
        r = direct({"198.18.0.5"}, kPng, "10.0.0.9");
        QCOMPARE(r.first.error, QString::fromLatin1(lp::errors::kBlocked));
    }

    void socksOnlyWindows()
    {
        FetchHarness h;
        h.http->hops.insert(QString::fromLatin1(kPng), png());
        h.http->routeAnswer         = lp::http::Route();
        h.http->routeAnswer.via     = lp::http::Route::Via::Refused;
        h.http->routeAnswer.refused = Failure::SocksOnlyWindows;
        const lp::Result r          = h.load(QString::fromLatin1(kPng));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
        QCOMPARE(r.proxyFailure, Failure::SocksOnlyWindows);
        QCOMPARE(r.via, Via::Refused);
        QCOMPARE(h.http->resolves.load(), 0);
        QCOMPARE(h.http->opens.load(), 0);
        h.http->routeAnswer.refused = Failure::UnsupportedWindowsProxy; // a setup script's 0 entries
        QCOMPARE(h.load(QStringLiteral("https://cdn.example.com/b.png")).proxyFailure, Failure::UnsupportedWindowsProxy);
    }

    void proxyFailureNeutral()
    {
        lp::HostGate::Limits limits; // the breaker as shipped, no spacing (12 loads in a row)
        limits.mediaSpacingMs = 0;
        limits.mediaBurst     = 100;
        auto gate             = std::make_shared<lp::HostGate>(std::function<qint64()>(), limits);
        lp::Fetcher::Options o;
        o.gate = gate;
        FetchHarness h(o);
        publishMode(Mode::Custom, 10, 40003, "tok");
        int n = 0;
        for (Failure f : {Failure::Unreachable, Failure::CantReachSite}) {
            for (int i = 0; i < 6; ++i) {
                const QString url = QStringLiteral("https://cdn.example.com/%1.png").arg(++n);
                ScriptedHop   hop;
                hop.status               = 502;
                hop.proxyConnectResponse = true;
                auto records             = h.records;
                hop.onOpen               = [records, f](const lp::http::HopOptions& options) {
                    Record r;
                    r.client  = 'f';
                    r.id      = FetchHarness::hopId(options);
                    r.failure = f;
                    r.step    = f == Failure::Unreachable ? Step::Connect : Step::Request;
                    records->add(r);
                };
                h.http->hops.insert(url, hop);
                lp::Caps automatic;
                automatic.media    = 25 * 1024 * 1024;
                const lp::Result r = h.load(url, automatic);
                QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
                QCOMPARE(r.proxyFailure, f);
            }
        }
        qint64 ms = 0;
        QVERIFY(gate->state(QStringLiteral("cdn.example.com"), lp::HostGate::Lane::Media, QString(), &ms) != lp::HostGate::Verdict::Broken);
    }

    void proxiedPageSlowHandshake()
    {
        lp::Fetcher::Options o;
        o.pageMinBytesAfterMs = 500;
        const QString page    = QStringLiteral("https://news.example.org/story");
        ScriptedHop   hop;
        hop.contentType = "text/html";
        hop.body        = linkfixtures::webCard(QStringLiteral("A story"), QStringLiteral("What happened"), QString());
        hop.headDelayMs = 900; // the CONNECT and TLS through a slow VPN
        {
            FetchHarness h(o);
            h.http->hops.insert(page, hop);
            publishMode(Mode::Custom, 11, 40004, "tok");
            const lp::Result r = h.load(page);
            QVERIFY2(r.ok, qPrintable(r.error));
        }
        {
            publishMode(Mode::Direct, 12);
            FetchHarness h(o);
            h.http->hops.insert(page, hop);
            const lp::Result r = h.load(page);
            QCOMPARE(r.error, QString::fromLatin1(lp::errors::kTimeout)); // 2.5's drip rule, direct
        }
    }

    void hopTimeoutIsProxySlow()
    {
        auto                 gate = std::make_shared<lp::HostGate>();
        lp::Fetcher::Options o;
        o.gate = gate;
        FetchHarness h(o);
        publishMode(Mode::Custom, 13, 40005, "tok");
        ScriptedHop hop;
        hop.headError = lp::http::Error::Timeout;
        auto records  = h.records;
        hop.onOpen    = [records](const lp::http::HopOptions& options) {
            Record r;
            r.client = 'f';
            r.id     = FetchHarness::hopId(options);
            r.step   = Step::Greeting; // still in the proxy's handshake
            records->add(r);
        };
        h.http->hops.insert(QString::fromLatin1(kPng), hop);
        const lp::Result r = h.load(QString::fromLatin1(kPng));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
        QCOMPARE(r.proxyFailure, Failure::Slow);
        QVERIFY(!r.proxyDecisive);
        qint64 ms = 0;
        QVERIFY(gate->state(QStringLiteral("cdn.example.com"), lp::HostGate::Lane::Media, QString(), &ms) != lp::HostGate::Verdict::Broken);
    }

    void siteForgesProxyHeader()
    {
        FetchHarness h;
        publishMode(Mode::Custom, 14, 40006, "tok");
        auto       records     = h.records;
        const auto established = [records](const lp::http::HopOptions& options) {
            Record r;
            r.client = 'f';
            r.id     = FetchHarness::hopId(options);
            r.step   = Step::Relay;
            r.down   = 4000;
            records->add(r);
        };
        ScriptedHop forged;
        forged.status      = 502;
        forged.proxyHeader = QStringLiteral("loginRefused"); // the site's own header, inside the tunnel
        forged.onOpen      = established;
        h.http->hops.insert(QString::fromLatin1(kPng), forged);
        lp::Result r = h.load(QString::fromLatin1(kPng));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kServerError));
        QCOMPARE(r.proxyFailure, Failure::None);
        // site407IsSiteLevel
        ScriptedHop site407;
        site407.status = 407;
        site407.onOpen = established;
        h.http->hops.insert(QStringLiteral("https://cdn.example.com/b.png"), site407);
        r = h.load(QStringLiteral("https://cdn.example.com/b.png"));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kBadResponse));
        QCOMPARE(r.proxyFailure, Failure::None);
        QVERIFY(r.routeOk);
        // recordSourceInjected: the bridge's answer without a record is no proxy failure of a kind it didn't say
        ScriptedHop bridge502;
        bridge502.status               = 502;
        bridge502.proxyConnectResponse = true;
        h.http->hops.insert(QStringLiteral("https://cdn.example.com/c.png"), bridge502);
        r = h.load(QStringLiteral("https://cdn.example.com/c.png"));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
        QCOMPARE(r.proxyFailure, Failure::Internal);
    }

    void interceptedRouteLevel()
    {
        FetchHarness h;
        publishMode(Mode::Custom, 15, 40007, "tok");
        auto       records  = h.records;
        const auto relayed  = [records](const lp::http::HopOptions& options) {
            Record r;
            r.client = 'f';
            r.id     = FetchHarness::hopId(options);
            r.step   = Step::Relay;
            r.down   = 1200; // the site's (or the interceptor's) TLS bytes came
            records->add(r);
        };
        ScriptedHop untrusted;
        untrusted.headError   = lp::http::Error::Tls;
        untrusted.secureFlags = kFlagInvalidCa;
        untrusted.onOpen      = relayed;
        h.http->hops.insert(QString::fromLatin1(kPng), untrusted);
        lp::Result r = h.load(QString::fromLatin1(kPng));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kProxy));
        QCOMPARE(r.proxyFailure, Failure::Intercepted);
        QVERIFY(!r.proxyDecisive);
        ScriptedHop expired = untrusted;
        expired.secureFlags = kFlagCertDateInvalid;
        h.http->hops.insert(QStringLiteral("https://cdn.example.com/b.png"), expired);
        r = h.load(QStringLiteral("https://cdn.example.com/b.png"));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kTls));
    }

    void routeGateAndFields()
    {
        bool                 open = false;
        lp::Fetcher::Options o;
        o.routeGate = [&open](bool userAsked) { return open || userAsked; };
        FetchHarness h(o);
        h.http->hops.insert(QString::fromLatin1(kPng), png());
        lp::Caps automatic;
        automatic.media = 25 * 1024 * 1024;
        lp::Result r    = h.load(QString::fromLatin1(kPng), automatic);
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kRouteDown));
        QCOMPARE(h.http->opens.load(), 0);
        r = h.load(QString::fromLatin1(kPng)); // a click passes
        QVERIFY(r.ok);
        QVERIFY(r.siteResponded);
        QCOMPARE(r.via, Via::Direct);
        QCOMPARE(r.routeGeneration, quint64(1));
        // a cached answer is still served while the gate says no
        r = h.load(QString::fromLatin1(kPng), automatic);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->opens.load(), 1);
        // resultRouteFields: a 404 through the bridge is still a route that works
        publishMode(Mode::Custom, 21, 40008, "tok");
        open = true;
        r    = h.load(QStringLiteral("https://cdn.example.com/missing.png"));
        QCOMPARE(r.error, QString::fromLatin1(lp::errors::kNotFound));
        QVERIFY(r.routeOk);
        QVERIFY(r.siteResponded);
        QCOMPARE(r.via, Via::Bridge);
        QCOMPARE(r.routeGeneration, quint64(21));
    }

    // Review fix (5.2): in Probation, the probe its site makes wait (HostGate's Wait) keeps the one slot: its second
    // admission doesn't end it as routedown; another automatic load meanwhile is refused.
    void probeWaitsForItsSite()
    {
        qint64 healthNow = 1'000'000;
        Health health([&healthNow] { return healthNow; });
        health.reset(7, true);
        QVERIFY(health.report(7, Failure::Unreachable, Step::Connect, QStringLiteral("example.com")));
        healthNow += Health::kLocalDelayMs;
        QVERIFY(health.tick());
        QCOMPARE(health.state(), Health::State::Probation);
        lp::HostGate::Limits limits;
        limits.mediaRunning = 1; // a second load of the site waits until the first ends
        lp::Fetcher::Options o;
        o.gate      = std::make_shared<lp::HostGate>(std::function<qint64()>(), limits);
        o.routeGate = [&health](bool userAsked) { return health.admits(userAsked); };
        FetchHarness h(o);
        ScriptedHop slow = png();
        slow.headDelayMs = 400;
        const QString a = QStringLiteral("https://cdn.example.com/a.png"), b = QStringLiteral("https://cdn.example.com/b.png"),
                      c = QStringLiteral("https://cdn.example.com/c.png");
        h.http->hops.insert(a, slow);
        h.http->hops.insert(b, png());
        h.http->hops.insert(c, png());
        QHash<QString, lp::Result> results;
        QObject::connect(h.fetcher.get(), &lp::Fetcher::finished, h.fetcher.get(),
                         [&results](const QString& key, const lp::Result& r) { results.insert(key, r); });
        lp::Caps automatic  = clickCaps();
        automatic.userAsked = false;
        const lp::Target ta = lp::classify(a), tb = lp::classify(b), tc = lp::classify(c);
        h.fetcher->load(ta, clickCaps(), h.dir.path()); // a click: runs and holds the site
        h.fetcher->load(tb, automatic, h.dir.path());   // the probe: the gate's one slot, then its site makes it wait
        h.fetcher->load(tc, automatic, h.dir.path());   // the slot is taken: routedown at once
        QVERIFY(waitFor([&]() { return results.size() == 3; }, 20000));
        QVERIFY(results.value(ta.key).ok);
        QVERIFY2(results.value(tb.key).ok, qPrintable(results.value(tb.key).error));
        QCOMPARE(results.value(tc.key).error, QString::fromLatin1(lp::errors::kRouteDown));
        QCOMPARE(h.http->opens.load(), 2);
    }

    // Review fix (5.2): a probe slot freed after 30 s without a report is a change (healthChanged), so the links paused
    // meanwhile go back to the on-screen pass and one of them becomes the next probe.
    void probeSlotFreedWakesPaused()
    {
        QTemporaryDir    dir;
        FakeSocks        fake;
        qint64           now = 1'000'000;
        Control::Options options;
        options.settingsFile = dir.path() + QStringLiteral("/settings.ini");
        options.clockMs      = [&now] { return now; };
        options.bridgeFactory = [](quint16 preferred, int* error) -> std::shared_ptr<Bridge> {
            Bridge::Options o = bridgeOptions();
            o.preferredPort   = preferred;
            return Bridge::create(o, error);
        };
        Config c;
        c.on   = true;
        c.host = QStringLiteral("127.0.0.1");
        c.port = fake.port();
        saveConfig(options.settingsFile, c);
        Control control(options);
        control.start();
        QCOMPARE(control.mode(), Mode::Custom);
        RouteReport down;
        down.failure    = Failure::Unreachable;
        down.step       = Step::Connect;
        down.host       = QStringLiteral("www.youtube.com");
        down.generation = control.generation();
        down.via        = Via::Bridge;
        control.report(down);
        QCOMPARE(control.state(), Health::State::Down);
        now += Health::kLocalDelayMs;
        QSignalSpy changed(&control, &Control::healthChanged);
        QVERIFY(control.admits(false)); // Probation: this load holds the probe's slot
        QCOMPARE(control.state(), Health::State::Probation);
        QVERIFY(!control.admits(false));
        const int before = changed.count();
        now += Health::kProbeSlotMs; // the probe ended without a report (its site said "cooling", say)
        QVERIFY(control.admits(false));
        QVERIFY(changed.count() > before);
        control.shutdown();
    }

    // Review fix: "Automatically detect settings" on (the Windows default) with a manual SOCKS-only setting: the player
    // (S13) and the updater (S12) refuse it as without detection, the status row offers Use it (W1).
    void autoDetectSocksOnly()
    {
        windowsproxy::setIeConfigProviderForTests([]() {
            windowsproxy::IeConfig c;
            c.ok         = true;
            c.autoDetect = true;
            c.proxy      = QStringLiteral("socks=127.0.0.1:10808");
            return c;
        });
        const windowsproxy::Summary summary = windowsproxy::windowsProxySummary();
        QCOMPARE(summary.kind, windowsproxy::Summary::Kind::SocksOnly);
        QCOMPARE(summary.socksPort, quint16(10808));
        Snapshot s;
        s.mode       = Mode::System;
        s.generation = 2;
        s.refused    = Failure::None;
        QCOMPARE(upd::sessionPlan(s, summary).refused, Failure::SocksOnlyWindows);
        QTemporaryDir    dir;
        Control::Options options;
        options.settingsFile = dir.path() + QStringLiteral("/settings.ini");
        Control control(options);
        control.start();
        QCOMPARE(control.mode(), Mode::System);
        const PlayerNetwork net = control.playerNetwork();
        QCOMPARE(net.kind, PlayerNetwork::Kind::Blocked);
        QCOMPARE(net.blockedBy, Failure::SocksOnlyWindows);
        // "http=...;socks=..." is SOCKS-only for https as well (WinINet and Chromium use the SOCKS entry for it)
        windowsproxy::setIeConfigProviderForTests([]() {
            windowsproxy::IeConfig c;
            c.ok    = true;
            c.proxy = QStringLiteral("http=127.0.0.1:10809;socks=127.0.0.1:10808");
            return c;
        });
        QCOMPARE(control.playerNetwork().blockedBy, Failure::SocksOnlyWindows);
        QCOMPARE(upd::sessionPlan(s, windowsproxy::windowsProxySummary()).refused, Failure::SocksOnlyWindows);
        control.shutdown();
    }

    // Review fix (D8): a setup script that doesn't answer in time refuses the hop (Slow), never the manual setting or
    // direct with local DNS.
    void pacSlowRefused()
    {
        PacServer pac;
        pac.slow.insert("/slow.pac");
        const QString base  = QStringLiteral("http://127.0.0.1:%1/").arg(pac.port());
        const QString nonce = QString::number(QRandomGenerator::global()->generate());
        windowsproxy::setIeConfigProviderForTests([&]() {
            windowsproxy::IeConfig c;
            c.ok            = true;
            c.autoConfigUrl = base + QStringLiteral("slow.pac?") + nonce;
            return c;
        });
        lp::http::Cancel cancel;
        QElapsedTimer    clock;
        clock.start();
        const lp::http::Route r = lp::http::winHttpTransport()->route(QUrl(QStringLiteral("https://www.example.com/a.png")), cancel, lp::http::nowMs() + 1500);
        if (pac.connections() == 0 && r.via != lp::http::Route::Via::Refused)
            QSKIP("WinHTTP's setup-script service didn't fetch the loopback script on this PC");
        QCOMPARE(r.via, lp::http::Route::Via::Refused);
        QCOMPARE(r.refused, Failure::Slow);
        QVERIFY2(clock.elapsed() < 6000, qPrintable(QString::number(clock.elapsed())));
    }

    void forgetFailures()
    {
        qint64           now = 1000000;
        lp::HostGate     gate([&now] { return now; });
        lp::HostGate::Ticket t;
        qint64           ms = 0;
        // a breaker (5 server errors) on one site, a wall on another, a 429 cooldown on a third
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(gate.acquire(QStringLiteral("www.broken-site.net"), lp::HostGate::Lane::Page, QString(), false, &ms, &t), lp::HostGate::Verdict::Go);
            gate.release(t, lp::HostGate::Outcome::ServerError);
            now += 3000; // past the page lane's spacing and burst
        }
        QCOMPARE(gate.state(QStringLiteral("www.broken-site.net"), lp::HostGate::Lane::Page, QString(), &ms), lp::HostGate::Verdict::Broken);
        gate.markWalled(QStringLiteral("www.walled-site.org"), QStringLiteral("x"));
        QCOMPARE(gate.acquire(QStringLiteral("www.cool-site.com"), lp::HostGate::Lane::Page, QString(), false, &ms, &t), lp::HostGate::Verdict::Go);
        gate.release(t, lp::HostGate::Outcome::RateLimited, 120000);
        lp::HostGate::Ticket running;
        QCOMPARE(gate.acquire(QStringLiteral("cdn.busy-site.io"), lp::HostGate::Lane::Media, QString(), false, &ms, &running), lp::HostGate::Verdict::Go);
        const int runningBefore = gate.runningCount();
        gate.forgetFailures();
        QCOMPARE(gate.state(QStringLiteral("www.broken-site.net"), lp::HostGate::Lane::Page, QString(), &ms), lp::HostGate::Verdict::Go);
        QCOMPARE(gate.state(QStringLiteral("www.walled-site.org"), lp::HostGate::Lane::Page, QStringLiteral("x"), &ms), lp::HostGate::Verdict::Go);
        QCOMPARE(gate.state(QStringLiteral("www.cool-site.com"), lp::HostGate::Lane::Page, QString(), &ms), lp::HostGate::Verdict::Cooling);
        QCOMPARE(gate.runningCount(), runningBefore);
    }

    void walledRecordAfterRouteChange()
    {
        FetchHarness h;
        const QString page = QStringLiteral("https://news.example.org/walled");
        ScriptedHop   wall;
        wall.status      = 403;
        wall.challenge   = true;
        wall.contentType = "text/html";
        wall.body        = "<html><head><title>Just a moment...</title></head></html>";
        h.http->hops.insert(page, wall);
        QCOMPARE(h.load(page).error, QString::fromLatin1(lp::errors::kWalled));
        lp::Result cached;
        QVERIFY(lp::Fetcher::cached(lp::classify(page), h.dir.path(), &cached));
        QCOMPARE(cached.error, QString::fromLatin1(lp::errors::kWalled));
        markProxyRouteChange(QDateTime::currentMSecsSinceEpoch() + 2000); // the route became the chat proxy after it
        QVERIFY(!lp::Fetcher::cached(lp::classify(page), h.dir.path(), &cached));
    }

    // 28. proxy::Control
    void controlApply()
    {
        QTemporaryDir   dir;
        FakeSocks       fake;
        std::atomic_int made{0};
        bool            failBind = false;
        Control::Options options;
        options.settingsFile  = dir.path() + QStringLiteral("/settings.ini");
        options.bridgeFactory = [&](quint16 preferred, int* error) -> std::shared_ptr<Bridge> {
            ++made;
            if (failBind) {
                *error = WSAEADDRINUSE;
                return nullptr;
            }
            Bridge::Options o = bridgeOptions();
            o.preferredPort   = preferred;
            return Bridge::create(o, error);
        };
        Control control(options);
        QSignalSpy routeChanged(&control, &Control::routeChanged);
        control.start();
        QCOMPARE(control.mode(), Mode::System);
        QCOMPARE(control.generation(), quint64(1));
        QCOMPARE(snapshot()->mode, Mode::System);
        QCOMPARE(made.load(), 0); // no bridge outside custom mode
        // nothing saved: turning on changes nothing
        QCOMPARE(control.turnOn(Control::Origin::Menu, 0), Toggle::NotSetUp);
        QCOMPARE(control.generation(), quint64(1));
        QCOMPARE(control.turnOff(Control::Origin::Menu, 0), Toggle::AlreadyOff);
        // whenOff decides the off mode
        Config c  = control.config();
        c.whenOff = WhenOff::Direct;
        QVERIFY(control.save(c));
        QCOMPARE(control.mode(), Mode::Direct);
        QCOMPARE(control.generation(), quint64(2));
        // entering custom creates the bridge before publishing
        c      = control.config();
        c.on   = true;
        c.type = Type::Socks5;
        c.host = QStringLiteral("127.0.0.1");
        c.port = fake.port();
        QVERIFY(control.save(c));
        QCOMPARE(control.mode(), Mode::Custom);
        QCOMPARE(control.generation(), quint64(3));
        QCOMPARE(made.load(), 1);
        std::shared_ptr<Bridge> bridge = control.bridge();
        QVERIFY(bridge);
        QCOMPARE(snapshot()->bridgePort, bridge->port());
        QCOMPARE(snapshot()->token, bridge->token());
        QCOMPARE(snapshot()->refused, Failure::None);
        QVERIFY(bridge->hasUpstream());
        QVERIFY(lastProxyRouteChangeMs() > 0);
        QCOMPARE(loadConfig(options.settingsFile).bridgePort, bridge->port()); // the port is saved
        // whenOff while on: no new generation
        c         = control.config();
        c.whenOff = WhenOff::System;
        control.save(c);
        QCOMPARE(control.generation(), quint64(3));
        QCOMPARE(control.turnOn(Control::Origin::Hotkey, 0), Toggle::AlreadyOn);
        // leaving custom keeps the bridge with no upstream (F5)
        QCOMPARE(control.turnOff(Control::Origin::Command, 0), Toggle::TurnedOff);
        QCOMPARE(control.mode(), Mode::System);
        QCOMPARE(control.generation(), quint64(4));
        QCOMPARE(control.bridge(), bridge);
        QVERIFY(!bridge->hasUpstream());
        QVERIFY(control.config().saved()); // nothing deleted
        // entering again reuses the token, realm and port
        const QByteArray token = bridge->token();
        QCOMPARE(control.toggle(Control::Origin::Hotkey, 0), Toggle::TurnedOn);
        QCOMPARE(control.generation(), quint64(5));
        QCOMPARE(control.bridge(), bridge);
        QCOMPARE(snapshot()->token, token);
        QCOMPARE(made.load(), 1);
        QVERIFY(routeChanged.count() >= 5);
        PlayerNetwork net = control.playerNetwork();
        QCOMPARE(net.kind, PlayerNetwork::Kind::Bridge);
        QCOMPARE(net.bridgePort, bridge->port());
        QCOMPARE(net.token, token);
        // route health: decisive -> down, admits only clicks; an old generation's report is ignored
        RouteReport report;
        report.failure    = Failure::Unreachable;
        report.step       = Step::Connect;
        report.host       = QStringLiteral("www.youtube.com");
        report.generation = 4;
        report.via        = Via::Bridge;
        control.report(report);
        QCOMPARE(control.state(), Health::State::Unknown);
        report.generation = 5;
        control.report(report);
        QCOMPARE(control.state(), Health::State::Down);
        QVERIFY(!control.admits(false));
        QVERIFY(control.admits(true));
        control.reportOk(5);
        QCOMPARE(control.state(), Health::State::Working);
        control.shutdown();
        QCOMPARE(bridge->liveThreads(), 0);
        QCOMPARE(snapshot()->mode, Mode::Unset); // anything late fails closed
    }

    void controlBindFailure()
    {
        QTemporaryDir    dir;
        Control::Options options;
        options.settingsFile  = dir.path() + QStringLiteral("/settings.ini");
        options.bridgeFactory = [](quint16, int* error) -> std::shared_ptr<Bridge> {
            *error = WSAEADDRINUSE;
            return nullptr;
        };
        Config c;
        c.on   = true;
        c.host = QStringLiteral("127.0.0.1");
        c.port = 2080;
        saveConfig(options.settingsFile, c);
        Control control(options);
        control.start();
        QCOMPARE(control.mode(), Mode::Custom); // never direct
        QCOMPARE(snapshot()->refused, Failure::BridgeUnavailable);
        QVERIFY(control.bridgeFailed());
        QCOMPARE(control.playerNetwork().kind, PlayerNetwork::Kind::Blocked);
        QCOMPARE(control.lastFailure(), Failure::BridgeUnavailable);
        control.shutdown();
    }

    void controlPasswordUnreadable()
    {
        QTemporaryDir    dir;
        Control::Options options;
        options.settingsFile = dir.path() + QStringLiteral("/settings.ini");
        Config c;
        c.on   = true;
        c.host = QStringLiteral("127.0.0.1");
        c.port = 2080;
        c.user = QStringLiteral("tester");
        Endpoint other;
        other.host = QStringLiteral("127.0.0.1");
        other.port = 2081; // sealed for another port: a hand-edited INI (S7)
        other.user = QStringLiteral("tester");
        QVERIFY(protect::seal(QStringLiteral("pw"), other, &c.passwordBlob));
        saveConfig(options.settingsFile, c);
        Control control(options);
        control.start();
        QCOMPARE(control.mode(), Mode::Custom);
        QCOMPARE(snapshot()->refused, Failure::PasswordUnreadable);
        QCOMPARE(control.lastFailure(), Failure::PasswordUnreadable);
        control.shutdown();
    }

    void systemModeNoStuckPause()
    {
        QTemporaryDir    dir;
        qint64           now = 5000;
        Control::Options options;
        options.settingsFile = dir.path() + QStringLiteral("/settings.ini");
        options.clockMs      = [&now] { return now; };
        Control control(options);
        control.start();
        QCOMPARE(control.mode(), Mode::System);
        RouteReport report;
        report.failure    = Failure::Unreachable;
        report.step       = Step::Connect;
        report.generation = control.generation();
        report.via        = Via::System;
        control.report(report);
        QCOMPARE(control.windowsStatus().lastFailure, Failure::Unreachable);
        for (int i = 0; i < 10; ++i)
            QVERIFY(control.admits(false)); // no gate, no pause in Windows mode (F2)
        control.reportSiteResponded();
        QCOMPARE(control.windowsStatus().lastFailure, Failure::None);
        control.shutdown();
    }

    void dpapiOncePerGeneration()
    {
        SocksScript script;
        script.method = 0x02;
        FakeSocks fake(script);
        auto      bridge = Bridge::create(bridgeOptions());
        auto      spec   = upstream(Type::Socks5, fake.port(), "tester");
        QVERIFY(protect::seal(QStringLiteral("pw-20"), spec->endpoint(), &spec->passwordBlob)); // opened lazily by the first tunnel
        bridge->setUpstream(std::move(spec));
        const int before = protect::unprotectCalls();
        for (int i = 0; i < 20; ++i) {
            Tunnel t = connectThrough(bridge, "www.youtube.com:443", static_cast<quint64>(i + 1));
            QCOMPARE(t.status(), QByteArray("HTTP/1.1 200 Connection established"));
        }
        QCOMPARE(protect::unprotectCalls(), before + 1);
        QCOMPARE(fake.seen().last().loginPass, QByteArray("pw-20"));
        // a transient DPAPI error: Internal (not decisive), and the next tunnel tries again
        auto again = upstream(Type::Socks5, fake.port(), "tester", nullptr, "127.0.0.1", 2);
        QVERIFY(protect::seal(QStringLiteral("pw-21"), again->endpoint(), &again->passwordBlob));
        bridge->setUpstream(std::move(again));
        protect::setUnprotectErrorForTests(ERROR_ACCESS_DENIED);
        Tunnel failed = connectThrough(bridge, "www.youtube.com:443", 100);
        QVERIFY(failed.head.contains("X-TSMedia-Proxy: internal"));
        QVERIFY(!isDecisive(Failure::Internal, Step::Login));
        protect::setUnprotectErrorForTests(0);
        Tunnel ok = connectThrough(bridge, "www.youtube.com:443", 101);
        QCOMPARE(ok.status(), QByteArray("HTTP/1.1 200 Connection established"));
        bridge->stop(2000);
    }

    // 29. the Test and Find jobs
    void testRun()
    {
        const auto run = [](TestRun::Options options, const Config& form, const char* typed = nullptr) {
            options.transport = std::make_shared<BridgeSocketTransport>();
            TestRun    test(options, nullptr);
            QSignalSpy spy(&test, &TestRun::finished);
            protect::SecretBytes password;
            if (typed)
                password.assign(typed, static_cast<int>(strlen(typed)));
            if (!test.start(form, std::move(password), QByteArray()))
                return TestResult();
            spy.wait(15000);
            return test.result();
        };
        Config form;
        form.type = Type::Socks5;
        form.host = QStringLiteral("127.0.0.1");
        {
            FakeSocks fake;
            form.port = fake.port();
            const TestResult r = run(TestRun::Options(), form);
            QCOMPARE(r.outcome, TestResult::Outcome::Works);
            QVERIFY(testResultText(r, QString(), 0).startsWith(QStringLiteral("Works · ")));
            QCOMPARE(fake.seen().value(0).target, QByteArray("www.youtube.com"));
        }
        {
            FakeSocks fake;
            form.port = fake.port();
            form.user = QStringLiteral("tester");
            const TestResult r = run(TestRun::Options(), form, "pw");
            QCOMPARE(r.outcome, TestResult::Outcome::Works);
            QVERIFY(r.loginUnused);
            QVERIFY(testResultText(r, QString(), 0).endsWith(QStringLiteral("(the proxy didn't ask for the login)")));
            form.user.clear();
        }
        {
            SocksScript s;
            s.rep = 8;
            FakeSocks fake(s);
            form.port          = fake.port();
            const TestResult r = run(TestRun::Options(), form);
            QCOMPARE(r.outcome, TestResult::Outcome::Failed);
            QCOMPARE(r.failure, Failure::NoNameConnect);
            QVERIFY(testResultText(r, QString(), 0).contains(QStringLiteral("by name")));
        }
        {
            form.port          = closedPort();
            const TestResult r = run(TestRun::Options(), form);
            QCOMPARE(r.failure, Failure::Unreachable);
            QCOMPARE(testResultText(r, QStringLiteral("127.0.0.1:%1").arg(form.port), form.port),
                     QStringLiteral("Can't reach the proxy at 127.0.0.1:%1. Is your VPN app running, and is %1 its port?").arg(form.port));
        }
        {
            FakeSocks        fake;
            TestRun::Options o;
            o.upstream.resolver = [](const QString&, QVector<lp::net::Address>* out, const HANDLE*, int, int) {
                *out = {lp::net::parseAddress(QStringLiteral("10.10.34.35"))};
                return 0;
            };
            Config named = form;
            named.host   = QStringLiteral("proxy.example.net");
            named.port   = fake.port();
            const TestResult r = run(o, named);
            QCOMPARE(r.failure, Failure::ProxyNameFiltered);
            QCOMPARE(fake.connections(), 0);
        }
        {
            HttpScript s;
            s.garbage = true;
            FakeHttpProxy fake(s);
            Config        http = form;
            http.type          = Type::Http;
            http.port          = fake.port();
            const TestResult r = run(TestRun::Options(), http);
            QCOMPARE(r.failure, Failure::NotHttp);
            QCOMPARE(testResultText(r, QString(), 0), QStringLiteral("This port doesn't answer as an HTTP proxy. Try SOCKS5 proxy."));
        }
        // cancel mid-run: nothing posted; destroying a running Test returns at once; shutdown joins the worker (F22)
        {
            SocksScript silent;
            silent.silent = true;
            FakeSocks        fake(silent);
            TestRun::Options o;
            o.transport = std::make_shared<BridgeSocketTransport>();
            form.port   = fake.port();
            auto*      test = new TestRun(o, nullptr);
            QSignalSpy spy(test, &TestRun::finished);
            QVERIFY(test->start(form, protect::SecretBytes(), QByteArray()));
            QThread::msleep(100);
            test->cancel();
            QVERIFY(!spy.wait(500));
            QVERIFY(test->start(form, protect::SecretBytes(), QByteArray()));
            QElapsedTimer clock;
            clock.start();
            delete test;
            QVERIFY(clock.elapsed() < 50);
            QVERIFY(workerCount() >= 1);
            joinWorkers(10000);
            QCOMPARE(workerCount(), 0);
        }
    }

    void findRun()
    {
        FakeSocks  first, second, bridgePort;
        FindRun::Options o;
        o.ports      = {closedPort(), bridgePort.port(), first.port(), second.port()};
        o.bridgePort = bridgePort.port();
        FindRun    find(o, nullptr);
        QSignalSpy spy(&find, &FindRun::finished);
        QVERIFY(find.start());
        QVERIFY(spy.wait(5000));
        QCOMPARE(find.found(), (QVector<quint16>{first.port(), second.port()}));
        QCOMPARE(bridgePort.connections(), 0); // never TS Media's own port
        QVERIFY(waitFor([&]() { return first.seen().size() == 1; }));
        QCOMPARE(first.seen().value(0).greeting, QByteArray("\x05\x01\x00", 3)); // nothing but the greeting
        QCOMPARE(first.seen().value(0).atyp, 0);
        QCOMPARE(findResultText(find.found()),
                 QStringLiteral("Found proxies on this PC at ports %1, %2. TS Media chose %1. Press Test, then OK.").arg(first.port()).arg(second.port()));
        QVERIFY(findResultText({}).startsWith(QStringLiteral("No VPN app answered")));
        QCOMPARE(findResultText({2080}), QStringLiteral("Found a proxy on this PC at port 2080. Press Test, then OK."));
    }

    // 30. the updater's route (S12, S3, F8)
    void updaterPlan()
    {
        const windowsproxy::Summary none;
        windowsproxy::Summary       socksOnly;
        socksOnly.kind = windowsproxy::Summary::Kind::SocksOnly;
        Snapshot s;
        upd::SessionPlan p = upd::sessionPlan(s, none);
        QCOMPARE(p.refused, Failure::NotReady);
        s.mode       = Mode::System;
        s.generation = 3;
        s.refused    = Failure::None;
        p            = upd::sessionPlan(s, none);
        QCOMPARE(p.access, upd::SessionPlan::Access::Automatic);
        QVERIFY(!p.useCredentials && !p.autologonHigh);
        QCOMPARE(p.refused, Failure::None);
        QCOMPARE(p.generation, quint64(3));
        p = upd::sessionPlan(s, socksOnly);
        QCOMPARE(p.refused, Failure::SocksOnlyWindows);
        s.mode = Mode::Direct;
        p      = upd::sessionPlan(s, socksOnly);
        QCOMPARE(p.access, upd::SessionPlan::Access::NoProxy);
        QCOMPARE(p.refused, Failure::None);
        s.mode       = Mode::Custom;
        s.bridgePort = 40123;
        s.token      = "tok";
        p            = upd::sessionPlan(s, none);
        QCOMPARE(p.access, upd::SessionPlan::Access::Named);
        QCOMPARE(p.proxy, QStringLiteral("127.0.0.1:40123"));
        QVERIFY(p.useCredentials && p.autologonHigh);
        QCOMPARE(p.via, Via::Bridge);
        s.refused = Failure::PasswordUnreadable;
        QCOMPARE(upd::sessionPlan(s, none).refused, Failure::PasswordUnreadable);
        // a changed snapshot between two hops: another session
        Snapshot custom = s;
        custom.refused  = Failure::None;
        Snapshot direct;
        direct.mode    = Mode::Direct;
        direct.refused = Failure::None;
        QVERIFY(!upd::sessionPlan(custom, none).sameSession(upd::sessionPlan(direct, none)));
        QVERIFY(upd::sessionPlan(custom, none).sameSession(upd::sessionPlan(custom, none)));
        // the status mapping: only with IS_PROXY_CONNECT_RESPONSE is it the proxy's (S3)
        Failure f = Failure::None;
        QCOMPARE(upd::statusError(407, true, &f), upd::http::Error::Proxy);
        QCOMPARE(f, Failure::NeedsLogin);
        QCOMPARE(upd::statusError(502, true, &f), upd::http::Error::Proxy);
        QCOMPARE(f, Failure::CantReachSite);
        QCOMPARE(upd::statusError(403, true, &f), upd::http::Error::Proxy);
        QCOMPARE(f, Failure::NotAllowed);
        QCOMPARE(upd::statusError(407, false, &f), upd::http::Error::BadResponse);
        QCOMPARE(f, Failure::None);
        QCOMPARE(upd::statusError(502, false, &f), upd::http::Error::ServerError);
        QCOMPARE(upd::statusError(404, false, &f), upd::http::Error::NotFound);
        QCOMPARE(upd::statusError(429, false, &f), upd::http::Error::RateLimited);
        // the texts
        QCOMPARE(upd::proxyShortError(Failure::Unreachable, Via::Bridge), QStringLiteral("couldn't reach GitHub through the chat proxy (can't reach the proxy)"));
        QCOMPARE(upd::proxyShortError(Failure::NeedsLogin, Via::System),
                 QStringLiteral("couldn't reach GitHub through your Windows proxy (the proxy asks for a login)"));
        const upd::ProxyErrorCopy copy = upd::proxyErrorCopy(Failure::LoginRefused, Via::Bridge);
        QCOMPARE(copy.heading, QStringLiteral("Couldn't reach GitHub through the chat proxy"));
        QCOMPARE(copy.body,
                 QStringLiteral("The proxy refused the login. Nothing was changed. Check the proxy in the Chat proxy window, or turn it off, then try again."));
        // updaterOldGenerationQuiet (F8)
        QVERIFY(upd::proxyFailureIsQuiet(3, 4, Failure::Unreachable));
        QVERIFY(upd::proxyFailureIsQuiet(3, 4, Failure::Off));
        QVERIFY(!upd::proxyFailureIsQuiet(4, 4, Failure::Unreachable));
        QVERIFY(!upd::proxyFailureIsQuiet(3, 4, Failure::Internal));
        QVERIFY(!upd::proxyFailureIsQuiet(0, 4, Failure::Unreachable));
    }

    // 31. source rules (F20 and the rest)
    void sourceRules()
    {
        QStringList files;
        QDirIterator it(QStringLiteral(TSMEDIA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            files << QDir(QStringLiteral(TSMEDIA_SOURCE_DIR)).relativeFilePath(it.next());
        QVERIFY(files.size() > 100);
        const QRegularExpression delayedSingleShot(QStringLiteral("QTimer::singleShot\\(\\s*([^,]+),"));
        for (const QString& file : files) {
            const QString text = sourceText(file);
            for (const QString& line : text.split(QLatin1Char('\n'))) {
                const int     comment = line.indexOf(QStringLiteral("//"));
                const QString code    = comment >= 0 ? line.left(comment) : line;
                if (code.contains(QStringLiteral("WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY")) || code.contains(QStringLiteral("WINHTTP_ACCESS_TYPE_DEFAULT_PROXY")))
                    QVERIFY2(file == QStringLiteral("src/update/httpclient.cpp"), qPrintable(file)); // the Windows-settings branch only
            }
            if (file.startsWith(QStringLiteral("src/linkpreview/")) || file.startsWith(QStringLiteral("src/update/")) || file.startsWith(QStringLiteral("src/proxy/"))) {
                // never a Proxy-Authorization header added by hand (it would reach the website inside TLS)
                for (const QString& line : text.split(QLatin1Char('\n'))) {
                    if (line.contains(QStringLiteral("\"Proxy-Authorization")) && !line.trimmed().startsWith(QStringLiteral("//")))
                        QVERIFY2(file.startsWith(QStringLiteral("src/proxy/wire.")), qPrintable(file + QStringLiteral(": ") + line));
                }
            }
            for (const QString& line : text.split(QLatin1Char('\n'))) {
                const int     comment = line.indexOf(QStringLiteral("//"));
                const QString code    = comment >= 0 ? line.left(comment) : line; // not what the comments say
                QVERIFY2(!code.contains(QStringLiteral("CRYPTPROTECT_LOCAL_MACHINE")), qPrintable(file));
                QVERIFY2(!code.contains(QStringLiteral("SECURITY_FLAG_IGNORE_")), qPrintable(file));
            }
            QVERIFY2(!QRegularExpression(QStringLiteral("route\\.viaProxy\\b(?!\\()")).match(text).hasMatch(), qPrintable(file)); // the function only
            const bool crashRuleScope = file.startsWith(QStringLiteral("src/proxy/")) || file.startsWith(QStringLiteral("src/ytplayer/"))
                                        || file.startsWith(QStringLiteral("src/update/")) || file.startsWith(QStringLiteral("src/linkpreview/"))
                                        || (file.startsWith(QStringLiteral("src/chat")) && file.endsWith(QStringLiteral(".cpp")))
                                        || file == QStringLiteral("src/plugin.cpp") || file == QStringLiteral("src/privacysection.cpp");
            if (crashRuleScope) {
                auto m = delayedSingleShot.globalMatch(text);
                while (m.hasNext()) {
                    const QString delay = m.next().captured(1).trimmed();
                    QVERIFY2(delay == QStringLiteral("0"), qPrintable(file + QStringLiteral(": QTimer::singleShot(") + delay));
                }
            }
        }
        QVERIFY(!sourceText(QStringLiteral("src/proxy/proxyconfig.cpp")).contains(QStringLiteral("QStringLiteral")));
        QVERIFY(sourceText(QStringLiteral("src/proxy/protect.cpp")).contains(QStringLiteral("CRYPTPROTECT_UI_FORBIDDEN")));
        // the plugin never links ws2_32, crypt32 or iphlpapi (loaded at run time from System32)
        const QString cmake = sourceText(QStringLiteral("CMakeLists.txt"));
        const QRegularExpression link(QStringLiteral("target_link_libraries\\(tsmedia\\s[^)]*\\)"));
        auto m = link.globalMatch(cmake);
        QVERIFY(m.hasNext());
        while (m.hasNext()) {
            const QString call = m.next().captured(0);
            for (const char* lib : {"ws2_32", "crypt32", "iphlpapi"})
                QVERIFY2(!call.contains(QLatin1String(lib)), qPrintable(call));
        }
    }
};

TSMEDIA_REGISTER_TEST(TestProxyBridge)
#include "tst_proxybridge.moc"
