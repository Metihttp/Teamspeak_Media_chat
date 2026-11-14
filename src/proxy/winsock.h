#pragma once

// 2.6 chat proxy: ws2_32.dll loaded at run time from System32 (the plugin never links it), one table for the bridge,
// its upstream connections, Find my VPN app and the link-preview transport's name lookups. WSAStartup runs once, on
// the first use, and is never undone (threads of the bridge may still be winding down at unload).
//
// Conn: one non-blocking socket with its own Win32 event (WSAEventSelect). Every wait also wakes on the caller's
// extra events (the bridge's stop event, the generation's epoch event) and is bounded by a deadline (GetTickCount64).
// Only the thread that owns a Conn calls into it (closesocket is never concurrent with another call on the socket).

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <QString>
#include <QVector>

#include "linkpreview/netpolicy.h"

namespace proxy::ws {

struct Api {
    HMODULE dll = nullptr;
    decltype(&::WSAStartup)                    startup        = nullptr;
    decltype(&::WSACleanup)                    cleanup        = nullptr;
    decltype(&::WSAGetLastError)               lastError      = nullptr;
    decltype(&::socket)                        socket         = nullptr;
    decltype(&::closesocket)                   closesocket    = nullptr;
    decltype(&::bind)                          bind           = nullptr;
    decltype(&::listen)                        listen         = nullptr;
    decltype(&::accept)                        accept         = nullptr;
    decltype(&::connect)                       connect        = nullptr;
    decltype(&::send)                          send           = nullptr;
    decltype(&::recv)                          recv           = nullptr;
    decltype(&::shutdown)                      shutdown       = nullptr;
    decltype(&::setsockopt)                    setsockopt     = nullptr;
    decltype(&::getsockname)                   getsockname    = nullptr;
    decltype(&::getpeername)                   getpeername    = nullptr;
    decltype(&::ioctlsocket)                   ioctlsocket    = nullptr;
    decltype(&::WSAEventSelect)                eventSelect    = nullptr;
    decltype(&::WSAEnumNetworkEvents)          enumEvents     = nullptr;
    decltype(&::GetAddrInfoW)                  getAddrInfo    = nullptr;
    decltype(&::FreeAddrInfoW)                 freeAddrInfo   = nullptr;
    decltype(&::GetAddrInfoExW)                getAddrInfoEx  = nullptr;
    decltype(&::FreeAddrInfoExW)               freeAddrInfoEx = nullptr;
    decltype(&::GetAddrInfoExCancel)           getAddrInfoExCancel     = nullptr; // Windows 8+
    decltype(&::GetAddrInfoExOverlappedResult) getAddrInfoExOverlapped = nullptr; // Windows 8+
    bool started = false; // WSAStartup succeeded

    bool ok() const;          // the socket calls the bridge needs, and WSAStartup done
    bool lookupOk() const { return dll && startup && cleanup && getAddrInfo && freeAddrInfo; }
    bool cancelable() const { return getAddrInfoEx && freeAddrInfoEx && getAddrInfoExCancel && getAddrInfoExOverlapped; }
};
const Api& api(); // loaded (and started) on the first call, thread-safe; never unloaded

linkpreview::net::Address fromSockaddr(const sockaddr* address);
// sockaddr for address:port; false for a null address.
bool toSockaddr(const linkpreview::net::Address& address, quint16 port, sockaddr_storage* out, int* length);

// Every address of name (A and AAAA), asynchronous and cancelable where Windows supports it: canceled when one of
// cancelEvents is set or after timeoutMs. 0: found (out may still be empty), 1: not found, 2: canceled or timed out,
// 3: Winsock unusable.
int lookup(const QString& name, QVector<linkpreview::net::Address>* out, const HANDLE* cancelEvents, int cancelCount, int timeoutMs);

class Conn
{
  public:
    enum class Io { Ok, Eof, Error, Timeout, Stopped };

    Conn() = default;
    ~Conn() { close(); }
    Conn(const Conn&)            = delete;
    Conn& operator=(const Conn&) = delete;

    // Takes s (non-blocking from now on, events FD_READ | FD_WRITE | FD_CLOSE on its own event). s is the Conn's from
    // the call on, also when it fails (then it is already closed): the caller never closes s itself.
    bool   attach(SOCKET s);
    // A new socket to address:port, non-blocking; waits for the connect (Ok, Error = refused/unreachable, Timeout,
    // Stopped).
    Io     connectTo(const linkpreview::net::Address& address, quint16 port, quint64 deadline, const HANDLE* extra, int extraCount);
    SOCKET socket() const { return m_socket; }
    HANDLE event() const { return m_event; }
    bool   isOpen() const { return m_socket != INVALID_SOCKET; }
    void   close();
    void   abort();             // close with a reset (SO_LINGER 0)
    void   shutdownSend();      // FIN to the other side
    int    error() const { return m_error; }

    Io sendAll(const char* data, int size, quint64 deadline, const HANDLE* extra, int extraCount);
    // At most capacity bytes: Ok with *got > 0, Eof (the other side closed), Error, Timeout, Stopped.
    Io recvSome(char* buffer, int capacity, int* got, quint64 deadline, const HANDLE* extra, int extraCount);
    // Non-blocking tries (no wait): *got / *sent 0 when nothing could move now.
    Io tryRecv(char* buffer, int capacity, int* got);
    Io trySend(const char* data, int size, int* sent);
    // Clears the event after a wait woke on it (WSAEnumNetworkEvents).
    long takeEvents();

  private:
    Io waitFor(quint64 deadline, const HANDLE* extra, int extraCount);

    SOCKET m_socket = INVALID_SOCKET;
    HANDLE m_event  = nullptr;
    int    m_error  = 0;
};

quint64 nowMs();               // GetTickCount64()
int     msUntil(quint64 when); // 0 when past

} // namespace proxy::ws
