#include "darkmode.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionTabWidgetFrame>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>

#include "chatscroll.h"
#include "darkglyphs.h"
#include "darktitlebar.h"
#include "i18n.h"
#include "layoutformat.h"
#include "settings.h"
#include "ts3api.h"

// 2.7 Dark mode (design_dark 1). Names compared with QLatin1String; every string built at run time (CRASH RULE).

namespace {

bool      g_active   = false; // DarkMode::isActive(): written on the GUI thread only
DarkMode* g_instance = nullptr;
int       g_changing = 0; // 2.7.2 (D7): DarkMode::changing() (GUI thread)

// Review fix (2.7.2, D3): TeamSpeak's chat tab widget class (ChatIntegration and ChatZoom know it by it too): the pane rule's
// selector at a start with dark mode on, before TeamSpeak has built a chat.
const char kChatTabsClass[] = "ChatTabWidget";

// 2.7.3 (M7): a style file larger than this isn't read (TeamSpeak's are a few KB).
constexpr qint64 kMostStyleBytes = 1 << 20;

// 2.7.2 (D7): the look is switching for the scope's life (it ends with themeChanged).
struct Changing {
    Changing() { ++g_changing; }
    ~Changing() { --g_changing; }
    Changing(const Changing&)            = delete;
    Changing& operator=(const Changing&) = delete;
};

bool named(const QObject* o, const char* name)
{
    return o->objectName() == QLatin1String(name);
}

// TeamSpeak's chat (a QTextBrowser) and its info panel (a QTextEdit), by the object names its skins style. Live test: also
// the poke window's text (PokeDialog's msgTextBrowser), whose nickname TeamSpeak writes in blue (1.8:1 on the dark field).
bool isThemedView(const QObject* o)
{
    return (named(o, "ChatTab") || named(o, "InfoFrame") || named(o, "msgTextBrowser")) && qobject_cast<const QTextEdit*>(o);
}

bool isTipLabel(const QObject* o)
{
    return named(o, "qtooltip_label");
}

// A window with a title bar of its own (no popup, tool tip or frameless window), created, and not one that draws its own
// dark caption (the media viewer, the send window).
bool takesTitleBar(const QWidget* w)
{
    if (!w || !w->isWindow() || !w->testAttribute(Qt::WA_WState_Created) || !w->internalWinId())
        return false;
    if (w->windowFlags() & Qt::FramelessWindowHint)
        return false;
    const Qt::WindowType type = w->windowType();
    if (type != Qt::Window && type != Qt::Dialog && type != Qt::Sheet && type != Qt::Drawer && type != Qt::Tool)
        return false;
    return !w->property(darktitlebar::kOwnCaption).toBool();
}

darktitlebar::Colors barColors()
{
    const darkpalette::Tokens& t = darkpalette::tokens();
    return {t.panel, t.text, t.border};
}

void logLine(const QString& line, LogLevel level = LogLevel_INFO)
{
    ts3::log(line, level);
}

} // namespace

DarkMode::DarkMode(QObject* parent)
    : QObject(parent)
{
    g_instance = this;
    m_clock.start();
    const auto timer = [this] {
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(0);
        return t;
    };
    m_adopt = timer();
    m_slice = timer();
    m_fresh = timer();
    m_tip   = timer();
    m_repane = timer();
    connect(m_adopt, &QTimer::timeout, this, &DarkMode::adoptSkin);
    connect(m_repane, &QTimer::timeout, this, &DarkMode::repane);
    connect(m_slice, &QTimer::timeout, this, &DarkMode::recolorPending);
    connect(m_fresh, &QTimer::timeout, this, &DarkMode::recolorFresh);
    connect(m_tip, &QTimer::timeout, this, &DarkMode::recolorTips);
    if (QCoreApplication* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, [this] { m_quitting = true; });
}

DarkMode::~DarkMode()
{
    shutdown();
    if (g_instance == this)
        g_instance = nullptr;
}

bool DarkMode::isActive()
{
    return g_active;
}

DarkMode* DarkMode::instance()
{
    return g_instance;
}

bool DarkMode::changing()
{
    return g_changing > 0;
}

void DarkMode::setGlyphBase(const QString& dir)
{
    m_glyphBase    = dir;
    m_glyphBaseSet = true;
}

void DarkMode::setDeferHiddenPlaces(bool on)
{
    m_deferHidden = on;
}

void DarkMode::setEnabled(bool on)
{
    if (m_shutDown || on == m_enabled)
        return;
    m_enabled = on;
    if (on)
        apply();
    else
        remove();
    emit enabledChanged(on);
}

// ---- the theme ---------------------------------------------------------------------------------------------------------

void DarkMode::buildTheme()
{
    const darkpalette::Tokens& t = darkpalette::tokens();
    m_k                          = darkpalette::scaleFactor(QApplication::font());
    const QString base           = m_glyphBaseSet ? m_glyphBase : ts3::dataDir();
    m_glyphDir                   = base.isEmpty() ? QString() : darkglyphs::ensure(base, m_k, t);
    if (m_glyphDir.isEmpty() && !base.isEmpty() && !m_glyphsLogged) {
        m_glyphsLogged = true;
        logLine(QString::fromLatin1("[dark] glyphs unavailable: the folder %1/darkmode can't be written").arg(base), LogLevel_WARNING);
    }
    m_sheet     = sheetNow();
    m_palette   = darkpalette::palette(m_snap.palette);
    m_darkSheets.clear(); // 2.7.3 (M9): each document's dark sheet, made when it is tracked (darkSheetFor)
    m_darkClasses.clear();
    m_tipMap    = darkchat::tipMap(t.link, t.error, t.raised);

    // TS Media's own lines (design_dark 1.5): the colours they were printed in on the skin's chat, and under dark mode.
    // Every choice of Settings -> Chat -> Text size and colors, not only the one in use now (review fix): a line printed
    // after the choice changed while dark mode was on maps back too. The one in use goes first (it wins a look two
    // choices would share).
    // 2.7.1: exactly the shapes ts3api prints: the label "TS Media chat" (bold, never a link), a link (underlined: dark mode's
    // chat sheet keeps the underline of links; a skin's may drop it, so both on the way there), the text (normal, or bold in
    // /tsmedia help and the chat layout's notice). Under dark mode the text has the chat text colour, which is also the
    // label's with Same as the chat text and a nickname's: on the way back the text wins its look among TS Media's shapes,
    // links are TeamSpeak's (the anchor in the look), and the label has a map of its own (darkchat::isLineLabel).
    chatcolors::Colors dark;
    dark.base        = t.window;
    dark.text        = t.text;
    dark.link        = t.link;
    dark.dark        = true;
    dark.themeAccent = t.accent;
    m_extrasForward.clear();
    m_extrasBack.clear();
    m_labelsBack.clear();
    struct Shape {
        bool bold, underline, anchor;
    };
    const Shape label{true, false, false}, text{false, false, false}, boldText{true, false, false}, link{false, true, true}, plainLink{false, false, true};
    const auto  add = [](QVector<QPair<darkchat::Look, QRgb>>& list, const QString& from, const QString& to, const Shape& s) {
        const QColor a(from), b(to);
        if (a.isValid() && b.isValid() && a != b)
            list.append({darkchat::Look{a.rgba(), s.bold, s.underline, s.anchor}, b.rgba()});
    };
    QStringList choices{Settings::normalizeLineColor(Settings::instance().chatLineColor), QString()};
    for (const char* name : Settings::lineColorChoices)
        choices << QString::fromLatin1(name);
    choices.removeDuplicates();
    // The text and the warning don't depend on the choice.
    const chatcolors::LineColors skin = chatcolors::lineColors(m_snap.chat, choices.first());
    const chatcolors::LineColors ours = chatcolors::lineColors(dark, choices.first());
    // Live test (2.7.0): on the skin's chat TeamSpeak draws a plugin line without a colour of ours in dark blue (chatcolors),
    // so a line printed while dark gets that back where TS Media doesn't colour its text there (the way there stays the
    // contrast rule's then).
    const QString textBack = skin.wrapBody ? skin.body : QString::fromLatin1("#00008b");
    for (const Shape& s : {text, boldText}) {
        add(m_extrasBack, ours.body, textBack, s);
        if (skin.wrapBody)
            add(m_extrasForward, skin.body, ours.body, s);
    }
    add(m_extrasBack, ours.warning, skin.warning, label);
    add(m_labelsBack, ours.warning, skin.warning, label);
    add(m_extrasForward, skin.warning, ours.warning, label);
    for (const QString& choice : qAsConst(choices)) {
        const chatcolors::LineColors before = chatcolors::lineColors(m_snap.chat, choice);
        const chatcolors::LineColors after  = chatcolors::lineColors(dark, choice);
        for (const Shape& s : {link, label})
            add(m_extrasBack, after.accent, before.accent, s);
        for (const Shape& s : {link, plainLink, label})
            add(m_extrasForward, before.accent, after.accent, s);
        add(m_labelsBack, after.accent, before.accent, label);
    }
    m_maps.clear();
    m_infoMaps.clear();
}

QString DarkMode::sheetNow() const
{
    QString sheet = darkpalette::appSheet(darkpalette::tokens(), m_k, m_snap.metrics, m_glyphDir);
    // The skin's own fonts after ours (review fix): the dark sheet has none, and Qt gives a widget only the fonts of the
    // sheet in use, so without them TeamSpeak's headlines and banners (and a custom skin's client or chat font) would
    // change while dark mode is on.
    const QString fonts = darkpalette::fontRules(m_snap.sheet);
    if (!fonts.isEmpty())
        sheet += QString::fromLatin1("\n/* the skin's fonts */\n") + fonts;
    return sheet;
}

QString DarkMode::darkSheetFor(const QString& skin)
{
    const auto known = m_darkSheets.constFind(skin);
    if (known != m_darkSheets.constEnd())
        return known.value();
    // 2.7.3 (M9): the style's own spacing in dark mode's sheet (its palette(...) as the lines got it: the skin's palette).
    const QString dark = darkpalette::chatSheet(darkpalette::tokens(), darkchat::resolvePaletteRefs(skin, m_snap.palette));
    m_darkSheets.insert(skin, dark);
    return dark;
}

const QVector<darkchat::ClassFormat>& DarkMode::darkClassesOf(const QString& dark)
{
    auto known = m_darkClasses.find(dark);
    if (known == m_darkClasses.end())
        known = m_darkClasses.insert(dark, darkchat::classFormats(dark, darkpalette::tokens().link));
    return known.value();
}

QString DarkMode::chatSheetFor(QTextDocument* doc) const
{
    const auto it = m_docs.constFind(doc);
    return it == m_docs.constEnd() ? QString() : it->darkSheet;
}

DarkMode::Maps DarkMode::mapsFor(const QString& skin, const QString& dark, bool info)
{
    const QHash<QString, Maps>& cache = (info ? m_infoMaps : m_maps)[dark];
    const auto                  known = cache.constFind(skin);
    if (known != cache.constEnd())
        return known.value();
    const darkpalette::Tokens& t = darkpalette::tokens();
    // The skin's sheet as it was when its lines came in: its palette(link) is the skin's link, not ours.
    const auto  from    = darkchat::classFormats(darkchat::resolvePaletteRefs(skin, m_snap.palette), m_snap.palette.color(QPalette::Active, QPalette::Link));
    const auto& classes = darkClassesOf(dark);
    // 2.7.2 (D9): the chats' maps and the info panel's (its classes first, a label without a colour in the skin loses dark
    // mode's), both made at once from the same class formats.
    for (const bool panel : {false, true}) {
        Maps maps;
        maps.forward = darkchat::buildMap(from, classes, m_extrasForward, t.window, {}, panel);
        maps.back    = darkchat::buildMap(classes, from, m_extrasBack, QColor(), m_labelsBack, panel);
        (panel ? m_infoMaps : m_maps)[dark].insert(skin, maps);
    }
    return (info ? m_infoMaps : m_maps)[dark].value(skin);
}

