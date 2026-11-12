#pragma once

// 2.5 link previews: the table of known sites. Pure QtCore (unit-tested in tst_providers), no network.
//
// classify() (linkurl.h) asks match() for every link that isn't YouTube or a Direct media file:
//  * MediaPage rows (Tenor, Giphy, Imgur, Klipy, Reddit videos, Pinterest, Streamable, Imgflip, 9GAG, Coub,
//    Gifer, Make A Gif, Gyazo, Lightshot, ImgBB, Postimages, Pixeldrain, Tumblr): the preview is the media;
//  * Site rows (Vimeo, SoundCloud, Spotify, Apple Music, Deezer, Dailymotion, Medal, Twitch, Kick, Bluesky, X,
//    GitHub, Steam, Wikipedia, Telegram, Discord invites, Reddit posts, YouTube live/embed/clip links): a rich
//    card with the site's own brand;
//  * Exclude rows (Gfycat, RedGifs, TikTok, IMDb, Amazon, Google Maps, and private or action links of the sites
//    above): never a preview, never a request.
// Host sets are exact lower-case hosts or anchored patterns that only name hosts the provider owns (never a bare
// suffix of a brand name, never anything a page says). Path rules are anchored regular expressions over the
// decoded path. A host owned by a provider whose path matches no row is a plain web page (Web class).
//  * Shape rows (design2 2.2): on a host that no row or family owns, a path shaped like a Nextcloud or ownCloud 10
//    public share (shares.h) is the MediaPage provider "nextcloud" (sub "page", "download", "preview", "pp", "dav";
//    the item id is the share's token). Its hop lists are "the share's own host", bound at run time.
//
// The Fetcher runs each row's plan (fetchplans.cpp); every request of a plan is bound to the row's hop lists:
// pageHopAllowed() for page, oEmbed, JSON, feed, manifest and redirect requests, mediaHopAllowed() for media and
// pictures, on every redirect hop. Off the list: no card (page steps) or no picture (picture steps).
//
// Release gate: every row has an `active` flag. A row switched off is skipped by match(): its links become Web
// links (MediaPage and Site rows). Every row is active in 2.5.0 unless release prep switches it off, except Deezer's:
// switched off without requests (its links get no preview at all, its plan kept; providers.cpp kOffWithoutRequest).
// After 2.5.0 is published the rows "lx" covers are frozen (design2 2.10): switching a row on again, or adding one,
// goes under a new cap name, never by widening "lx".

#include <QRgb>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "hostgate.h"
#include "linkurl.h"

