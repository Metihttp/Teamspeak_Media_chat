#include "winsock.h"

#include <climits>
#include <cstring>

namespace proxy::ws {

namespace {

template <typename T>
void resolveProc(HMODULE dll, T& fn, const char* name)
{
    fn = dll ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name))) : nullptr;
}

Api* loadApi()
{
    auto* a = new Api;
    a->dll  = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    resolveProc(a->dll, a->startup, "WSAStartup");
    resolveProc(a->dll, a->cleanup, "WSACleanup");
    resolveProc(a->dll, a->lastError, "WSAGetLastError");
    resolveProc(a->dll, a->socket, "socket");
    resolveProc(a->dll, a->closesocket, "closesocket");
    resolveProc(a->dll, a->bind, "bind");
    resolveProc(a->dll, a->listen, "listen");
    resolveProc(a->dll, a->accept, "accept");
    resolveProc(a->dll, a->connect, "connect");
    resolveProc(a->dll, a->send, "send");
    resolveProc(a->dll, a->recv, "recv");
    resolveProc(a->dll, a->shutdown, "shutdown");
    resolveProc(a->dll, a->setsockopt, "setsockopt");
    resolveProc(a->dll, a->getsockname, "getsockname");
    resolveProc(a->dll, a->getpeername, "getpeername");
    resolveProc(a->dll, a->ioctlsocket, "ioctlsocket");
    resolveProc(a->dll, a->eventSelect, "WSAEventSelect");
    resolveProc(a->dll, a->enumEvents, "WSAEnumNetworkEvents");
    resolveProc(a->dll, a->getAddrInfo, "GetAddrInfoW");
    resolveProc(a->dll, a->freeAddrInfo, "FreeAddrInfoW");
    resolveProc(a->dll, a->getAddrInfoEx, "GetAddrInfoExW");
    resolveProc(a->dll, a->freeAddrInfoEx, "FreeAddrInfoExW");
    resolveProc(a->dll, a->getAddrInfoExCancel, "GetAddrInfoExCancel");
    resolveProc(a->dll, a->getAddrInfoExOverlapped, "GetAddrInfoExOverlappedResult");
    if (a->startup) {
        WSADATA data{};
        a->started = a->startup(MAKEWORD(2, 2), &data) == 0; // once; never cleaned up
    }
    return a;
}

struct PendingLookup { // on the heap: Winsock writes into it until its completion (leaked if that never comes)
    OVERLAPPED   overlapped{};
    PADDRINFOEXW result       = nullptr;
    HANDLE       cancelHandle = nullptr;
};

constexpr DWORD kLookupCloseWaitMs = 15 * 1000;

} // namespace

bool Api::ok() const
{
    return dll && started && lastError && socket && closesocket && bind && listen && accept && connect && send && recv && shutdown && setsockopt
           && getsockname && getpeername && ioctlsocket && eventSelect && enumEvents;
}

const Api& api()
{
    static Api* a = loadApi(); // never deleted: nothing of it runs at unload
    return *a;
}

quint64 nowMs()
{
    return GetTickCount64();
}

int msUntil(quint64 when)
{
    const quint64 now = nowMs();
    if (when <= now)
        return 0;
    return static_cast<int>(qMin<quint64>(when - now, static_cast<quint64>(INT_MAX)));
}

linkpreview::net::Address fromSockaddr(const sockaddr* address)
{
    if (!address)
        return {};
    if (address->sa_family == AF_INET) {
        const auto*   in = reinterpret_cast<const sockaddr_in*>(address);
        const auto*   b  = reinterpret_cast<const quint8*>(&in->sin_addr);
        const quint32 v4 = (static_cast<quint32>(b[0]) << 24) | (static_cast<quint32>(b[1]) << 16) | (static_cast<quint32>(b[2]) << 8) | b[3];
        return linkpreview::net::Address::fromIPv4(v4);
    }
    if (address->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        return linkpreview::net::Address::fromIPv6(reinterpret_cast<const quint8*>(&in6->sin6_addr));
    }
    return {};
}

bool toSockaddr(const linkpreview::net::Address& address, quint16 port, sockaddr_storage* out, int* length)
{
    memset(out, 0, sizeof(*out));
    const quint16 netPort = static_cast<quint16>((port >> 8) | (port << 8));
    if (address.family == linkpreview::net::Address::Family::IPv4) {
        auto* in       = reinterpret_cast<sockaddr_in*>(out);
        in->sin_family = AF_INET;
        in->sin_port   = netPort;
        memcpy(&in->sin_addr, address.bytes.data(), 4);
        *length = static_cast<int>(sizeof(sockaddr_in));
        return true;
    }
    if (address.family == linkpreview::net::Address::Family::IPv6) {
        auto* in6        = reinterpret_cast<sockaddr_in6*>(out);
        in6->sin6_family = AF_INET6;
        in6->sin6_port   = netPort;
        memcpy(&in6->sin6_addr, address.bytes.data(), 16);
        *length = static_cast<int>(sizeof(sockaddr_in6));
        return true;
    }
    return false;
}

