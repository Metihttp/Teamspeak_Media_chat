#pragma once

// 2.6 chat proxy (design 5.2): what route health hears of one link-preview load, for both Fetchers (the chat's,
// ChatLinkPreviews; the sender's, linkpreview::Hub). Route-level failures only (never routedown: no request was made),
// a site's answer through the bridge (custom), and any site's answer (Windows mode). The Control drops a result of an
// older route generation itself. GUI thread.

#include <QString>

#include "linkpreview/fetcher.h"
#include "proxy/control.h"

namespace linkpreview {

inline void reportRoute(proxy::Control* control, const Result& result, const QString& host)
{
    if (!control)
        return;
    if (!result.ok && result.error == QLatin1String(errors::kProxy) && proxy::isRouteLevel(result.proxyFailure)) {
        proxy::RouteReport report;
        report.failure    = result.proxyFailure;
        report.step       = result.proxyStep;
        report.decisive   = result.proxyDecisive;
        report.host       = host;
        report.generation = result.routeGeneration;
        report.via        = result.via;
        control->report(report);
    }
    if (result.routeOk)
        control->reportOk(result.routeGeneration);
    if (result.siteResponded)
        control->reportSiteResponded();
}

// The admission gate of both Fetchers (Fetcher::Options::routeGate): automatic loads wait while the chat proxy is down;
// clicks always go. Without a Control (after the unload) nothing is loading anyway.
inline bool admitsRoute(bool userAsked)
{
    proxy::Control* control = proxy::Control::current();
    return !control || control->admits(userAsked);
}

} // namespace linkpreview
