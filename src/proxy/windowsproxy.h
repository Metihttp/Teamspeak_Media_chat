#pragma once

// 2.6 chat proxy: "Use Windows proxy settings" (design 3.3.2, 4.3 rule W1). The user's Windows settings (the manual
// proxy and its bypass list, a setup script or automatic detection), read through an injectable provider, and the pure
// mapping of a setup script's answer: the entries WinHttpGetProxyForUrlEx / WinHttpGetProxyResult report keep their
// scheme, so a SOCKS-only answer is refused instead of being read as DIRECT (S2).

#include <QString>
#include <QVector>

#include <functional>

#include "failure.h"

namespace proxy::windowsproxy {

// One WINHTTP_PROXY_RESULT_ENTRY (scheme and flags, never strings to parse).
struct Entry {
    enum class Scheme { Http, Https, Socks, Other };
    bool    proxy  = false; // fProxy: false = DIRECT (a bypass)
    Scheme  scheme = Scheme::Http;
    QString host;
    quint16 port = 0;
};

enum class Decision { Direct, Proxy, Refused };
struct Mapped {
    Decision decision = Decision::Direct;
    QString  proxies; // Proxy: "host:port[;host:port]" (HTTP entries only, in order)
    Failure  refused = Failure::None;
};
// 1. any HTTP proxy entry -> Proxy with every HTTP entry in order (a later DIRECT fallback dropped: fail closed);
// 2. else the first entry SOCKS -> Refused SocksOnlyWindows; 3. else the first HTTPS (TLS to the proxy) or another
// scheme -> Refused UnsupportedWindowsProxy; 4. else the first a bypass -> Direct; 5. no entry -> Refused
// UnsupportedWindowsProxy (what WinHTTP answers for "SOCKS5 x" and Chromium's "HTTP x").
Mapped mapProxyResult(const QVector<Entry>& entries);

// The user's Internet settings (WinHttpGetIEProxyConfigForCurrentUser).
struct IeConfig {
    bool    ok         = false; // false: no settings could be read (direct)
    bool    autoDetect = false;
    QString autoConfigUrl;
    QString proxy;
    QString bypass;
};
IeConfig ieConfig(); // the injected provider, else the real one
void     setIeConfigProviderForTests(std::function<IeConfig()> provider); // empty: the real one

// What the status row and the player check need (design 1.3.4, S13): one cheap read, no script run.
//  * Script: a setup script is set (it decides; the manual setting is only its fallback), or automatic detection is on
//    and no manual proxy is set.
//  * HttpProxy / SocksOnly: the manual setting, also while automatic detection is on (Windows' default): wherever no
//    WPAD server answers, Chromium, WinHTTP and the Fetcher fall back to it (review fix). autoDetect: detection is on too.
struct Summary {
    enum class Kind { None, HttpProxy, Script, SocksOnly };
    Kind    kind = Kind::None;
    QString hostPort; // HttpProxy: the first usable server; SocksOnly: the SOCKS server ("127.0.0.1:2080")
    QString socksHost;
    quint16 socksPort  = 0;
    bool    autoDetect = false; // HttpProxy / SocksOnly: "Automatically detect settings" is on as well
};
Summary summarize(const IeConfig& config); // pure
Summary windowsProxySummary();             // summarize(ieConfig())

} // namespace proxy::windowsproxy
