#include "darkmode.h"

#include <QApplication>
#include <QEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
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
    connect(m_adopt, &QTimer::timeout, this, &DarkMode::adoptSkin);
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
    m_sheet = darkpalette::appSheet(t, m_k, m_snap.metrics, m_glyphDir);
    // The skin's own fonts after ours (review fix): the dark sheet has none, and Qt gives a widget only the fonts of the
    // sheet in use, so without them TeamSpeak's headlines and banners (and a custom skin's client or chat font) would
    // change while dark mode is on.
    const QString fonts = darkpalette::fontRules(m_snap.sheet);
    if (!fonts.isEmpty())
        m_sheet += QString::fromLatin1("\n/* the skin's fonts */\n") + fonts;
    m_chatSheet = darkpalette::chatSheet(t);
    m_palette   = darkpalette::palette(m_snap.palette);
    m_darkLooks = darkchat::classLooks(m_chatSheet, t.link);
    m_tipMap    = darkchat::tipMap(t.link, t.error, t.raised);

    // TS Media's own lines (design_dark 1.5): the colours they were printed in on the skin's chat, and under dark mode.
    // Every choice of Settings -> Chat -> Text size and colors, not only the one in use now (review fix): a line printed
    // after the choice changed while dark mode was on maps back too. The one in use goes first (it wins a look two
    // choices would share).
    chatcolors::Colors dark;
    dark.base        = t.window;
    dark.text        = t.text;
    dark.link        = t.link;
    dark.dark        = true;
    dark.themeAccent = t.accent;
    m_extrasForward.clear();
    m_extrasBack.clear();
    const auto pair = [this](const QString& from, const QString& to, bool bold, bool underline) {
        const QColor a(from), b(to);
        if (!a.isValid() || !b.isValid() || a == b)
            return;
        m_extrasForward.append({darkchat::Look{a.rgba(), bold, underline}, b.rgba()});
        m_extrasBack.append({darkchat::Look{b.rgba(), bold, underline}, a.rgba()});
    };
    QStringList choices{Settings::normalizeLineColor(Settings::instance().chatLineColor), QString()};
    for (const char* name : Settings::lineColorChoices)
        choices << QString::fromLatin1(name);
    choices.removeDuplicates();
    for (int i = 0; i < choices.size(); ++i) {
        const chatcolors::LineColors before = chatcolors::lineColors(m_snap.chat, choices.at(i));
        const chatcolors::LineColors after  = chatcolors::lineColors(dark, choices.at(i));
        for (const bool bold : {true, false}) {
            for (const bool underline : {false, true}) {
                pair(before.accent, after.accent, bold, underline);
                if (i == 0) // the warning and the body don't depend on the choice
                    pair(before.warning, after.warning, bold, underline);
            }
        }
        if (i == 0 && before.wrapBody) {
            pair(before.body, after.body, false, false);
        } else if (i == 0) {
            // Live test: on the skin's chat TeamSpeak draws a plugin line without a colour of ours in dark blue
            // (chatcolors.cpp), so a line printed while dark gets that back (the way there stays the contrast rule's).
            const QColor onDark(after.body), plugin(QString::fromLatin1("#00008b"));
            if (onDark.isValid())
                m_extrasBack.append({darkchat::Look{onDark.rgba(), false, false}, plugin.rgba()});
        }
    }
    m_maps.clear();
}

DarkMode::Maps DarkMode::mapsFor(const QString& sheet)
{
    const auto known = m_maps.constFind(sheet);
    if (known != m_maps.constEnd())
        return known.value();
    const darkpalette::Tokens& t    = darkpalette::tokens();
    // The skin's sheet as it was when its lines came in: its palette(link) is the skin's link, not ours.
    const auto from = darkchat::classLooks(darkchat::resolvePaletteRefs(sheet, m_snap.palette), m_snap.palette.color(QPalette::Active, QPalette::Link));
    Maps                       maps;
    maps.forward = darkchat::buildMap(from, m_darkLooks, m_extrasForward, t.window);
    maps.back    = darkchat::buildMap(m_darkLooks, from, m_extrasBack, QColor());
    m_maps.insert(sheet, maps);
    return maps;
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
        m.chatFrame = 1;
        m.chatBar   = qApp->style() ? qApp->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 17;
        return m;
    }
    chat->ensurePolished();
    chat->verticalScrollBar()->ensurePolished();
    m.chatFrame = chat->frameWidth();
    m.chatBar   = qMax(1, chat->verticalScrollBar()->sizeHint().width());
    return m;
}

