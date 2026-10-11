#pragma once

// 2.3 text reactions in the chat: emoji reactions on messages without media (text, links, replies) in
// channel and private chats, exactly like the ones on media. Owned by ChatIntegration, which calls it at
// the places marked "2.3 text reactions": after every scan of a chat document, in its event filter (after
// ChatReactions), and from the action bar (ChatLayout) and the chat menu (ChatReplies). It:
//  * reads which messages each chat holds and gives them to TextReactHub's index (textreactdoc::identify);
//  * keeps a reaction row (textreactdoc.h) at the end of every message that has reactions: the same
//    pills as under media (reactionart.h), with counts, the add pill while the message is hovered, tool
//    tips naming who reacted, a click toggles yours. A row coming, changing or going keeps what the
//    reader looks at in place (chatscroll.h), and stays at the bottom when the chat was there;
//  * draws the rows' pictures only for the part of the chat on screen (a blank for the others);
//  * opens the quick reactions and the full emoji picker (the action bar's Add reaction, the add pill,
//    "Add reaction…" in the chat menu);
//  * gives every document back on destruction (rows out, their pictures' memory freed).
// Everything read from the chat is untrusted: nothing in it is run or fetched.

#include <QDate>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

#include "reactionart.h"
#include "textreact.h"

class ChatIntegration;
class QAction;
class QEvent;
class QMenu;
class QTextBlock;
class QTextBrowser;
class QTextDocument;
class QTimer;
class ReactionPicker;
class TextReactHub;

class ChatTextReactions : public QObject
{
    Q_OBJECT

  public:
    explicit ChatTextReactions(ChatIntegration* chat);
    ~ChatTextReactions() override;

    // After ChatIntegration scanned browser's document (and on settings changes, which rescan).
    void afterScan(QTextBrowser* browser);
    // Chat viewports: hover, clicks and tool tips on the rows, painting their pictures. True when the
    // event was for a row (ChatIntegration then stops).
    bool filterEvent(QObject* watched, QEvent* event);

    // The action bar and the chat menu: whether block's message can get a reaction here now, and the
    // quick reactions under anchor (global coordinates; the button that opened it closes it again).
    bool     canReact(QTextBrowser* browser, int block);
    void     openPickerAt(QTextBrowser* browser, int block, const QRect& anchor);
    // "Add reaction…" for TeamSpeak's chat menu (or ours) on block; nullptr when the message has none.
    QAction* menuAction(QMenu* menu, QTextBrowser* browser, int block);

    // ---- tests and the live-test driver -----------------------------------------------------------
    QString keyOf(QTextBrowser* browser, int block);
    int     rowCount(QTextBrowser* browser) const;
    // A pill's rect (viewport coordinates) of block's row; empty without one.
    QRect   pillRect(QTextBrowser* browser, int block, int reaction);

  private:
#ifdef TSMEDIA_TESTHOOKS
    friend class SelfTest; // test builds: the live-test driver toggles reactions on messages (selftest.cpp)
#endif
    struct Entry {
        quint64 signature = 0;     // the block's text (and whether it is shown) when it was read: signatureOf
        bool    read      = false; // false: not read in this numbering yet (eligible/key may be the last guess)
        bool    eligible  = false; // a message without media, with a time
        QString key;               // textreact::localKey
        QString hash;
        QString sender;
        qint64  timeSec = 0;
    };
    struct View {
        QPointer<QTextBrowser>   browser;
        QPointer<QTextDocument>  document;
        QVector<Entry>           entries; // by block number
        QVector<QDate>           days;    // the days the last reading used, by block number (textreact::keepDays)
        QHash<QString, QVector<int>> blocksByKey;
        textreact::ChatKind      kind = textreact::ChatKind::Unknown;
        QString                  serverUid;
        QString                  partnerUid;
        qint64                   kindCheckedMs = 0;
        QHash<QString, QString>  rendered; // picture name -> what it shows (drawn ones only)
        QString                  lookKey;  // the font and width the rows were laid out for (lookKeyOf)
        bool                     published = false;
        bool                     retry     = false; // a row waits for its message to be read again (reconcile)
    };
    struct Spot {
        QPointer<QTextBrowser> browser;
        int                    block = -1;
        rx::Zone               zone  = rx::Zone::None;
        int                    index = -1;
        bool same(const Spot& o) const { return browser == o.browser && block == o.block && zone == o.zone && index == o.index; }
    };
    struct Hit {
        int         block = -1;
        QRectF      object; // viewport coordinates
        rx::ZoneHit zone;
    };
    struct Look {
        bool   dark = false;
        QColor base;
        QFont  font;
        qreal  dpr      = 1.0;
        int    maxWidth = 400;
        bool   onBubble = false; // 2.3: the row is inside the message's bubble (its pills take the bubble's colour)
    };