bool DarkMode::teamSpeakDark(const QPalette& palette)
{
    return palette.brush(QPalette::Current, QPalette::WindowText).color().value() > palette.brush(QPalette::Current, QPalette::Window).color().value();
}

// 2.7.2 (live test), 2.7.3 (M7): TeamSpeak's dark-mode files, from its install folder's styles and the config folder's (a
// custom style's), in the compositions TeamSpeak's loaders make with them. Read when dark mode goes on and when it takes over
// a skin (a new style may bring its own).
void DarkMode::readTeamSpeakDarkFiles()
{
    m_chatPairs.clear();
    m_appPairs.clear();
    QStringList folders{QCoreApplication::applicationDirPath() + QString::fromLatin1("/styles")};
    const QString config = ts3::configDir();
    if (!config.isEmpty())
        folders << config + QString::fromLatin1("/styles");
    // The chat loader reads its files as bytes (QFile::readAll, UTF-8, CRLF kept); the application loader as text
    // (QTextStream, UTF-8, the file opened in text mode: CRLF read as LF).
    const auto bytesOf = [](const QString& path, QByteArray* out) {
        QFile file(path);
        if (!file.exists() || file.size() > kMostStyleBytes || !file.open(QIODevice::ReadOnly))
            return false;
        *out = file.readAll();
        return true;
    };
    const auto textOf = [](const QString& path, QString* out) {
        QFile file(path);
        if (!file.exists() || file.size() > kMostStyleBytes || !file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        *out = stream.readAll();
        return true;
    };
    const QString chatDarkSuffix = QString::fromLatin1("_chat_darkmode.qss");
    const QString winDarkSuffix  = QString::fromLatin1("_win_darkmode.qss");
    const QString darkSuffix     = QString::fromLatin1("_darkmode.qss");
    for (int f = 0; f < folders.size(); ++f) {
        const QDir dir(folders.at(f));
        QStringList styles; // those with an application dark file of their own
        for (const QString& name : dir.entryList({QString::fromLatin1("*") + darkSuffix}, QDir::Files, QDir::Name)) {
            if (name.endsWith(chatDarkSuffix, Qt::CaseInsensitive)) {
                // A chat: <style>_chat.qss (its folder's, else the other one's) + "\n" + this file, at the very end.
                const QString style = name.left(name.size() - chatDarkSuffix.size());
                QByteArray    own, dark;
                bool          found = false;
                for (int g = 0; g < folders.size() && !found; ++g) {
                    const QString chat = QDir(folders.at(g == 0 ? f : (g == f ? 0 : g))).filePath(style + QString::fromLatin1("_chat.qss"));
                    found              = bytesOf(chat, &own);
                }
                if (!found || !bytesOf(dir.filePath(name), &dark))
                    continue;
                Composition pair;
                pair.own  = QString::fromUtf8(own);
                pair.made = pair.own + QString::fromUtf8(QByteArray("\n").append(dark));
                bool known = false;
                for (const Composition& c : qAsConst(m_chatPairs))
                    known = known || c.made == pair.made;
                if (!known)
                    m_chatPairs.append(pair);
                continue;
            }
            styles << name.left(name.size() - darkSuffix.size());
            if (name.endsWith(winDarkSuffix, Qt::CaseInsensitive))
                styles << name.left(name.size() - winDarkSuffix.size());
        }
        styles.removeDuplicates();
        for (const QString& style : qAsConst(styles)) {
            const auto part = [&dir, &style](const char* suffix) { return dir.filePath(style + QString::fromLatin1(suffix)); };
            Composition app;
            if (!textOf(part(".qss"), &app.own))
                continue;
            app.made = app.own;
            QString piece;
            if (textOf(part("_darkmode.qss"), &piece))
                app.made += QLatin1Char('\n') + piece;
            if (textOf(part("_win.qss"), &piece)) {
                app.made += QLatin1Char('\n') + piece;
                app.own += QLatin1Char('\n') + piece;
            }
            if (textOf(part("_win_darkmode.qss"), &piece))
                app.made += QLatin1Char('\n') + piece;
            if (textOf(part("_vista.qss"), &piece)) { // Windows Vista or later
                app.made += QLatin1Char('\n') + piece;
                app.own += QLatin1Char('\n') + piece;
            }
            if (app.made != app.own)
                m_appPairs.append(app);
        }
    }
}

// Only TeamSpeak's own composition, compared whole: a style whose own files contain a dark-mode file's text keeps it.
QString DarkMode::ownChatSheet(const QString& sheet) const
{
    for (const Composition& c : m_chatPairs) {
        if (c.made == sheet)
            return c.own;
    }
    return sheet;
}

QString DarkMode::ownAppSheet(const QString& sheet) const
{
    for (const Composition& c : m_appPairs) {
        if (c.made == sheet)
            return c.own;
    }
    return sheet;
}

darkchat::ColorMap DarkMode::ownSheetMap(const QString& made, const QString& own, bool info) const
{
    const QColor link = m_snap.palette.color(QPalette::Active, QPalette::Link);
    const auto   from = darkchat::classFormats(darkchat::resolvePaletteRefs(made, m_snap.palette), link);
    const auto   to   = darkchat::classFormats(darkchat::resolvePaletteRefs(own, m_snap.palette), link);
    return darkchat::buildMap(from, to, {}, QColor(), {}, info, true); // review fix: a class coloured alike gets its text style
}

// 2.7.3 (M8; review fix): [*from, *to) after a change at position (removed characters replaced by added ones): an edit
// before it moves it, one inside it resizes it, a removal reaching past its end ends it there (one over all of it empties
// it). A change of as many characters as it removes is a format change (dark mode's and the chat layout's are), unless
// someone else (foreign) replaced the text up to its end with text as long: an info panel rendered again, which holds
// nothing from before then. Live test fix (2.7.3): our own change over its end or over all of it is the chat layout styling
// its lines in one edit (a Cozy or Compact row's picture with the time and the name in place of TeamSpeak's text, also in
// lines after it): it covers the new text (it emptied, and the first line of a chat made while dark mode was on kept
// TeamSpeak's dark chat colours after off). A line after it in that text came in while dark mode was on and has the style's
// look by then (the way back); the map from the sheet the range's lines came in under finds none of that sheet's own looks
// there unless the two sheets share them.
static void followEdit(int* from, int* to, int position, int removed, int added, bool foreign)
{
    if (*to <= *from || position >= *to)
        return; // empty, or the edit is after it
    const int delta = added - removed;
    if (position + removed <= *from) { // before it
        *from += delta;
        *to += delta;
        return;
    }
    if (delta == 0) {
        if (foreign && removed > 0 && position + removed >= *to)
            *to = qMax(*from, position);
        return;
    }
    if (position + removed <= *to) { // inside it (its first characters may have gone)
        *from = qMin(*from, position);
        *to   = qMax(*from, *to + delta);
    } else if (!foreign) { // ours over its end or all of it: the new text
        *from = qMin(*from, position);
        *to   = qMax(*to + delta, position + added);
    } else if (position <= *from) { // all of it removed
        *from = position;
        *to   = position;
    } else { // its end removed
        *to = position;
    }
}

// 2.7.2 (D3): the chat's tab widget (its nearest QTabWidget ancestor), or none.
static QTabWidget* chatTabsOf(QWidget* chat)
{
    QTabWidget* tabs = nullptr;
    for (QWidget* w = chat ? chat->parentWidget() : nullptr; w && !tabs; w = w->parentWidget())
        tabs = qobject_cast<QTabWidget*>(w);
    return tabs;
}

// 2.7.2 (D3): what names tabs alone in a sheet: its class (TeamSpeak's ChatTabWidget), else QTabWidget#name. Empty: tabs on
// the left or right (their bar takes the side), or a plain QTabWidget without a name (a rule would reach every tab widget).
static QString paneSelectorOf(const QTabWidget* tabs)
{
    if (!tabs || tabs->tabPosition() == QTabWidget::West || tabs->tabPosition() == QTabWidget::East)
        return QString();
    const auto plain = [](const QString& name) {
        for (const QChar c : name) {
            if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('_') && c != QLatin1Char('-'))
                return false;
        }
        return !name.isEmpty() && !name.at(0).isDigit();
    };
    const QString cls = QString::fromLatin1(tabs->metaObject()->className()).replace(QString::fromLatin1("::"), QString::fromLatin1("--"));
    if (cls != QLatin1String("QTabWidget") && plain(cls))
        return cls;
    if (plain(tabs->objectName()))
        return QString::fromLatin1("QTabWidget#") + tabs->objectName();
    return QString();
}

// 2.7.2 (D3): the side insets of tabs' pane in the look in place now (the skin's: apply measures before its sheet,
// adoptSkin after TeamSpeak's): a shown tab widget's page geometry, else what its style answers for it. False: none.
// 2.7.3 (M6): and the pages' distance from the top and the bottom, the tab bar not counted (-1: tabs on a side).
static bool paneInsets(QTabWidget* tabs, int* left, int* right, int* top = nullptr, int* bottom = nullptr)
{
    auto* stack = tabs ? tabs->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
    if (!stack)
        return false;
    tabs->ensurePolished();
    QRect outer, inner, barRect;
    if (tabs->isVisible()) {
        outer = tabs->rect();
        inner = stack->geometry(); // QTabWidget lays a shown one out at once on a new style (its StyleChange)
        if (QTabBar* tabBar = tabs->tabBar(); tabBar && tabBar->isVisible())
            barRect = tabBar->geometry();
    } else {
        // Not shown yet (TeamSpeak's start with dark mode on): the style's own answer, with the frame QTabWidget gives it.
        QStyleOptionTabWidgetFrame opt;
        opt.initFrom(tabs);
        opt.rect      = QRect(0, 0, qMax(400, tabs->width()), qMax(300, tabs->height()));
        opt.lineWidth = tabs->documentMode() ? 0 : tabs->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, tabs);
        opt.shape     = tabs->tabPosition() == QTabWidget::South ? (tabs->tabShape() == QTabWidget::Rounded ? QTabBar::RoundedSouth : QTabBar::TriangularSouth)
                                                                 : (tabs->tabShape() == QTabWidget::Rounded ? QTabBar::RoundedNorth : QTabBar::TriangularNorth);
        if (QTabBar* bar = tabs->tabBar(); bar && bar->count() > 0)
            opt.tabBarSize = bar->sizeHint();
        outer = opt.rect;
        inner = tabs->style()->subElementRect(QStyle::SE_TabWidgetTabContents, &opt, tabs);
        if (!tabs->tabBar()->isHidden() && tabs->count() > 0)
            barRect = tabs->style()->subElementRect(QStyle::SE_TabWidgetTabBar, &opt, tabs);
    }
    if (!inner.isValid() || inner.width() <= 0)
        return false;
    *left  = qBound(0, inner.left() - outer.left(), 64);
    *right = qBound(0, outer.right() - inner.right(), 64);
    if (top && bottom) {
        const QTabWidget::TabPosition position = tabs->tabPosition();
        *top = *bottom = -1;
        if (position == QTabWidget::North || position == QTabWidget::South) {
            const bool south = position == QTabWidget::South;
            const int  upper = !barRect.isEmpty() && !south ? barRect.bottom() + 1 : outer.top();
            const int  lower = !barRect.isEmpty() && south ? barRect.top() - 1 : outer.bottom();
            *top             = qBound(0, inner.top() - upper, 64);
            *bottom          = qBound(0, lower - inner.bottom(), 64);
        }
    }
    return true;
}

