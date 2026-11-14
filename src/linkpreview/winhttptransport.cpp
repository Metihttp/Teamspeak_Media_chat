// 2.4 link previews: the real transport (transport.h). WinHTTP in asynchronous mode: the worker thread
// starts each step and waits for its completion OR the cancel event, so a cancel closes the request at
// once instead of waiting for a stalled website. Every request's memory lives until WinHTTP reports
// its handle closed (WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING is always the last callback of a handle).
//
// Address checks: ws2_32's GetAddrInfoExW (asynchronous and cancelable where Windows supports it) lists
// every address of a host first; then WinHTTP reports the address it connects to (CONNECTING_TO_SERVER,
// CONNECTED_TO_SERVER: refused ones close the request before the TLS handshake and the request) and
// WINHTTP_OPTION_CONNECTION_INFO tells where the response came from.
//
// The Windows proxy (netpolicy.h): route() reads the user's settings (WinHttpGetIEProxyConfigForCurrentUser;
// an auto-config script or WPAD, as Windows does: first the automatic configuration, then the manual proxy and its
// bypass list). A request through a proxy skips the address checks above: the proxy resolves the name and connects
// (local DNS may be filtered); the peer WinHTTP reports is the proxy.
//
// 2.6: on Windows 8 and later a setup script's answer is read with WinHttpGetProxyForUrlEx / WinHttpGetProxyResult,
// which keep each entry's scheme: a SOCKS-only answer is refused (W1, S2), never read as DIRECT. The chat proxy's
// bridge (proxy/bridge.h) is a named proxy at exactly 127.0.0.1:<port>, pre-authenticated with WinHttpSetCredentials
// (research_proxy A3), its peer pinned to 127.0.0.1.

#include "transport.h"

#include "proxy/winsock.h" // winsock2.h before windows.h

#include <winhttp.h>

#include <QDateTime>
#include <QMutex>

#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "netpolicy.h"
#include "proxy/router.h"
#include "proxy/windowsproxy.h"

#ifndef WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE
#define WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE 104
#endif

namespace linkpreview::http {

namespace {

// 2.5: the honest user agent (net::userAgent()) unless HopOptions names one.
std::wstring userAgentOf(const QByteArray& named)
{
    return QString::fromLatin1(named.isEmpty() ? net::userAgent() : named).toStdWString();
}
constexpr DWORD   kMaxHeaderBytes   = 16 * 1024;
constexpr int     kReadChunk        = 64 * 1024;
constexpr DWORD   kCloseWaitMs      = 15 * 1000; // for WinHTTP / Winsock to let go after a close or cancel

std::atomic_bool g_mustStayLoaded{false};
std::atomic<quint64> g_sessionsOpened{0};
std::atomic_bool     g_anyLoopbackBridge{false};

// ---- system libraries (loaded once from System32, never unloaded) ---------------------------------------

template <typename T>
void resolveProc(HMODULE dll, T& fn, const char* name)
{
    fn = dll ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name))) : nullptr;
}

struct WinHttpApi {
    HMODULE dll = nullptr;
    decltype(&::WinHttpOpen)              open              = nullptr;
    decltype(&::WinHttpCloseHandle)       close             = nullptr;
    decltype(&::WinHttpConnect)           connect           = nullptr;
    decltype(&::WinHttpOpenRequest)       openRequest       = nullptr;
    decltype(&::WinHttpSendRequest)       send              = nullptr;
    decltype(&::WinHttpReceiveResponse)   receive           = nullptr;
    decltype(&::WinHttpQueryHeaders)      query             = nullptr;
    decltype(&::WinHttpReadData)          read              = nullptr;
    decltype(&::WinHttpSetOption)         setOption         = nullptr;
    decltype(&::WinHttpQueryOption)       queryOption       = nullptr;
    decltype(&::WinHttpSetTimeouts)       setTimeouts       = nullptr;
    decltype(&::WinHttpSetStatusCallback) setStatusCallback = nullptr;
    // The Windows proxy (optional: without them every request goes direct)
    decltype(&::WinHttpGetIEProxyConfigForCurrentUser) ieProxyConfig = nullptr;
    decltype(&::WinHttpGetProxyForUrl)                 proxyForUrl   = nullptr;
    // 2.6
    decltype(&::WinHttpSetCredentials)       setCredentials      = nullptr; // the bridge's pre-authentication
    decltype(&::WinHttpCreateProxyResolver)  createProxyResolver = nullptr; // Windows 8+
    decltype(&::WinHttpGetProxyForUrlEx)     proxyForUrlEx       = nullptr;
    decltype(&::WinHttpGetProxyResult)       proxyResult         = nullptr;
    decltype(&::WinHttpFreeProxyResult)      freeProxyResult     = nullptr;

