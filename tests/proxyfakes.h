#pragma once

// 2.6 chat proxy tests: in-test fake proxies on 127.0.0.1 (a SOCKS5 server and an HTTP CONNECT proxy, each with
// scripted behaviour and a log of what it saw), a small blocking client for the bridge, and a scripted link-preview
// transport for the Fetcher's route and proxy classification. Plain Winsock (the test target links ws2_32), blocking
// sockets with time-outs, one std::thread per connection; nothing leaves 127.0.0.1.

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "linkpreview/transport.h"

namespace proxyfakes {

// What happens after a fake proxy said yes to a CONNECT.
enum class After {
    Echo,              // the "site": every byte comes back
    CloseOnClientData, // V2Ray-style optimistic success: close (reset) as soon as the client sends a byte
    Hold,              // keep the connection, send nothing (an idle tunnel)
    Sink,              // read and drop, count the bytes
};

struct SocksScript {
    bool       notSocks        = false; // answer "HTTP/1.1 400" garbage to the greeting
    bool       silent          = false; // accept the TCP connection and never answer
    quint8     method          = 0x00;  // the method byte answered (00, 02, FF, or another)
    bool       loginOk         = true;  // RFC 1929 status 0
    int        rep             = 0;
    quint8     atyp            = 1;     // BND form: 1, 3, 4, or an unknown one
    QByteArray earlyAfterReply;         // "site" bytes sent right after the reply
    After      after           = After::Echo;
};

struct HttpScript {
    bool       challenge        = false; // 407 Basic until a login comes (refused when it isn't expectUser:expectPass)
    QByteArray expectUser, expectPass;
    bool       ntlmOnly         = false; // 407 with NTLM only
    bool       closeOnChallenge = false; // the 407 says Connection: close (and closes)
    QByteArray challengeAnswer;          // when set: this 407 (head and body) instead of the default one
    bool       closeAfterChallenge = false; // ... and the connection closes after it, whatever it said (gost, goproxy)
    int        status           = 200;   // the answer when not challenging
    QByteArray earlyAfter200;
    bool       garbage          = false; // "SSH-2.0-..."
    After      after            = After::Echo;
};

struct Seen {
    QByteArray greeting;  // SOCKS: the greeting; HTTP: the request head
    QByteArray loginUser; // SOCKS RFC 1929 / HTTP Basic, decoded (tests compare only their own test values)
    QByteArray loginPass;
    int        atyp = 0;  // SOCKS CONNECT's ATYP
    QByteArray target;    // the name asked for (ATYP 3) / the CONNECT authority
    bool       authHeader = false; // HTTP: a Proxy-Authorization line came
};

class FakeServer
{
  public:
    FakeServer();
    virtual ~FakeServer(); // the derived class calls stop() in its destructor
    quint16       port() const { return m_port; }
    void          stop();
    int           connections() const { return m_connections.load(); }
    qint64        sunk() const { return m_sunk.load(); }
    QVector<Seen> seen() const;

  protected:
    bool         start();
    virtual void serve(uintptr_t socket) = 0;
    void         record(const Seen& seen);
    void         afterYes(uintptr_t socket, After after); // the "site" side after a yes

    std::atomic_bool    m_stop{false};
    std::atomic_int     m_connections{0};
    std::atomic<qint64> m_sunk{0};

  private:
    void                     loop();
    uintptr_t                m_listener = ~uintptr_t(0);
    quint16                  m_port     = 0;
    std::thread              m_thread;
    std::mutex               m_threadsLock;
    std::vector<std::thread> m_threads;
    std::vector<uintptr_t>   m_sockets;
    mutable QMutex           m_seenLock;
    QVector<Seen>            m_seen;
};

class FakeSocks : public FakeServer
{
  public:
    explicit FakeSocks(const SocksScript& script = SocksScript());
    ~FakeSocks() override { stop(); }
    SocksScript script;

  protected:
    void serve(uintptr_t socket) override;
};

class FakeHttpProxy : public FakeServer
{
  public:
    explicit FakeHttpProxy(const HttpScript& script = HttpScript());
    ~FakeHttpProxy() override { stop(); }
    HttpScript script;

  protected:
    void serve(uintptr_t socket) override;
};

// A port that was free a moment ago (a closed port: connect refused).
quint16 closedPort();

// A blocking client of the bridge.
class Client
{
  public:
    explicit Client(quint16 port, int timeoutMs = 5000, const char* address = "127.0.0.1");
    ~Client();
    bool       ok() const { return m_socket != ~uintptr_t(0); }
    bool       send(const QByteArray& bytes);
    // Reads until "\r\n\r\n" (with whatever came after it in the same reads), the close, or the time-out.
    QByteArray readHead();
    // Reads exactly count bytes (or less on close / time-out).
    QByteArray readCount(int count);
    // Reads until the other side closes (or the time-out): the bytes, and *closed.
    QByteArray readToClose(bool* closed);
    bool       closedByPeer(int waitMs); // true when the bridge closed (recv 0 or a reset) within waitMs
    void       shutdownSend();
    void       close();

  private:
    uintptr_t m_socket = ~uintptr_t(0);
};

QByteArray connectRequest(const QByteArray& authority, const QByteArray& user, const QByteArray& token);

// A scripted link-preview transport for the Fetcher's proxy paths: what route() answers (Windows mode), the DNS, and
// per URL what open() and head() do. Counts everything.
struct ScriptedHop {
    linkpreview::http::Error openError            = linkpreview::http::Error::None;
    linkpreview::http::Error headError            = linkpreview::http::Error::None;
    linkpreview::http::Error readError            = linkpreview::http::Error::None; // at the first read
    int                      status               = 200;
    QByteArray               contentType          = "image/png";
    QByteArray               body;
    bool                     proxyConnectResponse = false;
    QString                  proxyHeader;
    QString                  peer;                 // empty: 93.184.216.34 direct, 127.0.0.1 through a proxy
    unsigned long            systemError          = 0;
    bool                     connected            = true;
    unsigned long            secureFlags          = 0;
    int                      headDelayMs          = 0;
    bool                     challenge            = false; // cf-mitigated: challenge
    int                      firstReadDelayMs     = 0;
    std::function<void(const linkpreview::http::HopOptions&)> onOpen; // e.g. a record for the bridge hop
};

class ScriptedTransport : public linkpreview::http::Transport
{
  public:
    QMutex                                             mutex;
    QHash<QString, QVector<linkpreview::net::Address>> dns;  // missing: 93.184.216.34
    QHash<QString, ScriptedHop>                        hops; // by the fully encoded URL; missing: a 404
    linkpreview::http::Route                           routeAnswer; // route()'s answer (via Direct by default)
    std::atomic_int                                    resolves{0};
    std::atomic_int                                    opens{0};
    std::atomic_int                                    routes{0};
    QVector<linkpreview::http::HopOptions>             opened;

    ScriptedTransport() { routeAnswer.via = linkpreview::http::Route::Via::Direct; }
    linkpreview::http::Error resolve(const QString& host, QVector<linkpreview::net::Address>* out, const linkpreview::http::Cancel& cancel,
                                     quint64 deadline) override;
    linkpreview::http::Route route(const QUrl& url, const linkpreview::http::Cancel& cancel, quint64 deadline) override;
    std::unique_ptr<linkpreview::http::Exchange> open(const linkpreview::http::HopOptions& options, const linkpreview::http::Cancel& cancel,
                                                      linkpreview::http::Error* error) override;
};

QByteArray pngBytes(int width = 4, int height = 4);

} // namespace proxyfakes
