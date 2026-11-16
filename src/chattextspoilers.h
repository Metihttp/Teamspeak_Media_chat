#pragma once

// 2.7 F1: text spoilers in TeamSpeak's chats (the chat module; textspoilerdoc.h changes the documents). Owned by
// ChatIntegration, which hands over every chat view (attach, documentSwapped), runs afterScan in every scan (after the
// replies, before the text reactions, link previews and the layout: a hidden link never gets a slot), forwards the
// chat viewports' events (right after ChatReplies) and the Settings change (revealSpoilers).
//  * Hiding comes before TeamSpeak paints the message: a document change that inserts "[spoiler" starts an owned 0 ms
//    timer (never an edit inside the signal); afterScan is the complete pass. The timer alone is not enough (TeamSpeak
//    can paint a chat first when one message reaches several server tabs), so the same insert also turns the chat
//    viewport's updates off (freeze; its child widgets with it; a viewport someone else froze is left alone) until
//    the raw pair is hidden: back on after the 0 ms pass, after the scan's pass when another module was editing, by the
//    owned 300 ms m_thaw at the latest, and on unload before the documents are given back. So a chat can stay unpainted
//    for up to 300 ms (ChatLayout::renderVisible and ChatIntegration::checkScale run on the viewport's Paint and wait
//    meanwhile), and whoever turns these updates on in between lets the raw pair paint.
//    Edits keep the reader's place (chatscroll::captureText / restoreText, as the scan does) and run under
//    ChatIntegration's "mutating" flag (no rescan, ChatLayout leaves them alone).
//  * A click on a bar, or "Reveal spoiler(s)" in the message's menu (the keyboard way), reveals; revealed stays
//    revealed until TeamSpeak restarts or the plugin reloads (keyed by the message's text-reaction key and the pair's
//    index). Settings::revealSpoilers on reveals everything; off again hides what the setting revealed (not what was
//    clicked) unless a link preview came into it meanwhile.
//  * The pointer over a bar: a pointing hand and the media spoilers' tool tip; the chat's own cursor back after.
//  * Unload: every document given back as TeamSpeak had it (also documents TeamSpeak swapped out), the handler
//    unregistered and deleted, the cursor back.
// QtWidgets; no ChatIntegration in it (Host), so the tests run it on TeamSpeak-like chats.

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class QAction;
class QEvent;
class QMenu;
class QPoint;
class QTextBrowser;
class QTextDocument;
class QTimer;

namespace textspoilerdoc {
class Handler;
}

class ChatTextSpoilers : public QObject
{
    Q_OBJECT

  public:
    struct Colors {
        QColor base; // the chat's background
        QColor text; // its text
    };
    struct Host {
        std::function<bool()>               othersMutating; // ChatIntegration, ChatEmoji or ChatLayout edit a chat now
        std::function<bool(bool)>           setMutating;    // ChatIntegration's flag around our hides (returns the one before)
        std::function<void(QTextBrowser*)>  edited;         // positions inside the chat's blocks moved
        // A pass hid spoilers in the block (under the mutating flag, which the chat layout ignores): it looks at the block
        // again, so a mention inside a hidden spoiler doesn't tint the row (review fix: the layout may style a block
        // before the first pass, e.g. right after a plugin load).
        std::function<void(QTextBrowser*, int block)> hid;
        std::function<Colors(QTextBrowser*)> colors;        // the chat's colours (default: paletteColors)
        std::function<QString()>            toolTip;        // "Spoiler · Click to reveal"
    };

    explicit ChatTextSpoilers(Host host, QObject* parent = nullptr);
    ~ChatTextSpoilers() override; // restoreAll(), the handler unregistered and deleted, the cursor back

    void attach(QTextBrowser* browser);
    void documentSwapped(QTextBrowser* browser);
    void afterScan(QTextBrowser* browser);
    void settingsChanged(); // Settings::revealSpoilers
    void themeChanged();    // 2.7.1 dark mode: TeamSpeak's look changed: every chat's bars in its colours now
    // ChatIntegration's event filter (chat viewports): clicks, hover, tool tips. True: taken.
    bool filterEvent(QObject* watched, QEvent* event);
    // The message menu (ChatReplies::actionsFor): "Reveal spoiler(s)" for block's hidden spoilers, parented to menu;
    // nullptr when it has none.
    QAction* revealAction(QMenu* menu, QTextBrowser* browser, int block);
    void     restoreAll();

