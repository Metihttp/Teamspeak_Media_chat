#pragma once

// 2.2 reply in the chat. Owned by ChatIntegration, which calls it at the places marked "2.2 reply" in
// chatintegration.cpp: after every scan of a chat document, first in its event filter (chat viewports and
// the chat input), in the plugin's own preview menu, from the send window and from a file drop.
//
//  * Starting a reply: right-click any message -> "Reply" (added at the top of TeamSpeak's own chat menu,
//    or of the plugin's preview menu; when TeamSpeak shows no menu, a small one of ours), Alt+Up in the
//    chat input (Alt+Up / Alt+Down move to older / newer messages), or the "Reply to the last message"
//    hotkey.
//  * Reply mode: ReplyBar above TeamSpeak's chat input. Enter sends the input as a reply to the same chat
//    (TeamSpeak commands "/..." and empty input are left to TeamSpeak), Esc cancels. Files sent through
//    the send window or dropped meanwhile go out as the reply (the quote line before the caption).
//  * Showing replies: each reply's quote line is replaced by a Discord-style reply line (replydoc.h);
//    a click on it scrolls to the original and flashes it. A message with replies has "View N replies".
// Everything read from the chat is untrusted; nothing in a quote line is ever run or fetched.

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVector>

#include "core.h"
#include "replies.h"
#include "replydoc.h"
#include "replyart.h"

class ChatIntegration;
class QAction;
class QEvent;
class QKeyEvent;
class QMenu;
class QTextBrowser;
class QTimer;
class QVariantAnimation;
class ReplyBar;
class RepliesPopup;

class ChatReplies : public QObject
{
    Q_OBJECT

  public:
    ChatReplies(ChatIntegration* chat, Core* core);
    ~ChatReplies() override; // gives every quote line back, removes the bar, its menus and popups

    // After ChatIntegration scanned browser's document: index its messages, turn new quote lines into
    // reply lines, redraw the reply lines whose original came or went.
    void afterScan(QTextBrowser* browser);
    // Chat viewports (reply lines, the context menu) and the chat input (Enter, Esc, Alt+Up/Down). True
    // when the event was for the replies (ChatIntegration then stops).
    bool filterEvent(QObject* watched, QEvent* event);
    // "Reply" / "View N replies" at the top of the plugin's own menu for a preview in browser.
    void addPreviewMenu(QMenu* menu, QTextBrowser* browser);
    // The hotkey: reply to the newest message of the chat that is shown.
    void replyToLatest();

    // ---- the send window and file drops -------------------------------------------------------------
    // "Replying to Alice" for the send window, while a reply to that chat is being written (else nullptr).
    QWidget* composeLine(QWidget* parent, const ChatTarget& target);
    // The quote line to put before the caption when files go to target now; empty: not a reply.
    QString leadFor(const ChatTarget& target) const;
    // Files went out with leadFor(target): the reply is done.
    void sentWithFiles(const ChatTarget& target);

  private:
    struct View {
        QVector<replydoc::Message> messages;
        QVector<int>               originalOf; // per message: its original's index, -1 if none
        QHash<int, QVector<int>>   repliesTo;  // message index -> the replies to it (document order)
        QHash<int, int>            byBlock;    // block number -> message index
        QHash<QString, QString>    rendered;   // reply line object -> what its picture shows
        QString                    styleKey;   // the chat's look the lines were last drawn for
        bool                       indexed = false;
    };
    struct Style {
        bool   dark = false;
        QColor base;
        QFont  font;
        qreal  dpr      = 1.0;
        int    maxWidth = 400;
    };
    struct Anchor {
        int   block  = -1;
        qreal offset = 0;
        bool  bottom = false;
    };
    struct Pending {
        QPointer<QTextBrowser> browser;
        ChatTarget             target;
        replies::Original      original;
        QColor                 nickColor;
        int                    block = -1;
        QString                snippet; // shown in the bar
        bool                   media = false;
    };
    struct MenuContext {
        QPointer<QTextBrowser> browser;
        int                    block = -1;
        QPoint                 globalPos;
        bool                   handled = false;
    };

