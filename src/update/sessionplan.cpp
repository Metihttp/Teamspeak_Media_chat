#include "sessionplan.h"

#include "i18n.h"

namespace upd {

SessionPlan sessionPlan(const proxy::Snapshot& snapshot, const proxy::windowsproxy::Summary& windows)
{
    SessionPlan plan;
    plan.generation = snapshot.generation;
    switch (snapshot.mode) {
    case proxy::Mode::Unset:
        plan.via     = proxy::Via::Refused;
        plan.refused = proxy::Failure::NotReady;
        return plan;
    case proxy::Mode::Direct:
        plan.via    = proxy::Via::Direct;
        plan.access = SessionPlan::Access::NoProxy;
        return plan;
    case proxy::Mode::System:
        if (windows.kind == proxy::windowsproxy::Summary::Kind::SocksOnly) {
            plan.via     = proxy::Via::Refused; // WinHTTP would go direct (S12)
            plan.refused = proxy::Failure::SocksOnlyWindows;
            return plan;
        }
        plan.via    = proxy::Via::System;
        plan.access = SessionPlan::Access::Automatic; // 2.5.0 exactly
        return plan;
    case proxy::Mode::Custom:
        if (snapshot.refused != proxy::Failure::None || snapshot.bridgePort < 1024 || snapshot.token.isEmpty()) {
            plan.via     = proxy::Via::Refused;
            plan.refused = snapshot.refused != proxy::Failure::None ? snapshot.refused : proxy::Failure::Internal;
            return plan;
        }
        plan.via            = proxy::Via::Bridge;
        plan.access         = SessionPlan::Access::Named;
        plan.proxy          = QLatin1String("127.0.0.1:") + QString::number(snapshot.bridgePort);
        plan.useCredentials = true;
        plan.autologonHigh  = true;
        plan.token          = snapshot.token;
        return plan;
    }
    plan.via     = proxy::Via::Refused;
    plan.refused = proxy::Failure::Internal;
    return plan;
}

http::Error statusError(int status, bool fromProxy, proxy::Failure* failure)
{
    *failure = proxy::Failure::None;
    if (fromProxy) {
        if (status == 407)
            *failure = proxy::Failure::NeedsLogin;
        else if (status == 502 || status == 503 || status == 504)
            *failure = proxy::Failure::CantReachSite;
        else
            *failure = proxy::Failure::NotAllowed;
        return http::Error::Proxy;
    }
    if (status == 404)
        return http::Error::NotFound;
    if (status == 407)
        return http::Error::BadResponse; // a website's 407 inside the tunnel is no proxy's answer (S3)
    if (status == 403 || status == 429)
        return http::Error::RateLimited;
    if (status >= 500 && status <= 599)
        return http::Error::ServerError;
    return http::Error::BadResponse;
}

bool proxyFailureIsQuiet(quint64 failedGeneration, quint64 currentGeneration, proxy::Failure failure)
{
    if (failedGeneration == 0 || failedGeneration >= currentGeneration)
        return false;
    return proxy::isRouteLevel(failure) || failure == proxy::Failure::Off;
}

QString proxyShortError(proxy::Failure failure, proxy::Via via)
{
    const QString reason = proxy::shortText(failure);
    if (via == proxy::Via::System || (via == proxy::Via::Refused && (failure == proxy::Failure::SocksOnlyWindows || failure == proxy::Failure::UnsupportedWindowsProxy)))
        return i18n::t("couldn't reach GitHub through your Windows proxy (%1)").arg(reason);
    return i18n::t("couldn't reach GitHub through the chat proxy (%1)").arg(reason);
}

ProxyErrorCopy proxyErrorCopy(proxy::Failure failure, proxy::Via via)
{
    QString reason = proxy::shortText(failure);
    if (!reason.isEmpty())
        reason[0] = reason.at(0).toUpper();
    if (via == proxy::Via::System || (via == proxy::Via::Refused && (failure == proxy::Failure::SocksOnlyWindows || failure == proxy::Failure::UnsupportedWindowsProxy)))
        return {i18n::t("Couldn't reach GitHub through your Windows proxy"),
                i18n::t("%1. Nothing was changed. Check the proxy in your Windows settings, or use the Chat proxy window, then try again.").arg(reason)};
    return {i18n::t("Couldn't reach GitHub through the chat proxy"),
            i18n::t("%1. Nothing was changed. Check the proxy in the Chat proxy window, or turn it off, then try again.").arg(reason)};
}

} // namespace upd
