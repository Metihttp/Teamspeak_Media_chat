#pragma once

// Chat redesign: the reader's place in a chat across the layout's edits (ChatLayout; the tests run it on a
// QTextBrowser filled like TeamSpeak's). TeamSpeak doesn't keep its chat pinned by itself: what the reader
// looks at stays where it was, and a chat at the bottom stays at the bottom (new lines keep coming into view).
// Header only (the tests use it as it is).

#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QSizeF>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QtMath>

#include "layoutdoc.h"

namespace chatscroll {

// 2.3.1: the room we keep under the last line counts as the end too (TeamSpeak may scroll only as far as its last
// line): a chat that close to its end stays at its very end.
inline bool atBottom(QTextBrowser* browser)
{
    const QScrollBar* bar = browser->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4 - qCeil(layoutdoc::endGapExtra(browser->document()));
}

// The scroll range for the document as it is now. QTextEdit takes a new range when the layout reports a new
// size, and a layout finished outside an edit reports it on a 0 ms timer: a value set before then would be
// clamped to the old range.
inline void syncRange(QTextBrowser* browser)
{
    QAbstractTextDocumentLayout* layout = browser->document()->documentLayout();
    const QSizeF                 size   = layout->documentSize(); // lays out what is left
    const int                    want   = qMax(0, qRound(size.height()) - browser->viewport()->height());
    if (browser->verticalScrollBar()->maximum() != want)
        emit layout->documentSizeChanged(size); // QTextEdit adjusts its scroll bars now
}

struct Place {
    layoutdoc::Anchor anchor;
    bool              bottom = false;
};

inline Place capture(QTextBrowser* browser)
{
    Place p;
    p.bottom = atBottom(browser);
    p.anchor = layoutdoc::anchorAt(browser->document(), browser->verticalScrollBar()->value());
    return p;
}

// After edits: at the bottom again, or the anchor's block where it was (one that was folded away since: the
// row above it). Never the chat's top for a block Qt gives no place.
inline void restore(QTextBrowser* browser, const Place& p)
{
    syncRange(browser);
    QScrollBar* bar = browser->verticalScrollBar();
    if (p.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    const qreal y = layoutdoc::anchorY(browser->document(), p.anchor);
    if (y >= 0)
        bar->setValue(qRound(y));
}

// 2.4 link previews: the reader's place as a character (its cursor follows the edits), for a picture of ours that
// changes size under one line (a link's card that becomes its media, a sender's preview that arrives). The line at
// the view's top stays where it is on screen wherever in its message the picture is: above it, nothing moves;
// below it, everything moves with it. (Place scales its offset with the block's new height: right for a block that
// got a new look, a jump for one whose picture grew.) A chat at its bottom stays at its bottom.
struct TextPlace {
    QTextCursor cursor; // the character at the view's top; null: none (an empty chat)
    qreal       offset = 0;
    bool        bottom = false;
};

// The top of the laid-out line that holds position, in document coordinates; -1: none.
inline qreal lineTopOf(QTextDocument* doc, int position)
{
    const QTextBlock block = doc->findBlock(position);
    if (!block.isValid() || !block.isVisible() || !block.layout())
        return -1;
    const QTextLine line = block.layout()->lineForTextPosition(position - block.position());
    if (!line.isValid())
        return -1;
    return doc->documentLayout()->blockBoundingRect(block).top() + line.y();
}

inline TextPlace captureText(QTextBrowser* browser)
{
    TextPlace      p;
    QTextDocument* doc = browser->document();
    p.bottom           = atBottom(browser);
    const qreal y      = browser->verticalScrollBar()->value();
    const int   hit    = doc->documentLayout()->hitTest(QPointF(doc->documentMargin() + 1, y + 1), Qt::FuzzyHit);
    const qreal top    = hit >= 0 ? lineTopOf(doc, hit) : -1;
    if (top < 0)
        return p;
    p.cursor = QTextCursor(doc);
    p.cursor.setPosition(hit);
    p.offset = y - top;
    return p;
}

inline void restoreText(QTextBrowser* browser, const TextPlace& p)
{
    syncRange(browser);
    QScrollBar* bar = browser->verticalScrollBar();
    if (p.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    if (p.cursor.isNull() || p.cursor.document() != browser->document())
        return;
    const qreal top = lineTopOf(browser->document(), p.cursor.position());
    if (top >= 0)
        bar->setValue(qRound(top + p.offset));
}

// A run of events opened or closed (a click on its chip): the clicked row stays where it is on screen, or the
// chat stays at the bottom when it was there.
struct RowPlace {
    int   block  = -1;
    qreal offset = 0;
    bool  bottom = false;
};

inline RowPlace captureRow(QTextBrowser* browser, int block)
{
    RowPlace         p;
    QTextDocument*   doc = browser->document();
    const QTextBlock b   = doc->findBlockByNumber(block);
    p.bottom             = atBottom(browser);
    if (b.isValid() && b.isVisible()) {
        p.block  = block;
        p.offset = doc->documentLayout()->blockBoundingRect(b).top() - browser->verticalScrollBar()->value();
    }
    return p;
}

inline void restoreRow(QTextBrowser* browser, const RowPlace& p)
{
    syncRange(browser);
    QScrollBar* bar = browser->verticalScrollBar();
    if (p.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    QTextDocument*   doc = browser->document();
    const QTextBlock b   = doc->findBlockByNumber(p.block);
    if (p.block >= 0 && b.isValid() && b.isVisible())
        bar->setValue(qRound(doc->documentLayout()->blockBoundingRect(b).top() - p.offset));
}

} // namespace chatscroll
