#include "linkurl.h"

#include <QCryptographicHash>
#include <QStringList>
#include <QUrlQuery>

namespace linkpreview {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

// Characters that never belong in a link we preview: C0/C1 controls, DEL, white space, bidi marks,
// zero-width characters and the byte order mark (a href is shown as text: these could hide or reorder it).
bool isBadChar(QChar ch)
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

bool hasBadChars(const QString& text)
{
    for (const QChar ch : text) {
        if (isBadChar(ch))
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
                                             "localdomain", "test", "invalid", "example", "onion", "arpa", "home-network"};
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

QString keyOf(const QString& normalized)
{
    return QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).left(kKeyChars / 2).toHex());
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

Kind kindForExtension(const QString& lastSegment)
{
    const int dot = lastSegment.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) // no extension, or a name that is only an extension
        return Kind::None;
    const QString ext = lastSegment.mid(dot + 1).toLower();
    if (ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("webp"))
        return Kind::Image;
    if (ext == QLatin1String("gif"))
        return Kind::Gif;
    if (ext == QLatin1String("mp4") || ext == QLatin1String("webm") || ext == QLatin1String("mov") || ext == QLatin1String("m4v"))
        return Kind::Video;
    return Kind::None;
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

} // namespace

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

Target classify(const QUrl& url)
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
            return none; // a channel, a playlist, a search: web pages
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
    const QString decodedPath = url.path(QUrl::FullyDecoded);
    const Kind    kind        = kindForExtension(decodedPath.mid(decodedPath.lastIndexOf(QLatin1Char('/')) + 1));
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

Target classify(const QString& href)
{
    if (href.isEmpty() || href.size() > kMaxUrlChars || hasBadChars(href))
        return {};
    // Only absolute http(s) addresses (http for YouTube only, see classify(QUrl)): a relative href,
    // "www.example.com" or "javascript:" never.
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
    return classify(QUrl(href, QUrl::TolerantMode));
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
    case Kind::None:
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
        if (!isBadChar(ch))
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
