#include "replydoc.h"

#include <QPair>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QVariantList>

#include "albums.h"      // uidFromClientHref
#include "emojiformat.h" // 2.2 emoji: HD pictures read as what they stand for

namespace replydoc {

namespace {

constexpr int kMaxFragments = 4000; // per block; a message has far fewer
constexpr int kMaxLeadChars = 48;   // icon, time and spaces in front of the nickname

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

bool isObject(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.hasProperty(kObjectProperty) && format.toImageFormat().name().startsWith(objectPrefix());
}

bool isSeparator(QTextDocument* doc, int position)
{
    return position >= 0 && position < doc->characterCount() && doc->characterAt(position) == QChar::LineSeparator
           && formatAt(doc, position).boolProperty(kSeparatorProperty);
}

bool isClientLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().startsWith(QLatin1String("client://"), Qt::CaseInsensitive);
}

bool isFileLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

QString nickFrom(const QString& quoted)
{
    QString nick = replies::visibleText(quoted).trimmed(); // no bidi marks: names are drawn and compared
    if (nick.size() >= 2 && nick.startsWith(QLatin1Char('"')) && nick.endsWith(QLatin1Char('"')))
        nick = nick.mid(1, nick.size() - 2);
    if (nick.isEmpty() || nick.size() > replies::kMaxNickChars)
        return {};
    for (const QChar c : nick) {
        if (c == QLatin1Char('\n') || c == QChar::LineSeparator || c == QChar::ParagraphSeparator || c == QChar::ObjectReplacementCharacter)
            return {};
    }
    return nick;
}

// "21:14:02", "9:14 PM" -> minutes of the day; -1 if it isn't a time.
int minutesOf(const QString& time)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{1,2}):(\\d{2})(?::\\d{2})?\\s*([AaPp][Mm])?\\s*$"));
    const QRegularExpressionMatch   m = re.match(time);
    if (!m.hasMatch())
        return -1;
    int       hours   = m.captured(1).toInt();
    const int minutes = m.captured(2).toInt();
    if (minutes > 59)
        return -1;
    if (!m.captured(3).isEmpty()) {
        if (hours < 1 || hours > 12)
            return -1;
        const bool pm = m.captured(3).startsWith(QLatin1Char('p'), Qt::CaseInsensitive);
        hours         = hours % 12 + (pm ? 12 : 0);
    } else if (hours > 23) {
        return -1;
    }
    return hours * 60 + minutes;
}

int clientIdOf(const QString& href)
{
    static const QRegularExpression re(QStringLiteral("^client://(\\d{1,5})/"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch   m = re.match(href);
    const int                       id = m.hasMatch() ? m.captured(1).toInt() : 0;
    return id > 0 && id <= 0xFFFF ? id : 0;
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
    format.clearProperty(kSeparatorProperty);
    format.clearProperty(kObjectProperty);
    format.clearProperty(kRunsProperty);
    format.clearProperty(kOffsetProperty);
    return format;
}

replies::Quote quoteFromRuns(const QVariantList& stored)
{
    // What was taken out ends with the quote line's line break: the line is what comes before it.
    QVector<replies::Run> runs;
    for (int i = 0; i + 1 < stored.size() && runs.size() < 256; i += 2) {
        const QTextFormat format = qvariant_cast<QTextFormat>(stored.at(i + 1));
        QString           text   = stored.at(i).toString();
        const int         end    = text.indexOf(QChar::LineSeparator);
        if (end >= 0)
            text.truncate(end);
        if (format.isImageFormat())
            runs.append({textOf(text, format.toCharFormat()), QString()});
        else if (!text.isEmpty())
            runs.append({text, format.toCharFormat().isAnchor() ? format.toCharFormat().anchorHref() : QString()});
        if (end >= 0)
            break;
    }
    return replies::parseQuote(runs);
}

// 2.2 emoji: a piece of the quote line as TeamSpeak showed it. An HD emoji goes back to its text (or to
// TeamSpeak's emoticon picture it replaced), so the stored line never holds a picture of ChatEmoji's,
// whose resources it drops when it goes.
QPair<QString, QTextFormat> asReceived(const QString& text, const QTextCharFormat& format)
{
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

} // namespace

QString objectPrefix()
{
    return QString::fromLatin1("tsmedia-reply:");
}

QString textOf(const QString& text, const QTextCharFormat& format)
{
    if (emojiformat::isHd(format)) {
        const QString emoji = emojiformat::originalText(format);
        return (emoji.isEmpty() ? emoticonText(emojiformat::emoticonName(format)) : emoji).repeated(text.size());
    }
    if (format.isImageFormat())
        return emoticonText(format.toImageFormat().name()).repeated(qMax(1, static_cast<int>(text.count(QChar::ObjectReplacementCharacter))));
    return text;
}

QString emoticonText(const QString& imageName)
{
    if (!imageName.contains(QLatin1String("emoticon"), Qt::CaseInsensitive))
        return {};
    QString base = imageName.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1).section(QLatin1Char(':'), -1);
    base         = base.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('.'), 0, 0).toLower();
    // TeamSpeak's own sets (gfx/default*.zip, emoticons/emoticons.txt): the first text of each picture.
    static const struct {
        const char* name;
        const char* text;
    } table[] = {{"smile", ":)"},   {"laugh", ":D"},     {"cool", "8)"},     {"twinkle", ";)"},  {"sad", ":("},
                 {"angry", ":C"},   {"scream", ":0"},    {"skeptical", ":/"}, {"stunned", ":x"}, {"tongue", ":P"}};
    for (const auto& entry : table) {
        if (base == QLatin1String(entry.name))
            return QString::fromLatin1(entry.text);
    }
    return {};
}

