#pragma once

// 2.5 YouTube player (design2.md 3): the pure parts. The page the helper serves for one Play (golden-tested byte for
// byte), what the page may say, the start time of a link, the sizes and conditions of the two windows, the note
// texts. Nothing here touches a window, a file or the network.

#include <QRect>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUrl>

namespace ytpage {

// Release gate of the anchored player (Y4b, design2 3.3): false until the live checks Y-L4..Y-L8 pass (Y-L15). While
// false every Play uses the popup player (a window of its own).
constexpr bool kAnchoredPlayer = false;

// Our app's own origin: a host name of our own that is never looked up (the helper answers every request to it from
// memory). YouTube gets it as the embedding app (Referer and origin=).
constexpr const char* kAppId     = "io.github.metihttp.tsmediachat";
constexpr const char* kAppOrigin = "https://io.github.metihttp.tsmediachat";
constexpr const char* kPagePath  = "/player";

// The popup's default and smallest size in du (design2 3.3); the anchored window's smallest thumbnail.
constexpr int kPopupWidthDu  = 640;
constexpr int kPopupHeightDu = 360;
constexpr int kMinWidthDu    = 356;
constexpr int kMinHeightDu   = 200;
constexpr int kMaxStartSec   = 86400;

// ^[A-Za-z0-9_-]{11}$
bool validId(const QString& id);
// A link's start time: "123", "123s", "1h2m3s" (any of the parts, in that order); anything else, or over 24 h: 0.
int parseStart(const QString& value);
// The start of a YouTube link: its t= or start= (query or fragment), parsed as above.
int startOf(const QUrl& link);

// The document for one Play (3.5). id must be valid (else empty) and startSec is clamped to 0..86400. popup: the same
// document for both windows in 2.5.0 (kept for the API). testApiUrl (player_smoke only): the iframe_api script comes
// from there (under our own origin, which the CSP then allows too). muted: TS Media's session mute (InlineMediaController::
// isMuted, "start videos muted" included): the embed address gets YouTube's mute=1, so the video starts muted before
// the plugin's commands can reach the page (the helper's web view also starts muted until the volume arrives).
QString pageHtml(const QString& id, int startSec, bool popup, const QString& testApiUrl = QString(), bool muted = false);

// What the page says (the helper's grammar, player/tsmpmsg.h). kind: 0 dropped, 1 ready, 2 state, 3 error, 4 time,
// 5 test (only with allowTest).
struct Message {
    int kind  = 0;
    int value = 0;
};
Message parseMessage(const QString& text, bool allowTest = false);
bool    validCommand(const QString& command);

// The page's CSS scale: CSS px of the player = device px / cssScale, so the player is as many CSS px as the thumbnail
// has du (s x Qt's dpr).
double cssScale(double s, double dpr);
// The anchored player may be used: the gate, a thumbnail of at least 356 x 200 du, and s >= 0.9 x the system scale (a
// Small preset or a zoomed-out chat would make it physically smaller than YouTube's minimum).
bool canPlayInPlace(bool gate, const QSizeF& thumbDu, double s, double sysScale);
// area(clip) / area(rect), 0 for an empty rect.
double visibleFraction(const QRect& rect, const QRect& clip);

// Settings::youTubeWindowSize ("WxH" in du; empty or bad: 640x360; at least 356x200, at most 4096x4096).
QSize popupSizeDu(const QString& setting);
QString popupSizeText(const QSize& du);
// The popup's client rect: du x s x dpr device px, centred on the owner's client rect (device px).
QRect popupRect(const QSize& du, double s, double dpr, const QRect& owner);
// A device-px "WxH" (ClosedByUser) back to du.
QSize sizeDuFromDevice(const QString& wxh, double s, double dpr);

// The note card (3.9). 2.6 chat proxy: Proxy (the route refused, or the player didn't get ready and the bridge recorded a
// route-level failure; design 1.5): "Can't play through the proxy", never remembered.
enum class Note { None, OnlyOnYouTube, Unavailable, CantPlay, Proxy };
Note    noteFor(int error);       // 101, 150 OnlyOnYouTube; 100 Unavailable; everything else CantPlay
bool    rememberedError(int error); // 100, 101, 150: facts about the video, kept for the session
QString noteText(Note note);
// A note's short form for a narrow card (one line above the pill): "Only on YouTube", "Can't play it here", "Video
// unavailable", "Proxy not working"; empty for any other text.
QString shortNoteText(const QString& note);
// "1:05", "1:02:03" (a time pill).
QString timeText(qint64 ms);
// https://www.youtube.com/watch?v=<id>[&t=<n>s] (the note's "Watch on YouTube" and the time a player stopped at).
QUrl    watchUrl(const QString& id, int startSec);

// The Y4-spike's page (no YouTube, no network): a box that answers "ready" when clicked.
QString spikePage();

// The texts of the Consent state and the notice (3.10).
QString consentQuestion();
QString consentLine();
QString consentNotice();
QString termsText();
QString privacyText();
QUrl    termsUrl();
QUrl    privacyUrl();

} // namespace ytpage
