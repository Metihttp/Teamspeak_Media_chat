#pragma once

// 2.4 link previews: the network rules every website fetch follows (the sender's preview and a viewer's
// "Load preview"). Pure QtCore (unit-tested); fetcher.cpp applies them, the WinHTTP transport checks the
// connected address with isPublicAddress() too.
//
//  * https only, port 443, a public DNS name (linkurl.h publicHostName()), no user info, at most
//    kMaxUrlChars. Every redirect target is checked again (at most kMaxRedirects hops; https -> http is
//    refused, so is any other scheme).
//  * Addresses: a host is refused if ANY address it resolves to is not public (private, loopback,
//    link-local, CGNAT, multicast, unspecified, broadcast, documentation, benchmarking, reserved ranges;
//    IPv4 inside IPv6 (mapped, NAT64 64:ff9b::/96, 6to4) is judged by its IPv4 address), and the address
//    the connection actually went to is checked again (DNS rebinding).
//  * YouTube: oEmbed only from www.youtube.com, thumbnails only from i.ytimg.com (every hop).
//  * The Windows proxy: when the user's Windows settings name a proxy for an address (manual, or found by
//    auto-config / WPAD), the request goes through it. The proxy resolves the name and makes the
//    connection, so the local DNS check and the connected-address check are left out then (the peer is
//    the proxy); every other rule above still applies. Without a proxy nothing changes.

#include <QString>
#include <QUrl>

#include <array>

namespace linkpreview::net {

constexpr int    kMaxRedirects    = 5;
constexpr int    kConnectTimeoutMs = 10 * 1000;
constexpr int    kReceiveTimeoutMs = 15 * 1000; // one read (a stalled server)
constexpr int    kTotalTimeoutMs  = 60 * 1000;  // a whole load: every request, hop and read
constexpr qint64 kMaxOEmbedBytes  = 64 * 1024;
constexpr qint64 kMaxThumbnailBytes = 2 * 1024 * 1024;

// ---- addresses ---------------------------------------------------------------------------------------

// An IP address (QtCore only: the plugin may not use QtNetwork).
struct Address {
    enum class Family : quint8 { None, IPv4, IPv6 };
    Family                 family = Family::None;
    std::array<quint8, 16> bytes{}; // network order; IPv4 in the first 4

    bool    isNull() const { return family == Family::None; }
    quint32 ipv4() const; // host byte order (IPv4 only)
    QString toString() const;
    bool    operator==(const Address& other) const { return family == other.family && bytes == other.bytes; }
    bool    operator!=(const Address& other) const { return !(*this == other); }

    static Address fromIPv4(quint32 hostOrder);
    static Address fromIPv6(const quint8* sixteenBytes);
};

bool isPublicIPv4(quint32 address); // host byte order
bool isPublicAddress(const Address& address); // false for a null address

// An address as text: strict dotted IPv4 ("93.184.216.34") or IPv6 ("2606:2800::1", "::ffff:1.2.3.4"),
// also as Windows may report one: "[::1]:443", "1.2.3.4:443", with a "%zone". Null if it is none.
Address parseAddress(const QString& text);

// ---- addresses of websites ------------------------------------------------------------------------

// The rules of the start address and of every redirect target: https, port 443 (or none), no user info,
// a public DNS name, at most kMaxUrlChars, no fragment needed.
bool isAllowedUrl(const QUrl& url);

// The next hop of a redirect: location resolved against current (relative, "//host/..." and absolute
// forms), its fragment dropped. Invalid when the result breaks isAllowedUrl(), or location is empty,
// longer than kMaxUrlChars or has control characters.
QUrl redirectTarget(const QUrl& current, const QString& location);

// ---- the Windows proxy ----------------------------------------------------------------------------

// The proxy an https request to host goes through, from the proxy list and bypass list of the user's
// Windows settings (WinHttpGetIEProxyConfigForCurrentUser / WinHttpGetProxyForUrl), or empty for a
// direct connection.
//  * proxyList: "host:port" (every protocol), or "scheme=host:port" entries separated by ';' or white
//    space. The https entry wins, then one without a scheme; "http=", "ftp=" and "socks=" entries never
//    carry an https request (WinHTTP has no SOCKS). Several usable entries: all of them, in order,
//    "a:1;b:2" (WinHTTP tries the next when one fails).
//  * bypassList: entries separated by ';' or white space, '*' wildcards, case-insensitive, optionally
//    with "scheme://" in front or ":port" after; "<local>" means names without a dot. A host that
//    matches one is fetched directly.
// At most kMaxProxyChars of either list are read; an entry with control characters is ignored.
constexpr int kMaxProxyChars = 4096;
QString proxyForHost(const QString& proxyList, const QString& bypassList, const QString& host);
bool    bypassesProxy(const QString& bypassList, const QString& host);
// What an auto-config script (a setup script, or one found by WPAD on the local network) is asked about:
// "https://<host>/" only, as browsers do for https, never the path or query (they may hold tokens, and the
// script may come from anyone on the network). Invalid for an address that isn't allowed.
QUrl proxyQueryUrl(const QUrl& url);

// ---- YouTube --------------------------------------------------------------------------------------

// https://www.youtube.com/oembed?url=<the canonical watch address>&format=json; invalid for a bad id.
QUrl oEmbedUrl(const QString& videoId);
bool isOEmbedHop(const QUrl& url); // https://www.youtube.com/oembed... or youtube.com (a redirect between them)

// A thumbnail address oEmbed may name: https://i.ytimg.com/vi/<videoId>/<name>.jpg (or /vi_webp/<id>/<name>.webp),
// no query, no port, no user info. Anything else is never fetched.
bool isAllowedThumbnail(const QUrl& url, const QString& videoId);
bool isThumbnailHop(const QUrl& url); // a redirect while fetching a thumbnail must stay on i.ytimg.com
QUrl defaultThumbnail(const QString& videoId); // https://i.ytimg.com/vi/<id>/hqdefault.jpg

} // namespace linkpreview::net
