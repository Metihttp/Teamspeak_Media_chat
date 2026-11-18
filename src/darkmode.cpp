#include "darkmode.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionTabWidgetFrame>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
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
    m_chatSheet = darkpalette::chatSheet(t);
    m_palette   = darkpalette::palette(m_snap.palette);
    m_darkClasses = darkchat::classFormats(m_chatSheet, t.link);
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

DarkMode::Maps DarkMode::mapsFor(const QString& sheet, bool info)
{
    const QHash<QString, Maps>& cache = info ? m_infoMaps : m_maps;
    const auto                  known = cache.constFind(sheet);
    if (known != cache.constEnd())
        return known.value();
    const darkpalette::Tokens& t = darkpalette::tokens();
    // The skin's sheet as it was when its lines came in: its palette(link) is the skin's link, not ours.
    const auto from = darkchat::classFormats(darkchat::resolvePaletteRefs(sheet, m_snap.palette), m_snap.palette.color(QPalette::Active, QPalette::Link));
    // 2.7.2 (D9): the chats' maps and the info panel's (its classes first, a label without a colour in the skin loses dark
    // mode's), both made at once from the same class formats.
    for (const bool panel : {false, true}) {
        Maps maps;
        maps.forward = darkchat::buildMap(from, m_darkClasses, m_extrasForward, t.window, {}, panel);
        maps.back    = darkchat::buildMap(m_darkClasses, from, m_extrasBack, QColor(), m_labelsBack, panel);
        (panel ? m_infoMaps : m_maps).insert(sheet, maps);
    }
    return (info ? m_infoMaps : m_maps).value(sheet);
}

// 2.7.2 (live test): TeamSpeak's dark-mode files, from its install folder's styles and the config folder's (a custom
// style's). Read when dark mode goes on and when it takes over a skin (a new style may bring its own).
void DarkMode::readTeamSpeakDarkFiles()
{
    m_teamSpeakDark.clear();
    QStringList folders{QCoreApplication::applicationDirPath() + QString::fromLatin1("/styles")};
    const QString config = ts3::configDir();
    if (!config.isEmpty())
        folders << config + QString::fromLatin1("/styles");
    for (const QString& folder : qAsConst(folders)) {
        const QDir dir(folder);
        for (const QString& name : dir.entryList({QString::fromLatin1("*_darkmode.qss")}, QDir::Files, QDir::Name)) {
            QFile file(dir.filePath(name));
            if (file.size() <= 0 || file.size() > (1 << 20) || !file.open(QIODevice::ReadOnly))
                continue;
            const QString text = QString::fromUtf8(file.readAll());
            QString       lf   = text;
            lf.replace(QString::fromLatin1("\r\n"), QString::fromLatin1("\n"));
            for (const QString& t : {text, lf}) {
                if (!t.trimmed().isEmpty() && !m_teamSpeakDark.contains(t))
                    m_teamSpeakDark << t;
            }
        }
    }
}

// sheet without what TeamSpeak added of its dark-mode files: "\n" + a file's text at the end (where TeamSpeak puts it), or
// followed by more of the sheet; each as often as it is there. A sheet that only is such a text (a skin made from one) stays.
QString DarkMode::withoutTeamSpeakDark(QString sheet) const
{
    for (bool again = true; again;) {
        again = false;
        for (const QString& text : m_teamSpeakDark) {
            const QString piece = QLatin1Char('\n') + text;
            if (sheet.size() > piece.size() && sheet.endsWith(piece)) {
                sheet.chop(piece.size());
                again = true;
            } else {
                const int at = sheet.indexOf(piece + QLatin1Char('\n'));
                if (at > 0) {
                    sheet.remove(at, piece.size());
                    again = true;
                }
            }
        }
    }
    return sheet;
}