// 2.7.2 (D3): the chat's tab widget pane, named and measured; nothing for one no selector can name alone.
static void measurePane(QTextEdit* chat, darkpalette::Metrics* m)
{
    QTabWidget*   tabs     = chatTabsOf(chat);
    const QString selector = paneSelectorOf(tabs);
    int           left = 0, right = 0, top = -1, bottom = -1;
    if (selector.isEmpty() || !paneInsets(tabs, &left, &right, &top, &bottom))
        return;
    m->paneSelector = selector;
    m->paneLeft     = left;
    m->paneRight    = right;
    m->paneTop      = top < 0 ? -1 : top;
    m->paneBottom   = top < 0 ? -1 : bottom;
}

// ---- 2.7.3 (M6): the heights of TeamSpeak's main window bars -------------------------------------------------------------
// Dark mode's bars took vertical paddings scaled with the font (design_dark 2.4), TeamSpeak's grow with the font only: the
// chat was 18 px shorter at 100 %, 60-73 px at 200 %. The dark sheet gives each bar the skin's height (barRules), measured
// here in the look in place (the skin's: apply measures before its sheet, adoptSkin after TeamSpeak's), from size hints and
// the menu bar's item rectangles (right after a style change too; no layout needed).

// What a tool button's size hint holds inside the style's frame (QToolButton::sizeHint's height): its icon, text or both.
static int toolContent(const QToolButton* b)
{
    Qt::ToolButtonStyle style = b->toolButtonStyle();
    if (style == Qt::ToolButtonFollowStyle)
        style = static_cast<Qt::ToolButtonStyle>(b->style()->styleHint(QStyle::SH_ToolButtonStyle, nullptr, b));
    if (b->icon().isNull() && b->arrowType() == Qt::NoArrow)
        style = b->text().isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextOnly;
    const auto* bar  = qobject_cast<const QToolBar*>(b->parentWidget()); // a tool bar's buttons draw its icon size
    const int   icon = (bar ? bar->iconSize() : b->iconSize()).height();
    const int text = b->fontMetrics().size(Qt::TextShowMnemonic, b->text()).height();
    switch (style) {
    case Qt::ToolButtonTextOnly:
        return text;
    case Qt::ToolButtonTextBesideIcon:
        return qMax(icon, text);
    case Qt::ToolButtonTextUnderIcon:
        return icon + 4 + text;
    default:
        return icon;
    }
}

// The tallest tool button of buttons (not a tool bar's extension; one hidden only by a tool bar's layout counts: it moved
// into the extension or the bar wasn't laid out yet), and its contents.
static void tallestButton(const QList<QToolButton*>& buttons, int* height, int* content)
{
    for (QToolButton* b : buttons) {
        const bool inBar = qobject_cast<QToolBar*>(b->parentWidget()) != nullptr;
        if ((b->isHidden() && !inBar) || b->inherits("QToolBarExtension"))
            continue;
        b->ensurePolished();
        const int h = b->sizeHint().height();
        if (h > *height) {
            *height  = h;
            *content = toolContent(b);
        }
    }
}

// The tab height of the tab bar with tabs nearest below area (the server tabs' bar, not one inside a page), 0: none.
// Live test fix (2.7.3): a bar with one tab has a selected tab only (the probe's; TeamSpeak's chat tabs at its window's first
// Show, before the channel's tab came). A skin that gives the other tabs more border or padding (the community dark skin: 5 px
// below a chat tab that isn't selected, 3 px below the selected one) makes the bar taller once a second tab comes: a start
// with dark mode on kept the chat tabs 2 px lower, the chat 2 px taller until the first off. The other state counts too, as
// the skin sizes the same tab in it.
static int tabHeightIn(QWidget* area)
{
    QTabBar* best  = nullptr;
    int      depth = 0;
    for (QTabBar* bar : area->findChildren<QTabBar*>()) {
        int d = 0;
        for (QWidget* w = bar; w && w != area; w = w->parentWidget())
            ++d;
        if (bar->count() > 0 && (!best || d < depth)) {
            best  = bar;
            depth = d;
        }
    }
    if (!best)
        return 0;
    best->ensurePolished();
    const int height = best->sizeHint().height();
    if (best->count() != 1)
        return height;
    QStyleOptionTab tab;
    tab.initFrom(best);
    tab.shape    = best->shape();
    tab.text     = best->tabText(0);
    tab.icon     = best->tabIcon(0);
    tab.iconSize = best->iconSize();
    tab.position = QStyleOptionTab::OnlyOneTab;
    const QFontMetrics metrics = best->fontMetrics();
    const QSize        icon    = tab.icon.isNull() ? QSize() : tab.iconSize;
    const QSize        contents(metrics.horizontalAdvance(tab.text) + icon.width(), qMax(metrics.height(), icon.height()));
    tab.state |= QStyle::State_Selected;
    const int selected = best->style()->sizeFromContents(QStyle::CT_TabBarTab, &tab, contents, best).height();
    tab.state &= ~QStyle::State_Selected;
    const int other = best->style()->sizeFromContents(QStyle::CT_TabBarTab, &tab, contents, best).height();
    return height + qMax(0, other - selected);
}

// Review fix (2.7.3): the smallest height QSplitter gives an area without a minimum of its own (Qt's qSmartMinSize).
static int smallestHeight(const QWidget* area)
{
    const int policy = static_cast<int>(area->sizePolicy().verticalPolicy());
    if (area->sizePolicy().verticalPolicy() == QSizePolicy::Ignored)
        return 0;
    const int hint = area->minimumSizeHint().height();
    return qMin((policy & QSizePolicy::ShrinkFlag) ? hint : qMax(area->sizeHint().height(), hint), area->maximumHeight());
}

// The main splitter's named areas (TeamSpeak's server area and chat area) and their smallest heights in the skin: dark
// mode's scroll bars and frames ask for other ones (the server area 106 -> 140 px at 100 %, 206 -> 278 at 200 %), so an area
// at its smallest moved the chat at every switch. An area with a minimum of TeamSpeak's own keeps it in both looks (not
// one a skin's rule set: Qt marks those, and dark mode's sheet replaces the skin's).
static void measureAreas(QSplitter* splitter, darkpalette::Metrics* m)
{
    m->areas.clear();
    for (int i = 0; i < splitter->count(); ++i) {
        QWidget* area = splitter->widget(i);
        if (!area || area->isHidden() || area->objectName().isEmpty())
            continue;
        const bool bySheet = area->property("_q_stylesheet_minh").toBool();
        if (area->minimumHeight() > 0 && !bySheet)
            continue;
        area->ensurePolished();
        const int height = area->minimumHeight() > 0 ? area->minimumHeight() : smallestHeight(area);
        if (height > 0)
            m->areas.append(qMakePair(area->objectName(), height));
    }
}

static void measureBars(QMainWindow* main, darkpalette::Metrics* m)
{
    main->ensurePolished();
    if (auto* menu = qobject_cast<QMenuBar*>(main->menuWidget()); menu && !menu->isHidden()) {
        for (QAction* action : menu->actions()) {
            const QRect item = action->isVisible() && !action->isSeparator() ? menu->actionGeometry(action) : QRect();
            if (item.height() > 0) {
                m->menuItem   = item.height();
                m->menuText   = menu->fontMetrics().height();
                m->menuMargin = menu->style()->pixelMetric(QStyle::PM_MenuBarPanelWidth, nullptr, menu)
                                + menu->style()->pixelMetric(QStyle::PM_MenuBarVMargin, nullptr, menu);
                break;
            }
        }
    }
    for (QToolBar* bar : main->findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (bar->isHidden() || bar->orientation() != Qt::Horizontal)
            continue;
        int button = 0, content = 0;
        tallestButton(bar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly), &button, &content);
        const int height = qMax(bar->sizeHint().height(), bar->minimumHeight());
        if (button > 0 && height > m->toolBar) {
            m->toolBar     = height;
            m->toolButton  = button;
            m->toolContent = content;
        }
    }
    if (QWidget* servers = main->findChild<QWidget*>(QString::fromLatin1("MainWindowServerTabsWidget")))
        m->serverTab = tabHeightIn(servers);
    if (QWidget* chats = main->findChild<QWidget*>(QString::fromLatin1("MainWindowChatWidget"))) {
        m->chatTab = tabHeightIn(chats);
        tallestButton(chats->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly), &m->inputButton, &m->inputContent);
    }
    for (QSplitter* splitter : main->findChildren<QSplitter*>()) {
        int& handle = splitter->orientation() == Qt::Vertical ? m->splitterV : m->splitterH;
        if (!splitter->isHidden() && handle == 0) {
            handle = splitter->handleWidth(); // the outermost of each direction first (TeamSpeak's main splitter)
            if (splitter->orientation() == Qt::Vertical)
                measureAreas(splitter, m);
        }
    }
    // The status bar as QMainWindow gives it its height (its smart minimum: a minimum height of its own, else its hints).
    if (auto* status = main->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly); status && !status->isHidden()) {
        const int own = status->minimumHeight();
        m->status     = qMin(own > 0 ? own : qMax(status->sizeHint().height(), status->minimumSizeHint().height()), status->maximumHeight());
    }
}

// TeamSpeak's start with dark mode on (no main window yet): a probe built like TeamSpeak's main window (a menu bar, a tool
// bar, the server tabs, the chat tabs below their pages with a tool button beside the input, a status bar), never shown,
// under the sheet in place now (the skin's). Its icons have the style's sizes: the first chat's window gives TeamSpeak's
// own (refineBars).
static void measureBarsOnProbe(darkpalette::Metrics* m)
{
    QMainWindow probe;
    probe.setObjectName(QString::fromLatin1("MainWindow"));
    probe.setAttribute(Qt::WA_DontShowOnScreen);
    const auto iconOf = [](const QSize& size) {
        QPixmap pixmap(size);
        pixmap.fill(Qt::gray);
        return QIcon(pixmap);
    };
    auto* menu = new QMenuBar(&probe);
    probe.setMenuBar(menu);
    menu->addMenu(QString::fromLatin1("&Connections"));
    auto* tools = new QToolBar(&probe);
    probe.addToolBar(tools);
    tools->addAction(iconOf(probe.iconSize()), QString::fromLatin1("Connect"));
    auto* split   = new QSplitter(Qt::Vertical);
    auto* servers = new QWidget;
    servers->setObjectName(QString::fromLatin1("MainWindowServerTabsWidget"));
    auto* serverTabs = new QTabWidget;
    serverTabs->setTabsClosable(true);
    serverTabs->addTab(new QWidget, iconOf(serverTabs->tabBar()->iconSize()), QString::fromLatin1("Server"));
    (new QVBoxLayout(servers))->addWidget(serverTabs);
    auto* chats = new QWidget;
    chats->setObjectName(QString::fromLatin1("MainWindowChatWidget"));
    auto* chatTabs = new QTabWidget;
    chatTabs->setTabPosition(QTabWidget::South);
    chatTabs->setTabsClosable(true);
    chatTabs->addTab(new QWidget, iconOf(chatTabs->tabBar()->iconSize()), QString::fromLatin1("Lobby"));
    const QString teamSpeaks = QString::fromLatin1(kChatTabsClass);
    const QString sheet      = qApp->styleSheet();
    if (sheet.contains(teamSpeaks)) { // the skin's rules for TeamSpeak's chat tab widget class (as the pane's probe)
        const QRegularExpression type(QString::fromLatin1("(?<![\\w#-])%1(?![\\w-])").arg(teamSpeaks));
        chatTabs->setStyleSheet(QString(sheet).replace(type, QString::fromLatin1("QTabWidget")));
    }
    auto* chatColumn = new QVBoxLayout(chats);
    chatColumn->addWidget(chatTabs, 1);
    auto* emoticons = new QToolButton;
    emoticons->setAutoRaise(true);
    emoticons->setIcon(iconOf(emoticons->iconSize()));
    chatColumn->addWidget(emoticons);
    split->addWidget(servers);
    split->addWidget(chats);
    probe.setCentralWidget(split);
    probe.statusBar()->addWidget(new QLabel(QString::fromLatin1("Connected")));
    probe.resize(1000, 700);
    probe.ensurePolished();
    menu->resize(1000, menu->sizeHint().height()); // its items' rectangles need its width
    measureBars(&probe, m);
    m->areas.clear(); // review fix: what the areas hold is TeamSpeak's own (its tree, info panel, chat): measured in its window only
    m->barsProbed = true;
}

