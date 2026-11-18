#include "darkchat.h"

#include <QPalette>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

#include <algorithm>

#include "chatcolors.h"
#include "layoutformat.h"
#include "polldoc.h"        // 2.7.1 review: the poll card's line break (bookkeeping)
#include "replydoc.h"       // 2.7.1 live test: the reply line's line break (bookkeeping)
#include "textspoilerdoc.h" // 2.7.1 review: a text spoiler's revealed text (bookkeeping)
#include "uiutil.h"

// 2.7 Dark mode (design_dark 1.5), proven by the prototype's recolor / classLooks / buildMap.

namespace darkchat {

namespace {

// A colour no skin uses: a link that comes out of a probe in it has no colour from the sheet (Qt gives such a link the
// application palette's link colour at insertion, which the caller passes as plainLink).
constexpr QRgb kSentinel = 0xff010203u;

// TS Media's modules keep their properties in 0x7450-0x74ff of a chat document's formats.
constexpr int kModuleFirst = QTextFormat::UserProperty + 0x7450;
constexpr int kModuleLast  = QTextFormat::UserProperty + 0x74ff;
// Formats kept inside kept formats (an HD emoji's text format inside a header run): this deep at most.
constexpr int kMaxDepth = 4;

quint64 g_keptSerial = 0; // keptSerial(): GUI thread only

struct Probe {
    const char* key;
    const char* html;
};

// TeamSpeak's chat classes first, then the info panel's (design_dark 1.5's priority list).
const Probe kProbes[] = {
    {"a", "<p><a href=\"https://example.com\">x</a></p>"},
    {"UserLink", "<p class=\"TextMessage_Message\"><a class=\"TextMessage_UserLink\" href=\"client://0/x\">x</a></p>"},
    {"ChannelLink", "<p class=\"TextMessage_Normal\"><a class=\"TextMessage_ChannelLink\" href=\"channelid://0\">x</a></p>"},
    {"ServerLink", "<p class=\"TextMessage_Connected\"><a class=\"TextMessage_ServerLink\" href=\"channelid://0\">x</a></p>"},
    {"OfflineLink", "<p class=\"TextMessage_Normal\"><a class=\"TextMessage_OfflineLink\" href=\"x\">x</a></p>"},
    {"Time", "<p class=\"TextMessage_Message\"><span class=\"TextMessage_Time\">x</span></p>"},
    {"Normal", "<p class=\"TextMessage_Normal\">x</p>"},
    {"Welcome", "<p class=\"TextMessage_Welcome\">x</p>"},
    {"Poke", "<p class=\"TextMessage_Poke\">x</p>"},
    {"Kicked", "<p class=\"TextMessage_ClientKicked\">x</p>"},
    {"ServerError", "<p class=\"TextMessage_ServerError\">x</p>"},
    {"History", "<p class=\"TextMessage_BeginHistory\">x</p>"},
    {"HostName", "<p><span class=\"TextMessage_HostName\">x</span></p>"},
    {"InfoTitleLink", "<p class=\"InfoFrame_Title\"><a class=\"TextMessage_ServerLink\" href=\"x\">x</a></p>"},
    {"Highlight", "<p class=\"Highlight\">x</p>"},
    {"Important", "<p class=\"Important\">x</p>"},
    {"Active", "<p class=\"Active\">x</p>"},
    {"Inactive", "<p class=\"Inactive\">x</p>"},
    // 2.7.2 (D9): the info panel's labels and custom nickname, in the context TeamSpeak's templates put them in, and its
    // title's own text (the spaces and pictures around the name: its size, no colour).
    {"Label", "<table class=\"InfoFrame_Table\"><tr><td class=\"Label\">x</td></tr></table>"},
    {"CustomNick", "<p class=\"InfoFrame_Title\"><span class=\"InfoFrame_CustomNick\">x</span></p>"},
    {"Title", "<p class=\"InfoFrame_Title\">x</p>"},
    // The bottom-aligned cells of TeamSpeak's client panel (the first row: its label and its value).
    {"BottomLabel", "<table class=\"InfoFrame_Table\"><tr><td class=\"Bottom Label\">x</td></tr></table>"},
    {"Bottom", "<table class=\"InfoFrame_Table\"><tr><td class=\"Bottom\">x</td></tr></table>"},
};

// The info panel's classes (they never occur in a chat). Label, CustomNick, Title, BottomLabel and Bottom (2.7.2) take part in
// the info panel's maps only.
bool isInfoOnly(const QByteArray& key)
{
    return key == "Label" || key == "CustomNick" || key == "Title" || key == "BottomLabel" || key == "Bottom";
}

bool isInfoClass(const QByteArray& key)
{
    return key == "InfoTitleLink" || key == "Highlight" || key == "Important" || key == "Active" || key == "Inactive" || isInfoOnly(key);
}

// 2.7.2 (D5): the properties of a class's text style: everything but its colour (mapped by look), the link's own
// properties, objects and the properties of TS Media or other plugins.
bool isStyleProperty(int id)
{
    return id != QTextFormat::ForegroundBrush && id != QTextFormat::IsAnchor && id != QTextFormat::AnchorHref && id != QTextFormat::AnchorName
           && id != QTextFormat::ObjectType && id != QTextFormat::ObjectIndex && id < QTextFormat::UserProperty;
}

QVariant valueOf(const QTextFormat& format, int id)
{
    return format.hasProperty(id) ? format.property(id) : QVariant();
}

// The text style of a class on the way back (map.fonts): each property whose value is one dark mode's sheet gave that look,
// except the properties in skip (id, value, id, value, ...: what the chat layout set itself). False: unchanged.
bool applyStyle(QTextFormat& format, const QVector<StyleFix>& style, const QVariantList& skip = {})
{
    bool changed = false;
    for (const StyleFix& fix : style) {
        bool layouts = false;
        for (int i = 0; i + 1 < skip.size(); i += 2)
            layouts = layouts || skip.at(i).toInt() == fix.id;
        const QVariant now = valueOf(format, fix.id);
        if (layouts || now == fix.value || !fix.from.contains(now))
            continue;
        if (fix.value.isValid())
            format.setProperty(fix.id, fix.value);
        else
            format.clearProperty(fix.id);
        changed = true;
    }
    return changed;
}

bool solidForeground(const QTextCharFormat& f)
{
    return f.hasProperty(QTextFormat::ForegroundBrush) && f.foreground().style() == Qt::SolidPattern;
}

struct Range {
    int             from;
    int             to;
    QTextCharFormat format; // the fragment's format as it will be
};

bool isModuleProperty(int id)
{
    return id >= kModuleFirst && id <= kModuleLast;
}

// What a module keeps on TeamSpeak's own text without making its colour the module's: the chat layout's format before, and
// (2.7.1 live test) an album's collapsed item link (its text and line breaks kept as strings) and the reply line's line break
// after its picture (the header's format). Recoloured both ways like any of TeamSpeak's text: a link folded while dark mode
// was on came back in dark mode's colour when the album unfolded at the unload. 2.7.1 review: likewise the poll card's line
// break (the message's format) and a text spoiler's text fragments, revealed (the text's format with the tint) or a link's
// space in a hidden one (the link's format without the link), each with its format before (kBefore, mapped as a kept format,
// what the spoiler gives back): a link revealed while dark mode was on kept dark mode's colour after off, one revealed before
// kept the skin's on the dark chat. The spoiler's objects (words, markers) are objects: never recoloured.
bool isBookkeeping(int id, const QVariant& value)
{
    if (id == textspoilerdoc::kKind)
        return value.toInt() == textspoilerdoc::Revealed || value.toInt() == textspoilerdoc::Space;
    return id == layoutformat::kOrigChar || id == layoutformat::kCollapsedLabel || id == layoutformat::kCollapsedSeparator
           || id == replydoc::kSeparatorProperty || id == polldoc::kSeparatorProperty || id == textspoilerdoc::kId
           || id == textspoilerdoc::kBefore;
}

// Whether the format's own colour is TeamSpeak's (or another plugin's, or a person's): no property of TS Media's modules,
// or only the chat layout's recolouring bookkeeping (kApplied, isBookkeeping) on TeamSpeak's text without a colour of the
// layout's own among what it set (a link that only got the layout's underline colour keeps TeamSpeak's colour).
bool ownColorIsTheirs(const QTextFormat& format, const QMap<int, QVariant>& properties)
{
    bool applied = false;
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!isModuleProperty(it.key()) || isBookkeeping(it.key(), it.value()))
            continue;
        if (it.key() != layoutformat::kApplied)
            return false;
        applied = true;
    }
    if (applied) {
        const QVariantList set = format.property(layoutformat::kApplied).toList();
        for (int i = 0; i + 1 < set.size(); i += 2) {
            if (set.at(i).toInt() == QTextFormat::ForegroundBrush)
                return false;
        }
    }
    return true;
}