chatcolors::Colors DarkMode::chatColorsNow(const QList<QTextEdit*>& views) const
{
    QTextEdit* chat = nullptr;
    for (QTextEdit* v : views) {
        if (named(v, "ChatTab") && (!chat || (v->isVisible() && !chat->isVisible())))
            chat = v;
    }
    if (chat)
        return chatcolors::fromPalette(chat->palette());
    // No chat yet (TeamSpeak's start, review fix): one polished by the sheet in use now (the skin's: apply and adoptSkin
    // ask before ours is set), never shown and gone at once. Not a QTextBrowser (no module takes it for a chat), and
    // m_applying is set (our filter leaves its Polish alone).
    QTextEdit probe;
    probe.setObjectName(QString::fromLatin1("ChatTab"));
    probe.setAttribute(Qt::WA_DontShowOnScreen);
    probe.ensurePolished();
    return chatcolors::fromPalette(probe.palette());
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
    m_applying                    = true;
    const QList<QTextEdit*> views = findViews();
    m_snap.sheet                  = qApp->styleSheet();
    m_snap.palette                = QApplication::palette();
    m_snap.metrics                = measure(views);
    m_snap.chat                   = chatColorsNow(views);
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
    logLine(QString::fromLatin1("[dark] on in %1 ms (sheet %2 ms, chats %3 ms, %4 widgets; sheet %5 chars, scale %6%, main window %7)")
                .arg(m_stats.lastApplyMs)
                .arg(m_stats.lastSheetMs)
                .arg(m_stats.lastChatsMs)
                .arg(m_stats.widgets)
                .arg(m_stats.sheetChars)
                .arg(qRound(m_k * 100))
                .arg(main ? QString::fromLatin1("yes") : QString::fromLatin1("no")));
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
        for (auto it = m_docs.begin(); it != m_docs.end(); ++it) {
            Doc& d = it.value();
            QObject::disconnect(d.changes);
            if (!d.doc)
                continue;
            const Maps maps = mapsFor(d.sheet);
            if (d.recolored)
                darkchat::recolor(d.doc, maps.back, false);
            else if (d.dirtyFrom >= 0)
                darkchat::recolor(d.doc, maps.back, false, d.dirtyFrom, d.dirtyTo);
            d.doc->setDefaultStyleSheet(d.sheet);
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
    restorePlaces(places);
    emit placesRestored(); // live test: a scale pass the switch started keeps this place
    revertTitleBars();
    m_docs.clear();
    m_pending.clear();
    m_maps.clear();
    m_applied  = false;
    m_paused   = false;
    m_applying = false;
    m_stats.lastRemoveMs = clock.elapsed();
    logLine(QString::fromLatin1("[dark] off in %1 ms").arg(m_stats.lastRemoveMs));
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

void DarkMode::shutdown()
{
    if (m_shutDown)
        return;
    stopTimers();
    // At Quit TeamSpeak hides its main window and then lets the plugin go, before aboutToQuit (live test). A Disable or
    // Reload is clicked in Options, over the main window, which is shown then (minimized counts as shown). The main window
    // is the sign (review fix): Qt doesn't hide a window's own windows with it, and another plugin's window or one TeamSpeak
    // leaves open may still be shown at Quit.
    bool mainShown = false;
    int  othersShown = 0;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (!top->isVisible() || top->windowType() == Qt::Popup || top->windowType() == Qt::ToolTip || top->objectName().startsWith(QLatin1String("tsmedia")))
            continue;
        if (top->inherits("QMainWindow"))
            mainShown = true;
        else
            ++othersShown;
    }
    if (m_applied && (m_quitting || QCoreApplication::closingDown() || !mainShown)) {
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
    for (QTimer* t : {m_adopt, m_slice, m_fresh, m_tip}) {
        if (t)
            t->stop();
    }
}

int DarkMode::timersActive() const
{
    int n = 0;
    for (QTimer* t : {m_adopt, m_slice, m_fresh, m_tip})
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
    m_applying                    = true;
    const QList<QTextEdit*> views = findViews();
    if (sheetChanged) {
        m_snap.sheet   = sheet;
        m_snap.metrics = measure(views); // polished with the new skin at this moment
        m_snap.chat    = chatColorsNow(views);
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

void DarkMode::recolorDoc(Doc& d)
{
    if (!d.doc)
        return;
    const Maps maps = mapsFor(d.sheet);
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
        const Maps maps = mapsFor(d.sheet);
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
