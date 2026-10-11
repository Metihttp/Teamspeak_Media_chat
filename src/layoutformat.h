#pragma once

// Chat redesign: how TS Media's chat layout ("Cozy" and "Compact", layoutdoc.h) sits in TeamSpeak's chat
// document, for every module that reads a chat message (replies, albums, HD emoji, copying). Header only,
// QtGui.
//
// A styled message block starts with one picture of ours whose format keeps TeamSpeak's header as it was
// ("runs": the icon, "<16:00:17>", " ", the quoted nickname linked to client://..., ": "):
//   Cozy head          [Head][Separator][body...]               (Separator: U+2028 in a 1 px font)
//   Cozy reply head    [Head][R0][ChatReplies' separator][body] (R0: ChatReplies' picture, 0x0)
//   Cozy continuation  [Continuation][body]
//   Compact head       [CompactHead][body]
//   Compact reply      [R][ChatReplies' separator][CompactHead][body]
//   Compact continuation [Continuation][body]
// System rows start with a SystemPrefix picture (it keeps "[icon]<time> *** " or TS Media's own prefix)
// and may end with a Trailer (our time text, removed on restore); day and history lines are one Divider
// picture that keeps the whole line. Fragments we only recoloured keep their format before in kOrigChar
// and what we set in kApplied, so a restore only touches what still carries our values. Blocks are never
// added or removed (block numbers index replies, albums and emoji).

#include <QColor>
#include <QRegularExpression>
#include <QString>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextFragment>
#include <QVariant>
#include <QVariantList>

namespace layoutformat {

// Properties (other than the HD emoji's 0x7460-0x7463, the replies' 0x7470-0x7473, the previews' and albums'
// 0x7453-0x7456).
constexpr int kKind      = QTextFormat::UserProperty + 0x7480; // int: Kind (char formats) or RowKind (block format)
constexpr int kRuns      = QTextFormat::UserProperty + 0x7481; // QVariantList: text, QTextFormat, text, ... (TeamSpeak's form)
constexpr int kOrigBlock = QTextFormat::UserProperty + 0x7482; // QTextFormat: the block format before ours (stored once)
constexpr int kOrigChar  = QTextFormat::UserProperty + 0x7483; // QTextFormat: a recoloured fragment's format before
constexpr int kBlockTag  = QTextFormat::UserProperty + 0x7484; // int: a serial per styled block
constexpr int kTime      = QTextFormat::UserProperty + 0x7485; // QString: "16:00:17" or "4:00:17 PM"
constexpr int kCollapsed = QTextFormat::UserProperty + 0x7486; // bool, block format: hidden by a collapsed run
constexpr int kApplied   = QTextFormat::UserProperty + 0x7487; // QVariantMap / QTextFormat: the values we set
constexpr int kMention   = QTextFormat::UserProperty + 0x7488; // bool, block format: the message mentions you (P2)
constexpr int kRunHead   = QTextFormat::UserProperty + 0x7489; // bool, block format: the row has chips; int 1 on a chip: it opens or closes the run (P2)
constexpr int kChip      = QTextFormat::UserProperty + 0x748a; // QString, char format: a chip's text ("x3", "+5 more events"; P2)

// Our pictures and texts (char formats).
enum Kind { None = 0, Head = 1, Continuation = 2, CompactHead = 3, SystemPrefix = 4, Trailer = 5, Divider = 6, Separator = 7, Chip = 8 };
// What a styled block is (its block format's kKind).
enum RowKind { RowNone = 0, RowMessage = 1, RowSystem = 2, RowDivider = 3, RowOther = 4 };

// ChatReplies' reply line (replydoc.h): read here without depending on it.
constexpr int kReplyObjectProperty    = QTextFormat::UserProperty + 0x7470;
constexpr int kReplySeparatorProperty = QTextFormat::UserProperty + 0x7473;

inline QString objectPrefix()
{
    return QString::fromLatin1("tsmedia-layout:");
}

// True while ChatLayout edits a document: ChatIntegration's rescans and ChatEmoji's change tracking leave
// those edits alone (ChatLayout tells the others itself).
inline bool& mutatingFlag()
{
    static bool flag = false;
    return flag;
}
inline bool mutating()
{
    return mutatingFlag();
}

class MutatingScope
{
  public:
    MutatingScope()
        : m_was(mutatingFlag())
    {
        mutatingFlag() = true;
    }
    ~MutatingScope() { mutatingFlag() = m_was; }
    MutatingScope(const MutatingScope&)            = delete;
    MutatingScope& operator=(const MutatingScope&) = delete;

