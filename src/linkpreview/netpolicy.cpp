#include "netpolicy.h"

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#endif

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <vector>

#include "linkurl.h"
#include "version.h"

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

QUrl maxresThumbnail(const QString& videoId)
{
    if (!isYouTubeId(videoId))
        return {};
    return QUrl(latin("https://i.ytimg.com/vi/") + videoId + latin("/maxresdefault.jpg"), QUrl::StrictMode);
}

QUrl defaultThumbnail(const QString& videoId)
{
    if (!isYouTubeId(videoId))
        return {};
    return QUrl(latin("https://i.ytimg.com/vi/") + videoId + latin("/hqdefault.jpg"), QUrl::StrictMode);
}

// ---- 2.5 --------------------------------------------------------------------------------------------------------

QByteArray userAgent()
{
    // major.minor only: the exact release isn't the site's business.
    const QString     version = QString::fromLatin1(TSMEDIA_VERSION);
    const QStringList parts   = version.split(QLatin1Char('.'));
    const QString     short_  = parts.size() >= 2 ? parts.at(0) + QLatin1Char('.') + parts.at(1) : version;
    return QByteArray("Mozilla/5.0 (compatible; TSMediaChat/") + short_.toLatin1() + QByteArray("; +https://github.com/Metihttp/Teamspeak_Media_chat)");
}

namespace {

// Public suffixes under which every name is someone else's site (a built-in list, no PSL snapshot).
const QSet<QString>& publicSuffixes()
{
    static const QSet<QString>* set = [] {
        static const char* const kList[] = {
            "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "sch.uk", "nhs.uk",
            "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp", "gr.jp", "ed.jp", "lg.jp",
            "com.au", "net.au", "org.au", "edu.au", "gov.au", "asn.au", "id.au", "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz", "geek.nz",
            "com.br", "net.br", "org.br", "gov.br", "edu.br", "com.ar", "com.co", "com.pe", "com.ve", "com.uy", "com.ec", "com.bo", "com.py",
            "gob.mx", "com.mx", "org.mx",
            "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "ac.cn", "com.tw", "org.tw", "net.tw", "idv.tw", "edu.tw", "gov.tw",
            "com.hk", "org.hk", "net.hk", "edu.hk", "gov.hk",
            "com.sg", "edu.sg", "gov.sg", "com.my", "edu.my", "gov.my", "com.ph", "edu.ph", "gov.ph", "com.vn", "edu.vn", "gov.vn",
            "co.th", "in.th", "ac.th", "go.th",
            "co.id", "or.id", "ac.id", "go.id", "web.id", "co.in", "net.in", "org.in", "gov.in", "ac.in", "edu.in", "res.in",
            "co.kr", "or.kr", "ne.kr", "ac.kr", "go.kr",
            "com.tr", "net.tr", "org.tr", "gov.tr", "edu.tr", "com.ua", "org.ua", "net.ua", "co.ua", "gov.ua",
            "co.za", "org.za", "gov.za", "ac.za", "co.il", "org.il", "ac.il", "gov.il",
            "co.ir", "ac.ir", "org.ir", "gov.ir", "net.ir", "id.ir", "sch.ir", "com.eg", "edu.eg", "gov.eg", "com.sa", "edu.sa", "gov.sa",
            "com.pk", "edu.pk", "gov.pk", "com.ng", "gov.ng", "co.ke", "or.ke", "com.pl", "net.pl", "org.pl", "co.at", "or.at",
            "com.es", "com.pt", "com.gr", "com.cy", "com.mt", "com.ru", "org.ru",
            "github.io", "gitlab.io", "netlify.app", "vercel.app", "pages.dev", "workers.dev", "herokuapp.com", "web.app", "firebaseapp.com",
            "appspot.com", "azurewebsites.net", "cloudfront.net", "blogspot.com", "wordpress.com", "wixsite.com", "substack.com",
            "neocities.org", "glitch.me", "onrender.com", "fly.dev", "ngrok.io", "ngrok-free.app", "duckdns.org", "ddns.net", "no-ip.org",
            "myshopify.com", "carrd.co", "notion.site", "s3.amazonaws.com"};
        auto* out = new QSet<QString>; // never deleted (plain data)
        for (const char* suffix : kList)
            out->insert(QString::fromLatin1(suffix));
        return out;
    }();
    return *set;
}

bool isAsciiDigits(const QByteArray& text, int minDigits, int maxDigits)
{
    if (text.size() < minDigits || text.size() > maxDigits)
        return false;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

// "123", "123.45" (no sign, no exponent); false otherwise.
bool parseDecimal(const QByteArray& text, double* out)
{
    const QByteArray t     = text.trimmed();
    const int        dot   = t.indexOf('.');
    const QByteArray whole = dot < 0 ? t : t.left(dot);
    const QByteArray frac  = dot < 0 ? QByteArray() : t.mid(dot + 1);
    if (!isAsciiDigits(whole, 1, 12) || (dot >= 0 && !isAsciiDigits(frac, 1, 9)))
        return false;
    bool         ok    = false;
    const double value = t.toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return false;
    *out = value;
    return true;
}

} // namespace

QString registrableDomain(const QString& host)
{
    QString h = host.toLower();
    if (h.endsWith(QLatin1Char('.')))
        h.chop(1);
    const QStringList labels = h.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() <= 2)
        return labels.join(QLatin1Char('.'));
    // The longest listed suffix the host ends with (the list has two- and three-label entries).
    int suffixLabels = 1;
    for (int n = qMin(3, labels.size()); n >= 2; --n) {
        if (publicSuffixes().contains(labels.mid(labels.size() - n).join(QLatin1Char('.')))) {
            suffixLabels = n;
            break;
        }
    }
    const int keep = qMin(labels.size(), suffixLabels + 1);
    return labels.mid(labels.size() - keep).join(QLatin1Char('.'));
}

