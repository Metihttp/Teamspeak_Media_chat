// 2.6 chat proxy, the pure core (design 9.1, tests 1-12): settings keys, the host rules and descriptions, the DPAPI
// password, the failure table, the SOCKS5 and HTTP CONNECT wire, the bridge's head parser and target policy, route
// health, the route snapshot, the Windows proxy mapping, the DNS answer classification and the paste parser.
// No sockets here (tst_proxybridge.cpp has them).

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <windows.h>

#include <atomic>

#include "linkpreview/linkurl.h"
#include "linkpreview/netpolicy.h"
#include "proxy/failure.h"
#include "proxy/health.h"
#include "proxy/linkparse.h"
#include "proxy/protect.h"
#include "proxy/proxyconfig.h"
#include "proxy/router.h"
#include "proxy/windowsproxy.h"
#include "proxy/wire.h"
#include "settings.h"
#include "testmain.h"

using namespace proxy;
using linkpreview::net::Address;
using linkpreview::net::parseAddress;

namespace {

// Every Fetcher test of the suite runs on the Windows-settings route (as a fresh 2.6 starts): a test that publishes
// another snapshot puts this one back.
void publishWindowsRoute()
{
    auto snap        = std::make_shared<Snapshot>();
    snap->mode       = Mode::System;
    snap->generation = 1;
    snap->refused    = Failure::None;
    publish(snap);
}

QString iniPath(const QTemporaryDir& dir)
{
    return dir.path() + QStringLiteral("/settings.ini");
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

Endpoint endpointOf(Type type, const char* host, quint16 port, const char* user)
{
    Endpoint e;
    e.type = type;
    e.host = QString::fromLatin1(host);
    e.port = port;
    e.user = QString::fromLatin1(user);
    return e;
}

std::atomic<int> g_wipedAllZero{0};
std::atomic<int> g_wipedNotZero{0};
void             observeWipe(const char* data, int size)
{
    bool zero = true;
    for (int i = 0; i < size; ++i)
        zero = zero && data[i] == 0;
    (zero ? g_wipedAllZero : g_wipedNotZero)++;
}

QByteArray basicHeader(const QByteArray& user, const QByteArray& token)
{
    return "Proxy-Authorization: Basic " + (user + ':' + token).toBase64() + "\r\n";
}

QByteArray connectHead(const QByteArray& authority, const QByteArray& extraHeaders)
{
    return "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n" + extraHeaders + "\r\n";
}

} // namespace

TSMEDIA_TEST_SETUP(proxyWindowsRoute, &publishWindowsRoute)

class TestProxyCore : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        protect::setUnprotectErrorForTests(0);
        protect::setWipeObserverForTests(nullptr);
        windowsproxy::setIeConfigProviderForTests({});
        publishWindowsRoute();
    }

    // 1. settings.ini (design 2.1)
    void configKeys()
    {
        QTemporaryDir dir;
        const QString file = iniPath(dir);
        const Config  none = loadConfig(file);
        QVERIFY(!none.on);
        QCOMPARE(none.whenOff, WhenOff::System);
        QCOMPARE(none.type, Type::Socks5);
        QVERIFY(none.host.isEmpty());
        QCOMPARE(none.port, quint16(0));
        QVERIFY(none.passwordBlob.isEmpty());
        QCOMPARE(none.effectiveMode(), Mode::System);

        Config c;
        c.on      = true;
        c.whenOff = WhenOff::Direct;
        c.type    = Type::Http;
        c.host    = QStringLiteral("[::1]");
        c.port    = 8080;
        c.user    = QStringLiteral("someone");
        QVERIFY(protect::seal(QStringLiteral("very-secret-pass"), endpointOf(Type::Http, "::1", 8080, "someone"), &c.passwordBlob));
        c.bridgePort = 40123;
        saveConfig(file, c);
        const Config back = loadConfig(file);
        QVERIFY(back.on);
        QCOMPARE(back.whenOff, WhenOff::Direct);
        QCOMPARE(back.type, Type::Http);
        QCOMPARE(back.host, QStringLiteral("::1")); // brackets gone
        QCOMPARE(back.port, quint16(8080));
        QCOMPARE(back.user, QStringLiteral("someone"));
        QCOMPARE(back.passwordBlob, c.passwordBlob);
        QCOMPARE(back.bridgePort, quint16(40123));
        QCOMPARE(back.effectiveMode(), Mode::Custom);
        // the plain password is never in the file
        QVERIFY(!fileBytes(file).contains("very-secret-pass"));
        QVERIFY(fileBytes(file).contains("proxyPassword=dpapi1:"));

        // garbage reads as the defaults
        {
            QSettings s(file, QSettings::IniFormat);
            s.setValue(QStringLiteral("proxyOn"), QStringLiteral("yes"));
            s.setValue(QStringLiteral("proxyWhenOff"), QStringLiteral("socks"));
            s.setValue(QStringLiteral("proxyType"), QStringLiteral("socks4"));
            s.setValue(QStringLiteral("proxyHost"), QStringLiteral("http://x"));
            s.setValue(QStringLiteral("proxyPort"), QStringLiteral("70000"));
            s.setValue(QStringLiteral("proxyPassword"), QStringLiteral("plain-text"));
            s.setValue(QStringLiteral("proxyBridgePort"), QStringLiteral("80"));
        }
        const Config junk = loadConfig(file);
        QVERIFY(!junk.on);
        QCOMPARE(junk.whenOff, WhenOff::System);
        QCOMPARE(junk.type, Type::Socks5);
        QVERIFY(junk.host.isEmpty());
        QCOMPARE(junk.port, quint16(0));
        QVERIFY(junk.passwordBlob.isEmpty());
        QCOMPARE(junk.bridgePort, quint16(0));

        // proxyOn=true without a saved endpoint loads as off
        {
            QSettings s(file, QSettings::IniFormat);
            s.setValue(QStringLiteral("proxyOn"), QStringLiteral("true"));
        }
        QVERIFY(!loadConfig(file).on);

        // an http user name with ':' loads empty and drops the blob
        Config h;
        h.type = Type::Http;
        h.host = QStringLiteral("127.0.0.1");
        h.port = 2080;
        h.user = QStringLiteral("ok");
        QVERIFY(protect::seal(QStringLiteral("pw"), h.endpoint(), &h.passwordBlob));
        saveConfig(file, h);
        {
            QSettings s(file, QSettings::IniFormat);
            s.setValue(QStringLiteral("proxyUser"), QStringLiteral("a:b"));
        }
        const Config colon = loadConfig(file);
        QVERIFY(colon.user.isEmpty());
        QVERIFY(colon.passwordBlob.isEmpty());

        // Settings::save() of a default Settings leaves every proxy* key alone
        saveConfig(file, c);
        Settings().save(file);
        const Config after = loadConfig(file);
        QVERIFY(after.on);
        QCOMPARE(after.host, QStringLiteral("::1"));
        QCOMPARE(after.passwordBlob, c.passwordBlob);
        QCOMPARE(after.bridgePort, quint16(40123));

        // sameRoute: whenOff counts only while off
        Config a = c, b = c;
        b.whenOff = WhenOff::System;
        QVERIFY(a.sameRoute(b));
        a.on = b.on = false;
        QVERIFY(!a.sameRoute(b));
        b.whenOff = WhenOff::Direct;
        QVERIFY(a.sameRoute(b));
        Config d = c;
        d.port   = 8081;
        QVERIFY(!c.sameRoute(d));
    }

    // 2. host rules, places, descriptions
    void hostRules()
    {
        for (const char* ok : {"127.0.0.1", "[::1]", "::1", "proxy.example.net", "localhost", "Proxy.Example.NET", "10.0.0.2", "fe80::1"})
            QVERIFY2(hostError(QString::fromLatin1(ok)) == HostError::None, ok);
        QCOMPARE(hostError(QString()), HostError::Empty);
        QCOMPARE(hostError(QStringLiteral("   ")), HostError::Empty);
        const QString longName = QString(254, QLatin1Char('a'));
        for (const QString& bad : {QStringLiteral("http://x"), QStringLiteral("a b"), QStringLiteral("x:1"), longName, QStringLiteral("0.0.0.0"),
                                   QStringLiteral("::"), QStringLiteral("224.0.0.1"), QStringLiteral("255.255.255.255"), QStringLiteral("[name]"),
                                   QStringLiteral("1.2.3"), QStringLiteral("a..b"), QStringLiteral("-a.b"), QStringLiteral("ff02::1")})
            QVERIFY2(hostError(bad) == HostError::Invalid, qPrintable(bad));
        QCOMPARE(normalizeHost(QStringLiteral(" [::1] ")), QStringLiteral("::1"));
        QCOMPARE(normalizeHost(QStringLiteral("Proxy.Example.NET.")), QStringLiteral("proxy.example.net"));
        QCOMPARE(hostPort(QStringLiteral("::1"), 8080), QStringLiteral("[::1]:8080"));

        for (const char* here : {"127.0.0.1", "127.8.9.1", "::1", "localhost"})
            QVERIFY2(placeOf(QString::fromLatin1(here)) == Place::ThisPc, here);
        for (const char* lan : {"10.0.0.2", "192.168.1.5", "172.16.3.4", "169.254.1.1", "fe80::1", "fd00::5"})
            QVERIFY2(placeOf(QString::fromLatin1(lan)) == Place::LocalNetwork, lan);
        for (const char* remote : {"203.0.113.9", "proxy.example.net", "172.32.0.1"})
            QVERIFY2(placeOf(QString::fromLatin1(remote)) == Place::Elsewhere, remote);

        Config c;
        c.type = Type::Socks5;
        c.host = QStringLiteral("127.0.0.1");
        c.port = 2080;
        QCOMPARE(describe(c, Where::Window), QStringLiteral("SOCKS5 proxy 127.0.0.1:2080"));
        QCOMPARE(describe(c, Where::ForChat), QStringLiteral("SOCKS5 proxy 127.0.0.1:2080"));
        c.user = QStringLiteral("someone");
        QVERIFY(!describe(c, Where::ForChat).contains(QStringLiteral("someone")));
        c.type = Type::Http;
        c.host = QStringLiteral("::1");
        c.port = 8080;
        QCOMPARE(describe(c, Where::Window), QStringLiteral("HTTP proxy [::1]:8080"));
        c.host = QStringLiteral("192.168.1.5");
        QCOMPARE(describe(c, Where::ForChat), QStringLiteral("HTTP proxy on the local network")); // F24: no address in chat lines
        c.type = Type::Socks5;
        c.host = QStringLiteral("proxy.example.net");
        QCOMPARE(describe(c, Where::ForChat), QStringLiteral("SOCKS5 proxy on another computer"));
        QCOMPARE(describe(c, Where::Window), QStringLiteral("SOCKS5 proxy proxy.example.net:8080"));

        // the bridge's own port on this PC is a loop (S17)
        QVERIFY(isBridgeSelfLoop(QStringLiteral("127.0.0.1"), 40123, 40123));
        QVERIFY(isBridgeSelfLoop(QStringLiteral("localhost"), 40123, 40123));
        QVERIFY(!isBridgeSelfLoop(QStringLiteral("127.0.0.1"), 2080, 40123));
        QVERIFY(!isBridgeSelfLoop(QStringLiteral("10.0.0.2"), 40123, 40123));

        QVERIFY(userValid(Type::Socks5, QStringLiteral("a:b")));
        QVERIFY(!userValid(Type::Http, QStringLiteral("a:b")));
        QVERIFY(!userValid(Type::Socks5, QString(256, QLatin1Char('u'))));
    }

    // 3. DPAPI with the endpoint's entropy (S7, S18)
    void dpapiRoundTrip()
    {
        const Endpoint e = endpointOf(Type::Socks5, "127.0.0.1", 2080, "user");
        QByteArray     blob;
        QVERIFY(protect::seal(QString::fromUtf8("p\xc3\xa4ss-w\xc3\xb6rd"), e, &blob));
        QVERIFY(blob.startsWith("dpapi1:"));
        protect::SecretBytes plain;
        QCOMPARE(protect::open(blob, e, &plain), protect::OpenResult::Ok);
        QCOMPARE(QString::fromUtf8(plain.data(), plain.size()), QString::fromUtf8("p\xc3\xa4ss-w\xc3\xb6rd"));

        // another endpoint (each field alone) can't open it
        QCOMPARE(protect::open(blob, endpointOf(Type::Http, "127.0.0.1", 2080, "user"), &plain), protect::OpenResult::Unreadable);
        QCOMPARE(protect::open(blob, endpointOf(Type::Socks5, "127.0.0.2", 2080, "user"), &plain), protect::OpenResult::Unreadable);
        QCOMPARE(protect::open(blob, endpointOf(Type::Socks5, "127.0.0.1", 2081, "user"), &plain), protect::OpenResult::Unreadable);
        QCOMPARE(protect::open(blob, endpointOf(Type::Socks5, "127.0.0.1", 2080, "other"), &plain), protect::OpenResult::Unreadable);
        QVERIFY(plain.isEmpty());
        // one byte changed, no prefix, bad base64
        QByteArray changed = blob;
        const int  at      = changed.size() / 2;
        changed[at]        = changed.at(at) == 'A' ? 'B' : 'A';
        QCOMPARE(protect::open(changed, e, &plain), protect::OpenResult::Unreadable);
        QCOMPARE(protect::open(blob.mid(7), e, &plain), protect::OpenResult::Unreadable);
        QCOMPARE(protect::open(QByteArray("dpapi1:!!!!"), e, &plain), protect::OpenResult::Unreadable);
        // an injected other error is transient (S18); ERROR_INVALID_DATA and NTE_BAD_KEY_STATE are unreadable
        const int callsBefore = protect::unprotectCalls();
        protect::setUnprotectErrorForTests(ERROR_ACCESS_DENIED);
        QCOMPARE(protect::open(blob, e, &plain), protect::OpenResult::Transient);
        protect::setUnprotectErrorForTests(ERROR_INVALID_DATA);
        QCOMPARE(protect::open(blob, e, &plain), protect::OpenResult::Unreadable);
        protect::setUnprotectErrorForTests(0x8009000BUL);
        QCOMPARE(protect::open(blob, e, &plain), protect::OpenResult::Unreadable);
        protect::setUnprotectErrorForTests(0);
        QCOMPARE(protect::unprotectCalls(), callsBefore + 3);
        // an empty password: an empty blob, which opens to nothing
        QByteArray empty("x");
        QVERIFY(protect::seal(QString(), e, &empty));
        QVERIFY(empty.isEmpty());
        QCOMPARE(protect::open(QByteArray(), e, &plain), protect::OpenResult::Ok);
        QVERIFY(plain.isEmpty());

        // SecretBytes is wiped before it is freed
        g_wipedAllZero = 0;
        g_wipedNotZero = 0;
        protect::setWipeObserverForTests(&observeWipe);
        {
            protect::SecretBytes s("secret-bytes", 12);
            protect::SecretBytes moved(std::move(s));
            QCOMPARE(moved.size(), 12);
            QVERIFY(s.isEmpty());
        }
        protect::setWipeObserverForTests(nullptr);
        QCOMPARE(g_wipedAllZero.load(), 1);
        QCOMPARE(g_wipedNotZero.load(), 0);

        // the in-memory protected secret
        protect::ProtectedSecret kept;
        QVERIFY(kept.set(protect::SecretBytes("a-17-byte-secret!", 17)));
        protect::SecretBytes shown;
        QVERIFY(kept.reveal(&shown));
        QCOMPARE(QByteArray(shown.data(), shown.size()), QByteArray("a-17-byte-secret!"));
        QVERIFY(kept.reveal(&shown)); // again
        QCOMPARE(shown.size(), 17);
        kept.clear();
        QVERIFY(!kept.isSet());
        QVERIFY(kept.reveal(&shown));
        QVERIFY(shown.isEmpty());

        // the local base64 encoder equals Qt's
        QByteArray input;
        for (int n = 0; n <= 300; ++n) {
            char      out[512];
            const int got = protect::base64Encode(input.constData(), input.size(), out, sizeof(out));
            QCOMPARE(QByteArray(out, got), input.toBase64());
            input.append(static_cast<char>((n * 37 + 11) & 0xff));
        }
        char tiny[3];
        QCOMPARE(protect::base64Encode("abc", 3, tiny, sizeof(tiny)), -1);
    }

    // 4. the failure table (design 5.1)
    void failureTable()
    {
        const Failure all[] = {Failure::Unreachable, Failure::NameNotFound, Failure::ProxyNameFiltered, Failure::NotSocks5, Failure::NotHttp,
                               Failure::NeedsLogin, Failure::LoginRefused, Failure::LoginMethod, Failure::PasswordUnreadable,
                               Failure::BridgeUnavailable, Failure::SocksOnlyWindows, Failure::UnsupportedWindowsProxy, Failure::NotAllowed,
                               Failure::CantReachSite, Failure::NoNameConnect, Failure::Slow, Failure::Intercepted, Failure::Internal,
                               Failure::NotReady, Failure::SiteRefused, Failure::Refused, Failure::Busy, Failure::Off};
        for (Failure f : all) {
            QCOMPARE(failureFromName(QString::fromLatin1(failureName(f))), f);
            if (isRouteLevel(f) || f == Failure::Internal || f == Failure::NotReady) {
                const QString text = shortText(f);
                QVERIFY2(!text.isEmpty(), failureName(f));
                for (const QChar ch : text)
                    QVERIFY2(ch.unicode() < 0x80, failureName(f)); // English, plain ASCII
            }
        }
        for (Failure f : {Failure::Unreachable, Failure::NameNotFound, Failure::ProxyNameFiltered, Failure::NotHttp, Failure::NeedsLogin,
                          Failure::LoginRefused, Failure::LoginMethod, Failure::PasswordUnreadable, Failure::BridgeUnavailable,
                          Failure::SocksOnlyWindows, Failure::UnsupportedWindowsProxy}) {
            QVERIFY(isRouteLevel(f));
            QVERIFY2(isDecisive(f, Step::Request), failureName(f));
        }
        for (Failure f : {Failure::NotAllowed, Failure::CantReachSite, Failure::NoNameConnect, Failure::Slow, Failure::Intercepted}) {
            QVERIFY(isRouteLevel(f));
            QVERIFY2(!isDecisive(f, Step::Greeting), failureName(f));
        }
        for (Failure f : {Failure::Internal, Failure::NotReady, Failure::SiteRefused, Failure::Refused, Failure::Busy, Failure::Off}) {
            QVERIFY2(!isRouteLevel(f), failureName(f));
            QVERIFY(!isDecisive(f, Step::Greeting));
        }
        // NotSocks5: decisive in the greeting only (S3)
        QVERIFY(isDecisive(Failure::NotSocks5, Step::Greeting));
        QVERIFY(!isDecisive(Failure::NotSocks5, Step::Request));
        QCOMPARE(shortText(Failure::Unreachable, QStringLiteral("127.0.0.1:2080")), QStringLiteral("can't reach the proxy at 127.0.0.1:2080"));
        QCOMPARE(shortText(Failure::Unreachable), QStringLiteral("can't reach the proxy"));
    }

    void socksRep8NotDecisive()
    {
        QCOMPARE(wire::failureForRep(7), Failure::NoNameConnect);
        QCOMPARE(wire::failureForRep(8), Failure::NoNameConnect);
        QVERIFY(!isDecisive(Failure::NoNameConnect, Step::Request));
    }

    // 5. SOCKS5 bytes
    void socksWire()
    {
        QCOMPARE(wire::socksGreeting(false), QByteArray("\x05\x01\x00", 3));
        QCOMPARE(wire::socksGreeting(true), QByteArray("\x05\x02\x00\x02", 4));
        Failure f      = Failure::None;
        bool    unused = false;
        QCOMPARE(wire::methodReply(QByteArray("\x05", 1), false, &f, &unused), wire::MethodVerdict::NeedMore);
        QCOMPARE(wire::methodReply(QByteArray("\x05\x00", 2), true, &f, &unused), wire::MethodVerdict::Proceed);
        QVERIFY(unused); // Throne answers 00 to a login offer
        QCOMPARE(wire::methodReply(QByteArray("\x05\x00", 2), false, &f, &unused), wire::MethodVerdict::Proceed);
        QVERIFY(!unused);
        QCOMPARE(wire::methodReply(QByteArray("\x05\x02", 2), true, &f, &unused), wire::MethodVerdict::SendLogin);
        QCOMPARE(wire::methodReply(QByteArray("\x05\x02", 2), false, &f, &unused), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::NeedsLogin);
        QCOMPARE(wire::methodReply(QByteArray("\x05\xff", 2), false, &f, &unused), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::NeedsLogin);
        QCOMPARE(wire::methodReply(QByteArray("\x05\xff", 2), true, &f, &unused), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::LoginMethod);
        QCOMPARE(wire::methodReply(QByteArray("\x04\x00", 2), false, &f, &unused), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::NotSocks5);
        QCOMPARE(wire::methodReply(QByteArray("\x05\x01", 2), false, &f, &unused), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::NotSocks5);

        char      message[600];
        const int n = wire::socksLoginMessage("user", 4, "pw", 2, message, sizeof(message));
        QCOMPARE(QByteArray(message, n), QByteArray("\x01\x04user\x02pw", 9));
        const QByteArray max(255, 'x');
        QCOMPARE(wire::socksLoginMessage(max.constData(), 255, max.constData(), 255, message, sizeof(message)), 3 + 510);
        QCOMPARE(wire::socksLoginMessage(max.constData(), 256, "p", 1, message, sizeof(message)), -1);
        QCOMPARE(wire::socksLoginMessage("u", 1, max.constData(), 256, message, sizeof(message)), -1);
        QCOMPARE(wire::loginReply(QByteArray("\x01\x00", 2), &f), wire::MethodVerdict::Proceed);
        QCOMPARE(wire::loginReply(QByteArray("\x01\x01", 2), &f), wire::MethodVerdict::Fail);
        QCOMPARE(f, Failure::LoginRefused);

        // CONNECT: always ATYP 3 with the name, port 443
        QCOMPARE(wire::socksConnectRequest("www.youtube.com"), QByteArray("\x05\x01\x00\x03\x0fwww.youtube.com\x01\xbb", 22));
        QVERIFY(wire::socksConnectRequest(QByteArray(256, 'a')).isEmpty());

        // replies with BND of each ATYP, read to their end; early bytes after them are the site's
        const QByteArray v4 = QByteArray("\x05\x00\x00\x01\x7f\x00\x00\x01\x01\xbb", 10);
        wire::SocksReply r  = wire::socksReply(v4 + "SITE");
        QVERIFY(r.complete);
        QCOMPARE(r.consumed, 10);
        QCOMPARE(r.failure, Failure::None);
        QVERIFY(!wire::socksReply(v4.left(9)).complete);
        const QByteArray v6 = QByteArray("\x05\x00\x00\x04", 4) + QByteArray(16, '\0') + QByteArray("\x01\xbb", 2);
        QCOMPARE(wire::socksReply(v6).consumed, 22);
        const QByteArray dom = QByteArray("\x05\x00\x00\x03\x09localhost\x01\xbb", 16);
        QCOMPARE(wire::socksReply(dom).consumed, 16);
        QVERIFY(!wire::socksReply(dom.left(15)).complete);
        r = wire::socksReply(QByteArray("\x05\x00\x00\x09", 4));
        QVERIFY(r.complete);
        QCOMPARE(r.failure, Failure::NotSocks5); // an unknown ATYP
        r = wire::socksReply(QByteArray("\x04\x5a\x00\x00", 4));
        QCOMPARE(r.failure, Failure::NotSocks5);
        struct {
            int     rep;
            Failure failure;
        } const reps[] = {{1, Failure::CantReachSite}, {2, Failure::NotAllowed},    {3, Failure::CantReachSite},
                          {4, Failure::CantReachSite}, {5, Failure::SiteRefused},   {6, Failure::CantReachSite},
                          {7, Failure::NoNameConnect}, {8, Failure::NoNameConnect}, {9, Failure::CantReachSite}};
        for (const auto& row : reps) {
            QByteArray reply = v4;
            reply[1]         = static_cast<char>(row.rep);
            QCOMPARE(wire::socksReply(reply).failure, row.failure);
            QCOMPARE(wire::socksReply(reply).rep, row.rep);
        }
    }

    // 6. HTTP CONNECT bytes and the challenge-first sequence (S8)
    void httpConnectWire()
    {
        char       out[2048];
        int        n       = wire::httpConnectRequest("www.youtube.com", "u", 1, "p", 1, false, out, sizeof(out));
        QByteArray request = QByteArray(out, n);
        QCOMPARE(request, QByteArray("CONNECT www.youtube.com:443 HTTP/1.1\r\nHost: www.youtube.com:443\r\n\r\n"));
        QVERIFY(!request.contains("User-Agent"));
        n       = wire::httpConnectRequest("www.youtube.com", "user", 4, "pass", 4, true, out, sizeof(out));
        request = QByteArray(out, n);
        QVERIFY(request.contains("Proxy-Authorization: Basic " + QByteArray("user:pass").toBase64() + "\r\n"));
        QVERIFY(request.endsWith("\r\n\r\n"));

        const auto head = [](const char* text) { return wire::responseHead(QByteArray(text)); };
        // not on this PC, a login set, a proxy that never challenges: success, no credentials ever produced
        {
            wire::ConnectFlow flow(false, true);
            QVERIFY(!flow.sendLogin());
            Failure f      = Failure::None;
            bool    unused = false;
            QCOMPARE(flow.onResponse(head("HTTP/1.1 200 Connection established\r\n\r\n"), &f, &unused), wire::ConnectFlow::Next::Success);
            QVERIFY(unused);
        }
        // 407 with Basic: one resend with Basic (same connection when the body length is known), a second 407: refused
        {
            wire::ConnectFlow flow(false, true);
            Failure           f      = Failure::None;
            bool              unused = false;
            QCOMPARE(flow.onResponse(head("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"x\"\r\nContent-Length: 5\r\n\r\n"),
                                     &f, &unused),
                     wire::ConnectFlow::Next::ResendSameConnection);
            QVERIFY(flow.sendLogin());
            QCOMPARE(flow.onResponse(head("HTTP/1.1 407 Nope\r\nProxy-Authenticate: Basic realm=\"x\"\r\nContent-Length: 0\r\n\r\n"), &f, &unused),
                     wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::LoginRefused);
        }
        // a 407 that closes, has no length, or is chunked: the resend on a new connection
        // (review fix: also HTTP/1.0 without keep-alive, and Proxy-Connection: close)
        for (const char* closing : {"HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
                                    "HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\n\r\n",
                                    "HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nTransfer-Encoding: chunked\r\n\r\n",
                                    "HTTP/1.0 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nContent-Length: 0\r\n\r\n",
                                    "HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nProxy-Connection: close\r\nContent-Length: 0\r\n\r\n"}) {
            wire::ConnectFlow flow(false, true);
            Failure           f      = Failure::None;
            bool              unused = false;
            QCOMPARE(flow.onResponse(head(closing), &f, &unused), wire::ConnectFlow::Next::ResendNewConnection);
        }
        // HTTP/1.0 that says keep-alive keeps the connection
        for (const char* kept : {"HTTP/1.0 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nProxy-Connection: keep-alive\r\nContent-Length: 0\r\n\r\n",
                                 "HTTP/1.0 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nConnection: Keep-Alive\r\nContent-Length: 3\r\n\r\n"}) {
            wire::ConnectFlow flow(false, true);
            Failure           f      = Failure::None;
            bool              unused = false;
            QCOMPARE(flow.onResponse(head(kept), &f, &unused), wire::ConnectFlow::Next::ResendSameConnection);
        }
        QVERIFY(head("HTTP/1.0 200 OK\r\n\r\n").http10);
        QVERIFY(!head("HTTP/1.1 200 OK\r\n\r\n").http10);
        QVERIFY(!head("HTTP/1.0 407 x\r\nContent-Length: 0\r\n\r\n").persistent());
        QVERIFY(head("HTTP/1.1 407 x\r\nContent-Length: 0\r\n\r\n").persistent());
        // on this PC: pre-emptive Basic; a 407 then is a refusal
        {
            wire::ConnectFlow flow(true, true);
            QVERIFY(flow.sendLogin());
            Failure f      = Failure::None;
            bool    unused = false;
            QCOMPARE(flow.onResponse(head("HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\nContent-Length: 0\r\n\r\n"), &f, &unused),
                     wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::LoginRefused);
        }
        // no login set: 407 asks for one; NTLM only: an unsupported method
        {
            wire::ConnectFlow flow(true, false);
            QVERIFY(!flow.sendLogin());
            Failure f      = Failure::None;
            bool    unused = false;
            QCOMPARE(flow.onResponse(head("HTTP/1.1 407 x\r\nProxy-Authenticate: Basic realm=\"x\"\r\n\r\n"), &f, &unused), wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::NeedsLogin);
            wire::ConnectFlow ntlm(false, true);
            QCOMPARE(ntlm.onResponse(head("HTTP/1.1 407 x\r\nProxy-Authenticate: NTLM\r\nProxy-Authenticate: Negotiate\r\n\r\n"), &f, &unused),
                     wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::LoginMethod);
        }
        // 200 with headers and early bytes; 403; 502/503/504; another status; not HTTP
        {
            const QByteArray         data = "HTTP/1.0 200 OK\r\nProxy-Agent: x\r\n\r\nEARLY";
            const wire::ResponseHead h    = wire::responseHead(data);
            QVERIFY(h.complete);
            QCOMPARE(data.mid(h.headBytes), QByteArray("EARLY"));
            wire::ConnectFlow flow(true, false);
            Failure           f      = Failure::None;
            bool              unused = false;
            QCOMPARE(flow.onResponse(h, &f, &unused), wire::ConnectFlow::Next::Success);
            QCOMPARE(flow.onResponse(head("HTTP/1.1 403 Forbidden\r\n\r\n"), &f, &unused), wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::NotAllowed);
            for (const char* down : {"HTTP/1.1 502 Bad Gateway\r\n\r\n", "HTTP/1.1 503 x\r\n\r\n", "HTTP/1.1 504 x\r\n\r\n"}) {
                QCOMPARE(flow.onResponse(head(down), &f, &unused), wire::ConnectFlow::Next::Fail);
                QCOMPARE(f, Failure::CantReachSite);
            }
            QCOMPARE(flow.onResponse(head("HTTP/1.1 418 Teapot\r\n\r\n"), &f, &unused), wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::NotAllowed);
        }
        {
            const wire::ResponseHead ssh = wire::responseHead("SSH-2.0-OpenSSH_9.6\r\n");
            QVERIFY(ssh.complete);
            QVERIFY(!ssh.http1);
            wire::ConnectFlow flow(true, false);
            Failure           f      = Failure::None;
            bool              unused = false;
            QCOMPARE(flow.onResponse(ssh, &f, &unused), wire::ConnectFlow::Next::Fail);
            QCOMPARE(f, Failure::NotHttp);
            QVERIFY(wire::responseHead(QByteArray("HTTP/1.1 200 OK\r\n") + QByteArray(9000, 'x')).tooLarge);
        }
    }

    // 7. the bridge's head parser and target policy (S11)
    void bridgeHeadParser()
    {
        const QByteArray  token = "TOKEN-0123456789-abcdefghijklmnopqrstuvwxy";
        const QByteArray  auth  = basicHeader("tsm-f42", token);
        const QStringList none;
        const auto        parse = [&](const QByteArray& head) { return wire::clientHead(head, token, none); };

        wire::ClientHead h = parse(connectHead("www.youtube.com:443", auth));
        QCOMPARE(h.verdict, wire::ClientHead::Verdict::Ok);
        QCOMPARE(h.host, QStringLiteral("www.youtube.com"));
        QCOMPARE(h.client, 'f');
        QCOMPARE(h.id, quint64(42));
        // upper case and one trailing dot: forwarded as the normalized name
        h = parse(connectHead("WWW.YOUTUBE.COM.:443", auth));
        QCOMPARE(h.verdict, wire::ClientHead::Verdict::Ok);
        QCOMPARE(h.host, QStringLiteral("www.youtube.com"));
        // early bytes after the head are kept for after the 200
        const QByteArray withEarly = connectHead("www.youtube.com:443", auth) + "\x16\x03\x01";
        h                          = parse(withEarly);
        QCOMPARE(withEarly.mid(h.headBytes), QByteArray("\x16\x03\x01"));
        // not complete yet
        QCOMPARE(parse("CONNECT www.youtube.com:443 HTTP/1.1\r\n").verdict, wire::ClientHead::Verdict::NeedMore);

        // methods: 405 (never a login prompt)
        QCOMPARE(parse("connect www.youtube.com:443 HTTP/1.1\r\n\r\n").verdict, wire::ClientHead::Verdict::MethodNotAllowed);
        QCOMPARE(parse("GET http://www.youtube.com/ HTTP/1.1\r\nHost: www.youtube.com\r\n\r\n").verdict, wire::ClientHead::Verdict::MethodNotAllowed);
        QCOMPARE(parse("POST / HTTP/1.1\r\n\r\n").verdict, wire::ClientHead::Verdict::MethodNotAllowed);
        // malformed: 400
        QCOMPARE(parse("CONNECT  www.youtube.com:443 HTTP/1.1\r\n\r\n").verdict, wire::ClientHead::Verdict::BadRequest);
        QCOMPARE(parse("garbage\r\n\r\n").verdict, wire::ClientHead::Verdict::BadRequest);
        QCOMPARE(parse("CONNECT www.youtube.com:443 HTTP/2\r\n\r\n").verdict, wire::ClientHead::Verdict::BadRequest);
        QCOMPARE(parse(connectHead("www.youtube.com:443", auth + auth)).verdict, wire::ClientHead::Verdict::BadRequest); // two logins
        QCOMPARE(parse(connectHead("www.youtube.com:443", auth + " folded\r\n")).verdict, wire::ClientHead::Verdict::BadRequest); // obs-fold
        QCOMPARE(parse(connectHead("www.youtube.com:443", auth + "Content-Length: 0\r\n")).verdict, wire::ClientHead::Verdict::Ok);
        QCOMPARE(parse(connectHead("www.youtube.com:443", auth + "Content-Length: 5\r\n")).verdict, wire::ClientHead::Verdict::BadRequest);
        QCOMPARE(parse(connectHead("www.youtube.com:443", auth + "Transfer-Encoding: chunked\r\n")).verdict, wire::ClientHead::Verdict::BadRequest);
        QCOMPARE(parse(QByteArray("CONNECT www.youtube.com:443 HTTP/1.1\r\n") + QByteArray(9000, 'x')).verdict, wire::ClientHead::Verdict::BadRequest);
        // the login: missing, wrong token, bad base64, a wrong user form: 407
        QCOMPARE(parse(connectHead("www.youtube.com:443", "")).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader("tsm-f1", "wrong"))).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader("tsm-f1", token + "x"))).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", "Proxy-Authorization: Basic !!notbase64!!\r\n")).verdict,
                 wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader("tsm-x1", token))).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader("someone", token))).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader("tsm-f12a", token))).verdict, wire::ClientHead::Verdict::AuthRequired);
        QCOMPARE(parse(connectHead("www.youtube.com:443", "Proxy-Authorization: Digest x\r\n")).verdict, wire::ClientHead::Verdict::AuthRequired);
        for (const char* client : {"tsm-u", "tsm-p", "tsm-t1", "tsm-f18446744073709551615"})
            QCOMPARE(parse(connectHead("www.youtube.com:443", basicHeader(client, token))).verdict, wire::ClientHead::Verdict::Ok);
    }

    void bridgeTargetPolicy()
    {
        const QByteArray  token = "tok";
        const QByteArray  auth  = basicHeader("tsm-f1", token);
        const QStringList suffixes{QStringLiteral("corp.example.com")};
        const QString     longName  = QString(250, QLatin1Char('a')) + QStringLiteral(".com");
        const QByteArray  refused[] = {"127.0.0.1:443",
                                      "[::1]:443",
                                      "2130706433:443",
                                      "0x7f.1:443",
                                      "www.youtube.com:80",
                                      "www.youtube.com:0443",
                                      "www.youtube.com:443 ",
                                      "www.youtube.com",
                                      "localhost:443",
                                      "a.localhost:443",
                                      "router.lan:443",
                                      "x.home.arpa:443",
                                      "x.internal:443",
                                      "x.alt:443",
                                      "nas.corp.example.com:443",
                                      "io.github.metihttp.tsmediachat:443",
                                      longName.toLatin1() + ":443",
                                      "a..b:443",
                                      "a.example.com#@127.0.0.1:443",
                                      "a.example.com/x:443",
                                      "a.example.com\\@127.0.0.1:443",
                                      "www%2eyoutube.com:443"};
        for (const QByteArray& authority : refused) {
            const wire::ClientHead h = wire::clientHead("CONNECT " + authority + " HTTP/1.1\r\n" + auth + "\r\n", token, suffixes);
            QVERIFY2(h.verdict == wire::ClientHead::Verdict::Forbidden || h.verdict == wire::ClientHead::Verdict::BadRequest, authority.constData());
            QVERIFY2(h.host.isEmpty(), authority.constData());
        }
        // allowed ones forward exactly the validated string
        for (const char* ok : {"www.youtube.com", "i.ytimg.com", "static.klipy.com", "a_b.example.com"}) {
            const wire::ClientHead h = wire::clientHead(connectHead(QByteArray(ok) + ":443", auth), token, suffixes);
            QCOMPARE(h.verdict, wire::ClientHead::Verdict::Ok);
            QCOMPARE(h.host, QString::fromLatin1(ok));
            QVERIFY(wire::targetAllowed(h.host, suffixes));
        }
        QVERIFY(!wire::targetAllowed(QStringLiteral("x.alt"), {}));
        QVERIFY(!wire::targetAllowed(QStringLiteral("io.github.metihttp.tsmediachat"), {}));
        // the error answers: no body, Connection: close, the realm only for 407, never the request
        const QByteArray e407 = wire::errorResponse(407, "tsmedia-0123456789abcdef");
        QVERIFY(e407.startsWith("HTTP/1.1 407 "));
        QVERIFY(e407.contains("Proxy-Authenticate: Basic realm=\"tsmedia-0123456789abcdef\"\r\n"));
        QVERIFY(e407.contains("Content-Length: 0\r\n"));
        QVERIFY(e407.contains("Connection: close\r\n"));
        QVERIFY(e407.endsWith("\r\n\r\n"));
        const QByteArray e405 = wire::errorResponse(405);
        QVERIFY(!e405.contains("Proxy-Authenticate"));
        QVERIFY(wire::errorResponse(502, QByteArray(), "cantReachSite").contains("X-TSMedia-Proxy: cantReachSite\r\n"));
    }

    // 8. route health (F11, F21)
    void routeHealth()
    {
        qint64 now = 1000000;
        Health health([&now]() { return now; });
        health.reset(5, true);
        QCOMPARE(health.state(), Health::State::Unknown);
        QVERIFY(health.admits(false));
        // a decisive failure: down at once; clicks still pass
        QVERIFY(health.report(5, Failure::Unreachable, Step::Connect, QStringLiteral("youtube.com")));
        QCOMPARE(health.state(), Health::State::Down);
        QVERIFY(!health.admits(false));
        QVERIFY(health.admits(true));
        // probation after 30 s on this PC; one automatic load at a time
        now += 29000;
        QVERIFY(!health.tick());
        now += 1000;
        QVERIFY(health.tick());
        QCOMPARE(health.state(), Health::State::Probation);
        QVERIFY(health.admits(false));
        QVERIFY(!health.admits(false));
        QVERIFY(health.admits(true));
        // the probe fails at the route: down again, the same delay (no backoff)
        QVERIFY(health.report(5, Failure::CantReachSite, Step::Relay, QStringLiteral("youtube.com")));
        QCOMPARE(health.state(), Health::State::Down);
        QCOMPARE(health.msUntilTick(), qint64(30000));
        now += 30000;
        health.tick();
        QCOMPARE(health.state(), Health::State::Probation);
        QVERIFY(health.admits(false));
        // a probe that never reports frees its slot after 30 s
        now += 29999;
        QVERIFY(!health.admits(false));
        QVERIFY(!health.tick());
        now += 1;
        QVERIFY(health.tick()); // review fix: a change, so the links paused meanwhile go back to the on-screen pass
        QCOMPARE(health.state(), Health::State::Probation);
        QVERIFY(!health.tick());
        QCOMPARE(health.msUntilTick(), qint64(-1));
        QVERIFY(health.admits(false));
        // success: working
        QVERIFY(health.reportOk(5));
        QCOMPARE(health.state(), Health::State::Working);
        QVERIFY(health.admits(false));
        // reportOk while down ends it at once
        health.report(5, Failure::LoginRefused, Step::Login, QStringLiteral("youtube.com"));
        QCOMPARE(health.state(), Health::State::Down);
        QVERIFY(health.reportOk(5));
        QCOMPARE(health.state(), Health::State::Working);

        // ambiguous failures: 3 on 2 sites in 30 s and no success in them
        health.reset(6, false);
        now += 100000;
        health.report(6, Failure::CantReachSite, Step::Relay, QStringLiteral("youtube.com"));
        now += 1000;
        health.report(6, Failure::CantReachSite, Step::Relay, QStringLiteral("klipy.com"));
        now += 1000;
        QVERIFY(health.report(6, Failure::CantReachSite, Step::Relay, QStringLiteral("youtube.com")));
        QCOMPARE(health.state(), Health::State::Down);
        // elsewhere: probation after 60 s
        now += 59000;
        health.tick();
        QCOMPARE(health.state(), Health::State::Down);
        now += 1000;
        health.tick();
        QCOMPARE(health.state(), Health::State::Probation);

        // the same with a success 10 s earlier: not down
        health.reset(7, true);
        health.reportOk(7);
        now += 10000;
        health.report(7, Failure::CantReachSite, Step::Relay, QStringLiteral("a.com"));
        health.report(7, Failure::NotAllowed, Step::Request, QStringLiteral("b.com"));
        health.report(7, Failure::Slow, Step::Greeting, QStringLiteral("c.com"));
        QCOMPARE(health.state(), Health::State::Working);
        // three on one site: that site's problem
        health.reset(8, true);
        for (int i = 0; i < 5; ++i)
            health.report(8, Failure::CantReachSite, Step::Relay, QStringLiteral("youtube.com"));
        QCOMPARE(health.state(), Health::State::Unknown);
        // two, then a 31 s gap, then one: not down
        health.reset(9, true);
        health.report(9, Failure::CantReachSite, Step::Relay, QStringLiteral("a.com"));
        health.report(9, Failure::CantReachSite, Step::Relay, QStringLiteral("b.com"));
        now += 31000;
        health.report(9, Failure::CantReachSite, Step::Relay, QStringLiteral("c.com"));
        QCOMPARE(health.state(), Health::State::Unknown);
        // another generation's reports are ignored; not route-level ones too
        QVERIFY(!health.report(8, Failure::Unreachable, Step::Connect, QStringLiteral("a.com")));
        QVERIFY(!health.report(9, Failure::Refused, Step::Head, QStringLiteral("a.com")));
        QVERIFY(!health.report(9, Failure::Internal, Step::None, QStringLiteral("a.com")));
        QCOMPARE(health.state(), Health::State::Unknown);
        // a new generation starts unknown
        health.report(9, Failure::NeedsLogin, Step::Greeting, QStringLiteral("a.com"));
        QCOMPARE(health.state(), Health::State::Down);
        health.reset(10, true);
        QCOMPARE(health.state(), Health::State::Unknown);
    }

    // 9. the snapshot before any publish (S1), and from two threads
    void snapshotDefaults()
    {
        init();
        std::shared_ptr<const Snapshot> s = snapshot();
        QVERIFY(s);
        QCOMPARE(s->mode, Mode::Unset);
        QCOMPARE(s->refused, Failure::NotReady);
        QCOMPARE(s->generation, quint64(0));
        QCOMPARE(lastProxyRouteChangeMs(), qint64(0));
        Record r;
        QVERIFY(!liveRecords()->outcome('f', 1, &r));

        std::atomic_bool stop{false};
        std::atomic_int  bad{0};
        QThread*         reader = QThread::create([&]() {
            while (!stop.load()) {
                const auto snap = snapshot();
                if (!snap || (snap->mode != Mode::Unset && snap->generation == 0))
                    ++bad;
            }
        });
        reader->start();
        for (quint64 g = 1; g <= 2000; ++g) {
            auto next        = std::make_shared<Snapshot>();
            next->mode       = (g % 2) ? Mode::Direct : Mode::System;
            next->generation = g;
            next->refused    = Failure::None;
            publish(next);
        }
        stop = true;
        reader->wait();
        delete reader;
        QCOMPARE(bad.load(), 0);
        QCOMPARE(snapshot()->generation, quint64(2000));
        init();
        QCOMPARE(snapshot()->mode, Mode::Unset);
    }

    // 10. the Windows proxy: setup-script entries by scheme (S2), and the manual strings
    void windowsProxyMapping()
    {
        using windowsproxy::Entry;
        const auto entry = [](bool proxy, Entry::Scheme scheme, const char* host, quint16 port) {
            Entry e;
            e.proxy  = proxy;
            e.scheme = scheme;
            e.host   = QString::fromLatin1(host);
            e.port   = port;
            return e;
        };
        const Entry httpA  = entry(true, Entry::Scheme::Http, "a.example.net", 3128);
        const Entry httpB  = entry(true, Entry::Scheme::Http, "127.0.0.1", 2080);
        const Entry socksB = entry(true, Entry::Scheme::Socks, "127.0.0.1", 2080);
        const Entry https  = entry(true, Entry::Scheme::Https, "p.example.net", 443);
        const Entry direct = entry(false, Entry::Scheme::Http, "", 0);

        auto m = windowsproxy::mapProxyResult({httpA});
        QCOMPARE(m.decision, windowsproxy::Decision::Proxy);
        QCOMPARE(m.proxies, QStringLiteral("a.example.net:3128"));
        m = windowsproxy::mapProxyResult({httpA, direct});
        QCOMPARE(m.decision, windowsproxy::Decision::Proxy); // the DIRECT fallback dropped
        QCOMPARE(m.proxies, QStringLiteral("a.example.net:3128"));
        m = windowsproxy::mapProxyResult({httpA, httpB});
        QCOMPARE(m.proxies, QStringLiteral("a.example.net:3128;127.0.0.1:2080"));
        m = windowsproxy::mapProxyResult({socksB});
        QCOMPARE(m.decision, windowsproxy::Decision::Refused);
        QCOMPARE(m.refused, Failure::SocksOnlyWindows);
        m = windowsproxy::mapProxyResult({socksB, direct});
        QCOMPARE(m.refused, Failure::SocksOnlyWindows);
        m = windowsproxy::mapProxyResult({https});
        QCOMPARE(m.refused, Failure::UnsupportedWindowsProxy);
        m = windowsproxy::mapProxyResult({});
        QCOMPARE(m.decision, windowsproxy::Decision::Refused);
        QCOMPARE(m.refused, Failure::UnsupportedWindowsProxy);
        m = windowsproxy::mapProxyResult({direct});
        QCOMPARE(m.decision, windowsproxy::Decision::Direct);
        for (const QVector<Entry>& mixed : {QVector<Entry>{httpA, socksB}, QVector<Entry>{socksB, httpA}}) {
            m = windowsproxy::mapProxyResult(mixed);
            QCOMPARE(m.decision, windowsproxy::Decision::Proxy);
            QCOMPARE(m.proxies, QStringLiteral("a.example.net:3128"));
        }

        const QString yt = QStringLiteral("www.youtube.com");
        auto          p  = linkpreview::net::pickProxy(QStringLiteral("socks=127.0.0.1:2080"), QString(), yt);
        QVERIFY(p.socksOnly);
        QCOMPARE(p.socksServer, QStringLiteral("127.0.0.1:2080"));
        QVERIFY(p.servers.isEmpty());
        p = linkpreview::net::pickProxy(QStringLiteral("socks5://127.0.0.1:1080"), QString(), yt);
        QVERIFY(p.socksOnly);
        // Review fix (design 3.3.2): for https a "socks=" entry comes before an "http=" one, as in WinINet and Chromium
        // (both would use SOCKS here, Chromium as SOCKS4 with local DNS): "http=X;socks=Y" is SOCKS-only, refused.
        p = linkpreview::net::pickProxy(QStringLiteral("http=127.0.0.1:10809;socks=127.0.0.1:10808"), QString(), yt);
        QVERIFY(p.socksOnly);
        QCOMPARE(p.socksServer, QStringLiteral("127.0.0.1:10808"));
        QVERIFY(p.servers.isEmpty());
        p = linkpreview::net::pickProxy(QStringLiteral("socks=127.0.0.1:10808;http=127.0.0.1:10809"), QString(), yt);
        QVERIFY(p.socksOnly && p.servers.isEmpty());
        // ... an "http=" entry alone still tunnels https; a plain or "https=" entry beats "socks=".
        p = linkpreview::net::pickProxy(QStringLiteral("http=127.0.0.1:10809"), QString(), yt);
        QVERIFY(!p.socksOnly);
        QCOMPARE(p.servers, QStringLiteral("127.0.0.1:10809"));
        p = linkpreview::net::pickProxy(QStringLiteral("https=127.0.0.1:10810;socks=127.0.0.1:10808"), QString(), yt);
        QVERIFY(!p.socksOnly);
        QCOMPARE(p.servers, QStringLiteral("127.0.0.1:10810"));
        p = linkpreview::net::pickProxy(QStringLiteral("127.0.0.1:10811;socks=127.0.0.1:10808"), QString(), yt);
        QVERIFY(!p.socksOnly);
        QCOMPARE(p.servers, QStringLiteral("127.0.0.1:10811"));
        // bypassed: direct, never "SOCKS only"
        p = linkpreview::net::pickProxy(QStringLiteral("socks=127.0.0.1:2080"), QStringLiteral("*.youtube.com"), yt);
        QVERIFY(!p.socksOnly);
        QVERIFY(p.servers.isEmpty());
        p = linkpreview::net::pickProxy(QString(), QString(), yt);
        QVERIFY(!p.socksOnly && p.servers.isEmpty());

        // the summary (status row, the player, the updater)
        windowsproxy::IeConfig ie;
        ie.ok    = true;
        ie.proxy = QStringLiteral("socks=127.0.0.1:2080");
        auto s   = windowsproxy::summarize(ie);
        QCOMPARE(s.kind, windowsproxy::Summary::Kind::SocksOnly);
        QCOMPARE(s.socksHost, QStringLiteral("127.0.0.1"));
        QCOMPARE(s.socksPort, quint16(2080));
        ie.proxy = QStringLiteral("127.0.0.1:10809");
        s        = windowsproxy::summarize(ie);
        QCOMPARE(s.kind, windowsproxy::Summary::Kind::HttpProxy);
        QCOMPARE(s.hostPort, QStringLiteral("127.0.0.1:10809"));
        QVERIFY(!s.autoDetect);
        ie.proxy = QStringLiteral("http=127.0.0.1:10809;socks=127.0.0.1:10808");
        s        = windowsproxy::summarize(ie);
        QCOMPARE(s.kind, windowsproxy::Summary::Kind::SocksOnly);
        QCOMPARE(s.hostPort, QStringLiteral("127.0.0.1:10808"));
        // Review fix: "Automatically detect settings" is on by default. Without a setup script the manual setting is what
        // applies wherever WPAD finds nothing (Chromium, WinHTTP and route() fall back to it): SOCKS-only stays SocksOnly
        // (the player and the updater refuse it), an HTTP proxy stays HttpProxy; detection alone is Script.
        ie.autoDetect = true;
        ie.proxy      = QStringLiteral("socks=127.0.0.1:10808");
        s             = windowsproxy::summarize(ie);
        QCOMPARE(s.kind, windowsproxy::Summary::Kind::SocksOnly);
        QCOMPARE(s.socksHost, QStringLiteral("127.0.0.1"));
        QCOMPARE(s.socksPort, quint16(10808));
        QVERIFY(s.autoDetect);
        ie.proxy = QStringLiteral("127.0.0.1:10809");
        s        = windowsproxy::summarize(ie);
        QCOMPARE(s.kind, windowsproxy::Summary::Kind::HttpProxy);
        QCOMPARE(s.hostPort, QStringLiteral("127.0.0.1:10809"));
        ie.proxy.clear();
        QCOMPARE(windowsproxy::summarize(ie).kind, windowsproxy::Summary::Kind::Script);
        // A setup script decides (the manual setting is only its fallback), with or without detection.
        ie.proxy         = QStringLiteral("socks=127.0.0.1:10808");
        ie.autoConfigUrl = QStringLiteral("http://wpad/wpad.dat");
        QCOMPARE(windowsproxy::summarize(ie).kind, windowsproxy::Summary::Kind::Script);
        ie.autoDetect = false;
        QCOMPARE(windowsproxy::summarize(ie).kind, windowsproxy::Summary::Kind::Script);
        QCOMPARE(windowsproxy::summarize(windowsproxy::IeConfig()).kind, windowsproxy::Summary::Kind::None);
        windowsproxy::setIeConfigProviderForTests([]() {
            windowsproxy::IeConfig c;
            c.ok    = true;
            c.proxy = QStringLiteral("socks=127.0.0.1:7891");
            return c;
        });
        QCOMPARE(windowsproxy::windowsProxySummary().kind, windowsproxy::Summary::Kind::SocksOnly);
    }

    // 11. local DNS answers on the direct route (S5, S6)
    void answerClassification()
    {
        using linkpreview::net::Answer;
        const auto cls = [](std::initializer_list<const char*> list) {
            QVector<Address> v;
            for (const char* a : list)
                v << parseAddress(QString::fromLatin1(a));
            return linkpreview::net::classifyAnswer(v);
        };
        QCOMPARE(cls({"93.184.216.34"}), Answer::Public);
        QCOMPARE(cls({"93.184.216.34", "2606:2800:220:1:248:1893:25c8:1946"}), Answer::Public);
        QCOMPARE(cls({"198.18.0.5"}), Answer::FakeIp);
        QCOMPARE(cls({"198.18.0.5", "2606:2800:220:1:248:1893:25c8:1946"}), Answer::FakeIp); // fakeIpPlusPublicV6
        QCOMPARE(cls({"198.19.255.1"}), Answer::FakeIp);
        QCOMPARE(cls({"198.18.0.5", "10.0.0.2"}), Answer::Blocked);
        QCOMPARE(cls({"fc00::5", "198.18.0.5"}), Answer::FakeIp);
        QCOMPARE(cls({"fc00:3fff::1"}), Answer::FakeIp);
        QCOMPARE(cls({"fc00:4000::1"}), Answer::Blocked); // outside fc00::/18
        QCOMPARE(cls({"10.10.34.35"}), Answer::Filtered);
        QCOMPARE(cls({"10.10.34.35", "10.10.34.36"}), Answer::Filtered);
        QCOMPARE(cls({"10.10.34.35", "93.184.216.34"}), Answer::Filtered);
        QCOMPARE(cls({"10.10.34.35", "192.168.1.10"}), Answer::Blocked);
        QCOMPARE(cls({"192.168.1.10"}), Answer::Blocked);
        QCOMPARE(cls({"127.0.0.1"}), Answer::Blocked);
        QCOMPARE(cls({"10.10.35.1"}), Answer::Blocked);
        QCOMPARE(linkpreview::net::classifyAnswer({}), Answer::Blocked);
        // the sinkhole set, one vector per entry (10.10.34.0/24 only)
        QVERIFY(linkpreview::net::isSinkholeAddress(parseAddress(QStringLiteral("10.10.34.0"))));
        QVERIFY(linkpreview::net::isSinkholeAddress(parseAddress(QStringLiteral("10.10.34.255"))));
        QVERIFY(!linkpreview::net::isSinkholeAddress(parseAddress(QStringLiteral("10.10.33.255"))));
        QVERIFY(!linkpreview::net::isSinkholeAddress(parseAddress(QStringLiteral("10.10.35.0"))));
        QVERIFY(!linkpreview::net::isSinkholeAddress(parseAddress(QStringLiteral("::ffff:10.10.34.35"))));
        QVERIFY(linkpreview::net::isFakeIpAddress(parseAddress(QStringLiteral("198.18.0.0"))));
        QVERIFY(!linkpreview::net::isFakeIpAddress(parseAddress(QStringLiteral("198.20.0.0"))));
        // the alt TLD is never loaded (RFC 9476)
        QVERIFY(linkpreview::publicHostName(QUrl(QStringLiteral("https://x.alt/"))).isEmpty());
        QVERIFY(!linkpreview::net::isAllowedUrl(QUrl(QStringLiteral("https://shop.example.alt/a.png"))));
    }

    // 12. Paste from clipboard (design 1.3.6)
    void linkParse()
    {
        ProxyLink l = parseProxyLink(QStringLiteral("socks5://127.0.0.1:10808"));
        QCOMPARE(l.result, ProxyLink::Result::Ok);
        QVERIFY(l.hasType);
        QCOMPARE(l.type, Type::Socks5);
        QCOMPARE(l.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(l.port, quint16(10808));
        QVERIFY(l.user.isEmpty());
        QVERIFY(!l.hasPassword);
        for (const char* socks : {"socks5h://127.0.0.1:2080/", "SOCKS://127.0.0.1:2080", " socks5://127.0.0.1:2080 "}) {
            l = parseProxyLink(QString::fromLatin1(socks));
            QCOMPARE(l.result, ProxyLink::Result::Ok);
            QCOMPARE(l.type, Type::Socks5);
            QCOMPARE(l.port, quint16(2080));
        }
        l = parseProxyLink(QStringLiteral("http://us%40er:p%3Ass@proxy.example.net:3128/"));
        QCOMPARE(l.result, ProxyLink::Result::Ok);
        QCOMPARE(l.type, Type::Http);
        QCOMPARE(l.user, QStringLiteral("us@er"));
        QVERIFY(l.hasPassword);
        QCOMPARE(l.password, QStringLiteral("p:ss"));
        QCOMPARE(l.host, QStringLiteral("proxy.example.net"));
        l = parseProxyLink(QStringLiteral("socks5://user@[::1]:1080"));
        QCOMPARE(l.result, ProxyLink::Result::Ok);
        QCOMPARE(l.user, QStringLiteral("user"));
        QVERIFY(!l.hasPassword);
        QCOMPARE(l.host, QStringLiteral("::1"));
        // host:port with exactly one colon; [v6]:port; the type unchanged
        l = parseProxyLink(QStringLiteral("127.0.0.1:2080"));
        QCOMPARE(l.result, ProxyLink::Result::Ok);
        QVERIFY(!l.hasType);
        QCOMPARE(l.port, quint16(2080));
        QVERIFY(l.user.isEmpty()); // pasteClearsUser (S7)
        l = parseProxyLink(QStringLiteral("[fe80::1]:7897"));
        QCOMPARE(l.result, ProxyLink::Result::Ok);
        QCOMPARE(l.host, QStringLiteral("fe80::1"));
        // two or more colons without brackets: an IPv6 host with no port (F19)
        for (const char* v6 : {"::1", "fe80::1"}) {
            l = parseProxyLink(QString::fromLatin1(v6));
            QCOMPARE(l.result, ProxyLink::Result::HostOnly);
            QCOMPARE(l.host, QString::fromLatin1(v6));
            QCOMPARE(l.port, quint16(0));
        }
        // refusals, each with its message
        struct {
            const char*       text;
            ProxyLink::Result result;
            const char*       message;
        } const rows[] = {
            {"", ProxyLink::Result::NoText, "The clipboard has no text."},
            {"vless://uuid@host:443?x=y#name", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"vmess://abc", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"ss://abc@host:8388", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"hysteria2://x@h:443", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"wg://x", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"https://sub.example.net/api/v1/client/subscribe?token=x", ProxyLink::Result::VpnLink, "This is a server link for a VPN app."},
            {"tg://socks?server=1.2.3.4&port=1080", ProxyLink::Result::TelegramSocks, "This is a Telegram proxy link."},
            {"https://t.me/socks?server=1.2.3.4&port=1080", ProxyLink::Result::TelegramSocks, "This is a Telegram proxy link."},
            {"t.me/socks?server=1.2.3.4&port=1080", ProxyLink::Result::TelegramSocks, "This is a Telegram proxy link."},
            {"tg://proxy?server=1.2.3.4&port=443&secret=x", ProxyLink::Result::TelegramMtproto, "This is a Telegram proxy (MTProto)."},
            {"t.me/proxy?server=1.2.3.4", ProxyLink::Result::TelegramMtproto, "This is a Telegram proxy (MTProto)."},
            {"https://proxy.example.net:8443", ProxyLink::Result::Https, "TS Media can't use proxies over https://."},
            {"socks4://127.0.0.1:1080", ProxyLink::Result::Socks4, "TS Media needs SOCKS5."},
            {"socks4a://127.0.0.1:1080", ProxyLink::Result::Socks4, "TS Media needs SOCKS5."},
            {"socks5://127.0.0.1", ProxyLink::Result::NoPort, "The link has no port."},
            {"socks5://127.0.0.1:99999", ProxyLink::Result::BadPort, "The link's port isn't a number from 1 to 65535."},
            {"127.0.0.1:0", ProxyLink::Result::BadPort, "The link's port isn't a number from 1 to 65535."},
            {"hello world", ProxyLink::Result::NotALink, "This isn't a proxy link."},
            {"ftp://127.0.0.1:21", ProxyLink::Result::NotALink, "This isn't a proxy link."},
        };
        for (const auto& row : rows) {
            l = parseProxyLink(QString::fromLatin1(row.text));
            QVERIFY2(l.result == row.result, row.text);
            QVERIFY2(linkMessage(l.result).startsWith(QString::fromLatin1(row.message)), row.text);
            QVERIFY2(l.host.isEmpty() || l.result == ProxyLink::Result::HostOnly, row.text);
        }
        QCOMPARE(parseProxyLink(QString(2049, QLatin1Char('a'))).result, ProxyLink::Result::NotALink);
        QVERIFY(linkMessage(ProxyLink::Result::Ok).isEmpty());
    }
};

TSMEDIA_REGISTER_TEST(TestProxyCore)
#include "tst_proxycore.moc"