  private:
    bool m_was;
};

// One of our pictures or texts.
inline bool isOurs(const QTextCharFormat& format)
{
    return format.hasProperty(kKind);
}

inline Kind kindOf(const QTextCharFormat& format)
{
    return static_cast<Kind>(format.intProperty(kKind));
}

// One of our pictures (a head, a continuation, a system prefix, a divider, a chip).
inline bool isObject(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.hasProperty(kKind) && format.toImageFormat().name().startsWith(QLatin1String("tsmedia-layout:"));
}

inline QVariantList runsOf(const QTextCharFormat& format)
{
    return format.property(kRuns).toList();
}

// A link to a client (TeamSpeak's nicknames, a reply's author).
inline bool isClientHref(const QString& href)
{
    return href.startsWith(QLatin1String("client://"), Qt::CaseInsensitive);
}

inline bool isReplyObject(const QTextCharFormat& format)
{
    return format.isImageFormat() && format.hasProperty(kReplyObjectProperty) && format.toImageFormat().name().startsWith(QLatin1String("tsmedia-reply:"));
}

inline bool isReplySeparator(const QTextCharFormat& format)
{
    return format.boolProperty(kReplySeparatorProperty);
}

// The leading part of a styled block: our picture, ChatReplies' reply line and the line breaks after them.
struct Lead {
    bool    styled    = false; // our picture is at the block start (or right after a reply line)
    Kind    kind      = None;  // that picture's kind
    int     object    = -1;    // its position
    QTextCharFormat format;    // its format (runs, time)
    int     reply     = -1;    // ChatReplies' picture's position, -1 if none
    int     bodyStart = 0;     // the first position of TeamSpeak's own text after all of it
};

inline Lead leadOf(const QTextBlock& block)
{
    Lead      lead;
    const int bpos = block.position();
    lead.bodyStart = bpos;
    if (!block.isValid())
        return lead;
    int expected = bpos;
    int guard    = 0;
    for (auto it = block.begin(); !it.atEnd() && guard < 8; ++it, ++guard) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        if (f.position() != expected)
            break;
        const QTextCharFormat cf = f.charFormat();
        if (isObject(cf) && f.length() == 1 && lead.object < 0) {
            const Kind kind = kindOf(cf);
            if (kind != Head && kind != Continuation && kind != CompactHead && kind != SystemPrefix && kind != Divider)
                break;
            lead.styled = true;
            lead.kind   = kind;
            lead.object = f.position();
            lead.format = cf;
            expected    = f.position() + 1;
            continue;
        }
        if (isReplyObject(cf) && f.length() == 1 && lead.reply < 0) {
            lead.reply = f.position();
            expected   = f.position() + 1;
            continue;
        }
        // Line breaks of ours or of the reply line (one character each; a fragment may hold more text).
        if ((kindOf(cf) == Separator || isReplySeparator(cf)) && block.document()->characterAt(f.position()) == QChar::LineSeparator) {
            expected = f.position() + 1;
            if (f.length() > 1)
                break;
            continue;
        }
        break;
    }
    if (!lead.styled)
        return lead; // a reply line alone is ChatReplies' (replydoc.h reads it)
    lead.bodyStart = expected;
    return lead;
}

// Where TeamSpeak's own text of a block starts: after our head (and a reply line), or the block start.
inline int bodyStart(const QTextBlock& block)
{
    return leadOf(block).bodyStart;
}

