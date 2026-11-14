#include "windowsproxy.h"

#include <windows.h>

#include <winhttp.h>

#include <QMutex>
#include <QStringList>

#include "linkpreview/netpolicy.h"
#include "proxyconfig.h"

namespace proxy::windowsproxy {

namespace {

QMutex                    g_providerLock;
std::function<IeConfig()> g_provider; // tests

QString takeGlobal(LPWSTR text)
{
    if (!text)
        return {};
    const QString out = QString::fromWCharArray(text, static_cast<int>(wcsnlen(text, linkpreview::net::kMaxProxyChars)));
    GlobalFree(text);
    return out;
}

IeConfig realConfig()
{
    using Fn = BOOL(WINAPI*)(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG*);
    static Fn fn = []() -> Fn {
        HMODULE dll = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // never freed
        return dll ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dll, "WinHttpGetIEProxyConfigForCurrentUser"))) : nullptr;
    }();
    IeConfig config;
    if (!fn)
        return config;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG raw{};
    if (!fn(&raw))
        return config;
    config.ok            = true;
    config.autoDetect    = raw.fAutoDetect != FALSE;
    config.autoConfigUrl = takeGlobal(raw.lpszAutoConfigUrl);
    config.proxy         = takeGlobal(raw.lpszProxy);
    config.bypass        = takeGlobal(raw.lpszProxyBypass);
    return config;
}

// "host:port" -> host and port (IPv6 in brackets).
void splitHostPort(const QString& server, QString* host, quint16* port)
{
    QString   s     = server;
    const int colon = s.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !s.mid(colon + 1).contains(QLatin1Char(']'))) {
        bool      ok = false;
        const int p  = s.mid(colon + 1).toInt(&ok);
        if (ok && p > 0 && p <= 65535) {
            *port = static_cast<quint16>(p);
            s     = s.left(colon);
        }
    }
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']')))
        s = s.mid(1, s.size() - 2);
    *host = s;
}

} // namespace

Mapped mapProxyResult(const QVector<Entry>& entries)
{
    Mapped      mapped;
    QStringList http;
    for (const Entry& e : entries) {
        if (e.proxy && e.scheme == Entry::Scheme::Http && !e.host.isEmpty())
            http << hostPort(e.host, e.port ? e.port : 80);
    }
    if (!http.isEmpty()) {
        mapped.decision = Decision::Proxy;
        mapped.proxies  = http.join(QLatin1Char(';'));
        return mapped;
    }
    if (entries.isEmpty()) {
        mapped.decision = Decision::Refused;
        mapped.refused  = Failure::UnsupportedWindowsProxy;
        return mapped;
    }
    const Entry& first = entries.first();
    if (!first.proxy) {
        mapped.decision = Decision::Direct; // the script said DIRECT
        return mapped;
    }
    mapped.decision = Decision::Refused;
    mapped.refused  = first.scheme == Entry::Scheme::Socks ? Failure::SocksOnlyWindows : Failure::UnsupportedWindowsProxy;
    return mapped;
}

IeConfig ieConfig()
{
    std::function<IeConfig()> provider;
    {
        QMutexLocker lock(&g_providerLock);
        provider = g_provider;
    }
    return provider ? provider() : realConfig();
}

void setIeConfigProviderForTests(std::function<IeConfig()> provider)
{
    QMutexLocker lock(&g_providerLock);
    g_provider = std::move(provider);
}

Summary summarize(const IeConfig& config)
{
    Summary summary;
    if (!config.ok)
        return summary;
    // A setup script decides (the manual setting is only its fallback when the script can't be read).
    if (!config.autoConfigUrl.isEmpty()) {
        summary.kind = Summary::Kind::Script;
        return summary;
    }
    // The manual setting as an https request to a public site would see it (the bypass list aside). Review fix:
    // "Automatically detect settings" is on by default in Windows, and wherever no WPAD server answers (nearly every home
    // network) Chromium, WinHTTP's AUTOMATIC_PROXY and the Fetcher's route() all fall back to the manual setting, so it
    // is what applies: a manual SOCKS-only setting is SocksOnly here too (the player S13 and the updater S12 refuse it).
    const linkpreview::net::PickedProxy picked = linkpreview::net::pickProxy(config.proxy, QString(), QStringLiteral("www.youtube.com"));
    summary.autoDetect                         = config.autoDetect;
    if (!picked.servers.isEmpty()) {
        summary.kind     = Summary::Kind::HttpProxy;
        summary.hostPort = picked.servers.section(QLatin1Char(';'), 0, 0);
        return summary;
    }
    if (picked.socksOnly) {
        summary.kind = Summary::Kind::SocksOnly;
        splitHostPort(picked.socksServer, &summary.socksHost, &summary.socksPort);
        summary.hostPort = picked.socksServer;
        return summary;
    }
    if (config.autoDetect)
        summary.kind = Summary::Kind::Script; // detection only, no manual proxy: whatever WPAD finds
    return summary;
}

Summary windowsProxySummary()
{
    return summarize(ieConfig());
}

} // namespace proxy::windowsproxy
