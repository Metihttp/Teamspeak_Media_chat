#pragma once

// 2.3 poll: the poll card in the chat, like Telegram's anonymous poll: the question, "Anonymous poll",
// then the options. Before you vote: a circle (one answer) or a box (multiple answers) per option, the
// whole row a button; with multiple answers a Vote button sends the ticked ones. After you vote: per
// option its percentage, its text, its count and a bar; a check on your choice; "N votes" and "Retract
// vote" at the bottom. The colour never says it alone (the check, the numbers). Geometry, painting and hit
// tests are pure functions of Card and Style, so the render harness and the tests draw exactly what the
// chat draws. QtGui only; HD emoji through emojitext.h.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pollart {

struct Style {
    bool   dark = false;
    QFont  font;            // the chat's
    QColor base;            // the chat's background
    QColor text;            // the chat's text
    qreal  dpr      = 1.0;
    int    maxWidth = 400;  // room in the chat's content column
    // 2.3: the card is in a message bubble (Cozy with bubbles): every flat colour the bubble can have
    // (layoutart::bubbleSurfaces). The bubble is then the card's panel: no panel or frame of its own (a box in
    // a box), the content from the bubble's text column, colours checked against these. Empty: none.
    QVector<QColor> bubbles;
};

struct Card {
    QString      question;
    QStringList  options;
    bool         multiple = false;
    // After your vote (results shown):
    bool         results = false;
    QVector<int> counts;   // per option
    QVector<int> percents; // per option
    int          voters = 0;
    quint16      own    = 0; // your options
    // Before it:
    quint16      selected = 0; // multiple answers: ticked, not sent yet
    bool         canVote  = true;
    QString      note;          // why you can't vote here (shown when !canVote)
    // Your vote on its way:
    bool         sending = false;
    bool         failed  = false;
    bool         locked  = false; // can't be changed any more
};

enum class Zone { None, Option, Submit, Retract, Retry, Badge };

struct Pointer {
    Zone hover        = Zone::None;
    int  hoverIndex   = -1;
    Zone pressed      = Zone::None;
    int  pressedIndex = -1;
};

struct Layout {
    QSize           size;     // logical: the picture and the chat object
    QRectF          card;     // the rounded panel
    QRectF          question;
    QRectF          badge;    // "Anonymous poll ..."
    QVector<QRectF> rows;     // per option: the whole row (the click target)
    QRectF          note;     // the note line (can't vote, failed), empty without
    QRectF          footer;
    QRectF          votes;    // "N votes"
    QRectF          action;   // Vote / Retract vote / Retry, empty without
    Zone            actionZone = Zone::None;
};

Layout layout(const Card& card, const Style& style);
void   paint(QPainter& p, const Card& card, const Style& style, const Layout& layout, const Pointer& pointer);
// The picture at style.dpr (transparent around the panel).
QImage render(const Card& card, const Style& style, const Pointer& pointer, Layout* layoutOut = nullptr);

struct Hit {
    Zone zone  = Zone::None;
    int  index = -1;
};
// What a press at local (logical, in the picture) does: only zones that do something now.
Hit hitTest(const Card& card, const Layout& layout, const QPointF& local);

// 2.3 ux review: the Vote button's colours as painted (opaque fill, its text at 4.5:1 on it), at rest, under
// the pointer and pressed. Light skins: white text; the light fills of dark skins: a dark ink.
struct ButtonColors {
    QColor fill, text;
};
ButtonColors voteButtonColors(const Style& style, bool hovered, bool pressed);

// The texts, shared with tool tips and screen readers.
QString votesText(int voters);       // "No votes yet", "1 vote", "125 votes"
QString badgeText(bool multiple);    // "Anonymous poll", "Anonymous poll · Multiple answers"
QString badgeToolTip();              // what anonymous means here
QString describe(const Card& card);  // one plain-text description of the card (accessibility, tool tips)

} // namespace pollart
