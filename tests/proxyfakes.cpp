#include "proxyfakes.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <QBuffer>
#include <QImage>
#include <QList>
#include <QUrl>

#include <cstring>

namespace proxyfakes {

namespace {

SOCKET sock(uintptr_t s)
{
    return static_cast<SOCKET>(s);
}

void startWinsock()
{
    static bool started = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    Q_UNUSED(started);
}

void setTimeouts(SOCKET s, int ms)
{
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

bool sendAll(SOCKET s, const QByteArray& data)
{
    int done = 0;
    while (done < data.size()) {
        const int n = ::send(s, data.constData() + done, data.size() - done, 0);
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

bool recvExact(SOCKET s, int count, QByteArray* out)
{
    while (out->size() < count) {
        char      buffer[4096];
        const int n = ::recv(s, buffer, qMin<int>(sizeof(buffer), count - out->size()), 0);
        if (n <= 0)
            return false;
        out->append(buffer, n);
    }
    return true;
}

// The bytes up to and with "\r\n\r\n" (one byte at a time: nothing after the head is consumed).
QByteArray recvHead(SOCKET s)
{
    QByteArray head;
    while (!head.endsWith("\r\n\r\n") && head.size() < 16384) {
        char      c = 0;
        const int n = ::recv(s, &c, 1, 0);
        if (n <= 0)
            return {};
        head.append(c);
    }
    return head;
}

void resetOnClose(SOCKET s)
{
    LINGER linger{1, 0};
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
}

} // namespace

// ---- FakeServer -----------------------------------------------------------------------------------------------------

FakeServer::FakeServer()
{
    startWinsock();
}

FakeServer::~FakeServer() = default;

bool FakeServer::start()
{
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    sockaddr_in address{};
    address.sin_family           = AF_INET;
    address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    address.sin_port             = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(s, 64) != 0) {
        closesocket(s);
        return false;
    }
    int length = sizeof(address);
    getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
    m_port     = ntohs(address.sin_port);
    m_listener = static_cast<uintptr_t>(s);
    m_thread   = std::thread([this]() { loop(); });
    return true;
}

void FakeServer::loop()
{
    while (!m_stop.load()) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock(m_listener), &set);
        timeval tv{0, 50 * 1000};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0)
            continue;
        const SOCKET s = ::accept(sock(m_listener), nullptr, nullptr);
        if (s == INVALID_SOCKET)
            continue;
        ++m_connections;
        setTimeouts(s, 10000);
        std::lock_guard<std::mutex> lock(m_threadsLock);
        m_sockets.push_back(static_cast<uintptr_t>(s));
        m_threads.emplace_back([this, s]() {
            serve(static_cast<uintptr_t>(s));
            ::shutdown(s, SD_SEND);
        });
    }
}

void FakeServer::stop()
{
    if (m_stop.exchange(true))
        return;
    if (m_thread.joinable())
        m_thread.join();
    if (m_listener != ~uintptr_t(0))
        closesocket(sock(m_listener));
    m_listener = ~uintptr_t(0);
    std::vector<std::thread> threads;
    std::vector<uintptr_t>   sockets;
    {
        std::lock_guard<std::mutex> lock(m_threadsLock);
        threads.swap(m_threads);
        sockets.swap(m_sockets);
    }
    for (uintptr_t s : sockets)
        ::shutdown(sock(s), SD_BOTH); // wakes a serving thread in recv
    for (std::thread& t : threads)
        t.join();
    for (uintptr_t s : sockets)
        closesocket(sock(s));
}

QVector<Seen> FakeServer::seen() const
{
    QMutexLocker lock(&m_seenLock);
    return m_seen;
}

void FakeServer::record(const Seen& seen)
{
    QMutexLocker lock(&m_seenLock);
    m_seen.append(seen);
}

void FakeServer::afterYes(uintptr_t socket, After after)
{
    const SOCKET s = sock(socket);
    char         buffer[64 * 1024];
    switch (after) {
    case After::Echo:
        for (;;) {
            const int n = ::recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0 || m_stop.load())
                return;
            if (!sendAll(s, QByteArray::fromRawData(buffer, n)))
                return;
        }
    case After::CloseOnClientData: {
        const int n = ::recv(s, buffer, sizeof(buffer), 0);
        Q_UNUSED(n);
        resetOnClose(s); // the closesocket at the end resets
        return;
    }
    case After::Hold:
        while (!m_stop.load()) {
            const int n = ::recv(s, buffer, sizeof(buffer), 0);
            if (n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT))
                return;
        }
        return;
    case After::Sink:
        for (;;) {
            const int n = ::recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0)
                return;
            m_sunk += n;
        }
    }
}