// The format's own colour, mapped by recolor()'s rules. label: the format is TS Media's line label (the way back looks it up
// in map.label first). False: unchanged.
bool mapOwn(QTextFormat& format, const ColorMap& map, bool forward, bool label)
{
    if (!forward && !map.fonts.isEmpty() && format.isCharFormat() && !format.hasProperty(QTextFormat::ForegroundBrush) && !format.hasProperty(kOriginal)
        && (format.isImageFormat() || format.objectType() == QTextFormat::NoObject)) {
        // 2.7.2 (D5): text without a colour of its own (and a picture beside it): the text style of its uncoloured look where
        // the map knows one (the info panel's title: its size).
        const auto style = map.fonts.constFind(uncoloredLook(format.toCharFormat()));
        return style != map.fonts.constEnd() && applyStyle(format, style.value());
    }
    if (!format.isCharFormat() || format.isImageFormat() || format.objectType() != QTextFormat::NoObject)
        return false;
    if (!format.hasProperty(QTextFormat::ForegroundBrush) || format.brushProperty(QTextFormat::ForegroundBrush).style() != Qt::SolidPattern)
        return false;
    const QTextCharFormat f = format.toCharFormat();
    if (forward) {
        if (format.hasProperty(kOriginal))
            return false;
        const QColor before = f.foreground().color();
        const auto   known  = map.exact.constFind(lookOf(f));
        QColor       now;
        if (known != map.exact.constEnd())
            now = QColor::fromRgba(known.value());
        else if (map.base.isValid() && ui::contrastRatio(ui::flatten(before, map.base), map.base) < map.minimum)
            now = chatcolors::readable(QColor(before.rgb()), map.base, map.minimum);
        else
            return false;
        format.setForeground(now);
        format.setProperty(kOriginal, uint(before.rgba()));
        return true;
    }
    if (format.hasProperty(kOriginal)) {
        format.setForeground(QColor::fromRgba(format.property(kOriginal).toUInt()));
        format.clearProperty(kOriginal);
        return true;
    }
    const Look look = lookOf(f);
    if (label) { // 2.7.1: TS Media's label (with Same as the chat text it has the look of its text, which exact gives the text)
        const auto own = map.label.constFind(look);
        if (own != map.label.constEnd()) {
            format.setForeground(QColor::fromRgba(own.value()));
            return true;
        }
    }
    bool       changed = false;
    const auto known   = map.exact.constFind(look);
    if (known != map.exact.constEnd()) {
        format.setForeground(QColor::fromRgba(known.value()));
        changed = true;
    } else if (map.uncolored.contains(look)) { // 2.7.2 (D9): an info panel class the skin doesn't colour
        format.clearProperty(QTextFormat::ForegroundBrush);
        changed = true;
    }
    // 2.7.2 (D5): the class's text style as the skin has it (lines added while dark have dark mode's sheet's).
    const auto style = map.fonts.constFind(look);
    if (style != map.fonts.constEnd())
        changed = applyStyle(format, style.value()) || changed;
    return changed;
}

