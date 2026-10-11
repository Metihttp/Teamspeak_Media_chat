#include "polldoc.h"

#include <QPair>
#include <QPixmap>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>
#include <QVariantList>

#include "emojiformat.h"  // HD emoji kept as what they stand for
#include "i18n.h"
#include "layoutformat.h" // TS Media's chat layout: whether the card needs a line break in front
#include "layoutdoc.h"    // 2.3: the layout's recolouring left out of what the card keeps
#include "replydoc.h"    // the header (TeamSpeak's or TS Media's layout) and the text of pictures

namespace polldoc {

namespace {

constexpr int kMaxFragments = 4000;

struct Piece {
    int             start = 0;
    int             end   = 0;
    QString         text;
    QTextCharFormat format;
};

QVector<Piece> piecesOf(const QTextBlock& block)
{
    QVector<Piece> out;
    for (auto it = block.begin(); !it.atEnd() && out.size() < kMaxFragments; ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid())
            out.append({f.position(), f.position() + f.length(), f.text(), f.charFormat()});
    }
    return out;
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

bool isSeparator(QTextDocument* doc, int position)
{
    return position >= 0 && position < doc->characterCount() - 1 && doc->characterAt(position) == QChar::LineSeparator
           && formatAt(doc, position).boolProperty(kSeparatorProperty);
}

// A piece as TeamSpeak showed it: an HD emoji goes back to its text (or the emoticon picture it replaced),
// so what we keep never holds a picture of ChatEmoji's (whose resources go with it). 2.3: and without TS
// Media's layout's recolouring (it styles a new message before the card is made, and is given back before
// the cards on unload): what comes back is TeamSpeak's format.
QPair<QString, QTextFormat> asReceived(const QString& text, const QTextCharFormat& styled)
{
    const QTextCharFormat format = layoutdoc::originalCharFormat(styled);
    if (!emojiformat::isHd(format))
        return {text, format};
    const QString emoji = emojiformat::originalText(format);
    if (!emoji.isEmpty())
        return {emoji.repeated(text.size()), emojiformat::originalFormat(format)};
    QTextCharFormat original = emojiformat::originalFormat(format);
    if (!original.isImageFormat()) {
        QTextImageFormat image;
        image.setName(emojiformat::emoticonName(format));
        original = image;
    }
    return {text, original};
}

QTextCharFormat plainCopy(QTextCharFormat format);

// Our line break: a 1 px font (a point size would win over the pixel size: it goes).
QTextCharFormat separatorFormat(const QTextCharFormat& base)
{
    QTextCharFormat separator = base;
    separator.clearProperty(QTextFormat::FontPointSize);
    separator.clearProperty(QTextFormat::FontSizeAdjustment);
    separator.setProperty(QTextFormat::FontPixelSize, 1);
    separator.setProperty(kSeparatorProperty, true);
    return separator;
}

// Whether the card needs our line break in front to start on a line of its own: after TeamSpeak's header
// and after a Compact head (both on the card's line otherwise), never in a Cozy head's body (it starts
// on its own line already; a line holding only a line break is a full text line high) or next to a
// continuation's gutter.
bool wantsSeparator(const QTextBlock& block)
{
    const layoutformat::Lead lead = layoutformat::leadOf(block);
    return !lead.styled || lead.kind == layoutformat::CompactHead;
}

QTextCharFormat plainCopy(QTextCharFormat format)
{
    format.setAnchor(false);
    format.clearProperty(QTextFormat::AnchorHref);
    format.clearProperty(QTextFormat::AnchorName);
    format.clearProperty(QTextFormat::ObjectType);
    format.clearProperty(QTextFormat::ImageName);
    format.clearProperty(QTextFormat::ImageWidth);
    format.clearProperty(QTextFormat::ImageHeight);
    format.clearProperty(QTextFormat::IsAnchor);
    format.clearProperty(kObjectProperty);
    format.clearProperty(kRunsProperty);
    format.clearProperty(kOffsetProperty);
    format.clearProperty(kPollProperty);
    return format;
}

} // namespace

QString objectPrefix()
{
    return QString::fromLatin1("tsmedia-poll:");
}

bool isObject(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.hasProperty(kObjectProperty) && format.toImageFormat().name().startsWith(objectPrefix());
}

QStringList pollToList(const polls::Poll& poll)
{
    QStringList list;
    list << poll.key << poll.question << QString::fromLatin1(poll.multiple ? "m" : "s") << QString::number(poll.channelId);
    list << poll.options;
    return list;
}

polls::Poll pollFromList(const QStringList& list)
{
    polls::Poll poll;
    if (list.size() < 4 + polls::kMinOptions || list.size() > 4 + polls::kMaxOptions)
        return poll;
    poll.key       = list.at(0);
    poll.question  = polls::cleanLine(list.at(1), 2 * polls::kMaxQuestionChars);
    poll.multiple  = list.at(2) == QLatin1String("m");
    poll.channelId = list.at(3).toULongLong();
    for (int i = 4; i < list.size(); ++i)
        poll.options << polls::cleanLine(list.at(i), 2 * polls::kMaxOptionChars);
    if (!poll.isValid())
        return polls::Poll();
    return poll;
}

QString summaryText(const QTextCharFormat& format)
{
    if (!isObject(format))
        return {};
    const polls::Poll poll = pollFromList(format.toImageFormat().property(kPollProperty).toStringList());
    if (!poll.isValid())
        return {};
    return QString::fromUtf8("\xF0\x9F\x93\x8A ") + i18n::t("Poll:") + QLatin1Char(' ') + poll.question;
}

QString copyText(const QTextCharFormat& format)
{
    if (!isObject(format))
        return {};
    const QVariantList runs = format.property(kRunsProperty).toList();
    QString            out;
    for (int i = 0; i + 1 < runs.size() && out.size() <= 16384; i += 2) {
        const QTextFormat f    = qvariant_cast<QTextFormat>(runs.at(i + 1));
        const QString     text = runs.at(i).toString();
        out += f.isImageFormat() ? replydoc::textOf(text, f.toCharFormat()) : text;
    }
    out.replace(QChar::LineSeparator, QLatin1Char('\n'));
    out.remove(QChar::ObjectReplacementCharacter);
    return out;
}

Found parseBlock(const QTextBlock& block)
{
    Found none;
    if (!block.isValid())
        return none;
    const QVector<Piece> pieces = piecesOf(block);
    // Already ours.
    for (const Piece& p : pieces) {
        if (!isObject(p.format))
            continue;
        const QTextImageFormat image = p.format.toImageFormat();
        Found                  f;
        f.poll = pollFromList(image.property(kPollProperty).toStringList());
        if (!f.poll.isValid())
            return none;
        f.block          = block.blockNumber();
        f.blockPosition  = block.position();
        f.collapsed      = true;
        f.objectPosition = p.start;
        f.object         = image.name();
        f.size           = QSizeF(image.width(), image.height());
        f.from           = p.start;
        f.to             = p.start + 1;
        return f;
    }
    // The marker link (several fragments when TeamSpeak split it).
    int           linkStart = -1;
    int           linkEnd   = -1;
    QString       linkHref;
    polls::Marker marker;
    for (const Piece& p : pieces) {
        if (linkStart >= 0) {
            if (p.start == linkEnd && p.format.isAnchor() && p.format.anchorHref() == linkHref) {
                linkEnd = p.end;
                continue;
            }
            break;
        }
        if (!p.format.isAnchor() || p.format.isImageFormat())
            continue;
        const polls::Marker m = polls::parseMarker(p.format.anchorHref());
        if (m.valid()) {
            marker    = m;
            linkStart = p.start;
            linkEnd   = p.end;
            linkHref  = p.format.anchorHref();
        }
    }
    if (linkStart < 0)
        return none;
    // Only in a chat message (its header: TeamSpeak's or TS Media's layout's), never in a status line.
    const replydoc::Message message = replydoc::parseBlock(block);
    if (message.block < 0 || message.restyled || message.textStart > linkStart)
        return none;
    QString text;
    for (const Piece& p : pieces) {
        const int from = qMax(p.start, message.textStart);
        const int to   = qMin(p.end, linkStart);
        if (from >= to)
            continue;
        const QString part = p.text.mid(from - p.start, to - from);
        text += p.format.isImageFormat() ? replydoc::textOf(part, p.format) : part;
        if (text.size() > 8192)
            return none;
    }
    const std::optional<polls::Poll> poll = polls::parseText(text, marker);
    if (!poll)
        return none;
    Found f;
    f.block         = block.blockNumber();
    f.blockPosition = block.position();
    f.from          = message.textStart;
    f.to            = linkEnd;
    f.poll          = *poll;
    return f;
}

QVector<Found> scan(QTextDocument* doc, int maxBlocks)
{
    QVector<Found> out;
    if (!doc)
        return out;
    const int first = qMax(0, doc->blockCount() - qMax(1, maxBlocks));
    for (QTextBlock b = doc->findBlockByNumber(first); b.isValid(); b = b.next()) {
        // Cheap test first: the marker's text is in the block.
        if (!b.text().contains(QChar::ObjectReplacementCharacter) && !b.text().contains(QLatin1String("TS Media")))
            continue;
        const Found f = parseBlock(b);
        if (f.block >= 0)
            out.append(f);
    }
    return out;
}

bool collapse(QTextDocument* doc, const Found& found, const QString& name, const QSizeF& size)
{
    if (!doc || found.collapsed || found.block < 0)
        return false;
    const QTextBlock block = doc->findBlockByNumber(found.block);
    if (!block.isValid() || block.position() != found.blockPosition)
        return false;
    const Found now = parseBlock(block);
    if (now.block < 0 || now.collapsed || now.from != found.from || now.to != found.to || now.poll.key != found.poll.key)
        return false;

    QVariantList runs;
    for (const Piece& p : piecesOf(block)) {
        const int from = qMax(p.start, found.from);
        const int to   = qMin(p.end, found.to);
        if (from >= to)
            continue;
        const QPair<QString, QTextFormat> received = asReceived(p.text.mid(from - p.start, to - from), p.format);
        runs << received.first;
        runs << static_cast<QVariant>(received.second);
    }
    if (runs.isEmpty())
        return false;
    const QTextCharFormat base = plainCopy(layoutdoc::originalCharFormat(formatAt(doc, found.from)));

    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(found.from);
    c.setPosition(found.to, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    c.setPosition(found.from);
    if (wantsSeparator(block))
        c.insertText(QString(QChar::LineSeparator), separatorFormat(base));
    QTextImageFormat image;
    image.setFont(base.font(), QTextCharFormat::FontPropertiesSpecifiedOnly);
    image.setName(name);
    image.setWidth(size.width());
    image.setHeight(size.height());
    image.setProperty(kObjectProperty, found.poll.key);
    image.setProperty(kRunsProperty, runs);
    image.setProperty(kOffsetProperty, found.from - found.blockPosition);
    image.setProperty(kPollProperty, pollToList(found.poll));
    c.insertImage(image);
    c.endEditBlock();
    return true;
}

bool normalize(QTextDocument* doc, int position)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1)
        return false;
    const QTextCharFormat format = formatAt(doc, position);
    if (!isObject(format) || doc->characterAt(position) != QChar::ObjectReplacementCharacter)
        return false;
    const QTextBlock block = doc->findBlock(position);
    const bool       has   = position - 1 >= block.position() && isSeparator(doc, position - 1);
    const bool       want  = wantsSeparator(block);
    if (has == want)
        return false;
    QTextCursor c(doc);
    if (has) {
        c.setPosition(position - 1);
        c.setPosition(position, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else {
        c.setPosition(position);
        c.insertText(QString(QChar::LineSeparator), separatorFormat(plainCopy(format)));
    }
    return true;
}

int normalizeAll(QTextDocument* doc)
{
    const QVector<int> objects = objectPositions(doc);
    if (objects.isEmpty())
        return 0;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    int done = 0;
    for (int i = objects.size() - 1; i >= 0; --i)
        done += normalize(doc, objects.at(i)) ? 1 : 0;
    batch.endEditBlock();
    return done;
}

bool restore(QTextDocument* doc, int position)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1)
        return false;
    const QTextCharFormat format = formatAt(doc, position);
    if (!isObject(format) || doc->characterAt(position) != QChar::ObjectReplacementCharacter)
        return false;
    const QVariantList runs = format.property(kRunsProperty).toList();
    const bool         ours = isSeparator(doc, position - 1);
    const int          at   = ours ? position - 1 : position;

    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(at);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    c.setPosition(at);
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QTextFormat f = qvariant_cast<QTextFormat>(runs.at(i + 1));
        if (f.isImageFormat()) {
            const int count = qBound(1, static_cast<int>(runs.at(i).toString().count(QChar::ObjectReplacementCharacter)), 64);
            for (int k = 0; k < count; ++k)
                c.insertImage(f.toImageFormat());
            continue;
        }
        c.insertText(runs.at(i).toString(), f.toCharFormat());
    }
    c.endEditBlock();
    return true;
}

