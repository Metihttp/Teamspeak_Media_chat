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

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

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

// 2.6: what a local DNS answer for a public name means on the direct route (design 4.3).
//  * fake-IP: 198.18.0.0/15 and fc00::/18, the defaults of Clash / mihomo and sing-box fake-IP DNS (a TUN VPN connects
//    by name at its exit);
//  * the sinkhole: 10.10.34.0/24, the addresses a filtered name gets from Iranian DNS (research_users 1.1), and nothing
//    else (S6: any other private answer stays a neutral "blocked" with no proxy offer).
bool isFakeIpAddress(const Address& address);
bool isSinkholeAddress(const Address& address);
enum class Answer {
    Public,   // every address public: load (2.5)
    FakeIp,   // public or fake-IP and at least one fake (W2): load with the proxied routes' name rules
    Filtered, // every non-public address in the sinkhole set (W3): "Your network blocks this site"
    Blocked,  // anything else (a LAN or split-horizon answer, 127.0.0.1, empty): 2.5's neutral refusal
};
Answer classifyAnswer(const QVector<Address>& addresses);

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
QString proxyForHost(const QString& proxyList, const QString& bypassList, const QString& host); // pickProxy().servers
// 2.6 (rule W1): the manual setting for host. servers: the usable HTTP proxies ("https=" entries first, then entries
// without a scheme, then "http=" ones when there is no SOCKS entry; an HTTP proxy tunnels https with CONNECT), joined
// with ';', to be handed to WinHTTP bare. socksOnly: the host isn't bypassed, no "https=" or plain entry is set, but a
// "socks=" entry or a socks5:// / socks:// server is (WinINet and Chromium use it for https before an "http=" entry):
// refused (SocksOnlyWindows), never direct. Bypassed or nothing at all: direct (both empty).
struct PickedProxy {
    QString servers;
    bool    socksOnly = false;
    QString socksServer; // the first SOCKS entry ("host:port")
};
PickedProxy pickProxy(const QString& proxyList, const QString& bypassList, const QString& host);
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
// 2.5 zoom: the 1280 x 720 picture, asked for first (sharp on high-DPI screens); some videos have none (404), or a
// small placeholder: then the standard one.
QUrl maxresThumbnail(const QString& videoId); // https://i.ytimg.com/vi/<id>/maxresdefault.jpg
constexpr int kMaxresMinWidth = 640; // a maxres answer narrower than this is YouTube's placeholder

// ---- 2.5: pages, sites and their pictures ----------------------------------------------------------------------

constexpr int    kPageTimeoutMs        = 15 * 1000;  // a whole page / oEmbed / JSON / feed / manifest / redirect request (all hops)
constexpr qint64 kPageMinBytes         = 16 * 1024;  // a page that sent less than this ...
constexpr int    kPageMinBytesAfterMs  = 5 * 1000;   // ... after this long stops (a slow drip of bytes)
constexpr qint64 kMaxPageBytes         = 512 * 1024; // the most of a page ever read (its head)
constexpr qint64 kMaxFeedBytes         = 64 * 1024;  // a Reddit RSS feed
constexpr qint64 kMaxManifestBytes     = 16 * 1024;  // a DASH manifest
constexpr qint64 kMaxJsonBytes         = kMaxOEmbedBytes; // oEmbed and the keyless JSON APIs

// The one honest User-Agent of every request (media too): "Mozilla/5.0 (compatible; TSMediaChat/<major>.<minor>;
// +https://github.com/Metihttp/Teamspeak_Media_chat)", built from TSMEDIA_VERSION.
QByteArray userAgent();

// The registrable domain of a host: its last two labels, or one more when they end in a public suffix of the
// built-in list ("co.uk", "github.io", "s3.amazonaws.com", ...). A host that is such a suffix is its own. Used for
// HostGate's health keys, the cross-site redirect check of a web card and the site-name match.
QString registrableDomain(const QString& host);

// An address a page, an oEmbed answer, a JSON API, a feed or a manifest named (og:image, thumbnail_url, ...),
// resolved against base (the final address of the response it came from; relative and "//host" forms work, <base>
// is ignored) and checked like a link: ASCII white space trimmed, TAB/CR/LF removed, any other bad character
// refused, at most kMaxUrlChars; https kept, http upgraded once to https (port 80 or none); every other scheme
// refused; no user info, no '@' or backslash in the authority, port none or 443, a public DNS name, isAllowedUrl();
// the fragment dropped; a path ending .svg / .svgz refused. Invalid when refused.
QUrl resolveExtracted(const QUrl& base, const QString& raw);

// Retry-After: delta seconds (1-9 ASCII digits) or an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT", against the
// PC's clock nowWallMs); ms, or -1 when absent or unreadable. A date in the past is 0.
qint64 parseRetryAfter(const QByteArray& value, qint64 nowWallMs);
// x-ratelimit-reset-after (Discord; seconds, decimals allowed) wins; else x-ratelimit-reset: up to 86400 is seconds
// from now (Reddit: "43"); at least 1 000 000 000 is a Unix time in seconds (GitHub, Discord; decimals allowed).
// ms from now, or -1 when absent, unreadable or past.
qint64 parseRateReset(const QByteArray& resetAfter, const QByteArray& reset, qint64 nowWallMs);

// A signed Discord attachment link (cdn.discordapp.com / media.discordapp.net with ?ex=<hex Unix time>) whose time
// is more than 60 s over: the Fetcher makes no request (errors::kExpired).
bool expiredSignedLink(const QUrl& url, qint64 nowSec);

// The PC's own DNS suffixes (each adapter's DNS suffix and suffix list, the primary domain, the search list),
// lower case, at least two labels; cached 10 minutes. Through the Windows proxy a host under one of them is never
// fetched (the proxy may reach the intranet).
QStringList localDnsSuffixes();
bool        underLocalSuffix(const QString& host, const QStringList& suffixes);

} // namespace linkpreview::net
