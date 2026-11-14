#include "linkurl.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>

#include <list>
#include <utility>

#include "providers.h"

namespace linkpreview {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

constexpr ushort kZwnj = 0x200C;
constexpr ushort kZwj  = 0x200D;

bool hasBadChars(const QString& text)
{
    for (const QChar ch : text) {
        if (isBadUrlChar(ch))
            return true;
    }
    return false;
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

bool isHexDigit(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

int hexValue(QChar ch)
{
    const ushort u = ch.unicode();
    if (u >= '0' && u <= '9')
        return u - '0';
    if (u >= 'a' && u <= 'f')
        return u - 'a' + 10;
    return u - 'A' + 10;
}

bool isUnreserved(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

// A fully encoded component with its percent-encoding made canonical: "%7e" -> "~" (unreserved
// characters decoded), every other escape with upper-case hex digits.
QString normalizePercent(const QString& encoded)
{
    QString out;
    out.reserve(encoded.size());
    for (int i = 0; i < encoded.size(); ++i) {
        const QChar ch = encoded.at(i);
        if (ch == QLatin1Char('%') && i + 2 < encoded.size() && isHexDigit(encoded.at(i + 1)) && isHexDigit(encoded.at(i + 2))) {
            const int value = hexValue(encoded.at(i + 1)) * 16 + hexValue(encoded.at(i + 2));
            if (isUnreserved(value)) {
                out += QChar(value);
            } else {
                out += QLatin1Char('%');
                out += encoded.at(i + 1).toUpper();
                out += encoded.at(i + 2).toUpper();
            }
            i += 2;
            continue;
        }
        out += ch;
    }
    return out;
}

bool isDigits(const QString& text)
{
    if (text.isEmpty())
        return false;
    for (const QChar ch : text) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return false;
    }
    return true;
}

// A label a browser would read as a number (WHATWG's IPv4 parser): decimal, octal or "0x" hex.
bool isNumericLabel(const QString& label)
{
    if (isDigits(label))
        return true;
    if (label.startsWith(QLatin1String("0x"))) {
        for (int i = 2; i < label.size(); ++i) {
            if (!isHexDigit(label.at(i)))
                return false;
        }
        return true;
    }
    return false;
}

bool isLocalSuffix(const QStringList& labels)
{
    static const char* const kLocalTlds[] = {"localhost", "local", "lan",  "internal", "intranet", "home", "corp", "private",
                                             "localdomain", "test", "invalid", "example", "onion", "arpa", "home-network",
                                             "alt"}; // 2.6: alt (RFC 9476, non-DNS names)
    const QString& tld = labels.last();
    for (const char* name : kLocalTlds) {
        if (tld == QLatin1String(name))
            return true;
    }
    return labels.first() == QLatin1String("localhost");
}

bool isYouTubeHost(const QString& host)
{
    return host == QLatin1String("youtube.com") || host == QLatin1String("www.youtube.com") || host == QLatin1String("m.youtube.com")
           || host == QLatin1String("music.youtube.com");
}

// "/<id>" or "/<id>/" after prefix (prefix "/" for youtu.be, "/shorts/" for Shorts).
QString idAfter(const QString& path, const QString& prefix)
{
    if (!path.startsWith(prefix))
        return {};
    QString rest = path.mid(prefix.size());
    if (rest.endsWith(QLatin1Char('/')))
        rest.chop(1);
    return isYouTubeId(rest) ? rest : QString();
}

QString youTubeId(const QUrl& url, const QString& host)
{
    const QString path = url.path(QUrl::FullyDecoded);
    if (host == QLatin1String("youtu.be"))
        return idAfter(path, latin("/"));
    if (!isYouTubeHost(host))
        return {};
    if (path == QLatin1String("/watch") || path == QLatin1String("/watch/")) {
        const QUrlQuery query(url);
        const QString   id = query.queryItemValue(latin("v"), QUrl::FullyDecoded);
        return isYouTubeId(id) ? id : QString();
    }
    return idAfter(path, latin("/shorts/"));
}

bool isOwnLink(const QUrl& url, const QString& host)
{
    // The "plugin required" note's default address and the newer-version hint ("releases/latest#tsmupd-...").
    if (url.fragment(QUrl::FullyDecoded).contains(QLatin1String("tsmupd-")))
        return true;
    if (host != QLatin1String("github.com") && host != QLatin1String("www.github.com"))
        return false;
    const QString path = url.path(QUrl::FullyDecoded);
    return path.compare(QLatin1String("/Metihttp/Teamspeak_Media_chat"), Qt::CaseInsensitive) == 0
           || path.startsWith(QLatin1String("/Metihttp/Teamspeak_Media_chat/"), Qt::CaseInsensitive);
}

QString lastSegmentOf(const QUrl& url)
{
    const QString path = url.path(QUrl::FullyDecoded);
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString extensionOf(const QString& lastSegment)
{
    const int dot = lastSegment.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return {};
    return lastSegment.mid(dot + 1).toLower();
}

// 2.4.0's classify(QUrl), byte for byte (except that a YouTube host without a video id is "not YouTube" instead of
// "never a preview": the 2.5 classes then decide).
Target classify24(const QUrl& url)
{
    Target none;
    if (!url.isValid() || url.isRelative())
        return none;
    // http only for YouTube: TeamSpeak links "www.youtube.com/..." typed without a scheme as http://, and a
    // YouTube fetch never uses the link's own address (always the canonical https oEmbed and i.ytimg.com).
    const bool secure = url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    const bool plain  = url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0;
    if (!secure && !plain)
        return none;
    if (!url.userInfo().isEmpty() || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')))
        return none; // "https://youtube.com@evil.example/": whose site is it?
    const int port = url.port(-1);
    if (port != -1 && port != (secure ? 443 : 80))
        return none;
    if (url.toString(QUrl::FullyEncoded).size() > kMaxUrlChars)
        return none;
    if (hasControls(url.path(QUrl::FullyDecoded)) || hasControls(url.query(QUrl::FullyDecoded)))
        return none;
    const QString host = publicHostName(url);
    if (host.isEmpty() || isOwnLink(url, host))
        return none;

    if (host == QLatin1String("youtu.be") || isYouTubeHost(host)) {
        const QString id = youTubeId(url, host);
        if (id.isEmpty())
            return none; // a channel, a playlist, a search: not a video (2.5: maybe a Site or Web link)
        Target target;
        target.kind    = Kind::YouTube;
        target.videoId = id;
        target.url     = youTubeWatchUrl(id);
        target.host    = host;
        target.key     = keyOf(latin("yt:") + id);
        return target;
    }

    if (!secure)
        return none; // media files: https only (the file itself is fetched from that address)
    const Kind kind = mediaKindOfName(lastSegmentOf(url));
    if (kind == Kind::None)
        return none;

    QString path = normalizePercent(url.path(QUrl::FullyEncoded));
    if (path.isEmpty())
        path = latin("/");
    QString normalized = latin("https://") + host + path;
    const QString query = normalizePercent(url.query(QUrl::FullyEncoded));
    if (!query.isEmpty())
        normalized += QLatin1Char('?') + query;
    if (normalized.size() > kMaxUrlChars)
        return none;

    Target target;
    target.kind = kind;
    target.url  = QUrl(normalized, QUrl::StrictMode);
    if (!target.url.isValid())
        return none;
    target.host = host;
    target.key  = keyOf(normalized);
    return target;
}

// classify(): the 2.4.0 rules first (unless the link has a joiner: then never a legacy link), then the 2.5 classes.
Target classifyImpl(const QUrl& url, bool legacyAllowed)
{
    Target none;
    if (legacyAllowed) {
        Target legacy = classify24(url);
        if (legacy.isValid()) {
            legacy.cls = legacy.kind == Kind::YouTube ? LinkClass::YouTube : LinkClass::Direct;
            if (legacy.cls == LinkClass::Direct)
                legacy.provider = providers::directRewriteOf(legacy.url, legacy.host);
            return legacy;
        }
    }

    // Gates of the new classes.
    if (!url.isValid() || url.isRelative())
        return none;
    const bool secure = url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    const bool plain  = url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0;
    if (!secure && !plain)
        return none;
    if (!url.userInfo().isEmpty() || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')))
        return none;
    const int port = url.port(-1);
    if (port != -1 && port != (secure ? 443 : 80))
        return none;
    if (url.toString(QUrl::FullyEncoded).size() > kMaxUrlChars)
        return none;
    if (hasControls(url.path(QUrl::FullyDecoded)) || hasControls(url.query(QUrl::FullyDecoded)))
        return none;
    const QString host = publicHostName(url);
    if (host.isEmpty() || isOwnLink(url, host))
        return none;
    const QUrl page = normalizedPage(url);
    if (!page.isValid())
        return none;

    Target target;
    const providers::Match m = providers::match(url, host);
    if (!m.id.isEmpty()) {
        if (m.cls == LinkClass::None)
            return none; // an Exclude row (or a provider link that is a download or an action): no request ever
        target.cls              = m.cls;
        target.kind             = m.expected;
        target.provider         = m.id;
        target.itemId           = m.itemId;
        target.sub              = m.sub;
        target.expectedRequests = m.expectedRequests;
    } else {
        const Kind mediaKind = mediaKindOfName(lastSegmentOf(url));
        if (mediaKind != Kind::None && (plain || !legacyAllowed)) {
            // A media file that 2.4.0 doesn't take (an http link, a joiner in its path): the built-in row media-file.
            target.cls      = LinkClass::MediaPage;
            target.kind     = mediaKind;
            target.provider = latin("media-file");
        } else {
            if (hasNonHtmlExtension(url) || isActionLike(url))
                return none;
            target.cls              = LinkClass::Web;
            target.kind             = Kind::Card;
            target.expectedRequests = 2;
        }
    }
    target.host     = host;
    target.page     = page;
    target.url      = page;
    target.fetchUrl = withoutTracking(page);
    target.key      = keyOf(latin("page:") + page.toString(QUrl::FullyEncoded));
    return target;
}

// Characters a token-like path segment consists of ([A-Za-z0-9]); anything else ends a run.
bool isAlnum(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9');
}

// A run of at least 24 letters and digits holding both, or of at least 32 hex digits.
bool hasTokenRun(const QString& encodedSegment)
{
    int  run = 0, hexRun = 0;
    bool letter = false, digit = false;
    const auto ends = [&]() {
        const bool token = (run >= 24 && letter && digit) || hexRun >= 32;
        run = hexRun = 0;
        letter = digit = false;
        return token;
    };
    for (const QChar ch : encodedSegment) {
        if (!isAlnum(ch)) {
            if (ends())
                return true;
            continue;
        }
        ++run;
        if (ch.isDigit())
            digit = true;
        else
            letter = true;
        if (isHexDigit(ch))
            ++hexRun;
        else if (hexRun >= 32)
            return true;
        else
            hexRun = 0;
    }
    return ends();
}

struct CacheEntry {
    QString key;
    Target  target;
};

} // namespace

bool isBadUrlChar(QChar ch)
{
    const ushort u = ch.unicode();
    if (u <= 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F))
        return true;
    if (u == 0x061C || u == 0x200E || u == 0x200F || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069))
        return true;
    if ((u >= 0x200B && u <= 0x200D) || u == 0x2060 || u == 0xFEFF || u == 0x2028 || u == 0x2029)
        return true;
    return ch.isSpace();
}

QString keyOf(const QString& text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).left(kKeyChars / 2).toHex());
}

Kind mediaKindOfName(const QString& lastSegment)
{
    const QString ext = extensionOf(lastSegment);
    if (ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("webp"))
        return Kind::Image;
    if (ext == QLatin1String("gif"))
        return Kind::Gif;
    if (ext == QLatin1String("mp4") || ext == QLatin1String("webm") || ext == QLatin1String("mov") || ext == QLatin1String("m4v"))
        return Kind::Video;
    return Kind::None;
}

QString publicHostName(const QUrl& url)
{
    QString host = url.host(QUrl::FullyEncoded).toLower();
    if (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    if (host.isEmpty() || host.size() > kMaxHostChars)
        return {};
    if (host.contains(QLatin1Char(':')) || host.contains(QLatin1Char('[')))
        return {}; // an IPv6 address (IPv4 ones end in a numeric label, below)
    const QStringList labels = host.split(QLatin1Char('.'));
    if (labels.size() < 2)
        return {}; // "intranet", "printer"
    for (const QString& label : labels) {
        if (label.isEmpty() || label.size() > 63 || label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-')))
            return {};
        for (const QChar ch : label) {
            const ushort u = ch.unicode();
            if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_'))
                return {};
        }
    }
    if (isNumericLabel(labels.last()))
        return {}; // "2130706433", "0x7f.1", "127.1": browsers read these as IPv4 addresses
    if (isLocalSuffix(labels))
        return {};
    return host;
}

bool isYouTubeId(const QString& id)
{
    if (id.size() != kYouTubeIdChars)
        return false;
    for (const QChar ch : id) {
        const ushort u = ch.unicode();
        if (!((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_'))
            return false;
    }
    return true;
}

bool isKey(const QString& key)
{
    if (key.size() != kKeyChars)
        return false;
    for (const QChar ch : key) {
        const ushort u = ch.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    return true;
}

QUrl youTubeWatchUrl(const QString& videoId)
{
    if (!isYouTubeId(videoId))
        return {};
    return QUrl(latin("https://www.youtube.com/watch?v=") + videoId, QUrl::StrictMode);
}

QUrl normalizedPage(const QUrl& url)
{
    const QString host = publicHostName(url);
    if (host.isEmpty())
        return {};
    QString path = normalizePercent(url.path(QUrl::FullyEncoded));
    if (path.isEmpty())
        path = latin("/");
    QString       normalized = latin("https://") + host + path;
    const QString query      = normalizePercent(url.query(QUrl::FullyEncoded));
    if (!query.isEmpty())
        normalized += QLatin1Char('?') + query;
    if (normalized.size() > kMaxUrlChars)
        return {};
    const QUrl page(normalized, QUrl::StrictMode);
    return page.isValid() ? page : QUrl();
}

QUrl withoutTracking(const QUrl& page)
{
    if (!page.hasQuery())
        return page;
    static const char* const kTracking[] = {"fbclid", "gclid",   "dclid",  "gbraid",  "wbraid", "msclkid", "yclid",   "twclid", "igshid", "igsh",
                                            "mc_eid", "mc_cid",  "_hsenc", "_hsmi",   "ref_src", "ref_url", "s_cid",  "spm",    "scm"};
    const QString host     = page.host(QUrl::FullyEncoded).toLower();
    const bool    siIsTrap = host == QLatin1String("open.spotify.com") || host == QLatin1String("youtube.com") || host == QLatin1String("www.youtube.com")
                          || host == QLatin1String("m.youtube.com") || host == QLatin1String("youtu.be") || host == QLatin1String("music.youtube.com");
    QStringList kept;
    const QStringList items = page.query(QUrl::FullyEncoded).split(QLatin1Char('&'));
    for (const QString& item : items) {
        const QString name = QUrl::fromPercentEncoding(item.section(QLatin1Char('='), 0, 0).toLatin1()).toLower();
        bool          drop = name.startsWith(QLatin1String("utm_")) || (siIsTrap && name == QLatin1String("si"));
        for (const char* tracking : kTracking) {
            if (name == QLatin1String(tracking))
                drop = true;
        }
        if (!drop)
            kept << item;
    }
    QUrl out = page;
    if (kept.isEmpty())
        out.setQuery(QString());
    else
        out.setQuery(kept.join(QLatin1Char('&')), QUrl::StrictMode);
    return out.isValid() ? out : page;
}

bool hasActionSegment(const QUrl& url)
{
    static const QSet<QString> kActions = [] {
        QSet<QString> set;
        for (const char* name : {"login", "logout", "signin", "sign-in", "signout", "sign-out", "signup", "sign-up", "register", "verify",
                                 "confirm", "activate", "unsubscribe", "reset", "reset-password", "password", "oauth", "oauth2", "callback",
                                 "auth", "authorize", "sso", "saml", "download", "downloads", "dl"})
            set.insert(QString::fromLatin1(name));
        return set;
    }();
    const QStringList segments = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        if (kActions.contains(segment.toLower()))
            return true;
    }
    return false;
}

bool isActionLike(const QUrl& url)
{
    static const QSet<QString> kTokenNames = [] {
        QSet<QString> set;
        for (const char* name : {"token", "access_token", "id_token", "auth", "authkey", "code", "otp", "key", "apikey", "api_key", "sig", "signature",
                                 "session", "sessionid", "sid", "reset", "verify", "verification", "confirm", "confirmation", "activate",
                                 "activation", "unsubscribe", "magic", "ticket", "nonce", "state", "password", "pass", "pwd", "login_hint", "invite"})
            set.insert(QString::fromLatin1(name));
        return set;
    }();
    if (url.hasQuery()) {
        const QStringList items = url.query(QUrl::FullyEncoded).split(QLatin1Char('&'), Qt::SkipEmptyParts);
        for (const QString& item : items) {
            const QString name = QUrl::fromPercentEncoding(item.section(QLatin1Char('='), 0, 0).toLatin1()).toLower();
            if (kTokenNames.contains(name))
                return true;
        }
    }
    if (hasActionSegment(url))
        return true;
    const QStringList segments = url.path(QUrl::FullyEncoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        if (hasTokenRun(segment))
            return true;
    }
    return false;
}

bool hasNonHtmlExtension(const QUrl& url)
{
    static const QSet<QString> kFiles = [] {
        QSet<QString> set;
        for (const char* ext : {"pdf", "zip", "rar", "7z", "exe", "msi", "dmg", "apk", "ipa", "iso", "img", "tar", "gz", "tgz", "bz2", "xz", "zst",
                                "mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt",
                                "ods", "odp", "rtf", "txt", "csv", "tsv", "json", "xml", "svg", "svgz", "bin", "torrent", "jar", "deb", "rpm",
                                "appimage", "ts", "m3u8", "mpd", "avi", "mkv", "wmv", "flv"})
            set.insert(QString::fromLatin1(ext));
        return set;
    }();
    const QString ext = extensionOf(lastSegmentOf(url));
    return !ext.isEmpty() && kFiles.contains(ext);
}

Target classify(const QUrl& url)
{
    return classifyImpl(url, true);
}

Target classify(const QString& href)
{
    if (href.isEmpty() || href.size() > kMaxUrlChars)
        return {};
    // Only absolute http(s) addresses: a relative href, "www.example.com" or "javascript:" never.
    int start = 0;
    if (href.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        start = 8;
    else if (href.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))
        start = 7;
    else
        return {};
    // The authority as written: an empty user name ("https://@host") or a backslash (browsers read it as
    // a slash) would make the host shown differ from the host fetched.
    int end = href.size();
    for (const char stop : {'/', '?', '#'}) {
        const int at = href.indexOf(QLatin1Char(stop), start);
        if (at >= 0 && at < end)
            end = at;
    }
    const QString authority = href.mid(start, end - start);
    if (authority.contains(QLatin1Char('@')) || authority.contains(QLatin1Char('\\')))
        return {};
    // White space, controls, bidi and zero-width characters: never. ZWNJ / ZWJ only in the path and the query (a
    // Persian slug), and then the link is never a legacy one (2.4.0 refused them).
    const int fragment = href.indexOf(QLatin1Char('#'), end);
    bool      joiner   = false;
    for (int i = 0; i < href.size(); ++i) {
        const QChar ch = href.at(i);
        if (!isBadUrlChar(ch))
            continue;
        const bool inPathOrQuery = i >= end && (fragment < 0 || i < fragment);
        if ((ch.unicode() == kZwnj || ch.unicode() == kZwj) && inPathOrQuery) {
            joiner = true;
            continue;
        }
        return {};
    }
    return classifyImpl(QUrl(href, QUrl::TolerantMode), !joiner);
}

Target classifyCached(const QString& href)
{
    static QMutex                                            mutex;
    static std::list<CacheEntry>                             order; // most recently used first
    static QHash<QString, std::list<CacheEntry>::iterator>   index;
    QMutexLocker lock(&mutex);
    const auto   it = index.constFind(href);
    if (it != index.constEnd()) {
        order.splice(order.begin(), order, it.value());
        return order.front().target;
    }
    const Target target = classify(href);
    order.push_front(CacheEntry{href, target});
    index.insert(href, order.begin());
    while (static_cast<int>(order.size()) > kClassifyCacheSize) {
        index.remove(order.back().key);
        order.pop_back();
    }
    return target;
}

QString trimLinkEnd(const QString& href)
{
    QString out = href;
    for (;;) {
        if (out.isEmpty())
            return out;
        const QChar last = out.at(out.size() - 1);
        if (last == QLatin1Char('.') || last == QLatin1Char(',') || last == QLatin1Char(';') || last == QLatin1Char(':') || last == QLatin1Char('!')
            || last == QLatin1Char('?') || last == QLatin1Char('\'')) {
            out.chop(1);
            continue;
        }
        if (last == QLatin1Char(')') && out.count(QLatin1Char(')')) > out.count(QLatin1Char('('))) {
            out.chop(1);
            continue;
        }
        return out;
    }
}

QVector<Target> selectLinks(const QVector<Target>& inMessageOrder)
{
    QVector<Target> legacy, other;
    for (const Target& t : inMessageOrder)
        (t.isLegacy() ? legacy : other).append(t);
    legacy = legacy.mid(0, kMaxSlots); // exactly 2.4.0's choice (it only sees legacy links)
    QSet<QString> chosen;
    for (const Target& t : qAsConst(legacy))
        chosen.insert(t.key);
    for (const Target& t : qAsConst(other)) {
        if (chosen.size() >= kMaxSlots)
            break;
        chosen.insert(t.key);
    }
    QVector<Target> out;
    for (const Target& t : inMessageOrder) {
        if (chosen.contains(t.key)) {
            out.append(t);
            chosen.remove(t.key); // a duplicate key once
        }
    }
    return out;
}

const char* kindName(Kind kind)
{
    switch (kind) {
    case Kind::YouTube:
        return "youtube";
    case Kind::Image:
        return "image";
    case Kind::Gif:
        return "gif";
    case Kind::Video:
        return "video";
    case Kind::Loop:
        return "loop";
    case Kind::Card:
        return "card";
    case Kind::None:
        break;
    }
    return "none";
}

const char* className(LinkClass cls)
{
    switch (cls) {
    case LinkClass::YouTube:
        return "youtube";
    case LinkClass::Direct:
        return "direct";
    case LinkClass::MediaPage:
        return "media";
    case LinkClass::Site:
        return "site";
    case LinkClass::Web:
        return "web";
    case LinkClass::None:
        break;
    }
    return "none";
}

QString logUrl(const QUrl& url)
{
    if (!url.isValid() || url.isRelative())
        return latin("(invalid address)");
    QString host = url.host(QUrl::FullyEncoded).toLower();
    if (host.size() > 80)
        host = host.left(80) + QChar(0x2026);
    const QStringList segments = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString           last     = segments.isEmpty() ? QString() : segments.last();
    QString           clean;
    for (const QChar ch : last) {
        if (!isBadUrlChar(ch))
            clean += ch;
    }
    if (clean.size() > 40)
        clean = clean.left(39) + QChar(0x2026);
    QString out = url.scheme().toLower() + latin("://") + host + QLatin1Char('/');
    if (segments.size() > 1)
        out += QString(QChar(0x2026)) + QLatin1Char('/');
    return out + clean;
}

} // namespace linkpreview
