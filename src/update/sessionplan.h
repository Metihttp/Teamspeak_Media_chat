#pragma once

// 2.6 chat proxy: how the updater's requests leave the PC (design 3.5). Pure, always built (the network part,
// httpclient.cpp, is only built with TSMEDIA_UPDATER=ON) and tested offscreen.
//
// Every request (and every redirect hop) reads the current route snapshot; a hop whose plan differs from the
// session's opens a new session. Nothing is canceled on a route change (F8): integrity doesn't depend on the route
// (TLS plus the manifest's SHA-256).
//  * Unset -> refused (NotReady).
//  * Windows settings -> 2.5.0 exactly (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY and its fallbacks, no autologon policy:
//    corporate proxy logins keep working), except a manual Windows setting that names only a SOCKS proxy: refused
//    (S12). A setup script that answers SOCKS isn't seen here (a documented gap).
//  * No proxy -> NO_PROXY.
//  * Chat proxy -> the bridge: NAMED_PROXY "127.0.0.1:<port>", no bypass, the token as Basic proxy credentials
//    (tsm-u<id>) set before each send on a new request handle, autologon HIGH. Refused custom -> refused.

#include <QByteArray>
#include <QString>

#include "httpclient.h"
#include "proxy/failure.h"
#include "proxy/router.h"
#include "proxy/windowsproxy.h"

namespace upd {

struct SessionPlan {
    enum class Access { NoProxy, Automatic, Named };
    Access         access         = Access::NoProxy;
    QString        proxy;                 // Named: "127.0.0.1:<port>"
    bool           useCredentials = false; // the bridge's token as Basic proxy credentials
    bool           autologonHigh  = false; // bridge sessions only (F8c)
    proxy::Failure refused        = proxy::Failure::None; // != None: nothing is sent
    proxy::Via     via            = proxy::Via::Unset;
    QByteArray     token;                 // the bridge's (never logged)
    quint64        generation     = 0;

    // The same session can carry the next hop.
    bool sameSession(const SessionPlan& other) const
    {
        return access == other.access && proxy == other.proxy && useCredentials == other.useCredentials && autologonHigh == other.autologonHigh
               && refused == other.refused && token == other.token;
    }
};

SessionPlan sessionPlan(const proxy::Snapshot& snapshot, const proxy::windowsproxy::Summary& windows);

// A non-2xx status: fromProxy is WINHTTP_OPTION_IS_PROXY_CONNECT_RESPONSE. From the proxy: Error::Proxy with 407 ->
// NeedsLogin, 403 -> NotAllowed, 502/503/504 -> CantReachSite, other -> NotAllowed. Not from the proxy: the 2.5
// mapping, except that a 407 is BadResponse (was Offline: only the flag makes it the proxy's, S3).
http::Error statusError(int status, bool fromProxy, proxy::Failure* failure);

// A route-level failure (or Off) of an older route generation than the current one: quiet, no "Last check failed" (F8).
bool proxyFailureIsQuiet(quint64 failedGeneration, quint64 currentGeneration, proxy::Failure failure);

// Settings' "Last check failed: ..." and the chat line: "couldn't reach GitHub through the chat proxy (%1)" /
// "... through your Windows proxy (%1)".
QString proxyShortError(proxy::Failure failure, proxy::Via via);
struct ProxyErrorCopy {
    QString heading;
    QString body;
};
ProxyErrorCopy proxyErrorCopy(proxy::Failure failure, proxy::Via via);

} // namespace upd