bool mapFormat(QTextFormat& format, const ColorMap& map, bool forward, int depth, bool label, bool* keptChanged = nullptr);

// The formats a module keeps in the format's properties (the chat layout's header runs and recoloured fragments' formats
// before, an HD emoji's text format, a text spoiler's hidden runs, a reply's quote line, a poll card's text): mapped like
// a fragment of their own, so whatever a module gives back later (another chat layout, unload) has the colours of the
// side dark mode is on, and the way back stays exact (kOriginal inside them). A format kept on its own is the fragment's
// text (label: the fragment's); in a list of runs (text, format, text, format, ...) a format is TS Media's label when the
// text before it is (2.7.1: the chat layout's prefix picture keeps the label that way). False: unchanged.
bool mapKept(QTextFormat& format, const ColorMap& map, bool forward, int depth, const QMap<int, QVariant>& properties, bool label)
{
    if (depth >= kMaxDepth)
        return false;
    bool changed = false;
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!isModuleProperty(it.key()))
            continue;
        const QVariant& value = it.value();
        if (value.userType() == QMetaType::QTextFormat) {
            QTextFormat inner = qvariant_cast<QTextFormat>(value);
            if (mapFormat(inner, map, forward, depth + 1, label)) {
                format.setProperty(it.key(), QVariant::fromValue<QTextFormat>(inner));
                changed = true;
            }
        } else if (value.userType() == QMetaType::QVariantList) {
            QVariantList list        = value.toList();
            bool         listChanged = false;
            bool         runLabel    = false; // the text item before the next format is TS Media's label
            for (QVariant& item : list) {
                if (item.userType() == QMetaType::QString) {
                    runLabel = isLineLabel(item.toString());
                    continue;
                }
                if (item.userType() != QMetaType::QTextFormat) {
                    runLabel = false;
                    continue;
                }
                QTextFormat inner   = qvariant_cast<QTextFormat>(item);
                const bool  isLabel = runLabel;
                runLabel            = false;
                if (mapFormat(inner, map, forward, depth + 1, isLabel)) {
                    item        = QVariant::fromValue<QTextFormat>(inner);
                    listChanged = true;
                }
            }
            if (listChanged) {
                format.setProperty(it.key(), list);
                changed = true;
            }
        }
    }
    return changed;
}