QVector<int> objectPositions(QTextDocument* doc)
{
    QVector<int> positions;
    if (!doc)
        return positions;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !isObject(f.charFormat()))
                continue;
            for (int p = f.position(); p < f.position() + f.length(); ++p)
                positions.append(p);
        }
    }
    return positions;
}

int restoreAll(QTextDocument* doc)
{
    if (!doc)
        return 0;
    const QVector<int> objects = objectPositions(doc);
    QTextCursor        batch(doc);
    batch.beginEditBlock();
    int done = 0;
    for (int i = objects.size() - 1; i >= 0; --i)
        done += restore(doc, objects.at(i)) ? 1 : 0;
    // Line breaks of ours whose picture went some other way.
    QVector<int> orphans;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !f.charFormat().boolProperty(kSeparatorProperty))
                continue;
            for (int p = f.position(); p < f.position() + f.length(); ++p) {
                if (doc->characterAt(p) == QChar::LineSeparator)
                    orphans.append(p);
            }
        }
    }
    for (int i = orphans.size() - 1; i >= 0; --i) {
        QTextCursor c(doc);
        c.setPosition(orphans.at(i));
        c.setPosition(orphans.at(i) + 1, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        ++done;
    }
    batch.endEditBlock();
    return done;
}

int giveBack(QTextDocument* doc)
{
    if (!doc)
        return 0;
    QStringList names;
    for (const int p : objectPositions(doc))
        names << formatAt(doc, p).toImageFormat().name();
    const int done = restoreAll(doc);
    for (const QString& name : qAsConst(names))
        doc->addResource(QTextDocument::ImageResource, QUrl(name), QPixmap());
    return done;
}

void resizeObject(QTextDocument* doc, int position, const QSizeF& size)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1)
        return;
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    const QTextCharFormat format = c.charFormat();
    if (!isObject(format))
        return;
    QTextImageFormat image = format.toImageFormat();
    if (qFuzzyCompare(image.width(), size.width()) && qFuzzyCompare(image.height(), size.height()))
        return;
    image.setWidth(size.width());
    image.setHeight(size.height());
    c.setCharFormat(image);
}

} // namespace polldoc
