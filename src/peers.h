#pragma once

// 2.2 presence: who in your channel (or your private chat) has TS Media. QtCore only: TeamSpeak is
// behind Env and the plugin-command transport behind Sender, so the state machine runs on a fake
// clock in the unit tests.
//
// Per connection:
//  * Entering a channel (connect, move, kick) starts a settle time (800 ms, so hopping through
//    channels says hello once); then one HELLO with rr=1 goes to the channel.
//  * Members answer with one HI to us only, after a random 150-900 ms (answers to several joiners
//    merge into one command). A member that hasn't answered within 4 s of our HELLO counts as without
//    TS Media; someone who joins gets 6 s (their own HELLO normally arrives within a second).
//  * TS Media 2.1 and older can't answer: they count as without. BYE (presence switched off, plugin
//    unloaded) makes someone "without" at once.
//  * Private chats: one HELLO to the partner, at most once a minute per person.
//  * The server chat never asks anyone.
// Nothing is kept on disk. People are counted by identity (UID): clones and ServerQuery clients are
// left out.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "peerprotocol.h"

class QTimer;

namespace peers {

using ClientId = quint16; // anyID

// What the directory reads from TeamSpeak.
class Env
{
  public:
    virtual ~Env() = default;
    virtual qint64           nowMs() const                                    = 0; // monotonic
    virtual ClientId         ownClientId(quint64 sch) const                   = 0;
    virtual quint64          ownChannel(quint64 sch) const                    = 0;
    virtual quint64          channelOf(quint64 sch, ClientId client) const    = 0; // 0: not visible
    virtual QVector<ClientId> channelClients(quint64 sch, quint64 channel) const = 0;
    virtual QString          uid(quint64 sch, ClientId client) const          = 0;
    virtual QString          nickname(quint64 sch, ClientId client) const     = 0;
    virtual bool             isQueryClient(quint64 sch, ClientId client) const = 0;
    virtual int              randomBetween(int low, int high)                 = 0; // inclusive
};

// Where a command goes: the own channel, or the listed clients (all must be visible right now: one
// stale id fails the whole command).
struct Target {
    bool              channel = true;
    QVector<ClientId> clients;

    static Target toChannel() { return {}; }
    static Target toClients(const QVector<ClientId>& ids) { return {false, ids}; }
};

// Ok: the server accepted it. Superseded: a newer state of the same thing replaced it before it went out.
// LateOk: it was reported Failed (no answer in time), and then the server's Ok came after all: it did
// reach the others (a second callback for the same command).
enum class SendResult { Ok, Flooded, Blocked, Failed, Superseded, LateOk };

// The plugin-command transport (PluginLink).
class Sender
{
  public:
    using Done = std::function<void(SendResult)>;
    virtual ~Sender()                                                                     = default;
    virtual void send(quint64 sch, const proto::Message& message, const Target& target, Done done) = 0;
    // Plugin commands were refused on this server (permissions, or a server that doesn't pass them on).
    virtual bool blocked(quint64 sch) const                                                = 0;
    // Queued commands of that type (on every connection) are dropped before they go out; their
    // callbacks get Superseded. Presence switched off: no HELLO or HI may follow.
    virtual void dropQueued(const QByteArray& type) { Q_UNUSED(type); }
};

enum class PeerState { Unknown, Checking, Has, Without };

// What the send window's presence line shows.
struct PresenceSummary {
    enum class Kind { Hidden, Channel, Private, Server };
    Kind kind = Kind::Hidden;

    // Channel: the others in it (distinct people), by state. Names as TeamSpeak reports them.
    QStringList has, without, checking;
    int         others() const { return has.size() + without.size() + checking.size(); }

