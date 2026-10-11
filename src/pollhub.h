#pragma once

// 2.3 poll: votes over plugin commands (polls.h has the format and why it is anonymous). QtCore only:
// TeamSpeak and the transport sit behind Env (PluginLink and TeamSpeak in the plugin, fakes in the tests).
//
//  * Your vote: the next state of your ballot goes to the channel at once (the reactions' priority, a
//    queued one is replaced by a newer one). Shown at once; a command that couldn't go out is sent again
//    up to twice (2 s, 5 s later, each time as a new state), then the card says it failed and offers Retry.
//  * Late joiners: polls of your channel that the chat shows ask the channel (PQ) what it has, once per
//    poll every 2 minutes, only when someone there has TS Media. Members whose ballots differ answer after
//    a random 0.3-1.5 s with the ballots (PV y=1, background priority, at most 64 per answer); an answer
//    overheard first takes those ballots off ours, so one answer usually covers everybody. Neither asked
//    nor answered while the user doesn't tell the channel they have TS Media (Env::quiet): then only the
//    votes they cast themselves go out.
//  * Receiving: only from people in your channel right now (no ServerQuery clients), and only for polls
//    the chat shows (watch()) that were posted in that channel; the sender's identity is used for that check
//    and dropped: nothing about who sent a ballot is kept or shown. Every state is checked against its chain
//    (polls::follows): a ballot can only be taken further by its own voter.
//  * polls.json in TS Media's data folder: the ballots seen and your own ballots' secrets (written at once
//    after your vote, a few seconds after other changes, and when the hub goes).
// Flood: plugin commands spend the connection's FloodGovernor plugin bucket (PluginLink); answers for late
// joiners also have their own budget per connection (Limits::answerBudget), whoever asks.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "peers.h"
#include "polls.h"

class QTimer;

class PollHub : public QObject
{
    Q_OBJECT

  public:
    class Env
    {
      public:
        virtual ~Env()                                                  = default;
        virtual qint64  nowMs() const                                   = 0; // monotonic
        virtual qint64  wallMs() const                                  = 0; // the store's "last active"
        virtual bool    isConnected(quint64 sch) const                  = 0;
        virtual quint64 ownChannel(quint64 sch) const                   = 0;
        virtual quint64 channelOf(quint64 sch, quint16 client) const    = 0; // 0: not visible
        virtual bool    isQueryClient(quint64 sch, quint16 client) const = 0;
        virtual bool    othersHaveTsMedia(quint64 sch) const            = 0; // someone in our channel
        virtual bool    blocked(quint64 sch) const                      = 0; // plugin commands refused there
        // Presence off there (Settings: "Tell people in your channel that you have TS Media"): nothing goes
        // out that the user didn't do (no questions, no answers for late joiners); their own votes still do.
        virtual bool    quiet(quint64 /*sch*/) const { return false; }
        virtual int     randomBetween(int low, int high)                = 0; // inclusive
        // background: answers for late joiners (else your vote). coalesce: a queued command with the same
        // key is replaced (its callback gets Superseded).
        virtual void send(quint64 sch, const proto::Message& message, bool background, const QString& coalesce, peers::Sender::Done done) = 0;
    };

    struct Limits {
        int retryMs[2]       = {2000, 5000};
        int syncDelayMs      = 1500;   // after a poll was seen, before PQ (more polls join the same PQ)
        int syncEveryMs      = 120000; // per poll
        int answerMinMs      = 300;
        int answerMaxMs      = 1500;
        int answerEveryMs    = 20000;  // per poll
        int maxAnswerBallots = 64;
        int saveDelayMs      = 3000;
        int answerBudget     = 16;     // PV answers per connection ...
        int answerBudgetMs   = 20000;  // ... per this long (refilled evenly)
        int maxWatched       = 2 * polls::kMaxPolls; // polls the chats showed lately
    };

    PollHub(Env& env, const QString& storePath, QObject* parent = nullptr);
    PollHub(Env& env, const QString& storePath, const Limits& limits, QObject* parent = nullptr);
    ~PollHub() override; // polls.json written; no callbacks run from here

