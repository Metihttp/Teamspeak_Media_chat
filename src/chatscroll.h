#pragma once

// Chat redesign: the reader's place in a chat across the layout's edits (ChatLayout; the tests run it on a
// QTextBrowser filled like TeamSpeak's). TeamSpeak doesn't keep its chat pinned by itself: what the reader
// looks at stays where it was, and a chat at the bottom stays at the bottom (new lines keep coming into view).
// Header only (the tests use it as it is).

#include <QAbstractTextDocumentLayout>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QScrollBar>
#include <QSizeF>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QtMath>

#include "layoutdoc.h"
#include "layoutformat.h" // 2.7 (R-DEF-1): the layout's own text is not the reader's

namespace chatscroll {

// 2.3.1: the room we keep under the last line counts as the end too (TeamSpeak may scroll only as far as its last
// line): a chat that close to its end stays at its very end.
inline bool atBottom(QTextBrowser* browser)
{
    const QScrollBar* bar = browser->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4 - qCeil(layoutdoc::endGapExtra(browser->document()));
}

// 2.5 zoom: how often syncRange laid the whole chat out (the tests count it: a zoom gesture must not do it per notch).
inline int& syncRangeCalls()
{
    static int calls = 0;
    return calls;
}

// The scroll range for the document as it is now. QTextEdit takes a new range when the layout reports a new
// size, and a layout finished outside an edit reports it on a 0 ms timer: a value set before then would be
// clamped to the old range.
inline void syncRange(QTextBrowser* browser)
{
    ++syncRangeCalls();
    QAbstractTextDocumentLayout* layout = browser->document()->documentLayout();
    const QSizeF                 size   = layout->documentSize(); // lays out what is left
    const int                    want   = qMax(0, qRound(size.height()) - browser->viewport()->height());
    if (browser->verticalScrollBar()->maximum() != want)
        emit layout->documentSizeChanged(size); // QTextEdit adjusts its scroll bars now
}

// 2.6 (R-DEF-1, design 8.1): to the chat's very end after an edit. Right after a small edit QTextEdit still has the old
// range (the layout reports the new size on its 0 ms timer), so a plain setValue(maximum()) lands short by the edit's
// growth, and every later pass then treats the reader as reading above the end: syncRange first.
inline void toBottom(QTextBrowser* browser)
{
    syncRange(browser);
    QScrollBar* bar = browser->verticalScrollBar();
    bar->setValue(bar->maximum());
}

struct Place {
    layoutdoc::Anchor anchor;
    bool              bottom = false;
    // 2.5 (review fix): the anchor block's start as a cursor, which follows the edits made while the place is held
    // (TeamSpeak's line limit trims blocks at the top and renumbers every block; a zoom gesture holds its place for
    // its whole length). The anchor's block number is taken from it when it is restored.
    QTextCursor cursor;
};

inline Place capture(QTextBrowser* browser)
{
    Place p;
    p.bottom = atBottom(browser);
    p.anchor = layoutdoc::anchorAt(browser->document(), browser->verticalScrollBar()->value());
    const QTextBlock block = browser->document()->findBlockByNumber(p.anchor.block);
    if (block.isValid()) {
        p.cursor = QTextCursor(browser->document());
        p.cursor.setPosition(block.position());
    }
    return p;
}

// The anchor as it is now: its block followed through the edits since capture (the cursor), its offset as captured.
inline layoutdoc::Anchor anchorNow(QTextDocument* doc, const Place& p)
{
    layoutdoc::Anchor a = p.anchor;
    if (!p.cursor.isNull() && p.cursor.document() == doc)
        a.block = p.cursor.block().blockNumber();
    return a;
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
    const qreal y = layoutdoc::anchorY(browser->document(), anchorNow(browser->document(), p));
    if (y >= 0)
        bar->setValue(qRound(y));
}

// 2.5 zoom: the anchor only, cheaply, during a zoom gesture (ChatScale's hold): the document is laid out down to the
// anchor's block (blockBoundingRect), never the whole chat (no syncRange: documentSize() would lay out all of it). A
// chat at the bottom is left alone here (its place comes back once, when the gesture settles).
inline void restoreAnchor(QTextBrowser* browser, const Place& p)
{
    if (p.bottom)
        return;
    const qreal y = layoutdoc::anchorY(browser->document(), anchorNow(browser->document(), p));
    if (y >= 0)
        browser->verticalScrollBar()->setValue(qRound(y));
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

// ---- 2.7 (R-DEF-1): the reader's place across the plugin's unload and load ------------------------------------
// TextPlace and Place read the place where it is now, which may be one of our pictures, rows or cards: the unload
// takes those away and the load puts them back, so the place jumps. The reader's place is taken on TeamSpeak's own
// text instead (a message's top, or a line of its text), which is there in TeamSpeak's look and in ours, and its
// offset is never scaled. ChatIntegration takes it once before the unload's first edit and puts it back once after
// the last one; it carries it to the next load on the chat widget (Carry), and LoadHold keeps it while the load styles
// the chat again.

// A character TeamSpeak wrote: not a picture of anyone (U+FFFC), not a line break, not a folded album link (chatalbums'
// U+2060), not the layout's own text (layoutformat::isOurs). It is still there after the plugin gave the chat back,
// and when the plugin styles it again. from and to are positions in the block; the first such position in [from, to)
// (in the block), -1: none.
inline int firstReaderChar(const QTextBlock& block, int from, int to)
{
    const int base = block.position();
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const int start = f.position() - base;
        const int end   = start + f.length();
        if (end <= from)
            continue;
        if (start >= to)
            break;
        const QTextCharFormat format = f.charFormat();
        if (format.isImageFormat() || layoutformat::isOurs(format))
            continue;
        const QString text = f.text();
        for (int i = qMax(from, start); i < qMin(to, end); ++i) {
            const QChar c = text.at(i - start);
            if (c != QChar::ObjectReplacementCharacter && c != QChar::LineSeparator && c != QChar::ParagraphSeparator
                && c != QLatin1Char('\n') && c.unicode() != 0x2060)
                return i;
        }
    }
    return -1;
}

struct ReaderPlace {
    QTextCursor cursor;        // Block: a character of the message (its first, followed through edits); Line: TeamSpeak's
                               // first character on that line. Null: nothing to keep (an empty chat, or the end only).
    bool        line   = false;
    qreal       offset = 0;    // the view's top minus the message's top (Block) or the line's top (Line)
    bool        bottom = false;
};

// Where the place's message (Block) or line (Line) is now, in document coordinates; -1: hidden, gone, another document.
inline qreal readerY(QTextDocument* doc, const ReaderPlace& p)
{
    if (!doc || p.cursor.isNull() || p.cursor.document() != doc)
        return -1;
    if (p.line)
        return lineTopOf(doc, p.cursor.position());
    const QTextBlock block = p.cursor.block();
    if (!block.isValid() || !block.isVisible())
        return -1;
    return doc->documentLayout()->blockBoundingRect(block).top();
}

// The scroll value that puts the place back: its message's or line's top plus the offset; -1: none (gone, another
// document). 2.7 review fix: a message the load folded away (a run of join and leave events the reader had expanded, an
// album's later message) goes to the shown row above it, which it folded into, at its top (layoutdoc::anchorY's rule),
// instead of the place being given up while the load styles the chat.
inline qreal readerTarget(QTextDocument* doc, const ReaderPlace& p)
{
    const qreal y = readerY(doc, p);
    if (y >= 0)
        return y + p.offset;
    if (!doc || p.cursor.isNull() || p.cursor.document() != doc)
        return -1;
    const QTextBlock block = p.cursor.block();
    if (!block.isValid() || block.isVisible())
        return -1;
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    for (QTextBlock above = block.previous(); above.isValid(); above = above.previous()) {
        if (above.isVisible())
            return layout->blockBoundingRect(above).top();
    }
    for (QTextBlock below = block.next(); below.isValid(); below = below.next()) {
        if (below.isVisible())
            return layout->blockBoundingRect(below).top();
    }
    return -1;
}

// The reader's place now: at the end, or the first line of TeamSpeak's text that the reader sees (more than half of it
// below the view's top). It is the message (Block) when that line is its message's first line of TeamSpeak's text (the
// view's top in the gap above it, in our head row or in its first line), else that line (Line). A view that shows no
// such line (one big picture): the message at the view's top.
inline ReaderPlace captureReader(QTextBrowser* browser)
{
    ReaderPlace    p;
    QTextDocument* doc = browser->document();
    p.bottom           = atBottom(browser);
    if (doc->characterCount() <= 1)
        return p;
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const qreal                  y      = browser->verticalScrollBar()->value();
    const qreal                  h      = browser->viewport()->height();
    const int                    hit    = layout->hitTest(QPointF(doc->documentMargin() + 1, y + 1), Qt::FuzzyHit);
    QTextBlock                   block  = hit >= 0 ? doc->findBlock(hit) : doc->begin();
    QTextBlock                   atTop; // the first visible block that reaches below the view's top
    for (; block.isValid(); block = block.next()) {
        if (!block.isVisible() || !block.layout())
            continue;
        const QRectF r = layout->blockBoundingRect(block);
        if (r.top() > y + h)
            break;
        if (!atTop.isValid() && r.bottom() > y)
            atTop = block;
        QTextLayout* lines         = block.layout();
        bool         firstCounting = true;
        for (int i = 0; i < lines->lineCount(); ++i) {
            const QTextLine line = lines->lineAt(i);
            const int       at   = firstReaderChar(block, line.textStart(), line.textStart() + line.textLength());
            if (at < 0)
                continue;
            const bool  first = firstCounting;
            firstCounting     = false;
            const qreal top   = r.top() + line.y();
            if (top > y + h)
                break;
            if (top + line.height() - y <= line.height() / 2) // the reader is not reading it (at most half of it shows)
                continue;
            p.cursor = QTextCursor(doc);
            if (first) {
                p.cursor.setPosition(block.position());
                p.offset = y - r.top();
            } else {
                p.cursor.setPosition(block.position() + at);
                p.line   = true;
                p.offset = y - top;
            }
            return p;
        }
    }
    if (!atTop.isValid())
        return p;
    p.cursor = QTextCursor(doc);
    p.cursor.setPosition(atTop.position());
    p.offset = y - layout->blockBoundingRect(atTop).top();
    return p;
}

// After edits: at the end again (the range synced), or the place's message or line where it was.
inline void restoreReader(QTextBrowser* browser, const ReaderPlace& p)
{
    syncRange(browser);
    QScrollBar* bar = browser->verticalScrollBar();
    if (p.bottom) {
        bar->setValue(bar->maximum());
        return;
    }
    const qreal y = readerTarget(browser->document(), p);
    if (y >= 0)
        bar->setValue(qRound(y));
}

// The place as the unload left it, carried to the next load on TeamSpeak's chat widget: one dynamic property holding a
// byte string of numbers built at run time (no text of the chat; the document's address is compared, never followed).
// The next load uses it only if the chat is still exactly where the unload left it (resolveCarry); otherwise it reads
// the place anew (the user's own scrolling wins). QObject::setProperty copies the name into its own QByteArray.
inline const char* carryName()
{
    return "tsmediaReaderPlace";
}

struct Carry {
    bool    bottom      = false;
    bool    line        = false;
    int     pos         = -1; // the cursor's position after the unload's edits
    int     inBlock     = 0;  // pos - its block's position
    int     blockLength = 0;  // that block's length
    uint    blockHash   = 0;  // qHash(block.text(), 0): a fingerprint, no text is stored
    int     fromEnd     = 0;  // characterCount() - pos (finds it again after TeamSpeak trimmed the top)
    double  offset      = 0;  // the place's offset as read in the styled chat (what the load restores)
    double  rawOffset   = 0;  // the scroll value after the unload's restore minus readerY then (offset unless clamped)
    quint64 doc         = 0;  // the QTextDocument's address (compared only)
};

// "r1 b l pos inBlock len hash fromEnd offset rawOffset doc" (QByteArray::number and append only: Qt's heap); empty:
// nothing worth carrying.
inline QByteArray encodeCarry(const Carry& c)
{
    if (!c.bottom && c.pos < 0)
        return QByteArray();
    QByteArray out;
    out.append('r').append('1');
    const auto add = [&out](const QByteArray& field) { out.append(' ').append(field); };
    add(QByteArray::number(c.bottom ? 1 : 0));
    add(QByteArray::number(c.line ? 1 : 0));
    add(QByteArray::number(c.pos));
    add(QByteArray::number(c.inBlock));
    add(QByteArray::number(c.blockLength));
    add(QByteArray::number(c.blockHash));
    add(QByteArray::number(c.fromEnd));
    add(QByteArray::number(c.offset, 'f', 3));
    add(QByteArray::number(c.rawOffset, 'f', 3));
    add(QByteArray::number(c.doc));
    return out;
}

// False: empty, another version, garbage.
inline bool decodeCarry(const QByteArray& bytes, Carry* out)
{
    const QList<QByteArray> f = bytes.split(' ');
    if (f.size() != 11 || f.at(0).size() != 2 || f.at(0).at(0) != 'r' || f.at(0).at(1) != '1')
        return false;
    bool      ok[10] = {};
    Carry     c;
    const int bottom = f.at(1).toInt(&ok[0]);
    const int line   = f.at(2).toInt(&ok[1]);
    c.pos            = f.at(3).toInt(&ok[2]);
    c.inBlock        = f.at(4).toInt(&ok[3]);
    c.blockLength    = f.at(5).toInt(&ok[4]);
    c.blockHash      = f.at(6).toUInt(&ok[5]);
    c.fromEnd        = f.at(7).toInt(&ok[6]);
    c.offset         = f.at(8).toDouble(&ok[7]);
    c.rawOffset      = f.at(9).toDouble(&ok[8]);
    c.doc            = f.at(10).toULongLong(&ok[9]);
    for (bool b : ok) {
        if (!b)
            return false;
    }
    if ((bottom != 0 && bottom != 1) || (line != 0 && line != 1) || !qIsFinite(c.offset) || !qIsFinite(c.rawOffset))
        return false;
    c.bottom = bottom == 1;
    c.line   = line == 1;
    if (out)
        *out = c;
    return true;
}

// After restoreReader, on the chat as the unload gave it back.
inline Carry carryOf(QTextBrowser* browser, const ReaderPlace& place)
{
    Carry          c;
    QTextDocument* doc = browser->document();
    c.bottom           = place.bottom;
    c.line             = place.line;
    c.offset           = place.offset;
    c.rawOffset        = place.offset;
    c.doc              = static_cast<quint64>(reinterpret_cast<quintptr>(doc));
    if (place.cursor.isNull() || place.cursor.document() != doc)
        return c;
    const QTextBlock block = place.cursor.block();
    c.pos                  = place.cursor.position();
    c.inBlock              = c.pos - block.position();
    c.blockLength          = block.length();
    c.blockHash            = qHash(block.text(), 0);
    c.fromEnd              = doc->characterCount() - c.pos;
    const qreal y          = readerY(doc, place);
    if (y >= 0)
        c.rawOffset = browser->verticalScrollBar()->value() - y;
    return c;
}

// The carried place, if the chat is still where the unload left it: the same document and at its end (for the end), or
// the place's block unchanged (found at its position, or at its distance from the end after TeamSpeak trimmed the top)
// and the scroll value where the unload put it (2 px). *out: the place with the carried (styled) offset.
inline bool resolveCarry(QTextBrowser* browser, const Carry& c, ReaderPlace* out)
{
    QTextDocument* doc = browser->document();
    if (c.doc != static_cast<quint64>(reinterpret_cast<quintptr>(doc)))
        return false;
    if (c.bottom) {
        if (!atBottom(browser))
            return false;
        ReaderPlace p;
        p.bottom = true;
        if (out)
            *out = p;
        return true;
    }
    const int  count   = doc->characterCount();
    const auto matches = [&](int at) {
        if (at < 0 || at >= count)
            return false;
        const QTextBlock block = doc->findBlock(at);
        return block.isValid() && at - block.position() == c.inBlock && block.length() == c.blockLength && qHash(block.text(), 0) == c.blockHash;
    };
    int pos = c.pos;
    if (!matches(pos)) {
        pos = count - c.fromEnd;
        if (!matches(pos))
            return false;
    }
    ReaderPlace p;
    p.cursor = QTextCursor(doc);
    p.cursor.setPosition(pos);
    p.line        = c.line;
    p.offset      = c.offset;
    const qreal y = readerY(doc, p);
    if (y < 0 || qAbs(browser->verticalScrollBar()->value() - (y + c.rawOffset)) > 2)
        return false;
    if (out)
        *out = p;
    return true;
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
