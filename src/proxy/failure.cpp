#include "failure.h"

#include "i18n.h"

namespace proxy {

namespace {

struct Row {
    Failure     failure;
    const char* name;
};

constexpr Row kRows[] = {
    {Failure::None, "none"},
    {Failure::Unreachable, "unreachable"},
    {Failure::NameNotFound, "nameNotFound"},
    {Failure::ProxyNameFiltered, "proxyNameFiltered"},
    {Failure::NotSocks5, "notSocks5"},
    {Failure::NotHttp, "notHttp"},
    {Failure::NeedsLogin, "needsLogin"},
    {Failure::LoginRefused, "loginRefused"},
    {Failure::LoginMethod, "loginMethod"},
    {Failure::PasswordUnreadable, "passwordUnreadable"},
    {Failure::BridgeUnavailable, "bridgeUnavailable"},
    {Failure::SocksOnlyWindows, "socksOnlyWindows"},
    {Failure::UnsupportedWindowsProxy, "unsupportedWindowsProxy"},
    {Failure::NotAllowed, "notAllowed"},
    {Failure::CantReachSite, "cantReachSite"},
    {Failure::NoNameConnect, "noNameConnect"},
    {Failure::Slow, "slow"},
    {Failure::Intercepted, "intercepted"},
    {Failure::Internal, "internal"},
    {Failure::NotReady, "notReady"},
    {Failure::SiteRefused, "siteRefused"},
    {Failure::Refused, "refused"},
    {Failure::Busy, "busy"},
    {Failure::Off, "off"},
};

} // namespace

const char* failureName(Failure failure)
{
    for (const Row& row : kRows) {
        if (row.failure == failure)
            return row.name;
    }
    return "none";
}

Failure failureFromName(const QString& name)
{
    for (const Row& row : kRows) {
        if (name == QLatin1String(row.name))
            return row.failure;
    }
    return Failure::None;
}

const char* stepName(Step step)
{
    switch (step) {
    case Step::None:
        return "none";
    case Step::Head:
        return "head";
    case Step::Resolve:
        return "resolve";
    case Step::Connect:
        return "connect";
    case Step::Greeting:
        return "greeting";
    case Step::Login:
        return "login";
    case Step::Request:
        return "request";
    case Step::Relay:
        return "relay";
    case Step::Done:
        return "done";
    }
    return "none";
}

bool isRouteLevel(Failure failure)
{
    switch (failure) {
    case Failure::Unreachable:
    case Failure::NameNotFound:
    case Failure::ProxyNameFiltered:
    case Failure::NotSocks5:
    case Failure::NotHttp:
    case Failure::NeedsLogin:
    case Failure::LoginRefused:
    case Failure::LoginMethod:
    case Failure::PasswordUnreadable:
    case Failure::BridgeUnavailable:
    case Failure::SocksOnlyWindows:
    case Failure::UnsupportedWindowsProxy:
    case Failure::NotAllowed:
    case Failure::CantReachSite:
    case Failure::NoNameConnect:
    case Failure::Slow:
    case Failure::Intercepted:
        return true;
    default:
        return false;
    }
}

bool isDecisive(Failure failure, Step step)
{
    switch (failure) {
    case Failure::Unreachable:
    case Failure::NameNotFound:
    case Failure::ProxyNameFiltered:
    case Failure::NotHttp:
    case Failure::NeedsLogin:
    case Failure::LoginRefused:
    case Failure::LoginMethod:
    case Failure::PasswordUnreadable:
    case Failure::BridgeUnavailable:
    case Failure::SocksOnlyWindows:
    case Failure::UnsupportedWindowsProxy:
        return true;
    case Failure::NotSocks5:
        return step == Step::Greeting; // a malformed reply to the CONNECT may depend on the site (S3)
    default:
        return false; // ambiguous ones (NotAllowed, CantReachSite, NoNameConnect, Slow, Intercepted) and the rest
    }
}

QString shortText(Failure failure, const QString& hostPort)
{
    switch (failure) {
    case Failure::Unreachable:
        return hostPort.isEmpty() ? i18n::t("can't reach the proxy") : i18n::t("can't reach the proxy at %1").arg(hostPort);
    case Failure::NameNotFound:
        return i18n::t("can't find the proxy's address");
    case Failure::ProxyNameFiltered:
        return i18n::t("the proxy's name leads to a filtered address");
    case Failure::NotSocks5:
        return i18n::t("the proxy doesn't answer as SOCKS5");
    case Failure::NotHttp:
        return i18n::t("the proxy doesn't answer as HTTP");
    case Failure::NeedsLogin:
        return i18n::t("the proxy asks for a login");
    case Failure::LoginRefused:
        return i18n::t("the proxy refused the login");
    case Failure::LoginMethod:
        return i18n::t("the proxy wants an unsupported login");
    case Failure::PasswordUnreadable:
        return i18n::t("the saved password can't be read");
    case Failure::BridgeUnavailable:
        return i18n::t("TS Media couldn't open its local connection");
    case Failure::SocksOnlyWindows:
        return i18n::t("Windows names only a SOCKS proxy");
    case Failure::UnsupportedWindowsProxy:
        return i18n::t("Windows names a proxy TS Media can't use");
    case Failure::NotAllowed:
        return i18n::t("the proxy refuses the connection");
    case Failure::CantReachSite:
        return i18n::t("the proxy can't reach websites");
    case Failure::NoNameConnect:
        return i18n::t("the proxy can't connect to websites by name");
    case Failure::Slow:
        return i18n::t("the proxy isn't answering");
    case Failure::Intercepted:
        return i18n::t("the proxy intercepts secure connections");
    case Failure::Internal:
        return i18n::t("an internal error");
    case Failure::NotReady:
        return i18n::t("TS Media wasn't ready");
    default:
        return QString();
    }
}

} // namespace proxy
