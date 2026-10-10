#pragma once

// 2.2 reply: drawing. The Discord-style reply line that replaces a reply's quote line in the chat, its
// colours, and the small vector glyphs the reply menu items, the reply bar and the replies popup use.
// QtGui only (render_gallery and the tests draw it too). Sizes are logical pixels; images carry dpr.

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

class QPainter;

namespace replyart {

// Colours for a chat's theme; muted text keeps 4.5:1 on base.
struct Palette {
    QColor text;   // normal text
    QColor muted;  // snippets, "Replying to"
    QColor spine;  // the connector of the reply line
    QColor accent; // the flash on a message jumped to, focus rings
    QColor name;   // a name whose own colour isn't known
    QColor surface; // the reply bar's background (a step from base)
    QColor hover;   // a hovered row or button
};
Palette paletteFor(bool dark, const QColor& base);
// color made readable on base (at least minimum:1, moved towards black or white); fallback if invalid.
QColor readable(const QColor& color, const QColor& base, const QColor& fallback, double minimum = 4.5);

// A font of the chat font's family at factor times its pixel size (at least 10 px).
QFont scaledFont(const QFont& chatFont, qreal factor);

struct Header {
    QString nick;
    QString snippet; // as quoted (may end in "…"); for a file its name; empty: none
    bool    media = false;
    bool    found = false; // the original is in this chat: the name in its colour, a click jumps there
    QColor  nickColor;     // the original's nickname colour in the chat (invalid: unknown)
};

struct HeaderStyle {
    bool   dark = false;
    QColor base;            // the chat's background
    QFont  font;            // the chat font
    qreal  dpr        = 1.0;
    qreal  lineHeight = 0;  // the message's header line; the reply line is exactly as high
    bool   hovered    = false;
    bool   pressed    = false;
};

// The reply line: as wide as it needs, at most maxWidth, lineHeight high (or the font's line height).
QSize headerSize(const Header& header, const HeaderStyle& style, int maxWidth);
QImage renderHeader(const Header& header, const HeaderStyle& style, const QSize& size);
// The part a click jumps from (the name and the snippet), in the picture's coordinates.
QRectF headerTargetRect(const Header& header, const HeaderStyle& style, const QSize& size);
// The colour the name is drawn in.
QColor headerNameColor(const Header& header, const HeaderStyle& style);

// One piece of text (a name, a snippet) starting at x on baseline y, in its own direction: a Persian
// snippet reads right to left in its place while the line around it stays left to right (as
// previewrenderer's drawFileName). Emoji are HD pictures (emojitext.h). Uses the painter's font; fm
// must be its metrics.
void drawRun(QPainter& p, qreal x, qreal baseline, const QFontMetricsF& fm, const QString& text);

// ---- glyphs (stroke style of the plugin's other icons) ------------------------------------------------
enum class Glyph { Reply, Close, Clip, Replies, Jump };
void  drawGlyph(QPainter& p, Glyph glyph, const QRectF& box, const QColor& color);
QIcon glyphIcon(Glyph glyph, const QColor& color, qreal dpr, int size = 16);

} // namespace replyart
