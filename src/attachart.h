#pragma once

// 2.3 attach: the vector glyphs of the attach button and its menu, drawn with QPainter paths (sharp at
// any scale, light and dark). QtGui only: the render harness and the tests draw them too.

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace attachart {

enum class Glyph { Paperclip, Photo, Document, Poll };

// The glyph in a square box (24 x 24 logical px is its design size), in color.
void draw(QPainter& p, const QRectF& box, Glyph glyph, const QColor& color);

// The menu entries' glyph colours: a hue that keeps 3:1 against light and dark menus.
QColor hue(Glyph glyph);

// A QIcon of the glyph in its hue, as pixmaps (24 logical px at the usual scales): no icon engine of
// this DLL inside, so a copy that outlives the plugin is harmless.
QIcon icon(Glyph glyph);

} // namespace attachart