// ---- FakeSocks ----------------------------------------------------------------------------------------------------------

FakeSocks::FakeSocks(const SocksScript& s)
    : script(s)
{
    start();
}

void FakeSocks::serve(uintptr_t socket)
{
    const SOCKET s = sock(socket);
    Seen         seen;
    bool         recorded = false;
    struct Recorder {
        FakeSocks* self;
        Seen*      seen;
        bool*      recorded;
        ~Recorder()
        {
            if (!*recorded)
                self->record(*seen);
        }
    } recorder{this, &seen, &recorded};
    if (script.silent) {
        afterYes(socket, After::Hold);
        return;
    }
    QByteArray greeting;
    if (!recvExact(s, 2, &greeting))
        return;
    if (!recvExact(s, 2 + static_cast<quint8>(greeting.at(1)), &greeting))
        return;
    seen.greeting = greeting;
    if (script.notSocks) {
        sendAll(s, "HTTP/1.1 400 Bad Request\r\n\r\n");
        return;
    }
    sendAll(s, QByteArray("\x05", 1) + static_cast<char>(script.method));
    if (script.method == 0x02) {
        QByteArray login;
        if (!recvExact(s, 2, &login))
            return;
        const int ulen = static_cast<quint8>(login.at(1));
        if (!recvExact(s, 2 + ulen + 1, &login))
            return;
        const int plen = static_cast<quint8>(login.at(2 + ulen));
        if (!recvExact(s, 3 + ulen + plen, &login))
            return;
        seen.loginUser = login.mid(2, ulen);
        seen.loginPass = login.mid(3 + ulen, plen);
        sendAll(s, QByteArray("\x01", 1) + (script.loginOk ? '\0' : '\x01'));
        if (!script.loginOk)
            return;
    } else if (script.method != 0x00) {
        return;
    }
    QByteArray request;
    if (!recvExact(s, 4, &request))
        return;
    seen.atyp = static_cast<quint8>(request.at(3));
    if (seen.atyp == 3) {
        if (!recvExact(s, 5, &request))
            return;
        const int len = static_cast<quint8>(request.at(4));
        if (!recvExact(s, 5 + len + 2, &request))
            return;
        seen.target = request.mid(5, len);
    } else if (seen.atyp == 1) {
        if (!recvExact(s, 4 + 4 + 2, &request))
            return;
    } else if (seen.atyp == 4) {
        if (!recvExact(s, 4 + 16 + 2, &request))
            return;
    } else {
        return;
    }
    record(seen); // what was asked, before the answer (the tunnel may stay open)
    recorded = true;
    QByteArray reply("\x05", 1);
    reply += static_cast<char>(script.rep);
    reply += '\0';
    reply += static_cast<char>(script.atyp);
    if (script.atyp == 1)
        reply += QByteArray("\x7f\x00\x00\x01\x30\x39", 6);
    else if (script.atyp == 4)
        reply += QByteArray(16, '\0') + QByteArray("\x30\x39", 2);
    else if (script.atyp == 3)
        reply += QByteArray("\x09localhost\x30\x39", 12);
    sendAll(s, reply);
    if (script.rep != 0)
        return;
    if (!script.earlyAfterReply.isEmpty())
        sendAll(s, script.earlyAfterReply);
    afterYes(socket, script.after);
}

// ---- FakeHttpProxy ------------------------------------------------------------------------------------------------------

FakeHttpProxy::FakeHttpProxy(const HttpScript& s)
    : script(s)
{
    start();
}