bool mapFormat(QTextFormat& format, const ColorMap& map, bool forward, int depth, bool label, bool* keptChanged)
{
    const QMap<int, QVariant> properties = format.properties();
    const bool                theirs     = ownColorIsTheirs(format, properties);
    // 2.7.2 (D5): a fragment whose colour the chat layout set (Cozy and Compact recolour names and channels): the text style
    // of the look its format before had (read before mapKept maps it), on the properties the layout didn't set itself (a
    // mention pill's weight stays the layout's). Not for a format before that dark mode recoloured (kOriginal: that one is
    // given back exactly).
    QVariantList applied;
    Look         before;
    bool         layoutStyle = false;
    if (!forward && !theirs && properties.contains(layoutformat::kApplied) && properties.contains(layoutformat::kOrigChar)) {
        applied           = properties.value(layoutformat::kApplied).toList();
        bool layoutColour = false;
        for (int i = 0; i + 1 < applied.size(); i += 2)
            layoutColour = layoutColour || applied.at(i).toInt() == QTextFormat::ForegroundBrush;
        const QTextFormat original = qvariant_cast<QTextFormat>(properties.value(layoutformat::kOrigChar));
        if (layoutColour && original.isCharFormat() && !original.hasProperty(kOriginal) && solidForeground(original.toCharFormat())) {
            before      = lookOf(original.toCharFormat());
            layoutStyle = !(label && map.label.contains(before));
        }
    }
    const bool kept    = mapKept(format, map, forward, depth, properties, label);
    bool       changed = kept;
    if (keptChanged)
        *keptChanged = kept;
    if (theirs) {
        changed = mapOwn(format, map, forward, label) || changed;
    } else if (layoutStyle) {
        const auto style = map.fonts.constFind(before);
        if (style != map.fonts.constEnd())
            changed = applyStyle(format, style.value(), applied) || changed;
    }
    if (!forward && format.hasProperty(kOriginal)) {
        // 2.7.2 (D6): kOriginal never survives the way back. It is copied wherever a recoloured fragment's format is copied:
        // ChatEmoji's picture (merge, chatemoji.cpp), the chat layout's recoloured fragment (withChanges), a block separator
        // (QTextEdit::append). A picture or object gets its colour back with it (never drawn: as if copied from TeamSpeak's
        // colour), so does TeamSpeak's own text; a colour a module set stays the module's (the layout gives its format
        // before back from kOrigChar, mapped above).
        if (theirs || format.isImageFormat() || format.objectType() != QTextFormat::NoObject)
            format.setForeground(QColor::fromRgba(format.property(kOriginal).toUInt()));
        format.clearProperty(kOriginal);
        changed = true;
    }
    return changed;
}

} // namespace

