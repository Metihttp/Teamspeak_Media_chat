#pragma once

// Chat redesign: TS Media's chat layout in TeamSpeak's chat views ("Cozy", like Discord, the default;
// "Compact"; "TeamSpeak classic"). Owned by ChatIntegration, which calls it at the places marked "chat
// redesign" in chatintegration.cpp (after every scan, first thing after the replies in its event filter,
// on settings changes, when a document is swapped). It:
//  * styles the chat documents in steps (layoutdoc.h): at most 4 ms every 16 ms, the screen and the newest
//    lines first, then the history backwards (the newest replydoc::kMaxBlocks blocks), keeping what the
//    reader looks at in place; new lines are styled before TeamSpeak paints them;
//  * draws the pictures of what is on screen right before the chat paints, and gives the others a blank;
//  * paints under the text (hover and focus rows, gutter times, link underlines); a mouse-transparent
//    overlay instead where the skin paints over that;
//  * shows the action bar (actionbar.h) over the hovered message, the full date and nickname as tool tips,
//    the reply row of a Cozy head as ChatReplies' reply line (a click jumps to the original);
//  * gives everything back on destruction, on "TeamSpeak classic" and before a mode switch.
// Everything read from the chat is untrusted: nothing in it is run or fetched.

#include <QColor>
#include <QDate>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QTextCursor>
#include <QVector>

#include "layoutdoc.h"

class ActionBar;
class ChatIntegration;
class Core;
class QPaintEvent;
class QPainter;
class QTextBrowser;
class QTextDocument;
class QThreadPool;
class QTimer;
class QWidget;

class ChatLayout : public QObject
{
    Q_OBJECT

  public:
    ChatLayout(ChatIntegration* chat, Core* core);
    ~ChatLayout() override; // every document back, the bar and overlays gone, filters removed

    // True while this edits a document (ChatIntegration doesn't rescan for those changes, ChatEmoji ignores them).
    static bool isMutating();

    void attach(QTextBrowser* browser);
    void documentSwapped(QTextBrowser* browser);
    // After ChatIntegration's scan (previews, albums, reply lines): the outermost layer goes on last.
    void afterScan(QTextBrowser* browser);
    // Chat viewport events (from ChatIntegration's filter); true when one was for the layout.
    bool filterEvent(QObject* watched, QEvent* event);
    void settingsChanged();

    // ChatReplies: before it collapses (or gives back) a quote line in block, and after.
    void unstyle(QTextBrowser* browser, int block);
    void restyleSoon(QTextBrowser* browser, int block);
    // ChatIntegration's albums showed or hid these blocks: the next visible message is grouped again.
    void visibilityChanged(QTextBrowser* browser, const QSet<int>& blocks);
    // ChatReplies: the reply being written changed (its row gets the focus look).
    void pendingChanged();
    // P2: TeamSpeak downloaded a client's avatar (ts3plugin_onAvatarUpdated): heads draw it from its cache.
    void avatarUpdated(quint64 sch, int clientId);

    layoutart::Mode mode() const;
    // The content column (previews are that much narrower): G (Cozy), L (Compact) or 0 (classic).
    int indentFor(QTextBrowser* browser) const;

    // ---- tests and the live-test driver ----------------------------------------------------------------
    void    processNow(QTextBrowser* browser); // everything waiting, now
    QString describe(QTextBrowser* browser) const;
    // Hovers the row of block (viewport coordinates of its first body character), as the pointer would.
    bool    hoverBlock(QTextBrowser* browser, int block, QPoint* at = nullptr);
    ActionBar* bar() const;
    bool    overlayMode(QTextBrowser* browser) const;

  signals:
    // This changed browser's document (positions inside its blocks moved; blocks stay where they are).
    void documentEdited(QTextBrowser* browser);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // the browser (keyboard menus), the bar

