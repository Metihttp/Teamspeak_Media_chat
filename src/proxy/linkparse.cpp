#include "linkparse.h"

#include <QStringList>
#include <QUrl>

#include <cstring>

#include "i18n.h"

namespace proxy {

namespace {

bool startsWithCi(const QString& text, const char* prefix)
{
    return text.startsWith(QLatin1String(prefix), Qt::CaseInsensitive);
}

ProxyLink refused(ProxyLink::Result result)
{
    ProxyLink link;
    link.result = result;
    return link;
}

// "host:port" / "[v6]:port" / "host" / "v6" (no brackets): fills host and port. Result Ok, HostOnly, NoPort (only when
// requirePort), BadPort or NotALink.
ProxyLink::Result parseHostPort(const QString& text, bool requirePort, QString* host, quint16* port)
{
    QString h;
    QString p;
    bool    hasPort = false;
    if (text.startsWith(QLatin1Char('['))) {
        const int end = text.indexOf(QLatin1Char(']'));
        if (end < 0)
            return ProxyLink::Result::NotALink;
        h          = text.mid(1, end - 1);
        const QString rest = text.mid(end + 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(QLatin1Char(':')))
                return ProxyLink::Result::NotALink;
            hasPort = true;
            p       = rest.mid(1);
        }
        if (!h.contains(QLatin1Char(':')))
            return ProxyLink::Result::NotALink; // brackets are for IPv6 addresses only
    } else if (text.count(QLatin1Char(':')) == 1) {
        const int colon = text.indexOf(QLatin1Char(':'));
        h               = text.left(colon);
        p               = text.mid(colon + 1);
        hasPort         = true;
    } else {
        h = text; // no colon, or an IPv6 address with no port (F19)
    }
    if (normalizeHost(h).isEmpty())
        return ProxyLink::Result::NotALink;
    *host = normalizeHost(h);
    if (!hasPort)
        return requirePort ? ProxyLink::Result::NoPort : ProxyLink::Result::HostOnly;
    if (p.isEmpty())
        return ProxyLink::Result::NoPort;
    bool      ok    = false;
    const int value = p.toInt(&ok);
    for (const QChar ch : p) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            ok = false;
    }
    if (!ok || value < 1 || value > 65535)
        return ProxyLink::Result::BadPort;
    *port = static_cast<quint16>(value);
    return ProxyLink::Result::Ok;
}

} // namespace