QUrl resolveExtracted(const QUrl& base, const QString& raw)
{
    // 1. ASCII white space trimmed, TAB / CR / LF removed (as browsers do), anything else odd refused.
    const auto isAsciiWhite = [](QChar ch) {
        return ch == QLatin1Char(' ') || ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\f');
    };
    QString text = raw;
    while (!text.isEmpty() && isAsciiWhite(text.at(0)))
        text.remove(0, 1);
    while (!text.isEmpty() && isAsciiWhite(text.at(text.size() - 1)))
        text.chop(1);
    text.remove(QLatin1Char('\t'));
    text.remove(QLatin1Char('\r'));
    text.remove(QLatin1Char('\n'));
    if (text.isEmpty() || text.size() > kMaxUrlChars)
        return {};
    for (const QChar ch : text) {
        if (isBadUrlChar(ch))
            return {};
    }
    // The authority as written ("//host/..." or "scheme://host/..."): no user info, no backslash.
    int       authorityStart = -1;
    const int schemeEnd      = text.indexOf(QLatin1String("://"));
    if (text.startsWith(QLatin1String("//")))
        authorityStart = 2;
    else if (schemeEnd > 0 && schemeEnd < 16)
        authorityStart = schemeEnd + 3;
    if (authorityStart >= 0) {
        int end = text.size();
        for (const char stop : {'/', '?', '#'}) {
            const int at = text.indexOf(QLatin1Char(stop), authorityStart);
            if (at >= 0 && at < end)
                end = at;
        }
        const QString authority = text.mid(authorityStart, end - authorityStart);
        if (authority.contains(QLatin1Char('@')) || authority.contains(QLatin1Char('\\')))
            return {};
    }
    // 2. Resolved against the response's own address.
    const QUrl relative(text, QUrl::TolerantMode);
    if (!relative.isValid())
        return {};
    QUrl url = base.resolved(relative);
    if (!url.isValid() || url.isRelative())
        return {};
    // 3. https; http upgraded once; anything else refused.
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http")) {
        const int port = url.port(-1);
        if (port != -1 && port != 80)
            return {};
        url.setScheme(latin("https"));
        url.setPort(-1);
    } else if (scheme != QLatin1String("https")) {
        return {};
    }
    // 4. A public host, the standard port, no user info; no fragment.
    url.setFragment(QString());
    if (!url.userInfo().isEmpty() || !isAllowedUrl(url))
        return {};
    // 5. Never an SVG.
    const QString path = url.path(QUrl::FullyDecoded).toLower();
    if (path.endsWith(QLatin1String(".svg")) || path.endsWith(QLatin1String(".svgz")))
        return {};
    return url;
}

