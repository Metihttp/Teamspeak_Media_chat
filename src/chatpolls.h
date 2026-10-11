#pragma once

// 2.3 poll in the chat. Owned by ChatIntegration, which calls it at the places marked "2.3 poll" in
// chatintegration.cpp: after every scan of a chat document (before the replies and the layout), and in its
// event filter (after the layout). It:
//  * turns poll messages (polls.h) into poll cards (polldoc.h, pollart.h) and gives the text back on unload;
//  * draws the cards on screen only (right before the chat paints) at the chat's width, theme and scale;
//  * handles hover, clicks and tool tips on the cards: voting, ticking options, Vote, Retract vote, Retry;
//  * registers "Poll" in the attach menu (attachmenu.h) and opens the New poll window (polldialog.h), whose
//    poll goes to the channel as an ordinary chat message;
//  * owns PollHub (votes over plugin commands) and asks the channel about the polls it shows.
// Polls are voted on in the channel they were posted in; elsewhere the card says so. Everything read from a
// chat is untrusted (polldoc.h).

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>

#include <memory>

#include "core.h"
#include "pollart.h"
#include "polls.h"

class ChatIntegration;
class PollDialog;
class QJsonObject;
class PollHub;
class QEvent;
class QTextBrowser;
class QTextDocument;
class QTimer;

class ChatPolls : public QObject
{
    Q_OBJECT

  public:
    explicit ChatPolls(ChatIntegration* chat);
    ~ChatPolls() override; // every poll's text back, the menu entry gone, the hub saved

    void afterScan(QTextBrowser* browser);
    bool filterEvent(QObject* watched, QEvent* event); // true: the event was for a card
    void openCreator(QWidget* source);                 // the attach menu's "Poll"
    PollHub* hub() const { return m_hub; }

    // The chat message for poll (as it is sent), or empty when it doesn't fit into one message.
    static QString messageFor(const polls::Poll& poll, quint64 sch);

    // ---- tests and the live-test driver -------------------------------------------------------------
    int     cardCount(QTextBrowser* browser) const;
    QString describeCards(QTextBrowser* browser) const; // one line per card
    // Posts a poll to sch's channel as the New poll window does; empty, or why it couldn't.
    QString post(quint64 sch, const QString& question, const QStringList& options, bool multiple);
#ifdef TSMEDIA_TESTHOOKS
    // The live-test driver's "poll.*" commands (selftest.cpp): post, vote, retract, log, attach. One line back.
    QString testCommand(const QString& cmd, const QJsonObject& c);
#endif

  private:
    struct Card {
        polls::Poll poll;
        QString     object;
        quint16     selected = 0; // multiple answers: ticked, not sent yet
        QString     rendered;     // what its picture shows now (empty: none)
    };
    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        QHash<QString, Card>    cards;     // by object name
        QHash<QString, int>     positions; // object name -> document position (positionsValid)
        bool                    positionsValid = false;
        bool                    tracked        = false;
    };
    struct Spot {
        QPointer<QTextBrowser> browser;
        QString                object;
        pollart::Zone          zone  = pollart::Zone::None;
        int                    index = -1;
        bool same(const Spot& o) const { return browser == o.browser && object == o.object && zone == o.zone && index == o.index; }
    };
    class HubEnv;

    View*             viewOf(QTextBrowser* browser);
    void              track(QTextBrowser* browser, View& view);
    void              ensurePositions(View& view);
    pollart::Style    styleOf(QTextBrowser* browser) const; // memoized for the current event (beginPass)
    pollart::Style    computeStyle(QTextBrowser* browser) const;
    void              beginPass() const;
    pollart::Card     stateOf(QTextBrowser* browser, const Card& card) const;
    pollart::Pointer  pointerFor(QTextBrowser* browser, const QString& object) const;
    QSize             objectSize(QTextBrowser* browser, const Card& card) const; // the picture's logical size
    QString           signatureOf(QTextBrowser* browser, const Card& card) const;
    QRectF            cardRect(QTextBrowser* browser, int position, const QSizeF& size) const; // the panel, viewport coordinates
    Spot              spotAt(QTextBrowser* browser, const QPoint& pos, QRectF* rect = nullptr);
    void              renderVisible(QTextBrowser* browser);
    void              render(QTextBrowser* browser, View& view, Card& card);
    void              refreshSizes(QTextBrowser* browser);
    void              scheduleRefresh(QTextBrowser* browser);
    void              repaintCard(QTextBrowser* browser, const QString& object);
    void              setHover(const Spot& spot);
    void              applyCursor(QTextBrowser* browser, const Spot& spot);
    void              activate(QTextBrowser* browser, const Spot& spot);
    void              keyChanged(const QString& key);
    void              syncVisible();
    bool              targetOf(QTextBrowser* browser, ChatTarget* target) const;
    QString           newObjectName();
    void              restoreAll();

    ChatIntegration*           m_chat;
    std::unique_ptr<HubEnv>    m_env;
    PollHub*                   m_hub = nullptr;
    QHash<QTextBrowser*, View> m_views;
    Spot                       m_hover;
    Spot                       m_press;
    bool                       m_cursorSet   = false;
    Qt::CursorShape            m_savedCursor = Qt::ArrowCursor; // the chat's, while ours is on
    QString                    m_tag;
    quint32                    m_nextObject = 0;
    QTimer*                    m_syncTimer    = nullptr;
    QTimer*                    m_refreshTimer = nullptr;
    QHash<QTextBrowser*, bool> m_refreshWanted;
    QPointer<PollDialog>       m_dialog;
    // What every card of a chat shares, read once per event (an animated GIF repaints the chat often).
    struct Memo {
        QPointer<QTextBrowser> targetBrowser;
        bool                   targetKnown = false;
        bool                   hasTarget   = false;
        ChatTarget             target;
        QPointer<QTextBrowser> styleBrowser;
        bool                   styleKnown = false;
        pollart::Style         style;
    };
    mutable Memo m_memo;
};
