#pragma once

// 2.2 protocol: the transport for "tsm1" messages over TeamSpeak plugin commands. QtCore only:
// TeamSpeak and Core sit behind Backend (a fake in the unit tests).
//
// Sending: one queue per connection, presence before reactions before background work. Every command
// carries its own return code and waits for its answer: a missing answer (about 3 s, S0) counts as
// NOT sent (Failed); should the server's Ok still come later (a lossy link), the same callback gets
// LateOk, so its owner can follow what the others received. The connection's FloodGovernor (Core)
// decides when a command may go: plugin commands spend its plugin bucket (S0: their own server
// counter, 5 points each), and a 0x020c ("retry in N ms") pauses both buckets for the hint plus its
// margin (FloodGovernor::commandFlooded).
// CLIENT targets are filtered to clients that are visible right before sending (one stale id fails
// the whole command). A permission error, or any error on a channel HELLO, blocks plugin commands
// on that connection for the session: presence and reactions are then unavailable there.
//
// Receiving: own echoes are dropped (CURRENT_CHANNEL commands come back to the sender), then a
// per-person rate limit, then the parser. Identity always comes from the invoker fields.

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "peerprotocol.h"
#include "peers.h"

class FloodGovernor;
class QTimer;

class PluginLink : public QObject, public peers::Sender
{
    Q_OBJECT

  public:
    // Presence goes first (it is time-critical and cheap), then reactions, then SYNC answers.
    enum class Priority { Presence = 0, Reaction = 1, Background = 2 };

    class Backend
    {
      public:
        virtual ~Backend()                                  = default;
        virtual qint64         nowMs() const                = 0; // the FloodGovernor's clock
        virtual FloodGovernor* governor(quint64 sch)        = 0; // nullptr: not connected
        virtual void           floodStateChanged(quint64 sch) = 0; // a flood pause: Core re-plans its posts
        virtual QString        newReturnCode()              = 0;
        virtual void sendCommand(quint64 sch, const QByteArray& payload, const peers::Target& target, const QString& returnCode) = 0;
        virtual bool    isConnected(quint64 sch) const                   = 0;
        virtual bool    clientVisible(quint64 sch, quint16 client) const = 0;
        virtual quint16 ownClientId(quint64 sch) const                   = 0;
    };

    struct Limits {
        int answerTimeoutMs    = 3000;  // no answer by then: not sent (S0)
        int presenceRetries    = 1;     // a flooded presence command goes again once after the pause
        int inboundBurst       = 20;    // commands per person ...
        int inboundWindowMs    = 30000; // ... per this long
        int maxQueued          = 64;    // per connection; the oldest background work goes first
        int returnCodeLingerMs = 30000; // late answers to timed-out commands are still recognised
        int maxRateEntries     = 512;   // people tracked by the inbound limit per connection
    };

    struct Counters {
        int  sent          = 0;
        int  answeredOk    = 0;
        int  failed        = 0; // errors and timeouts
        int  timeouts      = 0;
        int  floodBackoffs = 0;
        int  received      = 0;
        int  rejected      = 0; // malformed, too long, from a newer protocol
        int  rateLimited   = 0;
        bool blocked       = false;
    };

    explicit PluginLink(Backend& backend, QObject* parent = nullptr);
    PluginLink(Backend& backend, const Limits& limits, QObject* parent = nullptr);
    ~PluginLink() override; // no callbacks run from here

    // peers::Sender: presence priority.
    void send(quint64 sch, const proto::Message& message, const peers::Target& target, Done done) override;
    bool blocked(quint64 sch) const override;
    void dropQueued(const QByteArray& type) override;

    // coalesceKey: a queued command with the same key is replaced (its callback gets Superseded).
    void sendWith(quint64 sch, const proto::Message& message, const peers::Target& target, Priority priority, Done done, const QString& coalesceKey = {});

    // Shutdown: queued commands are dropped without callbacks; later sends go out at once if the
    // governor allows, or not at all (BYE).
    void setClosing();

    // ---- events (GUI thread) ------------------------------------------------------------------------
    void pump(); // the governor may allow commands again (Core::floodGovernorChanged)
    void connectionLost(quint64 sch);
    // Answers to our return codes. True if the code was ours.
    bool onServerError(quint64 sch, unsigned int error, const QString& returnCode, const QString& extraMessage, bool permissionError);
    void onCommand(quint64 sch, quint16 invoker, const QString& uid, const QString& name, const QByteArray& payload);

    // Any thread (TeamSpeak's callback thread asks before posting an answer to the GUI thread).
    static bool isOwnReturnCode(const QString& returnCode);

    Counters counters(quint64 sch) const;
    Counters totals() const;

  signals:
    void received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message);
    void blockedChanged(quint64 sch);

  private:
    struct Item {
        QByteArray     payload;
        QByteArray     type;
        peers::Target  target;
        Priority       priority = Priority::Background;
        Done           done;
        QString        coalesce;
        int            attempts = 0;
        quint64        seq      = 0;
    };
    struct InFlight {
        quint64 sch = 0;
        Item    item;
        qint64  sentMs = 0;
    };
    struct RateBucket {
        double tokens = 0.0;
        qint64 atMs   = -1;
    };
    struct Conn {
        QList<Item>                 queue; // by priority, then order
        Counters                    counters;
        QHash<QString, RateBucket>  inbound;
    };

    void enqueue(quint64 sch, Item item, bool front);
    void finish(Item& item, peers::SendResult result);
    void failQueued(quint64 sch, peers::SendResult result);
    void setBlocked(quint64 sch);
    bool takeInboundToken(Conn& conn, const QString& uid, qint64 now);
    void schedule(qint64 wakeMs);
    void runCallbacks(); // after the state is consistent: a callback may send again

    Backend&                 m_backend;
    Limits                   m_limits;
    QHash<quint64, Conn>     m_conns;
    QHash<QString, InFlight> m_inFlight; // return code -> command
    QHash<QString, qint64>   m_linger;   // return code -> forget after
    QHash<QString, Done>     m_lateDone; // return code -> callback of a command that timed out (a late Ok: LateOk)
    QTimer*                  m_timer   = nullptr;
    quint64                  m_seq     = 0;
    bool                     m_closing = false;
    bool                     m_pumping = false;
    bool                     m_repump  = false;
    QList<QPair<Done, peers::SendResult>> m_pendingCallbacks;
};
