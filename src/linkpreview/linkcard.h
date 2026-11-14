#pragma once

// 2.4 link previews: the cards under web links in the chat (the media itself, pictures, GIFs and videos, is
// drawn by previewrenderer like uploaded media). Pure drawing and hit testing, like pollart.h / audiocard.h:
// the renderer and the hit tests share one geometry function, so what is drawn and what is clicked never
// disagree. Sizes are logical pixels; images have devicePixelRatio == style.dpr. The look is the file
// card's (previewpaint.h): its palette, fonts, corner radius and icons; on a message bubble no panel.
//
// Looks:
//  * Click     a strip: the kind's glyph, the site, what it is, "Load preview" (the whole card loads it; its tool
//              tip says that loading connects your PC to that site, which sees your IP address); a file above the
//              automatic download limit says its size there ("Video · 38 MB"), or the limit when the site didn't
//              say ("Video · over 15 MB"), and its tool tip why it didn't load by itself
//  * Loading   the same strip with the progress ring and "Loading preview… 45%"; while it waits (2.4 auto-load)
//              "Making the preview…" (our own pipeline), "Waiting for the sender's preview…" or "Checking for the
//              sender's preview…" (still asking whether the sender makes one)
//  * Error     the alert glyph, why, "Retry"
//  * TooLarge  "Too large to preview here · 62 MB", "Open in browser"
//  * YouTube   Discord-like: a coloured bar, the channel, the title (the lines it needs, two at most; it comes with the
//              card, so its height never changes while it is shown), the 16:9 thumbnail with a play
//              button (a placeholder with a ring while it loads), the site; a click opens the video in the
//              default browser (no web engine inside TeamSpeak)
//  * Poster    a video that is too large to play here: its poster, a play glyph, its length, "Open in browser"
//  * Hidden    a small pill "Preview hidden · Show"
//  * Embed     (2.5) a site or web card, Discord-like: a bar (the site's brand colour, or the page's theme colour,
//              or a neutral grey), the site line (a well-known site's brand name in its colour, DemiBold; another
//              page's own name in grey, only when it matches the host), the author, the title (two lines), the text
//              (three, two in narrow chats), the picture (large under the text, or an 80 px thumbnail beside it; a
//              play symbol and the length on a video's), and the host with the open icon at the bottom. Every text
//              is plain text in its own reading direction, clipped to its box; the host is its own left-to-right
//              run. A page marked for adults gets the spoiler cover over its picture (a click on it reveals it).
//              Only a message's first card may be large: the others use the thumbnail (Card::slotIndex), so no card
//              changes its layout when another card of its message appears.
//  * Absent    (2.5) no slot at all (nothing is drawn: the chat puts nothing into the document).
// Strips of the new kinds: a loop has the GIF glyph, a card the page glyph; the second line names a well-known
// site's brand, "Web page", or the media's kind. "Site busy" (Waiting::Host) turns the ring with 60 s or less left
// and is still (no ring, no animation) above that.
// Every look but Hidden has the hide button (x) at its top right while the pointer is on the card; its room is
// kept free in every state, so nothing moves when it shows. The size of a card never depends on its pointer
// state, progress or thumbnail (no jumps while it loads), and never exceeds style.maxWidth (at least 160, as the
// chat gives it): a narrow chat gets a narrower card, never one sticking out of its column or bubble.

#include <QFontMetricsF>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include "linkpreview/carddata.h"
#include "linkpreview/linkurl.h"
#include "previewrenderer.h"

