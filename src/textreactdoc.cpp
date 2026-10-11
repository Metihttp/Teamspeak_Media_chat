#include "textreactdoc.h"

#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>

#include "emojiformat.h" // what an HD emoji keeps of TeamSpeak's format (scrubCopied)
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h" // what a recoloured fragment keeps of TeamSpeak's format (scrubCopied)
#include "polldoc.h" // 2.3: a poll message reads the same before and after it became a card
#include "replydoc.h"
#include "textreact.h"

namespace textreactdoc {

namespace {

// ChatIntegration's preview objects, cards and album grids are named "tsmedia:<key>" (kScheme there).
bool isPreview(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.toImageFormat().name().startsWith(QLatin1String("tsmedia:"));
}

bool isFileLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().startsWith(QLatin1String("ts3file:"), Qt::CaseInsensitive);
}

bool only(const QString& text, QChar c)
{
    if (text.isEmpty())
        return false;
    for (const QChar x : text) {
        if (x != c)
            return false;
    }
    return true;
}

// Our picture or our line break: the format and the character. TeamSpeak's text that took a row's format
// (its next line copies the last character's format, see scrubCopied) is never one of ours: it is never
// read as a row, and never taken out with one.
bool isOurs(const QTextFragment& f)
{
    const QTextCharFormat cf = f.charFormat();
    return (isRowObject(cf) && only(f.text(), QChar::ObjectReplacementCharacter)) || (isRowSeparator(cf) && only(f.text(), QChar::LineSeparator));
}

bool carriesRow(const QTextFormat& f)
{
    return f.hasProperty(kRowObject) || f.hasProperty(kRowKey) || f.hasProperty(kRowSeparator);
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

} // namespace

QString objectPrefix()
{
    return QString::fromLatin1("tsmedia-treact:");
}

// ---- reading ------------------------------------------------------------------------------------------

Identity identify(const QTextBlock& block)
{
    Identity id;
    if (!block.isValid() || !block.isVisible())
        return id; // hidden: a later message of an album, a folded run
    const layoutdoc::Row row = layoutdoc::read(block);
    if (row.type != layoutdoc::RowType::Message)
        return id;
    const replydoc::Message m = replydoc::parseBlock(block);
    if (m.block < 0)
        return id;
    id.message = true;
    id.uid     = m.uid.isEmpty() ? row.uid : m.uid;
    id.media   = !m.mediaLabel.isEmpty();
    id.text    = m.text;
    for (auto it = block.begin(); !it.atEnd() && !id.media; ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid() && (isPreview(f.charFormat()) || isFileLink(f.charFormat())))
            id.media = true;
        // 2.3 poll: a poll card reads as its summary; its key is the message as TeamSpeak shows it (the
        // card's original text), so the key is the same whether the card is there yet or not, everywhere.
        if (f.isValid() && polldoc::isObject(f.charFormat())) {
            const QString summary = polldoc::summaryText(f.charFormat());
            const int     at      = summary.isEmpty() ? -1 : id.text.indexOf(summary);
            if (at >= 0)
                id.text.replace(at, summary.size(), polldoc::copyText(f.charFormat()));
        }
    }
    id.seconds = layoutart::secondsOf(row.time);
    id.hash    = textreact::contentHash(id.uid, id.text);
    return id;
}

QDate dayOf(const QDate& date, int seconds, const QDate& today, int nowSeconds)
{
    if (date.isValid())
        return date;
    if (seconds >= 0 && seconds > nowSeconds + textreact::kFutureSec)
        return today.addDays(-1); // before midnight, read after it
    return today;
}

QString localKeyOf(const Identity& id, const QDate& day, qint64* timeSec)
{
    const qint64 t = id.eligible() ? textreact::epochOf(day, id.seconds) : 0;
    if (timeSec)
        *timeSec = t;
    return t > 0 ? textreact::localKey(id.hash, t) : QString();
}

QString localKeyOf(const Identity& id, const QDate& day, int utcOffsetSec, qint64* timeSec)
{
    const qint64 t = id.eligible() ? textreact::epochOf(day, id.seconds, utcOffsetSec) : 0;
    if (timeSec)
        *timeSec = t;
    return t > 0 ? textreact::localKey(id.hash, t) : QString();
}

// ---- the row --------------------------------------------------------------------------------------------

Row rowOf(const QTextBlock& block)
{
    Row row;
    if (!block.isValid())
        return row;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (!isOurs(f))
            continue;
        if (isRowSeparator(cf) && row.separator < 0) {
            row.separator = f.position();
        } else if (isRowObject(cf) && row.object < 0) {
            row.object = f.position();
            row.format = cf.toImageFormat();
            row.key    = cf.stringProperty(kRowKey);
        }
    }
    return row;
}

