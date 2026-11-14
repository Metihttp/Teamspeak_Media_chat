#include "httpclient.h"

#include <windows.h>

#include <winhttp.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <QDateTime>

#include "linkpreview/netpolicy.h"
#include "proxy/router.h"
#include "proxy/windowsproxy.h"
#include "sessionplan.h"
#include "updatepolicy.h"

#ifndef WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE
#define WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE 104
#endif

namespace upd::http {

namespace {

constexpr int kResolveTimeoutMs = 8000;
constexpr int kConnectTimeoutMs = 8000;
constexpr int kSendTimeoutMs    = 8000;
constexpr int kReceiveTimeoutMs = 15000;
constexpr int kChunk            = 16 * 1024;
constexpr DWORD kMaxHeaderBytes = 16 * 1024;

// winhttp.dll entry points, resolved per request. Only the declarations come from winhttp.h: the
// plugin doesn't import winhttp.dll.
struct WinHttp {
    HMODULE dll = nullptr;
    decltype(&::WinHttpOpen)                            open        = nullptr;
    decltype(&::WinHttpCloseHandle)                     close       = nullptr;
    decltype(&::WinHttpConnect)                         connect     = nullptr;
    decltype(&::WinHttpOpenRequest)                     openRequest = nullptr;
    decltype(&::WinHttpSendRequest)                     send        = nullptr;
    decltype(&::WinHttpReceiveResponse)                 receive     = nullptr;
    decltype(&::WinHttpQueryHeaders)                    query       = nullptr;
    decltype(&::WinHttpQueryDataAvailable)              available   = nullptr;
    decltype(&::WinHttpReadData)                        read        = nullptr;
    decltype(&::WinHttpSetOption)                       setOption   = nullptr;
    decltype(&::WinHttpSetTimeouts)                     setTimeouts = nullptr;
    decltype(&::WinHttpGetIEProxyConfigForCurrentUser) ieProxy     = nullptr;
    decltype(&::WinHttpQueryOption)                     queryOption = nullptr; // 2.6
    decltype(&::WinHttpSetCredentials)                  setCredentials = nullptr; // 2.6: the bridge's token

    WinHttp()
    {
        dll = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
            return;
        resolve(open, "WinHttpOpen");
        resolve(close, "WinHttpCloseHandle");
        resolve(connect, "WinHttpConnect");
        resolve(openRequest, "WinHttpOpenRequest");
        resolve(send, "WinHttpSendRequest");
        resolve(receive, "WinHttpReceiveResponse");
        resolve(query, "WinHttpQueryHeaders");
        resolve(available, "WinHttpQueryDataAvailable");
        resolve(read, "WinHttpReadData");
        resolve(setOption, "WinHttpSetOption");
        resolve(setTimeouts, "WinHttpSetTimeouts");
        resolve(ieProxy, "WinHttpGetIEProxyConfigForCurrentUser");
        resolve(queryOption, "WinHttpQueryOption");
        resolve(setCredentials, "WinHttpSetCredentials");
    }
    ~WinHttp()
    {
        if (dll)
            FreeLibrary(dll);
    }
    WinHttp(const WinHttp&)            = delete;
    WinHttp& operator=(const WinHttp&) = delete;

    bool ok() const
    {
        return dll && open && close && connect && openRequest && send && receive && query && available && read && setOption && setTimeouts;
    }

  private:
    template <typename T>
    void resolve(T& fn, const char* name)
    {
        fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
    }
};

class Handle
{
  public:
    Handle(const WinHttp& api, HINTERNET h)
        : m_api(api)
        , m_h(h)
    {
    }
    ~Handle()
    {
        if (m_h)
            m_api.close(m_h);
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return m_h; }
    explicit  operator bool() const { return m_h != nullptr; }

