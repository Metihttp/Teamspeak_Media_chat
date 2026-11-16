#pragma once

// 2.7 Dark mode (design_dark): all of TeamSpeak in TS Media's warm dark theme, and TeamSpeak's own skin back exactly.
//
// On: the application palette (darkpalette::palette), then the application style sheet (darkpalette::appSheet, which
// replaces the skin's), then each TeamSpeak chat, info panel and poke window document (ChatTab, InfoFrame, the poke
// window's msgTextBrowser): its default style sheet (new lines) and the colours already in it (darkchat::recolor, visible
// chats at once, hidden ones one per event-loop turn and always before their first paint), the title bars of TeamSpeak's
// windows (darktitlebar). Every chat keeps its reader's place (chatscroll's captureReader / restoreReader around each
// change; once the chat layer runs, a hidden chat Qt still has to lay out gets its place at its Show; after on and off,
// placesRestored lets a scale pass the switch started keep that place). Never touched: qApp->style(), any font (the
// skin's own font rules go after the dark sheet), TeamSpeak's settings and skin choice, Windows settings.
// Off (and shutdown on a Disable or Reload): the documents first (colours back, then their default sheets), then the
// palette, then the sheet (the skin reads palette(base) while it polishes), the title bars, each exactly as they were (the
// snapshot). Each of on, off and a skin taken over says so (themeChanged, 2.7.1) inside its place bracket: the chat layer
// draws TS Media's pictures for the new look then, also the ones a Disable or Reload leaves in TeamSpeak's chats.
// Shutdown at TeamSpeak's Quit (its main window no longer shown, or after aboutToQuit) puts nothing back: it
// would repolish windows being torn down (a crash in Qt's style sheet code); only the hooks go, and the dark sheet,
// palette, document sheets, recoloured fragments and title bars (heap copies, per-window attributes) stay until TeamSpeak
// exits.
// While on, an application event filter (cheap type checks, never consumes) follows TeamSpeak: another skin picked in
// Options → Design becomes the snapshot and dark mode is applied again on the next turn (at most 5 times in 10 s, then it
// pauses); new chat tabs and documents are themed before their first paint; new windows get the dark title bar before
// Windows shows them; tool tips get TeamSpeak's template colours remapped in their document.
//
// CRASH RULE: every sheet, document sheet and glyph path is built at run time (darkpalette, darkglyphs); owned timers
// only (children, single shot, 0 ms); connections take this object as context; kOriginal holds a uint; shutdown() leaves
// no hook of ours in Qt's state (filters, connections and the hidden chats' event filters removed; the glyph files on disk
// aren't Qt state; at Quit the theme's own state stays, none of it in the DLL's image). GUI thread only.

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPalette>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "chatcolors.h"
#include "chatscroll.h"
#include "darkchat.h"
#include "darkpalette.h"

class QTextBrowser;
class QTextDocument;
class QTextEdit;
class QTimer;
class QWidget;

class DarkMode : public QObject
{
    Q_OBJECT

  public:
    explicit DarkMode(QObject* parent = nullptr);
    ~DarkMode() override; // shutdown()

    // Applied and not paused (GUI thread). TS Media's own windows and chat colours ask this.
    static bool      isActive();
    static DarkMode* instance(); // the one alive (the live-test driver, diagnostics)

