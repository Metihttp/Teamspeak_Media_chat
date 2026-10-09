#pragma once

// 2.2 reactions: everything a reaction looks like, as pure drawing functions (QtGui only, so the
// render tools and unit tests can use them).
//  * The six reactions are coloured vector pictures (QPainterPath), drawn for this plugin in a flat,
//    friendly emoji style: no emoji font (TeamSpeak's Qt can't be relied on to draw colour emoji), no
//    bitmaps, no third-party artwork. Crisp at every device pixel ratio.
//  * The reaction row: pills of 24 px under the media, in the fixed reaction order, plus an "add"
//    pill whose slot is always reserved (hovering never changes the layout).
//  * The round "add reaction" button shown over a hovered picture that has no row yet.
// Coordinates of a preview object: the picture at (0, 0), the row right below it.

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

#include "reactions.h"

class QPainter;

namespace rx {

// "Thumbs up", "Heart", "Laughing", "Surprised", "Sad", "Fire" (i18n::t).
QString reactionName(int index);

// One reaction picture filling box (square).
void drawReaction(QPainter& p, const QRectF& box, int index);
// Line icons: a smiley with a plus (add reaction) and two people (presence line).
void drawAddReactionGlyph(QPainter& p, const QRectF& box, const QColor& color);
void drawPeopleGlyph(QPainter& p, const QRectF& box, const QColor& color);

// ---- the reaction row -----------------------------------------------------------------------------

constexpr int kRowTopMargin = 6;
constexpr int kPillHeight   = 24;
constexpr int kPillGap      = 6;
constexpr int kLineGap      = 6;
constexpr int kAddPillWidth = 32;
constexpr int kAddButton    = 28; // the round button over the picture
constexpr int kAddButtonInset = 8;

QString countText(int count); // "1".."99", "99+"

struct RowLayout {
    QSize           size;      // the row with its top margin; empty: no row
    QVector<QRectF> pills;     // relative to the row's top-left
    QVector<int>    reactions; // the reaction of each pill
    QRectF          addPill;   // reserved whenever there is a row

    bool isEmpty() const { return size.isEmpty(); }
};

// Width of a count text in the row's font (bold for your own reactions).
using MeasureText = std::function<qreal(const QString& text, bool bold)>;

// The row for view under a picture pictureWidth wide: one line as wide as max(picture, the pills),
// at most maxWidth, wrapping onto more lines when narrower. keep: a row (with only the add pill) even
// without reactions (the pointer is still on it after the last one was removed).
RowLayout layoutRow(const ReactionView& view, int pictureWidth, int maxWidth, const MeasureText& measure, bool keep = false);

// The fonts the counts are drawn in (from the chat font, like the preview texts).
QFont countFont(const QFont& chatFont, bool bold);

enum class Zone { None, Picture, AddButton, Pill, AddPill, Row };
struct ZoneHit {
    Zone   zone  = Zone::None;
    int    index = -1; // Pill: the reaction
    QRectF rect;       // of the zone, object coordinates
};
// What is at pos (object coordinates). addButton: the round button is shown on the picture.
ZoneHit zoneAt(const QSizeF& picture, const RowLayout& row, bool addButton, const QPointF& pos);
QRectF  addButtonRect(const QSizeF& picture);
bool    addButtonFits(const QSizeF& picture);

struct RowColors {
    QColor pill, pillHover, pillPressed, border, count;
    QColor mine, mineHover, minePressed, mineBorder, mineCount;
    QColor addIcon;
};
// base: the chat's background (the dark pills are translucent white over it).
RowColors rowColors(bool dark, const QColor& base);

struct RowPaint {
    int  hoverReaction   = -1; // pill under the pointer
    int  pressedReaction = -1;
    bool showAdd         = false; // the add pill (while the preview is hovered)
    bool addHover        = false;
    bool addPressed      = false;
};
void drawRow(QPainter& p, const QPointF& origin, const RowLayout& row, const ReactionView& view, const RowPaint& paint, const RowColors& colors,
             const QFont& chatFont);
void drawAddButton(QPainter& p, const QSizeF& picture, bool hover, bool pressed);

// ---- a whole preview object (what the chat shows; render_gallery draws the same) --------------------

struct ObjectState {
    bool   dark = false;
    QColor base;            // the chat's background
    QFont  font;            // the chat font
    qreal  dpr      = 1.0;
    int    maxWidth = 400;  // PreviewStyle::maxWidth
    bool   hovered  = false; // the pointer is on the preview (picture or row)
    bool   canAdd   = false; // reacting is possible here: add button / add pill while hovered
    bool   noButton = false; // no add button on the picture (audio and voice cards: the add pill and the menu only)
    bool   keep     = false; // keep an empty row (the last reaction was just removed under the pointer)
    Zone   hoverZone  = Zone::None;
    int    hoverIndex = -1;
    Zone   pressZone  = Zone::None; // held down there (drawn pressed while the pointer stays on it)
    int    pressIndex = -1;
};

struct ObjectLayout {
    QSize     picture; // logical
    RowLayout row;
    bool      button = false; // the add button is drawn on the picture
};

// The picture (logical size pictureSize, devicePixelRatio == state.dpr) with the reaction row under it
// and the add button on it. Returns picture itself when there is nothing to add; *objectSize is the
// object's logical size either way.
QImage composeObject(const QImage& picture, const QSize& pictureSize, const ReactionView& view, const ObjectState& state, ObjectLayout* layout,
                     QSize* objectSize);

// Colour pairs with the contrast each needs (render_gallery and the unit tests check them).
struct ColorPair {
    QString name;
    QColor  foreground;
    QColor  background;
    double  minimum = 4.5;
};
QVector<ColorPair> colorPairs(bool dark, const QColor& base);

} // namespace rx
