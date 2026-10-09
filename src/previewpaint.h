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
QColor  accent(); // #5865F2
qreal   cornerRadius();

QFont  font(const PreviewStyle& style, qreal delta, bool bold = false); // pixel size from the chat font
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

} // namespace previewpaint