Message parseBlock(const QTextBlock& block)
{
    Message m;
    if (!block.isValid())
        return m;
    const QVector<Piece> pieces = piecesOf(block);
    const int            bpos   = block.position();
    const QString        text   = block.text();
    int                  offset = 0; // characters of ours in front of TeamSpeak's header

    if (!pieces.isEmpty() && pieces.first().start == bpos && isObject(pieces.first().format)) {
        const QTextImageFormat image = pieces.first().format.toImageFormat();
        m.restyled                   = true;
        m.object                     = image.name();
        m.objectSize                 = QSizeF(image.width(), image.height());
        m.quote                      = quoteFromRuns(image.property(kRunsProperty).toList());
        m.hasQuote                   = m.quote.valid();
        offset                       = 1;
        for (const Piece& p : pieces) {
            if (p.start <= bpos + 1 && p.end > bpos + 1) {
                if (text.size() > 1 && text.at(1) == QChar::LineSeparator && p.format.boolProperty(kSeparatorProperty))
                    offset = 2;
                break;
            }
        }
    }

    // ---- TeamSpeak's header: [icon]<time> "Nick": --------------------------------------------------
    int             nickStart = -1;
    int             nickEnd   = -1;
    QString         nickHref;
    QTextCharFormat nickFormat;
    for (const Piece& p : pieces) {
        if (p.end <= bpos + offset)
            continue;
        if (isClientLink(p.format)) {
            if (nickStart < 0) {
                nickStart  = p.start;
                nickEnd    = p.end;
                nickHref   = p.format.anchorHref();
                nickFormat = p.format;
            } else if (p.format.anchorHref() == nickHref && p.start == nickEnd) {
                nickEnd = p.end;
            } else {
                break;
            }
            continue;
        }
        if (nickStart >= 0 || p.start - bpos > offset + kMaxLeadChars)
            break;
    }

    // The quotes are part of the link (S0); a header with them outside it is read too.
    static const QRegularExpression lead(QStringLiteral("^\\x{FFFC}{0,2}[ \\t]*(?:<([^<>\\n\\x{2028}\\x{2029}]{1,24})>)?[ \\t]*\"?$"));
    static const QRegularExpression textual(
        QStringLiteral("^\\x{FFFC}{0,2}[ \\t]*(?:<([^<>\\n\\x{2028}\\x{2029}]{1,24})>)?[ \\t]*\"((?:(?!\"[ \\t]*:)[^\\n\\x{2028}\\x{2029}]){1,64})\"[ \\t]*:[ \\t]?"));
    int     bodyStart = -1;
    QString timeText;
    if (nickStart >= 0) {
        const QRegularExpressionMatch before = lead.match(text.mid(offset, nickStart - bpos - offset));
        const QString                 nick   = nickFrom(text.mid(nickStart - bpos, nickEnd - nickStart));
        QString                       after  = text.mid(nickEnd - bpos, 3);
        int                           skip   = 0;
        if (after.startsWith(QLatin1Char('"'))) {
            after.remove(0, 1);
            skip = 1;
        }
        if (before.hasMatch() && !nick.isEmpty() && after.startsWith(QLatin1Char(':'))) {
            m.nick       = nick;
            m.uid        = albums::uidFromClientHref(nickHref);
            m.clientId   = m.uid.isEmpty() ? 0 : clientIdOf(nickHref);
            timeText     = before.captured(1);
            bodyStart    = nickEnd + skip + (after.size() > 1 && after.at(1) == QLatin1Char(' ') ? 2 : 1);
            const QBrush brush = nickFormat.foreground();
            if (brush.style() != Qt::NoBrush)
                m.nickColor = brush.color();
        }
    }
    if (bodyStart < 0) {
        // No nickname link (an unknown header format): `"Nick": ` after the optional icon and time.
        const QRegularExpressionMatch match = textual.match(text.mid(offset, 200));
        if (!match.hasMatch())
            return m; // not a chat message
        m.nick = nickFrom(QLatin1Char('"') + match.captured(2) + QLatin1Char('"'));
        if (m.nick.isEmpty())
            return m;
        timeText  = match.captured(1);
        bodyStart = bpos + offset + match.capturedLength();
    }
    m.block    = block.blockNumber();
    m.position = bpos;
    m.visible  = block.isVisible();
    m.minutes  = minutesOf(timeText);
    for (const Piece& p : pieces) {
        if (p.start >= bpos + offset && !p.format.isImageFormat()) {
            m.baseFormat = plainCopy(p.format);
            break;
        }
    }
    if (const QTextLayout* layout = block.layout()) {
        const int line = m.restyled ? 1 : 0;
        if (layout->lineCount() > line)
            m.lineHeight = layout->lineAt(line).height();
    }

    // ---- a raw quote line ------------------------------------------------------------------------------
    m.textStart = bodyStart;
    if (!m.restyled) {
        int at = bodyStart - bpos;
        while (at < text.size() && (text.at(at) == QLatin1Char(' ') || text.at(at) == QChar::Nbsp))
            ++at;
        if (at < text.size() && text.at(at) == replies::arrow().at(0)) {
            const int lineBreak = text.indexOf(QChar::LineSeparator, bodyStart - bpos);
            const int lineEnd   = lineBreak < 0 ? text.size() : lineBreak;
            if (lineEnd - (bodyStart - bpos) <= replies::kMaxQuoteLine) {
                QVector<replies::Run> runs;
                bool                  foreignLink = false;
                for (const Piece& p : pieces) {
                    const int from = qMax(p.start, bodyStart);
                    const int to   = qMin(p.end, bpos + lineEnd);
                    if (from >= to)
                        continue;
                    if (p.format.isImageFormat()) {
                        runs.append({textOf(p.text.mid(from - p.start, to - from), p.format), QString()});
                        continue;
                    }
                    if (p.format.isAnchor() && !isClientLink(p.format))
                        foreignLink = true; // a quote line of ours has no other links (URLs became "(link)")
                    runs.append({p.text.mid(from - p.start, to - from), p.format.isAnchor() ? p.format.anchorHref() : QString()});
                }
                const replies::Quote quote = foreignLink ? replies::Quote() : replies::parseQuote(runs);
                if (quote.valid()) {
                    m.hasQuote   = true;
                    m.quote      = quote;
                    m.quoteStart = bodyStart;
                    m.quoteEnd   = bpos + (lineBreak < 0 ? lineEnd : lineEnd + 1);
                    m.textStart  = m.quoteEnd;
                }
            }
        }
    }

    // ---- its text, up to the first file link (a TS Media link's note comes after it) -------------------
    QString body;
    for (int i = 0; i < pieces.size(); ++i) {
        const Piece& p = pieces.at(i);
        if (p.end <= m.textStart)
            continue;
        if (p.format.isImageFormat()) {
            body += textOf(p.text, p.format); // emoticons and HD emoji as their text
            continue;
        }
        if (isFileLink(p.format)) {
            QString label = p.text;
            for (int j = i + 1; j < pieces.size() && pieces.at(j).start == pieces.at(j - 1).end && pieces.at(j).format.anchorHref() == p.format.anchorHref(); ++j)
                label += pieces.at(j).text;
            label.remove(QChar(0x2060));
            m.mediaLabel = replies::leftChars(label.trimmed(), replies::kMaxNickChars * 4);
            break;
        }
        QString part = p.text.mid(qMax(0, m.textStart - p.start));
        part.replace(QChar::LineSeparator, QLatin1Char(' '));
        part.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
        part.remove(QChar(0x2060));
        part.remove(QChar::ObjectReplacementCharacter);
        body += part;
        if (body.size() > replies::kMaxBodyChars)
            break;
    }
    m.text = replies::leftChars(body, replies::kMaxBodyChars).trimmed();
    return m;
}

