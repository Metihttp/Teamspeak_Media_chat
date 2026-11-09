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
// Every look but Hidden has the hide button (x) at its top right while the pointer is on the card; its room is
// kept free in every state, so nothing moves when it shows. The size of a card never depends on its pointer
// state, progress or thumbnail (no jumps while it loads), and never exceeds style.maxWidth (at least 160, as the
// chat gives it): a narrow chat gets a narrower card, never one sticking out of its column or bubble.

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include "linkpreview/linkurl.h"
#include "previewrenderer.h"

namespace linkcard {

enum class Look { Click, Loading, Error, TooLarge, YouTube, Poster, Hidden };
enum class Zone { None, Body, Load, Retry, Open, Hide, Show };
// Loading while waiting (2.4 auto-load): for our own pipeline's download, for the sender's preview, or for the answer
// whether the sender makes one (Checking: no claim either way). An automatic load that is due waits as None
// ("Loading preview…").
enum class Waiting { None, Own, Sender, Checking };

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
    QRectF thumb;   // YouTube / Poster: the 16:9 picture
    QRectF play;    // the play button on it
    QRectF title;   // YouTube: the title's lines
    QRectF footer;  // YouTube / Poster: the site line (Poster: with "Open in browser" as action)
};

Layout  layout(const Card& card, const PreviewStyle& style);
QImage  render(const Card& card, const PreviewStyle& style, const Pointer& pointer, Layout* layoutOut = nullptr);
Zone    zoneAt(const Card& card, const Layout& layout, const QPointF& pos, bool over);
// What a click on zone does there is one zone of these; Body maps to the card's primary action.
Zone    primaryZone(const Card& card);
bool    isActionable(const Card& card, Zone zone);

// "YouTube video", "Picture", "GIF", "Video".
QString kindLabel(linkpreview::Kind kind);
// The tool tip of zone (plain text, i18n::t / arg() only: QToolTip's label outlives the plugin).
QString toolTip(const Card& card, Zone zone);
// The same as QToolTip must get it: escaped rich text. A title or channel name comes from a website or from the
// sender's announcement, and QToolTip shows text that looks like HTML as rich text, which loads the pictures it
// names (a "file://host/..." picture is a Windows file share request to that host). Empty when toolTip() is.
QString toolTipHtml(const Card& card, Zone zone);
// The status line of a strip as the card shows it (tests, the live-test log).
QString statusText(const Card& card);
// A title or channel name from a website made safe to show: control and bidi characters out, whitespace runs
// one space, at most maxChars (never half a surrogate pair).
QString cleanText(const QString& text, int maxChars = 200);

// The text colours of the cards with what they are drawn on (tools/render_gallery checks them).
QVector<PreviewColorPair> colorPairs(bool dark);

} // namespace linkcard