    // indexing
    View*  viewOf(QTextBrowser* browser);
    void   index(View& view, QTextDocument* doc) const;
    void   ensureIndex(QTextBrowser* browser);
    void   track(QTextBrowser* browser);
    int    messageAt(QTextBrowser* browser, const QPoint& viewportPos);
    int    lineAt(QTextBrowser* browser, const QPoint& viewportPos, QRectF* rect); // a reply line under the pointer

    // drawing
    Style                 styleOf(QTextBrowser* browser) const;
    static QString        keyOf(const Style& style);
    replyart::Header      headerFor(const View& view, int message) const;
    replyart::HeaderStyle headerStyle(QTextBrowser* browser, const Style& style, const replydoc::Message& m) const;
    void                  refreshLines(QTextBrowser* browser, bool force);
    QString               newObjectName();
    Anchor                capture(QTextBrowser* browser) const;
    void                  restoreAnchor(QTextBrowser* browser, const Anchor& anchor) const;
    void                  setHover(QTextBrowser* browser, const QString& object);
    void                  scheduleRelayout(QTextBrowser* browser);

    // menus
    void            prepareMenu(QTextBrowser* browser, const QPoint& pos, const QPoint& globalPos);
    void            arm();
    void            disarm();
    void            inject(QMenu* menu);
    QList<QAction*> actionsFor(QMenu* menu, QTextBrowser* browser, int block, const QPoint& globalPos);
    void            showFallbackMenu();
    void            showReplies(QTextBrowser* browser, int block, const QPoint& globalPos);
    friend class ReplyMenuCatcher;

    // reply mode
    void     startReply(QTextBrowser* browser, int block, bool show);
    void     cancelReply();
    QWidget* inputFor(QTextBrowser* browser) const;
    void     showBar(QWidget* input);
    void     hideBar();
    void     attachBar(QWidget* input);
    void     placeOverlayBar();
    bool     barShownFor(QWidget* input) const;
    void     watchPending();
    bool     handleKey(QWidget* input, QKeyEvent* event, bool shortcutOverride);
    bool     trySend(QWidget* input);
    void     step(QWidget* input, int direction);
    unsigned send(const ChatTarget& target, const QString& message) const;

    // jumping
    void jumpTo(QTextBrowser* browser, int block);
    void jumpToPending();
    void flash(QTextBrowser* browser, int block);
    void activateLine(QTextBrowser* browser, int message);
    void tip(QWidget* widget, const QString& text, bool error, bool atPointer = true) const;

    ChatIntegration*         m_chat;
    Core*                    m_core;
    QString                  m_tag; // this instance's part of object names
    quint32                  m_nextObject = 0;
    QHash<QTextBrowser*, View> m_views;
    QSet<QTextBrowser*>      m_tracked;
    QSet<QTextBrowser*>      m_dirty;
    QTimer*                  m_relayout   = nullptr;
    QTimer*                  m_styleCheck = nullptr;
    QTimer*                  m_watch      = nullptr;

    QPointer<QTextBrowser> m_hoverIn;
    QString                m_hoverObject;
    QPointer<QTextBrowser> m_pressIn;
    QString                m_pressObject;

    MenuContext              m_menu;
    QObject*                 m_catcher = nullptr;
    bool                     m_armed   = false;
    QList<QPointer<QAction>> m_injected;
    QPointer<QMenu>          m_fallback;
    QPointer<RepliesPopup>   m_popup;

    bool                m_hasPending = false;
    Pending             m_pending;
    QPointer<ReplyBar>  m_bar;
    QPointer<QWidget>   m_barInput;
    bool                m_barOverlay = false; // no vertical layout around the input: the bar floats above it

    QVariantAnimation*     m_scroll = nullptr;
    QPointer<QWidget>      m_flash;
};