  private:
    struct Rendered {
        QString signature;
        int     tag = 0;
    };
    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        layoutdoc::Env          env;
        QString                 styleKey;
        int                     epoch   = 1;
        int                     nextTag = 1;
        QHash<int, int>         tagEpoch; // tag -> the epoch its block was styled in
        QHash<int, int>         tagBlock; // tag -> block number (when it was styled)
        QHash<int, qint64>      arrivedByTag;
        QVector<QPair<QTextCursor, qint64>> arrivals; // blocks TeamSpeak appended while we watched
        QVector<QPair<QTextCursor, qint64>> burst;    // ... in the current turn of the event loop (settleBurst)
        QVector<QPair<QTextCursor, QTextCursor>> dirty; // ranges TeamSpeak changed
        QSet<int>               restyle;       // block numbers to style again
        int                     historyNext = -1; // the next block going backwards (-1: done)
        int                     blocksSeen  = 0;  // the document's block count at its last change
        QVector<QDate>          dates;
        int                     datesFrom  = 2147483647; // the first line appended since (INT_MAX: none)
        bool                    datesValid = false;
        QHash<QString, Rendered> rendered; // picture name -> what it shows
        // what the pictures on screen depend on changed (renderVisible skips its work while it hasn't)
        int                     renderStamp   = 1;
        int                     renderedStamp = 0;
        QPair<int, int>         renderedShown = {-1, -1};
        bool                    repliesTouched = false;
        // hover
        int                     hover = -1;     // block under the pointer
        QRect                   hoverSlot;      // its slot in viewport coordinates
        QVector<QRectF>         linkRects;      // the hovered link's line rects (document coordinates)
        QString                 linkHref;
        bool                    replyHover   = false; // on a Cozy head's reply row
        bool                    replyPressed = false;
        int                     replyPressBlock = -1;
        // painting
        bool                    overlay   = false; // the skin paints over the viewport: the overlay paints instead
        bool                    checked   = false; // the paint-under check ran
        bool                    probing   = false; // ... is running (no tint in that paint)
        QPointer<QWidget>       overlayWidget;
        int                     pendingWidth = 0; // a new chat width, applied once resizing pauses
        // mentions (P2): your nickname and uid on this chat's server, learned while it is shown
        QString                 ownNick;
        QString                 ownUid;
        QString                 serverUid; // avatars (P2): TeamSpeak's cache folder of this server
        // what the lines say about the skin (envFor), read now and then
        bool                    statsValid  = false;
        int                     statsBlocks = 0;
        qint64                  statsMs     = 0;
        QColor                  statNick;
        QColor                  statLink;
        bool                    statAmPm = false;
        // collapsed runs of events (P2)
        QSet<int>               expandedRuns; // first rows' tags of the runs the reader opened
        int                     chipSerial = 1;
        int                     chipHover  = -1;    // block whose run chip is under the pointer
        bool                    handCursor = false; // ours: over a reply row or a chip
        bool                    chipPressed = false; // the press went to a chip: so does its release
    };

    View* viewFor(QTextBrowser* browser);
    View* viewForViewport(QObject* viewport);
    void  watch(View& view);
    void  reset(View& view); // a new or cleared document
    void  dropFirst(View& view, int dropped); // TeamSpeak's line limit took the first blocks
    void  schedule(int delayMs = 16);
    void  quickStep();       // posted after TeamSpeak appended: before it paints
    void  step();
    bool  stepView(View& view, qint64 budgetMs, bool all);
    bool  needsWork(View& view, int n) const;
    bool  isCopy(View& view, const QTextBlock& block) const;
    bool  processBlock(View& view, int n);
    void  restoreView(View& view, bool dropPictures);
    layoutdoc::Env envFor(View& view) const;
    QString        styleKeyOf(const layoutdoc::Env& env) const;
    void  refreshEnv(View& view);
    QDate dateFor(View& view, int n);
    void   settleBurst(View& view); // the lines appended in a turn that is over: arrivals, or history
    qint64 arrivedFor(View& view, int n, int tag);
    QPair<int, int> blocksShown(QTextBrowser* browser, qreal margin) const;

    // pictures and painting
    void renderVisible(View& view);
    void evict(View& view);
    void paintLayer(View& view, QPainter& p, const QRegion& region, bool over);
    QRect slotOf(View& view, int block) const;
    void  checkPaintUnder(View& view);
    void  ensureOverlay(View& view);

    // the pointer
    int  blockAt(View& view, const QPoint& pos, QRect* slot) const;
    void updateHover(View& view, const QPoint& pos, bool moved);
    void clearHover(View& view);
    bool replyRowAt(View& view, const QPoint& pos, int* block) const;
    int  chipAt(View& view, const QPoint& pos) const; // the block whose run chip is at pos, or -1
    void collapseRuns(View& view, QVector<int> blocks, int floor); // the runs of events around blocks (P2)
    void toggleRun(View& view, int block);
    void placeBar(View& view);
    void hideBar(bool animate = true);
    void barAction(int action);
    bool toolTip(View& view, QEvent* event);
    QPoint bodyPoint(View& view, int block) const;
    void keyboardMenu(QTextBrowser* browser);
    void printIntro(QTextBrowser* browser);

    // avatars (P2): TeamSpeak's cached avatar files, decoded on a worker at the head's size, 64 kept
    struct Avatar {
        QImage image;
        qint64 checkedMs = 0;
        bool   loading   = false;
    };
    QImage avatarFor(View& view, const QString& uid);
    void   avatarLoaded(const QString& key, const QImage& image);

    ChatIntegration*          m_chat;
    Core*                     m_core;
    QString                   m_tag; // this instance's part of picture names
    QHash<QTextBrowser*, View> m_views;
    QVector<QPointer<QTextDocument>> m_retired; // documents a view had before TeamSpeak swapped them
    QTimer*                   m_timer    = nullptr;
    QTimer*                   m_midnight = nullptr;
    QTimer*                   m_resize   = nullptr;
    bool                      m_quickPosted = false;
    layoutart::Mode           m_mode        = layoutart::Mode::Cozy;
    bool                      m_group       = true;
    QPointer<ActionBar>       m_bar;
    QPointer<QTextBrowser>    m_barIn;   // the chat the bar is shown in
    int                       m_barBlock = -1;
    bool                      m_introDone = false;
    QHash<QString, Avatar>    m_avatars;      // "<server uid>|<uid>"
    QStringList               m_avatarOrder;  // least recently used first
    QThreadPool*              m_pool = nullptr; // one worker; waited for on destruction
};