QRectF rowRect(const QTextBlock& block)
{
    const Row row = rowOf(block);
    if (!row.exists() || !block.layout() || !block.isVisible() || !block.document())
        return {};
    const int       at   = row.object - block.position();
    const QTextLine line = block.layout()->lineForTextPosition(at);
    if (!line.isValid())
        return {};
    const QRectF br = block.document()->documentLayout()->blockBoundingRect(block);
    // AlignBottom: the picture's bottom is the line's (the line is as tall as the row).
    return QRectF(br.left() + line.cursorToX(at), br.top() + line.y() + line.height() - row.format.height(), row.format.width(), row.format.height());
}

bool insertRow(QTextDocument* doc, int n, const QString& name, const QString& key, const QSizeF& size)
{
    if (!doc)
        return false;
    const QTextBlock b = doc->findBlockByNumber(n);
    if (!b.isValid() || rowOf(b).exists())
        return false;
    if (rowOf(b).separator >= 0)
        removeRow(doc, n); // a line break of ours left alone
    const QTextBlock block = doc->findBlockByNumber(n);
    QTextCursor      c(doc);
    c.setPosition(block.position() + block.length() - 1);
    // A plain format (the document's font): nothing of the message's link or colour carries over.
    QTextCharFormat separator;
    separator.setProperty(kRowSeparator, true);
    c.insertText(QString(QChar::LineSeparator), separator);
    QTextImageFormat image;
    image.setName(name);
    image.setWidth(size.width());
    image.setHeight(size.height());
    image.setProperty(kRowObject, true);
    image.setProperty(kRowKey, key);
    image.setVerticalAlignment(QTextCharFormat::AlignBottom); // on the baseline: the line is as tall as the row
    c.insertImage(image);
    return true;
}

bool resizeRow(QTextDocument* doc, int n, const QSizeF& size)
{
    if (!doc)
        return false;
    const Row row = rowOf(doc->findBlockByNumber(n));
    if (!row.exists())
        return false;
    QTextImageFormat image = row.format;
    if (qFuzzyCompare(image.width(), size.width()) && qFuzzyCompare(image.height(), size.height()))
        return false;
    image.setWidth(size.width());
    image.setHeight(size.height());
    QTextCursor c(doc);
    c.setPosition(row.object);
    c.setPosition(row.object + 1, QTextCursor::KeepAnchor);
    c.setCharFormat(image);
    return true;
}

bool removeRow(QTextDocument* doc, int n)
{
    if (!doc)
        return false;
    const QTextBlock b = doc->findBlockByNumber(n);
    if (!b.isValid())
        return false;
    QVector<QPair<int, int>> spans; // position, length
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid() && isOurs(f))
            spans.append({f.position(), f.length()});
    }
    if (spans.isEmpty())
        return false;
    QTextCursor c(doc);
    for (int i = spans.size() - 1; i >= 0; --i) {
        c.setPosition(spans.at(i).first);
        c.setPosition(spans.at(i).first + spans.at(i).second, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    scrubCopied(doc, n + 1); // the next line no longer comes after our picture
    return true;
}

bool scrubCopied(QTextDocument* doc, int n)
{
    const QTextBlock b = doc ? doc->findBlockByNumber(n) : QTextBlock();
    if (!b.isValid())
        return false;
    QVector<QPair<int, int>> spans; // TeamSpeak's text with a row's format
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid() && carriesRow(f.charFormat()) && !isOurs(f))
            spans.append({f.position(), f.length()});
    }
    const bool own = carriesRow(b.charFormat());
    if (spans.isEmpty() && !own)
        return false;
    // What TeamSpeak copied without the row: the message's last character before our line break, as
    // TeamSpeak made it (a link the layout recoloured, an HD emoji: what they keep of it).
    QTextCharFormat was;
    const QTextBlock before = b.previous();
    if (before.isValid()) {
        const Row row = rowOf(before);
        const int end = row.separator >= 0 ? row.separator : row.exists() ? row.object : before.position() + before.length() - 1;
        if (end > before.position())
            was = formatAt(doc, end - 1);
    }
    if (emojiformat::isHd(was))
        was = emojiformat::originalFormat(was);
    if (was.hasProperty(layoutformat::kOrigChar))
        was = qvariant_cast<QTextFormat>(was.property(layoutformat::kOrigChar)).toCharFormat();
    if (carriesRow(was))
        was = QTextCharFormat();
    QTextCursor c(doc);
    for (const auto& span : qAsConst(spans)) {
        c.setPosition(span.first);
        c.setPosition(span.first + span.second, QTextCursor::KeepAnchor);
        c.setCharFormat(was);
    }
    if (own)
        QTextCursor(doc->findBlockByNumber(n)).setBlockCharFormat(was);
    return true;
}

int removeAll(QTextDocument* doc, QStringList* names)
{
    if (!doc)
        return 0;
    int count = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const Row row = rowOf(b);
        if (row.separator < 0 && !row.exists()) {
            scrubCopied(doc, b.blockNumber()); // a copy of a row that went some other way
            continue;
        }
        if (row.exists() && names)
            names->append(row.format.name());
        if (removeRow(doc, b.blockNumber()))
            ++count;
    }
    return count;
}

} // namespace textreactdoc