qint64 parseRetryAfter(const QByteArray& value, qint64 nowWallMs)
{
    const QByteArray text = value.trimmed();
    if (text.isEmpty())
        return -1;
    if (isAsciiDigits(text, 1, 9))
        return text.toLongLong() * 1000;
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    QDateTime when = QLocale::c().toDateTime(QString::fromLatin1(text), latin("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!when.isValid())
        return -1;
    when.setTimeSpec(Qt::UTC);
    return qMax<qint64>(0, when.toMSecsSinceEpoch() - nowWallMs);
}

qint64 parseRateReset(const QByteArray& resetAfter, const QByteArray& reset, qint64 nowWallMs)
{
    double value = 0;
    if (!resetAfter.trimmed().isEmpty() && parseDecimal(resetAfter, &value))
        return static_cast<qint64>(std::ceil(value * 1000.0));
    if (reset.trimmed().isEmpty() || !parseDecimal(reset, &value))
        return -1;
    if (value <= 86400.0)
        return static_cast<qint64>(std::ceil(value * 1000.0));
    if (value >= 1000000000.0) {
        const qint64 ms = static_cast<qint64>(std::ceil(value * 1000.0)) - nowWallMs;
        return ms > 0 ? ms : -1;
    }
    return -1;
}

bool expiredSignedLink(const QUrl& url, qint64 nowSec)
{
    const QString host = publicHostName(url);
    if (host != QLatin1String("cdn.discordapp.com") && host != QLatin1String("media.discordapp.net"))
        return false;
    const QStringList items = url.query(QUrl::FullyEncoded).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString& item : items) {
        if (!item.startsWith(QLatin1String("ex=")))
            continue;
        const QString hex = item.mid(3);
        if (hex.isEmpty() || hex.size() > 16)
            return false;
        bool          ok   = false;
        const quint64 time = hex.toULongLong(&ok, 16);
        if (!ok)
            return false;
        return static_cast<qint64>(qMin<quint64>(time, quint64(1) << 62)) < nowSec - 60;
    }
    return false;
}

bool underLocalSuffix(const QString& host, const QStringList& suffixes)
{
    const QString h = host.toLower();
    for (const QString& suffix : suffixes) {
        if (suffix.isEmpty())
            continue;
        if (h == suffix || h.endsWith(QLatin1Char('.') + suffix))
            return true;
    }
    return false;
}

namespace {

void addSuffix(QStringList* out, QString suffix)
{
    suffix = suffix.trimmed().toLower();
    while (suffix.endsWith(QLatin1Char('.')))
        suffix.chop(1);
    while (suffix.startsWith(QLatin1Char('.')))
        suffix.remove(0, 1);
    if (suffix.count(QLatin1Char('.')) < 1 || suffix.size() > 253 || out->contains(suffix))
        return; // at least two labels: never a whole top-level domain
    for (const QChar ch : suffix) {
        const ushort u = ch.unicode();
        if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_'))
            return;
    }
    out->append(suffix);
}

QStringList readLocalDnsSuffixes()
{
    QStringList out;
#ifdef Q_OS_WIN
    HMODULE dll = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll) {
        using GetAdaptersAddressesFn = ULONG(WINAPI*)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
        using GetNetworkParamsFn     = DWORD(WINAPI*)(PFIXED_INFO, PULONG);
        const auto adapters = reinterpret_cast<GetAdaptersAddressesFn>(reinterpret_cast<void*>(GetProcAddress(dll, "GetAdaptersAddresses")));
        const auto params   = reinterpret_cast<GetNetworkParamsFn>(reinterpret_cast<void*>(GetProcAddress(dll, "GetNetworkParams")));
        if (adapters) {
            ULONG             size = 16 * 1024;
            std::vector<char> buffer;
            ULONG             rc = ERROR_BUFFER_OVERFLOW;
            for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
                buffer.resize(size);
                rc = adapters(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                              reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
            }
            if (rc == NO_ERROR) {
                for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); a; a = a->Next) {
                    if (a->DnsSuffix && *a->DnsSuffix)
                        addSuffix(&out, QString::fromWCharArray(a->DnsSuffix));
                    if (a->Length >= sizeof(IP_ADAPTER_ADDRESSES)) {
                        for (auto* s = a->FirstDnsSuffix; s; s = s->Next)
                            addSuffix(&out, QString::fromWCharArray(s->String));
                    }
                }
            }
        }
        if (params) {
            ULONG             size = sizeof(FIXED_INFO);
            std::vector<char> buffer(size);
            DWORD             rc = params(reinterpret_cast<PFIXED_INFO>(buffer.data()), &size);
            if (rc == ERROR_BUFFER_OVERFLOW) {
                buffer.resize(size);
                rc = params(reinterpret_cast<PFIXED_INFO>(buffer.data()), &size);
            }
            if (rc == NO_ERROR)
                addSuffix(&out, QString::fromLocal8Bit(reinterpret_cast<PFIXED_INFO>(buffer.data())->DomainName));
        }
        FreeLibrary(dll);
    }
    wchar_t buffer[2048] = {};
    DWORD   bytes        = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"SearchList", RRF_RT_REG_SZ, nullptr, buffer,
                     &bytes)
        == ERROR_SUCCESS) {
        for (const QString& entry : QString::fromWCharArray(buffer).split(QLatin1Char(','), Qt::SkipEmptyParts))
            addSuffix(&out, entry);
    }
#endif
    return out;
}

} // namespace

QStringList localDnsSuffixes()
{
    static QMutex        mutex;
    static QStringList   cached;
    static qint64        readAt = -1;
    static QElapsedTimer clock;
    QMutexLocker         lock(&mutex);
    if (!clock.isValid())
        clock.start();
    if (readAt < 0 || clock.elapsed() - readAt > 10 * 60 * 1000) {
        cached = readLocalDnsSuffixes();
        readAt = clock.elapsed();
    }
    return cached;
}

} // namespace linkpreview::net