    WinHttpApi()
    {
        dll = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        resolveProc(dll, open, "WinHttpOpen");
        resolveProc(dll, close, "WinHttpCloseHandle");
        resolveProc(dll, connect, "WinHttpConnect");
        resolveProc(dll, openRequest, "WinHttpOpenRequest");
        resolveProc(dll, send, "WinHttpSendRequest");
        resolveProc(dll, receive, "WinHttpReceiveResponse");
        resolveProc(dll, query, "WinHttpQueryHeaders");
        resolveProc(dll, read, "WinHttpReadData");
        resolveProc(dll, setOption, "WinHttpSetOption");
        resolveProc(dll, queryOption, "WinHttpQueryOption");
        resolveProc(dll, setTimeouts, "WinHttpSetTimeouts");
        resolveProc(dll, setStatusCallback, "WinHttpSetStatusCallback");
        resolveProc(dll, ieProxyConfig, "WinHttpGetIEProxyConfigForCurrentUser");
        resolveProc(dll, proxyForUrl, "WinHttpGetProxyForUrl");
        resolveProc(dll, setCredentials, "WinHttpSetCredentials");
        resolveProc(dll, createProxyResolver, "WinHttpCreateProxyResolver");
        resolveProc(dll, proxyForUrlEx, "WinHttpGetProxyForUrlEx");
        resolveProc(dll, proxyResult, "WinHttpGetProxyResult");
        resolveProc(dll, freeProxyResult, "WinHttpFreeProxyResult");
    }
    bool resolverEx() const { return createProxyResolver && proxyForUrlEx && proxyResult && freeProxyResult; }
    // Every WinHttpOpen of the transport goes through here (tests count them).
    HINTERNET openSession(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags) const
    {
        ++g_sessionsOpened;
        return open(agent, access, proxy, bypass, flags);
    }
    bool ok() const
    {
        return dll && open && close && connect && openRequest && send && receive && query && read && setOption && queryOption && setTimeouts
               && setStatusCallback;
    }
};

// Function-local statics: built on the first fetch (thread-safe), the DLLs stay loaded for the process.
const WinHttpApi& winHttp()
{
    static WinHttpApi* api = new WinHttpApi; // never deleted: nothing of it runs at unload
    return *api;
}

net::Address fromSockaddr(const sockaddr* address)
{
    return proxy::ws::fromSockaddr(address);
}

bool isBridgePeer(const net::Address& address)
{
    if (address.family != net::Address::Family::IPv4)
        return false;
    const quint32 v = address.ipv4();
    if (g_anyLoopbackBridge.load())
        return (v >> 24) == 127; // tests: the string may name 127.0.0.2; the pin below still decides
    return v == 0x7F000001u;
}

Error mapError(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_RESEND_REQUEST:
        return Error::Offline;
    case ERROR_WINHTTP_TIMEOUT:
        return Error::Timeout;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return Error::Tls;
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return Error::Canceled;
    default:
        return Error::BadResponse;
    }
}

// ---- one request ---------------------------------------------------------------------------------------------

enum class Op : int { None = 0, Sent, Headers, Read, Failed, Closed };

// Shared with WinHTTP's callback threads. Owned by the Exchange; freed only after the request handle's
// HANDLE_CLOSING (or never, when that doesn't come in time).
struct Context {
    const WinHttpApi* api = nullptr;
    HANDLE            opEvent       = nullptr; // auto-reset: an operation completed (op says which)
    HANDLE            requestClosed = nullptr; // manual-reset: HANDLE_CLOSING of the request
    HINTERNET         session       = nullptr;
    HINTERNET         connection    = nullptr;
    std::atomic<HINTERNET> request{nullptr};   // cleared by whoever closes it first
    std::atomic<HINTERNET> requestRaw{nullptr}; // the request's handle value, kept to recognize its callbacks
    std::atomic<int>   op{0};
    std::atomic<DWORD> bytes{0};
    std::atomic<DWORD> error{0};
    std::atomic_bool   blocked{false};
    bool               viaProxy = false;       // the peer is the user's Windows proxy: not checked
    bool               bridge   = false;       // 2.6: the peer must be 127.0.0.1 (the bridge), nothing else
    std::atomic_bool   connected{false};       // CONNECTED_TO_SERVER seen
    std::atomic<DWORD> secureFlags{0};         // SECURE_FAILURE's flags
    std::function<bool(const net::Address&)> allowPeer; // set before the request starts, then read only
    QMutex             peerLock;
    net::Address       peer;                   // CONNECTED_TO_SERVER's address (a fallback for head())
    std::vector<char>  buffer;                 // WinHttpReadData writes here (owned here: alive until closed)
    std::wstring       headers;                // 2.5: the additional request headers (Accept-Encoding), alive until closed

    ~Context()
    {
        if (opEvent)
            CloseHandle(opEvent);
        if (requestClosed)
            CloseHandle(requestClosed);
    }

    void closeRequest()
    {
        if (HINTERNET handle = request.exchange(nullptr))
            api->close(handle);
    }
    void complete(Op which, DWORD count = 0, DWORD code = 0)
    {
        bytes.store(count);
        error.store(code);
        op.store(static_cast<int>(which));
        SetEvent(opEvent);
    }
};

net::Address connectionPeer(const WinHttpApi& api, HINTERNET request)
{
    WINHTTP_CONNECTION_INFO info{};
    info.cbSize = sizeof(info);
    DWORD size  = sizeof(info);
    if (!request || !api.queryOption(request, WINHTTP_OPTION_CONNECTION_INFO, &info, &size))
        return {};
    return fromSockaddr(reinterpret_cast<const sockaddr*>(&info.RemoteAddress));
}

