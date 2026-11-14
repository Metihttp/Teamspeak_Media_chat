#include "bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "linkpreview/netpolicy.h"
#include "wire.h"

namespace proxy {

namespace {

constexpr int    kRingSize   = 256;
constexpr int    kChunk      = 64 * 1024;
constexpr DWORD  kListenWake = 5000;
constexpr int    kAnswerMs   = 2000; // to write one of our short answers
constexpr int    kDrainMs    = 300;  // after an error answer: read what the client still sends, then close
constexpr int    kDrainBytes = 64 * 1024; // ... at most this much of it
// The first tunnel of a generation may run CryptUnprotectData and GetAddrInfoExW: a little more than design 3.4.2's
// 64 KB reservation.
constexpr int    kMinStack   = 256 * 1024;

std::atomic<int> g_liveAll{0};

} // namespace

struct BridgeState {
    Bridge::Options options;
    SOCKET          listener       = INVALID_SOCKET;
    HANDLE          acceptEvent    = nullptr;
    HANDLE          stopEvent      = nullptr;
    HANDLE          listenerThread = nullptr;
    quint16         port           = 0;
    QByteArray      token;
    QByteArray      realm;

    mutable SRWLOCK                lock = SRWLOCK_INIT;
    std::shared_ptr<UpstreamState> upstream; // null: none
    std::vector<HANDLE>            tunnels;  // thread handles (the listener adds and prunes; stop() takes them)

    std::atomic<int> unauthenticated{0}; // heads not read yet
    std::atomic<int> authenticated{0};   // tunnels past their head
    std::atomic<int> live{0};            // threads of this bridge still running

    struct Slot {
        Record  record;
        quint64 seq = 0;
    };
    std::vector<Slot> ring = std::vector<Slot>(kRingSize);
    quint64           nextSeq = 1;
    Bridge::Counters  counters;

    ~BridgeState()
    {
        for (HANDLE h : tunnels)
            CloseHandle(h);
        if (listenerThread)
            CloseHandle(listenerThread);
        if (listener != INVALID_SOCKET && ws::api().closesocket)
            ws::api().closesocket(listener);
        if (acceptEvent)
            CloseHandle(acceptEvent);
        if (stopEvent)
            CloseHandle(stopEvent);
    }

    bool stopping() const { return WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0; }

    quint64 newRecord()
    {
        AcquireSRWLockExclusive(&lock);
        const quint64 seq = nextSeq++;
        Slot&         s   = ring[static_cast<size_t>(seq % kRingSize)];
        s.seq             = seq;
        s.record          = Record();
        s.record.startMs  = ws::nowMs();
        ++counters.accepted;
        ReleaseSRWLockExclusive(&lock);
        return seq;
    }

    template <typename Fn>
    void update(quint64 seq, Fn fn)
    {
        AcquireSRWLockExclusive(&lock);
        Slot& s = ring[static_cast<size_t>(seq % kRingSize)];
        if (s.seq == seq)
            fn(s.record);
        ReleaseSRWLockExclusive(&lock);
    }

    template <typename Fn>
    void count(Fn fn)
    {
        AcquireSRWLockExclusive(&lock);
        fn(counters);
        ReleaseSRWLockExclusive(&lock);
    }

    std::shared_ptr<UpstreamState> currentUpstream() const
    {
        AcquireSRWLockShared(&lock);
        std::shared_ptr<UpstreamState> u = upstream;
        ReleaseSRWLockShared(&lock);
        return u;
    }

