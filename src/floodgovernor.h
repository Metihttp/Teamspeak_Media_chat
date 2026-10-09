#pragma once

// 2.2: TeamSpeak's anti-flood, as measured by the S0 spike (server 3.13 defaults, Guest; see
// docs/ARCHITECTURE.md). The server keeps points per client: every command adds its cost, every second
// takes antiflood_points_tick_reduce (5) away, and at antiflood_points_needed_command_block (150) it
// answers 0x020c "client is flooding" with "retry in <n>ms". Rejected attempts cost full points too, so
// probing while blocked never recovers. Text messages, file info, folders and transfers share one
// counter; plugin commands have their own (server 3.4.0 and later).
//
// One FloodGovernor per server connection (Core owns them; see Core::floodGovernor) models both
// counters as buckets and paces what the plugin sends. It is pure logic with an injected clock
// (milliseconds of any monotonic clock), so it can be unit-tested; whoever sends asks it before each
// command and reports what came back.
//
// Rules:
//  * Chat posts go one at a time: each waits for its own return-code answer, or postAnswerWaitMs.
//    A post goes only while the general bucket keeps generalReserve points for the user's own typing:
//    from rest about 8 posts back to back, then one every 3 s.
//  * Transfers, file info and folder requests spend the general bucket too. The sender's uploads
//    (folders, upload starts) wait for generalReady(): they go while the bucket keeps the reserve plus
//    room for one chat post, so a waiting post is never held back by them. Downloads are only charged.
//  * Plugin commands spend their own bucket: from rest 24 back to back, then one a second.
//  * 0x020c from either counter pauses both for the server's hint + floodHintMarginMs (or a fixed
//    pause without a hint). After the pause exactly one command fits, then the steady rate follows.
//    A flooded post is sent again at most postRetries times, then it fails.

#include <QString>
#include <QtGlobal>

class FloodGovernor
{
  public:
    struct Limits {
        // The general counter (S0: text 15, file info ~8, mkdir ~5, upload / download <= 3 points).
        int    generalCapacity     = 150;
        double generalRefillPerSec = 5.0;
        int    generalReserve      = 30; // left for the user's own chat messages (two)
        int    costText            = 15;
        int    costFileInfo        = 8;
        int    costMkdir           = 5;
        int    costTransfer        = 3; // starting an upload or a download
        // The plugin-command counter.
        int    pluginCapacity      = 150;
        double pluginRefillPerSec  = 5.0;
        int    pluginReserve       = 30;
        int    costPluginCommand   = 5;
        // Posting and floods.
        int    postAnswerWaitMs    = 1000; // the next post waits this long for the previous one's answer
        int    postRetries         = 3;    // a flooded post is sent again this often, then it fails
        int    floodHintMarginMs   = 250;  // added to the server's "retry in <n>ms"
        int    textFloodPauseMs    = 6000; // a flood without a hint (text hints were 5.3-5.7 s)
        int    commandFloodPauseMs = 2000; // the same for plugin commands, folders, file info (1.2-2.0 s)
    };

    // What adds to the general counter besides chat posts.
    enum class Cost { FileInfo, Mkdir, Transfer };

    // What happened so far (for the diagnostic info).
    struct Counters {
        int    postsSent     = 0;
        int    postFloods    = 0; // 0x020c answers to posts
        int    commandsSent  = 0;
        int    commandFloods = 0; // 0x020c answers to plugin commands
        int    otherFloods   = 0; // 0x020c answers to transfers, folders, file info
        qint64 lastFloodMs   = -1;
        int    lastPauseMs   = 0; // the pause the last flood caused
    };

    FloodGovernor(); // the default Limits
    explicit FloodGovernor(const Limits& limits);

    const Limits&   limits() const { return m_limits; }
    const Counters& counters() const { return m_counters; }

    // "retry in 5687ms" (also "retry in 5687 ms") in a 0x020c answer's extra message: the milliseconds,
    // or -1 when there is no such hint. Capped at 2 minutes.
    static int retryHintMs(const QString& extraMessage);

