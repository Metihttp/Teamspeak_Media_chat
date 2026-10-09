#pragma once

// 2.2 protocol: connects the plugin-to-plugin pieces to TeamSpeak and Core and owns them:
//  * PluginLink (transport), PeerDirectory (presence), ReactionStore (reactions + reactions.json);
//  * the real Env / Backend (ts3::funcs, Core's FloodGovernor);
//  * reactions over the wire: receiving R and SYNC (checked: scope, rate, keys), sending your own
//    toggles (optimistic, debounced, reverted when they couldn't be sent), SYNC after entering a
//    channel and the answers to someone else's SYNC (only your own reactions, never anyone else's).
// TeamSpeak's callbacks arrive on its threads: the static entry points copy what they need and post
// to the hub on the GUI thread (or drop it once the hub is gone).
//
// Lifetime (plugin.cpp): created after Core starts and before ChatIntegration; at shutdown
// prepareShutdown() first (BYE without waiting, reactions.json written), then the windows and
// ChatIntegration go, then the hub, then Core.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

#include "core.h"
#include "peers.h"
#include "reactions.h"

class PluginLink;
class QTimer;

class PeerHub : public QObject
{
    Q_OBJECT

  public:
    explicit PeerHub(Core* core, QObject* parent = nullptr);
    ~PeerHub() override;

    static PeerHub* instance(); // GUI thread; nullptr before start and after shutdown

    void start();           // reactions.json, HELLO on the servers already connected, the presence line
    void prepareShutdown(); // BYE where we said hello (no waiting), reactions.json written, timers off
    void applySettings();   // Settings::sharePresence / showReactions changed

    peers::PeerDirectory* directory() const { return m_directory; }
    PluginLink*           link() const { return m_link; }
    ReactionStore*        store() const { return m_store; }

    // ---- reactions (GUI) -----------------------------------------------------------------------
    enum class ReactError { None, Off, ServerChat, NotConnected, Blocked, PartnerOffline, Unknown };
    // Toggles one of your reactions on key in the chat target: shown at once, sent shortly after.
    ReactError     toggle(const QString& key, int reaction, const ChatTarget& target);
    // Why a reaction can't be added there right now (None: it can).
    ReactError     reactBlock(const ChatTarget& target) const;
    ReactionView   view(const QString& key) const;
    static QString errorText(ReactError error);
    static QString sendErrorText(peers::SendResult result);

    // The media keys a SYNC asks about: in the chat documents, of this server, most recent first.
    using PresentKeys = std::function<QStringList(const QString& serverUid)>;
    void setPresentKeys(QObject* owner, PresentKeys keys);

    // Counters for the diagnostics feature: "plugin commands sent 14, dropped 0, ..." and versions.
    QString diagnosticsLine(quint64 sch) const;

    // ---- TeamSpeak callbacks: any thread ----------------------------------------------------------
    static void onPluginCommand(quint64 sch, const char* pluginName, const char* command, quint16 invoker, const char* invokerName, const char* invokerUid);
    // True when the return code was ours (the caller returns 1 so TeamSpeak doesn't print it).
    static bool onServerError(quint64 sch, unsigned int error, const char* returnCode, const char* extraMessage, bool permissionError);
    static void onConnectStatus(quint64 sch, int status);
    static void onClientMove(quint64 sch, quint16 client, quint64 oldChannel, quint64 newChannel, int visibility);
    static void onClientRenamed(quint64 sch, quint16 client, const char* displayName);

  signals:
    void presenceChanged(quint64 sch);
    void reactionsChanged(const QString& key);                 // coalesced (50 ms)
    void reactionFailed(const QString& key, const QString& text); // a toggle was reverted

  private:
    struct Pending {
        quint64    sch = 0;
        ChatTarget target;
        quint8     committed = 0;  // what the others last got from us
        quint8     sent      = 0;  // what is on its way
        quint64    generation = 0; // of the last send
        qint64     dueMs = -1;     // debounce
        bool       inFlight = false;
    };

    class Env;

    void    received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message);
    void    receiveReact(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::React& react);
    void    receiveSync(quint64 sch, quint16 from, const QString& uid, const proto::Sync& sync);
    void    sendSync(quint64 sch);
    void    flushPending();
    void    reactionSent(const QString& key, quint64 generation, quint8 mask, peers::SendResult result);
    void    scheduleTimers();
    void    storeChanged(const QString& key);
    void    save();
    QString ownUid(quint64 sch) const;
    QString ownName(quint64 sch) const;
    QString ownUidFor(const QString& key) const;
    QString reactionsPath() const;
    qint64  nowMs() const;

    Core*                    m_core;
    std::unique_ptr<Env>     m_env;
    PluginLink*              m_link      = nullptr;
    peers::PeerDirectory*    m_directory = nullptr;
    ReactionStore*           m_store     = nullptr;
    QTimer*                  m_timer     = nullptr; // debounced sends, scheduled SYNC answers
    QTimer*                  m_coalesce  = nullptr; // reactionsChanged
    QTimer*                  m_saveTimer = nullptr;
    QTimer*                  m_orphanTimer = nullptr; // reactions for media that isn't registered yet
    QSet<QString>            m_changedKeys;
    mutable QHash<quint64, QString> m_ownUids; // connection -> our identity there
    QHash<QString, Pending>  m_pending;          // media key -> your toggle on its way
    QHash<quint64, qint64>   m_syncSentMs;       // connection -> our last SYNC (answers are accepted 10 s)
    QHash<QString, qint64>   m_syncAnsweredMs;   // "sch/uid" -> our last answer to their SYNC
    struct SyncAnswer {
        quint64     sch = 0;
        quint16     to  = 0;
        QStringList keys;
        qint64      dueMs = 0;
    };
    QVector<SyncAnswer>      m_syncAnswers;
    QPointer<QObject>        m_presentKeysOwner;
    PresentKeys              m_presentKeys;
    bool                     m_shuttingDown = false;
    bool                     m_dirty        = false;
};