namespace linkpreview::providers {

struct Match {
    QString   id;                    // "tenor"; empty: no provider row matched
    LinkClass cls = LinkClass::None; // MediaPage or Site; None with a non-empty id: an Exclude row (or a refused link)
    Kind      expected = Kind::None;
    QString   itemId, sub;           // what the row read from the link ("8FhXc8w45aN32", "gifs")
    int       expectedRequests = 1;
};
// The first active row whose hosts contain host and whose path rule matches url (as written, http or https).
Match match(const QUrl& url, const QString& host);

// The Direct rewrites (linkurl.h): "tenor-short" for Tenor's short GIF links (tenor.com/<code>.gif), "dropbox" for
// Dropbox share links of media files; empty for every other Direct link. The link keeps its 2.4.0 key and kind.
QString directRewriteOf(const QUrl& url, const QString& host);

struct Brand {
    const char* name  = nullptr; // shown on Site cards and their strips ("Vimeo"); never taken from a page
    QRgb        light = 0, dark = 0; // the card's bar
};
// name nullptr for unknown ids and MediaPage rows (except "tumblr", whose text posts become cards).
Brand brandOf(const QString& id);
// Every brand (light and dark colour) of the table: a Web page's theme colour must not look like one.
QVector<QRgb> brandColours();
// Every id of the table (rows of every class), the shape rows ("nextcloud"), the Direct rewrites and "media-file"; for
// tests and tools.
QStringList ids();
// design2 2.7: a card of this link follows the Web card's rules: it shows only while Show cards for other web pages is
// on, may wear the page's theme colour and a site name that matches the host. The Web class, and the share cards of
// the "nextcloud" rows (whose media never depends on that switch).
bool cardsFollowWebRules(LinkClass cls, const QString& provider);

// Hosts a load of this provider may contact (normative hop lists): page requests (page, oEmbed, JSON, feed,
// manifest, redirect) and media requests (media files, pictures), each with their redirects. "media-file", the
// Web class (empty id) and YouTube (its own rules in netpolicy.h) have no list: true. An unknown id: false.
bool pageHopAllowed(const QString& id, const QUrl& url);
bool mediaHopAllowed(const QString& id, const QUrl& url);
// The same for an id whose lists are bound at run time ("nextcloud": the share's own host, boundHost): the two-argument
// forms refuse every host for such an id; boundHost is ignored by every other id.
bool pageHopAllowed(const QString& id, const QUrl& url, const QString& boundHost);
bool mediaHopAllowed(const QString& id, const QUrl& url, const QString& boundHost);
// Whether the id has hop lists (tests: every active row must).
bool hasHopLists(const QString& id);

// HostGate overrides: the page lane's spacing of a spacing group (default 1000 ms; "reddit" 6000 ms), the media
// lane's spacing and burst of a group (-1: the gate's default; githubassets.com 1000 ms, 3 per 10 s), and the
// minimum cooldown after a 429 of a registrable domain (default 1000 ms; imgur.com 600000 ms).
int pageSpacingMs(const QString& spacingGroup);
int mediaSpacingMs(const QString& spacingGroup);
int mediaBurst(const QString& spacingGroup);
int minCooldownMs(const QString& domain);

// The first request a load of target makes (gated on the GUI thread before the load's thread starts). Pure.
struct FirstRequest {
    QString        host;
    HostGate::Lane lane = HostGate::Lane::Page;
    QString        firstSegment; // the request's first path segment (walled memos are per domain and segment)
};
FirstRequest firstRequestOf(const Target& target);

// Release gate: false when every row of id is switched off (an unknown id: false).
bool isActive(const QString& id);
// Tests only: rows of these ids count as switched off (the release gate's effect without editing the table).
void setInactiveForTests(const QStringList& ids);
// Tests only: rows of these ids count as switched on (a plan kept while its rows are off, e.g. Deezer's).
void setActiveForTests(const QStringList& ids);

// Klipy's GIF pages (klipy.com/gifs/<slug>) answer an honest client with a bot challenge (403, cf-mitigated:
// challenge; reported to Klipy). A load of one whose page is walled asks Klipy's public oEmbed instead, which isn't
// behind the challenge and needs no key (author's decision 2026-11-11): wallFallbackOf() is that request (the gate
// asks it when the page's first request is walled; an empty host for every other link), klipyOEmbedUrl() its address.
FirstRequest wallFallbackOf(const Target& target);
QUrl         klipyOEmbedUrl(const QString& slug);
// The GIF Klipy's oEmbed answer names: its thumbnail_url when it is a real media file of Klipy's: written as https, a
// host of Klipy's media hop list (static.klipy.com, static2.klipy.com, static<n>.klipy.com), a path below "/", and
// net::resolveExtracted's rules (no user info, the standard port, never an SVG). Anything else (not a JSON object, no
// thumbnail_url, the answer of 2026-11-11 "https://static2.klipy.com/"): an invalid QUrl. The file itself is then
// loaded under the media rules (every hop on the hop list, magic bytes, caps).
QUrl klipyOEmbedMedia(const QByteArray& json);

// Tenor's media id of a post id: base64url (no padding) of the id's 8 big-endian bytes (11 characters);
// "https://media.tenor.com/<it>AAAPo/tenor.mp4" is the post's MP4. Empty for 0.
QString tenorMediaId(quint64 id);

// A page's og:site_name is shown on a generic card only when it matches the link's host loosely: L = the first
// label of the registrable domain with '-' removed; with 4 or more characters L must be one word of the site name
// or several consecutive words joined ("The Guardian" on theguardian.com); with 1-3 characters the whole site name
// without separators must be L. "YouTube" on evil.example.net: no.
bool siteNameMatches(const QString& siteName, const QString& host);

} // namespace linkpreview::providers
