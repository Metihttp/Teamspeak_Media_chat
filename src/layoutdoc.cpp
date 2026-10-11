#include "layoutdoc.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextObject>
#include <QUrl>
#include <QVariantList>

#include "albums.h"      // uidFromClientHref
#include "emojiformat.h" // HD emoji: read and stored as TeamSpeak showed them
#include "i18n.h"
#include "uiutil.h"

namespace layoutdoc {

namespace {

using namespace layoutformat;

constexpr int kMaxFragments = 4000; // per block; a message has far fewer
constexpr int kMaxLeadChars = 48;   // icon, time and spaces in front of the nickname
constexpr int kMaxRuns      = 64;   // pieces of a header
constexpr int kJoinSeconds  = 7 * 60;

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

bool isClientLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().startsWith(QLatin1String("client://"), Qt::CaseInsensitive);
}

bool isFileLink(const QTextCharFormat& format)
{
    return format.isAnchor() && format.anchorHref().contains(QLatin1String("ts3file"), Qt::CaseInsensitive);
}

int clientIdOf(const QString& href)
{
    static const QRegularExpression re(QStringLiteral("^client://(\\d{1,5})/"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch   m  = re.match(href);
    const int                       id = m.hasMatch() ? m.captured(1).toInt() : 0;
    return id > 0 && id <= 0xFFFF ? id : 0;
}

// "<16:00:17>" at the start of text (TeamSpeak's header time, with or without seconds and AM/PM).
QString leadingTime(const QString& text, int* length)
{
    static const QRegularExpression re(QStringLiteral("^<(\\d{1,2}:\\d{2}(?::\\d{2})?(?:\\s?[AaPp][Mm])?)>"));
    const QRegularExpressionMatch   m = re.match(text);
    if (!m.hasMatch()) {
        *length = 0;
        return {};
    }
    *length = m.capturedLength();
    return m.captured(1);
}

// An HD emoji's piece as TeamSpeak showed it (its text, or TeamSpeak's emoticon picture it replaced), so
// what we keep never holds a picture of ChatEmoji's (whose resources it drops when it goes).
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

QVariantList takeRuns(const QTextBlock& block, int from, int to)
{
    QVariantList runs;
    for (const Piece& p : piecesOf(block)) {
        const int a = qMax(p.start, from);
        const int b = qMin(p.end, to);
        if (a >= b)
            continue;
        const QPair<QString, QTextFormat> received = asReceived(p.text.mid(a - p.start, b - a), p.format);
        runs << received.first;
        runs << QVariant::fromValue<QTextFormat>(received.second);
    }
    return runs;
}

void insertRuns(QTextCursor& c, const QVariantList& runs)
{
    QTextDocument* doc = c.document();
    for (int i = 0; i + 1 < runs.size() && i < 2 * kMaxRuns * 8; i += 2) {
        const QTextFormat f = qvariant_cast<QTextFormat>(runs.at(i + 1));
        if (f.isImageFormat()) {
            // One picture per character (two smileys in a row are one piece). TeamSpeak's HTML pictures sit in
            // a text object of their own (QTextCursor::insertImage with a position): one like it is made again.
            const int  count  = qBound(1, static_cast<int>(runs.at(i).toString().count(QChar::ObjectReplacementCharacter)), 64);
            const bool object = f.hasProperty(QTextFormat::ObjectIndex) && f.objectIndex() >= 0;
            QTextFrameFormat::Position position = QTextFrameFormat::InFlow;
            if (object) {
                if (QTextObject* o = doc ? doc->object(f.objectIndex()) : nullptr) {
                    if (o->format().isFrameFormat())
                        position = o->format().toFrameFormat().position();
                }
            }
            for (int k = 0; k < count; ++k) {
                if (object)
                    c.insertImage(f.toImageFormat(), position);
                else
                    c.insertImage(f.toImageFormat());
            }
            continue;
        }
        c.insertText(runs.at(i).toString(), f.toCharFormat());
    }
}

// ---- recolouring with a way back ------------------------------------------------------------------------
// kApplied: id, value, id, value, ... of what we set (an invalid value: we cleared it); kOrigChar: the
// format before. Restore puts back only what still has our value.

using Change = QPair<int, QVariant>;

bool sameValue(const QTextFormat& f, int id, const QVariant& value)
{
    if (!value.isValid())
        return !f.hasProperty(id);
    return f.hasProperty(id) && f.property(id) == value;
}

QTextCharFormat withChanges(QTextCharFormat format, const QVector<Change>& changes)
{
    QVariantList applied;
    const QTextCharFormat original = format;
    for (const Change& change : changes) {
        applied << change.first << change.second;
        if (change.second.isValid())
            format.setProperty(change.first, change.second);
        else
            format.clearProperty(change.first);
    }
    if (!format.hasProperty(kOrigChar))
        format.setProperty(kOrigChar, QVariant::fromValue<QTextFormat>(original));
    format.setProperty(kApplied, applied);
    return format;
}

// The fragment's format with our changes taken back (inside an HD emoji's kept format too). False if it
// had none.
template <typename F>
bool revertFormat(F& format)
{
    bool changed = false;
    if (format.hasProperty(kApplied)) {
        const QVariantList    applied  = format.property(kApplied).toList();
        const QTextFormat     original = qvariant_cast<QTextFormat>(format.property(kOrigChar));
        for (int i = 0; i + 1 < applied.size(); i += 2) {
            const int id = applied.at(i).toInt();
            if (!sameValue(format, id, applied.at(i + 1)))
                continue; // someone changed it since: theirs stays
            if (original.hasProperty(id))
                format.setProperty(id, original.property(id));
            else
                format.clearProperty(id);
        }
        format.clearProperty(kApplied);
        format.clearProperty(kOrigChar);
        changed = true;
    }
    if (format.hasProperty(emojiformat::kOriginalFormat)) {
        QTextCharFormat inner = qvariant_cast<QTextFormat>(format.property(emojiformat::kOriginalFormat)).toCharFormat();
        if (revertFormat(inner)) {
            format.setProperty(emojiformat::kOriginalFormat, QVariant::fromValue<QTextFormat>(inner));
            changed = true;
        }
    }
    return changed;
}

void setCharFormatAt(QTextDocument* doc, int from, int to, const QTextCharFormat& format)
{
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.setCharFormat(format);
}

// ---- block formats -------------------------------------------------------------------------------------

const int kBlockProperties[] = {QTextFormat::BlockLeftMargin, QTextFormat::TextIndent, QTextFormat::BlockTopMargin, QTextFormat::BlockBottomMargin,
                                QTextFormat::LineHeight,      QTextFormat::LineHeightType,  QTextFormat::LayoutDirection, QTextFormat::BackgroundBrush};

QTextBlockFormat revertedBlock(QTextBlockFormat format)
{
    if (format.hasProperty(kOrigBlock)) {
        const QTextFormat original = qvariant_cast<QTextFormat>(format.property(kOrigBlock));
        for (const int id : kBlockProperties) {
            if (original.hasProperty(id))
                format.setProperty(id, original.property(id));
            else
                format.clearProperty(id);
        }
    }
    format.clearProperty(kOrigBlock);
    format.clearProperty(kBlockTag);
    format.clearProperty(kKind);
    format.clearProperty(kCollapsed);
    format.clearProperty(kMention);
    format.clearProperty(kRunHead);
    return format;
}

bool blockHasOurs(const QTextBlockFormat& format)
{
    return format.hasProperty(kOrigBlock) || format.hasProperty(kBlockTag) || format.hasProperty(kKind) || format.hasProperty(kCollapsed) || format.hasProperty(kMention)
           || format.hasProperty(kRunHead);
}

bool wordChar(const QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c.category() == QChar::Mark_NonSpacing;
}

QString nameFor(const Env& env, const char* kind, int serial)
{
    return objectPrefix() + QString::fromLatin1(kind) + QLatin1Char('/') + env.tag + QLatin1Char('.') + QString::number(serial) + QLatin1Char('@')
           + QString::number(env.dpr, 'g', 3);
}

QString blankName()
{
    return QString::fromLatin1("tsmedia-layout:blank");
}

// ---- reading the text of a system or day line ------------------------------------------------------------

struct Prefix {
    bool    icon = false;
    QString time;
    int     length = 0; // characters of icon, time and spaces
};

Prefix prefixOf(const QString& text, const QTextBlock& block, int offset)
{
    Prefix p;
    int    at = offset;
    if (at < text.size() && text.at(at) == QChar::ObjectReplacementCharacter) {
        const QTextCharFormat f = formatAt(const_cast<QTextDocument*>(block.document()), block.position() + at);
        if (f.isImageFormat() && !isOurs(f) && !emojiformat::isHd(f) && !isReplyObject(f)) {
            p.icon = true;
            ++at;
        }
    }
    int len = 0;
    p.time  = leadingTime(text.mid(at, 32), &len);
    at += len;
    while (at < text.size() && (text.at(at) == QLatin1Char(' ') || text.at(at) == QChar::Nbsp))
        ++at;
    p.length = at - offset;
    return p;
}

QDate dateInside(const QString& text)
{
    // TeamSpeak's own "Chat begins 2026-10-10 04:44:02" first (whatever the system's date format is).
    static const QRegularExpression iso(QStringLiteral("(?<!\\d)(\\d{4})-(\\d{1,2})-(\\d{1,2})(?!\\d)"));
    const QRegularExpressionMatch   i = iso.match(text);
    if (i.hasMatch()) {
        const QDate d(i.captured(1).toInt(), i.captured(2).toInt(), i.captured(3).toInt());
        if (d.isValid())
            return d;
    }
    static const QRegularExpression re(QStringLiteral("(\\d{1,4}[./-]\\d{1,2}[./-]\\d{1,4})"));
    const QRegularExpressionMatch   m = re.match(text);
    return m.hasMatch() ? layoutart::parseDayText(m.captured(1)) : QDate();
}

// TeamSpeak's history marker: "*** Chat begins 2026-10-10 04:44:02" (the channel and private tabs) or "*** Log
// begins ..." (the server tab), the date and time last; words: TeamSpeak's words (names and channels blanked).
bool isHistoryMarker(const QString& words)
{
    static const QRegularExpression re(QStringLiteral("\\d{4}-\\d{1,2}-\\d{1,2} \\d{1,2}:\\d{2}(?::\\d{2})?\\s*$"));
    return words.size() <= 120 && re.match(words.trimmed()).hasMatch();
}

// TeamSpeak's info icon in front of its status lines: <img src="iconpath:MESSAGE_INFO?size=13x13" width="13"
// height="13">. Its size is in the HTML; a picture a user sends ([img]) has none.
bool isInfoIcon(const QTextCharFormat& format)
{
    if (!format.isImageFormat() || isOurs(format) || emojiformat::isHd(format) || isReplyObject(format))
        return false;
    const QTextImageFormat image = format.toImageFormat();
    const QString          name  = image.name();
    static const QLatin1String prefix("iconpath:MESSAGE_INFO");
    if (!name.startsWith(prefix) || (name.size() > prefix.size() && name.at(prefix.size()) != QLatin1Char('?')))
        return false;
    return image.width() > 0 && image.height() > 0 && image.width() <= 64 && image.height() <= 64;
}

} // namespace

SystemKind statusKindOf(const QString& wordsIn, bool* keepFormatting)
{
    struct Shape {
        const char* pattern;
        SystemKind  kind;
        bool        keep;
    };
    static const Shape shapes[] = {
        // joins, switches and moves
        {"^\"\" connected to channel \"\"", SystemKind::Join, false},
        {"^Connected to server\\b", SystemKind::Join, false},
        {"^(?:\"\"|You) switched from channel \"\" to \"\"", SystemKind::Join, false},
        {"^(?:\"\"|You) (?:was|were) moved from channel \"\"", SystemKind::Join, false},
        {"^\"\" appears, ", SystemKind::Join, false},
        {"^You are now talking in channel: \"\"", SystemKind::Join, false},
        // leaves
        {"^\"\" disconnected\\b", SystemKind::Leave, false},
        {"^(?:\"\"|You) dropped\\b", SystemKind::Leave, false},
        {"^\"\" left\\b", SystemKind::Leave, false},
        {"^\"\" timed out\\b", SystemKind::Leave, false},
        {"^Disconnected from server\\b", SystemKind::Leave, false},
        {"^Connection to server lost\\b", SystemKind::Leave, false},
        {"^Chat partner (?:has )?(?:disconnected|left|closed)\\b", SystemKind::Leave, false},
        // kicks, bans and errors: never folded, their red kept
        {"^(?:\"\"|You) (?:was|were) kicked from ", SystemKind::Danger, false},
        {"^(?:\"\"|You) (?:was|were) banned\\b", SystemKind::Danger, false},
        {"^Client banned: ", SystemKind::Danger, false},
        {"^insufficient client permissions\\b", SystemKind::Danger, false},
        {"^You don't have permissions?\\b", SystemKind::Danger, false},
        {"^No permissions?\\b", SystemKind::Danger, false},
        {"^Action currently not possible\\b", SystemKind::Danger, false},
        {"^invalid parameter\\b", SystemKind::Danger, false},
        {"^client is flooding\\b", SystemKind::Danger, false},
        {"^The icon for .{0,200} was not\\b", SystemKind::Danger, false},
        {"^Transfer \"\" reports\\b", SystemKind::Danger, false},
        {"^myTeamSpeak ID is invalid\\b", SystemKind::Danger, false},
        {"^This server is blacklisted\\b", SystemKind::Danger, false},
        {"^The TeamSpeak server could not\\b", SystemKind::Danger, false},
        {"^Reconnecting might solve\\b", SystemKind::Danger, false},
        // groups
        {"^Channel group \"\" (?:\\[[^\\]]{0,200}\\] )?was assigned to \"\"", SystemKind::Group, false},
        {"^\"\" was (?:added to|removed from) server group \"\"", SystemKind::Group, false},
        // edits
        {"^Channel \"\" (?:was )?(?:created|deleted|edited|renamed|moved)\\b", SystemKind::Edit, false},
        {"^Server properties have been edited\\b", SystemKind::Edit, false},
        {"^(?:\"\"|You) (?:is|are) now known as \"\"", SystemKind::Edit, false},
        {"^Your avatar was deleted\\b", SystemKind::Edit, false},
        // pokes: never folded, TeamSpeak's colour kept
        {"^\"\" pokes you\\b", SystemKind::Poke, false},
        {"^You poked \"\"", SystemKind::Poke, false},
        // TeamSpeak's own notes
        {"^Trying to connect to server on ", SystemKind::Info, false},
        {"^\"\" (?:started|stopped) recording\\b", SystemKind::Info, false},
        {"^Renewed myTeamSpeak ID received\\b", SystemKind::Info, false},
        // TeamSpeak's own welcome (its link, its colours)
        {"^Welcome to TeamSpeak, check ", SystemKind::Announce, true},
    };
    static const QVector<QRegularExpression> compiled = [] {
        QVector<QRegularExpression> out;
        for (const Shape& s : shapes)
            out.append(QRegularExpression(QString::fromLatin1(s.pattern), QRegularExpression::CaseInsensitiveOption));
        return out;
    }();
    // Quoted words are names (a group, a new nickname): "" like the linked ones.
    static const QRegularExpression quoted(QStringLiteral("\"[^\"\\n\\x{2028}]{0,200}\""));
    QString words = wordsIn.left(600);
    words.replace(quoted, QStringLiteral("\"\""));
    words = words.trimmed();
    for (int i = 0; i < compiled.size(); ++i) {
        if (compiled.at(i).match(words).hasMatch()) {
            if (keepFormatting)
                *keepFormatting = shapes[i].keep;
            return shapes[i].kind;
        }
    }
    if (keepFormatting)
        *keepFormatting = true;
    return SystemKind::Info;
}

namespace {

bool isOwnPrintPrefix(const Piece& piece)
{
    return piece.text.startsWith(QLatin1String("TS Media chat")) && piece.format.fontWeight() >= QFont::DemiBold && !piece.format.isImageFormat();
}

// A name or a channel in a system row ("client://...", "channelid://..."): words users choose themselves.
bool isNameLink(const QTextCharFormat& format)
{
    if (!format.isAnchor() || format.isImageFormat())
        return false;
    const QString href = format.anchorHref();
    return href.startsWith(QLatin1String("client://"), Qt::CaseInsensitive) || href.startsWith(QLatin1String("channelid://"), Qt::CaseInsensitive)
           || href.startsWith(QLatin1String("channel://"), Qt::CaseInsensitive);
}

// A system row's own words from document position from on, what its kind is read from: names and channels
// count as "" (a user called "Pokemon" or "Terror", a channel called "Kicked", is no poke, error or kick);
// our pictures, trailer and chips count as nothing.
QString eventWords(const QTextBlock& block, int from)
{
    QString out;
    for (auto it = block.begin(); !it.atEnd() && out.size() < 600; ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid() || f.position() + f.length() <= from)
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (isOurs(cf))
            continue;
        if (isNameLink(cf)) {
            if (!out.endsWith(QLatin1String("\"\"")))
                out += QLatin1String("\"\"");
            continue;
        }
        out += f.text().mid(qMax(0, from - f.position()));
    }
    return out;
}

// The row a "*** " line is (after its prefix): a day, a history marker or a system row. words: what the
// system row's kind is read from (eventWords).
void readStars(Row& row, const QString& rest, const Prefix& prefix, const QString& words)
{
    const QString body = rest.trimmed();
    if (prefix.time.isEmpty()) {
        const QDate day = layoutart::parseDayText(body);
        if (day.isValid()) {
            row.type    = RowType::Day;
            row.date    = day;
            row.dayText = body;
            return;
        }
        // Icon-less lines, and TeamSpeak's "*** Chat begins 2026-10-10 04:44:02" (with its info icon).
        if (!prefix.icon || isHistoryMarker(words)) {
            row.type    = RowType::History;
            row.date    = dateInside(body);
            row.dayText = body;
            return;
        }
    }
    row.type = RowType::System;
    row.time = prefix.time;
    row.kind = systemKindOf(words, &row.keepFormatting);
}

Row readStyled(const QTextBlock& block, const Lead& lead)
{
    Row row;
    row.styled    = true;
    row.lead      = lead.kind;
    row.bodyStart = lead.bodyStart;
    row.reply     = lead.reply >= 0;
    const QVariantList runs = runsOf(lead.format);
    switch (lead.kind) {
    case Head:
    case Continuation:
    case CompactHead: {
        const Header h = headerOf(block);
        if (!h.valid)
            break;
        row.type      = RowType::Message;
        row.nick      = nickFromQuoted(h.nickText);
        row.href      = h.nickHref;
        row.uid       = albums::uidFromClientHref(h.nickHref);
        row.clientId  = row.uid.isEmpty() ? 0 : clientIdOf(h.nickHref);
        row.nickColor = h.nickColor;
        row.time      = h.time;
        for (int i = 0; i + 1 < runs.size(); i += 2) {
            const QTextFormat f = qvariant_cast<QTextFormat>(runs.at(i + 1));
            if (f.isImageFormat() && f.toImageFormat().name().contains(QLatin1String("OUTGOING"), Qt::CaseInsensitive))
                row.outgoing = true;
        }
        break;
    }
    case SystemPrefix: {
        const QString prefixText = runsText(runs);
        // TeamSpeak's words of the row after our prefix (names and channels, our trailer and chips left out).
        const QString body = eventWords(block, row.bodyStart);
        if (prefixText.startsWith(QLatin1String("TS Media chat"))) {
            row.type     = RowType::System;
            row.ownPrint = true;
            row.kind     = prefixText.contains(QChar(0x26a0)) ? SystemKind::Warning : SystemKind::TsMedia;
            break;
        }
        int           len  = 0;
        QString       rest = prefixText;
        if (rest.startsWith(QChar::ObjectReplacementCharacter))
            rest.remove(0, 1);
        row.time = leadingTime(rest, &len);
        row.type = RowType::System;
        // As read() saw it: a "*** " line, or a status line without it (its prefix is the icon and time only).
        if (rest.mid(len).contains(QLatin1String("***")))
            row.kind = systemKindOf(body, &row.keepFormatting);
        else
            row.kind = statusKindOf(body, &row.keepFormatting);
        break;
    }
    case Divider: {
        QString text  = runsText(runs);
        bool    icon  = text.startsWith(QChar::ObjectReplacementCharacter);
        if (icon)
            text.remove(0, 1);
        int     len  = 0;
        const QString time = leadingTime(text, &len);
        text = text.mid(len).trimmed();
        Prefix prefix;
        prefix.icon = icon;
        prefix.time = time;
        if (text.startsWith(QLatin1String("***")))
            readStars(row, text.mid(3), prefix, text.mid(3)); // a divider: never a system row
        if (row.type != RowType::Day && row.type != RowType::History) {
            row.type = RowType::History; // a divider stays one
            row.date = dateInside(text);
        }
        break;
    }
    default:
        break;
    }
    return row;
}

} // namespace

// ============================================================================================
// Reading
// ============================================================================================

QVector<QPair<int, int>> mentionRanges(const QString& text, const QString& nickIn)
{
    QVector<QPair<int, int>> out;
    const QString            nick = nickIn.trimmed();
    if (nick.isEmpty() || nick.size() > 64 || text.size() > 20000)
        return out;
    const int  len        = nick.size();
    const bool wordFirst  = wordChar(nick.at(0));
    const bool wordLast   = wordChar(nick.at(len - 1));
    int        from       = 0;
    int        guard      = 0;
    while (from < text.size() && guard++ < 256) {
        const int at = text.indexOf(nick, from, Qt::CaseInsensitive);
        if (at < 0)
            break;
        const bool after = at + len >= text.size() || !wordLast || !wordChar(text.at(at + len));
        // "@Nick": the @ at the start or after something that isn't a word (not "mail@Nick").
        const bool atSign = at > 0 && text.at(at - 1) == QLatin1Char('@') && (at == 1 || !wordChar(text.at(at - 2)));
        const bool before = at == 0 || !wordFirst || !wordChar(text.at(at - 1));
        if (after && (atSign || (len >= 3 && before))) {
            out.append({atSign ? at - 1 : at, at + len});
            from = at + len;
        } else {
            from = at + 1;
        }
    }
    return out;
}

bool isMention(const QTextBlock& block)
{
    return block.isValid() && block.blockFormat().boolProperty(kMention);
}

QString nickFromQuoted(const QString& quoted)
{
    QString nick = quoted.trimmed();
    if (nick.size() >= 2 && nick.startsWith(QLatin1Char('"')) && nick.endsWith(QLatin1Char('"')))
        nick = nick.mid(1, nick.size() - 2);
    if (nick.isEmpty() || nick.size() > 64)
        return {};
    for (const QChar c : nick) {
        if (c == QLatin1Char('\n') || c == QChar::LineSeparator || c == QChar::ParagraphSeparator || c == QChar::ObjectReplacementCharacter)
            return {};
    }
    return nick;
}

SystemKind systemKindOf(const QString& textIn, bool* keepFormatting)
{
    const QString text = textIn.toLower();
    if (keepFormatting)
        *keepFormatting = false;
    const auto has = [&text](const char* word) { return text.contains(QLatin1String(word)); };
    if (has("poke"))
        return SystemKind::Poke;
    if (has("kicked") || has("banned") || has("error"))
        return SystemKind::Danger;
    if (has("disconnected") || has("left ") || text.endsWith(QLatin1String("left")) || has("dropped") || has("timed out") || has("timeout") || has("quit"))
        return SystemKind::Leave;
    if (has("connected") || has("joined") || has("switched") || has("now talking in") || has("entered") || has("moved"))
        return SystemKind::Join;
    if (has("server group") || has("channel group") || has("talk power"))
        return SystemKind::Group;
    if (has("welcome") || has("host message")) {
        if (keepFormatting)
            *keepFormatting = true;
        return SystemKind::Announce;
    }
    if (has("edited") || has("changed") || has("renamed") || has("created") || has("deleted") || has("description"))
        return SystemKind::Edit;
    return SystemKind::Info;
}

Row read(const QTextBlock& block)
{
    if (!block.isValid())
        return {};
    const Lead lead = leadOf(block);
    if (lead.styled)
        return readStyled(block, lead);

    Row                  row;
    const QVector<Piece> pieces = piecesOf(block);
    const int            bpos   = block.position();
    const QString        text   = block.text();
    int                  offset = 0;
    row.bodyStart               = bpos;
    if (!pieces.isEmpty() && pieces.first().start == bpos && isReplyObject(pieces.first().format)) {
        row.reply = true;
        offset    = 1;
        if (text.size() > 1 && text.at(1) == QChar::LineSeparator && isReplySeparator(formatAt(const_cast<QTextDocument*>(block.document()), bpos + 1)))
            offset = 2;
    }

    // TS Media's own lines: "TS Media chat" in bold first (ts3api's printInfo / printWarning).
    if (offset == 0 && !pieces.isEmpty() && isOwnPrintPrefix(pieces.first())) {
        const Piece& p    = pieces.first();
        int          end  = p.start + qMin(p.text.size(), 16);
        // The prefix fragment and the space after it.
        end = p.end;
        if (end - bpos < text.size() && text.at(end - bpos) == QLatin1Char(' '))
            ++end;
        row.type        = RowType::System;
        row.ownPrint    = true;
        row.kind        = p.text.contains(QChar(0x26a0)) ? SystemKind::Warning : SystemKind::TsMedia;
        row.headerStart = bpos;
        row.headerEnd   = end;
        row.bodyStart   = end;
        return row;
    }

    const Prefix prefix = prefixOf(text, block, offset);
    const int    after  = offset + prefix.length;
    if (text.mid(after, 4) == QLatin1String("*** ") || (text.mid(after) == QLatin1String("***"))) {
        readStars(row, text.mid(after + 3), prefix, eventWords(block, bpos + after + 3));
        if (row.type == RowType::Day || row.type == RowType::History) {
            if (offset != 0) { // never under a reply line
                row = Row();
                row.type = RowType::Other;
                return row;
            }
            row.headerStart = bpos;
            row.headerEnd   = bpos + text.size();
            row.bodyStart   = row.headerEnd;
            return row;
        }
        row.headerStart = bpos + offset;
        row.headerEnd   = bpos + qMin(text.size(), after + 4);
        row.bodyStart   = row.headerEnd;
        if (offset != 0) {
            row      = Row();
            row.type = RowType::Other;
        }
        return row;
    }

    // ---- a message: [icon]<time> "Nick": --------------------------------------------------------------------
    int     nickStart = -1;
    int     nickEnd   = -1;
    QString nickHref;
    QColor  nickColor;
    for (const Piece& p : pieces) {
        if (p.end <= bpos + offset)
            continue;
        if (isClientLink(p.format) && !p.format.isImageFormat()) {
            if (nickStart < 0) {
                nickStart = p.start;
                nickEnd   = p.end;
                nickHref  = p.format.anchorHref();
                if (p.format.foreground().style() != Qt::NoBrush)
                    nickColor = p.format.foreground().color();
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
    if (nickStart >= 0) {
        static const QRegularExpression leadRx(QStringLiteral("^\\x{FFFC}?[ \\t]*(?:<([^<>\\n\\x{2028}\\x{2029}]{1,24})>)?[ \\t]*\"?$"));
        const QRegularExpressionMatch   before = leadRx.match(text.mid(offset, nickStart - bpos - offset));
        const QString                   nick   = nickFromQuoted(text.mid(nickStart - bpos, nickEnd - nickStart));
        QString                         tail   = text.mid(nickEnd - bpos, 3);
        int                             skip   = 0;
        if (tail.startsWith(QLatin1Char('"'))) {
            tail.remove(0, 1);
            skip = 1;
        }
        if (before.hasMatch() && !nick.isEmpty() && tail.startsWith(QLatin1Char(':'))) {
            row.type      = RowType::Message;
            row.nick      = nick;
            row.href      = nickHref;
            row.uid       = albums::uidFromClientHref(nickHref);
            row.clientId  = row.uid.isEmpty() ? 0 : clientIdOf(nickHref);
            row.nickColor = nickColor;
            row.time      = before.captured(1);
            int len = 0;
            if (!row.time.isEmpty() && leadingTime(QLatin1Char('<') + row.time + QLatin1Char('>'), &len).isEmpty())
                row.time.clear(); // not a time we read
            if (text.at(offset) == QChar::ObjectReplacementCharacter) {
                const QTextCharFormat icon = formatAt(const_cast<QTextDocument*>(block.document()), bpos + offset);
                row.outgoing               = icon.isImageFormat() && icon.toImageFormat().name().contains(QLatin1String("OUTGOING"), Qt::CaseInsensitive);
            }
            row.headerStart = bpos + offset;
            row.headerEnd   = nickEnd + skip + (tail.size() > 1 && tail.at(1) == QLatin1Char(' ') ? 2 : 1);
            row.bodyStart   = row.headerEnd;
            return row;
        }
    }

    // ---- a status line without "*** " (the server tab, private chats): [MESSAGE_INFO]<time> text ----------
    // TeamSpeak's own: its info icon (with its size) leads the block; never under a reply line, and no nick
    // header (that is a message, above). Users' text never starts a block with that icon.
    if (offset == 0 && prefix.icon && after < text.size() && isInfoIcon(formatAt(const_cast<QTextDocument*>(block.document()), bpos))) {
        const QString words = eventWords(block, bpos + after);
        if (!words.trimmed().isEmpty()) {
            row.type        = RowType::System;
            row.time        = prefix.time;
            row.kind        = statusKindOf(words, &row.keepFormatting);
            row.headerStart = bpos;
            row.headerEnd   = bpos + after;
            row.bodyStart   = row.headerEnd;
            return row;
        }
    }
    row.type = RowType::Other;
    return row;
}

// ============================================================================================
// Planning
// ============================================================================================

namespace {

bool sameAuthor(const Row& a, const Row& b)
{
    if (a.nick != b.nick)
        return false;
    if (!a.uid.isEmpty() || !b.uid.isEmpty())
        return a.uid == b.uid;
    return true;
}

layoutart::HeadSpec headSpecFor(const Env& env, int blockNumber, const Row& row, const QDate& date)
{
    layoutart::HeadSpec spec;
    spec.nick      = row.nick;
    spec.uid       = row.uid;
    spec.nameColor = layoutart::nameColorFor(env.colors, row.nickColor, env.usualNickColor);
    spec.timeLabel = layoutart::headTimeLabel(date, env.today, row.time, env.tokens.ampm || layoutart::hasAmPm(row.time));
    if (row.reply && env.mode == Mode::Cozy && env.replyHeader) {
        replyart::Header header;
        if (env.replyHeader(blockNumber, &header)) {
            spec.reply       = true;
            spec.replyHeader = header;
            replyart::HeaderStyle style;
            style.dark     = env.colors.dark;
            style.base     = env.colors.base;
            spec.replyName = replyart::headerNameColor(header, style);
        }
    }
    return spec;
}

} // namespace

Plan plan(const Env& env, const QTextBlock& block, const Row& row, const Neighbour& previous, const QDate& date, qint64 arrivedMs, int serial)
{
    Plan                     p;
    const layoutart::Tokens& t       = env.tokens;
    const bool               compact = env.mode == Mode::Compact;
    const RowType            prev    = previous.valid ? previous.row.type : RowType::Unknown;
    const bool               afterDivider = prev == RowType::Day || prev == RowType::History;
    p.mode = env.mode;
    p.type = row.type;
    p.tag  = serial;
    p.time = row.time;
    switch (row.type) {
    case RowType::Message: {
        bool join = env.group && previous.valid && prev == RowType::Message && !row.reply && sameAuthor(previous.row, row);
        if (join) {
            const int a = layoutart::secondsOf(previous.row.time);
            const int b = layoutart::secondsOf(row.time);
            if (a >= 0 && b >= 0)
                join = b - a >= 0 && b - a <= kJoinSeconds;
            else if (a < 0 && b < 0) // no timestamps: both arrived live within the window
                join = previous.arrived > 0 && arrivedMs > 0 && arrivedMs - previous.arrived >= 0 && arrivedMs - previous.arrived <= kJoinSeconds * 1000LL;
            else
                join = false;
        }
        p.continuation = join;
        if (join) {
            p.name = blankName();
            p.size = compact ? QSizeF(t.L, 1) : QSizeF(t.G, 0);
            p.top  = 0;
            break;
        }
        if (compact) {
            p.name = nameFor(env, "head", serial);
            p.size = QSizeF(layoutart::compactHeadSize(row.nick, t, row.reply, env.contentWidth));
            p.top  = afterDivider ? t.afterDivider : t.compactHeadTop;
        } else {
            p.name = nameFor(env, "head", serial);
            p.size = QSizeF(layoutart::headSize(headSpecFor(env, block.blockNumber(), row, date), t, env.contentWidth));
            p.top  = afterDivider ? t.afterDivider : t.headTop;
        }
        break;
    }
    case RowType::System:
        p.name = nameFor(env, "sys", serial);
        p.size = QSizeF(layoutart::systemPrefixSize(t));
        p.kind = row.kind;
        if (compact)
            p.top = afterDivider ? t.afterDivider : t.compactSystemTop;
        else
            p.top = afterDivider ? t.afterDivider : (prev == RowType::System ? t.systemRunTop : t.systemTop);
        if (!compact && !row.time.isEmpty())
            p.trailer = QStringLiteral("  ") + layoutart::shortTime(row.time, t.ampm || layoutart::hasAmPm(row.time));
        break;
    case RowType::Day:
    case RowType::History:
        p.name   = nameFor(env, "day", serial);
        p.size   = QSizeF(qMax(40, env.contentWidth), layoutart::dividerHeight(t));
        p.top    = previous.valid ? t.dividerTop : 2;
        p.bottom = t.dividerBottom;
        break;
    case RowType::Other:
        p.top = afterDivider ? t.afterDivider : (compact ? t.compactSystemTop : t.systemRunTop);
        break;
    case RowType::Unknown:
        break;
    }
    return p;
}

// ============================================================================================
// Edits
// ============================================================================================

QPixmap blankPicture()
{
    QPixmap blank(1, 1);
    blank.fill(Qt::transparent);
    return blank;
}

bool hasOurs(const QTextBlock& block)
{
    if (!block.isValid())
        return false;
    if (blockHasOurs(block.blockFormat()))
        return true;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf = f.charFormat();
        if (cf.hasProperty(kKind) || cf.hasProperty(kApplied) || cf.hasProperty(kOrigChar))
            return true;
        if (emojiformat::isHd(cf) && emojiformat::originalFormat(cf).hasProperty(kApplied))
            return true;
    }
    return false;
}

bool apply(QTextDocument* doc, int n, const Plan& plan, const Env& env)
{
    QTextBlock block = doc ? doc->findBlockByNumber(n) : QTextBlock();
    if (!block.isValid() || plan.type == RowType::Unknown)
        return false;
    const Row row = read(block);
    if (row.styled || row.type != plan.type)
        return false;
    const layoutart::Tokens& t       = env.tokens;
    const bool               compact = plan.mode == Mode::Compact;
    const int                bpos    = block.position();
    QTextCursor              c(doc);

    // The header's plain format: our picture and line break take its font.
    QTextCharFormat base;
    if (row.headerStart >= 0) {
        for (const Piece& p : piecesOf(block)) {
            if (p.end > row.headerStart && !p.format.isImageFormat() && !p.format.isAnchor()) {
                base = p.format;
                break;
            }
        }
    }
    QFont baseFont = base.font().resolve(doc->defaultFont());

    // ---- the picture in place of TeamSpeak's header (or the whole line) -------------------------------------
    if (row.type != RowType::Other && row.headerStart >= 0 && row.headerEnd > row.headerStart) {
        const QVariantList runs = takeRuns(block, row.headerStart, row.headerEnd);
        Kind               kind = None;
        switch (row.type) {
        case RowType::Message:
            kind = plan.continuation ? Continuation : (compact ? CompactHead : Head);
            break;
        case RowType::System:
            kind = SystemPrefix;
            break;
        default:
            kind = Divider;
            break;
        }
        QTextImageFormat image;
        image.setFont(baseFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
        image.setName(plan.name);
        image.setWidth(plan.size.width());
        image.setHeight(plan.size.height());
        image.setProperty(kKind, static_cast<int>(kind));
        image.setProperty(kRuns, runs);
        if (!plan.time.isEmpty())
            image.setProperty(kTime, plan.time);
        // Compact: the picture is the body font's ascent + descent high. Qt puts an AlignNormal / AlignBaseline
        // picture's bottom on the baseline (its text would sit a descent too high); AlignBottom puts its bottom
        // on the line's bottom, so the baseline drawn in it is the text's.
        if (kind == CompactHead || (kind == SystemPrefix && compact))
            image.setVerticalAlignment(QTextCharFormat::AlignBottom);
        else if (kind == SystemPrefix)
            image.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        if ((kind == Head || kind == CompactHead) && !row.href.isEmpty()) {
            image.setAnchor(true); // a click on the head is a click on the nickname
            image.setAnchorHref(row.href);
        }
        // Blank until it is on screen (ChatLayout draws it then); never missing (Qt would draw its file icon).
        doc->addResource(QTextDocument::ImageResource, QUrl(plan.name), blankPicture());
        c.setPosition(row.headerStart);
        c.setPosition(row.headerEnd, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        // Cozy reply head: the head goes in front of the reply line ([H][R0][line break][body]).
        const int at = (kind == Head && row.reply) ? bpos : row.headerStart;
        c.setPosition(at);
        c.insertImage(image);
        if (kind == Head && !row.reply) {
            QTextCharFormat separator;
            QFont           tiny = baseFont;
            tiny.setPixelSize(1);
            separator.setFont(tiny, QTextCharFormat::FontPropertiesSpecifiedOnly);
            separator.setProperty(QTextFormat::FontPixelSize, 1);
            separator.setProperty(kKind, static_cast<int>(Separator));
            c.insertText(QString(QChar::LineSeparator), separator);
        }
        block = doc->findBlockByNumber(n);
    }

    // ---- recolouring (formats only: no position moves) ---------------------------------------------------------
    const Lead lead      = leadOf(block);
    const int  bodyStart = lead.styled ? lead.bodyStart : block.position();
    const int  blockEnd  = block.position() + block.length() - 1;
    if (row.type == RowType::Message && !plan.continuation && !compact && row.reply && lead.reply >= 0) {
        // ChatReplies' picture keeps the quote line but shows nothing: the head draws the reply row.
        const QTextCharFormat replyFormat = formatAt(doc, lead.reply);
        if (isReplyObject(replyFormat) && !replyFormat.hasProperty(kApplied))
            setCharFormatAt(doc, lead.reply, lead.reply + 1, withChanges(replyFormat, {{QTextFormat::ImageWidth, 0.0}, {QTextFormat::ImageHeight, 0.0}}));
        // ChatReplies' line break: a 1 px font, so the head's line is exactly the picture's height.
        const int at = lead.reply + 1;
        if (at < blockEnd && doc->characterAt(at) == QChar::LineSeparator) {
            const QTextCharFormat f = formatAt(doc, at);
            if (isReplySeparator(f) && !f.hasProperty(kApplied))
                setCharFormatAt(doc, at, at + 1, withChanges(f, {{QTextFormat::FontPixelSize, 1}, {QTextFormat::FontPointSize, QVariant()}}));
        }
    }
    QVector<QPair<QPair<int, int>, QTextCharFormat>> recolour;
    for (const Piece& p : piecesOf(block)) {
        if (p.end <= bodyStart || p.start >= blockEnd || p.format.isImageFormat() || isOurs(p.format) || p.format.hasProperty(kApplied))
            continue;
        if (isReplySeparator(p.format))
            continue;
        QVector<Change> changes;
        if (row.type == RowType::Message) {
            // Links (not file links, which become previews): the skin's colour at 4.5:1, a 40 % underline.
            if (!p.format.isAnchor() || isFileLink(p.format))
                continue;
            const QColor current = p.format.foreground().style() != Qt::NoBrush ? p.format.foreground().color() : QColor();
            if (!current.isValid() || ui::contrastRatio(current, env.colors.base) < 4.5 || ui::contrastRatio(current, layoutart::hoverRow(env.colors)) < 4.5)
                changes.append({QTextFormat::ForegroundBrush, QBrush(env.colors.link)});
            changes.append({QTextFormat::TextUnderlineColor, env.colors.linkUnderline});
        } else if (row.type == RowType::System && !row.keepFormatting && !row.ownPrint) {
            const QColor current = p.format.foreground().style() != Qt::NoBrush ? p.format.foreground().color() : QColor();
            if (row.kind == SystemKind::Danger || row.kind == SystemKind::Poke) {
                // Kicks, bans and errors keep their red (at 4.5:1); pokes keep TeamSpeak's colour.
                if (current.isValid() && row.kind == SystemKind::Danger && ui::contrastRatio(current, env.colors.base) < 4.5)
                    changes.append({QTextFormat::ForegroundBrush, QBrush(replyart::readable(current, env.colors.base, env.colors.danger, 4.5))});
            } else if (p.format.isAnchor()) {
                changes.append({QTextFormat::ForegroundBrush, QBrush(env.colors.name)}); // a name or a channel, bold kept
            } else {
                changes.append({QTextFormat::ForegroundBrush, QBrush(env.colors.muted)});
            }
        } else if (row.type == RowType::System && row.ownPrint && p.format.foreground().style() != Qt::NoBrush) {
            // TS Media's own lines: readable on this skin (their colour is TeamSpeak's dark blue otherwise).
            const QColor current = p.format.foreground().color();
            if (ui::contrastRatio(current, env.colors.base) < 4.5)
                changes.append({QTextFormat::ForegroundBrush, QBrush(env.colors.text)});
        }
        if (!changes.isEmpty())
            recolour.append({{qMax(p.start, bodyStart), qMin(p.end, blockEnd)}, withChanges(p.format, changes)});
    }
    for (const auto& r : qAsConst(recolour))
        setCharFormatAt(doc, r.first.first, r.first.second, r.second);

    // ---- mentions (P2): a pill on "@Nick" or the nickname in someone else's message; the row is marked ------
    bool mentioned = false;
    if (row.type == RowType::Message && env.mentions && !env.ownNick.isEmpty() && !row.outgoing && (env.ownUid.isEmpty() || row.uid != env.ownUid)) {
        block = doc->findBlockByNumber(n);
        QVector<QPair<QPair<int, int>, QTextCharFormat>> pills;
        for (const Piece& p : piecesOf(block)) {
            // Text only: never in links, pictures, the header (ours) or the reply line.
            if (p.end <= bodyStart || p.start >= blockEnd || p.format.isImageFormat() || p.format.isAnchor() || isOurs(p.format) || isReplySeparator(p.format)
                || p.format.hasProperty(kApplied))
                continue;
            const int     first = qMax(p.start, bodyStart);
            const int     last  = qMin(p.end, blockEnd);
            const QString text  = p.text.mid(first - p.start, last - first);
            for (const QPair<int, int>& r : mentionRanges(text, env.ownNick)) {
                pills.append({{first + r.first, first + r.second},
                              withChanges(p.format, {{QTextFormat::BackgroundBrush, QBrush(env.colors.pillBack)},
                                                     {QTextFormat::ForegroundBrush, QBrush(env.colors.pillText)},
                                                     {QTextFormat::FontWeight, static_cast<int>(QFont::DemiBold)}})});
            }
        }
        for (const auto& r : qAsConst(pills))
            setCharFormatAt(doc, r.first.first, r.first.second, r.second);
        mentioned = !pills.isEmpty();
    }

    // ---- the trailer: a system row's time at its end ------------------------------------------------------------
    if (!plan.trailer.isEmpty()) {
        block = doc->findBlockByNumber(n);
        const int       end  = block.position() + block.length() - 1;
        QTextCharFormat last = end > block.position() ? formatAt(doc, end - 1) : QTextCharFormat();
        QTextCharFormat trailer;
        QFont           small = t.time;
        trailer.setFont(small, QTextCharFormat::FontPropertiesSpecifiedOnly);
        trailer.setProperty(QTextFormat::FontPixelSize, small.pixelSize());
        trailer.setForeground(env.colors.muted);
        trailer.setProperty(kKind, static_cast<int>(Trailer));
        revertFormat(last); // as TeamSpeak made it
        last.clearProperty(kKind);
        trailer.setProperty(kOrigChar, QVariant::fromValue<QTextFormat>(last)); // what text typed after it should have had
        c.setPosition(end);
        c.insertText(plan.trailer, trailer);
    }

    // ---- the block format ------------------------------------------------------------------------------------
    block               = doc->findBlockByNumber(n);
    QTextBlockFormat bf = block.blockFormat();
    const QTextBlockFormat original = bf;
    if (!bf.hasProperty(kOrigBlock))
        bf.setProperty(kOrigBlock, QVariant::fromValue<QTextFormat>(original));
    bf.setLayoutDirection(Qt::LeftToRight); // the head stays on the left of a right-to-left message
    bf.setTopMargin(plan.top);
    bf.setBottomMargin(plan.bottom);
    int rowKind = RowOther;
    switch (row.type) {
    case RowType::Message:
        rowKind = RowMessage;
        if (compact) {
            const bool replyLine = row.reply; // [R][line break][name][body]: no hanging indent
            bf.setLeftMargin(t.L);
            bf.setTextIndent(replyLine ? 0 : -t.L);
            bf.setLineHeight(t.compactLeading, QTextBlockFormat::LineDistanceHeight);
        } else {
            bf.setLeftMargin(t.G);
            bf.setTextIndent(-t.G);
            bf.setLineHeight(t.lineMin, QTextBlockFormat::MinimumHeight);
        }
        break;
    case RowType::System: {
        rowKind = RowSystem;
        const int indent = compact ? t.L + 20 : t.G;
        bf.setLeftMargin(indent);
        bf.setTextIndent(-indent);
        if (compact)
            bf.setLineHeight(t.compactLeading, QTextBlockFormat::LineDistanceHeight);
        else
            bf.setLineHeight(t.lineMin, QTextBlockFormat::MinimumHeight);
        break;
    }
    case RowType::Day:
    case RowType::History:
        rowKind = RowDivider;
        bf.setLeftMargin(0);
        bf.setTextIndent(0);
        break;
    default:
        bf.setLeftMargin(compact ? t.L : t.G);
        bf.setTextIndent(0);
        break;
    }
    bf.setProperty(kBlockTag, plan.tag);
    bf.setProperty(kKind, rowKind);
    if (mentioned)
        bf.setProperty(kMention, true); // ChatLayout paints the row's tint and bar under it
    QTextCursor bc(block);
    bc.setBlockFormat(bf);
    return true;
}

bool unstyle(QTextDocument* doc, int n)
{
    QTextBlock block = doc ? doc->findBlockByNumber(n) : QTextBlock();
    if (!block.isValid() || !hasOurs(block))
        return false;
    QTextCursor c(doc);

    // Trailers and chips of ours (anywhere in the block), back to front.
    QVector<QPair<int, int>> remove;
    for (const Piece& p : piecesOf(block)) {
        const Kind kind = kindOf(p.format);
        if (kind == Trailer || kind == Chip)
            remove.append({p.start, p.end});
    }
    for (int i = remove.size() - 1; i >= 0; --i) {
        c.setPosition(remove.at(i).first);
        c.setPosition(remove.at(i).second, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }

    // What we recoloured gets its values back (formats only).
    block = doc->findBlockByNumber(n);
    QVector<QPair<QPair<int, int>, QTextCharFormat>> formats;
    for (const Piece& p : piecesOf(block)) {
        QTextCharFormat f = p.format;
        if (isObject(f))
            continue;
        if (revertFormat(f))
            formats.append({{p.start, p.end}, f});
    }
    for (const auto& r : qAsConst(formats))
        setCharFormatAt(doc, r.first.first, r.first.second, r.second);

    // Our picture (and our line break after it) back into TeamSpeak's header.
    block           = doc->findBlockByNumber(n);
    const Lead lead = leadOf(block);
    if (lead.styled) {
        const QVariantList runs = runsOf(lead.format);
        int                ours = 1;
        if (lead.object + 1 < doc->characterCount() && doc->characterAt(lead.object + 1) == QChar::LineSeparator && kindOf(formatAt(doc, lead.object + 1)) == Separator)
            ours = 2;
        c.setPosition(lead.object);
        c.setPosition(lead.object + ours, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(lead.bodyStart - ours);
        insertRuns(c, runs);
    }
    // Line breaks or other texts of ours left without their picture.
    block = doc->findBlockByNumber(n);
    QVector<QPair<int, int>> stray;
    for (const Piece& p : piecesOf(block)) {
        if (kindOf(p.format) == Separator)
            stray.append({p.start, p.end});
    }
    for (int i = stray.size() - 1; i >= 0; --i) {
        c.setPosition(stray.at(i).first);
        c.setPosition(stray.at(i).second, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }

    block                    = doc->findBlockByNumber(n);
    const QTextBlockFormat bf = block.blockFormat();
    if (blockHasOurs(bf)) {
        const bool collapsed = bf.boolProperty(kCollapsed);
        QTextCursor bc(block);
        bc.setBlockFormat(revertedBlock(bf));
        if (collapsed) {
            block = doc->findBlockByNumber(n);
            block.setVisible(true);
            doc->markContentsDirty(block.position(), block.length());
        }
    }
    return true;
}

bool sanitize(QTextDocument* doc, int n)
{
    QTextBlock block = doc ? doc->findBlockByNumber(n) : QTextBlock();
    if (!block.isValid())
        return false;
    bool        changed = false;
    QTextCursor c(doc);
    // Texts that carry our kinds or values without being ours here: TeamSpeak's text typed after one of
    // them took its format. They get the format they would have had (kOrigChar), or lose our properties.
    QVector<QPair<QPair<int, int>, QTextCharFormat>> formats;
    for (const Piece& p : piecesOf(block)) {
        QTextCharFormat f = p.format;
        bool            fix = false;
        if (isObject(f))
            continue;
        if (f.hasProperty(kKind)) {
            if (f.hasProperty(kOrigChar))
                f = qvariant_cast<QTextFormat>(f.property(kOrigChar)).toCharFormat();
            f.clearProperty(kKind);
            f.clearProperty(kOrigChar);
            fix = true;
        }
        if (revertFormat(f))
            fix = true;
        if (fix)
            formats.append({{p.start, p.end}, f});
    }
    for (const auto& r : qAsConst(formats))
        setCharFormatAt(doc, r.first.first, r.first.second, r.second);
    changed = !formats.isEmpty();
    block   = doc->findBlockByNumber(n);
    if (blockHasOurs(block.blockFormat())) {
        QTextCursor bc(block);
        bc.setBlockFormat(revertedBlock(block.blockFormat()));
        changed = true;
    }
    QTextCharFormat blockChar = block.charFormat();
    if (blockChar.hasProperty(kKind) || blockChar.hasProperty(kApplied) || blockChar.hasProperty(kOrigChar)) {
        if (blockChar.hasProperty(kOrigChar) && blockChar.hasProperty(kKind))
            blockChar = qvariant_cast<QTextFormat>(blockChar.property(kOrigChar)).toCharFormat();
        revertFormat(blockChar);
        blockChar.clearProperty(kKind);
        blockChar.clearProperty(kOrigChar);
        if (blockChar.isImageFormat())
            blockChar = QTextCharFormat();
        QTextCursor bc(block);
        bc.setBlockCharFormat(blockChar);
        changed = true;
    }
    return changed;
}

int restoreAll(QTextDocument* doc, QStringList* names)
{
    if (!doc)
        return 0;
    int done = 0;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    for (QTextBlock b = doc->lastBlock(); b.isValid(); b = b.previous()) {
        if (!hasOurs(b))
            continue;
        const int n = b.blockNumber();
        if (names) {
            const Lead lead = leadOf(b);
            if (lead.styled)
                names->append(lead.format.toImageFormat().name());
            for (const ChipRef& chip : chipsOf(b)) // P2: the chips' pictures go too
                names->append(chip.name);
        }
        done += unstyle(doc, n) ? 1 : 0;
        if (hasOurs(doc->findBlockByNumber(n)))
            sanitize(doc, n); // anything left (a format TeamSpeak copied from ours)
        b = doc->findBlockByNumber(n);
    }
    batch.endEditBlock();
    return done;
}

int giveBack(QTextDocument* doc)
{
    QStringList names;
    const int   done = restoreAll(doc, &names);
    for (const QString& name : qAsConst(names))
        doc->addResource(QTextDocument::ImageResource, QUrl(name), QVariant());
    doc->addResource(QTextDocument::ImageResource, QUrl(blankName()), QVariant());
    return done;
}

bool resizeObject(QTextDocument* doc, int n, const QSizeF& size)
{
    const QTextBlock block = doc ? doc->findBlockByNumber(n) : QTextBlock();
    const Lead       lead  = leadOf(block);
    if (!lead.styled)
        return false;
    QTextImageFormat image = lead.format.toImageFormat();
    if (qFuzzyCompare(image.width(), size.width()) && qFuzzyCompare(image.height(), size.height()))
        return false;
    image.setWidth(size.width());
    image.setHeight(size.height());
    setCharFormatAt(doc, lead.object, lead.object + 1, image);
    return true;
}

// ============================================================================================
// Pictures
// ============================================================================================

static QImage renderImpl(const Env& env, const QTextBlock& block, const QDate& date, const Look& look, QString* signature, bool draw)
{
    const Lead lead = leadOf(block);
    if (!lead.styled)
        return {};
    const QTextImageFormat image = lead.format.toImageFormat();
    const QSize            size(qMax(1, qRound(image.width())), qMax(1, qRound(image.height())));
    if (image.name() == blankName() || image.width() < 1 || image.height() < 1)
        return {};
    const Row                row = read(block);
    const layoutart::Tokens& t   = env.tokens;
    QString                  sig = QString::number(lead.kind) + QLatin1Char('|') + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height())
                  + QLatin1Char('|') + QString::number(env.dpr) + QLatin1Char('|') + env.colors.base.name() + QLatin1Char('|') + t.body.toString();
    QImage out;
    switch (lead.kind) {
    case Head: {
        layoutart::HeadSpec spec = headSpecFor(env, block.blockNumber(), row, date);
        spec.avatar              = look.avatar;
        spec.replyAvatar         = look.replyAvatar;
        spec.replyHover          = look.replyHover;
        spec.replyPressed        = look.replyPressed;
        sig += QLatin1Char('|') + spec.nick + QLatin1Char('|') + spec.uid + QLatin1Char('|') + spec.nameColor.name() + QLatin1Char('|') + spec.timeLabel
               + QLatin1Char('|') + QString::number(spec.reply) + spec.replyHeader.nick + QLatin1Char('|') + spec.replyHeader.snippet + QString::number(spec.replyHeader.found) + spec.replyHeader.uid
               + spec.replyName.name() + QString::number(look.replyHover) + QString::number(look.replyPressed) + QString::number(look.avatar.cacheKey())
               + QString::number(look.replyAvatar.cacheKey()) + QString::number(isMention(block));
        if (draw) {
            layoutart::Colors colors = env.colors;
            if (isMention(block))
                colors.muted = colors.mutedOnMention; // the time on the highlighted row keeps 4.5:1
            out = layoutart::renderHead(spec, t, colors, size, env.dpr);
        }
        break;
    }
    case CompactHead: {
        const QColor  name = layoutart::nameColorFor(env.colors, row.nickColor, env.usualNickColor);
        const QString time = layoutart::shortTime(row.time, t.ampm || layoutart::hasAmPm(row.time));
        sig += QLatin1Char('|') + row.nick + name.name() + time + QString::number(row.reply);
        if (draw)
            out = layoutart::renderCompactHead(row.nick, name, time, t, env.colors, size, row.reply, env.dpr);
        break;
    }
    case SystemPrefix: {
        const QString time = env.mode == Mode::Compact ? layoutart::shortTime(row.time, t.ampm || layoutart::hasAmPm(row.time)) : QString();
        sig += QLatin1Char('|') + QString::number(static_cast<int>(row.kind)) + time + QString::number(static_cast<int>(env.mode));
        if (draw)
            out = layoutart::renderSystemPrefix(row.kind, time, t, env.colors, env.dpr);
        break;
    }
    case Divider: {
        // date: the day line's date as accepted (not after tomorrow, not before the divider above); a date
        // that isn't is shown as TeamSpeak wrote it, without "***".
        QString label;
        if (row.type == RowType::Day)
            label = date.isValid() ? layoutart::dayLabel(date, env.today) : row.dayText;
        else
            label = row.date.isValid() || row.dayText.isEmpty() ? layoutart::historyLabel(row.date) : row.dayText.left(120); // a marker without a date: its own words
        sig += QLatin1Char('|') + label;
        if (draw)
            out = layoutart::renderDivider(label, size.width(), t, env.colors, env.dpr);
        break;
    }
    default:
        return {};
    }
    if (signature)
        *signature = sig;
    return out;
}

QImage render(const Env& env, const QTextBlock& block, const QDate& date, const Look& look, QString* signature)
{
    return renderImpl(env, block, date, look, signature, true);
}

QString signature(const Env& env, const QTextBlock& block, const QDate& date, const Look& look)
{
    QString sig;
    renderImpl(env, block, date, look, &sig, false);
    return sig;
}

// ============================================================================================
// The reader's place
// ============================================================================================

Anchor anchorAt(QTextDocument* doc, qreal y)
{
    Anchor a;
    if (!doc)
        return a;
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const int                    hit    = layout->hitTest(QPointF(doc->documentMargin() + 1, y + 1), Qt::FuzzyHit);
    if (hit < 0)
        return a;
    QTextBlock block = doc->findBlock(hit);
    while (block.isValid() && !block.isVisible())
        block = block.next();
    if (!block.isValid())
        return a;
    const QRectF r = layout->blockBoundingRect(block);
    a.block        = block.blockNumber();
    a.offset       = y - r.top();
    a.height       = r.height();
    return a;
}

qreal anchorY(QTextDocument* doc, const Anchor& a)
{
    if (!doc || a.block < 0)
        return -1;
    const QTextBlock block = doc->findBlockByNumber(a.block);
    if (!block.isValid())
        return -1;
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    if (!block.isVisible()) {
        // Hidden since (Qt gives a hidden block no place: its top would read 0, the chat's top): the shown
        // row above it, which its run folded into.
        QTextBlock above = block.previous();
        while (above.isValid() && !above.isVisible())
            above = above.previous();
        if (above.isValid())
            return layout->blockBoundingRect(above).top();
        QTextBlock below = block.next();
        while (below.isValid() && !below.isVisible())
            below = below.next();
        return below.isValid() ? layout->blockBoundingRect(below).top() : -1;
    }
    const QRectF r      = layout->blockBoundingRect(block);
    qreal        offset = a.offset;
    if (a.height > 0 && r.height() > 0 && offset >= 0 && offset <= a.height)
        offset = offset * r.height() / a.height; // as far into it as before (the new look made it taller or shorter)
    return r.top() + offset;
}

// ============================================================================================
// Lines that just arrived
// ============================================================================================

QVector<int> arrivedNow(const QVector<QTextBlock>& lines, int nowSeconds)
{
    constexpr int kMaxLive    = 3;        // different lines in one turn
    constexpr int kMaxSkew    = 3 * 60;   // seconds between TeamSpeak's time of a line and now
    constexpr int kDaySeconds = 24 * 3600;
    QVector<int> blocks;
    QVector<int> out;
    for (const QTextBlock& b : lines) {
        if (!b.isValid() || blocks.contains(b.blockNumber()))
            continue;
        blocks.append(b.blockNumber());
        if (blocks.size() > kMaxLive)
            return {}; // history being loaded
        const int seconds = layoutart::secondsOf(read(b).time);
        if (seconds >= 0) {
            int skew = qAbs(seconds - nowSeconds) % kDaySeconds;
            skew     = qMin(skew, kDaySeconds - skew);
            if (skew > kMaxSkew)
                continue; // a line from the log
        }
        out.append(b.blockNumber());
    }
    return out;
}

// ============================================================================================
// Whole documents
// ============================================================================================

QDate acceptDate(const QDate& date, const QDate& previous, const QDate& today)
{
    if (!date.isValid())
        return {};
    if (today.isValid() && date > today.addDays(1))
        return {};
    if (previous.isValid() && date < previous)
        return {};
    return date;
}

void extendDates(QTextDocument* doc, const QDate& today, QVector<QDate>* dates, int from)
{
    if (!doc || !dates)
        return;
    from = qBound(0, from, dates->size());
    dates->resize(from);
    dates->reserve(doc->blockCount());
    // The day so far: dates change only at accepted dividers, so it is the last valid one before from.
    QDate current;
    for (int i = from - 1; i >= 0 && !current.isValid(); --i)
        current = dates->at(i);
    QDate lastDivider = current;
    for (QTextBlock b = doc->findBlockByNumber(from); b.isValid(); b = b.next()) {
        const QString text = b.text();
        // Cheap first: only lines that could be a day line are read.
        if (text.contains(QLatin1String("***")) || (text.size() == 1 && text.at(0) == QChar::ObjectReplacementCharacter)) {
            const Row row = read(b);
            // A day line, or a history marker with its date ("Chat begins 2026-10-10 04:44:02"): the day of
            // the lines after it.
            if (row.type == RowType::Day || (row.type == RowType::History && row.date.isValid())) {
                const QDate accepted = acceptDate(row.date, lastDivider, today);
                if (accepted.isValid()) {
                    current     = accepted;
                    lastDivider = accepted;
                }
                dates->append(accepted);
                continue;
            }
        }
        dates->append(current);
    }
}

QVector<QDate> datesOf(QTextDocument* doc, const QDate& today)
{
    QVector<QDate> out;
    extendDates(doc, today, &out, 0);
    return out;
}

Neighbour previousOf(const QTextBlock& block)
{
    Neighbour n;
    for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
        if (!b.isVisible())
            continue;
        n.row   = read(b);
        n.valid = true;
        break;
    }
    return n;
}

int styleAll(QTextDocument* doc, const Env& env, int firstSerial)
{
    if (!doc)
        return 0;
    const QVector<QDate> dates  = datesOf(doc, env.today);
    int                  done   = 0;
    int                  serial = firstSerial;
    QTextCursor          batch(doc);
    batch.beginEditBlock();
    for (int n = 0; n < doc->blockCount(); ++n) {
        QTextBlock b = doc->findBlockByNumber(n);
        sanitize(doc, n);
        b             = doc->findBlockByNumber(n);
        const Row row = read(b);
        if (row.styled || row.type == RowType::Unknown)
            continue;
        const Plan p = plan(env, b, row, previousOf(b), dates.value(n), 0, serial++);
        done += apply(doc, n, p, env) ? 1 : 0;
    }
    batch.endEditBlock();
    return done;
}

void renderAll(QTextDocument* doc, const Env& env, const QDate& today)
{
    Q_UNUSED(today);
    if (!doc)
        return;
    const QVector<QDate> dates = datesOf(doc, env.today);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const Lead lead = leadOf(b);
        if (!lead.styled)
            continue;
        const QImage image = render(env, b, dates.value(b.blockNumber()), Look(), nullptr);
        if (!image.isNull())
            doc->addResource(QTextDocument::ImageResource, QUrl(lead.format.toImageFormat().name()), QPixmap::fromImage(image));
    }
}

QColor usualNickColor(QTextDocument* doc, int maxBlocks)
{
    QHash<QRgb, int> counts;
    int              seen = 0;
    for (QTextBlock b = doc ? doc->lastBlock() : QTextBlock(); b.isValid() && seen < maxBlocks; b = b.previous(), ++seen) {
        const Row row = read(b);
        if (row.type == RowType::Message && row.nickColor.isValid())
            ++counts[row.nickColor.rgb()];
    }
    QRgb best  = 0;
    int  count = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > count) {
            best  = it.key();
            count = it.value();
        }
    }
    return count > 0 ? QColor(best) : QColor();
}

QColor usualLinkColor(QTextDocument* doc, int maxBlocks)
{
    QHash<QRgb, int> counts;
    int              seen = 0;
    for (QTextBlock b = doc ? doc->lastBlock() : QTextBlock(); b.isValid() && seen < maxBlocks; b = b.previous(), ++seen) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (!f.isAnchor() || f.isImageFormat() || isClientLink(f) || f.foreground().style() == Qt::NoBrush)
                continue;
            QTextCharFormat original = f;
            revertFormat(original); // TeamSpeak's colour, not ours
            if (original.foreground().style() != Qt::NoBrush)
                ++counts[original.foreground().color().rgb()];
        }
    }
    QRgb best  = 0;
    int  count = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > count) {
            best  = it.key();
            count = it.value();
        }
    }
    return count > 0 ? QColor(best) : QColor();
}