    bool enabled() const { return m_enabled; }
    bool paused() const { return m_paused; }
    // On or off at once (idempotent): the whole apply or remove, the places kept.
    void setEnabled(bool on);
    // Plugin unload. Disable or Reload (TeamSpeak's main window shown): everything back and nothing left. TeamSpeak's
    // Quit (the main window no longer shown, or after aboutToQuit): only the hooks go, the theme stays on what is left.
    // Idempotent; the object can be deleted afterwards.
    void shutdown();
    // TeamSpeak is closing (its Quit), the test shutdown() makes (2.7.1: plugin.cpp asks it once at the unload, so the
    // chat layer draws nothing at Quit either, whether dark mode is on or off): aboutToQuit came, Qt is closing down, or
    // TeamSpeak's main window isn't shown (it hides it before it lets the plugins go). A Disable or Reload is clicked in
    // Options over the main window, which is shown then (minimized counts as shown). mainShown, othersShown: what it saw
    // (the log).
    bool teamSpeakClosing(bool* mainShown = nullptr, int* othersShown = nullptr) const;
    // Where the glyph folder goes (default: TeamSpeak's config folder). Tests point it elsewhere; "" turns glyphs off.
    void setGlyphBase(const QString& dir);
    // On (plugin.cpp, once the chat layer runs; review fix): a toggle or a skin change doesn't lay out hidden chats for
    // their new size; each one's place waits for its Show (Qt lays it out then anyway, just before). Off (the default: the
    // load at TeamSpeak's start or a reload, before the chat layer's own place keeping) and always at shutdown: they are
    // laid out and placed at once.
    void setDeferHiddenPlaces(bool on);

    QStringList diagnosticLines() const;
    struct Stats {
        int    adoptions   = 0;
        int    tracked     = 0; // documents
        int    recolored   = 0; // documents recoloured
        int    pending     = 0; // hidden documents waiting
        int    titleBars   = 0;
        int    tips        = 0;
        int    fragments   = 0; // fragments recoloured by the last apply (visible documents)
        int    sheetChars  = 0;
        int    widgets     = 0; // at the last apply
        qint64 lastApplyMs = -1;
        qint64 lastRemoveMs = -1;
        qint64 lastSheetMs = -1;
        qint64 lastChatsMs = -1;
        qint64 lastCardsMs = -1; // 2.7.1: the last themeChanged's receivers (TS Media's pictures drawn again)
    };
    Stats stats() const;

    // Tests and the live-test driver.
    qreal                scale() const { return m_k; }
    darkpalette::Metrics metrics() const { return m_snap.metrics; }
    QString              glyphDir() const { return m_glyphDir; }
    QString              appliedSheet() const { return m_sheet; }
    QString              chatSheetText() const { return m_chatSheet; }
    QString              snapshotSheet() const { return m_snap.sheet; }
    QPalette             snapshotPalette() const { return m_snap.palette; }
    bool                 isTracked(QTextDocument* doc) const;
    bool                 isPending(QTextDocument* doc) const;
    bool                 hasEventFilter() const { return m_filtering; }
    int                  timersActive() const;
    int                  tipEntries() const { return m_tips.size(); } // live and dead (review fix: the dead are dropped)
    int                  barEntries() const { return m_bars.size(); }
    int                  deferredPlaces() const { return m_deferred.size(); } // hidden chats whose place waits for their Show
    // 2.7.1: this hidden chat's place waits for its Show (ChatIntegration's ShowPlace: that place wins over the end it read).
    bool                 placeWaits(QObject* browser) const { return m_deferred.contains(browser); }

  public slots:
    // ChatIntegration found a new chat or a swapped document (chatsChanged): theme what isn't yet.
    void checkDocuments();

  signals:
    void enabledChanged(bool on);
    // Live test: every chat's reader is back in place after the theme went on or off (places taken before anything changed;
    // ChatIntegration::retakeScalePlaces then lets a scale pass the switch started keep that place: ChatScale::retake). Not
    // after a skin change (review fix): those places are read once TeamSpeak's skin has moved the chats, and the pass's own
    // place, taken at the chat's first event, is the right one.
    void placesRestored();
    // 2.7.1: TeamSpeak's look changed (dark mode on or off, a skin taken over while on, and the off of a Disable or
    // Reload): emitted inside the place bracket, once the palette, the sheet and the documents are final and the layouts
    // settled, before the places are put back. The chat layer draws its pictures again for the new look then (they stay in
    // TeamSpeak's chats after an unload). Never at TeamSpeak's Quit (nothing is put back there).
    void themeChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // false always