ProxyLink parseProxyLink(const QString& raw)
{
    if (raw.size() > kMaxLinkChars)
        return refused(ProxyLink::Result::NotALink);
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return refused(ProxyLink::Result::NoText);
    for (const QChar ch : text) {
        if (ch.isSpace() || ch.unicode() < 0x20 || ch.unicode() == 0x7f)
            return refused(ProxyLink::Result::NotALink);
    }

    // Telegram first (also as https:// links).
    QString bare = text;
    for (const char* prefix : {"https://", "http://"}) {
        if (startsWithCi(bare, prefix)) {
            bare = bare.mid(static_cast<int>(strlen(prefix)));
            break;
        }
    }
    if (startsWithCi(text, "tg://socks") || startsWithCi(bare, "t.me/socks") || startsWithCi(bare, "www.t.me/socks"))
        return refused(ProxyLink::Result::TelegramSocks);
    if (startsWithCi(text, "tg://proxy") || startsWithCi(bare, "t.me/proxy") || startsWithCi(bare, "www.t.me/proxy"))
        return refused(ProxyLink::Result::TelegramMtproto);

    for (const char* vpn : {"vless://", "vmess://", "ss://", "ssr://", "trojan://", "hy2://", "hysteria://", "hysteria2://", "tuic://",
                            "wireguard://", "wg://"}) {
        if (startsWithCi(text, vpn))
            return refused(ProxyLink::Result::VpnLink);
    }
    if (startsWithCi(text, "socks4://") || startsWithCi(text, "socks4a://"))
        return refused(ProxyLink::Result::Socks4);

    const int sep = text.indexOf(QLatin1String("://"));
    if (sep < 0) {
        // host:port, [v6]:port, or a host alone
        if (text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('@')) || text.contains(QLatin1Char('?')) || text.contains(QLatin1Char('#')))
            return refused(ProxyLink::Result::NotALink);
        ProxyLink link;
        link.result = parseHostPort(text, false, &link.host, &link.port);
        if (link.result != ProxyLink::Result::Ok && link.result != ProxyLink::Result::HostOnly) {
            const ProxyLink::Result r = link.result;
            return refused(r);
        }
        return link;
    }

    const QString scheme = text.left(sep).toLower();
    QString       rest   = text.mid(sep + 3);
    const int     end    = [&rest]() {
        int at = rest.size();
        for (const QChar c : {QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#')}) {
            const int i = rest.indexOf(c);
            if (i >= 0 && i < at)
                at = i;
        }
        return at;
    }();
    const QString authority = rest.left(end);
    const QString tail      = rest.mid(end);
    if (scheme == QLatin1String("https")) {
        if (!tail.isEmpty() && tail != QLatin1String("/"))
            return refused(ProxyLink::Result::VpnLink); // a subscription address
        return refused(ProxyLink::Result::Https);
    }
    ProxyLink link;
    if (scheme == QLatin1String("socks5") || scheme == QLatin1String("socks5h") || scheme == QLatin1String("socks")) {
        link.type = Type::Socks5;
    } else if (scheme == QLatin1String("http")) {
        link.type = Type::Http;
    } else {
        return refused(ProxyLink::Result::NotALink);
    }
    link.hasType = true;
    if (!tail.isEmpty() && tail != QLatin1String("/"))
        return refused(ProxyLink::Result::NotALink);
    QString   hostPart = authority;
    const int at       = authority.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        const QString userInfo = authority.left(at);
        hostPart               = authority.mid(at + 1);
        const int colon        = userInfo.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            link.user        = QUrl::fromPercentEncoding(userInfo.left(colon).toUtf8());
            link.password    = QUrl::fromPercentEncoding(userInfo.mid(colon + 1).toUtf8());
            link.hasPassword = true;
        } else {
            link.user = QUrl::fromPercentEncoding(userInfo.toUtf8());
        }
    }
    link.result = parseHostPort(hostPart, true, &link.host, &link.port);
    if (link.result != ProxyLink::Result::Ok) {
        const ProxyLink::Result r = link.result;
        return refused(r);
    }
    return link;
}

QString linkMessage(ProxyLink::Result result)
{
    switch (result) {
    case ProxyLink::Result::Ok:
    case ProxyLink::Result::HostOnly:
        return QString();
    case ProxyLink::Result::NoText:
        return i18n::t("The clipboard has no text.");
    case ProxyLink::Result::VpnLink:
        return i18n::t("This is a server link for a VPN app. Add it to your VPN app, then enter the app's local port here.");
    case ProxyLink::Result::TelegramSocks:
        return i18n::t("This is a Telegram proxy link. Use your VPN app's local port instead.");
    case ProxyLink::Result::TelegramMtproto:
        return i18n::t("This is a Telegram proxy (MTProto). It only works in Telegram.");
    case ProxyLink::Result::Https:
        return i18n::t("TS Media can't use proxies over https://. Use the proxy's HTTP or SOCKS5 port.");
    case ProxyLink::Result::Socks4:
        return i18n::t("TS Media needs SOCKS5. Use the proxy's SOCKS5 or HTTP port.");
    case ProxyLink::Result::NoPort:
        return i18n::t("The link has no port.");
    case ProxyLink::Result::BadPort:
        return i18n::t("The link's port isn't a number from 1 to 65535.");
    case ProxyLink::Result::NotALink:
        break;
    }
    return i18n::t("This isn't a proxy link. Copy a link like socks5://127.0.0.1:10808.");
}

} // namespace proxy
