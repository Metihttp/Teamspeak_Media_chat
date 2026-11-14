#pragma once

// 2.6 chat proxy: why a route or one proxied request failed, and the step it failed in (design 5.1). One table for
// every user: the bridge's records, the Fetcher, the updater, the YouTube player, route health and the texts.
//
// "Route-level": the failure is the proxy's (or the route's), not the website's. "Decisive": it can't depend on the
// website asked for, so one is enough to call the route down (isDecisive(failure, step): NotSocks5 is decisive in the
// greeting and not in the reply to the CONNECT). Pure QtCore. FROZEN (design 3.1): a change needs a note in the
// commit message and in design.md.

#include <QString>

namespace proxy {

enum class Failure : quint8 {
    None,
    // route-level, decisive
    Unreachable,             // TCP connect to the proxy failed (Windows proxy: 12029 before connected)
    NameNotFound,            // the proxy's own name not found (Windows proxy: 12007 before connected)
    ProxyNameFiltered,       // the proxy's name resolves only into the sinkhole set
    NotSocks5,               // SOCKS5 chosen: greeting byte 0 != 05 / unknown method (decisive); reply byte 0 / ATYP (not)
    NotHttp,                 // HTTP chosen: the CONNECT status line isn't HTTP/1.x
    NeedsLogin,              // the proxy asks for a login and none is set
    LoginRefused,            // RFC 1929 status != 0, a 407 after our Basic
    LoginMethod,             // FF with a login; 407 without Basic
    PasswordUnreadable,      // DPAPI can't open the saved password (other user, other PC, tampered, wrong endpoint)
    BridgeUnavailable,       // the loopback bridge couldn't start (bind, WSAStartup)
    SocksOnlyWindows,        // W1: Windows names only a SOCKS proxy
    UnsupportedWindowsProxy, // W1: a setup-script answer TS Media can't use (HTTPS-only, no entry)
    // route-level, not decisive (ambiguous)
    NotAllowed,    // SOCKS REP 2; CONNECT 403 or another non-2xx
    CantReachSite, // REP 1/3/4/6/other; 502/503/504; success then EOF before the site's first byte
    NoNameConnect, // REP 7/8: the proxy can't connect by name
    Slow,          // a handshake step over its budget
    Intercepted,   // custom: a TLS failure with INVALID_CA after the tunnel relayed bytes from the site
    // not route-level
    Internal,    // DPAPI transient error, a bridge hop with no record, an internal bridge error
    NotReady,    // the initial snapshot (a start-order bug; logged once)
    SiteRefused, // REP 5: site-level (kOffline)
    Refused,     // the bridge's own policy refused the request (a bug or a hostile request)
    Busy,        // the bridge's 64 tunnels are in use
    Off,         // the bridge's upstream is none (a request raced a switch away from custom)
};

// Where a bridge tunnel (or a proxied request) was when it ended or now is (records.h).
enum class Step : quint8 {
    None,
    Head,     // reading the client's CONNECT head
    Resolve,  // the proxy's own name
    Connect,  // TCP to the proxy
    Greeting, // SOCKS5 method negotiation
    Login,    // RFC 1929 / the Basic resend
    Request,  // SOCKS5 CONNECT and its reply; HTTP CONNECT and its status line
    Relay,    // the tunnel is established
    Done,
};

const char* failureName(Failure failure);    // "unreachable", "cantReachSite", ... (logs, X-TSMedia-Proxy, tests)
Failure     failureFromName(const QString& name); // None for an unknown name
const char* stepName(Step step);             // "head", "resolve", ...

bool isRouteLevel(Failure failure);
bool isDecisive(Failure failure, Step step); // only route-level failures can be decisive

// A failure while the tunnel was still being set up (a handshake step).
inline bool isHandshakeStep(Step step)
{
    return step == Step::Head || step == Step::Resolve || step == Step::Connect || step == Step::Greeting || step == Step::Login
           || step == Step::Request;
}

// The short text of design 5.1 ("can't reach the proxy at %1"), %1 filled with hostPort (Unreachable only; chat lines
// pass the 1.4 rule's text: "127.0.0.1:2080" on this PC, empty elsewhere: then "can't reach the proxy"). Empty for the
// failures that have none (None, SiteRefused, Refused, Busy, Off).
QString shortText(Failure failure, const QString& hostPort = QString());

} // namespace proxy
