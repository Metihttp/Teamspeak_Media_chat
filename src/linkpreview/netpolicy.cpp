#include "netpolicy.h"

#include <QRegularExpression>
#include <QStringList>
#include <QVector>

#include "linkurl.h"

namespace linkpreview::net {

namespace {

struct Range4 {
    quint32 base;
    int     prefix;
};

constexpr quint32 ip4(int a, int b, int c, int d)
{
    return (static_cast<quint32>(a) << 24) | (static_cast<quint32>(b) << 16) | (static_cast<quint32>(c) << 8) | static_cast<quint32>(d);
}

// Not reachable on the public internet, or not meant to be (IANA special-purpose registry).
constexpr Range4 kRefused4[] = {
    {ip4(0, 0, 0, 0), 8},       // "this network", 0.0.0.0
    {ip4(10, 0, 0, 0), 8},      // private
    {ip4(100, 64, 0, 0), 10},   // carrier-grade NAT
    {ip4(127, 0, 0, 0), 8},     // loopback
    {ip4(169, 254, 0, 0), 16},  // link-local
    {ip4(172, 16, 0, 0), 12},   // private
    {ip4(192, 0, 0, 0), 24},    // IETF protocol assignments
    {ip4(192, 0, 2, 0), 24},    // documentation (TEST-NET-1)
    {ip4(192, 88, 99, 0), 24},  // 6to4 relay anycast (deprecated)
    {ip4(192, 168, 0, 0), 16},  // private
    {ip4(198, 18, 0, 0), 15},   // benchmarking
    {ip4(198, 51, 100, 0), 24}, // documentation (TEST-NET-2)
    {ip4(203, 0, 113, 0), 24},  // documentation (TEST-NET-3)
    {ip4(224, 0, 0, 0), 4},     // multicast
    {ip4(240, 0, 0, 0), 4},     // reserved, and 255.255.255.255 broadcast
};

bool inRange(quint32 address, const Range4& range)
{
    const quint32 mask = range.prefix == 0 ? 0u : ~0u << (32 - range.prefix);
    return (address & mask) == (range.base & mask);
}

using Bytes16 = std::array<quint8, 16>;

quint32 ip4At(const Bytes16& a, int offset)
{
    return (static_cast<quint32>(a[offset]) << 24) | (static_cast<quint32>(a[offset + 1]) << 16) | (static_cast<quint32>(a[offset + 2]) << 8)
           | static_cast<quint32>(a[offset + 3]);
}

bool allZero(const Bytes16& a, int from, int to)
{
    for (int i = from; i < to; ++i) {
        if (a[i] != 0)
            return false;
    }
    return true;
}

bool isPublicIPv6(const Bytes16& a)
{
    // IPv4-mapped ::ffff:a.b.c.d (what a dual-stack socket reports for IPv4)
    if (allZero(a, 0, 10) && a[10] == 0xff && a[11] == 0xff)
        return isPublicIPv4(ip4At(a, 12));
    // NAT64 64:ff9b::a.b.c.d (IPv6-only networks reach IPv4 websites this way)
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b && allZero(a, 4, 12))
        return isPublicIPv4(ip4At(a, 12));
    // 6to4 2002:AABB:CCDD::/48 carries the IPv4 address AA.BB.CC.DD
    if (a[0] == 0x20 && a[1] == 0x02)
        return isPublicIPv4(ip4At(a, 2));
    // Only global unicast 2000::/3 from here (refuses ::, ::1, IPv4-compatible ::a.b.c.d, ULA fc00::/7,
    // link-local fe80::/10, site-local fec0::/10, multicast ff00::/8, discard 100::/64, 64:ff9b:1::/48 ...).
    if ((a[0] & 0xE0) != 0x20)
        return false;
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] <= 0x01)
        return false; // 2001::/23 IETF protocol assignments (Teredo, benchmarking, ORCHID)
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8)
        return false; // 2001:db8::/32 documentation
    if (a[0] == 0x3f && a[1] == 0xff && (a[2] & 0xF0) == 0x00)
        return false; // 3fff::/20 documentation
    return true;
}

bool hasControls(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F))
            return true;
    }
    return false;
}

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

