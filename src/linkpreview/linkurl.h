#pragma once

// 2.4 link previews: which links in the chat get a preview, and the stable key of each. Pure QtCore
// (unit-tested), no network.
//
// A link is previewable when it is a web address (never ts3file, client, channel, ...) of
//  * a YouTube video, https or http (TeamSpeak links "www.youtube.com/..." typed without a scheme as
//    http://; the fetch never uses the link's address, always the canonical https one):
//    youtube.com/watch?v=ID, youtube.com/shorts/ID, youtu.be/ID, also on www., m. and music.youtube.com
//    (exactly these hosts: "youtube.com.evil.example" is just another website). ID: exactly 11 of
//    [A-Za-z0-9_-];
//  * a media file on any other website, https only, by the last path segment's extension (a query is
//    allowed): .png .jpg .jpeg .webp (Image), .gif (Gif), .mp4 .webm .mov .m4v (Video).
// Never: user names or passwords in the address, ports other than 443 (80 for http), IP addresses and numeric hosts,
// single-label and local names (localhost, *.local, *.lan, *.internal, *.home.arpa, ...), addresses over
// kMaxUrlChars, control characters or white space, and TS Media's own links (its GitHub repository, the
// "#tsmupd-" update hints). Web pages are never scraped (no OpenGraph in 2.4).

#include <QString>
#include <QUrl>

namespace linkpreview {

enum class Kind { None, YouTube, Image, Gif, Video };

struct Target {
    Kind    kind = Kind::None;
    // What is fetched. YouTube: the canonical https://www.youtube.com/watch?v=<id> (a card opens the
    // link's own href, which may carry a start time). Media files: the normalized address (lower-case
    // ASCII host, no port, no fragment, percent-encoding normalized).
    QUrl    url;
    QString videoId; // YouTube only
    // Lower-case ASCII host without port or trailing dot. International names stay in their "xn--"
    // form (show this, never a Unicode form: look-alike letters can't fake a known site that way).
    QString host;
    // kKeyChars lower-case hex digits: the start of the SHA-256 of "yt:<id>" for YouTube (every address
    // form of one video shares it), else of the normalized address. The same link gives the same key on
    // every client, so it names the link in LP announcements and the cache.
    QString key;

    bool isValid() const { return kind != Kind::None; }
};

constexpr int kMaxUrlChars  = 2048;
constexpr int kMaxHostChars = 253;
constexpr int kKeyChars     = 32;
constexpr int kYouTubeIdChars = 11;

Target classify(const QUrl& url);
// An anchor's href as TeamSpeak's chat document holds it (parsed tolerantly, then the rules above).
Target classify(const QString& href);

bool isYouTubeId(const QString& id);
bool isKey(const QString& key); // kKeyChars lower-case hex digits
QUrl youTubeWatchUrl(const QString& videoId); // the canonical address; invalid for an invalid id

// "youtube", "image", "gif", "video", "none" (logs, tests).
const char* kindName(Kind kind);

// An address for the log: scheme, host and the last path segment cut to 40 characters, never the
// query, fragment, user info or port ("https://cdn.example/…/cat.gif").
QString logUrl(const QUrl& url);

// The lower-case ASCII form of a host as classify() checks it (punycode for international names, no
// trailing dot), or empty when it is not a usable public DNS name (IP literal, numeric, single label,
// local name, bad characters, too long).
QString publicHostName(const QUrl& url);

} // namespace linkpreview
