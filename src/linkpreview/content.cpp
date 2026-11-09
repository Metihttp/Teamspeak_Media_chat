#include "content.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cstring>

#include "netpolicy.h"

namespace linkpreview::content {

namespace {

bool startsWith(const QByteArray& data, const char* magic, int size, int offset = 0)
{
    return data.size() >= offset + size && memcmp(data.constData() + offset, magic, static_cast<size_t>(size)) == 0;
}

// Removed outright (format characters that hide or reorder text). ZWNJ U+200C and ZWJ U+200D stay.
bool isInvisible(uint u)
{
    return u == 0x00AD || u == 0x061C || u == 0x180E || u == 0x200B || u == 0x200E || u == 0x200F || (u >= 0x202A && u <= 0x202E)
           || (u >= 0x2060 && u <= 0x2064) || (u >= 0x2066 && u <= 0x206F) || u == 0xFEFF || (u >= 0xFFF9 && u <= 0xFFFB);
}

bool isNonCharacter(uint u)
{
    return (u >= 0xFDD0 && u <= 0xFDEF) || (u & 0xFFFE) == 0xFFFE;
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
        return format == Format::Png || format == Format::Jpeg || format == Format::Gif || format == Format::WebP;
    case Kind::Video:
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
        return type.startsWith("image/") && !type.contains("svg");
    case Kind::Video:
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
    QString out;
    out.reserve(qMin(text.size(), maxChars + 2));
    bool pendingSpace = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        uint        u  = ch.unicode();
        int         units = 1;
        if (ch.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate())
                continue; // lone surrogate
            u     = QChar::surrogateToUcs4(ch, text.at(i + 1));
            units = 2;
        } else if (ch.isLowSurrogate()) {
            continue;
        }
        const QChar::Category category = QChar::category(u);
        const bool            space    = QChar::isSpace(u) || u == 0x2028 || u == 0x2029 || u == '\t' || u == '\n' || u == '\r';
        if (space) {
            pendingSpace = !out.isEmpty();
            i += units - 1;
            continue;
        }
        if (category == QChar::Other_Control || isInvisible(u) || isNonCharacter(u)) {
            i += units - 1;
            continue;
        }
        const int needed = units + (pendingSpace ? 1 : 0);
        if (out.size() + needed > maxChars) {
            // Cut: room for the ellipsis, never half a pair.
            while (out.size() > qMax(0, maxChars - 1)) {
                out.chop(1);
                if (!out.isEmpty() && out.at(out.size() - 1).isHighSurrogate())
                    out.chop(1);
            }
            if (maxChars > 0)
                out += QChar(0x2026);
            return out.trimmed();
        }
        if (pendingSpace)
            out += QLatin1Char(' ');
        pendingSpace = false;
        out += ch;
        if (units == 2)
            out += text.at(i + 1);
        i += units - 1;
    }
    return out;
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

} // namespace linkpreview::content