    // ---- chat posts (Core's post queue) ----------------------------------------------------------
    // How many posts are waiting to be sent (for whoever wants to know when they are all out).
    void setPendingPosts(int count) { m_pendingPosts = qMax(0, count); }
    int  pendingPosts() const { return m_pendingPosts; }
    // True when the next post may go now: no post in flight (or its answer took longer than
    // postAnswerWaitMs), no flood pause, and the general bucket keeps its reserve after this post.
    bool postReady(qint64 nowMs) const;
    // A post went out (charged to the general bucket); returns a ticket for its answer.
    quint64 postSent(qint64 nowMs);
    // The answer to a post (late answers for older tickets only count for the flood state). Returns true
    // if the post should be retried (flooded and retries left; attempts = how often it was sent).
    // retryHintMs: retryHintMs() of the answer, -1 if none. A post that got no answer at all is reported
    // with flooded = false (it is not retried: it may have arrived).
    bool postAnswered(quint64 ticket, qint64 nowMs, bool flooded, int attempts, int retryHintMs = -1);

    // ---- other commands on the general counter (Core's transfers, folders, file info) -------------
    // True when a command of that cost may go now: no flood pause, and after it the general bucket still
    // keeps generalReserve plus one chat post (costText). Call charge() once it went.
    bool   generalReady(Cost cost, qint64 nowMs) const;
    qint64 nextGeneralCheckMs(Cost cost, qint64 nowMs) const; // when generalReady() becomes true by itself
    void charge(Cost cost, qint64 nowMs);
    void generalFlooded(qint64 nowMs, int retryHintMs = -1); // one of them got 0x020c

    // ---- plugin commands (PluginLink) -------------------------------------------------------------
    // True when a plugin command may go now: no flood pause and the plugin bucket keeps its reserve.
    bool commandReady(qint64 nowMs) const;
    // A command went out (spends from the plugin bucket). Call only after commandReady() said yes.
    void commandSent(qint64 nowMs);
    // The server answered a command with 0x020c: everything pauses.
    void commandFlooded(qint64 nowMs, int retryHintMs = -1);
    // A command (or post) was answered without a flood error.
    void answeredOk(qint64 nowMs);

    // ---- scheduling -------------------------------------------------------------------------------
    // When postReady() / commandReady() becomes true by itself (a pause ending, an answer wait running
    // out, the bucket draining): nowMs if it already is.
    qint64 nextPostCheckMs(qint64 nowMs) const;
    qint64 nextCommandCheckMs(qint64 nowMs) const;
    bool   postsPaused(qint64 nowMs) const { return nowMs < m_postsPausedUntil; }
    bool   commandsPaused(qint64 nowMs) const { return nowMs < m_commandsPausedUntil; }

    // The modelled points of each counter (0 = rested).
    double generalPoints(qint64 nowMs) const { return m_general.points(nowMs); }
    double pluginPoints(qint64 nowMs) const { return m_plugin.points(nowMs); }

  private:
    struct Bucket {
        double rate   = 5.0; // points per second
        double value  = 0.0; // as of at
        qint64 at     = 0;   // a time in the future holds the value until then (a flood pause)

        double points(qint64 nowMs) const;
        void   add(double cost, qint64 nowMs);
        // When points() is at most level, from nowMs on.
        qint64 timeUntil(double level, qint64 nowMs) const;
    };

    int costOf(Cost cost) const;
    // After a flood: both counters pause; the flooded one holds exactly one command's room at the end.
    void flooded(qint64 nowMs, int retryHintMs, int fallbackMs, Bucket& bucket, double oneFits);

    Limits   m_limits;
    Counters m_counters;
    Bucket   m_general;
    Bucket   m_plugin;
    int      m_pendingPosts        = 0;
    quint64  m_nextTicket          = 0;
    quint64  m_inFlight            = 0; // ticket of the post waiting for its answer, 0 = none
    qint64   m_inFlightSince       = 0;
    qint64   m_postsPausedUntil    = 0;
    qint64   m_commandsPausedUntil = 0;
};