bool usesAmPm(QTextDocument* doc, int maxBlocks)
{
    int seen = 0;
    for (QTextBlock b = doc ? doc->lastBlock() : QTextBlock(); b.isValid() && seen < maxBlocks; b = b.previous(), ++seen) {
        const Row row = read(b);
        if ((row.type == RowType::Message || row.type == RowType::System) && !row.time.isEmpty())
            return layoutart::hasAmPm(row.time);
    }
    return false;
}

bool headGeometry(const Env& env, const QTextBlock& block, const QDate& date, layoutart::HeadGeometry* geometry, QSizeF* size)
{
    const Lead lead = leadOf(block);
    if (!lead.styled || lead.kind != Head)
        return false;
    const QTextImageFormat image = lead.format.toImageFormat();
    const QSize            s(qRound(image.width()), qRound(image.height()));
    const Row              row = read(block);
    if (geometry)
        *geometry = layoutart::headGeometry(headSpecFor(env, block.blockNumber(), row, date), env.tokens, s);
    if (size)
        *size = QSizeF(s);
    return true;
}

// ============================================================================================
// Collapsed runs of events (P2)
// ============================================================================================

namespace {

constexpr int kMinRun    = 4;   // different events in a run before it collapses
constexpr int kMaxRunWalk = 120; // blocks looked at around a change (a longer run is two)
constexpr int kChipGap   = 6;

QString chipName(const Env& env, int serial)
{
    return nameFor(env, "chip", serial);
}

} // namespace