    void pruneTunnelsLocked()
    {
        for (size_t i = tunnels.size(); i-- > 0;) {
            if (WaitForSingleObject(tunnels[i], 0) == WAIT_OBJECT_0) {
                CloseHandle(tunnels[i]);
                tunnels.erase(tunnels.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
};

namespace {

struct ThreadArg {
    std::shared_ptr<BridgeState> state;
    SOCKET                       socket = INVALID_SOCKET;
    quint64                      seq    = 0;
    QStringList                  suffixes;
    HMODULE                      module = nullptr;
};

bool pinModule(HMODULE* module, const void* address)
{
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), module) != FALSE;
}

// One of our short answers, then a graceful close (what the client still sends is read and dropped for a moment, so
// the answer isn't lost to a reset).
void answerAndClose(ws::Conn& conn, const QByteArray& answer, HANDLE stop)
{
    if (!conn.isOpen())
        return;
    const quint64 deadline = ws::nowMs() + kAnswerMs;
    if (conn.sendAll(answer.constData(), answer.size(), deadline, &stop, 1) == ws::Conn::Io::Ok) {
        conn.shutdownSend();
        char          sink[1024];
        const quint64 drainUntil = ws::nowMs() + kDrainMs;
        int           drained    = 0;
        // Review fix: recvSome checks its deadline and the stop event only when it has to wait, so a client that keeps
        // sending would hold this thread (outside every cap) for as long as it likes: the time, the stop event and a
        // byte cap are checked here too.
        while (drained < kDrainBytes && ws::nowMs() < drainUntil && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            int                got = 0;
            const ws::Conn::Io io  = conn.recvSome(sink, sizeof(sink), &got, drainUntil, &stop, 1);
            if (io != ws::Conn::Io::Ok)
                break;
            drained += got;
        }
    }
    conn.close();
}

struct Buffer {
    std::vector<char> data = std::vector<char>(kChunk);
    int               begin = 0;
    int               end   = 0;
    bool empty() const { return begin == end; }
    void clear() { begin = end = 0; }
    void fill(const QByteArray& bytes)
    {
        const int n = qMin(bytes.size(), kChunk);
        memcpy(data.data(), bytes.constData(), static_cast<size_t>(n));
        begin = 0;
        end   = n;
    }
};

// The relay of an established tunnel (design 3.4.5). Returns the record's failure (None, or CantReachSite by the
// "optimistic proxy" rule).
Failure relay(BridgeState& st, quint64 seq, ws::Conn& client, ws::Conn& upstream, const QByteArray& clientEarly, const QByteArray& siteEarly,
              HANDLE epoch)
{
    Buffer  toUpstream, toClient;
    qint64  upBytes = 0, downBytes = 0, siteBytes = 0;
    bool    clientEof = false, upstreamEof = false, upstreamShut = false, clientShut = false;
    Failure failure  = Failure::None;
    quint64 activity = ws::nowMs();
    // Early bytes: what the client pipelined after its head, what the proxy sent after its answer (the site's).
    if (!clientEarly.isEmpty())
        toUpstream.fill(clientEarly);
    if (!siteEarly.isEmpty()) {
        toClient.fill(siteEarly);
        siteBytes += toClient.end;
    }
    const int idleMs = qMax(100, st.options.idleMs);
    for (;;) {
        bool progress = false;
        // client -> site
        if (toUpstream.empty() && !clientEof) {
            int                got = 0;
            const ws::Conn::Io io  = client.tryRecv(toUpstream.data.data(), kChunk, &got);
            if (io == ws::Conn::Io::Eof) {
                clientEof = true;
                progress  = true;
            } else if (io != ws::Conn::Io::Ok) {
                upstream.abort();
                break;
            } else if (got > 0) {
                toUpstream.begin = 0;
                toUpstream.end   = got;
                progress         = true;
            }
        }
        if (!toUpstream.empty()) {
            int                sent = 0;
            const ws::Conn::Io io   = upstream.trySend(toUpstream.data.data() + toUpstream.begin, toUpstream.end - toUpstream.begin, &sent);
            if (io != ws::Conn::Io::Ok) {
                if (siteBytes == 0 && !clientEof) {
                    // the proxy said yes, then dropped the connection before the site answered (recorded first)
                    failure = Failure::CantReachSite;
                    st.update(seq, [](Record& r) { r.failure = Failure::CantReachSite; });
                }
                client.abort();
                break;
            }
            toUpstream.begin += sent;
            upBytes += sent;
            if (toUpstream.empty())
                toUpstream.clear();
            progress = progress || sent > 0;
        }
        if (clientEof && toUpstream.empty() && !upstreamShut) {
            upstream.shutdownSend(); // the client's FIN, passed on after its last byte (F14)
            upstreamShut = true;
        }
        // site -> client
        if (toClient.empty() && !upstreamEof) {
            int                got = 0;
            const ws::Conn::Io io  = upstream.tryRecv(toClient.data.data(), kChunk, &got);
            if (io == ws::Conn::Io::Eof) {
                upstreamEof = true;
                progress    = true;
            } else if (io != ws::Conn::Io::Ok) {
                if (siteBytes == 0 && upBytes > 0 && !clientEof) {
                    failure = Failure::CantReachSite;
                    st.update(seq, [](Record& r) { r.failure = Failure::CantReachSite; });
                }
                client.abort();
                break;
            } else if (got > 0) {
                toClient.begin = 0;
                toClient.end   = got;
                siteBytes += got;
                progress = true;
            }
        }
        if (!toClient.empty()) {
            int                sent = 0;
            const ws::Conn::Io io   = client.trySend(toClient.data.data() + toClient.begin, toClient.end - toClient.begin, &sent);
            if (io != ws::Conn::Io::Ok) {
                upstream.abort();
                break;
            }
            toClient.begin += sent;
            downBytes += sent;
            if (toClient.empty())
                toClient.clear();
            progress = progress || sent > 0;
        }
        if (upstreamEof && toClient.empty() && !clientShut) {
            // 2.6 I2a finding: a client that closed first (Chromium drops a spare connection after its ClientHello) isn't
            // the proxy failing to reach the site: the rule needs the client still there.
            if (siteBytes == 0 && upBytes > 0 && !clientEof) {
                // V2Ray / sing-box style: success first, then the site couldn't be reached (recorded before the close).
                failure = Failure::CantReachSite;
                st.update(seq, [&](Record& r) { r.failure = Failure::CantReachSite; });
                client.abort();
                break;
            }
            client.shutdownSend();
            clientShut = true;
        }
        if (clientEof && upstreamEof && toUpstream.empty() && toClient.empty())
            break;
        if (progress) {
            activity = ws::nowMs();
            if (upBytes + downBytes > 0)
                st.update(seq, [&](Record& r) {
                    r.up   = upBytes;
                    r.down = downBytes;
                });
            // Review fix: a tunnel that never runs dry still sees the stop and epoch events (a zero wait per chunk), so
            // the unload's bounded stop and a route change reach it at once, not when it goes idle.
            HANDLE      events[2] = {st.stopEvent, epoch};
            const DWORD signaled  = WaitForMultipleObjects(epoch ? 2 : 1, events, FALSE, 0);
            if (signaled == WAIT_OBJECT_0 || signaled == WAIT_OBJECT_0 + 1)
                break;
            continue;
        }
        const quint64 now = ws::nowMs();
        if (now - activity >= static_cast<quint64>(idleMs))
            break; // idle
        HANDLE      handles[4] = {client.event(), upstream.event(), st.stopEvent, epoch};
        const DWORD wait       = static_cast<DWORD>(qMin<quint64>(1000, static_cast<quint64>(idleMs) - (now - activity)));
        const DWORD r          = WaitForMultipleObjects(epoch ? 4 : 3, handles, FALSE, wait);
        if (r == WAIT_OBJECT_0 + 2 || r == WAIT_OBJECT_0 + 3)
            break; // the bridge stops, or a newer generation replaced this one (F4)
        client.takeEvents();
        upstream.takeEvents();
    }
    st.update(seq, [&](Record& r) {
        r.up   = upBytes;
        r.down = downBytes;
    });
    return failure;
}

void runTunnel(ThreadArg& arg)
{
    BridgeState&       st = *arg.state;
    const quint64      seq = arg.seq;
    ws::Conn           client;
    HANDLE             stop = st.stopEvent;
    bool               headPending = true;
    const auto         leaveHead   = [&]() {
        if (headPending) {
            headPending = false;
            --st.unauthenticated;
        }
    };
    const auto finish = [&](Failure failure, int code) {
        st.update(seq, [&](Record& r) {
            if (r.failure == Failure::None)
                r.failure = failure;
            if (code)
                r.code = code;
            r.step  = Step::Done;
            r.endMs = ws::nowMs();
        });
    };
    if (!client.attach(arg.socket)) {
        // attach() owns the socket from the call on and closed it itself (review fix: a second closesocket here could hit
        // a handle value another thread had been given meanwhile).
        leaveHead();
        finish(Failure::Internal, 0);
        return;
    }

    // 1. The head: at most 8 KB within 2 s (our clients send it at once).
    QByteArray       data;
    wire::ClientHead head;
    const quint64    headDeadline = ws::nowMs() + static_cast<quint64>(qMax(100, st.options.headTimeoutMs));
    char             buffer[4096];
    for (;;) {
        head = wire::clientHead(data, st.token, arg.suffixes);
        if (head.verdict != wire::ClientHead::Verdict::NeedMore)
            break;
        int                got = 0;
        const ws::Conn::Io io  = client.recvSome(buffer, sizeof(buffer), &got, headDeadline, &stop, 1);
        if (io != ws::Conn::Io::Ok) {
            // Slow, closed or stopped: closed without an answer.
            leaveHead();
            st.count([](Bridge::Counters& c) { ++c.unauthenticated; });
            finish(Failure::None, 0);
            client.abort();
            return;
        }
        data.append(buffer, got);
    }
    leaveHead();
    const quint64 headAt = ws::nowMs();
    switch (head.verdict) {
    case wire::ClientHead::Verdict::BadRequest:
    case wire::ClientHead::Verdict::MethodNotAllowed:
    case wire::ClientHead::Verdict::AuthRequired: {
        const int status = head.verdict == wire::ClientHead::Verdict::BadRequest ? 400 : head.verdict == wire::ClientHead::Verdict::MethodNotAllowed ? 405 : 407;
        st.count([](Bridge::Counters& c) { ++c.unauthenticated; });
        finish(Failure::None, status);
        answerAndClose(client, wire::errorResponse(status, status == 407 ? st.realm : QByteArray()), stop);
        return;
    }
    case wire::ClientHead::Verdict::Forbidden:
        st.count([](Bridge::Counters& c) { ++c.refused; });
        finish(Failure::Refused, 403);
        answerAndClose(client, wire::errorResponse(403, QByteArray(), "refused"), stop);
        return;
    case wire::ClientHead::Verdict::Ok:
    case wire::ClientHead::Verdict::NeedMore:
        break;
    }
    st.update(seq, [&](Record& r) {
        r.client = head.client;
        r.id     = head.id;
        r.host   = head.host;
    });

    // 2. The cap of authenticated tunnels.
    if (++st.authenticated > qMax(1, st.options.maxTunnels)) {
        --st.authenticated;
        st.count([](Bridge::Counters& c) { ++c.busy; });
        finish(Failure::Busy, 503);
        answerAndClose(client, wire::errorResponse(503, QByteArray(), "busy"), stop);
        return;
    }
    struct Release {
        BridgeState& st;
        ~Release() { --st.authenticated; }
    } release{st};

    // 3. The user's proxy.
    const std::shared_ptr<UpstreamState> up = st.currentUpstream();
    if (!up) {
        st.count([](Bridge::Counters& c) { ++c.failures[static_cast<int>(Failure::Off)]; });
        finish(Failure::Off, 502);
        answerAndClose(client, wire::errorResponse(502, QByteArray(), failureName(Failure::Off)), stop);
        return;
    }
    ws::Conn             upstream;
    const UpstreamResult result = openUpstream(*up, head.host.toLatin1(), upstream, stop, st.port, headAt, st.options.upstream,
                                               [&](Step s) { st.update(seq, [s](Record& r) { r.step = s; }); });
    if (result.failure != Failure::None) {
        st.count([&](Bridge::Counters& c) { ++c.failures[static_cast<int>(result.failure)]; });
        st.update(seq, [&](Record& r) {
            r.failure = result.failure;
            r.step    = result.step;
            r.code    = result.code;
            r.endMs   = ws::nowMs();
        });
        answerAndClose(client, wire::errorResponse(502, QByteArray(), failureName(result.failure)), stop);
        return;
    }
    st.update(seq, [&](Record& r) {
        r.step        = Step::Relay;
        r.loginUnused = result.loginUnused;
    });
    HANDLE waits[2] = {stop, up->epoch()};
    if (client.sendAll(wire::kEstablished, static_cast<int>(strlen(wire::kEstablished)), ws::nowMs() + kAnswerMs, waits, 2) != ws::Conn::Io::Ok) {
        finish(Failure::None, 0);
        upstream.abort();
        client.abort();
        return;
    }
    // Review fix (design 3.5, D13): the updater is never interrupted by a route change; its running read finishes on the
    // route it started on (its next request reads the new route). So an established tunnel of client 'u' (the update
    // check and download) doesn't close at a new generation, only at the bridge's stop or when it goes idle; every
    // other tunnel closes at once (F4).
    const HANDLE  relayEpoch = head.client == 'u' ? nullptr : up->epoch();
    const Failure failure    = relay(st, seq, client, upstream, data.mid(head.headBytes), result.early, relayEpoch);
    if (failure != Failure::None)
        st.count([&](Bridge::Counters& c) { ++c.failures[static_cast<int>(failure)]; });
    finish(failure, 0);
    upstream.close();
    client.close();
}

DWORD WINAPI tunnelThread(void* parameter)
{
    auto*   arg    = static_cast<ThreadArg*>(parameter);
    HMODULE module = arg->module;
    runTunnel(*arg);
    std::shared_ptr<BridgeState> state = std::move(arg->state);
    delete arg;
    --state->live;
    state.reset();
    --g_liveAll;
    FreeLibraryAndExitThread(module, 0); // drops the pin without returning into a DLL that may be unloaded now
}

HANDLE launchTunnel(const std::shared_ptr<BridgeState>& st, SOCKET s, quint64 seq, const QStringList& suffixes)
{
    auto* arg     = new ThreadArg;
    arg->state    = st;
    arg->socket   = s;
    arg->seq      = seq;
    arg->suffixes = suffixes;
    if (!pinModule(&arg->module, reinterpret_cast<const void*>(&tunnelThread))) {
        delete arg;
        return nullptr;
    }
    ++st->live;
    ++g_liveAll;
    const DWORD stack  = static_cast<DWORD>(qMax(kMinStack, st->options.threadStackBytes));
    HANDLE      thread = CreateThread(nullptr, stack, &tunnelThread, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        --st->live;
        --g_liveAll;
        FreeLibrary(arg->module);
        delete arg;
    }
    return thread;
}

struct ListenerArg {
    std::shared_ptr<BridgeState> state;
    HMODULE                      module = nullptr;
};

void runListener(const std::shared_ptr<BridgeState>& st)
{
    const ws::Api& api       = ws::api();
    HANDLE         waits[2]  = {st->acceptEvent, st->stopEvent};
    for (;;) {
        const DWORD r = WaitForMultipleObjects(2, waits, FALSE, kListenWake);
        AcquireSRWLockExclusive(&st->lock);
        st->pruneTunnelsLocked(); // a long session holds only the handles of live tunnels (F5)
        ReleaseSRWLockExclusive(&st->lock);
        if (st->stopping())
            break;
        if (r != WAIT_OBJECT_0)
            continue;
        WSANETWORKEVENTS events{};
        api.enumEvents(st->listener, st->acceptEvent, &events);
        const QStringList suffixes = st->options.localSuffixes ? st->options.localSuffixes() : linkpreview::net::localDnsSuffixes();
        for (;;) {
            const SOCKET s = api.accept(st->listener, nullptr, nullptr);
            if (s == INVALID_SOCKET)
                break;
            if (st->stopping() || st->unauthenticated.load() >= qMax(1, st->options.maxUnauthenticated)) {
                // over the cap of heads not read yet: closed at once, no answer (S17)
                LINGER linger{1, 0};
                api.setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
                api.closesocket(s);
                st->count([](Bridge::Counters& c) { ++c.unauthenticated; });
                continue;
            }
            ++st->unauthenticated;
            const quint64 seq    = st->newRecord();
            HANDLE        thread = launchTunnel(st, s, seq, suffixes);
            if (!thread) {
                --st->unauthenticated;
                api.closesocket(s);
                st->update(seq, [](Record& rec) {
                    rec.failure = Failure::Internal;
                    rec.step    = Step::Done;
                });
                continue;
            }
            AcquireSRWLockExclusive(&st->lock);
            st->tunnels.push_back(thread);
            ReleaseSRWLockExclusive(&st->lock);
        }
    }
    if (st->listener != INVALID_SOCKET) {
        api.closesocket(st->listener);
        st->listener = INVALID_SOCKET;
    }
}

DWORD WINAPI listenerThread(void* parameter)
{
    auto*   arg    = static_cast<ListenerArg*>(parameter);
    HMODULE module = arg->module;
    std::shared_ptr<BridgeState> state = std::move(arg->state);
    delete arg;
    runListener(state);
    --state->live;
    state.reset();
    --g_liveAll;
    FreeLibraryAndExitThread(module, 0);
}

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    return bytes;
}

SOCKET bindLoopback(quint16 port, int* error)
{
    const ws::Api& api = ws::api();
    const SOCKET   s   = api.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        *error = api.lastError();
        return INVALID_SOCKET;
    }
    BOOL exclusive = TRUE; // before bind: nobody can take the port over with SO_REUSEADDR
    api.setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family           = AF_INET;
    address.sin_port             = static_cast<USHORT>((port >> 8) | (port << 8));
    address.sin_addr.S_un.S_addr = 0x0100007F; // 127.0.0.1 in network order; never 0.0.0.0 or ::
    if (api.bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || api.listen(s, 16) != 0) {
        *error = api.lastError();
        api.closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

} // namespace

// ---- Bridge -------------------------------------------------------------------------------------------------------------

QByteArray newBridgeToken()
{
    const QByteArray bytes = randomBytes(32);
    return bytes.isEmpty() ? QByteArray() : bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray newBridgeRealm()
{
    const QByteArray bytes = randomBytes(8);
    return bytes.isEmpty() ? QByteArray() : QByteArray("tsmedia-") + bytes.toHex();
}

Bridge::Bridge(std::shared_ptr<BridgeState> state)
    : m_state(std::move(state))
{
}

std::shared_ptr<Bridge> Bridge::create(const Options& options, int* error)
{
    int  dummy = 0;
    int& err   = error ? *error : dummy;
    err        = 0;
    const ws::Api& api = ws::api();
    if (!api.ok()) {
        err = -1;
        return nullptr;
    }
    auto st     = std::make_shared<BridgeState>();
    st->options = options;
    st->token   = newBridgeToken();
    st->realm   = newBridgeRealm();
    if (st->token.size() != 43 || st->realm.size() != 24) {
        err = -1;
        return nullptr;
    }
    SOCKET s = INVALID_SOCKET;
    if (options.preferredPort >= 1024)
        s = bindLoopback(options.preferredPort, &err);
    if (s == INVALID_SOCKET)
        s = bindLoopback(0, &err);
    if (s == INVALID_SOCKET)
        return nullptr;
    st->listener = s;
    sockaddr_in bound{};
    int         length = sizeof(bound);
    if (api.getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        err = api.lastError();
        return nullptr;
    }
    st->port        = static_cast<quint16>((bound.sin_port >> 8) | (bound.sin_port << 8));
    st->acceptEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    st->stopEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!st->acceptEvent || !st->stopEvent || api.eventSelect(s, st->acceptEvent, FD_ACCEPT) != 0) {
        err = -1;
        return nullptr;
    }
    auto* arg  = new ListenerArg;
    arg->state = st;
    if (!pinModule(&arg->module, reinterpret_cast<const void*>(&listenerThread))) {
        delete arg;
        err = -1;
        return nullptr;
    }
    ++st->live;
    ++g_liveAll;
    st->listenerThread = CreateThread(nullptr, 0, &listenerThread, arg, 0, nullptr);
    if (!st->listenerThread) {
        --st->live;
        --g_liveAll;
        FreeLibrary(arg->module);
        delete arg;
        err = -1;
        return nullptr;
    }
    return std::shared_ptr<Bridge>(new Bridge(st));
}

Bridge::~Bridge()
{
    if (m_state && m_state->stopEvent)
        SetEvent(m_state->stopEvent); // never waits here: the threads hold the state and end by themselves
}

quint16 Bridge::port() const
{
    return m_state->port;
}

QByteArray Bridge::token() const
{
    return m_state->token;
}

QByteArray Bridge::realm() const
{
    return m_state->realm;
}

void Bridge::setUpstream(std::unique_ptr<UpstreamSpec> spec)
{
    std::shared_ptr<UpstreamState> next;
    if (spec)
        next = std::make_shared<UpstreamState>(std::move(*spec));
    std::shared_ptr<UpstreamState> old;
    AcquireSRWLockExclusive(&m_state->lock);
    old               = std::move(m_state->upstream);
    m_state->upstream = std::move(next);
    ReleaseSRWLockExclusive(&m_state->lock);
    if (old)
        SetEvent(old->epoch()); // every tunnel of the old generation closes within one wait
}

bool Bridge::hasUpstream() const
{
    return m_state->currentUpstream() != nullptr;
}

bool Bridge::stop(int waitMs)
{
    BridgeState&  st    = *m_state;
    const quint64 start = ws::nowMs();
    SetEvent(st.stopEvent);
    const auto left = [&]() -> DWORD {
        const quint64 used = ws::nowMs() - start;
        return used >= static_cast<quint64>(qMax(0, waitMs)) ? 0 : static_cast<DWORD>(static_cast<quint64>(waitMs) - used);
    };
    bool allDone = true;
    if (st.listenerThread) {
        // The listener first: after it nothing new is accepted (F5).
        if (WaitForSingleObject(st.listenerThread, left()) != WAIT_OBJECT_0)
            allDone = false;
    }
    std::vector<HANDLE> tunnels;
    AcquireSRWLockExclusive(&st.lock);
    tunnels.swap(st.tunnels);
    ReleaseSRWLockExclusive(&st.lock);
    for (size_t i = 0; i < tunnels.size(); i += MAXIMUM_WAIT_OBJECTS) {
        const DWORD count = static_cast<DWORD>(qMin<size_t>(MAXIMUM_WAIT_OBJECTS, tunnels.size() - i));
        if (WaitForMultipleObjects(count, tunnels.data() + i, TRUE, left()) == WAIT_TIMEOUT)
            allDone = false;
    }
    for (HANDLE h : tunnels)
        CloseHandle(h); // a thread still running keeps its own pin and ends by itself
    return allDone;
}

bool Bridge::isRunning() const
{
    return !m_state->stopping();
}

bool Bridge::outcome(char client, quint64 id, Record* out) const
{
    bool    found = false;
    quint64 best  = 0;
    AcquireSRWLockShared(&m_state->lock);
    for (const BridgeState::Slot& s : m_state->ring) {
        if (s.seq != 0 && s.record.client == client && s.record.id == id && s.seq > best) {
            best  = s.seq;
            *out  = s.record;
            found = true;
        }
    }
    ReleaseSRWLockShared(&m_state->lock);
    return found;
}

QVector<Record> Bridge::recent(char client, quint64 sinceMs) const
{
    std::vector<std::pair<quint64, Record>> found;
    AcquireSRWLockShared(&m_state->lock);
    for (const BridgeState::Slot& s : m_state->ring) {
        if (s.seq != 0 && s.record.client == client && s.record.startMs >= sinceMs)
            found.emplace_back(s.seq, s.record);
    }
    ReleaseSRWLockShared(&m_state->lock);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QVector<Record> out;
    for (const auto& f : found)
        out.append(f.second);
    return out;
}

Bridge::Counters Bridge::counters() const
{
    AcquireSRWLockShared(&m_state->lock);
    Counters c = m_state->counters;
    ReleaseSRWLockShared(&m_state->lock);
    c.tunnelsOpen = m_state->authenticated.load();
    return c;
}

int Bridge::liveThreads() const
{
    return m_state->live.load();
}

int Bridge::openHandles() const
{
    AcquireSRWLockShared(&m_state->lock);
    const int n = static_cast<int>(m_state->tunnels.size());
    ReleaseSRWLockShared(&m_state->lock);
    return n;
}

int Bridge::liveThreadsAll()
{
    return g_liveAll.load();
}

} // namespace proxy