uint qHash(const Look& look, uint seed)
{
    return ::qHash(look.rgb, seed) ^ (look.bold ? 0x1000u : 0u) ^ (look.underline ? 0x2000u : 0u) ^ (look.anchor ? 0x4000u : 0u);
}

Look lookOf(const QTextCharFormat& format)
{
    return {format.foreground().color().rgba(), format.fontWeight() > QFont::Normal, format.fontUnderline(), format.isAnchor()};
}

Look uncoloredLook(const QTextCharFormat& format)
{
    return {0u, format.fontWeight() > QFont::Normal, format.fontUnderline(), format.isAnchor()}; // fully transparent: no text's colour
}

QVector<ClassFormat> classFormats(const QString& css, const QColor& plainLink)
{
    const QString        sheet = QString::fromLatin1("a { color: #010203; }\n") + css;
    const QString        text  = QString::fromLatin1("x");
    QVector<ClassFormat> out;
    for (const Probe& probe : kProbes) {
        QTextDocument doc;
        doc.setUndoRedoEnabled(false);
        doc.setDefaultStyleSheet(sheet);
        doc.setHtml(QString::fromLatin1(probe.html));
        // The probe's text (2.7.2: in any block: the label is in a table cell).
        bool found = false;
        for (QTextBlock b = doc.begin(); b.isValid() && !found; b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                if (it.fragment().text() != text)
                    continue;
                found = true;
                ClassFormat c;
                c.key    = QByteArray(probe.key);
                c.format = it.fragment().charFormat();
                if (solidForeground(c.format)) {
                    if (c.format.foreground().color().rgba() == kSentinel) {
                        if (!plainLink.isValid())
                            break; // a link without its colour: not known
                        c.format.setForeground(plainLink);
                    }
                    c.colored = true;
                    c.look    = lookOf(c.format);
                }
                out.append(c);
                break;
            }
        }
    }
    return out;
}

QVector<QPair<QByteArray, Look>> classLooks(const QString& css, const QColor& plainLink)
{
    QVector<QPair<QByteArray, Look>> out;
    for (const ClassFormat& c : classFormats(css, plainLink)) {
        if (c.colored)
            out.append({c.key, c.look});
    }
    return out;
}

QString resolvePaletteRefs(const QString& css, const QPalette& palette)
{
    // The role names of Qt's style sheets.
    struct Role {
        const char*         name;
        QPalette::ColorRole role;
    };
    static const Role kRoles[] = {{"alternate-base", QPalette::AlternateBase}, {"base", QPalette::Base}, {"bright-text", QPalette::BrightText},
                                  {"button", QPalette::Button}, {"button-text", QPalette::ButtonText}, {"dark", QPalette::Dark},
                                  {"highlight", QPalette::Highlight}, {"highlighted-text", QPalette::HighlightedText}, {"light", QPalette::Light},
                                  {"link", QPalette::Link}, {"link-visited", QPalette::LinkVisited}, {"mid", QPalette::Mid},
                                  {"midlight", QPalette::Midlight}, {"shadow", QPalette::Shadow}, {"text", QPalette::Text},
                                  {"window", QPalette::Window}, {"window-text", QPalette::WindowText}};
    if (!css.contains(QLatin1String("palette("), Qt::CaseInsensitive))
        return css;
    const QRegularExpression ref(QString::fromLatin1("palette\\(\\s*([a-zA-Z-]+)\\s*\\)"), QRegularExpression::CaseInsensitiveOption);
    QString                  out;
    int                      last = 0;
    auto                     it   = ref.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m    = it.next();
        const QString                 name = m.captured(1).toLower();
        QString                       value;
        for (const Role& r : kRoles) {
            if (name == QLatin1String(r.name))
                value = palette.color(QPalette::Active, r.role).name();
        }
        if (value.isEmpty())
            continue; // a name Qt doesn't know: left as it is
        out += css.midRef(last, m.capturedStart() - last);
        out += value;
        last = m.capturedEnd();
    }
    out += css.midRef(last);
    return out;
}