    View*   viewFor(QTextBrowser* browser);
    View*   viewForViewport(QObject* viewport);
    void    track(QTextBrowser* browser);
    void    classify(View& view, bool force);
    static quint64 signatureOf(const QTextBlock& block);
    static QString lookKeyOf(const Look& look);
    static Entry readEntry(QTextDocument* doc, const QTextBlock& block, QVector<QDate>* dates, bool* datesRead, const QVector<QDate>& kept, int up);
    bool    index(View& view);   // true if the messages changed
    void    publish(View& view); // to the hub's index
    void    reconcile(View& view, const QVector<int>& blocks);
    void    reconcileAll(View& view);
    void    refreshKey(const QString& key);
    bool    wantsRow(const View& view, int block) const;
    Look    lookOf(QTextBrowser* browser) const;
    rx::RowLayout layoutFor(const View& view, const QString& key, const Look& look, bool keep) const;
    void    renderVisible(View& view);
    bool    renderRow(View& view, int block, bool force); // true if it was drawn now
    QPair<int, int> blocksShown(QTextBrowser* browser, qreal margin) const;
    Hit     hitAt(View& view, const QPoint& pos) const;
    QRectF  objectRect(QTextBrowser* browser, int block) const; // viewport coordinates
    int     blockAt(View& view, const QPoint& pos) const;
    void    setSpot(const Spot& spot);
    void    setHoverBlock(QTextBrowser* browser, int block);
    void    redraw(QTextBrowser* browser, int block);
    void    toggle(QTextBrowser* browser, const QString& key, int reaction);
    void    openPicker(QTextBrowser* browser, const QString& key, const QRect& anchor);
    void    openFullPicker(QTextBrowser* browser, const QString& key, const QRect& anchor);
    bool    canAddIn(const View& view) const;
    QString blockedText(QTextBrowser* browser) const;
    QString pillToolTip(const QString& key, int reaction) const;
    QString newName();
    void    giveBack(QTextDocument* doc, View* view);
    void    afterEdit(QTextBrowser* browser); // positions moved: the replies and the previews read them again

    ChatIntegration*              m_chat;
    QPointer<TextReactHub>        m_hub;
    QString                       m_tag;
    quint32                       m_serial = 0;
    QHash<QTextBrowser*, View>    m_views;
    QSet<QTextBrowser*>           m_tracked;
    Spot                          m_hover;     // a pill or the add pill under the pointer
    Spot                          m_press;
    QPointer<QTextBrowser>        m_hoverIn;   // the chat whose message is hovered
    int                           m_hoverBlock = -1;
    QSet<QString>                 m_keep;      // rows kept under the pointer after their last reaction went
    QPointer<ReactionPicker>      m_picker;
    QPointer<QWidget>             m_fullPicker;
    QPixmap                       m_blank;
    QTimer*                       m_relayout = nullptr; // rows laid out again once the chat stops resizing
    QSet<QTextBrowser*>           m_relayoutViews;
    QTimer*                       m_indexTimer = nullptr; // reading long chats goes on in steps
    QSet<QTextBrowser*>           m_indexViews;
    bool                          m_shown = true; // Settings::showReactions when the rows were last made
};
