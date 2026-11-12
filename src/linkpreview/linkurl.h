#pragma once

// Link previews: which links in the chat get a preview, which rules each follows, and the stable key of each.
// Pure QtCore (unit-tested), no network.
//
// Five classes (2.5; providers.h has the table of known sites):
//  * YouTube (2.4): a YouTube video, https or http (TeamSpeak links "www.youtube.com/..." typed without a scheme
//    as http://; the fetch never uses the link's address, always the canonical https one):
//    youtube.com/watch?v=ID, youtube.com/shorts/ID, youtu.be/ID, also on www., m. and music.youtube.com
//    (exactly these hosts: "youtube.com.evil.example" is just another website). ID: exactly 11 of
//    [A-Za-z0-9_-]. Key: SHA-256 of "yt:<id>".
//  * Direct (2.4): a media file on any other website, https only, by the last path segment's extension (a
//    query is allowed): .png .jpg .jpeg .webp (Image), .gif (Gif), .mp4 .webm .mov .m4v (Video). Key: SHA-256 of
//    the normalized address. Two Direct links get a fetch rewrite (providers.h: Tenor's short GIF links, Dropbox
//    share links); their key and kind stay 2.4.0's.
//  * MediaPage (2.5): a page of a media site (Tenor, Giphy, Imgur, Klipy, ...): the preview is the media itself
//    (Image, Gif, Loop, Video), derived from the link or read from the page. Also http:// links to media files
//    and media file links with a joiner in their path (the built-in row "media-file").
//  * Site (2.5): a page of a well-known site (Vimeo, Spotify, X, GitHub, Wikipedia, ...): a rich card from the
//    site's official oEmbed, a keyless API or the page's meta.
//  * Web (2.5): any other web page: a generic card from the page's meta (OpenGraph, Twitter Card, <title>).
//    Links that look like actions or tokens and links to non-HTML files never get one.
// New classes accept https and http (fetched as https); their key is SHA-256 of "page:" + the normalized page
// address ("page v1", frozen for all of 2.5.x and later): never a provider's id, so a later version can move a
// link between new classes without changing its key.
// Never: user names or passwords in the address, ports other than 443 (80 for http), IP addresses and numeric
// hosts, single-label and local names (localhost, *.local, *.lan, *.internal, *.home.arpa, ...), addresses over
// kMaxUrlChars, control characters or white space, and TS Media's own links (its GitHub repository, the
// "#tsmupd-" update hints).

#include <QString>
#include <QUrl>
#include <QVector>

namespace linkpreview {

// What a slot shows. 2.5: Loop (a silent looping video shown like a GIF) and Card (a rich or generic web card).
enum class Kind { None, YouTube, Image, Gif, Video, Loop, Card };

// Which rules a link follows (2.5). YouTube and Direct are exactly 2.4.0's classes (same keys, LP/LPNO).
enum class LinkClass { None, YouTube, Direct, MediaPage, Site, Web };

struct Target {
    // Legacy: as 2.4.0. New classes: the EXPECTED kind (waits, caps, the strip's glyph); the load tells what it
    // turned out to be (Result::kind).
    Kind    kind = Kind::None;
    // What is fetched. YouTube: the canonical https://www.youtube.com/watch?v=<id> (a card opens the
    // link's own href, which may carry a start time). Media files: the normalized address (lower-case
    // ASCII host, no port, no fragment, percent-encoding normalized). New classes: == page.
    QUrl    url;
    QString videoId; // YouTube only
    // Lower-case ASCII host without port or trailing dot. International names stay in their "xn--"
    // form (show this, never a Unicode form: look-alike letters can't fake a known site that way).
    QString host;
    // kKeyChars lower-case hex digits: the start of the SHA-256 of "yt:<id>" for YouTube (every address
    // form of one video shares it), of the normalized address for Direct links, and of "page:" + page for the
    // new classes. The same link gives the same key on every client, so it names the link in announcements
    // and the cache.
    QString key;
    // 2.5
    LinkClass cls = LinkClass::None;
    QString   provider; // providers.h id ("tenor", "vimeo"); "tenor-short"/"dropbox" for Direct rewrites; empty otherwise
    QString   itemId;   // the provider's id read from the link (may be empty)
    QString   sub;      // the provider's form ("gif", "sticker", "clip", "album", "post", "channel", ...)
    QUrl      page;     // new classes: the https form of the link as written (normalizedPage)
    QUrl      fetchUrl; // new classes: page without tracking parameters; what a Page step of the link requests
    int       expectedRequests = 1; // 1..3: automatic-budget tokens one load of it takes