// Strict dotted quad: four decimal parts 0..255, no leading zeros, nothing else.
bool parseIPv4(const QString& text, quint32* out)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    quint32 value = 0;
    for (const QString& part : parts) {
        if (part.isEmpty() || part.size() > 3 || (part.size() > 1 && part.at(0) == QLatin1Char('0')))
            return false;
        int number = 0;
        for (const QChar ch : part) {
            if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
                return false;
            number = number * 10 + (ch.unicode() - '0');
        }
        if (number > 255)
            return false;
        value = (value << 8) | static_cast<quint32>(number);
    }
    *out = value;
    return true;
}

// Colon-separated hex groups (1-4 digits each); the last may be a dotted quad when allowed.
bool parseGroups(const QString& text, QVector<quint16>* groups, bool allowIPv4)
{
    if (text.isEmpty())
        return true;
    const QStringList items = text.split(QLatin1Char(':'));
    for (int i = 0; i < items.size(); ++i) {
        const QString& item = items.at(i);
        if (allowIPv4 && i == items.size() - 1 && item.contains(QLatin1Char('.'))) {
            quint32 v4 = 0;
            if (!parseIPv4(item, &v4))
                return false;
            groups->append(static_cast<quint16>(v4 >> 16));
            groups->append(static_cast<quint16>(v4 & 0xFFFF));
            continue;
        }
        if (item.isEmpty() || item.size() > 4)
            return false;
        uint value = 0;
        for (const QChar ch : item) {
            const ushort u = ch.unicode();
            int          digit;
            if (u >= '0' && u <= '9')
                digit = u - '0';
            else if (u >= 'a' && u <= 'f')
                digit = u - 'a' + 10;
            else if (u >= 'A' && u <= 'F')
                digit = u - 'A' + 10;
            else
                return false;
            value = value * 16 + static_cast<uint>(digit);
        }
        groups->append(static_cast<quint16>(value));
    }
    return true;
}

bool parseIPv6(const QString& text, Bytes16* out)
{
    const int gap = text.indexOf(QLatin1String("::"));
    if (gap >= 0 && text.indexOf(QLatin1String("::"), gap + 1) >= 0)
        return false; // ":::" or two gaps
    QVector<quint16> head;
    QVector<quint16> tail;
    if (gap < 0) {
        if (!parseGroups(text, &head, true) || head.size() != 8)
            return false;
    } else {
        if (!parseGroups(text.left(gap), &head, false) || !parseGroups(text.mid(gap + 2), &tail, true))
            return false;
        if (head.size() + tail.size() > 7)
            return false;
    }
    QVector<quint16> groups = head;
    while (groups.size() + tail.size() < 8)
        groups.append(0);
    groups += tail;
    for (int i = 0; i < 8; ++i) {
        (*out)[static_cast<size_t>(2 * i)]     = static_cast<quint8>(groups.at(i) >> 8);
        (*out)[static_cast<size_t>(2 * i + 1)] = static_cast<quint8>(groups.at(i) & 0xFF);
    }
    return true;
}

} // namespace

quint32 Address::ipv4() const
{
    return (static_cast<quint32>(bytes[0]) << 24) | (static_cast<quint32>(bytes[1]) << 16) | (static_cast<quint32>(bytes[2]) << 8) | bytes[3];
}

QString Address::toString() const
{
    if (family == Family::IPv4)
        return QStringLiteral("%1.%2.%3.%4").arg(bytes[0]).arg(bytes[1]).arg(bytes[2]).arg(bytes[3]);
    if (family == Family::IPv6) {
        QStringList groups;
        for (int i = 0; i < 8; ++i)
            groups << QString::number((bytes[static_cast<size_t>(2 * i)] << 8) | bytes[static_cast<size_t>(2 * i + 1)], 16);
        return groups.join(QLatin1Char(':'));
    }
    return {};
}

Address Address::fromIPv4(quint32 hostOrder)
{
    Address address;
    address.family   = Family::IPv4;
    address.bytes[0] = static_cast<quint8>(hostOrder >> 24);
    address.bytes[1] = static_cast<quint8>(hostOrder >> 16);
    address.bytes[2] = static_cast<quint8>(hostOrder >> 8);
    address.bytes[3] = static_cast<quint8>(hostOrder);
    return address;
}