int lookup(const QString& name, QVector<linkpreview::net::Address>* out, const HANDLE* cancelEvents, int cancelCount, int timeoutMs)
{
    out->clear();
    const Api& ws = api();
    if (!ws.lookupOk() || !ws.started)
        return 3;
    const auto collect = [out](const auto* list) {
        for (auto it = list; it && out->size() < 64; it = it->ai_next) {
            const linkpreview::net::Address address = fromSockaddr(it->ai_addr);
            if (!address.isNull())
                out->append(address);
        }
    };
    const std::wstring wname = name.toStdWString();
    if (ws.cancelable()) {
        ADDRINFOEXW hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        auto* pending     = new PendingLookup;
        pending->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pending->overlapped.hEvent) {
            delete pending;
            return 3;
        }
        INT rc = ws.getAddrInfoEx(wname.c_str(), nullptr, NS_DNS, nullptr, &hints, &pending->result, nullptr, &pending->overlapped, nullptr,
                                  &pending->cancelHandle);
        if (rc == WSA_IO_PENDING) {
            HANDLE handles[4] = {pending->overlapped.hEvent, nullptr, nullptr, nullptr};
            int    count      = 1;
            for (int i = 0; i < cancelCount && count < 4; ++i) {
                if (cancelEvents[i])
                    handles[count++] = cancelEvents[i];
            }
            const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(count), handles, FALSE, static_cast<DWORD>(qMax(0, timeoutMs)));
            if (wait != WAIT_OBJECT_0) {
                ws.getAddrInfoExCancel(&pending->cancelHandle);
                if (WaitForSingleObject(pending->overlapped.hEvent, kLookupCloseWaitMs) != WAIT_OBJECT_0)
                    return 2; // Winsock may still write into pending: leave it
                if (pending->result)
                    ws.freeAddrInfoEx(pending->result);
                CloseHandle(pending->overlapped.hEvent);
                delete pending;
                return 2;
            }
            rc = ws.getAddrInfoExOverlapped(&pending->overlapped);
        }
        const bool unsupported = rc == WSAEINVAL || rc == WSAEOPNOTSUPP; // no asynchronous lookups (Windows 7)
        if (rc == 0)
            collect(pending->result);
        if (pending->result)
            ws.freeAddrInfoEx(pending->result);
        CloseHandle(pending->overlapped.hEvent);
        delete pending;
        if (!unsupported)
            return rc == 0 ? 0 : 1;
    }
    // Blocking lookup (older Windows): bounded by the system's own DNS time-outs.
    ADDRINFOW hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOW result = nullptr;
    const INT  rc     = ws.getAddrInfo(wname.c_str(), nullptr, &hints, &result);
    if (rc == 0)
        collect(result);
    if (result)
        ws.freeAddrInfo(result);
    for (int i = 0; i < cancelCount; ++i) {
        if (cancelEvents[i] && WaitForSingleObject(cancelEvents[i], 0) == WAIT_OBJECT_0)
            return 2;
    }
    return rc == 0 ? 0 : 1;
}

// ---- Conn -----------------------------------------------------------------------------------------------------------

bool Conn::attach(SOCKET s)
{
    close();
    const Api& ws = api();
    if (s == INVALID_SOCKET)
        return false;
    if (!ws.ok()) {
        if (ws.closesocket)
            ws.closesocket(s); // taken all the same: the caller never closes s after this call
        return false;
    }
    m_socket = s;
    m_event  = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual reset: WSAEnumNetworkEvents resets it
    if (!m_event || ws.eventSelect(m_socket, m_event, FD_READ | FD_WRITE | FD_CLOSE | FD_CONNECT) != 0) {
        close();
        return false;
    }
    return true;
}

