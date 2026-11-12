#pragma once

// 2.5 link previews: reading a web page's meta (and Reddit's Atom feed and v.redd.it's DASH manifest) safely. Pure
// QtCore (unit-tested in tst_pagemeta), never on the GUI thread, never renders anything.
//
// HTML is untrusted input. One hand-written pass over the bytes, linear in the input:
//  * comments, <!...>, <?...> and CDATA are skipped; raw-text elements (script, style, textarea, noscript, template,
//    iframe, xmp, noembed, noframes) are skipped to their end tag; <base> is ignored;
//  * tags: names of at most 16 letters and digits, at most 64 attributes kept per tag (names of at most 64
//    characters, values of at most 4096 characters; a duplicate attribute: the first wins), at most 4096 tags;
//  * the first <title> (at most 4096 characters), and <meta> tags (key: property, else name; value: content);
//  * events: </head> (HeadEnd), <body> (BodyStart). A page is read up to its head unless a row asks for its body.
// Charset (decoderFor): a byte order mark, then the Content-Type header's charset, then a prescan of the first
// 8 KiB for <meta charset>, then UTF-8 (windows-1252 when the UTF-8 is invalid); names map to an allowlist of
// decoders, anything else is UTF-8. Only attribute values and the title text are decoded; a value whose decoded
// text is more than 10% U+FFFD is dropped. Entities are decoded once (a fixed list of names, numeric references).
// The feed and the manifest use the same tokenizer in "xml-lite" mode (names may hold ':'); no XML parser is used.

#include <QByteArray>
#include <QRgb>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <functional>

#include "carddata.h"

class QTextCodec;