namespace linkcard {

// 2.5: Embed (a site or web card, Discord-like) and Absent (no slot at all: nothing is put into the chat).
enum class Look { Click, Loading, Error, TooLarge, YouTube, Poster, Hidden, Embed, Absent };
// 2.5: Reveal (the picture of a sensitive card while it is covered).
// 2.5 YouTube player: Play (the thumbnail of a playable card), Stop (the hide button's room while a player is open), and
// the Consent state's pills and links.
enum class Zone { None, Body, Load, Retry, Open, Hide, Show, Reveal, Play, Stop, ConsentPlay, ConsentBrowser, Terms, Privacy };
// 2.5 YouTube player (design2 3.7): the player's state on a YouTube card. None: the thumbnail and its play disc; Consent:
// the one-time notice drawn on the card (a 55% scrim, the question, the Terms and Privacy links, the pills Play and Open
// in browser); Starting: the disc with a busy ring; Open: the player covers the thumbnail (the anchored window); Popup:
// "Playing in a window"; Hidden: paused and hidden, a time pill; Note: why it can't play here and "Watch on YouTube".
enum class Player { None, Consent, Starting, Open, Popup, Hidden, Note };
// Loading while waiting (2.4 auto-load): for our own pipeline's download, for the sender's preview, or for the answer
// whether the sender makes one (Checking: no claim either way). An automatic load that is due waits as None
// ("Loading preview…"). 2.5: Host: the site asked to wait (HTTP 429, its allowance used up): "Site busy" (a ring with
// 60 s or less left, still above that).
enum class Waiting { None, Own, Sender, Checking, Host };
// 2.6 chat proxy (design 1.5): a strip about the route. Paused*: the Click look of an automatic load that waits (the
// chat proxy is down: "Paused · the proxy isn't working"; the Windows proxy failed: "Couldn't load through your
// Windows proxy"), Load preview. ChatProxy / Windows: the Error look with Retry. Filtered*: "Your network blocks this
// site" with "Use chat proxy" (a proxy is saved) or "Set up proxy…" (none) instead of Retry.
enum class ProxyStrip { None, PausedChatProxy, PausedWindows, ChatProxy, Windows, FilteredUseProxy, FilteredSetUp };

struct Card {
    Look              look = Look::Click;
    linkpreview::Kind kind = linkpreview::Kind::None;
    QString           host;   // "youtube.com", shown as is (the caller cleans it)
    QString           title;  // YouTube, sanitized
    QString           author; // YouTube channel, sanitized
    QImage            thumbnail; // YouTube / Poster: any size (drawn to cover the 16:9 box); null: placeholder
    bool              thumbnailLoading = false; // the placeholder gets a ring
    double            progress   = -1.0; // Loading: 0..1, < 0 unknown
    Waiting           waiting    = Waiting::None; // Loading: what it waits for
    QString           error;             // Error: one short line
    qint64            size       = -1;   // TooLarge, Click (above the automatic limit): bytes (< 0 unknown)
    qint64            durationMs = 0;    // Poster
    // Click: it is above the automatic download limit (2.4 auto-load), also when its size is unknown; autoLimit: that
    // limit in bytes (shown as "over 15 MB" when the size is unknown; <= 0: not shown).
    bool              overAutoLimit = false;
    qint64            autoLimit     = -1;
    // 2.5
    linkpreview::LinkClass cls = linkpreview::LinkClass::None;
    QString           provider;       // providers.h id of a site / media page (its brand: name and colour)
    linkpreview::CardData data;       // Embed: site (shown as given: the chat applied siteNameMatches), author, title,
                                      // description, picture size, layout, colour (Web), play, duration, sensitive
    QImage            picture;        // Embed: the picture, any size (drawn to cover its box); null: a placeholder
    bool              pictureLoading = false; // ... with a ring (the sender's picture is still coming)
    bool              revealed       = false; // a sensitive picture shown (the user clicked it, or "show spoilers")
    int               slotIndex      = 0;     // the slot's place among its message's selected links (only 0 may be large)
    QString           redirectedTo;   // Embed: a web card loaded after a cross-site redirect: the host it went to (host
                                      // stays the link's own, what a click opens; the footer says "host → redirectedTo")
    bool              fromSender     = false; // Embed: made by the sender's TS Media (an LPX)
    qint64            retryInMs      = 0;     // Waiting::Host: until the site may be asked again
    QString           busySite;               // Waiting::Host: the site that asked to wait (its registrable domain)
    bool              nothing        = false; // Error: a click found nothing ("No preview for this page", no Retry)
    // 2.5 YouTube player (Look::YouTube only): the player's state; playable: Play goes to our player (else the whole card
    // opens the browser as in 2.4); note: the Note's line; resumeMs: a remembered position (a time pill on the thumbnail).
    Player            player   = Player::None;
    bool              playable = false;
    QString           note;
    qint64            resumeMs = 0;
    // 2.6 chat proxy: the route's strip (Click and Error only) and the failure's short text (the tool tips' %1).
    ProxyStrip        proxyStrip = ProxyStrip::None;
    QString           proxyReason;
};

struct Pointer {
    bool over    = false; // the pointer is on the card (shows the hide button and the hover look)
    Zone hover   = Zone::None;
    Zone pressed = Zone::None;
};

struct Layout {
    QSize  size;
    QRectF icon;    // strips: the kind's glyph / ring / alert
    QRectF text;    // strips: the two lines
    QRectF action;  // the right-hand action (Load / Retry / Open), empty: none
    QRectF hide;    // the hide button (its room)
    QRectF thumb;   // YouTube / Poster: the 16:9 picture; Embed: the picture's box (empty: none)
    QRectF play;    // the play button on it
    QRectF title;   // YouTube / Embed: the title's lines
    QRectF footer;  // YouTube / Poster / Embed: the site line (Poster: with "Open in browser" as action)
    // 2.5 Embed
    QRectF bar;
    QRectF site, author, description;
    QStringList titleLines, descriptionLines;
    QString siteText, authorText;   // elided as drawn
    bool   thumbSide = false;       // the picture beside the text (80 x 80)
    // Each paragraph's reading direction, decided once from its whole text (a Persian title's line that starts with a
    // Latin word still reads right to left), and the box each of its lines is drawn in (the card's first text line
    // is as narrow as it was wrapped: it leaves the hide button its room, also when it is right-aligned).
    bool   siteRtl = false, authorRtl = false, titleRtl = false, descriptionRtl = false;
    QVector<QRectF> titleRects, descriptionRects;
    linkpreview::CardLayout pictureLayout = linkpreview::CardLayout::None; // as drawn (index > 0: never Large)
    // 2.5 YouTube player: the Consent state's parts (empty otherwise), the lines as drawn.
    QRectF      consentBox;                 // the scrim (the card's content area)
    QRectF      question, notice, terms, privacy, consentPlay, consentBrowser;
    QStringList questionLines, noticeLines;
    bool        linksStacked = false, pillsStacked = false;
};

// 2.5: the Embed card's numbers (9.2), for tests.
constexpr int kEmbedWidth       = 400;
constexpr int kEmbedThumb       = 80;
constexpr int kEmbedMaxPicture  = 300;
constexpr int kEmbedMinThumbColumn = 240;
constexpr int kEmbedNarrow      = 280;

Layout  layout(const Card& card, const PreviewStyle& style);
QImage  render(const Card& card, const PreviewStyle& style, const Pointer& pointer, Layout* layoutOut = nullptr);
Zone    zoneAt(const Card& card, const Layout& layout, const QPointF& pos, bool over);
// What a click on zone does there is one zone of these; Body maps to the card's primary action.
Zone    primaryZone(const Card& card);
// 2.5 YouTube player: a player is open on the card (Starting, Open, Popup, Hidden): its (x) is Stop.
bool    playerOpen(const Card& card);
// The context menu's default (bold) item: what a click on the card does (Load, Retry, Show), else Open in browser.
// "No preview for this page" (Error with nothing) has no Retry: Open.
Zone    menuDefault(const Card& card);
bool    isActionable(const Card& card, Zone zone);

// "YouTube video", "Picture", "GIF", "Video"; 2.5: a loop "GIF", a card "Web page".
QString kindLabel(linkpreview::Kind kind);
// 2.5: what a strip says the link is: a well-known site's brand ("Vimeo"), "Web page", "Shared file" (a Nextcloud or
// ownCloud share), or the media's kind.
QString stripLabel(const Card& card);
// 2.5: the bar's and the brand line's colours of a card (readable on the backgrounds of style: 3:1, 4.5:1).
QColor  barColour(const Card& card, const PreviewStyle& style);
QColor  brandLineColour(const Card& card, const PreviewStyle& style);
// The tool tip of zone (plain text, i18n::t / arg() only: QToolTip's label outlives the plugin).
QString toolTip(const Card& card, Zone zone);
// The same as QToolTip must get it: escaped rich text. A title or channel name comes from a website or from the
// sender's announcement, and QToolTip shows text that looks like HTML as rich text, which loads the pictures it
// names (a "file://host/..." picture is a Windows file share request to that host). Empty when toolTip() is.
QString toolTipHtml(const Card& card, Zone zone);
// 2.5 YouTube player: the Note state's text as drawn: the note in the lines it needs (at most three), and the pill's text with its
// icon, without it, or just "YouTube" (a narrow card: nothing is cut). popupPillText: "Playing in a window", or "In a
// window" where that doesn't fit.
struct NoteText {
    QStringList lines;
    QString     pill;
    bool        pillIcon  = true;
    qreal       pillWidth = 0;
};
NoteText noteTextFor(const Card& card, const Layout& l, const PreviewStyle& style);
QString  popupPillText(const Layout& l, const PreviewStyle& style);
// The status line of a strip as the card shows it (tests, the live-test log).
QString statusText(const Card& card);
// 2.6: the strip's action label ("Load preview", "Retry", "Use chat proxy", "Set up proxy…", "Open in browser").
QString actionLabel(const Card& card);
// 2.6 chat proxy: the strip line of a proxy failure (the Error look's error text), empty for None and the paused ones.
QString proxyErrorText(ProxyStrip strip);
// Everything render() draws a card from, as one string: the chat draws a card again only when it changes (a "Site
// busy" countdown when its shown number changes, a picture when it arrives, a cover when it is revealed, ...). The
// time of a turning ring is not in it (the chat adds it).
QString signature(const Card& card, const PreviewStyle& style, const Pointer& pointer);
// 2.5: an Embed card's footer as drawn in width with fm: the link's own host (a long one loses its start, never its
// end); after a cross-site redirect " → " and the host it went to, which is shortened or left out first, so the
// link's own host (what a click opens) always shows.
QString footerText(const Card& card, const QFontMetricsF& fm, qreal width);
// A title or channel name from a website made safe to show: textclean::clean (one line, invisible and reordering
// characters out, ZWNJ/ZWJ kept between visible characters, at most maxChars with "…" when cut).
QString cleanText(const QString& text, int maxChars = 200);

// The text colours of the cards with what they are drawn on (tools/render_gallery checks them).
QVector<PreviewColorPair> colorPairs(bool dark);

} // namespace linkcard