Address Address::fromIPv6(const quint8* sixteenBytes)
{
    Address address;
    address.family = Family::IPv6;
    for (size_t i = 0; i < 16; ++i)
        address.bytes[i] = sixteenBytes[i];
    return address;
}

bool isPublicIPv4(quint32 address)
{
    for (const Range4& range : kRefused4) {
        if (inRange(address, range))
            return false;
    }
    return true;
}

bool isPublicAddress(const Address& address)
{
    switch (address.family) {
    case Address::Family::IPv4:
        return isPublicIPv4(address.ipv4());
    case Address::Family::IPv6:
        return isPublicIPv6(address.bytes);
    case Address::Family::None:
        break;
    }
    return false;
}

Address parseAddress(const QString& text)
{
    QString s = text.trimmed();
    if (s.isEmpty() || s.size() > 100)
        return {};
    if (s.startsWith(QLatin1Char('['))) {
        const int end = s.indexOf(QLatin1Char(']'));
        if (end < 0)
            return {};
        s = s.mid(1, end - 1);
    } else if (s.count(QLatin1Char(':')) == 1) {
        s = s.left(s.indexOf(QLatin1Char(':'))); // "1.2.3.4:443"
    }
    const int zone = s.indexOf(QLatin1Char('%'));
    if (zone >= 0)
        s = s.left(zone);
    quint32 v4 = 0;
    if (parseIPv4(s, &v4))
        return Address::fromIPv4(v4);
    Bytes16 v6{};
    if (s.contains(QLatin1Char(':')) && parseIPv6(s, &v6))
        return Address::fromIPv6(v6.data());
    return {};
}

