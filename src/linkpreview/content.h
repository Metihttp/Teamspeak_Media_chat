#pragma once

// 2.4 link previews: what a website sent, checked before it is kept. Pure QtCore (unit-tested).
//  * media files: the Content-Type must fit the kind (or be generic/missing), and the first bytes must
//    be one of the formats of that kind (PNG, JPEG, GIF, WebP; MP4/MOV, WebM). SVG, HTML, scripts never.
//  * YouTube oEmbed: JSON only, at most net::kMaxOEmbedBytes, one object; the title (required) and the
//    channel name are strings, cleaned and cut; a thumbnail address is used only when it is an
//    i.ytimg.com one of this very video, else the standard one is built from the id.

#include <QByteArray>
#include <QString>
#include <QUrl>

#include "linkurl.h"

namespace linkpreview::content {

enum class Format { Unknown, Png, Jpeg, Gif, WebP, Mp4, Mov, WebM };

constexpr int kSniffBytes      = 16; // sniff() needs this many bytes (or the whole, shorter, body)
constexpr int kMaxTitleChars   = 200;
constexpr int kMaxAuthorChars  = 100;

Format      sniff(const QByteArray& head);
const char* mimeOf(Format format);      // "image/png", ..., "video/mp4", "video/quicktime", "video/webm"; "" for Unknown
const char* extensionOf(Format format); // "png", "jpg", "gif", "webp", "mp4", "mov", "webm"; "" for Unknown
bool        fits(Kind kind, Format format); // Image/Gif: the four picture formats; Video: MP4, MOV, WebM

// The media type of a Content-Type header ("image/png; charset=x" -> "image/png"), lower case.
QByteArray mediaType(const QByteArray& contentType);
bool       contentTypeAllowed(Kind kind, const QByteArray& contentType);
bool       isJsonType(const QByteArray& contentType);

// Text from a website made safe to show and to send: C0/C1 controls, bidi marks, zero-width spaces, the
// byte order mark, non-characters and lone surrogates removed (ZWJ/ZWNJ stay: emoji, Persian); every
// white-space run one space; trimmed; at most maxChars UTF-16 units, never half a surrogate pair, "…"
// at the end when cut.
QString cleanText(const QString& text, int maxChars);

struct OEmbed {
    QString title;     // cleaned, never empty
    QString author;    // cleaned, may be empty
    QUrl    thumbnail; // always an allowed i.ytimg.com address of this video
};

// false with *error = "badJson" / "noTitle" / "tooLarge" / "wrongType".
bool parseOEmbed(const QByteArray& body, const QString& videoId, OEmbed* out, QString* error);

} // namespace linkpreview::content