bool collapsible(const Row& row)
{
    if (row.type != RowType::System || row.ownPrint || row.keepFormatting)
        return false;
    switch (row.kind) {
    case SystemKind::Poke:
    case SystemKind::Danger:
    case SystemKind::Announce:
    case SystemKind::Warning:
    case SystemKind::TsMedia:
        return false;
    default:
        return true;
    }
}

QString eventText(const QTextBlock& block)
{
    const Row row = read(block);
    if (row.type != RowType::System)
        return {};
    QString out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid() || f.position() + f.length() <= row.bodyStart || isOurs(f.charFormat()))
            continue;
        const int from = qMax(0, row.bodyStart - f.position());
        out += f.text().mid(from);
        if (out.size() > 600)
            break;
    }
    return out.trimmed();
}

QVector<ChipRef> chipsOf(const QTextBlock& block)
{
    QVector<ChipRef> out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment   f  = it.fragment();
        const QTextCharFormat cf = f.charFormat();
        if (!f.isValid() || kindOf(cf) != Chip || !cf.isImageFormat())
            continue;
        ChipRef c;
        c.position = f.position();
        c.text     = cf.stringProperty(kChip);
        c.name     = cf.toImageFormat().name();
        c.toggle   = cf.intProperty(kRunHead) != 0;
        out.append(c);
    }
    return out;
}