    enum class VoteError { None, NotConnected, WrongChannel, Blocked, Locked, Invalid };
    // Why you can't vote on poll from sch now (None: you can).
    VoteError      canVote(quint64 sch, const polls::Poll& poll) const;
    // mask 0 takes your vote back.
    VoteError      vote(quint64 sch, const polls::Poll& poll, quint16 mask);
    void           retry(quint64 sch, const polls::Poll& poll); // the card's Retry: your vote again
    static QString errorText(VoteError error);

    enum class SendState { Idle, Sending, Failed };
    struct View {
        polls::Tally tally;
        QVector<int> percents;
        quint16      own   = 0; // your options (0: you haven't voted)
        SendState    state = SendState::Idle;
        bool         locked = false; // your ballot can't change any more
    };
    View view(const polls::Poll& poll) const;

    // A poll a chat shows: only its ballots are taken (in the channel it was posted in), and only it is answered
    // for. Polls no chat has shown for a long time are forgotten again (Limits::maxWatched).
    void watch(const polls::Poll& poll);
    bool watching(const QString& key, quint64 channel) const;
    // Polls of sch's channel the chat shows (asked about after a moment, see above).
    void requestSync(quint64 sch, const QStringList& keys);
    // A plugin command (PluginLink::received). uid and name are not used: nothing of the sender is kept.
    void received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message);
    void connectionLost(quint64 sch);
    void prepareShutdown(); // no more sends; polls.json written

    // ---- tests ----------------------------------------------------------------------------------------
    void          setAutoTimer(bool on); // off: the owner calls tick()
    void          tick();
    qint64        nextWakeMs() const;
    polls::Store& store() { return m_store; }
    void          save();

  signals:
    void changed(const QString& key); // a poll's tally or your vote's state (coalesced, 50 ms)

  private:
    struct Own {
        quint64 sch       = 0;
        quint64 channelId = 0;
        int     attempts  = 0;
        qint64  retryAt   = -1;
        bool    sending   = false;
        bool    failed    = false;
    };
    struct Answer {
        quint64          sch = 0;
        QString          key;
        QSet<QByteArray> remaining;
        qint64           dueMs = 0;
    };
    struct Watched {
        QSet<quint64> channels; // where its message said it was posted (a copy may say another)
        qint64        seenMs = 0;
    };
    struct Budget {
        double tokens = 0;
        bool   known  = false; // false: full (never used)
        qint64 atMs   = 0;
    };

    void sendOwn(const QString& key, const polls::Ballot& ballot);
    void ownDone(const QString& key, int step, quint16 mask, peers::SendResult result);
    void sendAnswer(const Answer& answer);
    int  answerRoom(quint64 sch); // PV answers the budget allows now
    void sendQueries(quint64 sch);
    void noteChanged(const QString& key);
    void scheduleSave();
    void reschedule();

    Env&                             m_env;
    Limits                           m_limits;
    QString                          m_path;
    polls::Store                     m_store;
    QHash<QString, Own>              m_own;        // poll key -> your vote on its way
    QHash<quint64, QSet<QString>>    m_syncWanted; // connection -> poll keys to ask about
    QHash<quint64, qint64>           m_syncDue;
    QHash<QString, qint64>           m_askedMs;    // "sch/key" -> our last PQ about it
    QHash<QString, qint64>           m_answeredMs; // poll key -> our last answer
    QVector<Answer>                  m_answers;
    QHash<QString, Watched>          m_watched;    // poll key -> where the chats showed it
    QHash<quint64, Budget>           m_budgets;    // connection -> answers it may still send
    QSet<QString>                    m_changed;
    mutable QHash<QString, polls::Tally> m_tallies; // "key|count|s/m" -> its tally (dropped on every change)
    QTimer*                          m_timer     = nullptr;
    QTimer*                          m_coalesce  = nullptr;
    QTimer*                          m_saveTimer = nullptr;
    bool                             m_autoTimer = true;
    bool                             m_closing   = false;
    bool                             m_dirty     = false;
};