void FakeHttpProxy::serve(uintptr_t socket)
{
    const SOCKET s = sock(socket);
    for (int round = 0; round < 4; ++round) {
        const QByteArray head = recvHead(s);
        if (head.isEmpty())
            return;
        Seen seen;
        seen.greeting               = head;
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> parts = lines.value(0).trimmed().split(' ');
        seen.target                 = parts.value(1);
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.toLower().startsWith("proxy-authorization:")) {
                seen.authHeader       = true;
                const QByteArray b64  = line.mid(line.indexOf(':') + 1).trimmed().mid(6).trimmed();
                const QByteArray pair = QByteArray::fromBase64(b64);
                seen.loginUser        = pair.left(pair.indexOf(':'));
                seen.loginPass        = pair.mid(pair.indexOf(':') + 1);
            }
        }
        record(seen);
        if (script.garbage) {
            sendAll(s, "SSH-2.0-OpenSSH_9.6\r\n");
            return;
        }
        if (script.ntlmOnly) {
            sendAll(s, "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: NTLM\r\nProxy-Authenticate: Negotiate\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        if (script.challenge) {
            const bool good = seen.authHeader && seen.loginUser == script.expectUser && seen.loginPass == script.expectPass;
            if (!good) {
                if (script.closeOnChallenge) {
                    sendAll(s, "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"fake\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
                if (!script.challengeAnswer.isEmpty()) {
                    sendAll(s, script.challengeAnswer);
                    if (script.closeAfterChallenge)
                        return;
                    continue; // the same connection
                }
                sendAll(s, "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"fake\"\r\nContent-Length: 4\r\n\r\nnope");
                continue; // the same connection
            }
        }
        if (script.status != 200) {
            sendAll(s, "HTTP/1.1 " + QByteArray::number(script.status) + " Refused\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        sendAll(s, QByteArray("HTTP/1.1 200 Connection established\r\n\r\n") + script.earlyAfter200);
        afterYes(socket, script.after);
        return;
    }
}

quint16 closedPort()
{
    startWinsock();
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in  address{};
    address.sin_family           = AF_INET;
    address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int length = sizeof(address);
    getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
    const quint16 port = ntohs(address.sin_port);
    closesocket(s);
    return port;
}

// ---- Client ------------------------------------------------------------------------------------------------------------

Client::Client(quint16 port, int timeoutMs, const char* address)
{
    startWinsock();
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = htons(port);
    inet_pton(AF_INET, address, &to.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
        closesocket(s);
        return;
    }
    setTimeouts(s, timeoutMs);
    m_socket = static_cast<uintptr_t>(s);
}

Client::~Client()
{
    close();
}

bool Client::send(const QByteArray& bytes)
{
    return ok() && sendAll(sock(m_socket), bytes);
}

QByteArray Client::readHead()
{
    QByteArray data;
    while (ok() && !data.contains("\r\n\r\n")) {
        char      buffer[4096];
        const int n = ::recv(sock(m_socket), buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        data.append(buffer, n);
    }
    return data;
}

QByteArray Client::readCount(int count)
{
    QByteArray data;
    if (ok())
        recvExact(sock(m_socket), count, &data);
    return data;
}

QByteArray Client::readToClose(bool* closed)
{
    QByteArray data;
    *closed = false;
    while (ok()) {
        char      buffer[65536];
        const int n = ::recv(sock(m_socket), buffer, sizeof(buffer), 0);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT)) {
            *closed = true;
            break;
        }
        if (n < 0)
            break;
        data.append(buffer, n);
    }
    return data;
}

bool Client::closedByPeer(int waitMs)
{
    if (!ok())
        return true;
    setTimeouts(sock(m_socket), waitMs);
    char      c = 0;
    const int n = ::recv(sock(m_socket), &c, 1, 0);
    return n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT);
}

void Client::shutdownSend()
{
    if (ok())
        ::shutdown(sock(m_socket), SD_SEND);
}

void Client::close()
{
    if (ok())
        closesocket(sock(m_socket));
    m_socket = ~uintptr_t(0);
}

QByteArray connectRequest(const QByteArray& authority, const QByteArray& user, const QByteArray& token)
{
    QByteArray r = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
    if (!user.isEmpty())
        r += "Proxy-Authorization: Basic " + (user + ':' + token).toBase64() + "\r\n";
    return r + "\r\n";
}

// ---- ScriptedTransport --------------------------------------------------------------------------------------------------

namespace {

class ScriptedExchange : public linkpreview::http::Exchange
{
  public:
    ScriptedExchange(const ScriptedHop& hop, const linkpreview::net::Address& peer, const linkpreview::http::Cancel& cancel)
        : m_hop(hop)
        , m_peer(peer)
        , m_cancel(cancel)
    {
    }
    linkpreview::http::Error head(linkpreview::http::Head* out) override
    {
        if (m_hop.headDelayMs > 0 && m_cancel.wait(m_hop.headDelayMs))
            return linkpreview::http::Error::Canceled;
        if (m_hop.headError != linkpreview::http::Error::None)
            return m_hop.headError;
        out->status               = m_hop.status;
        out->contentType          = m_hop.contentType;
        out->contentLength        = m_hop.body.size();
        out->peer                 = m_peer;
        out->proxyConnectResponse = m_hop.proxyConnectResponse;
        out->proxyHeader          = m_hop.proxyHeader;
        out->challenge            = m_hop.challenge;
        return linkpreview::http::Error::None;
    }
    linkpreview::http::Error read(char* buffer, int capacity, int* got) override
    {
        *got = 0;
        if (m_first) {
            m_first = false;
            if (m_hop.firstReadDelayMs > 0 && m_cancel.wait(m_hop.firstReadDelayMs))
                return linkpreview::http::Error::Canceled;
            if (m_hop.readError != linkpreview::http::Error::None)
                return m_hop.readError;
        }
        const int n = qMin(capacity, m_hop.body.size() - m_pos);
        if (n > 0) {
            memcpy(buffer, m_hop.body.constData() + m_pos, static_cast<size_t>(n));
            m_pos += n;
            *got = n;
        }
        return linkpreview::http::Error::None;
    }
    unsigned long systemError() const override { return m_hop.systemError; }
    bool          connectedToServer() const override { return m_hop.connected; }
    unsigned long secureFailureFlags() const override { return m_hop.secureFlags; }

  private:
    ScriptedHop                      m_hop;
    linkpreview::net::Address        m_peer;
    const linkpreview::http::Cancel& m_cancel;
    int                              m_pos   = 0;
    bool                             m_first = true;
};

} // namespace

linkpreview::http::Error ScriptedTransport::resolve(const QString& host, QVector<linkpreview::net::Address>* out, const linkpreview::http::Cancel& cancel,
                                                    quint64 deadline)
{
    Q_UNUSED(deadline);
    if (cancel.isSet())
        return linkpreview::http::Error::Canceled;
    ++resolves;
    QMutexLocker lock(&mutex);
    *out = dns.value(host, {linkpreview::net::parseAddress(QStringLiteral("93.184.216.34"))});
    return linkpreview::http::Error::None;
}

linkpreview::http::Route ScriptedTransport::route(const QUrl& url, const linkpreview::http::Cancel& cancel, quint64 deadline)
{
    Q_UNUSED(url);
    Q_UNUSED(cancel);
    Q_UNUSED(deadline);
    ++routes;
    QMutexLocker lock(&mutex);
    return routeAnswer;
}

std::unique_ptr<linkpreview::http::Exchange> ScriptedTransport::open(const linkpreview::http::HopOptions& options, const linkpreview::http::Cancel& cancel,
                                                                     linkpreview::http::Error* error)
{
    *error = linkpreview::http::Error::None;
    ++opens;
    ScriptedHop hop;
    {
        QMutexLocker  lock(&mutex);
        const QString key = options.url.toString(QUrl::FullyEncoded);
        opened.append(options);
        if (hops.contains(key)) {
            hop = hops.value(key);
        } else {
            hop.status      = 404;
            hop.contentType = "text/plain";
            hop.body        = "missing";
        }
    }
    if (hop.onOpen)
        hop.onOpen(options);
    if (hop.openError != linkpreview::http::Error::None) {
        *error = hop.openError;
        return nullptr;
    }
    QString peer = hop.peer;
    if (peer.isEmpty())
        peer = options.route.viaProxy() ? QStringLiteral("127.0.0.1") : QStringLiteral("93.184.216.34");
    const linkpreview::net::Address address = linkpreview::net::parseAddress(peer);
    if (options.allowPeer && !options.allowPeer(address)) {
        *error = linkpreview::http::Error::Blocked;
        return nullptr;
    }
    return std::unique_ptr<linkpreview::http::Exchange>(new ScriptedExchange(hop, address, cancel));
}

QByteArray pngBytes(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray data;
    QBuffer    buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

} // namespace proxyfakes