QVector<RunPlan> planRuns(QTextDocument* doc, int from, int to, const QSet<int>& expanded, int floor)
{
    QVector<RunPlan> runs;
    if (!doc)
        return runs;
    const int count = doc->blockCount();
    from            = qBound(floor, from, count - 1);
    to              = qBound(from, to, count - 1);
    // Whole runs: out to the first rows that can't be in one.
    const auto inRun = [doc](int n) { return collapsible(read(doc->findBlockByNumber(n))); };
    for (int walked = 0; from > floor && walked < kMaxRunWalk && inRun(from - 1); ++walked)
        --from;
    for (int walked = 0; to + 1 < count && walked < kMaxRunWalk && inRun(to + 1); ++walked)
        ++to;
    RunPlan current;
    const auto finish = [&](RunPlan& run) {
        if (run.rows.isEmpty())
            return;
        // Identical neighbours: the first of each group stays, with "×N".
        QVector<int>  heads;
        QVector<int>  sizes;
        QString       last;
        for (const int n : qAsConst(run.rows)) {
            const QString text = eventText(doc->findBlockByNumber(n));
            if (!heads.isEmpty() && text == last && !text.isEmpty()) {
                ++sizes.last();
                run.shown.insert(n, false);
                continue;
            }
            heads.append(n);
            sizes.append(1);
            last = text;
            run.shown.insert(n, true);
        }
        const QTextBlock first = doc->findBlockByNumber(run.rows.first());
        run.tag                = first.blockFormat().intProperty(kBlockTag);
        const bool longRun     = heads.size() >= kMinRun;
        const bool open        = longRun && run.tag > 0 && expanded.contains(run.tag);
        for (int i = 0; i < heads.size(); ++i) {
            if (sizes.at(i) > 1)
                run.chips[heads.at(i)].append(QString(QChar(0x00d7)) + QString::number(sizes.at(i)));
            if (longRun && !open && i > 0)
                run.shown.insert(heads.at(i), false);
        }
        if (longRun) {
            const int hidden = run.rows.size() - 1;
            if (open) {
                run.chips[heads.first()].append(i18n::t("Show fewer"));
            } else {
                run.chips[heads.first()] = QStringList{i18n::t("+%1 more events").arg(hidden)}; // one chip says it all
            }
            run.toggle.insert(heads.first(), true);
        }
        runs.append(run);
        run = RunPlan();
    };
    for (int n = from; n <= to; ++n) {
        if (inRun(n)) {
            current.rows.append(n);
            continue;
        }
        finish(current);
    }
    finish(current);
    return runs;
}

