#include "content.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>

#include <cstring>

#include "netpolicy.h"
#include "pagemeta.h"
#include "textclean.h"

namespace linkpreview::content {

namespace {

bool startsWith(const QByteArray& data, const char* magic, int size, int offset = 0)
{
    return data.size() >= offset + size && memcmp(data.constData() + offset, magic, static_cast<size_t>(size)) == 0;
}

} // namespace

Format sniff(const QByteArray& head)
{
    if (startsWith(head, "\x89PNG\r\n\x1a\n", 8))
        return Format::Png;
    if (startsWith(head, "\xFF\xD8\xFF", 3))
        return Format::Jpeg;
    if (startsWith(head, "GIF87a", 6) || startsWith(head, "GIF89a", 6))
        return Format::Gif;
    if (startsWith(head, "RIFF", 4) && startsWith(head, "WEBP", 4, 8))
        return Format::WebP;
    if (startsWith(head, "\x1A\x45\xDF\xA3", 4))
        return Format::WebM; // EBML: WebM / Matroska
    if (startsWith(head, "ftyp", 4, 4))
        return startsWith(head, "qt  ", 4, 8) ? Format::Mov : Format::Mp4;
    // Old QuickTime files start with another atom.
    for (const char* atom : {"moov", "mdat", "wide", "free", "skip", "pnot"}) {
        if (startsWith(head, atom, 4, 4))
            return Format::Mov;
    }
    return Format::Unknown;
}

const char* mimeOf(Format format)
{
    switch (format) {
    case Format::Png:
        return "image/png";
    case Format::Jpeg:
        return "image/jpeg";
    case Format::Gif:
        return "image/gif";
    case Format::WebP:
        return "image/webp";
    case Format::Mp4:
        return "video/mp4";
    case Format::Mov:
        return "video/quicktime";
    case Format::WebM:
        return "video/webm";
    case Format::Unknown:
        break;
    }
    return "";
}

const char* extensionOf(Format format)
{
    switch (format) {
    case Format::Png:
        return "png";
    case Format::Jpeg:
        return "jpg";
    case Format::Gif:
        return "gif";
    case Format::WebP:
        return "webp";
    case Format::Mp4:
        return "mp4";
    case Format::Mov:
        return "mov";
    case Format::WebM:
        return "webm";
    case Format::Unknown:
        break;
    }
    return "";
}

bool fits(Kind kind, Format format)
{
    switch (kind) {
    case Kind::Image:
    case Kind::Gif:
    case Kind::YouTube: // its thumbnail
    case Kind::Card:    // 2.5: its picture
        return format == Format::Png || format == Format::Jpeg || format == Format::Gif || format == Format::WebP;
    case Kind::Video:
    case Kind::Loop: // 2.5: a silent looping video
        return format == Format::Mp4 || format == Format::Mov || format == Format::WebM;
    case Kind::None:
        break;
    }
    return false;
}

QByteArray mediaType(const QByteArray& contentType)
{
    const int semicolon = contentType.indexOf(';');
    return (semicolon < 0 ? contentType : contentType.left(semicolon)).trimmed().toLower();
}

bool contentTypeAllowed(Kind kind, const QByteArray& contentType)
{
    const QByteArray type = mediaType(contentType);
    if (type.isEmpty() || type == "application/octet-stream" || type == "binary/octet-stream" || type == "application/binary")
        return true; // the first bytes decide
    switch (kind) {
    case Kind::Image:
    case Kind::Gif:
    case Kind::YouTube:
    case Kind::Card:
        return type.startsWith("image/") && !type.contains("svg");
    case Kind::Video:
    case Kind::Loop:
        return type.startsWith("video/") || type == "application/mp4" || type == "application/x-matroska";
    case Kind::None:
        break;
    }
    return false;
}

bool isJsonType(const QByteArray& contentType)
{
    const QByteArray type = mediaType(contentType);
    return type == "application/json" || type == "text/json" || type == "application/json+oembed";
}

QString cleanText(const QString& text, int maxChars)
{
    return textclean::clean(text, maxChars);
}

bool parseOEmbed(const QByteArray& body, const QString& videoId, OEmbed* out, QString* error)
{
    const auto fail = [error](const char* code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    if (body.size() > net::kMaxOEmbedBytes)
        return fail("tooLarge");
    QJsonParseError     parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail("badJson");
    const QJsonObject object = doc.object();

    const QJsonValue type = object.value(QLatin1String("type"));
    if (!type.isUndefined() && type.toString() != QLatin1String("video"))
        return fail("wrongType");

    const QJsonValue title = object.value(QLatin1String("title"));
    if (!title.isString())
        return fail("noTitle");
    OEmbed parsed;
    parsed.title = cleanText(title.toString(), kMaxTitleChars);
    if (parsed.title.isEmpty())
        return fail("noTitle");

    const QJsonValue author = object.value(QLatin1String("author_name"));
    if (author.isString())
        parsed.author = cleanText(author.toString(), kMaxAuthorChars);

    const QJsonValue thumbnail = object.value(QLatin1String("thumbnail_url"));
    if (thumbnail.isString() && thumbnail.toString().size() <= 512) {
        const QUrl named(thumbnail.toString(), QUrl::StrictMode);
        if (net::isAllowedThumbnail(named, videoId))
            parsed.thumbnail = named;
    }
    if (!parsed.thumbnail.isValid())
        parsed.thumbnail = net::defaultThumbnail(videoId);
    if (out)
        *out = parsed;
    return true;
}

// ---- 2.5 ----------------------------------------------------------------------------------------------------------

namespace {

constexpr int kMaxJsonString = 4096;

// The top-level object of a JSON answer, or false with *error.
bool jsonObject(const QByteArray& body, QJsonObject* out, QString* error)
{
    const auto fail = [error](const char* code) {
        if (error)
            *error = QString::fromLatin1(code);
        return false;
    };
    if (body.size() > net::kMaxJsonBytes)
        return fail("tooLarge");
    const QByteArray trimmed = body.trimmed();
    if (trimmed.isEmpty())
        return fail("badJson");
    if (trimmed.at(0) != '{')
        return fail(trimmed.at(0) == '[' ? "wrongType" : "badJson");
    QJsonParseError     parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail("badJson");
    if (!doc.isObject())
        return fail("wrongType");
    *out = doc.object();
    return true;
}

QString stringOf(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isString())
        return {};
    const QString s = v.toString();
    return s.size() > kMaxJsonString ? QString() : s;
}

// A whole number in [lo, hi] (JSON numbers, or digits in a string as some sites send them); fallback otherwise.
qint64 numberOf(const QJsonObject& o, const char* key, qint64 lo, qint64 hi, qint64 fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    double           d = 0;
    if (v.isDouble()) {
        d = v.toDouble();
    } else if (v.isString() && v.toString().size() <= 12) {
        bool ok = false;
        d       = v.toString().toDouble(&ok);
        if (!ok)
            return fallback;
    } else {
        return fallback;
    }
    if (!(d >= static_cast<double>(lo) && d <= static_cast<double>(hi)))
        return fallback;
    return static_cast<qint64>(d);
}

bool isHashOf(const QString& value)
{
    static const QRegularExpression re(QStringLiteral("^(a_)?[0-9a-f]{32}$"));
    return re.match(value).hasMatch();
}

quint32 le32(const QByteArray& data, int at)
{
    const auto* d = reinterpret_cast<const unsigned char*>(data.constData()) + at;
    return quint32(d[0]) | (quint32(d[1]) << 8) | (quint32(d[2]) << 16) | (quint32(d[3]) << 24);
}

// Walks chunks in [from, to): Lossless / Malformed as soon as one says so; nested: inside an ANMF frame.
WebpVerdict walkChunks(const QByteArray& file, qint64 from, qint64 to, bool nested, int* budget)
{
    qint64 at        = from;
    bool   anyChunks = false;
    while (at < to) {
        if (--*budget < 0 || at + 8 > to)
            return WebpVerdict::Malformed;
        const QByteArray fourcc = file.mid(static_cast<int>(at), 4);
        const qint64     size   = le32(file, static_cast<int>(at + 4));
        const qint64     start  = at + 8;
        const qint64     end    = start + size;
        if (end > to)
            return WebpVerdict::Malformed;
        anyChunks = true;
        if (fourcc == "VP8L")
            return WebpVerdict::Lossless;
        if (fourcc == "ALPH") {
            if (size < 1)
                return WebpVerdict::Malformed;
            if ((static_cast<unsigned char>(file.at(static_cast<int>(start))) & 0x03) == 1)
                return WebpVerdict::Lossless;
        }
        if (fourcc == "ANMF" && !nested) {
            if (size < 16)
                return WebpVerdict::Malformed;
            const WebpVerdict inner = walkChunks(file, start + 16, end, true, budget);
            if (inner != WebpVerdict::Ok)
                return inner;
        }
        at = end + (size & 1); // payloads are padded to an even size
        if (at > to + 1)
            return WebpVerdict::Malformed;
    }
    return anyChunks || nested ? WebpVerdict::Ok : WebpVerdict::Malformed;
}

} // namespace

bool parseOEmbedData(const QByteArray& body, OEmbedData* out, QString* error)
{
    QJsonObject o;
    if (!jsonObject(body, &o, error))
        return false;
    OEmbedData data;
    const QString type = stringOf(o, "type");
    if (type == QLatin1String("photo") || type == QLatin1String("video") || type == QLatin1String("link") || type == QLatin1String("rich"))
        data.type = type;
    data.title           = stringOf(o, "title");
    data.authorName      = stringOf(o, "author_name");
    data.providerName    = stringOf(o, "provider_name");
    data.description     = stringOf(o, "description");
    data.thumbnailUrl    = stringOf(o, "thumbnail_url");
    data.url             = data.type == QLatin1String("photo") ? stringOf(o, "url") : QString();
    data.thumbnailWidth  = static_cast<int>(numberOf(o, "thumbnail_width", 0, 16384, 0));
    data.thumbnailHeight = static_cast<int>(numberOf(o, "thumbnail_height", 0, 16384, 0));
    data.width           = static_cast<int>(numberOf(o, "width", 0, 16384, 0));
    data.height          = static_cast<int>(numberOf(o, "height", 0, 16384, 0));
    data.durationMs      = numberOf(o, "duration", 0, 86400, 0) * 1000;
    if (out)
        *out = data;
    return true;
}

bool parseWikiSummary(const QByteArray& body, WikiSummary* out, QString* error)
{
    QJsonObject o;
    if (!jsonObject(body, &o, error))
        return false;
    WikiSummary summary;
    summary.title   = stringOf(o, "title");
    summary.extract = stringOf(o, "extract");
    const QJsonValue thumbnail = o.value(QLatin1String("thumbnail"));
    if (thumbnail.isObject()) {
        const QJsonObject t     = thumbnail.toObject();
        summary.thumbnail       = stringOf(t, "source");
        summary.thumbnailWidth  = static_cast<int>(numberOf(t, "width", 0, 16384, 0));
        summary.thumbnailHeight = static_cast<int>(numberOf(t, "height", 0, 16384, 0));
    }
    if (out)
        *out = summary;
    return true;
}

bool parseDiscordInvite(const QByteArray& body, DiscordInvite* out, QString* error)
{
    QJsonObject o;
    if (!jsonObject(body, &o, error))
        return false;
    DiscordInvite    invite;
    const QJsonValue guildValue = o.value(QLatin1String("guild"));
    if (guildValue.isObject()) {
        const QJsonObject guild = guildValue.toObject();
        static const QRegularExpression kId(QStringLiteral("^\\d{15,22}$"));
        const QString id = stringOf(guild, "id");
        if (kId.match(id).hasMatch())
            invite.guildId = id;
        invite.name        = stringOf(guild, "name");
        invite.description = stringOf(guild, "description");
        const QString splash = stringOf(guild, "splash");
        const QString icon   = stringOf(guild, "icon");
        invite.splash        = isHashOf(splash) ? splash : QString();
        invite.icon          = isHashOf(icon) ? icon : QString();
        invite.nsfw = (guild.value(QLatin1String("nsfw")).isBool() && guild.value(QLatin1String("nsfw")).toBool())
                      || numberOf(guild, "nsfw_level", 0, 100, 0) == 3;
    }
    invite.members = numberOf(o, "approximate_member_count", 0, 1000000000LL, -1);
    invite.online  = numberOf(o, "approximate_presence_count", 0, 1000000000LL, -1);
    if (out)
        *out = invite;
    return true;
}

bool parsePixeldrainInfo(const QByteArray& body, PixeldrainInfo* out, QString* error)
{
    QJsonObject o;
    if (!jsonObject(body, &o, error))
        return false;
    PixeldrainInfo info;
    info.mimeType = stringOf(o, "mime_type").toLower().left(100);
    info.size     = numberOf(o, "size", 0, 1LL << 50, -1);
    if (out)
        *out = info;
    return true;
}

QString stripTags(const QString& html)
{
    const QString decoded = pagemeta::decodeEntities(html);
    QString       out;
    out.reserve(decoded.size());
    bool inTag = false;
    for (const QChar ch : decoded) {
        if (inTag) {
            if (ch == QLatin1Char('>')) {
                inTag = false;
                out += QLatin1Char(' ');
            }
            continue;
        }
        if (ch == QLatin1Char('<')) {
            inTag = true;
            continue;
        }
        out += ch;
    }
    return out;
}

WebpVerdict webpVerdict(const QByteArray& file)
{
    if (file.size() < 12 || !file.startsWith("RIFF") || file.mid(8, 4) != "WEBP")
        return WebpVerdict::Malformed;
    const qint64 riffEnd = qint64(le32(file, 4)) + 8;
    if (riffEnd > file.size() || riffEnd < 12)
        return WebpVerdict::Malformed;
    int budget = 8192;
    return walkChunks(file, 12, riffEnd, false, &budget);
}

} // namespace linkpreview::content
