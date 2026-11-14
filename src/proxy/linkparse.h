#pragma once

// 2.6 chat proxy: "Paste from clipboard" and links typed into the server field (design 1.3.6). Pure QtCore.
//
// Accepted: socks5://[user[:pass]@]host:port[/] (also socks5h:// and socks://), http://[user[:pass]@]host:port[/], and
// host:port (exactly one ':') or [v6]:port (the type unchanged). Two or more colons without brackets are an IPv6 host
// with no port (F19). The user name is always set from the link, empty when it has none (S7). VPN server links,
// Telegram proxy links, https:// proxies and SOCKS4 are refused with their own message.

#include <QString>

#include "proxyconfig.h"

namespace proxy {

struct ProxyLink {
    enum class Result {
        Ok,              // host and port (and type when the link has a scheme)
        HostOnly,        // a host with no port ("::1", "fe80::1", "proxy.example.net"): the server field keeps it
        NoText,          // the clipboard has no text
        VpnLink,         // vless:// vmess:// ss:// ... or an https address with a path (a subscription)
        TelegramSocks,   // tg://socks?..., t.me/socks?...
        TelegramMtproto, // tg://proxy?..., t.me/proxy?...
        Https,           // https://host:port (TLS to the proxy)
        Socks4,          // socks4://, socks4a://
        NoPort,          // a link with a scheme and no port
        BadPort,         // a port that isn't 1..65535
        NotALink,        // anything else
    };
    Result  result  = Result::NotALink;
    bool    hasType = false; // the link named the type (socks5 / http); else the form's type stays
    Type    type    = Type::Socks5;
    QString host; // normalizeHost()
    quint16 port = 0;
    QString user;     // percent-decoded; empty when the link has none
    QString password; // percent-decoded; filled only when hasPassword
    bool    hasPassword = false;
};

constexpr int kMaxLinkChars = 2048;

ProxyLink parseProxyLink(const QString& text);
// The result row's text for a refusal (design 1.3.6); empty for Ok and HostOnly.
QString linkMessage(ProxyLink::Result result);

} // namespace proxy