QSize chipPictureSize(const QString& text, const Env& env)
{
    const QSize chip = layoutart::chipSize(text, env.tokens);
    return QSize(chip.width() + kChipGap, chip.height());
}

QImage chipPicture(const QString& text, bool hovered, const Env& env, QSize* size)
{
    const QSize s = chipPictureSize(text, env);
    if (size)
        *size = s;
    QImage out(s * env.dpr, QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(env.dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawImage(QPointF(kChipGap, 0), layoutart::renderChip(text, hovered, env.tokens, env.colors, env.dpr));
    p.end();
    return out;
}

bool applyRun(QTextDocument* doc, const RunPlan& run, const Env& env, int* serial)
{
    bool changed = false;
    for (const int n : run.rows) {
        QTextBlock block = doc->findBlockByNumber(n);
        if (!block.isValid())
            continue;
        // Chips: what the row has against what it should have (removed and made anew when they differ).
        const QStringList want   = run.chips.value(n);
        const bool        toggle = run.toggle.value(n, false);
        QStringList       have;
        bool              haveToggle = false;
        for (const ChipRef& c : chipsOf(block)) {
            have << c.text;
            haveToggle = haveToggle || c.toggle;
        }
        // (joined: QList's operator== warns with this compiler's checked iterators)
        if (have.join(QChar(0x1f)) != want.join(QChar(0x1f)) || have.size() != want.size() || haveToggle != (toggle && !want.isEmpty())) {
            const QVector<ChipRef> old = chipsOf(block);
            QTextCursor         c(doc);
            for (int i = old.size() - 1; i >= 0; --i) {
                c.setPosition(old.at(i).position);
                c.setPosition(old.at(i).position + 1, QTextCursor::KeepAnchor);
                c.removeSelectedText();
                doc->addResource(QTextDocument::ImageResource, QUrl(old.at(i).name), QVariant());
            }
            block = doc->findBlockByNumber(n);
            for (int i = 0; i < want.size(); ++i) {
                const QString    name = chipName(env, (*serial)++);
                const QSize      size = chipPictureSize(want.at(i), env);
                QTextImageFormat image;
                image.setName(name);
                image.setWidth(size.width());
                image.setHeight(size.height());
                image.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                image.setProperty(kKind, static_cast<int>(Chip));
                image.setProperty(kChip, want.at(i));
                if (toggle && i == want.size() - 1)
                    image.setProperty(kRunHead, 1); // a click opens or closes the run
                doc->addResource(QTextDocument::ImageResource, QUrl(name), blankPicture());
                c.setPosition(block.position() + block.length() - 1);
                c.insertImage(image);
                block = doc->findBlockByNumber(n);
            }
            changed = true;
        }
        // The block format: kRunHead marks a row with chips (ChatLayout draws them); kCollapsed a hidden one.
        block                      = doc->findBlockByNumber(n);
        QTextBlockFormat bf        = block.blockFormat();
        const bool       hidden    = !run.shown.value(n, true);
        const bool       chips     = !want.isEmpty();
        const bool       wasHidden = bf.boolProperty(kCollapsed);
        if (bf.boolProperty(kRunHead) != chips || wasHidden != hidden) {
            if (chips)
                bf.setProperty(kRunHead, true);
            else
                bf.clearProperty(kRunHead);
            if (hidden)
                bf.setProperty(kCollapsed, true);
            else
                bf.clearProperty(kCollapsed);
            QTextCursor bc(block);
            bc.setBlockFormat(bf);
            changed = true;
        }
        block = doc->findBlockByNumber(n);
        if (block.isVisible() == hidden) {
            block.setVisible(!hidden);
            doc->markContentsDirty(block.position(), block.length());
            changed = true;
        }
    }
    return changed;
}

} // namespace layoutdoc