darkchat::ColorMap DarkMode::ownSheetMap(const QString& made, const QString& own, bool info) const
{
    const QColor link = m_snap.palette.color(QPalette::Active, QPalette::Link);
    const auto   from = darkchat::classFormats(darkchat::resolvePaletteRefs(made, m_snap.palette), link);
    const auto   to   = darkchat::classFormats(darkchat::resolvePaletteRefs(own, m_snap.palette), link);
    return darkchat::buildMap(from, to, {}, QColor(), {}, info);
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
static bool paneInsets(QTabWidget* tabs, int* left, int* right)
{
    auto* stack = tabs ? tabs->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
    if (!stack)
        return false;
    tabs->ensurePolished();
    QRect outer, inner;
    if (tabs->isVisible()) {
        outer = tabs->rect();
        inner = stack->geometry(); // QTabWidget lays a shown one out at once on a new style (its StyleChange)
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
    }
    if (!inner.isValid() || inner.width() <= 0)
        return false;
    *left  = qBound(0, inner.left() - outer.left(), 64);
    *right = qBound(0, outer.right() - inner.right(), 64);
    return true;
}

// 2.7.2 (D3): the chat's tab widget pane, named and measured; nothing for one no selector can name alone.
static void measurePane(QTextEdit* chat, darkpalette::Metrics* m)
{
    QTabWidget*   tabs     = chatTabsOf(chat);
    const QString selector = paneSelectorOf(tabs);
    int           left = 0, right = 0;
    if (selector.isEmpty() || !paneInsets(tabs, &left, &right))
        return;
    m->paneSelector = selector;
    m->paneLeft     = left;
    m->paneRight    = right;
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
        tabsProbe.addTab(new QWidget, QString::fromLatin1("Lobby"));
        int left = 0, right = 0;
        if (paneInsets(&tabsProbe, &left, &right)) {
            m.paneSelector = teamSpeaks;
            m.paneLeft     = left;
            m.paneRight    = right;
            m.paneProbed   = true;
        }
        return m;
    }
    chat->ensurePolished();
    chat->verticalScrollBar()->ensurePolished();
    m.chatFrame = chat->frameWidth();
    m.chatBar   = qMax(1, chat->verticalScrollBar()->sizeHint().width());
    measurePane(chat, &m);
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
    m_snap.sheet                  = withoutTeamSpeakDark(qApp->styleSheet());
    m_snap.palette                = QApplication::palette();
    m_snap.metrics                = measure(views);
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
    for (QTextEdit* view : views)
        track(view);
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
    {
        layoutformat::MutatingScope scope;
        QHash<QString, darkchat::ColorMap> ownMaps; // 2.7.2 (live test): per sheet a chat was made with, and chat or info panel
        for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
            Doc& d = it.value();
            QObject::disconnect(d.changes);
            if (!d.doc)
                continue;
            const Maps maps = mapsFor(d.sheet, d.info);
            if (d.recolored)
                darkchat::recolor(d.doc, maps.back, false);
            else if (d.dirtyFrom >= 0)
                darkchat::recolor(d.doc, maps.back, false, d.dirtyFrom, d.dirtyTo);
            // 2.7.2 (live test): a chat TeamSpeak made while dark mode was on (a new server tab or private chat, every chat of
            // a start with dark mode on) has TeamSpeak's dark-mode file in its sheet, and its lines that file's look: the
            // skin's own sheet goes back (else every later line kept those colours, light blue on white), and the lines get
            // its look, as TeamSpeak gives a chat it makes with the skin's palette.
            const QString own = withoutTeamSpeakDark(d.sheet);
            if (own != d.sheet) {
                const QString key = d.sheet + (d.info ? QLatin1Char('1') : QLatin1Char('0'));
                if (!ownMaps.contains(key))
                    ownMaps.insert(key, ownSheetMap(d.sheet, own, d.info));
                darkchat::recolor(d.doc, ownMaps.value(key), false);
            }
            d.doc->setDefaultStyleSheet(own);
        }
        // 2.7.2 (D6): each view's own character format (QTextEdit::append keeps the one it gave the new block as the cursor's,
        // and copies it into every block it appends later): one that holds kOriginal is mapped back too. Only without a
        // selection (with one, setCurrentCharFormat would format the selected text); no document edit, no scroll.
        for (auto it = m_docs.cbegin(); it != m_docs.cend(); ++it) {
            const Doc& d = it.value();
            if (!d.view || !d.doc || d.view->document() != d.doc || d.view->textCursor().hasSelection())
                continue;
            QTextCharFormat cursor = d.view->currentCharFormat();
            if (cursor.hasProperty(darkchat::kOriginal) && darkchat::mapBack(cursor, mapsFor(d.sheet, d.info).back))
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
    m_applied  = false;
    m_paused   = false;
    m_applying = false;
    m_stats.lastRemoveMs = clock.elapsed();
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
        if (it->doc && it->doc->defaultStyleSheet() != m_chatSheet)
            docsChanged << it->doc.data();
    }
    if (!sheetChanged && !paletteChanged && docsChanged.isEmpty())
        return; // our own change (a late StyleChange): nothing to take over
    if (!sheetChanged && !paletteChanged) {
        // Only documents TeamSpeak gave its skin's sheet again (a new chat): theirs is the one to give back, ours goes on.
        // No repolish, and not a fight over the application sheet.
        for (QTextDocument* doc : qAsConst(docsChanged)) {
            m_docs[doc].sheet = doc->defaultStyleSheet();
            doc->setDefaultStyleSheet(m_chatSheet);
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
        // tab names white on a light tab bar after off): the skin's sheet is the one without it.
        readTeamSpeakDarkFiles();
        m_snap.sheet   = withoutTeamSpeakDark(sheet);
        m_snap.metrics = measure(views); // polished with the new skin at this moment
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
    for (QTextDocument* doc : qAsConst(docsChanged))
        m_docs[doc].sheet = doc->defaultStyleSheet(); // the new skin's _chat.qss
    buildTheme();
    const QVector<Place> places = capturePlaces(views);
    QApplication::setPalette(m_palette);
    m_palette = QApplication::palette();
    qApp->setStyleSheet(m_sheet);
    m_sheet = qApp->styleSheet();
    for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
        if (it->doc && it->doc->defaultStyleSheet() != m_chatSheet)
            it->doc->setDefaultStyleSheet(m_chatSheet);
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
    if (m_applied && m_snap.metrics.paneProbed && named(view, "ChatTab"))
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
    d.changes = connect(doc, &QTextDocument::contentsChange, this,
                        [this, doc](int position, int removed, int added) { onContentsChange(doc, position, removed, added); });
    doc->setDefaultStyleSheet(m_chatSheet);
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
void DarkMode::confirmPane(QTextEdit* chat)
{
    QTabWidget* tabs = chatTabsOf(chat);
    if (!tabs)
        return; // not in its tab widget yet: its Show tells
    m_snap.metrics.paneProbed = false;
    const QString selector    = paneSelectorOf(tabs);
    if (selector == m_snap.metrics.paneSelector)
        return;
    m_snap.metrics.paneSelector = selector;
    m_repane->start();
}

void DarkMode::repane()
{
    if (!m_applied || m_paused || m_applying)
        return;
    QElapsedTimer clock;
    clock.start();
    m_applying                     = true;
    const QList<QTextEdit*> views  = findViews();
    const QVector<Place>    places = capturePlaces(views);
    m_sheet                        = sheetNow();
    qApp->setStyleSheet(m_sheet);
    m_sheet = qApp->styleSheet();
    restorePlaces(places);
    m_applying         = false;
    m_stats.sheetChars = m_sheet.size();
    logPane();
    logLine(QString::fromLatin1("[dark] the sheet again for the first chat's tab widget in %1 ms").arg(clock.elapsed()));
}

void DarkMode::recolorDoc(Doc& d)
{
    if (!d.doc)
        return;
    const Maps maps = mapsFor(d.sheet, d.info);
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
    if (m_applying || layoutformat::mutating() || added <= 0)
        return; // our own edits and the chat layout's
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
        if (d.doc->defaultStyleSheet() != m_chatSheet) {
            // TeamSpeak gave it its skin's sheet after ours (a brand-new chat, a skin change): that is its sheet now.
            d.sheet = d.doc->defaultStyleSheet();
            d.doc->setDefaultStyleSheet(m_chatSheet);
        }
        const int  from = d.doc->findBlock(d.dirtyFrom).position();
        const int  to   = d.dirtyTo;
        const Maps maps = mapsFor(d.sheet, d.info);
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
            if (isTipLabel(w)) {
                hookTip(w);
            } else if (isThemedView(w)) {
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
            else if (isThemedView(watched))
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