// 2.7.3 (M6): bars measured on the probe: the real window's tool bar and input row hold TeamSpeak's own icons, whose sizes
// the probe took from the style; a button keeps the probe's frame around the real contents. True: something changed.
static bool refineBars(QMainWindow* main, darkpalette::Metrics* m)
{
    bool       changed = false;
    const auto follow  = [&](const QList<QToolButton*>& buttons, int* bar, int* button, int* content) {
        int height = 0, real = 0;
        tallestButton(buttons, &height, &real);
        if (*button > 0 && *content > 0 && real > 0 && real != *content) {
            const int delta = real - *content;
            *button += delta;
            if (bar && *bar > 0)
                *bar += delta;
            *content = real;
            changed  = true;
        }
    };
    for (QToolBar* bar : main->findChildren<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!bar->isHidden() && bar->orientation() == Qt::Horizontal) {
            follow(bar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly), &m->toolBar, &m->toolButton, &m->toolContent);
            break;
        }
    }
    if (QWidget* chats = main->findChild<QWidget*>(QString::fromLatin1("MainWindowChatWidget")))
        follow(chats->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly), nullptr, &m->inputButton, &m->inputContent);
    return changed;
}

// Review fix (2.7.3): a main window with its bars (a menu bar, a tool bar or one of TeamSpeak's two tab areas). TeamSpeak's
// exists at the plugin's load but is still empty (the 2.7.2 live logs: "2 widgets; main window yes") and builds its bars
// afterwards: measured then, nothing was kept and the session had the design's bars.
static bool barsBuilt(const QMainWindow* main)
{
    return main && (main->menuWidget() || main->findChild<QToolBar*>(QString(), Qt::FindDirectChildrenOnly)
                    || main->findChild<QWidget*>(QString::fromLatin1("MainWindowChatWidget"))
                    || main->findChild<QWidget*>(QString::fromLatin1("MainWindowServerTabsWidget")));
}

// The window whose bars dark mode keeps: the chat's main window, else TeamSpeak's (the one main window there is).
static QMainWindow* mainWindowOf(QWidget* chat)
{
    if (auto* main = chat ? qobject_cast<QMainWindow*>(chat->window()) : nullptr)
        return main;
    QMainWindow* found = nullptr;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (auto* main = qobject_cast<QMainWindow*>(top); main && (!found || (main->isVisible() && !found->isVisible())))
            found = main;
    }
    return found;
}

void DarkMode::logPane()
{
    // 2.7.2 (D3): the live test reads what the chat column keeps (no names, only the tab widget's class and numbers).
    const darkpalette::Metrics& m = m_snap.metrics;
    if (m.paneSelector.isEmpty())
        logLine(QString::fromLatin1("[dark] chat column: no chat tab widget measured"));
    else
        logLine(QString::fromLatin1("[dark] chat column: %1 pane left %2 right %3%4 (chat frame %5, scroll bar %6)")
                    .arg(m.paneSelector)
                    .arg(m.paneLeft)
                    .arg(m.paneRight)
                    .arg(m.paneProbed ? QString::fromLatin1(", probed: no chat yet") : QString())
                    .arg(m.chatFrame)
                    .arg(m.chatBar));
    // 2.7.3 (M6): the heights the main window's bars keep (numbers only).
    if (m.menuItem > 0 || m.toolBar > 0 || m.serverTab > 0 || m.chatTab > 0 || m.status > 0)
        logLine(QString::fromLatin1("[dark] bars: menu item %1 (text %2, margin %16), tool bar %3 (button %4, contents %5), server tab %6, chat tab %7, "
                                    "input button %8 (contents %9), splitters %10/%11, status %12, chat pane top %13 bottom %14%15")
                    .arg(m.menuItem)
                    .arg(m.menuText)
                    .arg(m.toolBar)
                    .arg(m.toolButton)
                    .arg(m.toolContent)
                    .arg(m.serverTab)
                    .arg(m.chatTab)
                    .arg(m.inputButton)
                    .arg(m.inputContent)
                    .arg(m.splitterV)
                    .arg(m.splitterH)
                    .arg(m.status)
                    .arg(m.paneTop)
                    .arg(m.paneBottom)
                    .arg(m.barsProbed ? QString::fromLatin1(", probed: no main window yet") : QString())
                    .arg(m.menuMargin));
    else
        logLine(QString::fromLatin1("[dark] bars: no main window measured"));
    // Review fix: the main splitter's areas (TeamSpeak's own object names) and their smallest heights.
    if (!m.areas.isEmpty()) {
        QStringList areas;
        for (const QPair<QString, int>& area : m.areas)
            areas << QString::fromLatin1("%1 %2").arg(area.first).arg(area.second);
        logLine(QString::fromLatin1("[dark] splitter areas at their smallest: %1").arg(areas.join(QString::fromLatin1(", "))));
    }
}

darkpalette::Metrics DarkMode::measure(const QList<QTextEdit*>& views) const
{
    darkpalette::Metrics m;
    QTextEdit*           chat = nullptr;
    for (QTextEdit* v : views) {
        if (named(v, "ChatTab") && (!chat || (v->isVisible() && !chat->isVisible())))
            chat = v;
    }
    if (!chat) {
        // Review fix: no chat yet (TeamSpeak's start with dark mode on: it builds its chats after the plugin's load). Probes
        // polished by the sheet in place now (the skin's: apply measures before ours is set, adoptSkin after TeamSpeak's)
        // give the chat's frame and scroll bar and a tab widget's pane, so the dark sheet keeps the skin's column from the
        // start (before, defaults and no pane rule stayed until an off and on, and the session's first off changed every
        // chat's column). The chat probe is a QTextEdit, not a QTextBrowser (no module takes it for a chat). The pane rule
        // names TeamSpeak's chat tab widget class until the first chat shows its own (confirmPane); a skin's rules for that
        // class reach the tab probe through its own sheet with the class's name made QTabWidget (the same specificity, so
        // the same rule wins).
        QTextEdit chatProbe;
        chatProbe.setObjectName(QString::fromLatin1("ChatTab"));
        chatProbe.setAttribute(Qt::WA_DontShowOnScreen);
        chatProbe.ensurePolished();
        // Its scroll bar was polished in QTextEdit's constructor (QScrollBar::sizeHint), before the probe had its name: a
        // chat TeamSpeak names is polished again when it goes into its tab widget, the probe's bar here.
        QScrollBar* bar = chatProbe.verticalScrollBar();
        bar->style()->unpolish(bar);
        bar->style()->polish(bar);
        m.chatFrame = chatProbe.frameWidth();
        m.chatBar   = qMax(1, bar->sizeHint().width());
        const QString teamSpeaks = QString::fromLatin1(kChatTabsClass);
        const QString sheet      = qApp->styleSheet();
        QTabWidget    tabsProbe;
        tabsProbe.setAttribute(Qt::WA_DontShowOnScreen);
        if (sheet.contains(teamSpeaks)) {
            const QRegularExpression type(QString::fromLatin1("(?<![\\w#-])%1(?![\\w-])").arg(teamSpeaks));
            tabsProbe.setStyleSheet(QString(sheet).replace(type, QString::fromLatin1("QTabWidget")));
        }
        tabsProbe.setTabPosition(QTabWidget::South); // 2.7.3 (M6): TeamSpeak's chat tabs are below their pages
        tabsProbe.addTab(new QWidget, QString::fromLatin1("Lobby"));
        int left = 0, right = 0, top = -1, bottom = -1;
        if (paneInsets(&tabsProbe, &left, &right, &top, &bottom)) {
            m.paneSelector = teamSpeaks;
            m.paneLeft     = left;
            m.paneRight    = right;
            m.paneTop      = top < 0 ? -1 : top;
            m.paneBottom   = top < 0 ? -1 : bottom;
            m.paneProbed   = true;
        }
        // 2.7.3 (M6): the main window's bars (TeamSpeak's own when it built them already, else a probe's: review fix, also
        // when its window exists without them yet).
        if (QMainWindow* main = mainWindowOf(nullptr); barsBuilt(main))
            measureBars(main, &m);
        else
            measureBarsOnProbe(&m);
        return m;
    }
    chat->ensurePolished();
    chat->verticalScrollBar()->ensurePolished();
    m.chatFrame = chat->frameWidth();
    m.chatBar   = qMax(1, chat->verticalScrollBar()->sizeHint().width());
    measurePane(chat, &m);
    if (QMainWindow* main = mainWindowOf(chat)) // 2.7.3 (M6)
        measureBars(main, &m);
    return m;
}

// The colours of the skin's chat as TS Media printed its lines for them (2.7.1): the ChatTab's palette with its own
// style-sheet background sampled, exactly as ChatIntegration's chatcolors::Cache reads them (a picture background never
// reaches the palette). Never inside a paint (apply and adoptSkin only).
static chatcolors::Colors skinChatColors(QTextEdit* chat)
{
    const QPalette palette = chat->palette();
    return chatcolors::withSample(chatcolors::fromPalette(palette), chatcolors::sampleBackground(chat), chatcolors::isTexture(palette));
}