QVector<Message> scan(QTextDocument* doc, int maxBlocks)
{
    if (!doc)
        return {};
    return scanFrom(doc, qMax(0, doc->blockCount() - qMax(1, maxBlocks)));
}

QVector<Message> scanFrom(QTextDocument* doc, int firstBlock)
{
    QVector<Message> out;
    if (!doc)
        return out;
    for (QTextBlock b = doc->findBlockByNumber(qMax(0, firstBlock)); b.isValid(); b = b.next()) {
        Message m = parseBlock(b);
        if (m.block >= 0)
            out.append(m);
    }
    return out;
}

bool collapse(QTextDocument* doc, const Message& m, const QString& name, const QSizeF& size)
{
    if (!doc || m.restyled || !m.hasQuote || m.quoteStart < 0 || m.quoteEnd <= m.quoteStart)
        return false;
    const QTextBlock block = doc->findBlockByNumber(m.block);
    if (!block.isValid() || block.position() != m.position || m.quoteEnd > block.position() + block.length() - 1)
        return false;

    QVariantList runs;
    for (const Piece& p : piecesOf(block)) {
        const int from = qMax(p.start, m.quoteStart);
        const int to   = qMin(p.end, m.quoteEnd);
        if (from >= to)
            continue;
        const QPair<QString, QTextFormat> received = asReceived(p.text.mid(from - p.start, to - from), p.format);
        runs << received.first;
        runs << static_cast<QVariant>(received.second);
    }
    if (runs.isEmpty())
        return false;

    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(m.quoteStart);
    c.setPosition(m.quoteEnd, QTextCursor::KeepAnchor);
    c.removeSelectedText();

    // The picture sits on the line's baseline; the line break after it (in the header's font) adds the
    // font's descent below, so a picture of the header line's height minus that descent gives the reply
    // line exactly the height of a message line.
    QTextImageFormat image;
    image.setFont(m.baseFormat.font(), QTextCharFormat::FontPropertiesSpecifiedOnly);
    image.setName(name);
    image.setWidth(size.width());
    image.setHeight(size.height());
    image.setProperty(kObjectProperty, 1);
    image.setProperty(kRunsProperty, runs);
    image.setProperty(kOffsetProperty, m.quoteStart - m.position);
    c.setPosition(m.position);
    c.insertImage(image);
    QTextCharFormat separator = m.baseFormat;
    separator.setProperty(kSeparatorProperty, true);
    c.insertText(QString(QChar::LineSeparator), separator);
    c.endEditBlock();
    return true;
}

