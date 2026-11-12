#pragma once

// 2.5 link previews: HostGate, the one place that decides when a website may be asked again. It spaces requests
// to one site, honours a site's "slow down" (HTTP 429 / 503 with Retry-After, x-ratelimit headers), stops asking a
// site that keeps failing for a while, and remembers bot-challenge walls. Pure QtCore and thread-safe (one
// QMutex); no timers and no threads: a clock function decides (tests inject one).
//
// One per plugin: sharedGate() (both Fetchers hold it); tests and tools may make their own.
//
// Two kinds of key:
//  * the spacing group (spacingGroup(host)): the registrable domain, with one company's hosts merged
//    ("youtube" = youtube.com, youtu.be, ytimg.com; "x" = x.com, twitter.com, twimg.com; ...). Per group and lane:
//    how many requests run at once, the spacing between two starts and the burst in a 10 s window. Page lane
//    (page, oEmbed, JSON, feed, manifest, redirect): 1 at a time, 1 s apart, 4 per 10 s; media lane (media files,
//    pictures): 2 at a time, 250 ms apart, 8 per 10 s (providers.h overrides some groups);
//  * the health key (healthKey(host) = the registrable domain of the host that ANSWERED, never merged by
//    brand): cooldowns, the breaker and walled memos. A 429 from discord.com leaves cdn.discordapp.com alone; a
//    link shortener can never cool or wall another site.
// Every hop of a load (the first request of a step and each redirect it follows) takes its own ticket under its
// own host and releases it with that hop's outcome.
//
// Cooldowns: 429 (or 503 with Retry-After): Retry-After, else the rate reset, else 60 s (also for a value of 0),
// clamped to [max(1 s, the provider minimum), 600 s] (hard). Any answer with x-ratelimit-remaining below 1 and a
// reset time: a soft cooldown of that time, clamped to [1 s, 600 s] (not a failure; an existing longer cooldown
// stays). Breaker: 5 server errors, time-outs or offline answers in a row stop automatic requests to that
// domain for 10 minutes (a user's click passes it). A bot challenge walls (domain, first path segment) for 30
// minutes. At most Limits::maxKeys entries of each kind; idle ones are dropped first.

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace linkpreview {

class HostGate
{
  public:
    enum class Lane { Page, Media }; // Page: page, oEmbed, JSON, RSS, manifest, redirect; Media: media and pictures
    enum class Verdict { Go, Wait, Cooling, Walled, Broken };
    enum class Outcome { Ok, RateLimited, ServerError, Timeout, Offline, Walled, Other };
    struct Limits {
        int pageRunning = 1, pageSpacingMs = 1000, pageBurst = 4;
        int mediaRunning = 2, mediaSpacingMs = 250, mediaBurst = 8;
        int burstWindowMs = 10000;
        int defaultCooldownMs = 60000, minCooldownMs = 1000, maxCooldownMs = 600000;
        int breakerFailures = 5, breakerMs = 600000, walledMs = 1800000;
        int maxKeys = 512;
        int idleMs  = 600000; // an entry with nothing running, no cooldown and no start for this long is idle
    };
    // What release() needs (the keys are computed once, from the hop's own host).
    struct Ticket {
        QString group, domain, firstSegment;
        Lane    lane = Lane::Page;
        quint64 id   = 0;
    };

    // clockMs: a monotonic clock in ms (default: QElapsedTimer's reference clock).
    explicit HostGate(std::function<qint64()> clockMs = {}, const Limits& limits = Limits());
    HostGate(const HostGate&)            = delete;
    HostGate& operator=(const HostGate&) = delete;

    // Go: *ticket filled, counted as running until release() or unused(). Wait: *ms until a start may be possible
    // (at most burstWindowMs). Cooling / Walled / Broken: *ms until that ends; *soft (optional) tells a soft
    // cooldown (x-ratelimit) from a hard one. userAsked passes Broken (one try).
    Verdict acquire(const QString& host, Lane lane, const QString& firstSegment, bool userAsked, qint64* ms, Ticket* ticket, bool* soft = nullptr);
    // The hop's outcome, applied to ticket.domain only. rateRemaining / rateResetMs: the response's x-ratelimit
    // headers (-1 absent), applied on Ok too. A ticket the gate doesn't know (cleared) is ignored.
    void release(const Ticket& ticket, Outcome outcome, qint64 retryAfterMs = -1, double rateRemaining = -1, qint64 rateResetMs = -1);
    // A ticket that made no request (a cache hit, canceled first): not running any more, its start un-counted.
    void unused(const Ticket& ticket);
    // What acquire() would say now, without side effects (userAsked false).
    Verdict state(const QString& host, Lane lane, const QString& firstSegment, qint64* ms, bool* soft = nullptr) const;
    // Walls (healthKey(host), firstSegment) for walledMs, exactly as a bot challenge does: for an answer that came
    // through (200) and can never give a preview (Klipy's oEmbed naming no GIF), so it isn't asked again meanwhile.
    void    markWalled(const QString& host, const QString& firstSegment);
    void    clear(); // everything forgotten; later releases of old tickets are ignored

    // For diagnostics (TESTHOOKS links.gate): one line per group and health entry that is running, cooling,
    // broken or walled ("group reddit page running 1", "health imgur.com cooling hard 512 s"). No paths.
    QStringList describe() const;
    int         runningCount() const; // tickets not yet released
    const Limits& limits() const { return m_limits; }

  private:
    struct LaneState {
        int                              running   = 0;
        qint64                           lastStart = -1;
        QVector<QPair<qint64, quint64>>  starts; // (time, ticket id) within the burst window
    };
    struct Group {
        LaneState lanes[2];
        qint64    lastUsed = 0;
    };
    struct Health {
        qint64                 coolUntil   = 0;
        bool                   soft        = false;
        int                    failures    = 0;
        qint64                 brokenUntil = 0;
        QHash<QString, qint64> walledUntil; // by first path segment
        qint64                 lastUsed    = 0;
    };
    struct Running {
        QString group;
        Lane    lane = Lane::Page;
    };

    qint64  now() const;
    Verdict check(const QString& group, const QString& domain, Lane lane, const QString& firstSegment, bool userAsked, qint64 now,
                  qint64* ms, bool* soft) const;
    int     spacingOf(const QString& group, Lane lane) const;
    int     burstOf(const QString& group, Lane lane) const;
    int     runningLimit(Lane lane) const;
    void    prune(LaneState& lane, qint64 now) const;
    Group&  groupEntry(const QString& group, qint64 now);
    Health& healthEntry(const QString& domain, qint64 now);
    void    evictGroups(qint64 now);
    void    evictHealth(qint64 now);

    mutable QMutex           m_mutex;
    std::function<qint64()>  m_clock;
    Limits                   m_limits;
    QHash<QString, Group>    m_groups;
    QHash<QString, Health>   m_health;
    QHash<quint64, Running>  m_running; // tickets not yet released
    quint64                  m_nextId = 0;
};

// The spacing group of a host: its registrable domain, merged by brand ("youtube", "x", "reddit", "giphy",
// "discord", "twitch", "vimeo", "pinterest", "soundcloud", "spotify", "steam").
QString spacingGroup(const QString& host);
// The health key of a host: its registrable domain (cooldowns, breaker, walled memos).
QString healthKey(const QString& host);
// The plugin's one gate: the live one, or a new one when every holder let go (a function-local weak_ptr under a
// mutex). Both Fetchers get it at construction; each load keeps its own copy until its thread ends.
std::shared_ptr<HostGate> sharedGate();

} // namespace linkpreview
