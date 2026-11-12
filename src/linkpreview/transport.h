#pragma once

// 2.4 link previews: the network under the Fetcher, one HTTPS exchange at a time. The real transport is
// WinHTTP (winhttptransport.cpp); unit tests inject a scripted one and never touch the internet.
//
// Everything here runs on a fetch worker thread and blocks it, never the GUI. A Cancel wakes every wait
// at once (it is a Win32 event too), so cancel() and shutdown() never wait for a slow website.
//
// A transport never follows redirects, sends no cookies, credentials or Referer, and asks
// HopOptions::allowPeer about every address it is about to connect to or did connect to: a refused one
// is dropped before the request is sent. Head::peer is the address the response came from (the Fetcher
// checks it again and refuses an unknown one).
//
// The Windows proxy (netpolicy.h): route() says whether a request goes through the proxy the user's
// Windows settings name for its address. Then the proxy resolves the name and connects, so the Fetcher
// asks for no resolve() and checks no peer (the peer is the proxy); open() sends the request through it.

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "netpolicy.h"

namespace linkpreview::http {

enum class Error {
    None,
    Unavailable, // winhttp.dll / ws2_32.dll missing or too old
    Offline,     // name not resolved, can't connect, connection lost
    Timeout,     // a step or the whole load took too long
    Tls,         // secure channel or certificate failure
    Blocked,     // an address that isn't public (before anything was sent)
    Canceled,
    BadResponse, // anything else unexpected
};

QString errorName(Error error); // "offline", "timeout", ... (logs, tests)

// A cancel flag that workers can wait on. set() is safe from any thread, any number of times.
class Cancel
{
  public:
    Cancel();
    ~Cancel();
    Cancel(const Cancel&)            = delete;
    Cancel& operator=(const Cancel&) = delete;

    void  set();
    bool  isSet() const { return m_flag.load(); }
    void* event() const { return m_event; } // a manual-reset Win32 event, signaled by set()
    bool  wait(int ms) const;               // true when set (now or within ms)

  private:
    std::atomic_bool m_flag{false};
    void*            m_event = nullptr;
};

quint64 nowMs();                         // GetTickCount64()
int     msUntil(quint64 deadline);       // 0 when past, at most INT_MAX

struct Head {
    int          status = 0;
    QByteArray   contentType;        // as sent (at most 256 bytes)
    qint64       contentLength = -1; // -1: not sent
    QString      location;           // redirects (at most 8 KB)
    net::Address peer;               // the address the response came from; null: unknown
    // 2.5
    QByteArray contentEncoding;    // as sent, lower case (at most 64 bytes); empty: none
    qint64     retryAfterMs  = -1; // Retry-After (delta seconds or an HTTP date), else on a 429 the rate reset below; -1: none
    double     rateRemaining = -1; // x-ratelimit-remaining (a decimal >= 0, "0.0" allowed); -1: absent or unreadable
    qint64     rateResetMs   = -1; // net::parseRateReset of x-ratelimit-reset-after / x-ratelimit-reset; -1: absent or unreadable
    bool       challenge     = false; // "cf-mitigated: challenge" (a Cloudflare bot challenge)
};

// How one request reaches its website.
struct Route {
    bool    viaProxy = false; // through the user's Windows proxy
    QString proxy;            // for the transport only ("host:port[;host:port]"), never logged
};

struct HopOptions {
    QUrl       url;
    Route      route;            // direct, or through the Windows proxy (Transport::route())
    QByteArray accept;           // the Accept header ("image/*")
    quint64    deadline = 0;     // nowMs() value: no wait goes past it
    int        connectTimeoutMs = 10 * 1000;
    int        receiveTimeoutMs = 15 * 1000;
    QByteArray userAgent;                   // 2.5: net::userAgent() (the Fetcher fills it); empty: the transport's own
    QByteArray acceptEncoding = "identity"; // 2.5: never compressed (no decompression code, no bomb)
    // Called (on any thread) with each address before connecting and after connecting; false drops the
    // connection before the request is sent (Error::Blocked).
    std::function<bool(const net::Address&)> allowPeer;
};

class Exchange
{
  public:
    virtual ~Exchange() = default; // closes the connection (and waits until the system let go of it)
    virtual Error head(Head* out) = 0; // waits for the response headers
    // Up to capacity bytes of the body into buffer; *got = 0 when the body has ended.
    virtual Error read(char* buffer, int capacity, int* got) = 0;
    virtual unsigned long systemError() const { return 0; } // the last WinHTTP / Winsock error, for the log
};

class Transport
{
  public:
    virtual ~Transport() = default;
    // Every address of an ASCII host name (A and AAAA). An empty list with Error::None counts as Offline.
    virtual Error resolve(const QString& host, QVector<net::Address>* out, const Cancel& cancel, quint64 deadline) = 0;
    // Starts one GET (no redirects followed). Null with *error set when it can't.
    virtual std::unique_ptr<Exchange> open(const HopOptions& options, const Cancel& cancel, Error* error) = 0;
    // The route of a request to url (an allowed https address): direct (the default), or through the
    // proxy the user's Windows settings name for it. May block (auto-config); bounded by the deadline
    // as far as the system allows.
    virtual Route route(const QUrl& url, const Cancel& cancel, quint64 deadline)
    {
        Q_UNUSED(url);
        Q_UNUSED(cancel);
        Q_UNUSED(deadline);
        return {};
    }
};

// WinHTTP (asynchronous mode, so a cancel closes a request at once). Direct connections unless the
// user's Windows settings name a proxy (manual, or auto-config / WPAD): then through it (route()).
// winhttp.dll and ws2_32.dll are loaded from System32 on the first use and stay loaded (their thread
// pools may still be winding down after the last handle closed).
std::shared_ptr<Transport> winHttpTransport();

// A worker that could not get the system to let go of a request in time leaves its memory alone and
// keeps the plugin DLL loaded (a late system callback must never land in an unloaded DLL). Fetcher
// threads check this before releasing their pin.
bool mustStayLoaded();
void setMustStayLoaded();

} // namespace linkpreview::http
