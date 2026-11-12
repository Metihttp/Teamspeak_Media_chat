#pragma once

// 2.4 link previews: what one link's slot in the chat shows, and the made-up file links that let Core show
// a link's media like uploaded media. Pure logic (QtCore + medialink), unit-tested; ChatLinkPreviews feeds it.
//
// What a slot shows, first match wins:
//  1. Hidden (the user hid this card): the "Preview hidden · Show" pill.
//  2. Loaded here (the viewer clicked Load preview and the website answered): YouTube -> the YouTube card;
//     a picture, GIF or video -> the media itself (Core entry made by adoptLocalFile). 2.5: a card -> the Embed card;
//     a silent looping video -> the media, played like a GIF.
//  3. Announced by the message's sender (sender-made preview, files on the TeamSpeak server) and not failed:
//     YouTube -> the YouTube card (its thumbnail loading from the TeamSpeak server first); a picture / GIF, or
//     a video with its file -> the media (Core downloads it from the TeamSpeak server and checks its SHA-256);
//     a video without its file (too large) -> the poster card with "Open in browser". 2.5: an LPX card -> the Embed
//     card (its picture from the TeamSpeak server; a placeholder of the announced size until then); LPX media as LP.
//  4. (2.5) Nothing here (the page has no preview: no card, a wall, an expired link): no slot at all (Absent),
//     except after a click (Load link previews): the error strip "No preview for this page" without Retry. The
//     sender said so (LPXNO r=n / r=x) and nothing loaded here: Absent too.
//  5. Loading here -> the strip with the ring; failed -> the error strip with Retry; too large -> "Open in
//     browser"; (2.5) cooling (the site asked to wait) -> "Site busy".
//  6. Waiting (2.4 "Load link previews automatically", linkpreview/autoload.h): for our own pipeline ("Making the
//     preview…"), for the sender's preview ("Waiting for the sender's preview…"), for the answer whether the sender
//     makes one ("Checking for the sender's preview…"), or for an automatic load that is about to start -> the same
//     strip with the ring (the same size as every strip: nothing jumps).
//  7. Otherwise the click-to-load strip (with the file's size when it is above the automatic limit).
// 2.5, the quiet class: a link to another web page (LinkClass::Web) shows nothing in 5-7 (Absent; the look it would
// have had is Decision::underlying, which the chat's lazy loading reads), except for a load the user started by a
// click (Load link previews): Loading, and Error with Retry. A web link whose load found media is a media slot. Web
// slots while Settings::showWebCards is off: always Absent. A Nextcloud share (a MediaPage link: its strips as other
// media pages) whose load or LPX is a share card: Absent while that setting is off (design2 2.7).
// A sender's preview whose files fail for good (gone from the server, no permission to download them there) falls
// back to 4-6; a failure that may pass (connection, quota, a hash that didn't match while the upload was still
// arriving) keeps it, with Core's own retry.

#include <QString>

#include "core.h"
#include "linkpreview/announcement.h"
#include "linkpreview/carddata.h"
#include "linkpreview/hostgate.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/previewstore.h"