Conn::Io Conn::connectTo(const linkpreview::net::Address& address, quint16 port, quint64 deadline, const HANDLE* extra, int extraCount)
{
    close();
    const Api& ws = api();
    if (!ws.ok())
        return Io::Error;
    sockaddr_storage storage{};
    int              length = 0;
    if (!toSockaddr(address, port, &storage, &length))
        return Io::Error;
    const SOCKET s = ws.socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return Io::Error;
    if (!attach(s))
        return Io::Error;
    if (ws.connect(m_socket, reinterpret_cast<const sockaddr*>(&storage), length) == 0)
        return Io::Ok;
    if (ws.lastError() != WSAEWOULDBLOCK) {
        m_error = ws.lastError();
        return Io::Error;
    }
    for (;;) {
        const Io wait = waitFor(deadline, extra, extraCount);
        if (wait != Io::Ok)
            return wait;
        WSANETWORKEVENTS events{};
        if (ws.enumEvents(m_socket, m_event, &events) != 0) {
            m_error = ws.lastError();
            return Io::Error;
        }
        if (events.lNetworkEvents & FD_CONNECT) {
            m_error = events.iErrorCode[FD_CONNECT_BIT];
            if (m_error != 0)
                return Io::Error;
            if (events.lNetworkEvents & FD_WRITE)
                SetEvent(m_event); // keep the write readiness for the first send
            return Io::Ok;
        }
        if (events.lNetworkEvents & FD_CLOSE)
            return Io::Error;
    }
}

void Conn::close()
{
    const Api& ws = api();
    if (m_socket != INVALID_SOCKET && ws.closesocket)
        ws.closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    if (m_event)
        CloseHandle(m_event);
    m_event = nullptr;
}

void Conn::abort()
{
    const Api& ws = api();
    if (m_socket != INVALID_SOCKET && ws.setsockopt) {
        LINGER linger{1, 0};
        ws.setsockopt(m_socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
    }
    close();
}

void Conn::shutdownSend()
{
    if (m_socket != INVALID_SOCKET)
        api().shutdown(m_socket, SD_SEND);
}

long Conn::takeEvents()
{
    if (m_socket == INVALID_SOCKET)
        return 0;
    WSANETWORKEVENTS events{};
    if (api().enumEvents(m_socket, m_event, &events) != 0)
        return 0;
    return events.lNetworkEvents;
}

Conn::Io Conn::waitFor(quint64 deadline, const HANDLE* extra, int extraCount)
{
    HANDLE handles[8] = {m_event};
    int    count      = 1;
    for (int i = 0; i < extraCount && count < 8; ++i) {
        if (extra[i])
            handles[count++] = extra[i];
    }
    const int left = msUntil(deadline);
    if (left <= 0)
        return Io::Timeout;
    const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(count), handles, FALSE, static_cast<DWORD>(left));
    if (r == WAIT_OBJECT_0)
        return Io::Ok;
    if (r == WAIT_TIMEOUT)
        return Io::Timeout;
    if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + static_cast<DWORD>(count))
        return Io::Stopped;
    return Io::Error;
}

Conn::Io Conn::tryRecv(char* buffer, int capacity, int* got)
{
    *got = 0;
    if (m_socket == INVALID_SOCKET)
        return Io::Error;
    const Api& ws = api();
    const int  n  = ws.recv(m_socket, buffer, capacity, 0);
    if (n > 0) {
        *got = n;
        return Io::Ok;
    }
    if (n == 0)
        return Io::Eof;
    const int error = ws.lastError();
    if (error == WSAEWOULDBLOCK)
        return Io::Ok;
    m_error = error;
    return Io::Error;
}

Conn::Io Conn::trySend(const char* data, int size, int* sent)
{
    *sent = 0;
    if (m_socket == INVALID_SOCKET)
        return Io::Error;
    if (size <= 0)
        return Io::Ok;
    const Api& ws = api();
    const int  n  = ws.send(m_socket, data, size, 0);
    if (n >= 0) {
        *sent = n;
        return Io::Ok;
    }
    const int error = ws.lastError();
    if (error == WSAEWOULDBLOCK)
        return Io::Ok;
    m_error = error;
    return Io::Error;
}

Conn::Io Conn::sendAll(const char* data, int size, quint64 deadline, const HANDLE* extra, int extraCount)
{
    int done = 0;
    while (done < size) {
        int      sent = 0;
        const Io io   = trySend(data + done, size - done, &sent);
        if (io != Io::Ok)
            return io;
        done += sent;
        if (done >= size)
            break;
        if (sent == 0) {
            const Io wait = waitFor(deadline, extra, extraCount);
            if (wait != Io::Ok)
                return wait;
            takeEvents();
        }
    }
    return Io::Ok;
}

Conn::Io Conn::recvSome(char* buffer, int capacity, int* got, quint64 deadline, const HANDLE* extra, int extraCount)
{
    *got = 0;
    for (;;) {
        const Io io = tryRecv(buffer, capacity, got);
        if (io != Io::Ok || *got > 0)
            return io;
        const Io wait = waitFor(deadline, extra, extraCount);
        if (wait != Io::Ok)
            return wait;
        takeEvents();
    }
}

} // namespace proxy::ws