  private:
    const WinHttp& m_api;
    HINTERNET      m_h;
};

Error mapError(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
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

bool queryString(const WinHttp& api, HINTERNET request, DWORD info, std::wstring* out)
{
    DWORD size = 0;
    api.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > kMaxHeaderBytes)
        return false;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
    if (!api.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return false;
    out->assign(buffer.data(), size / sizeof(wchar_t));
    return true;
}

// Retry-After as seconds (delta seconds or an HTTP date, the link previews' one parser), clamped to 24 h.
qint64 retryAfter(const WinHttp& api, HINTERNET request)
{
    std::wstring value;
    if (!queryString(api, request, WINHTTP_QUERY_RETRY_AFTER, &value))
        return 0;
    const qint64 ms = linkpreview::net::parseRetryAfter(QString::fromStdWString(value).toLatin1(), QDateTime::currentMSecsSinceEpoch());
    return ms < 0 ? 0 : qBound<qint64>(0, (ms + 999) / 1000, kMaxRetryAfterSec);
}

ULONGLONG nowMs()
{
    return GetTickCount64();
}

// The id of an updater request through the bridge (tsm-u<id>): process-wide, never 0.
std::atomic<quint64> g_bridgeRequestId{0};

// A session for plan (2.5.0's for the Windows settings).
HINTERNET openSession(const WinHttp& api, const SessionPlan& plan)
{
    switch (plan.access) {
    case SessionPlan::Access::NoProxy:
        return api.open(L"TSMedia-Updater/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    case SessionPlan::Access::Named: {
        const std::wstring proxy = plan.proxy.toStdWString(); // bare "127.0.0.1:<port>", never a mapped string
        return api.open(L"TSMedia-Updater/1", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    }
    case SessionPlan::Access::Automatic:
        break;
    }
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1+) uses the system's proxy settings and PAC
    // scripts. Older Windows: a static proxy from the user's settings, else the WinHTTP default.
    HINTERNET rawSession = api.open(L"TSMedia-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!rawSession) {
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
        if (api.ieProxy && api.ieProxy(&ie) && ie.lpszProxy && !ie.fAutoDetect && !ie.lpszAutoConfigUrl)
            rawSession = api.open(L"TSMedia-Updater/1", WINHTTP_ACCESS_TYPE_NAMED_PROXY, ie.lpszProxy, ie.lpszProxyBypass ? ie.lpszProxyBypass : WINHTTP_NO_PROXY_BYPASS, 0);
        if (ie.lpszProxy)
            GlobalFree(ie.lpszProxy);
        if (ie.lpszProxyBypass)
            GlobalFree(ie.lpszProxyBypass);
        if (ie.lpszAutoConfigUrl)
            GlobalFree(ie.lpszAutoConfigUrl);
        if (!rawSession)
            rawSession = api.open(L"TSMedia-Updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    return rawSession;
}

void configureSession(const WinHttp& api, HINTERNET session)
{
    // TLS 1.2 and 1.3; Windows versions without TLS 1.3 support reject the flag, so retry with 1.2.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!api.setOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        api.setOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    api.setOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
}

} // namespace

QString errorCode(Error error)
{
    const char* code = "unknown";
    switch (error) {
    case Error::None:
        code = "ok";
        break;
    case Error::Unavailable:
        code = "unavailable";
        break;
    case Error::Offline:
        code = "offline";
        break;
    case Error::Timeout:
        code = "timeout";
        break;
    case Error::Tls:
        code = "tls";
        break;
    case Error::NotFound:
        code = "notFound";
        break;
    case Error::RateLimited:
        code = "rateLimited";
        break;
    case Error::ServerError:
        code = "serverError";
        break;
    case Error::Redirect:
        code = "redirect";
        break;
    case Error::TooLarge:
        code = "tooLarge";
        break;
    case Error::BadResponse:
        code = "badResponse";
        break;
    case Error::Canceled:
        code = "canceled";
        break;
    case Error::Sink:
        code = "sink";
        break;
    case Error::Proxy:
        code = "proxy";
        break;
    }
    return QString::fromLatin1(code);
}

Response get(const Request& request)
{
    Response   response;
    const auto fail = [&response](Error error, DWORD winError = 0) {
        response.error    = error;
        response.winError = winError;
        response.body.clear();
        return response;
    };
    if (!isAllowedUrl(request.url))
        return fail(Error::Redirect);

    WinHttp api;
    if (!api.ok())
        return fail(Error::Unavailable, GetLastError());

    const ULONGLONG start     = nowMs();
    const auto      remaining = [&]() -> qint64 { return request.deadlineMs - static_cast<qint64>(nowMs() - start); };
    const auto      canceled  = [&]() { return request.cancel && request.cancel->load(); };

    bool direct = false; // no proxy (loopback test server)
#ifdef TSMEDIA_TESTHOOKS
    direct = !testOrigin().isEmpty();
#endif
    const auto planNow = [&request, direct]() {
        const std::shared_ptr<const proxy::Snapshot> snap = request.snapshotProvider ? request.snapshotProvider() : proxy::snapshot();
        if (direct) {
            SessionPlan plan; // TESTHOOKS' loopback test origin keeps NO_PROXY
            plan.via        = proxy::Via::Direct;
            plan.generation = snap ? snap->generation : 0;
            return plan;
        }
        const proxy::windowsproxy::Summary windows =
            snap && snap->mode == proxy::Mode::System ? proxy::windowsproxy::windowsProxySummary() : proxy::windowsproxy::Summary();
        return sessionPlan(snap ? *snap : proxy::Snapshot(), windows);
    };
    const auto refuse = [&](const SessionPlan& plan) {
        response.proxyFailure = plan.refused;
        response.via          = plan.via;
        return fail(Error::Proxy);
    };
    SessionPlan current = planNow();
    response.routeGeneration = current.generation;
    response.via             = current.via;
    if (current.refused != proxy::Failure::None)
        return refuse(current);
    std::unique_ptr<Handle> session(new Handle(api, openSession(api, current)));
    if (!*session)
        return fail(Error::Unavailable, GetLastError());
    configureSession(api, session->get());

    QUrl url = request.url;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (canceled())
            return fail(Error::Canceled);
        if (remaining() <= 0)
            return fail(Error::Timeout);
        if (hop > 0) {
            // Each hop reads the current route (S12): a hop after a switch never leaves outside it.
            const SessionPlan next = planNow();
            response.routeGeneration = next.generation;
            response.via             = next.via;
            if (next.refused != proxy::Failure::None)
                return refuse(next);
            if (!next.sameSession(current)) {
                current = next;
                session.reset(new Handle(api, openSession(api, current)));
                if (!*session)
                    return fail(Error::Unavailable, GetLastError());
                configureSession(api, session->get());
            }
        }

        const bool         secure = url.scheme() == QLatin1String("https");
        const std::wstring host   = url.host(QUrl::FullyEncoded).toStdWString();
        const INTERNET_PORT port  = static_cast<INTERNET_PORT>(url.port(secure ? 443 : 80));
        QString            target = url.path(QUrl::FullyEncoded);
        if (target.isEmpty())
            target = QString::fromLatin1("/");
        if (url.hasQuery())
            target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
        const std::wstring path = target.toStdWString();

        Handle connection(api, api.connect(session->get(), host.c_str(), port, 0));
        if (!connection)
            return fail(mapError(GetLastError()), GetLastError());
        Handle req(api, api.openRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0));
        if (!req)
            return fail(mapError(GetLastError()), GetLastError());
        DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        api.setOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
        quint64 bridgeId = 0;
        if (current.useCredentials) {
            // The bridge: its token before the send (pre-authentication), on this new request handle; never the Windows
            // account's credentials (autologon HIGH, bridge sessions only, F8c).
            bridgeId                 = ++g_bridgeRequestId;
            const std::wstring user  = (QLatin1String("tsm-u") + QString::number(bridgeId)).toStdWString();
            const std::wstring token = QString::fromLatin1(current.token).toStdWString();
            if (!api.setCredentials || !api.setCredentials(req.get(), WINHTTP_AUTH_TARGET_PROXY, WINHTTP_AUTH_SCHEME_BASIC, user.c_str(), token.c_str(), nullptr))
                return fail(Error::Unavailable);
        }
        if (current.autologonHigh) {
            DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
            api.setOption(req.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
        }
        // A route-level failure the bridge recorded for this request (the record is the truth, S3).
        const auto bridgeFailure = [&]() {
            proxy::Record rec;
            if (bridgeId && proxy::liveRecords()->outcome('u', bridgeId, &rec) && (proxy::isRouteLevel(rec.failure) || rec.failure == proxy::Failure::Off))
                return rec.failure;
            return proxy::Failure::None;
        };
        const auto step = [&](int ms) { return static_cast<int>(qBound<qint64>(1000, qMin<qint64>(ms, remaining()), ms)); };
        api.setTimeouts(req.get(), step(kResolveTimeoutMs), step(kConnectTimeoutMs), step(kSendTimeoutMs), step(kReceiveTimeoutMs));

        if (!api.send(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !api.receive(req.get(), nullptr)) {
            const DWORD error = GetLastError();
            const proxy::Failure f = bridgeFailure();
            if (f != proxy::Failure::None) {
                response.proxyFailure = f;
                return fail(Error::Proxy, error);
            }
            return fail(mapError(error), error);
        }

        DWORD status = 0;
        DWORD size   = sizeof(status);
        if (!api.query(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return fail(Error::BadResponse, GetLastError());
        response.status = static_cast<int>(status);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::wstring location;
            if (!queryString(api, req.get(), WINHTTP_QUERY_LOCATION, &location))
                return fail(Error::BadResponse);
            const QUrl next = url.resolved(QUrl(QString::fromStdWString(location), QUrl::StrictMode));
            if (!isAllowedUrl(next))
                return fail(Error::Redirect);
            url = next;
            continue;
        }
        if (status != 200) {
            // 2.6: a status is the proxy's only with IS_PROXY_CONNECT_RESPONSE (or the bridge's record), S3.
            BOOL  fromProxy = FALSE;
            DWORD flagSize  = sizeof(fromProxy);
            if (api.queryOption)
                api.queryOption(req.get(), WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE, &fromProxy, &flagSize);
            if (fromProxy && current.via == proxy::Via::Bridge) {
                const proxy::Failure f = bridgeFailure();
                response.proxyFailure  = f != proxy::Failure::None ? f : proxy::Failure::Internal;
                return fail(Error::Proxy);
            }
            proxy::Failure f     = proxy::Failure::None;
            const Error    error = statusError(static_cast<int>(status), fromProxy != FALSE, &f);
            response.proxyFailure = f;
            if (error == Error::RateLimited)
                response.retryAfterSec = retryAfter(api, req.get());
            return fail(error);
        }

        // Content-Length is untrusted: only an early "too large".
        std::wstring lengthText;
        if (queryString(api, req.get(), WINHTTP_QUERY_CONTENT_LENGTH, &lengthText)) {
            bool         ok     = false;
            const qint64 length = QString::fromStdWString(lengthText).trimmed().toLongLong(&ok);
            if (ok && length > request.maxBytes)
                return fail(Error::TooLarge);
        }
        response.finalHost = url.host();

        std::vector<char> buffer(kChunk);
        ULONGLONG         lastProgress = 0;
        for (;;) {
            if (canceled())
                return fail(Error::Canceled);
            if (remaining() <= 0)
                return fail(Error::Timeout);
            DWORD available = 0;
            if (!api.available(req.get(), &available)) {
                const DWORD error = GetLastError();
                return fail(mapError(error), error);
            }
            if (available == 0)
                break; // the whole body has arrived
            const DWORD want = available < static_cast<DWORD>(kChunk) ? available : static_cast<DWORD>(kChunk);
            DWORD       got  = 0;
            if (!api.read(req.get(), buffer.data(), want, &got)) {
                const DWORD error = GetLastError();
                return fail(mapError(error), error);
            }
            if (got == 0)
                break;
            response.received += got;
            if (response.received > request.maxBytes)
                return fail(Error::TooLarge);
            if (request.sink) {
                if (!request.sink(buffer.data(), got))
                    return fail(Error::Sink);
            } else {
                response.body.append(buffer.data(), static_cast<int>(got));
            }
            if (request.progress && nowMs() - lastProgress >= 100) {
                lastProgress = nowMs();
                request.progress(response.received);
            }
        }
        if (request.progress)
            request.progress(response.received);
        response.error = Error::None;
        return response;
    }
    return fail(Error::Redirect); // too many hops
}

} // namespace upd::http
