#include "linkpreview/linkdoc.h"

#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>

#include <algorithm>

#include "emojiformat.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "textreactdoc.h"

namespace linkdoc {

namespace {

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
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

bool isObjectAt(QTextDocument* doc, int position)
{
    return position >= 0 && position < doc->characterCount() - 1 && doc->characterAt(position) == QChar::ObjectReplacementCharacter
           && isSlotObject(formatAt(doc, position));
}

bool isSeparatorAt(QTextDocument* doc, int position)
{
    return position >= 0 && position < doc->characterCount() - 1 && doc->characterAt(position) == QChar::LineSeparator
           && isSlotSeparator(formatAt(doc, position));
}

QString cleanHref(QString href)
{
    href.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return href.trimmed();
}

bool isFileHref(const QString& href)
{
    return href.contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

// The plugin's own addresses that are links in chats: the update hint's and the poll marker's.
bool isPluginHref(const QString& href)
{
    return href.contains(QLatin1String("#tsmupd-")) || href.contains(QLatin1String("#tsmpoll"));
}

bool sameAddress(const QString& a, const QString& b)
{
    const QUrl ua(a);
    const QUrl ub(b);
    if (ua.isValid() && ub.isValid())
        return ua.adjusted(QUrl::StripTrailingSlash | QUrl::NormalizePathSegments) == ub.adjusted(QUrl::StripTrailingSlash | QUrl::NormalizePathSegments);
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// Text reactions' row pictures and line breaks (textreactdoc.h): at a message's end after a slot of ours.
bool isRowChar(const QTextCharFormat& format)
{
    return textreactdoc::isRowObject(format) || textreactdoc::isRowSeparator(format);
}

} // namespace

QString cardPrefix()
{
    return QString::fromLatin1("tsmedia-link:");
}

QString mediaPrefix()
{
    return QString::fromLatin1("tsmedia:");
}

bool isSlotObject(const QTextCharFormat& format)
{
    if (!format.isImageFormat() || !format.boolProperty(kSlotObject))
        return false;
    const QString name = format.toImageFormat().name();
    return name.startsWith(cardPrefix()) || name.startsWith(mediaPrefix());
}

bool isSlotSeparator(const QTextCharFormat& format)
{
    return format.boolProperty(kSlotSeparator) && !format.isImageFormat();
}

bool carriesSlot(const QTextFormat& format)
{
    return format.hasProperty(kSlotObject) || format.hasProperty(kSlotUrl) || format.hasProperty(kSlotSeparator) || format.hasProperty(kSlotHref);
}

// ---- reading --------------------------------------------------------------------------------------------

QVector<Link> linksIn(const QTextBlock& block, const QStringList& excluded)
{
    QVector<Link> out;
    if (!block.isValid() || !block.isVisible())
        return out;
    // A cheap look first: most lines have no web link.
    bool any = false;
    for (auto it = block.begin(); !it.atEnd() && !any; ++it) {
        const QTextFragment f = it.fragment();
        any = f.isValid() && f.charFormat().isAnchor() && f.charFormat().anchorHref().startsWith(QLatin1String("http"), Qt::CaseInsensitive);
    }
    if (!any)
        return out;
    const layoutdoc::Row row = layoutdoc::read(block);
    if (row.type != layoutdoc::RowType::Message)
        return out; // system rows, TS Media's own lines, day lines
    QSet<QString> seen;
    QString       lastHref;
    int           lastEnd = -1;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (!cf.isAnchor() || cf.isImageFormat()) {
            lastHref.clear(); // something else between two pieces of one address: two links
            continue;
        }
        const QString href  = cleanHref(cf.anchorHref());
        const int     start = f.position();
        const int     end   = start + f.length();
        if (isFileHref(href))
            break; // a TS Media file link: what follows in this message is its note (and its link)
        if (href == lastHref && start == lastEnd && !out.isEmpty() && out.last().end == start) {
            out.last().end = end; // one address over several fragments
            lastEnd        = end;
            continue;
        }
        lastHref = href;
        lastEnd  = end;
        // Web links only (classify() takes http for YouTube alone, https for everything else).
        if (href.size() > kMaxHrefChars || !href.startsWith(QLatin1String("http"), Qt::CaseInsensitive) || isPluginHref(href))
            continue;
        bool skip = false;
        for (const QString& x : excluded)
            skip = skip || (!x.isEmpty() && sameAddress(href, x));
        if (skip)
            continue;
        const linkpreview::Target target = linkpreview::classify(href);
        if (target.kind == linkpreview::Kind::None || target.key.isEmpty() || seen.contains(target.key))
            continue;
        if (out.size() >= kMaxPerMessage)
            break;
        seen.insert(target.key);
        Link link;
        link.block    = block.blockNumber();
        link.start    = start;
        link.end      = end;
        link.href     = href;
        link.target   = target;
        link.outgoing = row.outgoing;
        out.append(link);
    }
    return out;
}

QVector<Link> findLinks(QTextDocument* doc, const QStringList& excluded, int maxBlocks)
{
    QVector<Link> out;
    if (!doc)
        return out;
    const int first = qMax(0, doc->blockCount() - qMax(1, maxBlocks));
    for (QTextBlock b = doc->findBlockByNumber(first); b.isValid(); b = b.next())
        out += linksIn(b, excluded);
    return out;
}

QVector<Slot> slotsIn(const QTextBlock& block)
{
    QVector<Slot> out;
    if (!block.isValid())
        return out;
    QTextDocument* doc = const_cast<QTextDocument*>(block.document());
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (!isSlotObject(cf) || !only(f.text(), QChar::ObjectReplacementCharacter))
            continue;
        const QTextImageFormat image = cf.toImageFormat();
        for (int i = 0; i < f.length(); ++i) {
            Slot s;
            s.object    = f.position() + i;
            s.separator = isSeparatorAt(doc, s.object - 1) ? s.object - 1 : -1;
            s.after     = isSeparatorAt(doc, s.object + 1) && s.object + 1 < block.position() + block.length() - 1 ? s.object + 1 : -1;
            s.name      = image.name();
            s.urlKey    = cf.stringProperty(kSlotUrl);
            s.href      = cf.stringProperty(kSlotHref);
            s.size      = QSizeF(image.width(), image.height());
            out.append(s);
        }
    }
    return out;
}

QVector<Slot> allSlots(QTextDocument* doc)
{
    QVector<Slot> out;
    if (!doc)
        return out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        out += slotsIn(b);
    return out;
}

Slot slotAfter(const QTextBlock& block, const Link& link)
{
    for (const Slot& s : slotsIn(block)) {
        if (s.separator == link.end && s.urlKey == link.target.key)
            return s;
    }
    return Slot();
}

// ---- edits ------------------------------------------------------------------------------------------------

bool insertSlot(QTextDocument* doc, const Link& link, const QString& name, const QSizeF& size)
{
    if (!doc || link.start < 0 || link.end <= link.start || link.end >= doc->characterCount())
        return false;
    const QTextBlock block = doc->findBlock(link.start);
    if (!block.isValid() || block.blockNumber() != link.block || link.end > block.position() + block.length() - 1)
        return false;
    // The link is still there, whole, and nothing of ours follows it yet.
    const QTextCharFormat first = formatAt(doc, link.start);
    const QTextCharFormat last  = formatAt(doc, link.end - 1);
    if (!first.isAnchor() || !last.isAnchor() || cleanHref(first.anchorHref()) != link.href || cleanHref(last.anchorHref()) != link.href)
        return false;
    if (isSeparatorAt(doc, link.end) || isObjectAt(doc, link.end))
        return false;
    QTextCursor c(doc);
    c.setPosition(link.end);
    QTextCharFormat separator; // a plain format (the document's font): nothing of the link carries over
    separator.setProperty(kSlotSeparator, true);
    c.insertText(QString(QChar::LineSeparator), separator);
    QTextImageFormat image;
    image.setName(name);
    image.setWidth(size.width());
    image.setHeight(size.height());
    image.setProperty(kSlotObject, true);
    image.setProperty(kSlotUrl, link.target.key);
    image.setProperty(kSlotHref, link.href);
    c.insertImage(image);
    // Text going on in that line starts on a line of its own below the picture.
    const int  next     = c.position();
    const bool blockEnd = next >= block.position() + block.length() - 1;
    if (!blockEnd) {
        const QChar ch = doc->characterAt(next);
        if (ch != QChar::LineSeparator && ch != QChar::ParagraphSeparator)
            c.insertText(QString(QChar::LineSeparator), separator);
    }
    return true;
}

bool replaceObject(QTextDocument* doc, int object, const QString& name, const QSizeF& size)
{
    if (!doc || !isObjectAt(doc, object))
        return false;
    QTextImageFormat image = formatAt(doc, object).toImageFormat();
    if (image.name() == name && qFuzzyCompare(image.width(), size.width()) && qFuzzyCompare(image.height(), size.height()))
        return false;
    image.setName(name);
    image.setWidth(size.width());
    image.setHeight(size.height());
    QTextCursor c(doc);
    c.setPosition(object);
    c.setPosition(object + 1, QTextCursor::KeepAnchor);
    c.setCharFormat(image);
    return true;
}

bool resizeObject(QTextDocument* doc, int object, const QSizeF& size)
{
    if (!doc || !isObjectAt(doc, object))
        return false;
    const QTextImageFormat image = formatAt(doc, object).toImageFormat();
    return replaceObject(doc, object, image.name(), size);
}

bool removeSlot(QTextDocument* doc, int object)
{
    if (!doc || !isObjectAt(doc, object))
        return false;
    const QTextBlock block = doc->findBlock(object);
    const int        from  = isSeparatorAt(doc, object - 1) ? object - 1 : object;
    const int        to    = isSeparatorAt(doc, object + 1) && object + 1 < block.position() + block.length() - 1 ? object + 2 : object + 1;
    QTextCursor      c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    scrubCopied(doc, block.blockNumber() + 1); // the next line no longer comes after our picture
    return true;
}

bool scrubCopied(QTextDocument* doc, int n)
{
    const QTextBlock b = doc ? doc->findBlockByNumber(n) : QTextBlock();
    if (!b.isValid())
        return false;
    QVector<QPair<int, int>> spans; // TeamSpeak's text with our format
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f   = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf   = f.charFormat();
        const bool            ours = (isSlotObject(cf) && only(f.text(), QChar::ObjectReplacementCharacter))
                          || (isSlotSeparator(cf) && only(f.text(), QChar::LineSeparator));
        if (carriesSlot(cf) && !ours)
            spans.append({f.position(), f.length()});
    }
    const bool own = carriesSlot(b.charFormat());
    if (spans.isEmpty() && !own)
        return false;
    // What TeamSpeak copied without us: the message's last character that isn't ours (nor a reaction row's).
    QTextCharFormat  was;
    const QTextBlock before = b.previous();
    if (before.isValid()) {
        int p = before.position() + before.length() - 2;
        while (p >= before.position()) {
            const QTextCharFormat f  = formatAt(doc, p);
            const QChar           ch = doc->characterAt(p);
            const bool ours = (isSlotObject(f) && ch == QChar::ObjectReplacementCharacter) || (isSlotSeparator(f) && ch == QChar::LineSeparator) || isRowChar(f);
            if (!ours)
                break;
            --p;
        }
        if (p >= before.position())
            was = formatAt(doc, p);
    }
    if (emojiformat::isHd(was))
        was = emojiformat::originalFormat(was);
    if (was.hasProperty(layoutformat::kOrigChar))
        was = qvariant_cast<QTextFormat>(was.property(layoutformat::kOrigChar)).toCharFormat();
    if (carriesSlot(was))
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
        const QVector<Slot> found = slotsIn(b);
        for (int i = found.size() - 1; i >= 0; --i) {
            if (names)
                names->append(found.at(i).name);
            if (removeSlot(doc, found.at(i).object))
                ++count;
        }
        if (found.isEmpty())
            scrubCopied(doc, b.blockNumber()); // a copy of a slot that went some other way
    }
    return count;
}

} // namespace linkdoc