bool isLineLabel(const QString& text)
{
    const QLatin1String label("TS Media chat"); // ts3api's printInfo, and printWarning's with " " U+26A0 (layoutdoc's own prints)
    if (text.size() == label.size())
        return text == label;
    return text.size() == label.size() + 2 && text.startsWith(label) && text.at(label.size()) == QLatin1Char(' ') && text.at(label.size() + 1) == QChar(0x26a0);
}

namespace {

ColorMap buildMapOf(const QVector<ClassFormat>& from, const QVector<ClassFormat>& to, const QVector<QPair<Look, QRgb>>& extras, const QColor& base,
                    const QVector<QPair<Look, QRgb>>& labels, bool infoPanel, bool withStyle)
{
    ColorMap map;
    map.base = base;
    for (const auto& l : labels) {
        if (!map.label.contains(l.first) && l.first.rgb != l.second)
            map.label.insert(l.first, l.second);
    }
    QHash<QByteArray, const ClassFormat*> target;
    for (const ClassFormat& t : to) {
        if (!target.contains(t.key))
            target.insert(t.key, &t);
    }
    // A chat's map leaves the info panel's Label and CustomNick out (2.7.2: its maps are exactly 2.7.1's).
    const auto takesPart = [infoPanel](const ClassFormat& f) { return f.colored && (infoPanel || !isInfoOnly(f.key)); };
    QSet<Look> won; // the looks a class won (the extras' carry no text style: ts3api prints its [b] in both looks)
    const auto classes = [&](bool info) {
        for (const ClassFormat& f : from) {
            if (!takesPart(f) || isInfoClass(f.key) != info || map.exact.contains(f.look) || map.uncolored.contains(f.look))
                continue;
            const ClassFormat* t = target.value(f.key);
            if (!t)
                continue;
            if (t->colored) {
                if (t->look.rgb != f.look.rgb) {
                    map.exact.insert(f.look, t->look.rgb);
                    won.insert(f.look);
                }
            } else if (infoPanel && info) {
                // 2.7.2 (D9): an info panel class the target doesn't colour (TeamSpeak's labels): its colour goes. Never a
                // chat class (a host name the skin doesn't colour inherits its line's colour: no colour would be wrong).
                map.uncolored.insert(f.look);
                won.insert(f.look);
            }
        }
    };
    const auto addExtras = [&] {
        for (const auto& e : extras) {
            if (!map.exact.contains(e.first) && !map.uncolored.contains(e.first) && e.first.rgb != e.second)
                map.exact.insert(e.first, e.second);
        }
    };
    if (infoPanel) {
        // 2.7.2 (D9): TeamSpeak's info panel: its own classes win their looks first (under dark mode its labels look like a
        // host name, which won that look before: the labels came back in the host name's colour).
        classes(true);
        classes(false);
        addExtras();
    } else {
        // The info panel's classes come after the extras (live test): they never occur in a chat, while TS Media's own lines
        // do. Under TeamSpeak's own dark chat sheet (Windows in dark mode) the info title link and TS Media's line text shared
        // the dark text look, and the line text came back light steel blue on the white chat (1.8:1). (2.7.1: the title is a
        // link and TS Media's text never is, so their looks differ now; the order stays.)
        classes(false);
        addExtras();
        classes(true);
    }
    if (!withStyle)
        return map;
    // 2.7.2 (D5): each won look's text style: a property where every class of that look has the same value on the target
    // side (none counts as a value) and some has another one on this side. A property the classes disagree on is left
    // alone: no class's style goes on another class's text. The classes are those whose text the map's documents hold: a
    // chat's, the chat classes (an info panel class sharing a look, e.g. Active with Welcome, never occurs in a chat); the
    // info panel's, all of them.
    using Sharing     = QVector<QPair<const ClassFormat*, const ClassFormat*>>;
    const auto styleOf = [](const Sharing& sharing) {
        QSet<int> ids;
        for (const auto& s : sharing) {
            for (int id : s.first->format.properties().keys())
                ids.insert(id);
            for (int id : s.second->format.properties().keys())
                ids.insert(id);
        }
        QList<int> sorted = ids.values();
        std::sort(sorted.begin(), sorted.end());
        QVector<StyleFix> style;
        for (int id : qAsConst(sorted)) {
            if (!isStyleProperty(id))
                continue;
            StyleFix fix;
            fix.id       = id;
            fix.value    = valueOf(sharing.first().second->format, id);
            bool agree   = true;
            bool differs = false;
            for (const auto& s : sharing) {
                if (valueOf(s.second->format, id) != fix.value) {
                    agree = false;
                    break;
                }
                const QVariant mine = valueOf(s.first->format, id);
                differs             = differs || mine != fix.value;
                if (!fix.from.contains(mine))
                    fix.from.append(mine);
            }
            if (agree && differs)
                style.append(fix);
        }
        return style;
    };
    for (const Look& look : qAsConst(won)) {
        Sharing sharing;
        bool    known = true;
        for (const ClassFormat& f : from) {
            if (!takesPart(f) || !(f.look == look) || (!infoPanel && isInfoClass(f.key)))
                continue;
            const ClassFormat* t = target.value(f.key);
            if (!t) {
                known = false;
                break;
            }
            sharing.append({&f, t});
        }
        if (!known || sharing.isEmpty())
            continue;
        const QVector<StyleFix> style = styleOf(sharing);
        if (!style.isEmpty())
            map.fonts.insert(look, style);
    }
    if (infoPanel) {
        // The info panel's title text, which neither side colours (its spaces and pictures around the name: dark mode's
        // sheet makes it x-large as TeamSpeak's does; a skin without that size has it back).
        QHash<Look, Sharing> plain;
        for (const ClassFormat& f : from) {
            const ClassFormat* t = target.value(f.key);
            if (isInfoOnly(f.key) && !f.colored && t && !t->colored)
                plain[uncoloredLook(f.format)].append({&f, t});
        }
        for (auto it = plain.constBegin(); it != plain.constEnd(); ++it) {
            const QVector<StyleFix> style = styleOf(it.value());
            if (!style.isEmpty())
                map.fonts.insert(it.key(), style);
        }
    }
    return map;
}

} // namespace