    bool isValid() const { return kind != Kind::None; }
    bool isLegacy() const { return cls == LinkClass::YouTube || cls == LinkClass::Direct; }
    bool isNewClass() const { return cls == LinkClass::MediaPage || cls == LinkClass::Site || cls == LinkClass::Web; }
};

constexpr int kMaxUrlChars    = 2048;
constexpr int kMaxHostChars   = 253;
constexpr int kKeyChars       = 32;
constexpr int kYouTubeIdChars = 11;

// The links of one message that get a slot: at most kMaxSlots, legacy first (2.4.0's choice: the first three
// legacy links), then the other links in message order until kMaxSlots; returned in message order. Pure and
// settings-independent: the sender and every receiver pick the same links. inMessageOrder: every classified link
// of the message (Kind != None), duplicates by key removed (the first kept).
constexpr int kMaxSlots = 3;
QVector<Target> selectLinks(const QVector<Target>& inMessageOrder);

// How many links of one message are looked at, on the sender and on receivers alike: the first
// kMaxScannedLinks in message order, the rest never.
constexpr int kMaxScannedLinks = 32;

// The end of a link as both sides read it: trailing . , ; : ! ? ' removed, and a trailing ) removed only when the
// link holds more ) than (; repeated until nothing changes ("Foo_(bar)" keeps its ")", "https://x.com/a)." loses
// ")."). Pure; applied before classify() on both sides.
QString trimLinkEnd(const QString& href);

// The classification of a URL (tests, 2.4.0 callers). A link read from a message goes through classify(QString).
Target classify(const QUrl& url);
// THE entry point for a link of a message: an anchor's href (receivers) or a link's text (the sender), after
// trimLinkEnd(). Parsed tolerantly, then the rules above. Refused outright: white space, controls, bidi marks,
// zero-width characters; ZWNJ and ZWJ only in the path and query of a new-class link (Persian slugs), never in
// the authority and never in a legacy link.
Target classify(const QString& href);
// classify(href) behind a cache (kClassifyCacheSize hrefs, least recently used dropped; thread-safe). classify is
// pure (no clock, no settings), so a cached Target is always right.
constexpr int kClassifyCacheSize = 4096;
Target        classifyCached(const QString& href);

bool isYouTubeId(const QString& id);
bool isKey(const QString& key); // kKeyChars lower-case hex digits
QUrl youTubeWatchUrl(const QString& videoId); // the canonical address; invalid for an invalid id

// "youtube", "image", "gif", "video", "loop", "card", "none" (logs, tests, cache meta).
const char* kindName(Kind kind);
// "youtube", "direct", "media", "site", "web", "none" (logs, tests, cache meta).
const char* className(LinkClass cls);

// An address for the log: scheme, host and the last path segment cut to 40 characters, never the
// query, fragment, user info or port ("https://cdn.example/…/cat.gif").
QString logUrl(const QUrl& url);

// The lower-case ASCII form of a host as classify() checks it (punycode for international names, no
// trailing dot), or empty when it is not a usable public DNS name (IP literal, numeric, single label,
// local name, bad characters, too long).
QString publicHostName(const QUrl& url);

// ---- 2.5 helpers (pure; providers, the Fetcher and the tests use them) ----------------------------------------

// "https://" + host + path + ("?" + query): the page address of a new-class link (host lower case, punycode, no
// trailing dot; path and query with canonical percent-encoding, "/" for an empty path; no fragment, no port).
// Invalid for a link without a public host or one longer than kMaxUrlChars.
QUrl normalizedPage(const QUrl& url);
// The first kKeyChars hex digits of SHA-256(text).
QString keyOf(const QString& text);
// page without tracking parameters (utm_*, fbclid, gclid, ..., and "si" on Spotify and YouTube hosts); the other
// parameters keep their order and encoding.
QUrl withoutTracking(const QUrl& page);
// A link that looks like an action or carries a token (a sign-in, confirm or unsubscribe link, a download, a
// token-like path segment or query parameter): no generic card, no picture request.
bool isActionLike(const QUrl& url);
// A path segment that names an action (login, confirm, unsubscribe, download, oauth, ...), case-insensitive.
bool hasActionSegment(const QUrl& url);
// The last path segment's extension is one of a file that is never an HTML page (pdf, zip, exe, mp3, json, ...).
bool hasNonHtmlExtension(const QUrl& url);
// The 2.4 media kind of a file name by its extension (Image, Gif, Video), None otherwise.
Kind mediaKindOfName(const QString& lastSegment);
// A character that never belongs in a link (white space, controls, bidi, zero-width, BOM, line separators).
bool isBadUrlChar(QChar ch);

} // namespace linkpreview
