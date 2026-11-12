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

// Text from a website made safe to show: textclean::clean (one line, invisible and reordering characters
// removed, ZWNJ/ZWJ kept between visible characters, "…" when cut). A 2.4 name kept for its callers.
QString cleanText(const QString& text, int maxChars);

struct OEmbed {
    QString title;     // cleaned, never empty
    QString author;    // cleaned, may be empty
    QUrl    thumbnail; // always an allowed i.ytimg.com address of this video
};

// false with *error = "badJson" / "noTitle" / "tooLarge" / "wrongType".
bool parseOEmbed(const QByteArray& body, const QString& videoId, OEmbed* out, QString* error);

// ---- 2.5: official oEmbed answers and keyless JSON APIs of other sites ------------------------------------------
//
// At most net::kMaxJsonBytes, one JSON object at the top (first non-white byte '{'). Only the named keys are read,
// each only with its expected JSON type; strings longer than 4096 characters are ignored. "html", "author_url",
// "provider_url" and "iframe_url" are never read. Texts are returned as sent (the Fetcher cleans them); addresses
// as written (the Fetcher resolves and checks them like a link).

struct OEmbedData {
    QString type; // photo | video | link | rich, else empty
    QString title, authorName, providerName, description;
    QString thumbnailUrl, url; // url: photo only
    int     thumbnailWidth = 0, thumbnailHeight = 0, width = 0, height = 0; // 0..16384
    qint64  durationMs = 0;                                                 // "duration" in seconds, 0..86400
};
// false with *error = "badJson" / "tooLarge" / "wrongType" (not an object).
bool parseOEmbedData(const QByteArray& body, OEmbedData* out, QString* error);

// Wikipedia's REST page summary (title, extract, thumbnail.source/width/height). Never displaytitle or extract_html.
struct WikiSummary {
    QString title, extract, thumbnail;
    int     thumbnailWidth = 0, thumbnailHeight = 0;
};
bool parseWikiSummary(const QByteArray& body, WikiSummary* out, QString* error);

// Discord's invite API (with_counts=true): the guild's id, name, description, splash and icon hashes, NSFW
// flags and the approximate counts.
struct DiscordInvite {
    QString guildId, name, description, splash, icon; // id: 15-22 digits; splash/icon: (a_)?32 hex digits, else empty
    bool    nsfw = false;                              // guild.nsfw or guild.nsfw_level == 3
    qint64  members = -1, online = -1;                 // approximate_member_count / approximate_presence_count
};
bool parseDiscordInvite(const QByteArray& body, DiscordInvite* out, QString* error);

// Pixeldrain's file info: mime_type and size.
struct PixeldrainInfo {
    QString mimeType;
    qint64  size = -1;
};
bool parsePixeldrainInfo(const QByteArray& body, PixeldrainInfo* out, QString* error);

// An HTML fragment as plain text (SoundCloud's oEmbed description): entities decoded once, then every <...> removed.
QString stripTags(const QString& html);

// ---- 2.5: lossless WebP (CVE-2023-4863 in the libwebp of TeamSpeak's Qt) -----------------------------------------

// Ok: a lossy WebP; Lossless: a VP8L chunk anywhere, or an ALPH chunk with lossless compression (also inside an
// animation frame); Malformed: anything inconsistent (sizes outside the file, not a WebP). Both of the last two are
// refused before any decoder sees the file.
enum class WebpVerdict { Ok, Lossless, Malformed };
WebpVerdict webpVerdict(const QByteArray& file);

} // namespace linkpreview::content