ColorMap buildMap(const QVector<QPair<QByteArray, Look>>& from, const QVector<QPair<QByteArray, Look>>& to, const QVector<QPair<Look, QRgb>>& extras,
                  const QColor& base, const QVector<QPair<Look, QRgb>>& labels)
{
    const auto formats = [](const QVector<QPair<QByteArray, Look>>& looks) {
        QVector<ClassFormat> out;
        for (const auto& l : looks) {
            ClassFormat c;
            c.key     = l.first;
            c.colored = true;
            c.look    = l.second;
            out.append(c);
        }
        return out;
    };
    return buildMapOf(formats(from), formats(to), extras, base, labels, false, false);
}

ColorMap buildMap(const QVector<ClassFormat>& from, const QVector<ClassFormat>& to, const QVector<QPair<Look, QRgb>>& extras, const QColor& base,
                  const QVector<QPair<Look, QRgb>>& labels, bool infoPanel)
{
    return buildMapOf(from, to, extras, base, labels, infoPanel, true);
}

bool isModuleFormat(const QTextCharFormat& format)
{
    const QMap<int, QVariant> properties = format.properties();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const int id = it.key();
        if (id >= QTextFormat::UserProperty + 0x7450 && id <= QTextFormat::UserProperty + 0x74ff)
            return true;
    }
    return false;
}

namespace {

// kOriginal in format or in any format kept inside it (2.7.2).
bool holdsOriginal(const QTextFormat& format, int depth)
{
    if (format.hasProperty(kOriginal))
        return true;
    if (depth >= kMaxDepth)
        return false;
    const QMap<int, QVariant> properties = format.properties();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (it.value().userType() == QMetaType::QTextFormat) {
            if (holdsOriginal(qvariant_cast<QTextFormat>(it.value()), depth + 1))
                return true;
        } else if (it.value().userType() == QMetaType::QVariantList) {
            for (const QVariant& item : it.value().toList()) {
                if (item.userType() == QMetaType::QTextFormat && holdsOriginal(qvariant_cast<QTextFormat>(item), depth + 1))
                    return true;
            }
        }
    }
    return false;
}

} // namespace