chatcolors::Colors DarkMode::chatColorsNow(const QList<QTextEdit*>& views) const
{
    // A probe: never shown and gone at once. Not a QTextBrowser (no module takes it for a chat), and m_applying is set (our
    // filter leaves its Polish alone).
    const auto probe = [](const QPalette* palette, const QString& sheet) {
        QTextEdit edit;
        edit.setObjectName(QString::fromLatin1("ChatTab"));
        edit.setAttribute(Qt::WA_DontShowOnScreen);
        if (palette) {
            edit.setPalette(*palette);
            edit.setStyleSheet(sheet);
        }
        edit.ensurePolished();
        return skinChatColors(&edit);
    };
    // 2.7.2: the visible chat shows the skin only while both its sheet and its palette are in place (apply, before ours is
    // set; a skin change that set both in one turn). In every other shape (dark mode's sheet still in place, or its palette,
    // or roles of both mixed) the probe: a chat is never read under dark mode's ChatTab rule (a complete palette set alone,
    // the sheet left as it was, made the merged palette equal to the application's and the chat read dark).
    const bool skinShown = (qApp->styleSheet().constData() == m_snap.sheet.constData() || qApp->styleSheet() == m_snap.sheet)
                           && QApplication::palette() == m_snap.palette;
    if (m_applied && !skinShown) {
        // 2.7.1: a skin taken over while dark mode is on: the application palette is dark mode's, and Qt resolves a sheet's
        // palette(base) with the application palette, so a chat would show the skin's sheet on dark mode's colours. The
        // probe gets the skin's palette and the skin's sheet with those references resolved by it (a widget's own sheet
        // wins over the application's): what the skin shows once dark mode is off. Review fix: the application sheet may
        // still be dark mode's (only the palette changed), and its ChatTab rule would fill in what the skin's leaves out
        // (TeamSpeak's own sets no text colour): the probe's sheet first sets the background, the text and the frame
        // the way a chat shows them without a rule (the skin's palette; a texture's background is left to the sample),
        // then the skin's rules (they win where they say something: any selector beats *).
        const QBrush& base  = m_snap.palette.brush(QPalette::Active, QPalette::Base);
        const QString reset = QString::fromLatin1("* { %1color: %2; border: none; }\n")
                                  .arg(base.style() == Qt::SolidPattern ? QString::fromLatin1("background-color: %1; ").arg(base.color().name(QColor::HexRgb)) : QString(),
                                       m_snap.palette.color(QPalette::Active, QPalette::Text).name(QColor::HexRgb));
        return probe(&m_snap.palette, reset + darkchat::resolvePaletteRefs(m_snap.sheet, m_snap.palette));
    }
    QTextEdit* chat = nullptr;
    for (QTextEdit* v : views) {
        if (named(v, "ChatTab") && (!chat || (v->isVisible() && !chat->isVisible())))
            chat = v;
    }
    if (chat) {
        if (m_applied) {
            // 2.7.2: a skin taken over that set its sheet before its palette left the chat polished while the palette before
            // was in place (Qt resolves a sheet's palette(base) when it polishes a widget, with the application palette of
            // that moment): polished again with both of the skin's in place, so it shows what the skin shows once dark mode
            // is off (that off sets the palette first). The same sheet and palette: no font or size changes.
            QStyle* style = chat->style();
            style->unpolish(chat);
            style->polish(chat);
        }
        return skinChatColors(chat);
    }
    // No chat yet (TeamSpeak's start, review fix): the probe polished by the sheet in use now (the skin's: apply asks before
    // ours is set).
    return probe(nullptr, QString());
}

QList<QTextEdit*> DarkMode::findViews() const
{
    QList<QTextEdit*> out;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (isThemedView(top))
            out << static_cast<QTextEdit*>(top);
        for (QTextEdit* e : top->findChildren<QTextEdit*>()) {
            if (isThemedView(e))
                out << e;
        }
    }
    return out;
}

// ---- on and off --------------------------------------------------------------------------------------------------------

void DarkMode::apply()
{
    QElapsedTimer clock;
    clock.start();
    const Changing changing; // 2.7.2 (D7)
    m_applying                    = true;
    const QList<QTextEdit*> views = findViews();
    readTeamSpeakDarkFiles(); // 2.7.2 (live test)
    // 2.7.3 (M7): TeamSpeak set this sheet under the style's own palette: whatever it holds is the style's (a style with
    // dark colours of its own gets TeamSpeak's dark-mode files from TeamSpeak itself).
    m_snap.sheet                  = qApp->styleSheet();
    m_appStripped                 = false;
    m_snap.palette                = QApplication::palette();
    m_snap.metrics                = measure(views);
    m_barsGuessed                 = m_snap.metrics.barsProbed; // live test fix: measured in TeamSpeak's window at its Show
    m_styleMeasure                = nullptr;
    m_snap.chat                   = chatColorsNow(views);
    logPane();
    buildTheme();
    const QVector<Place> places = capturePlaces(views);

    m_applied = true;
    m_paused  = false;
    g_active  = true; // before the sheet: windows that re-theme on its StyleChange ask isActive()
    m_adoptTimes.clear();
    // The palette first, then the sheet, then the documents (design_dark 1.3).
    QApplication::setPalette(m_palette);
    m_palette = QApplication::palette();
    qApp->setStyleSheet(m_sheet);
    m_sheet                = qApp->styleSheet(); // QApplication's shared copy: adoptSkin knows ours by its data
    m_stats.lastSheetMs    = clock.elapsed();
    m_stats.fragments      = 0;
    m_trackingAtApply      = true; // 2.7.3 (M7): these documents' sheets are the style's (Doc::skinSheet)
    for (QTextEdit* view : views)
        track(view);
    m_trackingAtApply   = false;
    m_stats.lastChatsMs = clock.elapsed() - m_stats.lastSheetMs;
    announceTheme(); // 2.7.1: TS Media's pictures for the dark look, inside the bracket
    restorePlaces(places);
    emit placesRestored(); // live test: a scale pass the switch started keeps this place
    for (QWidget* window : QApplication::topLevelWidgets())
        titleBar(window);
    installFilter(true);
    m_applying = false;
    if (!m_pending.isEmpty())
        m_slice->start();
    m_stats.sheetChars  = m_sheet.size();
    m_stats.widgets     = QApplication::allWidgets().size();
    m_stats.lastApplyMs = clock.elapsed();
    QWidget* main = nullptr;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (top->inherits("QMainWindow"))
            main = top;
    }
    logLine(QString::fromLatin1("[dark] on in %1 ms (sheet %2 ms, chats %3 ms, cards %4 ms, %5 widgets; sheet %6 chars, scale %7%, main window %8)")
                .arg(m_stats.lastApplyMs)
                .arg(m_stats.lastSheetMs)
                .arg(m_stats.lastChatsMs)
                .arg(m_stats.lastCardsMs)
                .arg(m_stats.widgets)
                .arg(m_stats.sheetChars)
                .arg(qRound(m_k * 100))
                .arg(main ? QString::fromLatin1("yes") : QString::fromLatin1("no")));
}

void DarkMode::announceTheme()
{
    // The chats' widths final first (tab bar and input heights; restorePlaces does it again and finds nothing left).
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QElapsedTimer clock;
    clock.start();
    emit themeChanged();
    m_stats.lastCardsMs = clock.elapsed();
}

void DarkMode::remove()
{
    if (!m_applied && !m_paused) {
        stopTimers();
        installFilter(false);
        revertTitleBars();
        return;
    }
    QElapsedTimer clock;
    clock.start();
    const Changing changing; // 2.7.2 (D7)
    m_applying = true;
    stopTimers();
    installFilter(false);
    QList<QTextEdit*> views;
    for (const Doc& d : qAsConst(m_docs)) {
        if (d.view && d.doc && d.view->document() == d.doc)
            views << d.view.data();
    }
    const QVector<Place> places = capturePlaces(views);
    g_active                    = false;
    // The documents first: their colours back (lines added while dark get the skin's colours), then their own sheets.
    int strippedChats = 0;
    {
        layoutformat::MutatingScope scope;
        // 2.7.3 (M8): per sheet TeamSpeak printed lines under, the sheet they get back, chat or info panel.
        QHash<QString, darkchat::ColorMap> printedMaps;
        // 2.7.3 (M7): TeamSpeak's chat loader tests the palette after a pick, which without dark mode is the snapshot's.
        const bool styleDark = teamSpeakDark(m_snap.palette);
        for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
            Doc& d = it.value();
            QObject::disconnect(d.changes);
            if (!d.doc)
                continue;
            // 2.7.2 (live test): a chat TeamSpeak made while dark mode was on (a new server tab or private chat, every chat of
            // a start with dark mode on) has TeamSpeak's dark-mode file in its sheet, and its lines that file's look: the
            // skin's own sheet goes back (else every later line kept those colours, light blue on white), and the lines get
            // its look, as TeamSpeak gives a chat it makes with the skin's palette. 2.7.3 (M7): only a sheet TeamSpeak set
            // under dark mode's palette, only when the style's own palette wouldn't have got the file, and only TeamSpeak's
            // exact composition (a style with dark colours of its own, or whose own files hold such a text, keeps it).
            const QString own = d.skinSheet || styleDark ? d.sheet : ownChatSheet(d.sheet);
            if (own != d.sheet)
                ++strippedChats;
            // Lines printed while dark: from the dark sheet's looks to the style shown after off (2.7.3, M8: directly; the
            // ones with kOriginal get exactly their colour before). Review fix: lines printed under a dark sheet the document
            // had before a style was picked get the style's text sizes and fonts too (backMapFor).
            const darkchat::ColorMap back = backMapFor(d, own);
            if (d.recolored)
                darkchat::recolor(d.doc, back, false);
            else if (d.dirtyFrom >= 0)
                darkchat::recolor(d.doc, back, false, d.dirtyFrom, d.dirtyTo);
            // 2.7.3 (M8): the lines TeamSpeak printed under a sheet of its own while dark mode was on (a chat made while on,
            // before dark mode saw it; review fix: a line in the very turn TeamSpeak set a picked style's sheet), their look
            // that sheet's (kOriginal given back above), get the style shown after off too, also after another style was
            // picked meanwhile (they kept TeamSpeak's dark chat colours).
            struct Range {
                int                from, to;
                darkchat::ColorMap map;
            };
            QVector<Range> ranges;
            for (const Doc::Printed& p : qAsConst(d.printed)) {
                if (p.sheet == own || p.to <= p.from)
                    continue;
                const QString key = p.sheet + QChar(0) + own + (d.info ? QLatin1Char('1') : QLatin1Char('0'));
                auto          map = printedMaps.constFind(key);
                if (map == printedMaps.constEnd())
                    map = printedMaps.insert(key, ownSheetMap(p.sheet, own, d.info));
                ranges.append({p.from, p.to, map.value()});
                darkchat::recolor(d.doc, map.value(), false, p.from, p.to);
            }
            d.doc->setDefaultStyleSheet(own);
            // 2.7.2 (D6): the view's own character format (QTextEdit::append keeps the one it gave the new block as the
            // cursor's, and copies it into every block it appends later, and plain text appended or typed takes it): mapped
            // back like a fragment, the one of a line printed while dark too (2.7.3, M5: a click on a name printed while dark
            // left that name's dark look there, without kOriginal), and in the lines TeamSpeak printed under its own sheet
            // with them (the cursor in one of those ranges). Only without a selection
            // (with one, setCurrentCharFormat would format the selected text); no document edit, no scroll.
            if (!d.view || d.view->document() != d.doc || d.view->textCursor().hasSelection())
                continue;
            QTextCharFormat cursor  = d.view->currentCharFormat();
            bool            changed = darkchat::mapBack(cursor, back);
            const int       at      = d.view->textCursor().position();
            for (const Range& r : qAsConst(ranges)) {
                if (at >= r.from && at <= r.to)
                    changed = darkchat::mapBack(cursor, r.map) || changed;
            }
            if (changed)
                d.view->setCurrentCharFormat(cursor);
        }
    }
    for (const QMetaObject::Connection& c : qAsConst(m_tipConnections))
        QObject::disconnect(c);
    m_tipConnections.clear();
    m_tips.clear();
    QToolTip::hideText(); // a tip open now would keep its remapped colours on the light tip
    // Then the palette, then the sheet: the skin reads palette(base) when it polishes the widgets.
    QApplication::setPalette(m_snap.palette);
    restorePalette();
    qApp->setStyleSheet(m_snap.sheet);
    announceTheme(); // 2.7.1: TS Media's pictures for TeamSpeak's look (also those a Disable or Reload leaves), inside the bracket
    restorePlaces(places);
    emit placesRestored(); // live test: a scale pass the switch started keeps this place
    revertTitleBars();
    m_docs.clear();
    m_pending.clear();
    m_maps.clear();
    m_infoMaps.clear();
    m_darkSheets.clear();
    m_darkClasses.clear();
    m_applied  = false;
    m_paused   = false;
    m_applying = false;
    m_barsGuessed  = false;
    m_styleMeasure = nullptr;
    m_stats.lastRemoveMs = clock.elapsed();
    logLine(QString::fromLatin1("[dark] TeamSpeak's dark-mode files taken off: application sheet %1, chats %2").arg(m_appStripped ? 1 : 0).arg(strippedChats));
    m_appStripped = false;
    logLine(QString::fromLatin1("[dark] off in %1 ms (cards %2 ms)").arg(m_stats.lastRemoveMs).arg(m_stats.lastCardsMs));
}