void CALLBACK onStatus(HINTERNET handle, DWORD_PTR contextValue, DWORD status, LPVOID info, DWORD length)
{
    auto* ctx = reinterpret_cast<Context*>(contextValue);
    if (!ctx)
        return; // the session and connection handles carry no context
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER: {
        net::Address address;
        if (status == WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER && handle == ctx->requestRaw.load())
            address = connectionPeer(*ctx->api, handle);
        if (address.isNull() && info && length > 0)
            address = net::parseAddress(QString::fromWCharArray(static_cast<const wchar_t*>(info), static_cast<int>(wcsnlen(static_cast<const wchar_t*>(info), 64))));
        if (status == WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER && !address.isNull()) {
            QMutexLocker lock(&ctx->peerLock);
            ctx->peer = address;
        }
        // Unknown or refused: closed now, before the TLS handshake and before the request is sent. The bridge's peer is
        // pinned to 127.0.0.1 (an accidental direct connection is closed here, S1).
        bool refused = false;
        if (ctx->bridge)
            refused = address.isNull() || address != net::Address::fromIPv4(0x7F000001u) || (ctx->allowPeer && !ctx->allowPeer(address));
        else if (!ctx->viaProxy)
            refused = address.isNull() || !ctx->allowPeer || !ctx->allowPeer(address);
        if (refused) {
            ctx->blocked.store(true);
            ctx->closeRequest();
        } else if (status == WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER) {
            ctx->connected.store(true);
        }
        return;
    }
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
        if (info && length >= sizeof(DWORD))
            ctx->secureFlags.store(ctx->secureFlags.load() | *static_cast<const DWORD*>(info));
        return;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        ctx->complete(Op::Sent);
        return;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        ctx->complete(Op::Headers);
        return;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        ctx->complete(Op::Read, length);
        return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        ctx->complete(Op::Failed, 0, result ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
        return;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        if (handle == ctx->requestRaw.load()) {
            ctx->complete(Op::Closed);
            SetEvent(ctx->requestClosed); // the last touch of ctx from here: it may be freed right after
        }
        return;
    default:
        return;
    }
}

bool queryText(const WinHttpApi& api, HINTERNET request, DWORD info, QString* out, DWORD maxBytes = kMaxHeaderBytes)
{
    DWORD size = 0;
    api.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > maxBytes)
        return false;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
    if (!api.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return false;
    *out = QString::fromWCharArray(buffer.data(), static_cast<int>(size / sizeof(wchar_t)));
    return true;
}