    // Private chat
    QString   partner;
    PeerState partnerState = PeerState::Unknown;
};

// The line's text ("3 of 5 people here will see it in the chat. The others get a download link."),
// empty when nothing is to be shown.
QString presenceText(const PresenceSummary& summary);
// The "Who?" panel: one line per group, then a footnote. Empty when there is nothing to list.
QStringList presenceDetails(const PresenceSummary& summary);
// A nickname as the line shows it: without bidi and control characters, at most 32 characters.
QString presenceName(const QString& nickname);

class PeerDirectory : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        int settleMs           = 800;   // after entering a channel, before HELLO
        int memberTimeoutMs    = 4000;  // after our HELLO went out
        int joinerTimeoutMs    = 6000;  // after someone joined
        int helloMinIntervalMs = 3000;  // per connection
        int helloRetries       = 2;     // a HELLO that failed (flood) is sent again this often
        int helloRetryMs       = 5000;
        int hiJitterMinMs      = 150;
        int hiJitterMaxMs      = 900;
        int hiMaxTargets       = 50;
        int answerInChannelMs  = 20000; // at most one HI per person in our channel per ...
        int answerOutsideMs    = 60000; // ... and per person outside it
        int partnerPingMs      = 60000; // private-chat check, per person
        int partnerFreshMs     = 600000; // a partner heard from this recently needs no check
        int syncDelayMs        = 2000;  // reactions: SYNC this long after entering a channel
    };

    // Our own HELLO / HI content.
    struct Local {
        QString version;
        bool    presence  = true; // Settings::sharePresence
        bool    reactions = true; // Settings::showReactions (caps "r")
    };

    PeerDirectory(Env& env, Sender& sender, QObject* parent = nullptr);
    PeerDirectory(Env& env, Sender& sender, const Limits& limits, QObject* parent = nullptr);
    ~PeerDirectory() override;

    // Off: no timer of its own, the owner calls tick() (unit tests).
    void setAutoTimer(bool on);
    void setLocal(const Local& local); // presence off: BYE where we said hello; on: HELLO again

    // ---- TeamSpeak events (GUI thread) -------------------------------------------------------------
    void connected(quint64 sch); // STATUS_CONNECTION_ESTABLISHED, or plugin start while connected
    void disconnected(quint64 sch);
    void clientMoved(quint64 sch, ClientId client, quint64 fromChannel, quint64 toChannel);
    void clientRenamed(quint64 sch, ClientId client, const QString& name);
    // A HELLO / HI / BYE from someone (invoker as filled in by the server; own echoes already dropped).
    void received(quint64 sch, ClientId from, const QString& uid, const QString& name, const proto::Message& message);
    void commandsBlocked(quint64 sch); // the transport gave up on this server

    // ---- queries ------------------------------------------------------------------------------------
    // For a send target (TextMessageTarget_* mode). A private target is checked when it is unknown.
    PresenceSummary summary(quint64 sch, int targetMode, ClientId partner);
    void            checkPartner(quint64 sch, ClientId client); // at most once a minute per person
    PeerState       stateOf(quint64 sch, const QString& uid) const;
    bool            hasCapability(quint64 sch, const QString& uid, const char* cap) const;
    bool            channelHasCapability(quint64 sch, const char* cap) const; // a member of our channel
    bool            isAvailable(quint64 sch) const; // presence on and commands not blocked
    quint64         channelOf(quint64 sch) const;   // our channel as the directory saw us enter it
    QVector<quint64> connections() const;

    // Version histogram of the people known to have TS Media on sch ("2.2.0" -> 3), for diagnostics.
    QHash<QString, int> versions(quint64 sch) const;

    // BYE to every connection we said hello on (presence switched off, plugin unloading).
    void sayGoodbye();

    // ---- time ---------------------------------------------------------------------------------------
    void   tick();
    qint64 nextWakeMs() const; // -1: nothing scheduled

  signals:
    void presenceChanged(quint64 sch);
    // Reactions: a while after entering a channel with someone that supports reactions (SYNC).
    void syncDue(quint64 sch);

  private:
    struct PeerInfo {
        PeerState   state = PeerState::Unknown;
        QString     version;
        QStringList caps;
        QString     name;
        ClientId    clientId    = 0;
        qint64      heardMs     = -1;
        qint64      answeredMs  = -1; // our last HI to them
        ClientId    answeredId  = 0;
        qint64      pingMs      = -1; // our last private HELLO to them
        qint64      pingDeadline = -1;
    };
    struct Member {
        QString   uid;
        QString   name;
        PeerState state      = PeerState::Checking;
        qint64    deadlineMs = -1; // -1: waits for our HELLO to go out
    };
    struct Conn {
        quint64                  channel   = 0;
        qint64                   enteredMs = -1;
        qint64                   helloDue  = -1;
        qint64                   lastHelloMs = -1;
        int                      helloAttempts = 0;
        bool                     helloInFlight = false;
        bool                     announced = false;
        QString                  ownUid;
        QHash<ClientId, Member>  members;
        QHash<QString, PeerInfo> peers; // by uid, this session only
        QSet<ClientId>           hiTargets;
        qint64                   hiDue    = -1;
        qint64                   syncAt   = -1;
        qint64                   syncUntil = -1;
        quint64                  generation = 0; // bumped on every channel entry (late HELLO answers)
    };

    bool    presenceOn() const;
    QStringList caps() const;
    void    enterChannel(quint64 sch, quint64 channel);
    void    addMember(Conn& c, quint64 sch, ClientId client, qint64 deadlineMs);
    void    sendHello(quint64 sch);
    void    sendHi(quint64 sch);
    void    helloDone(quint64 sch, quint64 generation, peers::SendResult result);
    void    reschedule();
    void    changed(quint64 sch);
    PeerInfo& remember(Conn& c, const QString& uid, ClientId client, const QString& name);

    Env&                 m_env;
    Sender&              m_sender;
    Limits               m_limits;
    Local                m_local;
    QHash<quint64, Conn> m_conns;
    QTimer*              m_timer     = nullptr;
    bool                 m_autoTimer = true;
    bool                 m_ticking   = false;
};

} // namespace peers