namespace linkpreview::pagemeta {

// ---- charset ------------------------------------------------------------------------------------------------

struct Decoder {
    QTextCodec* codec = nullptr; // never null after decoderFor()
    QByteArray  name;            // "UTF-8", "windows-1252", ...
    bool        utf16 = false;   // the body had a UTF-16 byte order mark: prepared() converted it to UTF-8
};
// The decoder of a page by the rules above. contentType: the response's Content-Type header (may be empty).
Decoder decoderFor(const QByteArray& body, const QByteArray& contentType);
// The bytes the tokenizer reads: body itself, or a UTF-16 body converted to UTF-8 (byte order mark removed).
QByteArray prepared(const QByteArray& body, const Decoder& decoder);
// The allowlisted codec name of a charset label ("utf-8", "latin1", "sjis", ...); empty when it isn't allowed.
QByteArray codecNameFor(const QByteArray& label);

// Entities decoded once (3.6.2): numeric references (';' required, at most 8 digits), the named list (';' required)
// and, without ';', only &amp &lt &gt &quot &nbsp. Anything else is kept as written.
QString decodeEntities(const QString& text);

// ---- meta -----------------------------------------------------------------------------------------------------

struct Meta {
    QString     ogTitle, ogDescription, ogSiteName, ogType, twitterTitle, twitterDescription, twitterCard, title, description;
    QStringList ogImages;      // og:image, og:image:url, og:image:secure_url in document order (deduplicated), at most 4
    QStringList twitterImages; // twitter:image, twitter:image:src, at most 2
    struct Video {
        QString url, type;
    };
    QVector<Video> ogVideos; // og:video, og:video:url, og:video:secure_url; og:video:type pairs with the last video before it; at most 4
    QString        twitterPlayerStream;
    int            ogImageWidth = 0, ogImageHeight = 0, videoDurationSec = 0; // og:image:width/height, og:video:duration or video:duration
    QString        themeColor;     // the chosen theme-color value as written (3.6.5)
    QString        rating, ageRestriction; // name=rating; og:restrictions:age
    bool           robotsNoindex = false;  // name=robots containing "noindex"
    QString        charset;        // a <meta charset> / http-equiv content-type charset label (the prescan)
    enum class Stop { HeadEnd, BodyStart, Cap, Tags, End } stop = Stop::End;
    int            bytesScanned = 0;
    int            tags         = 0;
};
// The meta of html (already prepared(): UTF-8 for a UTF-16 page) decoded with codec (null: UTF-8). In head mode
// the scan ends at </head> or <body>; in body mode (bodyMode, Kick's clip pages) it reads on to the end.
Meta scan(const QByteArray& html, const QTextCodec* codec, bool bodyMode);
// The TextMode::enough predicate of a page read: !bodyMode && (HeadEnd || BodyStart).
bool headDone(const Meta& m, bool bodyMode);
// scan() as the Fetcher's page read uses it: at most capBytes are looked at; Stop::Cap when the bytes were cut.
Meta scanPage(const QByteArray& html, const QTextCodec* codec, bool bodyMode, bool truncated);

// ---- tags (shares.h reads a share page's hidden inputs with the same tokenizer) ---------------------------------

// One start or end tag as the tokenizer above gives it (comments, raw-text elements and their content skipped).
struct Tag {
    bool                                   end = false;
    QByteArray                             name;  // lower case
    QVector<QPair<QByteArray, QByteArray>> attrs; // lower-case names, raw values (the first of each name)
    const QByteArray*                      attr(const char* attrName) const;
};
struct Walk {
    int  bytesScanned = 0;
    int  tags         = 0;
    bool tagLimit     = false; // stopped at the tokenizer's tag limit (4096 tags)
};
// Every tag of html in document order until visit returns false. maxValueBytes: the longest attribute value kept
// (a longer one is dropped, as in scan(), whose limit is 16 KiB; at most 128 KiB).
Walk walkTags(const QByteArray& html, int maxValueBytes, const std::function<bool(const Tag&)>& visit);
// An attribute value as scan() reads one: decoded with codec (null: UTF-8), dropped when it is mostly U+FFFD,
// entities decoded once, at most maxChars (at most 4096) characters.
QString decodeAttribute(const QByteArray& raw, const QTextCodec* codec, int maxChars = 4096);

// ---- card fields (3.6.3) --------------------------------------------------------------------------------------

// The first non-empty value after cleaning (textclean), with the card limits of carddata.h.
QString titleOf(const Meta& m);       // og:title, twitter:title, <title>
QString descriptionOf(const Meta& m); // og:description, twitter:description, <meta name=description>
QString siteNameOf(const Meta& m);    // og:site_name
// Picture candidates as written, in order: ogImages, then twitterImages (at most 4; the caller resolves each).
QStringList pictureCandidates(const Meta& m);
// The card's layout: twitter:card summary_large_image / player -> Large, summary -> Thumb; else Large when the
// picture is at least 400 px wide with an aspect (w/h) of at least 1.3; Thumb otherwise; None without a picture.
CardLayout layoutFor(const QString& twitterCard, bool hasPicture, int pictureWidth, int pictureHeight);
// og:video:duration / video:duration in ms (1..86400 s), 0 otherwise.
qint64 durationMsOf(const Meta& m);

// ---- no card: walls, consent pages, boilerplate (3.6.4) --------------------------------------------------------

enum class Verdict { Card, Walled, Nothing };
// linkUrl: the link as the user posted it; finalUrl: the last page hop; challenge: "cf-mitigated: challenge".
Verdict wallOf(const Meta& m, const QUrl& linkUrl, const QUrl& finalUrl, bool challenge);
// A cleaned title that is a bot challenge's ("Just a moment...", "Attention Required!", ...).
bool isChallengeTitle(const QString& cleanedTitle);

// ---- theme colour, sensitive ------------------------------------------------------------------------------------

// "#rgb" / "#rrggbb" (case-insensitive) -> colour; false otherwise.
bool parseHexColour(const QString& text, QRgb* out);
// A theme colour a Web card may wear: not too light (relative luminance above 0.85) or too dark (below 0.02), and
// not within CIE76 delta E 6 of any brand colour of the provider table.
bool isAcceptableThemeColour(QRgb colour);
// The page's theme colour by the rules above (first theme-color without media, else one whose media says light,
// else the first); false when there is none or it isn't acceptable.
bool themeColourOf(const Meta& m, QRgb* out);
// CIE76 delta E of two sRGB colours (D65).
double deltaE(QRgb a, QRgb b);
// <meta name=rating> containing "adult" or the RTA label, or og:restrictions:age of 18 or more.
bool isSensitive(const Meta& m);

// ---- xml-lite: Reddit's post feed, v.redd.it's manifest --------------------------------------------------------

struct FeedEntry {
    bool    found = false; // an <entry> was there
    QString title;         // decoded once, not cleaned
    QString category;      // <category label="r/gifs">
    QString author;        // <author><name>
    QString thumbnail;     // <media:thumbnail url>
    QString content;       // <content>, entities decoded once (HTML text)
};
// The first <entry> of an Atom feed.
FeedEntry readFeed(const QByteArray& xml);
// The href of the first <a href="...">[link]</a> of a feed entry's content; empty when there is none.
QString linkOfContent(const QString& content);

struct Manifest {
    bool hasAudio = false; // any AdaptationSet (or Representation) of audio
    struct Video {
        int     height = 0;
        QString baseUrl;
    };
    QVector<Video> videos;
};
Manifest readManifest(const QByteArray& xml);
// The video to load: the largest height <= maxHeight, else the smallest; its BaseURL must be a plain relative name
// (^[A-Za-z0-9_.-]{1,64}$). Empty when there is none.
QString pickManifestVideo(const Manifest& manifest, int maxHeight = 480);

} // namespace linkpreview::pagemeta