// Every role of every group as it was; one that Qt resolved differently is set again (it enters the resolve mask then).
void DarkMode::restorePalette()
{
    const QPalette now     = QApplication::palette();
    QPalette       fixed   = now;
    int            differ  = 0;
    for (int g = 0; g < QPalette::NColorGroups; ++g) {
        for (int r = 0; r < QPalette::NColorRoles; ++r) {
            const auto group = static_cast<QPalette::ColorGroup>(g);
            const auto role  = static_cast<QPalette::ColorRole>(r);
            if (now.brush(group, role) != m_snap.palette.brush(group, role)) {
                fixed.setBrush(group, role, m_snap.palette.brush(group, role));
                ++differ;
            }
        }
    }
    if (differ > 0) {
        QApplication::setPalette(fixed);
        logLine(QString::fromLatin1("[dark] palette restore: %1 roles differ, fixed (resolve mask %2 -> %3)")
                    .arg(differ)
                    .arg(m_snap.palette.resolve())
                    .arg(QApplication::palette().resolve()),
                LogLevel_WARNING);
    }
}

void DarkMode::pause()
{
    m_paused = true;
    g_active = false;
    stopTimers();
    installFilter(false);
    logLine(QString::fromLatin1("[dark] the application style sheet keeps changing; dark mode pauses until it is turned on again"), LogLevel_WARNING);
}

bool DarkMode::teamSpeakClosing(bool* mainShown, int* othersShown) const
{
    // At Quit TeamSpeak hides its main window and then lets the plugin go, before aboutToQuit (live test). A Disable or
    // Reload is clicked in Options, over the main window, which is shown then (minimized counts as shown). The main window
    // is the sign (review fix): Qt doesn't hide a window's own windows with it, and another plugin's window or one TeamSpeak
    // leaves open may still be shown at Quit.
    bool main   = false;
    int  others = 0;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (!top->isVisible() || top->windowType() == Qt::Popup || top->windowType() == Qt::ToolTip || top->objectName().startsWith(QLatin1String("tsmedia")))
            continue;
        if (top->inherits("QMainWindow"))
            main = true;
        else
            ++others;
    }
    if (mainShown)
        *mainShown = main;
    if (othersShown)
        *othersShown = others;
    return m_quitting || QCoreApplication::closingDown() || !main;
}

void DarkMode::shutdown()
{
    if (m_shutDown)
        return;
    stopTimers();
    bool       mainShown   = false;
    int        othersShown = 0;
    const bool closing     = teamSpeakClosing(&mainShown, &othersShown);
    if (m_applied && closing) {
        // TeamSpeak is closing (live test): its windows are being torn down, and putting its skin back repolishes every one
        // of them (a crash in the style sheet code at Quit, in a widget half gone). Its main window isn't shown any more and
        // nothing of dark mode in Qt needs this DLL (the sheets, colours and document sheets are copies on the heap), so only
        // our hooks go; the theme stays on what is left until TeamSpeak exits.
        logLine(QString::fromLatin1("[dark] TeamSpeak is closing: its style is left as it is (quit %1, closing down %2, main window %3, other windows %4)")
                    .arg(m_quitting ? 1 : 0)
                    .arg(QCoreApplication::closingDown() ? 1 : 0)
                    .arg(mainShown ? 1 : 0)
                    .arg(othersShown));
        installFilter(false);
        for (auto it = m_docs.begin(); it != m_docs.end(); ++it)
            QObject::disconnect(it->changes);
        for (const QMetaObject::Connection& c : qAsConst(m_tipConnections))
            QObject::disconnect(c);
        m_tipConnections.clear();
        m_tips.clear();
        for (const Place& p : qAsConst(m_deferred)) {
            if (p.browser)
                p.browser->removeEventFilter(this);
        }
        m_deferred.clear();
        m_docs.clear();
        m_pending.clear();
        m_applied = false;
        darktitlebar::shutdown();
        g_active   = false;
        m_shutDown = true;
        return;
    }
    if (m_applied)
        logLine(QString::fromLatin1("[dark] the plugin goes: TeamSpeak's style comes back (main window 1, other windows %1)").arg(othersShown));
    m_deferHidden = false; // the plugin goes: every hidden chat laid out and placed now
    remove(); // everything back (nothing to do when it is off), the filter removed, the title bars reverted
    settleDeferred();
    installFilter(false);
    for (const QMetaObject::Connection& c : qAsConst(m_tipConnections))
        QObject::disconnect(c);
    m_tipConnections.clear();
    darktitlebar::shutdown();
    g_active   = false;
    m_shutDown = true;
}

void DarkMode::installFilter(bool on)
{
    if (on == m_filtering || !qApp)
        return;
    if (on)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
    m_filtering = on;
}

void DarkMode::stopTimers()
{
    for (QTimer* t : {m_adopt, m_slice, m_fresh, m_tip, m_repane}) {
        if (t)
            t->stop();
    }
}

int DarkMode::timersActive() const
{
    int n = 0;
    for (QTimer* t : {m_adopt, m_slice, m_fresh, m_tip, m_repane})
        n += t && t->isActive() ? 1 : 0;
    return n;
}

// ---- another skin while dark mode is on (design_dark 1.4) ---------------------------------------------------------------

void DarkMode::adoptSkin()
{
    if (!m_applied || m_paused || m_applying)
        return;
    const QString  sheet        = qApp->styleSheet();
    const bool     sheetChanged = !(sheet.constData() == m_sheet.constData() || sheet == m_sheet);
    const QPalette now          = QApplication::palette();
    const bool     paletteChanged = !(now == m_palette);
    QList<QTextDocument*> docsChanged;
    for (auto it = m_docs.cbegin(); it != m_docs.cend(); ++it) {
        if (it->doc && it->doc->defaultStyleSheet() != it->darkSheet)
            docsChanged << it->doc.data();
    }
    if (!sheetChanged && !paletteChanged && docsChanged.isEmpty())
        return; // our own change (a late StyleChange): nothing to take over
    if (!sheetChanged && !paletteChanged) {
        // Only documents TeamSpeak gave its skin's sheet again (a new chat): theirs is the one to give back, ours goes on.
        // No repolish, and not a fight over the application sheet.
        for (QTextDocument* doc : qAsConst(docsChanged)) {
            Doc& d = m_docs[doc];
            takeSheet(d, doc->defaultStyleSheet());
            doc->setDefaultStyleSheet(d.darkSheet);
        }
        return;
    }
    // Another plugin or tool fighting over the sheet: at most 5 adoptions in 10 s.
    const qint64 t = m_clock.elapsed();
    m_adoptTimes.append(t);
    while (!m_adoptTimes.isEmpty() && m_adoptTimes.first() < t - 10000)
        m_adoptTimes.removeFirst();
    if (m_adoptTimes.size() > 5) {
        pause();
        return;
    }
    ++m_stats.adoptions;
    const Changing changing; // 2.7.2 (D7)
    m_applying                    = true;
    const QList<QTextEdit*> views = findViews();
    if (sheetChanged) {
        // 2.7.2 (live test): TeamSpeak put the skin in place under our palette, with its own dark-mode file after it (the
        // tab names white on a light tab bar after off): the skin's sheet is the one without it. 2.7.3 (M7): only when the
        // palette TeamSpeak's own test would have seen without dark mode (the style's before this pick: the snapshot's,
        // before this adoption's roles go in below) wouldn't have got it, and only TeamSpeak's exact composition.
        readTeamSpeakDarkFiles();
        m_snap.sheet = teamSpeakDark(m_snap.palette) ? sheet : ownAppSheet(sheet);
        const bool stripped = m_snap.sheet != sheet;
        m_appStripped       = m_appStripped || stripped;
        logLine(QString::fromLatin1("[dark] TeamSpeak's dark-mode files taken off: application sheet %1, chats 0").arg(stripped ? 1 : 0));
        m_snap.metrics = measure(views); // polished with the new skin at this moment
        m_barsGuessed  = m_snap.metrics.barsProbed;
        logPane();
    }
    if (paletteChanged) {
        // Every role TeamSpeak changed from what we set (a skin that sets only Link keeps the rest of the original).
        for (int g = 0; g < QPalette::NColorGroups; ++g) {
            for (int r = 0; r < QPalette::NColorRoles; ++r) {
                const auto group = static_cast<QPalette::ColorGroup>(g);
                const auto role  = static_cast<QPalette::ColorRole>(r);
                if (now.brush(group, role) != m_palette.brush(group, role))
                    m_snap.palette.setBrush(group, role, now.brush(group, role));
            }
        }
    }
    m_snap.chat = chatColorsNow(views); // 2.7.1: with the skin's sheet and palette as they are now (both above)
    for (QTextDocument* doc : qAsConst(docsChanged)) {
        Doc& d      = m_docs[doc]; // the new skin's _chat.qss (its dark sheet below, made for the new snapshot)
        d.sheet     = doc->defaultStyleSheet();
        d.skinSheet = false; // 2.7.3 (M7): set under dark mode's palette
    }
    buildTheme();
    const QVector<Place> places = capturePlaces(views);
    QApplication::setPalette(m_palette);
    m_palette = QApplication::palette();
    qApp->setStyleSheet(m_sheet);
    m_sheet = qApp->styleSheet();
    for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
        if (!it->doc)
            continue;
        setDarkSheet(it.value(), darkSheetFor(it->sheet)); // 2.7.3 (M9): the (new) style's spacing
        if (it->doc->defaultStyleSheet() != it->darkSheet)
            it->doc->setDefaultStyleSheet(it->darkSheet);
    }
    announceTheme(); // 2.7.1: what the skin's turn drew is drawn for dark mode again, inside the bracket
    restorePlaces(places);
    // No placesRestored here (review fix): these places were read after TeamSpeak's skin had changed the chats (a new chat
    // font moves every line under the same scroll value), so a scale pass the skin started keeps the place it took at the
    // chat's first FontChange or StyleChange, before its document got anything new.
    m_applying = false;
    m_stats.sheetChars = m_sheet.size();
    logLine(QString::fromLatin1("[dark] adopted the skin (%1 chars)").arg(m_snap.sheet.size()));
}

// ---- documents ---------------------------------------------------------------------------------------------------------

