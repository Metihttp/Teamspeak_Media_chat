#include "upstream.h"

#include <cstring>

#include "wire.h"

namespace proxy {

namespace {

constexpr qint64 kAddressCacheMs = 60 * 1000;
constexpr int    kMaxReplyBytes  = 8 * 1024;

using linkpreview::net::Address;

bool isLoopback(const Address& a)
{
    if (a.family == Address::Family::IPv4)
        return (a.ipv4() >> 24) == 127;
    if (a.family == Address::Family::IPv6) {
        for (int i = 0; i < 15; ++i) {
            if (a.bytes[static_cast<size_t>(i)] != 0)
                return false;
        }
        return a.bytes[15] == 1;
    }
    return false;
}

bool isUnusable(const Address& a)
{
    if (a.family == Address::Family::IPv4) {
        const quint32 v = a.ipv4();
        return v == 0 || (v >> 28) == 0xE || v == 0xFFFFFFFFu;
    }
    if (a.family == Address::Family::IPv6) {
        bool zero = true;
        for (quint8 b : a.bytes)
            zero = zero && b == 0;
        return zero || a.bytes[0] == 0xff;
    }
    return true;
}

Address literalOf(const QString& host)
{
    if (host.isEmpty() || host.size() > 64)
        return {};
    for (const QChar ch : host) {
        const ushort u = ch.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F') || u == '.' || u == ':'))
            return {};
    }
    if (host.count(QLatin1Char(':')) == 1)
        return {}; // "host:port" is never a host here
    return linkpreview::net::parseAddress(host);
}

// Reads into *data until done(data) or the deadline. Returns the last I/O state (Ok when done).
template <typename Done>
ws::Conn::Io readUntil(ws::Conn& conn, QByteArray* data, quint64 deadline, const HANDLE* waits, int waitCount, Done done)
{
    char buffer[4096];
    while (!done(*data)) {
        if (data->size() > kMaxReplyBytes + 4096)
            return ws::Conn::Io::Ok; // the parser decides (too large)
        int            got = 0;
        const ws::Conn::Io io  = conn.recvSome(buffer, sizeof(buffer), &got, deadline, waits, waitCount);
        if (io != ws::Conn::Io::Ok)
            return io;
        data->append(buffer, got);
    }
    return ws::Conn::Io::Ok;
}

// Reads and drops exactly count bytes (a 407's body, at most ConnectFlow::kMaxChallengeBody), never a byte more: review
// fix, readUntil's cap stopped short of a body over 12 KB and the next head was read from the rest of it.
ws::Conn::Io drainBytes(ws::Conn& conn, qint64 count, quint64 deadline, const HANDLE* waits, int waitCount)
{
    char buffer[4096];
    while (count > 0) {
        int                got = 0;
        const ws::Conn::Io io  = conn.recvSome(buffer, static_cast<int>(qMin<qint64>(count, sizeof(buffer))), &got, deadline, waits, waitCount);
        if (io != ws::Conn::Io::Ok)
            return io;
        count -= got;
    }
    return ws::Conn::Io::Ok;
}

constexpr quint64 kMinConnectSliceMs = 1500;

} // namespace

quint64 connectSliceMs(quint64 leftMs, int addressesLeft)
{
    if (addressesLeft <= 1)
        return leftMs;
    return qMin(leftMs, qMax(kMinConnectSliceMs, leftMs / static_cast<quint64>(addressesLeft)));
}

// ---- UpstreamState --------------------------------------------------------------------------------------------------

UpstreamState::UpstreamState(UpstreamSpec spec)
    : m_spec(std::move(spec))
    , m_epoch(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (m_spec.opened) {
        // An already opened password (the Test's typed one, or Control's check): under CryptProtectMemory from now on.
        m_opened     = true;
        m_openResult = m_secret.set(*m_spec.opened) ? protect::OpenResult::Ok : protect::OpenResult::Transient;
        if (m_openResult != protect::OpenResult::Ok)
            m_opened = false;
        m_spec.opened.reset(); // wiped
    }
}

UpstreamState::~UpstreamState()
{
    m_secret.clear();
    if (m_epoch)
        CloseHandle(m_epoch);
}

protect::OpenResult UpstreamState::password(protect::SecretBytes* out)
{
    out->reset();
    AcquireSRWLockExclusive(&m_lock);
    if (!m_opened) {
        protect::SecretBytes plain;
        const protect::OpenResult result = protect::open(m_spec.passwordBlob, m_spec.endpoint(), &plain);
        if (result == protect::OpenResult::Ok) {
            if (m_secret.set(plain)) {
                m_opened     = true;
                m_openResult = protect::OpenResult::Ok;
            } else {
                ReleaseSRWLockExclusive(&m_lock);
                return protect::OpenResult::Transient;
            }
        } else if (result == protect::OpenResult::Unreadable) {
            m_opened     = true; // it stays unreadable for this generation
            m_openResult = protect::OpenResult::Unreadable;
        } else {
            ReleaseSRWLockExclusive(&m_lock);
            return protect::OpenResult::Transient; // tried again at the next tunnel
        }
    }
    protect::OpenResult result = m_openResult;
    if (result == protect::OpenResult::Ok && !m_secret.reveal(out))
        result = protect::OpenResult::Transient;
    ReleaseSRWLockExclusive(&m_lock);
    return result;
}

bool UpstreamState::cachedAddresses(QVector<Address>* out)
{
    AcquireSRWLockShared(&m_lock);
    const bool fresh = !m_addresses.isEmpty() && ws::nowMs() < m_addressesUntil;
    if (fresh)
        *out = m_addresses;
    ReleaseSRWLockShared(&m_lock);
    return fresh;
}

void UpstreamState::cacheAddresses(const QVector<Address>& addresses)
{
    AcquireSRWLockExclusive(&m_lock);
    m_addresses      = addresses;
    m_addressesUntil = ws::nowMs() + kAddressCacheMs;
    ReleaseSRWLockExclusive(&m_lock);
}

// ---- the handshake ----------------------------------------------------------------------------------------------------

UpstreamResult openUpstream(UpstreamState& state, const QByteArray& name, ws::Conn& conn, HANDLE stop, quint16 bridgePort, quint64 headAt,
                            const UpstreamOptions& options, const std::function<void(Step)>& onStep)
{
    UpstreamResult      r;
    const UpstreamSpec& spec     = state.spec();
    HANDLE              waits[2] = {stop, state.epoch()};
    const quint64       overall  = headAt + static_cast<quint64>(qMax(1000, options.totalMs));
    Step                step     = Step::None;
    const auto          enter    = [&](Step s) {
        step = s;
        if (onStep)
            onStep(s);
    };
    const auto fail = [&](Failure failure, int code = 0) {
        r.failure = failure;
        r.step    = step;
        r.code    = code;
        conn.close();
        return r;
    };
    // An I/O state that ended a wait: the step's failure for a time-out, Off when the bridge stops or the generation
    // changed (not route-level: such a result is dropped).
    const auto ioFailure = [&](ws::Conn::Io io, Failure eofFailure) {
        switch (io) {
        case ws::Conn::Io::Timeout:
            return Failure::Slow;
        case ws::Conn::Io::Stopped:
            return Failure::Off;
        default:
            return eofFailure;
        }
    };

    // The password first: an unreadable one never lets anything leave the PC.
    protect::SecretBytes password;
    if (state.haveLogin()) {
        enter(Step::Login);
        const protect::OpenResult opened = state.password(&password);
        if (opened == protect::OpenResult::Unreadable)
            return fail(Failure::PasswordUnreadable);
        if (opened == protect::OpenResult::Transient)
            return fail(Failure::Internal);
    }

    // The proxy's address.
    enter(Step::Resolve);
    QVector<Address> addresses;
    const Address    literal = literalOf(spec.host);
    if (!literal.isNull()) {
        addresses << literal;
    } else if (!state.cachedAddresses(&addresses)) {
        QVector<Address> found;
        const int        timeout = qMax(1, qMin(options.lookupMs, ws::msUntil(overall)));
        const int        rc      = options.resolver ? options.resolver(spec.host, &found, waits, 2, timeout)
                                                    : ws::lookup(spec.host, &found, waits, 2, timeout);
        if (rc == 2) {
            const bool stopped = WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || WaitForSingleObject(state.epoch(), 0) == WAIT_OBJECT_0;
            return fail(stopped ? Failure::Off : Failure::Slow);
        }
        if (rc != 0 || found.isEmpty())
            return fail(Failure::NameNotFound);
        bool allSinkhole = true;
        for (const Address& a : qAsConst(found))
            allSinkhole = allSinkhole && linkpreview::net::isSinkholeAddress(a);
        if (allSinkhole)
            return fail(Failure::ProxyNameFiltered); // before any connect (S8)
        for (const Address& a : qAsConst(found)) {
            if (!linkpreview::net::isSinkholeAddress(a))
                addresses << a;
        }
        state.cacheAddresses(addresses);
    }
    QVector<Address> usable;
    for (const Address& a : qAsConst(addresses)) {
        if (isUnusable(a))
            continue;
        if (isLoopback(a) && spec.port == bridgePort)
            continue; // a loop back into the bridge (S17)
        usable << a;
    }
    if (usable.isEmpty())
        return fail(Failure::Unreachable);

    // TCP.
    enter(Step::Connect);
    const quint64 connectBy = qMin(ws::nowMs() + static_cast<quint64>(options.connectMs), overall);
    Address       connected;
    for (int i = 0; i < usable.size(); ++i) {
        const quint64 now = ws::nowMs();
        if (now >= connectBy)
            break; // 6 s for all of them
        // Each address its share of what is left (review fix): one that drops the packets can't starve the next.
        const quint64      until = now + connectSliceMs(connectBy - now, usable.size() - i);
        const ws::Conn::Io io    = conn.connectTo(usable.at(i), spec.port, until, waits, 2);
        if (io == ws::Conn::Io::Ok) {
            connected = usable.at(i);
            break;
        }
        if (io == ws::Conn::Io::Stopped)
            return fail(Failure::Off);
        // refused, unreachable, or its share is up: the next address
    }
    if (connected.isNull())
        return fail(Failure::Unreachable);
    const bool    loopback  = !options.treatAsRemote && isLoopback(connected);
    const quint64 handshake = qMin(ws::nowMs() + static_cast<quint64>(options.handshakeMs), overall);
    const QByteArray user   = spec.user.toUtf8();

    if (spec.type == Type::Socks5) {
        enter(Step::Greeting);
        const QByteArray greeting = wire::socksGreeting(state.haveLogin());
        ws::Conn::Io     io       = conn.sendAll(greeting.constData(), greeting.size(), handshake, waits, 2);
        if (io != ws::Conn::Io::Ok)
            return fail(ioFailure(io, Failure::NotSocks5));
        QByteArray data;
        io = readUntil(conn, &data, handshake, waits, 2, [](const QByteArray& d) { return d.size() >= 2 || (!d.isEmpty() && d.at(0) != 0x05); });
        if (io != ws::Conn::Io::Ok)
            return fail(ioFailure(io, Failure::NotSocks5));
        Failure                   failure     = Failure::None;
        bool                      loginUnused = false;
        const wire::MethodVerdict verdict     = wire::methodReply(data.left(2), state.haveLogin(), &failure, &loginUnused);
        if (verdict == wire::MethodVerdict::Fail || verdict == wire::MethodVerdict::NeedMore)
            return fail(failure == Failure::None ? Failure::NotSocks5 : failure);
        r.loginUnused = loginUnused;
        data          = data.mid(2);
        if (verdict == wire::MethodVerdict::SendLogin) {
            enter(Step::Login);
            char      message[3 + 2 * wire::kMaxLoginBytes];
            const int size = wire::socksLoginMessage(user.constData(), user.size(), password.data(), password.size(), message, sizeof(message));
            password.reset();
            if (size < 0) {
                SecureZeroMemory(message, sizeof(message));
                return fail(Failure::Internal);
            }
            io = conn.sendAll(message, size, handshake, waits, 2);
            SecureZeroMemory(message, sizeof(message));
            if (io != ws::Conn::Io::Ok)
                return fail(ioFailure(io, Failure::LoginRefused));
            io = readUntil(conn, &data, handshake, waits, 2, [](const QByteArray& d) { return d.size() >= 2; });
            if (io != ws::Conn::Io::Ok)
                return fail(ioFailure(io, Failure::LoginRefused));
            if (wire::loginReply(data.left(2), &failure) != wire::MethodVerdict::Proceed)
                return fail(Failure::LoginRefused);
            data = data.mid(2);
        }
        password.reset();
        enter(Step::Request);
        const QByteArray request = wire::socksConnectRequest(name);
        if (request.isEmpty())
            return fail(Failure::Internal);
        io = conn.sendAll(request.constData(), request.size(), handshake, waits, 2);
        if (io != ws::Conn::Io::Ok)
            return fail(ioFailure(io, Failure::CantReachSite));
        io = readUntil(conn, &data, handshake, waits, 2, [](const QByteArray& d) { return wire::socksReply(d).complete; });
        if (io != ws::Conn::Io::Ok)
            return fail(ioFailure(io, Failure::CantReachSite));
        const wire::SocksReply reply = wire::socksReply(data);
        if (reply.failure != Failure::None)
            return fail(reply.failure, reply.rep);
        r.early = data.mid(reply.consumed);
        r.step  = Step::Relay;
        r.conn  = &conn;
        return r;
    }

    // HTTP CONNECT, challenge-first unless the proxy is on this PC.
    wire::ConnectFlow flow(loopback, state.haveLogin());
    enter(Step::Request);
    // A new connection to the same address (after a 407 that closes, or a resend that found the connection closed).
    const auto reconnect = [&]() {
        const ws::Conn::Io again = conn.connectTo(connected, spec.port, handshake, waits, 2);
        if (again == ws::Conn::Io::Ok)
            return Failure::None;
        return again == ws::Conn::Io::Stopped ? Failure::Off : again == ws::Conn::Io::Timeout ? Failure::Slow : Failure::Unreachable;
    };
    bool sameConnection = false; // this CONNECT is the resend on the connection that carried the 407
    bool reconnected    = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (flow.sendLogin() && attempt > 0)
            enter(Step::Login);
        char      request[512 + 2 * 3 * wire::kMaxLoginBytes];
        const int size = wire::httpConnectRequest(name, user.constData(), user.size(), password.data(), password.size(), flow.sendLogin(), request,
                                                  sizeof(request));
        if (size < 0) {
            SecureZeroMemory(request, sizeof(request));
            return fail(Failure::Internal);
        }
        ws::Conn::Io io = conn.sendAll(request, size, handshake, waits, 2);
        SecureZeroMemory(request, sizeof(request));
        QByteArray data;
        if (io == ws::Conn::Io::Ok) {
            io = readUntil(conn, &data, handshake, waits, 2, [](const QByteArray& d) {
                const wire::ResponseHead h = wire::responseHead(d);
                return h.complete || h.tooLarge;
            });
        }
        // Review fix: a proxy may close after its 407 without saying so (an HTTP/1.0 answer, gost, goproxy). The resend
        // with Basic that finds that connection closed before any byte goes once more, on a new connection, instead of
        // reading the closed connection as "not HTTP".
        if (sameConnection && !reconnected && data.isEmpty() && (io == ws::Conn::Io::Eof || io == ws::Conn::Io::Error)) {
            reconnected    = true;
            sameConnection = false;
            const Failure f = reconnect();
            if (f != Failure::None)
                return fail(f);
            --attempt; // the same CONNECT with Basic, on the new connection
            continue;
        }
        sameConnection = false;
        if (io == ws::Conn::Io::Eof || io == ws::Conn::Io::Error) {
            // Closed without an HTTP answer: a port that doesn't speak HTTP (a SOCKS-only port closes at once).
            return fail(data.isEmpty() || !data.startsWith("HTTP/") ? Failure::NotHttp : Failure::CantReachSite);
        }
        if (io != ws::Conn::Io::Ok)
            return fail(ioFailure(io, Failure::NotHttp));
        const wire::ResponseHead head = wire::responseHead(data);
        if (head.tooLarge)
            return fail(Failure::NotHttp);
        Failure                       failure     = Failure::None;
        bool                          loginUnused = false;
        const wire::ConnectFlow::Next next        = flow.onResponse(head, &failure, &loginUnused);
        switch (next) {
        case wire::ConnectFlow::Next::Success:
            password.reset();
            r.loginUnused = loginUnused;
            r.early       = data.mid(head.headBytes);
            r.step        = Step::Relay;
            r.conn        = &conn;
            return r;
        case wire::ConnectFlow::Next::Fail:
            return fail(failure, head.status);
        case wire::ConnectFlow::Next::ResendSameConnection: {
            // The 407's body is read and dropped by count; the CONNECT goes again with Basic on the same connection.
            const qint64 left = head.contentLength - (data.size() - head.headBytes);
            io                = left > 0 ? drainBytes(conn, left, handshake, waits, 2) : ws::Conn::Io::Ok;
            if (io == ws::Conn::Io::Eof || io == ws::Conn::Io::Error) {
                const Failure f = reconnect(); // closed in or right after its body: a new connection
                if (f != Failure::None)
                    return fail(f);
                break;
            }
            if (io != ws::Conn::Io::Ok)
                return fail(ioFailure(io, Failure::LoginRefused));
            sameConnection = true;
            break;
        }
        case wire::ConnectFlow::Next::ResendNewConnection: {
            const Failure f = reconnect();
            if (f != Failure::None)
                return fail(f);
            break;
        }
        }
    }
    return fail(Failure::LoginRefused);
}

} // namespace proxy
