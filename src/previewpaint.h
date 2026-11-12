#pragma once

// The drawing primitives of previewrenderer.cpp (palette, fonts, canvas, vector icons, rings), for
// preview modules that live in their own file (audiocard.cpp). Defined in previewrenderer.cpp next to
// the originals, so every preview keeps one look: same colours, stroke weights and spinner.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>

#include "previewrenderer.h"

class QFontMetricsF;
class QPainter;

namespace previewpaint {

// The theme colours of cards (see paletteFor() in previewrenderer.cpp for the contrast rules).
struct Palette {
    QColor background, backgroundHover, border, title, link, muted;
    QColor errorText, errorFill;
    QColor track, progress; // progress at least 3:1 against track and the card
    QColor placeholder, placeholderHover, hairline;
};

Palette palette(bool dark);
// 2.3: a card's colours for style: on a bubble (style.bubbles) no panel, no frame and no hover fill (the
// bubble is the panel), the texts and rings nudged to read on every bubble colour.
Palette palette(const PreviewStyle& style);
bool    onBubble(const PreviewStyle& style);
QColor  accent(); // #5865F2
qreal   cornerRadius();

QFont  font(const PreviewStyle& style, qreal delta, bool bold = false); // pixel size: the base (11 du) + delta
// 2.5 zoom: the base text size of every preview, 11 du whatever the chat font (the canvas ratio carries the chat's
// size). No module computes a size from points any more (a source-rule test checks the art modules).
qreal  basePixelSize(const QFont& font);
// The chat font's family at factor times the base size, in pixels (polls: 1.06 / 1.0 / 0.9 / 0.95).
QFont  emFont(const PreviewStyle& style, qreal factor, bool bold = false);
// 2.5 zoom live test: the chat font for a canvas drawn at ratio R. Above 1 its text is laid out unhinted (fractional
// advances; DirectWrite on Windows): hinted advances are whole pixels at the du size, so R doubled their rounding and
// glyphs of 9-10 du text stood unevenly at 200% ("A nony mous", "1:0 2"). At 1 TeamSpeak's own hinting, as in 2.4.
QFont  fontForRatio(QFont font, qreal ratio);
qreal  ratio(const PreviewStyle& style);                                // device pixel ratio, bounded
QImage canvas(const QSize& logical, qreal dpr);                         // transparent, devicePixelRatio set
void   prepare(QPainter& p);                                            // antialiasing and smooth images

// The first text that fits width (most to least informative); the last one elided if none does.
QString fitText(const QFontMetricsF& fm, const QStringList& texts, qreal width);
// A file name in its own reading direction (shown: already elided, from displayNameFor()).
void drawFileName(QPainter& p, const QRectF& rect, Qt::Alignment align, const QString& shown);

void drawPlayIcon(QPainter& p, const QRectF& box, const QColor& color);
void drawPauseIcon(QPainter& p, const QRectF& box, const QColor& color);
void drawCircularArrow(QPainter& p, const QRectF& box, const QColor& color); // replay / retry
void drawOpenIcon(QPainter& p, const QRectF& box, const QColor& color);
void drawDownloadIcon(QPainter& p, const QRectF& box, const QColor& color);
void drawAlertIcon(QPainter& p, const QRectF& box, const QColor& background, const QColor& mark);
// Determinate (progress >= 0) or indeterminate ring; the static waiting glyph when !animate.
void drawRing(QPainter& p, const QPointF& center, qreal radius, qreal width, double progress, const QColor& track, const QColor& arc, bool animate);

// 2.3: draws with background (draw gets the colour to use), or, for a card on a bubble (a transparent
// background), cuts the shape out of the picture so the bubble shows through: the ring round a badge, the
// glyph in a badge, a knob's halo.
void drawInBackground(QPainter& p, const QColor& background, const std::function<void(const QColor&)>& draw);

} // namespace previewpaint