void DarkMode::track(QTextEdit* view)
{
    QTextDocument* doc = view ? view->document() : nullptr;
    if (!doc)
        return;
    if (m_applied && (m_snap.metrics.paneProbed || m_snap.metrics.barsProbed) && named(view, "ChatTab"))
        confirmPane(view);
    auto known = m_docs.find(doc);
    if (known != m_docs.end() && !known->doc) {
        // A document freed since (a closed chat) whose address a new one got (review fix): the new one is tracked.
        QObject::disconnect(known->changes);
        m_docs.erase(known);
        known = m_docs.end();
    }
    if (known != m_docs.end()) {
        if (known->view != view)
            known->view = view;
        return;
    }
    Doc d;
    d.view    = view;
    d.doc     = doc;
    d.sheet   = doc->defaultStyleSheet();
    d.info    = named(view, "InfoFrame"); // 2.7.2 (D9): the info panel's maps
    d.skinSheet = m_trackingAtApply;       // 2.7.3 (M7): set by TeamSpeak before dark mode was on
    d.darkSheet = darkSheetFor(d.sheet);   // 2.7.3 (M9): with its own sheet's spacing
    if (!m_trackingAtApply && !d.sheet.isEmpty() && doc->characterCount() > 1) {
        // 2.7.3 (M8): made while dark mode was on (a new server tab or private chat, every chat of a start with dark mode on,
        // a document TeamSpeak swapped in): the lines TeamSpeak printed before we saw it, under the sheet it made it with (a
        // chat seen before TeamSpeak gave it its sheet: onContentsChange notes the lines printed under that one).
        d.printed.append({d.sheet, 0, doc->characterCount() - 1});
    }
    d.changes = connect(doc, &QTextDocument::contentsChange, this,
                        [this, doc](int position, int removed, int added) { onContentsChange(doc, position, removed, added); });
    doc->setDefaultStyleSheet(d.darkSheet);
    Doc& entry = m_docs.insert(doc, d).value();
    if (view->isVisible()) {
        recolorDoc(entry);
    } else {
        entry.pending = true;
        m_pending.append(doc);
        m_slice->start();
    }
}

// Review fix (2.7.2, D3): the first chat of a session whose dark sheet was measured on probes (TeamSpeak's start with dark
// mode on): its tab widget's own selector. TeamSpeak's ChatTabWidget, which the probe's rule names: nothing to do. Another
// (or none: tabs on the side, a plain QTabWidget without a name): the sheet once more with it, on the next turn (never
// inside the Polish or Show that found the chat).
// 2.7.3 (M6): and the bars measured on the probe follow the real window's icons (review fix: once the chat is in a main
// window with its bars; until then they stay probed, and the chat's Show tells again); the pane's top and bottom insets
// were the probe's for tabs below the pages (TeamSpeak's): tabs elsewhere get the design's.
void DarkMode::confirmPane(QTextEdit* chat)
{
    QTabWidget* tabs = chatTabsOf(chat);
    if (!tabs)
        return; // not in its tab widget yet: its Show tells
    darkpalette::Metrics& m     = m_snap.metrics;
    bool                  again = false;
    if (m.barsProbed) {
        auto* main = qobject_cast<QMainWindow*>(chat->window());
        if (barsBuilt(main)) {
            m.barsProbed = false;
            again        = refineBars(main, &m);
        }
    }
    // Live test fix: a chat in TeamSpeak's window shown already (its first Show came before its bars): measured in the skin.
    if (m_barsGuessed) {
        auto* main = qobject_cast<QMainWindow*>(chat->window());
        if (barsBuilt(main) && main->isVisible()) {
            m_barsGuessed  = false;
            m_styleMeasure = main;
            again          = true;
        }
    }
    if (m.paneProbed && tabs->tabPosition() != QTabWidget::South && m.paneTop >= 0) {
        m.paneTop    = -1;
        m.paneBottom = -1;
        again        = true;
    }
    m.paneProbed           = false;
    const QString selector = paneSelectorOf(tabs);
    if (selector != m.paneSelector) {
        m.paneSelector = selector;
        again          = true;
    }
    if (again)
        m_repane->start();
}

void DarkMode::repane()
{
    if (!m_applied || m_paused || m_applying)
        return;
    QElapsedTimer clock;
    clock.start();
    m_applying                      = true;
    const QList<QTextEdit*> views   = findViews();
    const QVector<Place>    places  = capturePlaces(views);
    const bool              inStyle = !m_styleMeasure.isNull();
    if (inStyle) {
        QMainWindow* main = m_styleMeasure.data();
        m_styleMeasure    = nullptr;
        measureInStyle(main);
    }
    m_sheet = sheetNow();
    qApp->setStyleSheet(m_sheet);
    m_sheet = qApp->styleSheet();
    restorePlaces(places);
    m_applying         = false;
    m_stats.sheetChars = m_sheet.size();
    logPane();
    if (inStyle)
        logLine(QString::fromLatin1("[dark] the sheet again for TeamSpeak's window measured in its style in %1 ms").arg(clock.elapsed()));
    else
        logLine(QString::fromLatin1("[dark] the sheet again for the first chat's tab widget in %1 ms").arg(clock.elapsed()));
}

// Live test fix (2.7.3, M6): at a start with dark mode on the probe was measured: a status bar with text, a plain tab
// widget, no splitter areas. TeamSpeak's own bars hold more (its status bar's icons, taller than its text, and its server
// tabs' contents: live 3-5 px and 9-15 px taller, so the chat was 2-3 px taller in dark mode and moved at the first off; the
// splitter stopped earlier at its ends until the next on). Its window is measured once in the skin's sheet, as an on
// measures it (the application palette stays dark mode's: no size depends on it; the dark sheet is back before the window
// is painted). A style without a sheet gets one without rules for it (none would replace the application's style object).
void DarkMode::measureInStyle(QMainWindow* main)
{
    if (!main)
        return;
    QElapsedTimer clock;
    clock.start();
    qApp->setStyleSheet(m_snap.sheet.isEmpty() ? QString::fromLatin1("/* the style's own look */") : m_snap.sheet);
    darkpalette::Metrics real;
    measureBars(main, &real);
    darkpalette::Metrics& m = m_snap.metrics;
    if (real.menuItem > 0 && real.menuText > 0) { // a bar it hasn't (or can't measure) keeps the probe's
        m.menuItem   = real.menuItem;
        m.menuText   = real.menuText;
        m.menuMargin = real.menuMargin;
    }
    if (real.toolBar > 0 && real.toolButton > 0) {
        m.toolBar     = real.toolBar;
        m.toolButton  = real.toolButton;
        m.toolContent = real.toolContent;
    }
    if (real.inputButton > 0) {
        m.inputButton  = real.inputButton;
        m.inputContent = real.inputContent;
    }
    const QPair<int, int*> heights[] = {{real.serverTab, &m.serverTab}, {real.chatTab, &m.chatTab}, {real.splitterV, &m.splitterV},
                                        {real.splitterH, &m.splitterH}, {real.status, &m.status}};
    for (const QPair<int, int*>& height : heights) {
        if (height.first > 0)
            *height.second = height.first;
    }
    m.areas      = real.areas;
    m.barsProbed = false;
    logLine(QString::fromLatin1("[dark] TeamSpeak's window measured in its style in %1 ms").arg(clock.elapsed()));
}

// TeamSpeak gave a tracked document another sheet while dark mode is on (its own after ours: a brand-new chat, a style
// picked): that is its sheet now, one set under dark mode's palette (2.7.3, M7), with the dark sheet for its spacing (M9).
void DarkMode::takeSheet(Doc& d, const QString& sheet)
{
    d.sheet     = sheet;
    d.skinSheet = false;
    setDarkSheet(d, darkSheetFor(sheet));
}

void DarkMode::setDarkSheet(Doc& d, const QString& dark)
{
    if (dark == d.darkSheet)
        return;
    if (!d.darkSheet.isEmpty() && !d.formerDark.contains(d.darkSheet))
        d.formerDark.append(d.darkSheet);
    d.darkSheet = dark;
}

darkchat::ColorMap DarkMode::backMapFor(const Doc& d, const QString& own)
{
    darkchat::ColorMap back = mapsFor(own, d.darkSheet, d.info).back;
    for (const QString& former : d.formerDark) {
        if (former == d.darkSheet)
            continue;
        // The same looks (one template's colours and text styles): only the values a former dark sheet's text styles gave
        // its lines (the style's sizes and fonts then) join those each text style fix takes.
        // A value the current dark sheet's fixes already take keeps its fix (the current sheet's wins).
        const darkchat::ColorMap then = mapsFor(own, former, d.info).back;
        for (auto it = then.fonts.constBegin(); it != then.fonts.constEnd(); ++it) {
            QVector<darkchat::StyleFix>& fixes = back.fonts[it.key()];
            for (const darkchat::StyleFix& fix : it.value()) {
                QVariantList from;
                for (const QVariant& value : fix.from) {
                    bool taken = false;
                    for (const darkchat::StyleFix& mine : qAsConst(fixes))
                        taken = taken || (mine.id == fix.id && mine.from.contains(value));
                    if (!taken)
                        from.append(value);
                }
                if (from.isEmpty())
                    continue;
                int same = -1;
                for (int i = 0; i < fixes.size() && same < 0; ++i)
                    same = fixes.at(i).id == fix.id && fixes.at(i).value == fix.value ? i : -1;
                if (same >= 0) {
                    fixes[same].from += from;
                } else {
                    darkchat::StyleFix added = fix;
                    added.from               = from;
                    fixes.append(added);
                }
            }
        }
    }
    return back;
}

void DarkMode::recolorDoc(Doc& d)
{
    if (!d.doc)
        return;
    const Maps maps = mapsFor(d.sheet, d.darkSheet, d.info);
    const bool was  = m_applying;
    m_applying      = true;
    {
        layoutformat::MutatingScope scope; // ChatIntegration, ChatEmoji and ChatLayout don't rescan colours
        m_stats.fragments += darkchat::recolor(d.doc, maps.forward, true);
    }
    m_applying  = was;
    d.recolored = true;
    d.pending   = false;
    d.dirtyFrom = -1;
    d.dirtyTo   = -1;
}

void DarkMode::recolorPending()
{
    if (!m_applied || m_paused)
        return;
    while (!m_pending.isEmpty()) {
        const QPointer<QTextDocument> doc = m_pending.takeFirst();
        const auto                    it  = doc ? m_docs.find(doc.data()) : m_docs.end();
        if (it == m_docs.end() || !it->pending)
            continue;
        recolorDoc(it.value());
        break; // one per turn
    }
    if (!m_pending.isEmpty())
        m_slice->start();
}

void DarkMode::onContentsChange(QTextDocument* doc, int position, int removed, int added)
{
    if (!m_applied || m_paused)
        return;
    const auto it = m_docs.find(doc);
    if (it == m_docs.end())
        return;
    Doc&      d     = it.value();
    const int delta = added - removed;
    if (d.dirtyFrom >= 0 && delta != 0) { // the waiting range stays on its text
        if (position + removed <= d.dirtyFrom) {
            d.dirtyFrom = qMax(0, d.dirtyFrom + delta);
            d.dirtyTo   = qMax(d.dirtyFrom, d.dirtyTo + delta);
        } else if (position < d.dirtyTo) {
            d.dirtyTo = qMax(d.dirtyFrom, d.dirtyTo + delta);
        }
    }
    // 2.7.3 (M8): the lines TeamSpeak printed under a sheet of its own stay on their text (followEdit).
    const bool ours = m_applying || layoutformat::mutating();
    for (int i = d.printed.size() - 1; i >= 0; --i) {
        Doc::Printed& p = d.printed[i];
        followEdit(&p.from, &p.to, position, removed, added, !ours);
        if (p.to <= p.from)
            d.printed.removeAt(i);
    }
    if (ours || added <= 0)
        return; // our own edits and the chat layout's
    // Review fix (2.7.3): printed under a sheet TeamSpeak set itself, not under the dark one (a style picked: TeamSpeak sets
    // every chat's sheet, and a line can come in before dark mode takes the style over; a chat dark mode saw before TeamSpeak
    // gave it its sheet). Off gives those lines the style's look from that sheet's.
    const QString sheet = doc->defaultStyleSheet();
    if (!sheet.isEmpty() && !(sheet.constData() == d.darkSheet.constData() || sheet == d.darkSheet)) {
        bool merged = false;
        for (Doc::Printed& p : d.printed) {
            if (position <= p.to && position + added >= p.from && p.sheet == sheet) {
                p.from = qMin(p.from, position);
                p.to   = qMax(p.to, position + added);
                merged = true;
                break;
            }
        }
        if (!merged)
            d.printed.append({sheet, position, position + added});
    }
    d.dirtyFrom = d.dirtyFrom < 0 ? position : qMin(d.dirtyFrom, position);
    d.dirtyTo   = qMax(d.dirtyTo, position + added);
    if (!d.pending)
        m_fresh->start(); // ahead of the paint (a 0 ms timer runs before the low-priority update request)
}

