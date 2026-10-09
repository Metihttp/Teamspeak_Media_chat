#pragma once

// Synchronous HTTPS GET through WinHTTP (Windows' own TLS), for the update worker thread only.
//
// - winhttp.dll is loaded from System32 when a request starts and released when it ends, so a plugin
//   with updates off never maps a network library.
// - Redirects are followed by hand (at most kMaxRedirects), and every hop must pass isAllowedUrl():
//   GitHub hosts over https only.
// - No cookies, no credentials, no certificate revocation fetches requested by us; the User-Agent is
//   "TSMedia-Updater/1" without a version.
// - A cancel flag and an overall deadline are checked between reads. WinHttpCloseHandle is never
//   called from another thread (not allowed for synchronous requests).

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>

namespace upd::http {

enum class Error {
    None,
    Unavailable, // winhttp.dll missing or too old
    Offline,     // name not resolved, can't connect
    Timeout,     // a step timed out, or the overall deadline passed
    Tls,         // secure channel failure
    NotFound,    // 404
    RateLimited, // 403 / 429 (retryAfterSec may be set)
    ServerError, // 5xx
    Redirect,    // a redirect to a host that isn't allowed, or too many
    TooLarge,    // more bytes than allowed
    BadResponse, // anything else unexpected
    Canceled,
    Sink,        // the consumer refused the data (disk full, write error)
};

QString errorCode(Error error); // "offline", "timeout", ... (state.ini, logs)

struct Request {
    QUrl   url;
    qint64 maxBytes   = 64 * 1024;
    qint64 deadlineMs = 60 * 1000; // overall, all hops and reads
    const std::atomic_bool* cancel = nullptr;
    // Receives the body in chunks; return false to stop (Error::Sink). Without it the body is
    // collected in Response::body.
    std::function<bool(const char* data, qint64 size)> sink;
    // Bytes received so far (called at most every 100 ms and once at the end).
    std::function<void(qint64 received)> progress;
};

struct Response {
    Error         error         = Error::None;
    int           status        = 0;
    qint64        retryAfterSec = 0; // clamped to 24 h
    unsigned long winError      = 0; // the WinHTTP error code, for the log
    QString       finalHost;         // where the body came from (logged; no IP addresses)
    QByteArray    body;              // only without a sink
    qint64        received = 0;
};

Response get(const Request& request);

} // namespace upd::http
