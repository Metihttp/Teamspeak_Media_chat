#pragma once

// 2.2 drag-out: the picture that follows the pointer while a file is dragged out of the chat or the
// viewer. Pure drawing (QtGui), so tools/render_gallery can show every variant.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPoint>
#include <QPointF>

struct MediaEntry;

struct DragImage {
    QImage image;   // devicePixelRatio == the requested dpr
    QPoint hotSpot; // logical pixels: where the pointer holds the picture
};

// Pictures and video posters: the still scaled to fit 160 x 160 logical pixels, rounded corners
// (8 px) with a hairline, at 85% opacity. pressFraction: where the press was inside the preview
// (0..1 each way); the pointer holds the thumbnail at the same spot.
DragImage renderDragPicture(const QImage& still, qreal dpr, const QPointF& pressFraction);

// Files, and media without a still: a 240 x 56 mini card, always dark (it floats over other apps):
// the file-type glyph, the name (displayNameFor, elided in the middle) and the size, or
// "Program · 2.4 MB" for programs and scripts (program: Core::isUnsafeToOpen).
DragImage renderDragCard(const MediaEntry& entry, bool program, const QFont& font, qreal dpr);

// The two text colours of the mini card and its background (render_gallery checks their contrast).
QColor dragCardBackground();
QColor dragCardTitleColor();
QColor dragCardDetailColor();