// The text of runs (TeamSpeak's form: text, QTextFormat, ...).
inline QString runsText(const QVariantList& runs)
{
    QString out;
    for (int i = 0; i + 1 < runs.size(); i += 2)
        out += runs.at(i).toString();
    return out;
}

// TeamSpeak's message header, read back from a styled block.
struct Header {
    bool    valid = false;
    QString lead;      // the header's text before the nickname link (icon, "<time>", " "), a reply line's characters first
    QString nickText;  // "\"Nick\""
    QString nickHref;  // client://<clid>/<uid>~<nick>
    QColor  nickColor; // the link's colour (invalid: none set)
    QString after;     // after the link: ": "
    QString time;      // the text inside "<...>", empty without one
    QTextCharFormat baseFormat; // the header's plain text format (no picture, no link)
    int     bodyStart = 0;      // document position of the message text
    bool    replyLine = false;  // ChatReplies' picture in front
};

inline Header headerOf(const QTextBlock& block)
{
    Header     h;
    const Lead lead = leadOf(block);
    if (!lead.styled || (lead.kind != Head && lead.kind != Continuation && lead.kind != CompactHead))
        return h;
    const QVariantList runs = runsOf(lead.format);
    bool               nick = false;
    bool               base = false;
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QString         text   = runs.at(i).toString();
        const QTextCharFormat format = qvariant_cast<QTextFormat>(runs.at(i + 1)).toCharFormat();
        const bool            link   = format.isAnchor() && format.anchorHref().startsWith(QLatin1String("client://"), Qt::CaseInsensitive);
        if (link && (!nick || format.anchorHref() == h.nickHref) && h.after.isEmpty()) {
            nick = true;
            h.nickText += text;
            h.nickHref = format.anchorHref();
            if (format.foreground().style() != Qt::NoBrush)
                h.nickColor = format.foreground().color();
            continue;
        }
        if (!nick)
            h.lead += text;
        else
            h.after += text;
        if (!format.isImageFormat() && !link && !base) {
            h.baseFormat = format;
            base         = true;
        }
    }
    if (!nick)
        return h;
    static const QRegularExpression time(QStringLiteral("<([^<>\\n\\x{2028}\\x{2029}]{1,24})>"));
    const QRegularExpressionMatch   m = time.match(h.lead);
    if (m.hasMatch())
        h.time = m.captured(1);
    h.replyLine = lead.reply >= 0;
    if (h.replyLine)
        h.lead.prepend(QString(QChar::ObjectReplacementCharacter) + QChar(QChar::LineSeparator));
    h.bodyStart = lead.bodyStart;
    h.valid     = true;
    return h;
}

// What one of our pictures or texts gives as text when copied: a head, continuation, system prefix or
// divider gives TeamSpeak's text it took (pictures in it as nothing, TeamSpeak's emoticons through
// emoticonCode), a line break or trailer of ours nothing.
template <typename EmoticonCode>
QString copyText(const QTextCharFormat& format, EmoticonCode emoticonCode)
{
    const Kind kind = kindOf(format);
    if (kind == Separator || kind == Trailer || kind == Chip || kind == None)
        return {};
    QString            out;
    const QVariantList runs = runsOf(format);
    for (int i = 0; i + 1 < runs.size(); i += 2) {
        const QTextCharFormat f = qvariant_cast<QTextFormat>(runs.at(i + 1)).toCharFormat();
        if (f.isImageFormat()) {
            const QString code = emoticonCode(f.toImageFormat().name());
            const int     n    = qMax(1, static_cast<int>(runs.at(i).toString().count(QChar::ObjectReplacementCharacter)));
            for (int k = 0; k < n && !code.isEmpty(); ++k)
                out += code;
            continue;
        }
        QString text = runs.at(i).toString();
        text.remove(QChar::ObjectReplacementCharacter);
        out += text;
    }
    return out;
}

} // namespace layoutformat
