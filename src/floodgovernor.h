#pragma once

// 2.2 foundation: TeamSpeak's anti-flood counts every command of a client together, chat messages and
// plugin commands alike, so one FloodGovernor per server connection paces both (Core owns them; see
// Core::floodGovernor). It is pure logic with an injected clock (milliseconds of any monotonic clock),
// so it can be unit-tested; whoever sends asks it before each command and reports what came back.
//
// Rules (all numbers in Limits, to be set from the S0 flood measurement):
//  * Chat posts go one at a time: each waits for its own return-code answer, or postAnswerWaitMs.
//  * Posts have priority: plugin commands wait while posts are queued or in flight.
//  * ERROR_client_is_flooding (0x020c) from either pauses both: posts for postFloodPauseMs (then the
//    flooded post is retried, at most postRetries times), plugin commands for commandBackoffMs,
//    doubling on every further flood up to commandBackoffMaxMs (back to the start after a calm period).
//  * Plugin commands also spend tokens of a small bucket (commandBurst, one more every
//    commandRefillMs); posts spend one when there is one but never wait for it.

#include <QtGlobal>

class FloodGovernor
{
  public:
    struct Limits {
        // TODO(S0): every value here is a starting point until the flood test on the local server.
        int postAnswerWaitMs     = 1000;  // a post without an answer counts as delivered after this
        int postMinSpacingMs     = 0;     // between two posts
        int postFloodPauseMs     = 2000;  // after a 0x020c
        int postRetries          = 3;     // a flooded post is sent again this often, then it fails
        int commandBurst         = 6;     // plugin commands that may go back to back
        int commandRefillMs      = 1500;  // one more token every ...
        int commandBackoffMs     = 15000; // first back-off of plugin commands after a 0x020c
        int commandBackoffMaxMs  = 60000;
        int commandCalmResetMs   = 60000; // without a flood this long, the back-off starts small again
    };

    // What happened so far (for the diagnostic info).
    struct Counters {
        int    postsSent       = 0;
        int    postFloods      = 0; // 0x020c answers to posts
        int    commandsSent    = 0;
        int    commandFloods   = 0; // 0x020c answers to plugin commands
        qint64 lastFloodMs     = -1;
        int    currentBackoffMs = 0; // of plugin commands, 0 = none
    };

    FloodGovernor(); // the default Limits
    explicit FloodGovernor(const Limits& limits);

    const Limits&   limits() const { return m_limits; }
    const Counters& counters() const { return m_counters; }

    // ---- chat posts (Core's post queue) ----------------------------------------------------------
    // How many posts are waiting to be sent (plugin commands wait while there are any).
    void setPendingPosts(int count);
    // True when the next post may go now: no post in flight (or its answer took longer than
    // postAnswerWaitMs), no flood pause, and the minimum spacing has passed.
    bool postReady(qint64 nowMs) const;
    // A post went out; returns a ticket for its answer.
    quint64 postSent(qint64 nowMs);
    // The answer to a post (late answers for older tickets only count for the flood state). Returns true
    // if the post should be retried (flooded and retries left; attempts = how often it was sent).
    bool postAnswered(quint64 ticket, qint64 nowMs, bool flooded, int attempts);

    // ---- plugin commands (PluginLink) -------------------------------------------------------------
    // True when a plugin command may go now: no posts pending or in flight, no back-off, a token left.
    bool commandReady(qint64 nowMs) const;
    // A command went out (spends a token). Call only after commandReady() said yes.
    void commandSent(qint64 nowMs);
    // The server answered a command with 0x020c: everything pauses.
    void commandFlooded(qint64 nowMs);
    // A command (or post) was answered without a flood error.
    void answeredOk(qint64 nowMs);

    // ---- scheduling -------------------------------------------------------------------------------
    // When postReady() / commandReady() becomes true by itself (a pause ending, an answer wait running
    // out, a token coming back): nowMs if it already is. nextCommandCheckMs() is -1 while posts are
    // pending: then only setPendingPosts() can change it.
    qint64 nextPostCheckMs(qint64 nowMs) const;
    qint64 nextCommandCheckMs(qint64 nowMs) const;
    bool   postsPaused(qint64 nowMs) const { return nowMs < m_postsPausedUntil; }
    bool   commandsPaused(qint64 nowMs) const { return nowMs < m_commandsPausedUntil; }

  private:
    void   flooded(qint64 nowMs);
    double tokens(qint64 nowMs) const;
    void   spendToken(qint64 nowMs);

    Limits   m_limits;
    Counters m_counters;
    int      m_pendingPosts       = 0;
    quint64  m_nextTicket         = 0;
    quint64  m_inFlight           = 0; // ticket of the post waiting for its answer, 0 = none
    qint64   m_inFlightSince      = 0;
    qint64   m_lastPostMs         = -1;
    qint64   m_postsPausedUntil    = 0;
    qint64   m_commandsPausedUntil = 0;
    double   m_tokens             = 0.0; // as of m_tokensAt
    qint64   m_tokensAt           = -1;  // -1: the bucket is full
};