int recolor(QTextDocument* doc, const ColorMap& map, bool forward, int from, int to)
{
    if (!doc)
        return 0;
    const int end   = to < 0 ? doc->characterCount() : qMin(to, doc->characterCount());
    const int start = qBound(0, from, end);
    if (start >= end && to >= 0)
        return 0;
    QVector<Range> ranges;
    QVector<Range> blocks; // 2.7.2 (D6): block character formats (from: the block's position)
    bool           kept = false;
    for (QTextBlock b = start > 0 ? doc->findBlock(start) : doc->begin(); b.isValid() && b.position() < end; b = b.next()) {
        if (!forward) {
            // The block's own character format (its separator's): QTextEdit::append gives each new block its cursor's format,
            // a recoloured fragment's when the cursor sits on one (TeamSpeak's at the end): one that holds kOriginal is mapped
            // like a fragment, so no block keeps it or dark mode's colour (and no later append copies it from there). The
            // others are left as they are (never drawn; an old line's stays byte-exact), and so is every one on the way there.
            QTextFormat format = b.charFormat();
            if (holdsOriginal(format, 0) && mapFormat(format, map, false, 0, false))
                blocks.append({b.position(), b.position(), format.toCharFormat()});
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.position() + fragment.length() <= start)
                continue;
            if (fragment.position() >= end)
                break;
            QTextFormat               format     = fragment.charFormat();
            const QMap<int, QVariant> properties = format.properties();
            // 2.7.1: TS Media's label (13 or 15 characters in bold: only then is the fragment's text read).
            const bool label = (fragment.length() == 13 || fragment.length() == 15) && format.toCharFormat().fontWeight() > QFont::Normal
                               && isLineLabel(fragment.text());
            bool       keptNow = false;
            const bool changed = mapFormat(format, map, forward, 0, label, &keptNow);
            if (!changed)
                continue;
            kept = kept || keptNow;
            ranges.append({fragment.position(), fragment.position() + fragment.length(), format.toCharFormat()});
        }
    }
    if (kept)
        ++g_keptSerial;
    if (ranges.isEmpty() && blocks.isEmpty())
        return 0;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (const Range& r : qAsConst(blocks)) {
        cursor.setPosition(r.from);
        cursor.setBlockCharFormat(r.format);
    }
    for (const Range& r : qAsConst(ranges)) {
        cursor.setPosition(r.from);
        cursor.setPosition(r.to, QTextCursor::KeepAnchor);
        cursor.setCharFormat(r.format);
    }
    cursor.endEditBlock();
    return ranges.size() + blocks.size();
}

bool mapBack(QTextCharFormat& format, const ColorMap& map)
{
    QTextFormat f = format;
    if (!mapFormat(f, map, false, 0, false))
        return false;
    format = f.toCharFormat();
    return true;
}

int taggedCount(QTextDocument* doc)
{
    int count = 0;
    if (!doc)
        return 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it)
            count += it.fragment().charFormat().hasProperty(kOriginal) ? 1 : 0;
    }
    return count;
}

quint64 keptSerial()
{
    return g_keptSerial;
}

ColorMap tipMap(const QColor& link, const QColor& error, const QColor& tipBase)
{
    ColorMap map;
    map.base = tipBase;
    for (const bool bold : {false, true}) {
        for (const bool underline : {false, true}) {
            for (const bool anchor : {false, true}) { // 2.7.1: in a link or not
                map.exact.insert(Look{QColor(Qt::blue).rgba(), bold, underline, anchor}, link.rgba());
                map.exact.insert(Look{QColor(Qt::red).rgba(), bold, underline, anchor}, error.rgba());
                map.exact.insert(Look{QColor(Qt::darkRed).rgba(), bold, underline, anchor}, error.rgba());
                map.exact.insert(Look{QColor(0x8b, 0x00, 0x00).rgba(), bold, underline, anchor}, error.rgba()); // CSS darkred
            }
        }
    }
    return map;
}

} // namespace darkchat