namespace linkstate {

// OverAutoLimit: an automatic load stopped because the file is larger than the automatic limit (size: what the
// website announced, -1 unknown); the click card says so and a click loads it with the click cap.
// 2.5: Nothing: the page has nothing to show (kNothing, kWalled, kExpired); Cooling: the site asked to wait
// (kRateLimited / kGateBusy on an automatic load): it loads again by itself after retryAtMs (6.4).
enum class Local { Idle, Loading, Loaded, Failed, TooLarge, OverAutoLimit, Nothing, Cooling };

// What a slot without anything to show waits for (2.4 auto-load): our own pipeline, the sender's preview, or an
// automatic website load that is due (about to start; ChatLinkPreviews starts it). Checking: we are still asking
// whether the sender makes previews (autoload::Wait::Checking). 2.5: Host: the site asked to wait (Cooling).
enum class Wait { None, Own, Sender, Checking, Auto, Host };

// The viewer's own fetch of a link (by Target::key; one per link, shared by every message showing it).
struct LocalState {
    Local   state    = Local::Idle;
    double  progress = -1.0;
    QString error;
    qint64  size = -1; // TooLarge / OverAutoLimit: what the website announced
    QString mediaKey; // Loaded picture / GIF / video: Core's key (adoptLocalFile)
    // Loaded YouTube
    QString title, author, thumbnailFile;
    // 2.5
    linkpreview::CardData card;                            // Loaded card (card.pictureFile in cache/links)
    linkpreview::Kind     kind = linkpreview::Kind::None;  // what it turned out to be (Loaded; OverAutoLimit of a page's media)
    QString               finalHost;                       // a web card loaded after a cross-site redirect: the host it went to
    qint64                retryAtMs     = 0;               // Cooling: when it may load again
    int                   coolRetries   = 0;               // hard rounds so far (429 / 503 with Retry-After)
    qint64                coolingMs     = 0;               // how long this link has been cooling in all (this session)
    bool                  softCooling   = false;           // the current round is a soft one (x-ratelimit) or a gate wait
    bool                  clicked       = false;           // the user started this load (a click, Load link previews)
    QString               serverUid;                       // OverAutoLimit: the chat's server when the limit was hit
    qint64                capHit        = -1;              // ... and the automatic cap it hit
    bool                  heldCrossSite = false;           // Nothing: a card from another website held back (afterCard)
    bool                  webCards      = true;            // Loading: the Caps::webCards its load holds (restartForWebCards)
};

// What Core says about the files of a sender's announcement (Core keys from serverLink(), the sender's linkFor()).
struct ServerState {
    bool       announced = false;
    bool       hasMedia  = false;  // the announcement names the full file
    QString    mainKey;            // the media (hasMedia) or the preview picture as the main file
    MediaState state = MediaState::Idle; // of mainKey
    MediaError error = MediaError::None;
    bool       previewFailed = false; // mainKey's preview picture (the poster of an announced full file) failed for good
    // 2.5: an LPX
    bool                  extended  = false;
    linkpreview::Kind     kind      = linkpreview::Kind::None;
    linkpreview::CardData card;     // k = c (its picture is mainKey's file, when it has one)
    bool                  sensitive = false;
};

// What Core says of mainKey's entry (nullptr: none yet), as the chat reads it into a ServerState.
void readEntry(ServerState* server, const MediaEntry* entry);

// Whether a sender's announcement of kind announced is shown under a link the chat classifies as link. The sender
// names the kind by the bytes it got (a ".jpg" that is an animated GIF goes up as a GIF, a ".gif" that is a still
// as a picture); pictures and GIFs are one family here (decide() shows both as the media). Never across families.
// 2.5: an LP only under a 2.4 link (YouTube, Direct).
bool kindFits(linkpreview::Kind announced, linkpreview::Kind link);
bool lpFits(linkpreview::Kind announced, const linkpreview::Target& link);
// 2.5: an LPX only under a link of the new classes (any of its kinds: the sender knows what the page turned out to
// be), a web page's only while Settings::showWebCards is on (webCards). A Nextcloud share's card (design2 2.7) also
// only while webCards is on; its media always.
bool lpxFits(linkpreview::Kind announced, const linkpreview::Target& link, bool webCards);

// 2.5: what decide() needs to know beyond the slot's own state.
struct Context {
    bool autoOn        = true;  // Settings::autoLoadLinkPreviews
    bool webCards      = true;  // Settings::showWebCards
    bool senderNothing = false; // the sender's plugin said the page has nothing to show or went elsewhere (LPXNO r=n / r=x)
};

struct Decision {
    bool           media = false; // show the media preview (mediaKey) instead of a card
    QString        mediaKey;
    linkcard::Look look       = linkcard::Look::Click;
    bool           fromServer = false;
    Wait           wait       = Wait::None; // look Loading: what it waits for (None: a load here is running)
    // 2.5
    linkcard::Look underlying = linkcard::Look::Click; // the look before the quiet rule of the web class (== look otherwise)
    int            index      = 0;                     // the slot's place among its message's selected links (0..2)
    bool           nothing    = false;                 // a click found nothing ("No preview for this page", no Retry)
};

// wait: what the slot would wait for when nothing else is to be shown (rule 6). A wait never beats anything
// shown, loading, failed or too large; Auto only for a link nobody tried here yet (Idle).
Decision decide(linkpreview::Kind kind, bool hidden, const LocalState& local, const ServerState& server, Wait wait = Wait::None);
// 2.5: by the link's class (the quiet web class, cards, LPX), with the context above.
Decision decide(const linkpreview::Target& target, bool hidden, const LocalState& local, const ServerState& server, Wait wait,
                const Context& context);
// What a Loading strip says it waits for. Live test fix: a sender's preview that is here already, whose file still
// comes from the TeamSpeak server (paced, one a second), is loading, never "Waiting for the sender's preview…" (that
// read wrong in history, also on our own messages).
linkcard::Waiting waitingShown(const Decision& d);

// A sender's preview that can't be shown any more: its main file's download failed for good (NotFound, Permission,
// Password), or the full file waits for a click (not loaded automatically) and its preview picture failed for good.
bool serverFailed(const ServerState& server);
// The sender's preview is shown (decide's rule 3): a usable file, or (2.5) an LPX card (its text alone is a card; a
// card that is only a picture whose picture can never come is not).
bool serverUsable(const ServerState& server);
// 2.5: an LPX card's picture can never come here: its download failed for good (gone from the server, not ours to
// download there: a private chat's sender in another channel, a channel password, a missing permission).
bool serverCardPictureGone(const ServerState& server);
// 2.5: an LPX card's data as the slot draws it: without its picture box when it has none or it can never come (a
// text card, never an empty box).
linkpreview::CardData serverCardData(const ServerState& server);

// ---- 2.5: busy sites (6.4) ---------------------------------------------------------------------------------------

constexpr int    kMaxCoolRetries     = 2;      // hard rounds an automatic load waits out before it shows the error
constexpr qint64 kMaxCoolingMs       = 600000; // ... and at most this long cooling in all
constexpr qint64 kGateBusyRetryMs    = 10000;  // kGateBusy without a time
// An answer of kRateLimited / kGateBusy (gateBusy) for a load: automatic -> Cooling until now + retryAfterMs (a hard
// round counts toward kMaxCoolRetries, soft rounds and gate waits don't; over kMaxCoolRetries or kMaxCoolingMs: Failed
// with the busy text); a click -> Failed with the busy text at once.
LocalState afterBusy(const LocalState& before, qint64 retryAfterMs, bool soft, bool gateBusy, bool automatic, qint64 nowMs);
// A Cooling state whose time has come is a candidate again (Idle for the rules), its rounds kept.
bool coolingDue(const LocalState& local, qint64 nowMs);

// 2.5 (6.4): a link nobody loaded here (Idle) whose site is cooling shows the wait (Cooling until nowMs + ms); one whose
// site is behind a bot check (walled) is Nothing, exactly as a load of it that met the wall (no slot; after a click
// "No preview for this page"), never a retryable error a click can't fix. Anything else is returned as it is.
LocalState underGate(LocalState local, linkpreview::HostGate::Verdict gate, qint64 ms, qint64 nowMs);
// What Load link previews (a click) shows at once, with no request, for a link whose site refuses it (Cooling or
// Walled): the busy line with Retry, or for a wall "No preview for this page" (live test fix: it was the plain error
// with a Retry that could never work).
LocalState clickedUnderGate(linkpreview::HostGate::Verdict gate);

// A link loaded here (no Core entry: a YouTube card, 2.5 a site or web card) whose picture file left the cache
// (pruned, cleared): forgotten, so it loads again by the usual rules instead of keeping an empty picture box.
bool localFilesGone(const LocalState& local);

// ---- 2.5: cards from another website (D18, review fix) -------------------------------------------------------------

// A web card loaded here from a page on another website than the link (a cross-site redirect: finalHost set). The
// link's own site may answer this plugin's honest user agent with a redirect it never shows browsers (a well-known
// site's real page, while the link itself opens something else), so such a card shows only for a load the user
// started (a click, Load link previews) or for our own message; an automatic or cached load gives Nothing (no slot),
// like a 2.5 sender's LPXNO r=x.
bool crossSiteCardShown(const QString& finalHost, bool clicked, bool own);
// What a card answer (a load here, our pipeline's, or the cache's) makes of local (Loading): Loaded with the card, or
// Nothing when it has no card or is a card from another website that isn't shown (crossSiteCardShown). Live test fix:
// a card held back that way is Nothing with heldCrossSite set: the cache keeps it, so our own message's slot shows it
// after all (ownShowsHeldCard), also after a restart, when our pipeline's record of the link is gone (golang.org ->
// go.dev: "a cached cross-site result must not count as Nothing for our own message").
LocalState afterCard(LocalState local, const linkpreview::CardData& card, const QString& finalHost, bool own);
// Our own message's slot over a link whose card from another website was held back: read it again, as our own.
bool ownShowsHeldCard(const LocalState& local, bool own);
// A card's data as a slot draws it: text cleaned again, a web page's own site name only when it matches host (the
// link's own host, the one the footer shows) and never for a card from another website, and a theme colour only on
// a web card that isn't from another website. 2.5: a Nextcloud share's card (provider "nextcloud") follows the web
// card's rules (providers::cardsFollowWebRules).
linkpreview::CardData shownCardData(linkpreview::CardData data, linkpreview::LinkClass cls, const QString& host, bool crossSite,
                                    const QString& provider = QString());

// 2.5 (8.3): an OverAutoLimit entry is still over the automatic cap (cap: autoload::autoCap now): the size it
// announced is above it, or it didn't say and the cap isn't above the one it hit.
bool stillOverLimit(const LocalState& local, qint64 cap);

// ---- 2.5: why a slot shows nothing or waits (3.12) ----------------------------------------------------------------

enum class HoldReason { None, Hidden, Setting, Waiting, Offscreen, Turn, Budget, Gate, Cap, SenderNothing, Nothing, Failed };
const char* holdReasonName(HoldReason reason); // "none", "hidden", "setting", ...
// turnFree / budgetFree: a free automatic slot / a budget token for the sender; gate: the site's state for its first
// request (Go: free to ask). Pure.
HoldReason holdReason(const Decision& d, const LocalState& local, const Context& context, bool onScreen, bool turnFree, bool budgetFree,
                      linkpreview::HostGate::Verdict gate, bool capOk = true);

// ---- Core links ---------------------------------------------------------------------------------------

// An announcement with the file browser that holds its files filled in where it doesn't say (announcements
// always do since the hub stores them with their server and channel; the fallbacks are the connection's
// server and the sender's or our channel).
linkpreview::Announcement withFileBrowser(linkpreview::Announcement a, const QString& serverUid, quint64 channelId);

// The link Core downloads an announcement's main file by: exactly the sender-previews' linkFor() (the full file
// when it was announced, its preview picture as its preview; else the preview picture), so Core knows one entry
// per file whichever side asks first. False when the announcement isn't usable (no server or channel, broken
// paths or hashes, an LPX card without a picture): nothing is downloaded then. Core checks the downloaded file
// against its SHA-256. 2.5: an LPX loop is a looping link, a sensitive one gets the spoiler cover.
bool serverLink(const linkpreview::Announcement& a, MediaLink* main);

// What "Open in browser" opens and "Copy link" copies for a link's slot: the link as it was written (a YouTube
// link keeps its start time and its own host, as clicking the link itself would), https (TeamSpeak links
// "www.youtube.com/..." typed without a scheme as http://); target's checked address when href isn't that link.
// 2.5: a page link opens as written, always https (its page address).
QUrl webAddress(const QString& href, const linkpreview::Target& target);

// The card's line for a failed website fetch (linkpreview::errors code): empty (the card says "Couldn't load the
// preview") for everything about the connection (blocked, offline, timeout, tls, ...); the details are logged.
// 2.5: a busy site "The site is busy right now. Try again later."; a refused picture "This picture can't be shown
// here".
QString errorText(const QString& code);
// 2.5: the page has nothing to show (kNothing, kWalled, kExpired), or a share link stopped at another web page while
// cards for other web pages are off (kWebOff): no card at all (Local::Nothing).
bool isNothingError(const QString& code);
// Review fix: a share link's load (provider "nextcloud") holds Caps::webCards as it started (LocalState::webCards): with
// it off it ends at another web page (kWebOff), with it on it loads that page. A running one whose value isn't
// Settings::showWebCards any more (webCards) starts again under the switch as it is now.
bool loadFollowsWebCards(const linkpreview::Target& target);
bool restartForWebCards(const LocalState& local, const linkpreview::Target& target, bool webCards);

// A file loaded from a website, as a made-up link for Core::adoptLocalFile: server "tsmedia-web", channel 1,
// folder "/web/<key>", the file's name from the address (kept clean) with the extension its content has
// (mime, else the address'); size and dimensions as known. Never sent anywhere. 2.5: loop (a silent looping video,
// played like a GIF) and sensitive (the spoiler cover).
MediaLink webLink(const linkpreview::Target& target, const QString& mime, qint64 size, int width, int height, qint64 durationMs, bool loop = false,
                  bool sensitive = false);
bool      isWebLink(const MediaLink& link);
QString   webServerUid();

} // namespace linkstate