  private:
    struct Doc {
        QPointer<QTextEdit>     view;
        QPointer<QTextDocument> doc;
        QString                 sheet; // its default style sheet before dark mode (the skin's _chat.qss)
        bool                    recolored = false;
        bool                    pending   = false;
        int                     dirtyFrom = -1; // lines added while dark, not recoloured yet
        int                     dirtyTo   = -1;
        QMetaObject::Connection changes;
    };
    struct Maps {
        darkchat::ColorMap forward;
        darkchat::ColorMap back;
    };
    struct Snapshot {
        QString              sheet;
        QPalette             palette;
        darkpalette::Metrics metrics;
        chatcolors::Colors   chat; // the visible chat's colours under the skin (TS Media's line colours were made for them)
    };
    struct Place {
        QPointer<QTextBrowser>  browser;
        chatscroll::ReaderPlace place;
    };

    void apply();
    void remove();
    void pause();
    void adoptSkin();
    void buildTheme();
    void installFilter(bool on);
    void stopTimers();
    QList<QTextEdit*> findViews() const;
    void track(QTextEdit* view);
    void recolorDoc(Doc& d);
    void onContentsChange(QTextDocument* doc, int position, int removed, int added);
    void recolorPending();
    void recolorFresh();
    void recolorTips();
    void hookTip(QWidget* label);
    void titleBar(QWidget* window);
    void revertTitleBars();
    Maps mapsFor(const QString& sheet);
    QVector<Place> capturePlaces(const QList<QTextEdit*>& views) const;
    void restorePlaces(const QVector<Place>& places, bool settleHidden = false);
    void settleDeferred(); // every waiting place now (shutdown)
    darkpalette::Metrics measure(const QList<QTextEdit*>& views) const;
    chatcolors::Colors chatColorsNow(const QList<QTextEdit*>& views) const;
    void restorePalette();
    void announceTheme(); // themeChanged, timed (inside the place bracket)

    bool m_enabled   = false;
    bool m_applied   = false;
    bool m_paused    = false;
    bool m_applying  = false;
    bool m_filtering = false;
    bool m_shutDown  = false;
    bool m_quitting  = false; // QCoreApplication::aboutToQuit seen: TeamSpeak is closing (shutdown() then puts nothing back)
    bool m_glyphsLogged = false;

    Snapshot m_snap;
    QString  m_glyphBase;
    bool     m_glyphBaseSet = false;
    QString  m_glyphDir;
    qreal    m_k = 1.0;
    QString  m_sheet;     // the application sheet we set (a shared copy of what QApplication keeps)
    QString  m_chatSheet; // the documents' sheet we set
    QPalette m_palette;   // the application palette we set (as QApplication keeps it)
    QVector<QPair<QByteArray, darkchat::Look>> m_darkLooks;
    QVector<QPair<darkchat::Look, QRgb>>       m_extrasForward;
    QVector<QPair<darkchat::Look, QRgb>>       m_extrasBack;
    QVector<QPair<darkchat::Look, QRgb>>       m_labelsBack; // 2.7.1: TS Media's line label on the way back
    QHash<QString, Maps>                       m_maps;
    darkchat::ColorMap                         m_tipMap;

    QHash<QTextDocument*, Doc>        m_docs;
    QList<QPointer<QTextDocument>>    m_pending;
    QList<QPointer<QTextDocument>>    m_tips;
    QList<QMetaObject::Connection>    m_tipConnections;
    QList<QPair<QPointer<QWidget>, quintptr>> m_bars;
    bool                                      m_deferHidden = false;
    QHash<QObject*, Place>                    m_deferred; // hidden chats (their own event filter: our app filter goes at off)

    QTimer* m_adopt = nullptr; // a skin change seen: adopt it on the next turn
    QTimer* m_slice = nullptr; // one hidden document per turn
    QTimer* m_fresh = nullptr; // lines added while dark
    QTimer* m_tip   = nullptr; // a tool tip's document

    QElapsedTimer  m_clock;
    QList<qint64>  m_adoptTimes;
    Stats          m_stats;
};