void DarkMode::recolorFresh()
{
    if (!m_applied || m_paused)
        return;
    for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
        Doc& d = it.value();
        if (d.pending || d.dirtyFrom < 0 || !d.doc)
            continue;
        if (d.doc->defaultStyleSheet() != d.darkSheet) {
            // TeamSpeak gave it its skin's sheet after ours (a brand-new chat, a skin change): that is its sheet now.
            takeSheet(d, d.doc->defaultStyleSheet());
            d.doc->setDefaultStyleSheet(d.darkSheet);
        }
        const int  from = d.doc->findBlock(d.dirtyFrom).position();
        const int  to   = d.dirtyTo;
        const Maps maps = mapsFor(d.sheet, d.darkSheet, d.info);
        d.dirtyFrom     = -1;
        d.dirtyTo       = -1;
        m_applying      = true;
        {
            layoutformat::MutatingScope scope;
            darkchat::recolor(d.doc, maps.forward, true, from, to);
        }
        m_applying = false;
    }
}

void DarkMode::checkDocuments()
{
    if (!m_applied || m_paused)
        return;
    for (auto it = m_docs.begin(); it != m_docs.end();) {
        if (!it->doc)
            it = m_docs.erase(it);
        else
            ++it;
    }
    for (QTextEdit* view : findViews())
        track(view); // a new chat, or a chat with a new document
}

bool DarkMode::isTracked(QTextDocument* doc) const
{
    return m_docs.contains(doc);
}

bool DarkMode::isPending(QTextDocument* doc) const
{
    const auto it = m_docs.constFind(doc);
    return it != m_docs.constEnd() && it->pending;
}

// ---- places (design_dark 1.10) -----------------------------------------------------------------------------------------

QVector<DarkMode::Place> DarkMode::capturePlaces(const QList<QTextEdit*>& views) const
{
    QVector<Place> out;
    for (QTextEdit* v : views) {
        auto* browser = qobject_cast<QTextBrowser*>(v);
        if (!browser || !named(browser, "ChatTab") || browser->document()->characterCount() <= 1)
            continue;
        // A hidden chat still waiting for its Show keeps the place it had (it isn't laid out for its size yet).
        const auto waiting = m_deferred.constFind(browser);
        if (waiting != m_deferred.constEnd() && waiting->browser == browser) {
            out.append(waiting.value());
            continue;
        }
        out.append({browser, chatscroll::captureReader(browser)});
    }
    return out;
}

void DarkMode::restorePlaces(const QVector<Place>& places, bool settleHidden)
{
    for (auto it = m_deferred.begin(); it != m_deferred.end();) { // chats gone meanwhile (their filter went with them)
        if (!it->browser)
            it = m_deferred.erase(it);
        else
            ++it;
    }
    if (places.isEmpty())
        return;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest); // tab bar and input heights final
    for (const Place& p : places) {
        QTextBrowser* b = p.browser;
        if (!b)
            continue;
        if (!b->isVisible() && b->testAttribute(Qt::WA_PendingResizeEvent)) {
            if (m_deferHidden && !settleHidden) {
                // Review fix: not laid out now (a long hidden chat costs a whole layout); Qt does that just before its
                // Show, where its place is put back (eventFilter).
                if (!m_deferred.contains(b))
                    b->installEventFilter(this); // once (a second filter would see the Show twice)
                m_deferred.insert(b, p);
                continue;
            }
            // What Qt does at its show: the document laid out for its new width now (Qt's own resize later changes nothing).
            QResizeEvent resize(b->size(), QSize());
            QCoreApplication::sendEvent(b, &resize);
        }
        if (m_deferred.remove(b) > 0)
            b->removeEventFilter(this);
        chatscroll::restoreReader(b, p.place);
    }
}

void DarkMode::settleDeferred()
{
    const QHash<QObject*, Place> waiting = m_deferred;
    m_deferred.clear();
    QVector<Place> places;
    for (const Place& p : waiting) {
        if (!p.browser)
            continue;
        p.browser->removeEventFilter(this);
        places.append(p);
    }
    restorePlaces(places, true);
}

// ---- tool tips (design_dark 1.6) ---------------------------------------------------------------------------------------

void DarkMode::hookTip(QWidget* label)
{
    QTextDocument* doc = label ? label->findChild<QTextDocument*>() : nullptr;
    if (!doc)
        return; // a plain text tip, or another Qt: the tip is dark with TeamSpeak's colours
    // Qt deletes a tip's label when it hides and makes a new one for the next tip: the ones gone are dropped (review fix).
    bool known = false;
    for (int i = m_tips.size() - 1; i >= 0; --i) {
        if (!m_tips.at(i)) {
            QObject::disconnect(m_tipConnections.at(i)); // nothing left to disconnect: the record goes
            m_tips.removeAt(i);
            m_tipConnections.removeAt(i);
        } else if (m_tips.at(i) == doc) {
            known = true;
        }
    }
    if (!known) {
        m_tips.append(doc);
        m_tipConnections.append(connect(doc, &QTextDocument::contentsChanged, this, [this] {
            if (m_applied && !m_paused && !m_applying)
                m_tip->start();
        }));
    }
    m_tip->start();
}

void DarkMode::recolorTips()
{
    if (!m_applied || m_paused)
        return;
    m_applying = true;
    for (const QPointer<QTextDocument>& tip : qAsConst(m_tips)) {
        if (tip)
            darkchat::recolor(tip, m_tipMap, true); // the label's text stays TeamSpeak's (the tip doesn't move)
    }
    m_applying = false;
}

// ---- title bars (design_dark 1.7) --------------------------------------------------------------------------------------

void DarkMode::titleBar(QWidget* window)
{
    if (!takesTitleBar(window))
        return;
    const quintptr id = static_cast<quintptr>(window->internalWinId());
    for (int i = m_bars.size() - 1; i >= 0; --i) { // windows gone since (review fix: the list stays the live ones)
        if (!m_bars.at(i).first)
            m_bars.removeAt(i);
    }
    for (auto& bar : m_bars) {
        if (bar.first == window) {
            if (bar.second != id) { // a new handle (WinIdChange)
                bar.second = id;
                darktitlebar::apply(id, barColors());
            }
            return;
        }
    }
    darktitlebar::apply(id, barColors());
    m_bars.append({window, id});
}

void DarkMode::revertTitleBars()
{
    for (const auto& bar : qAsConst(m_bars)) {
        if (bar.first && static_cast<quintptr>(bar.first->internalWinId()) == bar.second)
            darktitlebar::revert(bar.second);
    }
    m_bars.clear();
}

// ---- the application event filter --------------------------------------------------------------------------------------

bool DarkMode::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::StyleChange:
    case QEvent::ApplicationPaletteChange:
        // TeamSpeak (or someone) set another sheet or palette: taken over on the next turn, never inside the event.
        if (!m_applying && m_applied && !m_paused && watched->isWidgetType() && static_cast<QWidget*>(watched)->isWindow())
            m_adopt->start();
        break;
    case QEvent::Show:
        if (!m_applying && watched->isWidgetType()) {
            auto* w = static_cast<QWidget*>(watched);
            if (w->isWindow())
                titleBar(w); // before Windows shows it: no light caption flashes
            if (m_barsGuessed && m_applied && !m_paused && w->isWindow()) {
                // Live test fix (2.7.3, M6): TeamSpeak's main window at its first Show after a start with dark mode on: its
                // own bars measured in the skin on the next turn (before its first paint), not the probe's.
                auto* main = qobject_cast<QMainWindow*>(w);
                if (barsBuilt(main)) {
                    m_barsGuessed  = false;
                    m_styleMeasure = main;
                    m_repane->start();
                }
            }
            if (isTipLabel(w)) {
                hookTip(w);
            } else if (isThemedView(w) && m_applied && !m_paused) {
                // Live test fix (2.7.3): only while dark mode is on. After off a hidden chat whose place waits keeps this
                // filter until its Show (restorePlaces), and that Show tracked it again: dark mode's sheet and colours on
                // every line, also those after off, and a stale entry into the next on and off (2.7.2 the same).
                auto* view = static_cast<QTextEdit*>(w);
                track(view);
                const auto it = m_docs.find(view->document());
                if (it != m_docs.end() && it->pending)
                    recolorDoc(it.value()); // before its first paint
            }
        }
        break;
    case QEvent::WinIdChange:
        if (!m_applying && watched->isWidgetType() && static_cast<QWidget*>(watched)->isWindow())
            titleBar(static_cast<QWidget*>(watched));
        break;
    case QEvent::Polish:
        if (!m_applying && watched->isWidgetType()) {
            if (isTipLabel(watched))
                hookTip(static_cast<QWidget*>(watched));
            else if (isThemedView(watched) && m_applied && !m_paused) // live test fix: not after off (the Show above)
                track(static_cast<QTextEdit*>(watched)); // a new chat: themed before its first paint
        }
        break;
    default:
        break;
    }
    if (event->type() == QEvent::Show && !m_deferred.isEmpty()) {
        // A hidden chat whose place waits (review fix): Qt delivered its pending resize just before its Show, so it is laid
        // out for its size now (and recoloured above, if it was waiting for that too); its place back before its first paint.
        const auto waiting = m_deferred.find(watched);
        if (waiting != m_deferred.end()) {
            const Place place = waiting.value();
            m_deferred.erase(waiting);
            watched->removeEventFilter(this);
            if (place.browser && place.browser.data() == watched)
                chatscroll::restoreReader(place.browser, place.place);
        }
    }
    return false;
}

// ---- diagnostics -------------------------------------------------------------------------------------------------------

DarkMode::Stats DarkMode::stats() const
{
    Stats s = m_stats;
    s.tracked   = 0;
    s.recolored = 0;
    s.pending   = 0;
    for (const Doc& d : qAsConst(m_docs)) {
        if (!d.doc)
            continue;
        ++s.tracked;
        s.recolored += d.recolored ? 1 : 0;
        s.pending += d.pending ? 1 : 0;
    }
    s.titleBars = 0;
    for (const auto& bar : m_bars)
        s.titleBars += bar.first ? 1 : 0;
    s.tips = 0;
    for (const QPointer<QTextDocument>& tip : m_tips)
        s.tips += tip ? 1 : 0;
    return s;
}

QStringList DarkMode::diagnosticLines() const
{
    QStringList out;
    if (m_paused)
        out << i18n::t("Dark mode: on, paused (the style sheet kept changing)");
    else
        out << (m_applied ? i18n::t("Dark mode: on") : i18n::t("Dark mode: off"));
    if (m_applied) {
        out << i18n::t("Style sheet: %1 KB, scale %2%, glyphs %3")
                   .arg(QString::number(m_sheet.size() / 1024.0, 'f', 1))
                   .arg(qRound(m_k * 100))
                   .arg(m_glyphDir.isEmpty() ? i18n::t("unavailable") : i18n::t("ok"));
    }
    const Stats s = stats();
    out << i18n::t("Skin changes taken over: %1; chats recolored: %2 (waiting: %3); title bars: %4").arg(s.adoptions).arg(s.recolored).arg(s.pending).arg(s.titleBars);
    return out;
}
