#include "darkchat.h"

#include <QPalette>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

#include "chatcolors.h"
#include "layoutformat.h"
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
};

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

// Whether the format's own colour is TeamSpeak's (or another plugin's, or a person's): no property of TS Media's modules,
// or only the chat layout's recolouring bookkeeping (kApplied, kOrigChar) on TeamSpeak's text without a colour of the
// layout's own among what it set (a link that only got the layout's underline colour keeps TeamSpeak's colour).
bool ownColorIsTheirs(const QTextFormat& format, const QMap<int, QVariant>& properties)
{
    bool applied = false;
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!isModuleProperty(it.key()) || it.key() == layoutformat::kOrigChar)
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

// The format's own colour, mapped by recolor()'s rules. False: unchanged.
bool mapOwn(QTextFormat& format, const ColorMap& map, bool forward)
{
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
    const auto known = map.exact.constFind(lookOf(f));
    if (known == map.exact.constEnd())
        return false;
    format.setForeground(QColor::fromRgba(known.value()));
    return true;
}

bool mapFormat(QTextFormat& format, const ColorMap& map, bool forward, int depth);

// The formats a module keeps in the format's properties (the chat layout's header runs and recoloured fragments' formats
// before, an HD emoji's text format, a text spoiler's hidden runs, a reply's quote line, a poll card's text): mapped like
// a fragment of their own, so whatever a module gives back later (another chat layout, unload) has the colours of the
// side dark mode is on, and the way back stays exact (kOriginal inside them). False: unchanged.
bool mapKept(QTextFormat& format, const ColorMap& map, bool forward, int depth, const QMap<int, QVariant>& properties)
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
            if (mapFormat(inner, map, forward, depth + 1)) {
                format.setProperty(it.key(), QVariant::fromValue<QTextFormat>(inner));
                changed = true;
            }
        } else if (value.userType() == QMetaType::QVariantList) {
            QVariantList list        = value.toList();
            bool         listChanged = false;
            for (QVariant& item : list) {
                if (item.userType() != QMetaType::QTextFormat)
                    continue;
                QTextFormat inner = qvariant_cast<QTextFormat>(item);
                if (mapFormat(inner, map, forward, depth + 1)) {
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

bool mapFormat(QTextFormat& format, const ColorMap& map, bool forward, int depth)
{
    const QMap<int, QVariant> properties = format.properties();
    bool                      changed    = mapKept(format, map, forward, depth, properties);
    if (ownColorIsTheirs(format, properties))
        changed = mapOwn(format, map, forward) || changed;
    return changed;
}

} // namespace

uint qHash(const Look& look, uint seed)
{
    return ::qHash(look.rgb, seed) ^ (look.bold ? 0x1000u : 0u) ^ (look.underline ? 0x2000u : 0u);
}

Look lookOf(const QTextCharFormat& format)
{
    return {format.foreground().color().rgba(), format.fontWeight() > QFont::Normal, format.fontUnderline()};
}

QVector<QPair<QByteArray, Look>> classLooks(const QString& css, const QColor& plainLink)
{
    const QString                    sheet = QString::fromLatin1("a { color: #010203; }\n") + css;
    QVector<QPair<QByteArray, Look>> out;
    for (const Probe& probe : kProbes) {
        QTextDocument doc;
        doc.setUndoRedoEnabled(false);
        doc.setDefaultStyleSheet(sheet);
        doc.setHtml(QString::fromLatin1(probe.html));
        for (auto it = doc.begin().begin(); !it.atEnd(); ++it) {
            const QTextCharFormat f = it.fragment().charFormat();
            if (!solidForeground(f))
                continue;
            Look look = lookOf(f);
            if (look.rgb == kSentinel) {
                if (!plainLink.isValid())
                    break;
                look.rgb = plainLink.rgba();
            }
            out.append({QByteArray(probe.key), look});
            break;
        }
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

ColorMap buildMap(const QVector<QPair<QByteArray, Look>>& from, const QVector<QPair<QByteArray, Look>>& to, const QVector<QPair<Look, QRgb>>& extras,
                  const QColor& base)
{
    ColorMap map;
    map.base = base;
    QHash<QByteArray, Look> target;
    for (const auto& t : to)
        target.insert(t.first, t.second);
    // The info panel's classes come after the extras (live test): they never occur in a chat, while TS Media's own lines
    // do. Under TeamSpeak's own dark chat sheet (Windows in dark mode) the info title link and TS Media's line text share
    // the dark text look, and the line text came back light steel blue on the white chat (1.8:1).
    const auto infoPanel = [](const QByteArray& key) {
        return key == "InfoTitleLink" || key == "Highlight" || key == "Important" || key == "Active" || key == "Inactive";
    };
    const auto classes = [&](bool info) {
        for (const auto& f : from) {
            if (infoPanel(f.first) != info || !target.contains(f.first) || map.exact.contains(f.second))
                continue;
            const QRgb want = target.value(f.first).rgb;
            if (want != f.second.rgb)
                map.exact.insert(f.second, want);
        }
    };
    classes(false);
    for (const auto& e : extras) {
        if (!map.exact.contains(e.first) && e.first.rgb != e.second)
            map.exact.insert(e.first, e.second);
    }
    classes(true);
    return map;
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

int recolor(QTextDocument* doc, const ColorMap& map, bool forward, int from, int to)
{
    if (!doc)
        return 0;
    const int end   = to < 0 ? doc->characterCount() : qMin(to, doc->characterCount());
    const int start = qBound(0, from, end);
    if (start >= end && to >= 0)
        return 0;
    QVector<Range> ranges;
    bool           kept = false;
    for (QTextBlock b = start > 0 ? doc->findBlock(start) : doc->begin(); b.isValid() && b.position() < end; b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.position() + fragment.length() <= start)
                continue;
            if (fragment.position() >= end)
                break;
            QTextFormat               format     = fragment.charFormat();
            const QMap<int, QVariant> properties = format.properties();
            const bool                keptNow    = mapKept(format, map, forward, 0, properties);
            const bool                own        = ownColorIsTheirs(format, properties) && mapOwn(format, map, forward);
            if (!keptNow && !own)
                continue;
            kept = kept || keptNow;
            ranges.append({fragment.position(), fragment.position() + fragment.length(), format.toCharFormat()});
        }
    }
    if (kept)
        ++g_keptSerial;
    if (ranges.isEmpty())
        return 0;
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (const Range& r : qAsConst(ranges)) {
        cursor.setPosition(r.from);
        cursor.setPosition(r.to, QTextCursor::KeepAnchor);
        cursor.setCharFormat(r.format);
    }
    cursor.endEditBlock();
    return ranges.size();
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
            map.exact.insert(Look{QColor(Qt::blue).rgba(), bold, underline}, link.rgba());
            map.exact.insert(Look{QColor(Qt::red).rgba(), bold, underline}, error.rgba());
            map.exact.insert(Look{QColor(Qt::darkRed).rgba(), bold, underline}, error.rgba());
            map.exact.insert(Look{QColor(0x8b, 0x00, 0x00).rgba(), bold, underline}, error.rgba()); // CSS darkred
        }
    }
    return map;
}

} // namespace darkchat
