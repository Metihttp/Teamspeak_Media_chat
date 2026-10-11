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
#include <QTextDocument>
#include <QtMath>

#include "layoutdoc.h"

namespace chatscroll {

inline bool atBottom(QTextBrowser* browser)
{
    const QScrollBar* bar = browser->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4;
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