    // Without a colours hook (tests; the plugin passes ChatIntegration::chatColorsOf, design J3): the ChatTab's palette
    // Base when it is a valid solid colour, else #2f3136 when the palette's text is light, else #ffffff; the text from
    // the palette.
    static Colors paletteColors(QTextBrowser* browser);

    // ---- also for tests and the live-test driver ----------------------------------------------------------------
    bool earlyPending() const;  // the 0 ms pass is queued
    void runEarlyPass();        // ... run now (what its timer runs)
    int  revealAt(QTextBrowser* browser, const QPoint& viewportPos); // a click's reveal; the spoiler's id or -1
    int  barAt(QTextBrowser* browser, const QPoint& viewportPos) const; // the spoiler id of the bar there, -1: none
    int  hiddenCount() const;
    int  revealedCount() const;
    QStringList diagnosticLines() const;
    // Per spoiler of the chat: block, id, state, key and its bars' rects in viewport coordinates (the driver's dump).
    QStringList describe(QTextBrowser* browser) const;
    QVector<QRect> barRects(QTextBrowser* browser, int id = -1) const;
    // Raw pairs the pass would hide in the blocks the chat shows now (the paint meter: must always be 0).
    int            rawPairsShown(QTextBrowser* browser) const;

  private:
    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        int                     dirtyFrom = 0;  // lowest position changed since the last pass (INT_MAX: none)
        int                     earlyFrom = -1; // lowest position of an insert with "[spoiler" waiting for the 0 ms pass
        QObject*                meter     = nullptr; // test builds: the paint meter
        bool                    frozen    = false;   // its viewport's updates are off until the raw pairs are hidden
    };

    View* viewFor(QTextBrowser* browser);
    View* viewForViewport(QObject* viewport);
    void  watch(View& view);
    void  ensureHandler(QTextDocument* doc);
    void  updateLook(View& view);
    // 2.7 live: no paint of a view between an insert with a raw pair and the pass that hides it (the viewport's updates
    // are off meanwhile; TeamSpeak can paint a chat before the 0 ms pass when the same message reaches several tabs).
    void  freeze(View& view);
    void  thaw(View& view);
    void  thawAll();
    // Hides the raw pairs of the view's blocks from position from on (at most replydoc::kMaxBlocks newest).
    void  pass(View& view, int from);
    void  revealSpoiler(QTextBrowser* browser, int id, bool clicked);
    void  revealBlock(QTextBrowser* browser, int block);
    void  setHover(QTextBrowser* browser, int id);
    void  setPressed(QTextBrowser* browser, int id);
    void  rememberRevealed(const QString& key);
    QString keyFor(QTextDocument* doc, int block, int index) const;

    Host                                 m_host;
    textspoilerdoc::Handler*             m_handler = nullptr;
    QHash<QTextBrowser*, View>           m_views;
    QVector<QPointer<QTextDocument>>     m_documents; // every document we registered on (swapped-out ones too)
    QHash<const QTextDocument*, int>     m_serials;   // the next spoiler id per document
    QSet<QString>                        m_revealed;  // keys revealed by a click or the menu this session
    QStringList                          m_revealedOrder; // oldest first (at most 4000 keys)
    QTimer*                              m_early   = nullptr;
    QTimer*                              m_thaw    = nullptr; // the frozen views' updates back on at the latest after it
    bool                                 m_editing = false; // our own edits (not noted as changes)
    bool                                 m_lastReveal = false;
    bool                                 m_shutdown   = false;
    // The pointer.
    QPointer<QTextBrowser> m_hoverBrowser;
    int                    m_hoverId   = -1;
    QPointer<QTextBrowser> m_pressBrowser;
    int                    m_pressId   = -1;
    qint64                 m_revealMs  = 0; // a double click right after a reveal is taken too
    bool                   m_cursorSet = false;
    Qt::CursorShape        m_savedCursor = Qt::ArrowCursor;
};
