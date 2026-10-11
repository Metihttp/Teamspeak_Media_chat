#pragma once

// 2.3 text reactions: reactions on chat messages without media, over the wire (textreact.h: TR, TSYNC).
// Owned by PeerHub (created, started and shut down with it) and built on its pieces: the same transport
// (PluginLink: priorities, the plugin-command flood bucket, return codes), the same presence directory
// (TSYNC after entering a channel) and the same rules as reactions on media:
//  * your toggles are shown at once, sent 400 ms after the last click (debounced, coalesced), and taken
//    back with the reason when they couldn't be sent;
//  * a channel reaction counts only from someone in our channel right now, a private one only on a message
//    of the private chat with the reactor (theirs or ours); answers to TSYNC only shortly after we asked;
//    nobody speaks for anyone else (TSYNC answers carry the answerer's own reactions only);
//  * a reaction for a message this client doesn't have (yet) waits a little (15 s) for it to arrive;
//  * text-reactions.json keeps them for 30 days (ReactionStore, keys textreact::localKey).
// The chat side (ChatTextReactions) tells the index which messages each chat holds.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include "core.h"
#include "peerhub.h"
#include "reactions.h"
#include "textreact.h"

class PluginLink;
class QTimer;
namespace peers {
class PeerDirectory;
}

class TextReactHub : public QObject
{
    Q_OBJECT

  public:
    TextReactHub(Core* core, PluginLink* link, peers::PeerDirectory* directory, QObject* parent = nullptr);
    ~TextReactHub() override;

    static TextReactHub* instance(); // GUI thread; nullptr before PeerHub made it and after it went

    void start();           // text-reactions.json
    void prepareShutdown(); // unsent toggles taken back, the file written, timers off

    ReactionStore*     store() const { return m_store; }
    textreact::Index&  index() { return m_index; }
    // The chats' messages changed (ChatTextReactions): reactions waiting for a message are matched again.
    void               indexChanged();

    using ReactError = PeerHub::ReactError;
    // Toggles one of your reactions (an emoji id) on the message with this local key in target's chat.
    ReactError   toggle(const QString& key, int reaction, const ChatTarget& target);
    ReactionView view(const QString& key) const;
    int          waitingCount() const { return m_waiting.size(); }

  signals:
    void reactionsChanged(const QString& key);                     // coalesced (50 ms)
    void reactionFailed(const QString& key, const QString& text); // a toggle was taken back

  private:
    struct Pending {
        quint64            sch = 0;
        ChatTarget         target;
        proto::ReactionSet committed; // what the others last got from us
        quint64            generation = 0;
        qint64             dueMs      = -1;
        bool               inFlight   = false;
    };
    struct Waiting {
        QString            serverUid;
        proto::Scope       scope = proto::Scope::Channel;
        QString            reactorUid, ownUid, name, hash;
        qint64             estimatedSec = 0;
        proto::ReactionSet set;
        qint64             atMs = 0;
    };
    struct SyncAnswer {
        quint64     sch = 0;
        quint16     to  = 0;
        QString     uid;
        QStringList refs;
        qint64      dueMs = 0;
    };

    void    received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message);
    void    receiveReact(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::React& react);
    void    receiveSync(quint64 sch, quint16 from, const QString& uid, const proto::Sync& sync);
    void    sendSync(quint64 sch);
    void    flushPending();
    void    reactionSent(quint64 sch, const QString& key, quint64 generation, const proto::ReactionSet& set, peers::SendResult result);
    void    scheduleTimers();
    void    storeChanged(const QString& key);
    void    matchWaiting();
    void    save();
    QString ownUid(quint64 sch) const;
    QString ownName(quint64 sch) const;
    QString reactionsPath() const;
    qint64  nowMs() const; // the flood governor's clock (monotonic)

    Core*                         m_core;
    QPointer<PluginLink>          m_link;
    QPointer<peers::PeerDirectory> m_directory;
    ReactionStore*                m_store       = nullptr;
    QTimer*                       m_timer       = nullptr; // debounced sends, TSYNC answers
    QTimer*                       m_coalesce    = nullptr;
    QTimer*                       m_saveTimer   = nullptr;
    QTimer*                       m_waitTimer   = nullptr;
    textreact::Index              m_index;
    QSet<QString>                 m_changedKeys;
    mutable QHash<quint64, QString> m_ownUids;
    QHash<QString, Pending>       m_pending;  // local key -> your toggle on its way
    QVector<Waiting>              m_waiting;  // reactions for messages not in a chat (yet)
    QHash<quint64, qint64>        m_syncSentMs;
    QHash<QString, qint64>        m_syncAnsweredMs; // "sch/uid" -> our last answer to their TSYNC
    QVector<SyncAnswer>           m_syncAnswers;
    bool                          m_shuttingDown = false;
    bool                          m_dirty        = false;
};
