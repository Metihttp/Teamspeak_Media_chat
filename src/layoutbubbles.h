#pragma once

// 2.3 chat-visuals: what ChatLayout paints under the chat's text for message bubbles (Cozy with "Message
// bubbles", like Telegram: every message on a rounded surface a step off the chat's background, yours tinted
// with the accent, the bubbles of one group meeting with small corners) and for the pills under centred
// system rows. The shapes come from the laid-out blocks (document coordinates); nothing in the document is
// changed for them (layoutdoc gives the blocks their margins). QtGui only: the tests and the harness paint
// with it too.
//
// A bubble holds the whole message block: its name row (a reply head's reply row stays above it, Discord's
// connector reaching into it), the text, previews, albums, voice cards and anything a module appends at the
// block's end (a reaction row), so a block may grow and its bubble follows. The avatar stays outside, left.

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include "layoutdoc.h"

class QPainter;
class QTextBlock;
class QTextDocument;

namespace layoutbubbles {

enum class Kind { None, Bubble, Pill };

struct Item {
    int    block = -1;
    Kind   kind  = Kind::None;
    QRectF rect;               // document coordinates
    bool   own       = false;  // your message
    bool   mention   = false;  // it mentions you
    bool   joinAbove = false;  // the bubble right above is the same group's: their corners meet small
    bool   joinBelow = false;
};

// The shape of a laid-out block (kind None: none). Bubbles for styled Cozy messages and pills for styled
// system rows, both only with env.tokens.bubbles.
Item itemOf(const QTextBlock& block, const layoutdoc::Env& env);
// The shapes of the visible blocks from..to (one more on each side: a neighbour's padding reaches in).
QVector<Item> itemsIn(QTextDocument* doc, int from, int to, const layoutdoc::Env& env);
// Its outline (rounded; small corners where it meets its group).
QPainterPath pathOf(const Item& item, const layoutart::Tokens& tokens);

// 2.3 live test: what the action bar must not cover near a hovered message (document coordinates): the lines of
// the visible message and system rows from..to, so their text, pictures and reaction rows. A Cozy head's line
// is its picture: its name row counts as wide as the name and time are (the rest is the bubble's empty top
// right), a reply row above it as wide as the picture.
QVector<QRectF> keepOut(QTextDocument* doc, int from, int to, const layoutdoc::Env& env);
// ... of the rows that reach within reach above or below block's bubble (its row without one), block's own too.
QVector<QRectF> keepOutNear(QTextDocument* doc, int block, qreal reach, const layoutdoc::Env& env);

// 2.3 live test: where the action bar goes over a hovered bubble. All in viewport coordinates: area (the
// viewport), bubble, keepOut; bar is its size; right: its inset from the right edge. As in 2.3: beside the
// bubble's top right corner (centred on a one-line bubble), or on the bubble's top edge at the right when the
// bubble is too wide for that; always inside the area (2 px). New: it never covers keepOut where it can help
// it (before, near the viewport's bottom it was pushed up over the message above): it moves up or down along
// the bubble first (within 4 px of it), then further right, then into the bubble's top right corner; where
// nothing is free, it takes the place that covers the least. textPad: what is round a text (a bubble's
// padding): moved right past a text, the bar keeps that much more air (it clears the text's bubble too).
QRect barPlace(const QSize& bar, const QRect& area, const QRect& bubble, const QVector<QRect>& keepOut, int right, int textPad = 0);

struct State {
    int hover = -1; // the block under the pointer
    int focus = -1; // the message being replied to
};
// Paints the items, offset added (document to painter coordinates): the surface, a mention's tint and bar,
// the hover layer, the focus ring.
void paint(QPainter& p, const QVector<Item>& items, const layoutdoc::Env& env, const QPointF& offset, const State& state);

} // namespace layoutbubbles