bool isAllowedUrl(const QUrl& url)
{
    if (!url.isValid() || url.isRelative() || url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
        return false;
    if (!url.userInfo().isEmpty() || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')))
        return false;
    const int port = url.port(-1);
    if (port != -1 && port != 443)
        return false;
    if (url.toString(QUrl::FullyEncoded).size() > kMaxUrlChars)
        return false;
    if (hasControls(url.path(QUrl::FullyDecoded)) || hasControls(url.query(QUrl::FullyDecoded)))
        return false;
    return !publicHostName(url).isEmpty();
}

QUrl redirectTarget(const QUrl& current, const QString& location)
{
    const QString text = location.trimmed();
    if (text.isEmpty() || text.size() > kMaxUrlChars || hasControls(text))
        return {};
    const QUrl relative(text, QUrl::TolerantMode);
    if (!relative.isValid())
        return {};
    QUrl next = current.resolved(relative);
    next.setFragment(QString());
    if (!isAllowedUrl(next))
        return {};
    return next;
}

// ---- the Windows proxy ----------------------------------------------------------------------------

namespace {

QStringList proxyEntries(const QString& list)
{
    QStringList out;
    QString     current;
    const QString text = list.left(kMaxProxyChars);
    for (const QChar ch : text) {
        if (ch == QLatin1Char(';') || ch.isSpace()) {
            if (!current.isEmpty())
                out << current;
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.isEmpty())
        out << current;
    QStringList clean;
    for (const QString& entry : qAsConst(out)) {
        if (!hasControls(entry))
            clean << entry;
    }
    return clean;
}

// '*' matches any run of characters (also none); everything else exactly, case-insensitive.
bool wildcardMatch(const QString& pattern, const QString& text)
{
    int p = 0, t = 0, star = -1, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern.at(p) != QLatin1Char('*') && pattern.at(p).toLower() == text.at(t).toLower()) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            star = p++;
            mark = t;
        } else if (star >= 0) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

} // namespace

bool bypassesProxy(const QString& bypassList, const QString& host)
{
    if (host.isEmpty())
        return false;
    for (QString entry : proxyEntries(bypassList)) {
        if (entry.compare(QLatin1String("<local>"), Qt::CaseInsensitive) == 0) {
            if (!host.contains(QLatin1Char('.')))
                return true;
            continue;
        }
        const int scheme = entry.indexOf(QLatin1String("://"));
        if (scheme >= 0)
            entry = entry.mid(scheme + 3);
        const int colon = entry.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && !entry.contains(QLatin1Char(']')))
            entry = entry.left(colon); // a port (IPv6 entries never match a host name anyway)
        while (entry.endsWith(QLatin1Char('/')))
            entry.chop(1);
        if (!entry.isEmpty() && wildcardMatch(entry, host))
            return true;
    }
    return false;
}

QString proxyForHost(const QString& proxyList, const QString& bypassList, const QString& host)
{
    if (host.isEmpty() || bypassesProxy(bypassList, host))
        return {};
    QStringList https, any;
    for (const QString& entry : proxyEntries(proxyList)) {
        const int eq = entry.indexOf(QLatin1Char('='));
        QString   server;
        if (eq < 0) {
            server = entry;
        } else {
            const QString scheme = entry.left(eq).trimmed().toLower();
            if (scheme != QLatin1String("https"))
                continue; // http=, ftp=, socks=: not for an https request
            server = entry.mid(eq + 1);
        }
        // "http://host:port" as some tools write it: WinHTTP takes "host:port".
        const int s = server.indexOf(QLatin1String("://"));
        if (s >= 0) {
            const QString scheme = server.left(s).toLower();
            if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
                continue; // socks5://...
            server = server.mid(s + 3);
        }
        while (server.endsWith(QLatin1Char('/')))
            server.chop(1);
        if (server.isEmpty() || server.contains(QLatin1Char('/')) || server.contains(QLatin1Char('@')))
            continue;
        (eq < 0 ? any : https) << server;
    }
    const QStringList use = https.isEmpty() ? any : https;
    return use.join(QLatin1Char(';'));
}

QUrl proxyQueryUrl(const QUrl& url)
{
    if (!isAllowedUrl(url))
        return {};
    return QUrl(latin("https://") + publicHostName(url) + QLatin1Char('/'), QUrl::StrictMode);
}

QUrl oEmbedUrl(const QString& videoId)
{
    const QUrl watch = youTubeWatchUrl(videoId);
    if (!watch.isValid())
        return {};
    const QByteArray encoded = QUrl::toPercentEncoding(watch.toString(QUrl::FullyEncoded));
    return QUrl(latin("https://www.youtube.com/oembed?url=") + QString::fromLatin1(encoded) + latin("&format=json"), QUrl::StrictMode);
}

bool isOEmbedHop(const QUrl& url)
{
    if (!isAllowedUrl(url))
        return false;
    const QString host = publicHostName(url);
    return (host == QLatin1String("www.youtube.com") || host == QLatin1String("youtube.com")) && url.path(QUrl::FullyEncoded) == QLatin1String("/oembed");
}

bool isAllowedThumbnail(const QUrl& url, const QString& videoId)
{
    if (!isYouTubeId(videoId) || !isThumbnailHop(url) || url.hasQuery() || url.hasFragment())
        return false;
    const QString path = url.path(QUrl::FullyEncoded);
    if (path != url.path(QUrl::FullyDecoded))
        return false; // no escapes in a thumbnail path
    const QRegularExpression jpg(latin("^/vi/([A-Za-z0-9_-]{11})/[A-Za-z0-9_]{1,32}\\.jpg$"));
    const QRegularExpression webp(latin("^/vi_webp/([A-Za-z0-9_-]{11})/[A-Za-z0-9_]{1,32}\\.webp$"));
    QRegularExpressionMatch match = jpg.match(path);
    if (!match.hasMatch())
        match = webp.match(path);
    return match.hasMatch() && match.captured(1) == videoId;
}

bool isThumbnailHop(const QUrl& url)
{
    return isAllowedUrl(url) && publicHostName(url) == QLatin1String("i.ytimg.com");
}

QUrl defaultThumbnail(const QString& videoId)
{
    if (!isYouTubeId(videoId))
        return {};
    return QUrl(latin("https://i.ytimg.com/vi/") + videoId + latin("/hqdefault.jpg"), QUrl::StrictMode);
}

} // namespace linkpreview::net