bool restore(QTextDocument* doc, int position)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1)
        return false;
    const QTextCharFormat format = formatAt(doc, position);
    if (!isObject(format) || doc->characterAt(position) != QChar::ObjectReplacementCharacter)
        return false;
    const QVariantList runs   = format.property(kRunsProperty).toList();
    const int          offset = format.intProperty(kOffsetProperty);
    const bool         ours   = isSeparator(doc, position + 1);

    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(position);
    c.setPosition(position + (ours ? 2 : 1), QTextCursor::KeepAnchor);
    c.removeSelectedText();
    const QTextBlock block = doc->findBlock(position);
    const int        at    = block.position() + qBound(0, offset, qMax(0, block.length() - 1));
    c.setPosition(at);
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QTextFormat f = qvariant_cast<QTextFormat>(runs.at(i + 1));
        if (f.isImageFormat()) {
            // One picture per character (two smileys in a row are one piece).
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
    // One edit for all of them: the chat is laid out again once, not once per reply line (1000 reply
    // lines: 1.4 s one by one, 0.1 s together), and TeamSpeak's view gets one contentsChange.
    QTextCursor batch(doc);
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
    if (!orphans.isEmpty()) {
        QTextCursor c(doc);
        c.beginEditBlock();
        for (int i = orphans.size() - 1; i >= 0; --i) {
            c.setPosition(orphans.at(i));
            c.setPosition(orphans.at(i) + 1, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        c.endEditBlock();
        done += orphans.size();
    }
    batch.endEditBlock();
    return done;
}

void resizeObject(QTextDocument* doc, int position, const QSizeF& size)
{
    if (!doc || position < 0 || position >= doc->characterCount() - 1)
        return;
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    QTextCharFormat format = c.charFormat();
    if (!isObject(format))
        return;
    QTextImageFormat image = format.toImageFormat();
    if (qFuzzyCompare(image.width(), size.width()) && qFuzzyCompare(image.height(), size.height()))
        return;
    image.setWidth(size.width());
    image.setHeight(size.height());
    c.setCharFormat(image);
}

} // namespace replydoc