// A header by name (WINHTTP_QUERY_CUSTOM); false when absent or too long.
bool queryCustom(const WinHttpApi& api, HINTERNET request, const wchar_t* name, QString* out)
{
    out->clear();
    DWORD size = 0;
    api.query(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 512)
        return false;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
    if (!api.query(request, WINHTTP_QUERY_CUSTOM, name, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return false;
    *out = QString::fromWCharArray(buffer.data(), static_cast<int>(size / sizeof(wchar_t)));
    return true;
}

class WinHttpExchange : public Exchange
{
  public:
    WinHttpExchange(Context* ctx, const Cancel& cancel, const HopOptions& options)
        : m_ctx(ctx)
        , m_cancel(cancel)
        , m_deadline(options.deadline)
        , m_receiveMs(options.receiveTimeoutMs)
    {
    }

    ~WinHttpExchange() override
    {
        const WinHttpApi& api = *m_ctx->api;
        m_ctx->closeRequest();
        bool released = true;
        if (m_ctx->requestRaw.load())
            released = WaitForSingleObject(m_ctx->requestClosed, kCloseWaitMs) == WAIT_OBJECT_0;
        // No more callbacks wanted for the connection and the session (they have no context anyway).
        if (m_ctx->connection) {
            api.setStatusCallback(m_ctx->connection, nullptr, 0, 0);
            api.close(m_ctx->connection);
        }
        if (m_ctx->session) {
            api.setStatusCallback(m_ctx->session, nullptr, 0, 0);
            api.close(m_ctx->session);
        }
        if (released) {
            delete m_ctx;
        } else {
            // WinHTTP still holds the request: its memory stays (leaked), and so does the plugin DLL.
            setMustStayLoaded();
        }
    }

    // Sends the request and waits for the headers.
    Error start()
    {
        HINTERNET request = m_ctx->request.load();
        if (!request)
            return m_ctx->blocked ? Error::Blocked : Error::Canceled;
        ResetEvent(m_ctx->opEvent);
        const wchar_t* headers = m_ctx->headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : m_ctx->headers.c_str();
        const DWORD    length  = m_ctx->headers.empty() ? 0 : static_cast<DWORD>(-1L);
        if (!m_ctx->api->send(request, headers, length, WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(m_ctx))) {
            m_error = GetLastError();
            return m_ctx->blocked ? Error::Blocked : mapError(m_error);
        }
        Error error = wait(Op::Sent, m_receiveMs, nullptr);
        if (error != Error::None)
            return error;
        request = m_ctx->request.load();
        if (!request)
            return m_ctx->blocked ? Error::Blocked : Error::Canceled;
        ResetEvent(m_ctx->opEvent);
        if (!m_ctx->api->receive(request, nullptr)) {
            m_error = GetLastError();
            return m_ctx->blocked ? Error::Blocked : mapError(m_error);
        }
        return wait(Op::Headers, m_receiveMs, nullptr);
    }

    Error head(Head* out) override
    {
        if (m_started == Error::None && !m_headDone) {
            m_started  = start();
            m_headDone = true;
        }
        if (m_started != Error::None)
            return m_started;
        HINTERNET request = m_ctx->request.load();
        if (!request)
            return m_ctx->blocked ? Error::Blocked : Error::Canceled;
        const WinHttpApi& api = *m_ctx->api;

        DWORD status = 0;
        DWORD size   = sizeof(status);
        if (!api.query(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                       WINHTTP_NO_HEADER_INDEX)) {
            m_error = GetLastError();
            return Error::BadResponse;
        }
        Head head;
        head.status = static_cast<int>(status);
        QString text;
        if (queryText(api, request, WINHTTP_QUERY_CONTENT_TYPE, &text, 512))
            head.contentType = text.left(256).toLatin1();
        if (queryText(api, request, WINHTTP_QUERY_CONTENT_LENGTH, &text, 64)) {
            bool         ok     = false;
            const qint64 length = text.trimmed().toLongLong(&ok);
            head.contentLength  = ok && length >= 0 ? length : -1;
        }
        if (queryText(api, request, WINHTTP_QUERY_LOCATION, &text, 3 * 8192))
            head.location = text.left(8192);
        // 2.5: what the response says about itself and about the site's limits.
        if (queryText(api, request, WINHTTP_QUERY_CONTENT_ENCODING, &text, 256))
            head.contentEncoding = text.trimmed().toLower().left(64).toLatin1();
        const qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
        if (queryText(api, request, WINHTTP_QUERY_RETRY_AFTER, &text, 256))
            head.retryAfterMs = net::parseRetryAfter(text.toLatin1(), wallMs);
        if (queryCustom(api, request, L"cf-mitigated", &text))
            head.challenge = text.trimmed().compare(QLatin1String("challenge"), Qt::CaseInsensitive) == 0;
        if (queryCustom(api, request, L"x-ratelimit-remaining", &text)) {
            bool         ok        = false;
            const double remaining = text.trimmed().toDouble(&ok);
            if (ok && remaining >= 0 && remaining < 1e12)
                head.rateRemaining = remaining;
        }
        QString reset, resetAfter;
        queryCustom(api, request, L"x-ratelimit-reset", &reset);
        queryCustom(api, request, L"x-ratelimit-reset-after", &resetAfter);
        head.rateResetMs = net::parseRateReset(resetAfter.toLatin1(), reset.toLatin1(), wallMs);
        if (head.status == 429 && head.retryAfterMs < 0 && head.rateResetMs > 0)
            head.retryAfterMs = head.rateResetMs;
        head.peer = connectionPeer(api, request);
        if (head.peer.isNull()) {
            QMutexLocker lock(&m_ctx->peerLock);
            head.peer = m_ctx->peer;
        }
        // 2.6: did the proxy answer the CONNECT itself (S3)?
        BOOL  fromProxy = FALSE;
        DWORD flagSize  = sizeof(fromProxy);
        if (api.queryOption(request, WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE, &fromProxy, &flagSize))
            head.proxyConnectResponse = fromProxy != FALSE;
        if (queryCustom(api, request, L"X-TSMedia-Proxy", &text))
            head.proxyHeader = text.trimmed().left(40);
        *out = head;
        return Error::None;
    }

    Error read(char* buffer, int capacity, int* got) override
    {
        *got              = 0;
        HINTERNET request = m_ctx->request.load();
        if (!request)
            return m_ctx->blocked ? Error::Blocked : Error::Canceled;
        const DWORD want = static_cast<DWORD>(qBound(1, capacity, kReadChunk));
        ResetEvent(m_ctx->opEvent);
        if (!m_ctx->api->read(request, m_ctx->buffer.data(), want, nullptr)) {
            m_error = GetLastError();
            return mapError(m_error);
        }
        DWORD       count = 0;
        const Error error = wait(Op::Read, m_receiveMs, &count);
        if (error != Error::None)
            return error;
        count = qMin(count, want);
        memcpy(buffer, m_ctx->buffer.data(), count);
        *got = static_cast<int>(count);
        return Error::None;
    }

    unsigned long systemError() const override { return m_error; }
    bool          connectedToServer() const override { return m_ctx->connected.load(); }
    unsigned long secureFailureFlags() const override { return m_ctx->secureFlags.load(); }

  private:
    // Waits for the operation's completion, the cancel event, the step limit or the deadline.
    Error wait(Op expected, int stepMs, DWORD* count)
    {
        HANDLE handles[2] = {m_ctx->opEvent, static_cast<HANDLE>(m_cancel.event())};
        for (;;) {
            const int left = qMin(msUntil(m_deadline), stepMs);
            if (left <= 0)
                return Error::Timeout;
            const DWORD result = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(left));
            if (result == WAIT_OBJECT_0 + 1)
                return Error::Canceled;
            if (result == WAIT_TIMEOUT)
                return Error::Timeout;
            if (result != WAIT_OBJECT_0)
                return Error::BadResponse;
            const Op op = static_cast<Op>(m_ctx->op.exchange(static_cast<int>(Op::None)));
            if (m_ctx->blocked)
                return Error::Blocked;
            if (op == expected) {
                if (count)
                    *count = m_ctx->bytes.load();
                return Error::None;
            }
            if (op == Op::Failed) {
                m_error = m_ctx->error.load();
                return mapError(m_error);
            }
            if (op == Op::Closed)
                return Error::Canceled;
            // Anything else: not ours to wait for, keep waiting.
        }
    }

    Context*      m_ctx;
    const Cancel& m_cancel;
    quint64       m_deadline;
    int           m_receiveMs;
    bool          m_headDone = false;
    Error         m_started  = Error::None;
    unsigned long m_error    = 0;
};

class WinHttpTransport : public Transport
{
  public:
    Error resolve(const QString& host, QVector<net::Address>* out, const Cancel& cancel, quint64 deadline) override;
    std::unique_ptr<Exchange> open(const HopOptions& options, const Cancel& cancel, Error* error) override;
    Route route(const QUrl& url, const Cancel& cancel, quint64 deadline) override;
};

// ---- the Windows proxy -----------------------------------------------------------------------------------

// WPAD that found nothing is not asked again for a while (it can take seconds on every request).
std::atomic<quint64> g_wpadFailedUntil{0};
constexpr quint64    kWpadRetryMs    = 10 * 60 * 1000;
constexpr int        kAutoProxyMaxMs = 8 * 1000;

QString takeGlobalString(LPWSTR text)
{
    if (!text)
        return {};
    const QString out = QString::fromWCharArray(text, static_cast<int>(wcsnlen(text, net::kMaxProxyChars)));
    GlobalFree(text);
    return out;
}

// The automatic configuration's answer for url: true with *list (empty: DIRECT) when it decided, false when
// there is none or it failed (the manual settings apply then).
bool autoProxy(const WinHttpApi& api, const QUrl& url, LPCWSTR configUrl, bool detect, quint64 deadline, QString* list, QString* bypass)
{
    WINHTTP_AUTOPROXY_OPTIONS options{};
    if (configUrl) {
        options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
        options.lpszAutoConfigUrl = configUrl;
    }
    if (detect && nowMs() >= g_wpadFailedUntil.load()) {
        options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
        options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    }
    // The script learns the host only (netpolicy.h proxyQueryUrl), never the path or query.
    const QUrl query = net::proxyQueryUrl(url);
    if (options.dwFlags == 0 || !api.proxyForUrl || !query.isValid())
        return false;
    options.fAutoLogonIfChallenged = FALSE; // never the Windows account's credentials
    const std::wstring userAgent = userAgentOf(QByteArray());
    HINTERNET          session   = api.openSession(userAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    const int wait = qBound(1000, qMin(kAutoProxyMaxMs, msUntil(deadline)), kAutoProxyMaxMs);
    api.setTimeouts(session, wait, wait, wait, wait);
    const std::wstring target = query.toString(QUrl::FullyEncoded).toStdWString();
    WINHTTP_PROXY_INFO info{};
    const BOOL         ok    = api.proxyForUrl(session, target.c_str(), &options, &info);
    const DWORD        error = ok ? 0 : GetLastError();
    api.close(session);
    if (!ok) {
        if (error == ERROR_WINHTTP_AUTODETECTION_FAILED && (options.dwFlags & WINHTTP_AUTOPROXY_AUTO_DETECT))
            g_wpadFailedUntil.store(nowMs() + kWpadRetryMs);
        return false;
    }
    const QString proxies = takeGlobalString(info.lpszProxy);
    const QString skip    = takeGlobalString(info.lpszProxyBypass);
    *list                 = info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? proxies : QString();
    *bypass               = skip;
    return true;
}

// ---- the Windows proxy, Windows 8+: WinHttpGetProxyForUrlEx keeps every entry's scheme (S2) --------------------------

struct ResolverContext { // on the heap; freed after the resolver's HANDLE_CLOSING (leaked if that never comes)
    HANDLE             done   = nullptr; // manual reset: GETPROXYFORURL_COMPLETE or REQUEST_ERROR
    HANDLE             closed = nullptr; // manual reset: HANDLE_CLOSING of the resolver
    std::atomic<HINTERNET> resolver{nullptr};
    std::atomic<DWORD> error{0};
    ~ResolverContext()
    {
        if (done)
            CloseHandle(done);
        if (closed)
            CloseHandle(closed);
    }
};

void CALLBACK onResolverStatus(HINTERNET handle, DWORD_PTR contextValue, DWORD status, LPVOID info, DWORD length)
{
    Q_UNUSED(length);
    auto* ctx = reinterpret_cast<ResolverContext*>(contextValue);
    if (!ctx)
        return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE:
        ctx->error.store(0);
        SetEvent(ctx->done);
        return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        ctx->error.store(result ? result->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
        SetEvent(ctx->done);
        return;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        if (handle == ctx->resolver.load())
            SetEvent(ctx->closed); // the last touch of ctx from here
        return;
    default:
        return;
    }
}

// TimedOut: no answer in time (review fix: slow, not failed; a configured setup script then refuses the hop instead of
// falling back to the manual setting or direct, D8).
enum class ScriptOutcome { Decided, Failed, Canceled, TimedOut };

// The setup script's (or WPAD's) answer for url as entries with their schemes.
ScriptOutcome scriptAnswerEx(const WinHttpApi& api, const QUrl& url, const proxy::windowsproxy::IeConfig& config, const Cancel& cancel,
                             quint64 deadline, QVector<proxy::windowsproxy::Entry>* entries)
{
    WINHTTP_AUTOPROXY_OPTIONS options{};
    const std::wstring        configUrl = config.autoConfigUrl.toStdWString();
    if (!config.autoConfigUrl.isEmpty()) {
        options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
        options.lpszAutoConfigUrl = configUrl.c_str();
    }
    if (config.autoDetect && nowMs() >= g_wpadFailedUntil.load()) {
        options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
        options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    }
    const QUrl query = net::proxyQueryUrl(url); // the host only, never the path or query
    if (options.dwFlags == 0 || !query.isValid())
        return ScriptOutcome::Failed;
    options.fAutoLogonIfChallenged = FALSE; // never the Windows account's credentials
    const std::wstring userAgent   = userAgentOf(QByteArray());
    HINTERNET session = api.openSession(userAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session)
        return ScriptOutcome::Failed;
    auto* ctx   = new ResolverContext;
    ctx->done   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ctx->closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto closeSession = [&]() {
        api.setStatusCallback(session, nullptr, 0, 0);
        api.close(session);
    };
    if (!ctx->done || !ctx->closed
        || api.setStatusCallback(session, &onResolverStatus, WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_GETPROXYFORURL_COMPLETE | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
               == WINHTTP_INVALID_STATUS_CALLBACK) {
        closeSession();
        delete ctx;
        return ScriptOutcome::Failed;
    }
    HINTERNET resolver = nullptr;
    if (api.createProxyResolver(session, &resolver) != ERROR_SUCCESS || !resolver) {
        closeSession();
        delete ctx;
        return ScriptOutcome::Failed;
    }
    ctx->resolver.store(resolver);
    DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(ctx);
    api.setOption(resolver, WINHTTP_OPTION_CONTEXT_VALUE, &contextValue, sizeof(contextValue)); // HANDLE_CLOSING finds ctx
    const std::wstring target = query.toString(QUrl::FullyEncoded).toStdWString();
    const DWORD        rc     = api.proxyForUrlEx(resolver, target.c_str(), &options, contextValue);
    ScriptOutcome      outcome = ScriptOutcome::Failed;
    if (rc == ERROR_IO_PENDING) {
        HANDLE      handles[2] = {ctx->done, static_cast<HANDLE>(cancel.event())};
        const int   wait       = qBound(1000, qMin(kAutoProxyMaxMs, msUntil(deadline)), kAutoProxyMaxMs);
        const DWORD r          = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(wait));
        if (r == WAIT_OBJECT_0 + 1) {
            outcome = ScriptOutcome::Canceled;
        } else if (r == WAIT_TIMEOUT) {
            outcome = ScriptOutcome::TimedOut;
        } else if (r == WAIT_OBJECT_0) {
            const DWORD error = ctx->error.load();
            if (error == 0) {
                WINHTTP_PROXY_RESULT result{};
                if (api.proxyResult(resolver, &result) == ERROR_SUCCESS) {
                    for (DWORD i = 0; i < result.cEntries && entries->size() < 16; ++i) {
                        const WINHTTP_PROXY_RESULT_ENTRY& e = result.pEntries[i];
                        proxy::windowsproxy::Entry        entry;
                        entry.proxy = e.fProxy != FALSE;
                        switch (e.ProxyScheme) {
                        case INTERNET_SCHEME_HTTP:
                            entry.scheme = proxy::windowsproxy::Entry::Scheme::Http;
                            break;
                        case INTERNET_SCHEME_HTTPS:
                            entry.scheme = proxy::windowsproxy::Entry::Scheme::Https;
                            break;
                        case INTERNET_SCHEME_SOCKS:
                            entry.scheme = proxy::windowsproxy::Entry::Scheme::Socks;
                            break;
                        default:
                            entry.scheme = proxy::windowsproxy::Entry::Scheme::Other;
                            break;
                        }
                        if (e.pwszProxy)
                            entry.host = QString::fromWCharArray(e.pwszProxy, static_cast<int>(wcsnlen(e.pwszProxy, 256)));
                        entry.port = e.ProxyPort;
                        entries->append(entry);
                    }
                    api.freeProxyResult(&result);
                    outcome = ScriptOutcome::Decided;
                }
            } else if (error == ERROR_WINHTTP_AUTODETECTION_FAILED && (options.dwFlags & WINHTTP_AUTOPROXY_AUTO_DETECT)) {
                g_wpadFailedUntil.store(nowMs() + kWpadRetryMs);
            } else if (error == ERROR_WINHTTP_TIMEOUT) {
                outcome = ScriptOutcome::TimedOut; // WinHTTP's own time-out reading the script: slow, not failed
            }
        }
    } else if (rc == ERROR_WINHTTP_AUTODETECTION_FAILED && (options.dwFlags & WINHTTP_AUTOPROXY_AUTO_DETECT)) {
        g_wpadFailedUntil.store(nowMs() + kWpadRetryMs);
    }
    // Closing the resolver cancels a pending lookup; its HANDLE_CLOSING is the last callback that touches ctx.
    api.close(resolver);
    const bool released = WaitForSingleObject(ctx->closed, kCloseWaitMs) == WAIT_OBJECT_0;
    closeSession();
    if (released)
        delete ctx;
    else
        setMustStayLoaded(); // WinHTTP may still call back into ctx: it stays, and so does the DLL
    return outcome;
}

Route WinHttpTransport::route(const QUrl& url, const Cancel& cancel, quint64 deadline)
{
    Route             route; // Unset: refused (never direct by default, F1)
    const WinHttpApi& api = winHttp();
    if (!api.ok() || cancel.isSet())
        return route;
    const proxy::windowsproxy::IeConfig config = proxy::windowsproxy::ieConfig();
    if (!config.ok) {
        route.via = Route::Via::Direct; // no settings: direct (2.5.0, now explicit)
        return route;
    }
    const QString host = url.host(QUrl::FullyEncoded).toLower();
    QString       manual = config.proxy, manualBypass = config.bypass;
    if (config.autoDetect || !config.autoConfigUrl.isEmpty()) {
        if (api.resolverEx()) {
            QVector<proxy::windowsproxy::Entry> entries;
            const ScriptOutcome                 outcome = scriptAnswerEx(api, url, config, cancel, deadline, &entries);
            if (outcome == ScriptOutcome::Canceled || cancel.isSet())
                return route;
            if (outcome == ScriptOutcome::Decided) {
                const proxy::windowsproxy::Mapped mapped = proxy::windowsproxy::mapProxyResult(entries);
                switch (mapped.decision) {
                case proxy::windowsproxy::Decision::Proxy:
                    route.via   = Route::Via::System;
                    route.proxy = mapped.proxies;
                    break;
                case proxy::windowsproxy::Decision::Direct:
                    route.via = Route::Via::Direct;
                    break;
                case proxy::windowsproxy::Decision::Refused:
                    route.via     = Route::Via::Refused;
                    route.refused = mapped.refused;
                    break;
                }
                return route;
            }
            if (outcome == ScriptOutcome::TimedOut && !config.autoConfigUrl.isEmpty()) {
                // Review fix (D8): the setup script didn't answer in time. Its answer might have been a proxy (or SOCKS,
                // refused): going on with the manual setting, usually direct with local DNS, would be a fail-open on a
                // slow script. Refused, nothing looked up or sent; the next load asks the script again.
                route.via     = Route::Via::Refused;
                route.refused = proxy::Failure::Slow;
                return route;
            }
            // no script, a failed download, detection found nothing (or detection alone was slow): the manual settings
            // apply (2.5)
        } else {
            // Windows 7: 2.5's WinHttpGetProxyForUrl (a SOCKS answer reads as DIRECT there: a documented gap)
            QString           list, bypass;
            const std::wstring configUrl = config.autoConfigUrl.toStdWString();
            if (autoProxy(api, url, config.autoConfigUrl.isEmpty() ? nullptr : configUrl.c_str(), config.autoDetect, deadline, &list, &bypass)) {
                manual       = list;
                manualBypass = bypass;
                if (list.isEmpty()) {
                    route.via = Route::Via::Direct;
                    return route;
                }
            }
        }
    }
    const net::PickedProxy picked = net::pickProxy(manual, manualBypass, host);
    if (!picked.servers.isEmpty()) {
        route.via   = Route::Via::System;
        route.proxy = picked.servers;
    } else if (picked.socksOnly) {
        route.via     = Route::Via::Refused; // W1: never direct
        route.refused = proxy::Failure::SocksOnlyWindows;
    } else {
        route.via = Route::Via::Direct;
    }
    return route;
}

// ---- name resolution -------------------------------------------------------------------------------------

Error WinHttpTransport::resolve(const QString& host, QVector<net::Address>* out, const Cancel& cancel, quint64 deadline)
{
    out->clear();
    HANDLE    cancelEvent = static_cast<HANDLE>(cancel.event());
    const int rc          = proxy::ws::lookup(host, out, &cancelEvent, cancelEvent ? 1 : 0, msUntil(deadline));
    switch (rc) {
    case 0:
        return Error::None;
    case 1:
        return Error::Offline;
    case 2:
        return cancel.isSet() ? Error::Canceled : Error::Timeout;
    default:
        return Error::Unavailable;
    }
}

// ---- requests -----------------------------------------------------------------------------------------------

std::unique_ptr<Exchange> WinHttpTransport::open(const HopOptions& options, const Cancel& cancel, Error* error)
{
    *error                = Error::None;
    const WinHttpApi& api = winHttp();
    if (!api.ok()) {
        *error = Error::Unavailable;
        return nullptr;
    }
    if (cancel.isSet()) {
        *error = Error::Canceled;
        return nullptr;
    }
    const QUrl& url = options.url;
    if (!net::isAllowedUrl(url)) {
        *error = Error::Blocked;
        return nullptr;
    }
    // 2.6 guards (S1, F1), before any session: nobody decided, refused, a proxied route without a proxy, a bridge that
    // isn't exactly 127.0.0.1:<port>; in custom mode (or before Control::start) nothing but the bridge; a Windows-proxy
    // route only in Windows mode. A hop that raced a switch fails here (its old-generation result is dropped anyway).
    const Route& route = options.route;
    if (route.via == Route::Via::Refused) {
        *error = Error::Proxy;
        return nullptr;
    }
    const proxy::Mode mode = proxy::snapshot()->mode;
    if (route.via == Route::Via::Unset || (route.viaProxy() && route.proxy.isEmpty())
        || (route.via == Route::Via::Bridge && !isBridgeProxyString(route.proxy))
        || ((mode == proxy::Mode::Custom || mode == proxy::Mode::Unset) && route.via != Route::Via::Bridge)
        || (mode != proxy::Mode::System && route.via == Route::Via::System)) {
        *error = Error::Blocked;
        return nullptr;
    }

    auto* ctx          = new Context;
    ctx->api           = &api;
    ctx->allowPeer     = options.allowPeer;
    ctx->viaProxy      = route.via == Route::Via::System;
    ctx->bridge        = route.via == Route::Via::Bridge;
    ctx->buffer.resize(kReadChunk);
    ctx->opEvent       = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ctx->requestClosed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto fail    = [&](Error e) -> std::unique_ptr<Exchange> {
        *error = e;
        // Nothing asynchronous has started yet unless the request exists: the Exchange cleans up then.
        if (ctx->requestRaw.load())
            return std::unique_ptr<Exchange>(new WinHttpExchange(ctx, cancel, options)); // closes on destruction
        // No callback of ours for these handles any more (as ~WinHttpExchange): a late HANDLE_CLOSING on a
        // system thread must not call into a DLL that the end of this thread may unload.
        if (ctx->connection) {
            api.setStatusCallback(ctx->connection, nullptr, 0, 0);
            api.close(ctx->connection);
        }
        if (ctx->session) {
            api.setStatusCallback(ctx->session, nullptr, 0, 0);
            api.close(ctx->session);
        }
        delete ctx;
        return nullptr;
    };
    if (!ctx->opEvent || !ctx->requestClosed)
        return fail(Error::Unavailable);

    const std::wstring userAgent = userAgentOf(options.userAgent);
    if (ctx->viaProxy || ctx->bridge) {
        // The user's Windows proxy, or the bridge: always a bare "host:port" list, never a scheme-mapped string (WinHTTP
        // would go direct for https, research_proxy M4/M5). The https request is tunneled; the certificate is still the
        // website's.
        const std::wstring proxy = route.proxy.left(net::kMaxProxyChars).toStdWString();
        ctx->session = api.openSession(userAgent.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    } else {
        ctx->session = api.openSession(userAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    }
    // 2.5: never compressed (WinHTTP's own decompression stays off too).
    const QByteArray encoding = options.acceptEncoding.isEmpty() ? QByteArray("identity") : options.acceptEncoding;
    ctx->headers = (QLatin1String("Accept-Encoding: ") + QString::fromLatin1(encoding.left(64)) + QLatin1String("\r\n")).toStdWString();
    if (!ctx->session)
        return fail(Error::Unavailable);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!api.setOption(ctx->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; // Windows without TLS 1.3
        api.setOption(ctx->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    api.setOption(ctx->session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    const DWORD notifications = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER
                                | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE;
    if (api.setStatusCallback(ctx->session, &onStatus, notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return fail(Error::Unavailable);

    const std::wstring host = url.host(QUrl::FullyEncoded).toLower().toStdWString();
    ctx->connection         = api.connect(ctx->session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!ctx->connection)
        return fail(Error::Offline);

    QString target = url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QString::fromLatin1("/");
    if (url.hasQuery())
        target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    const std::wstring path   = target.toStdWString();
    const std::wstring accept = QString::fromLatin1(options.accept.isEmpty() ? QByteArray("*/*") : options.accept).toStdWString();
    LPCWSTR            acceptTypes[] = {accept.c_str(), nullptr};
    HINTERNET request = api.openRequest(ctx->connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, acceptTypes, WINHTTP_FLAG_SECURE);
    if (!request)
        return fail(Error::Unavailable);
    DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(ctx);
    if (!api.setOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &contextValue, sizeof(contextValue))) {
        api.setStatusCallback(request, nullptr, 0, 0); // no context: nothing waits for its callbacks, none comes
        api.close(request);
        return fail(Error::Unavailable);
    }
    ctx->requestRaw.store(request);
    ctx->request.store(request);

    // The bridge: authentication stays on (or the credentials are never sent, research_proxy A3b); its token is set
    // before the send, so the first CONNECT carries it (pre-authentication, A3). A new request handle per attempt.
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_KEEP_ALIVE;
    if (!ctx->bridge)
        features |= WINHTTP_DISABLE_AUTHENTICATION;
    api.setOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    if (ctx->bridge) {
        const std::wstring user  = QString::fromLatin1(route.bridgeUser).toStdWString();
        const std::wstring token = QString::fromLatin1(route.bridgeToken).toStdWString();
        if (!api.setCredentials || !api.setCredentials(request, WINHTTP_AUTH_TARGET_PROXY, WINHTTP_AUTH_SCHEME_BASIC, user.c_str(), token.c_str(), nullptr))
            return fail(Error::Unavailable);
    }
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; // never the Windows account's credentials
    api.setOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
    DWORD headerLimit = 64 * 1024;
    api.setOption(request, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &headerLimit, sizeof(headerLimit)); // newer Windows only
    const int step = qBound(1000, qMin(options.connectTimeoutMs, msUntil(options.deadline)), qMax(1000, options.connectTimeoutMs));
    const int recv = qBound(1000, qMin(options.receiveTimeoutMs, msUntil(options.deadline)), qMax(1000, options.receiveTimeoutMs));
    api.setTimeouts(request, step, step, step, recv);

    return std::unique_ptr<Exchange>(new WinHttpExchange(ctx, cancel, options));
}

} // namespace

// ---- shared ---------------------------------------------------------------------------------------------------

QString errorName(Error error)
{
    const char* name = "unknown";
    switch (error) {
    case Error::None:
        name = "ok";
        break;
    case Error::Unavailable:
        name = "unavailable";
        break;
    case Error::Offline:
        name = "offline";
        break;
    case Error::Timeout:
        name = "timeout";
        break;
    case Error::Tls:
        name = "tls";
        break;
    case Error::Blocked:
        name = "blocked";
        break;
    case Error::Canceled:
        name = "canceled";
        break;
    case Error::BadResponse:
        name = "badResponse";
        break;
    case Error::Proxy:
        name = "proxy";
        break;
    }
    return QString::fromLatin1(name);
}

Cancel::Cancel()
    : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

Cancel::~Cancel()
{
    if (m_event)
        CloseHandle(static_cast<HANDLE>(m_event));
}

void Cancel::set()
{
    m_flag.store(true);
    if (m_event)
        SetEvent(static_cast<HANDLE>(m_event));
}

bool Cancel::wait(int ms) const
{
    if (isSet())
        return true;
    if (!m_event) {
        Sleep(static_cast<DWORD>(qMax(0, ms)));
        return isSet();
    }
    return WaitForSingleObject(static_cast<HANDLE>(m_event), static_cast<DWORD>(qMax(0, ms))) == WAIT_OBJECT_0 || isSet();
}

quint64 nowMs()
{
    return GetTickCount64();
}

int msUntil(quint64 deadline)
{
    const quint64 now = nowMs();
    if (deadline <= now)
        return 0;
    return static_cast<int>(qMin<quint64>(deadline - now, static_cast<quint64>(INT_MAX)));
}

std::shared_ptr<Transport> winHttpTransport()
{
    return std::make_shared<WinHttpTransport>();
}

bool mustStayLoaded()
{
    return g_mustStayLoaded.load();
}

void setMustStayLoaded()
{
    g_mustStayLoaded.store(true);
}

quint64 sessionsOpened()
{
    return g_sessionsOpened.load();
}

void setAnyLoopbackBridgeForTests(bool allow)
{
    g_anyLoopbackBridge.store(allow);
}

bool isBridgeProxyString(const QString& proxy)
{
    const int colon = proxy.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0)
        return false;
    const QString host = proxy.left(colon);
    const QString port = proxy.mid(colon + 1);
    if (port.isEmpty() || port.size() > 5)
        return false;
    for (const QChar ch : port) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return false;
    }
    if (port.size() > 1 && port.at(0) == QLatin1Char('0'))
        return false;
    const int value = port.toInt();
    if (value < 1024 || value > 65535)
        return false;
    if (host == QLatin1String("127.0.0.1"))
        return true;
    if (!g_anyLoopbackBridge.load())
        return false;
    const net::Address a = net::parseAddress(host);
    return isBridgePeer(a) && !host.contains(QLatin1Char(':'));
}

} // namespace linkpreview::http
