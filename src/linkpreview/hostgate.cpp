#include "hostgate.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <algorithm>
#include <memory>

#include "netpolicy.h"
#include "providers.h"

namespace linkpreview {

namespace {

int laneIndex(HostGate::Lane lane)
{
    return lane == HostGate::Lane::Page ? 0 : 1;
}

qint64 defaultClock()
{
    static QElapsedTimer* timer = [] {
        auto* t = new QElapsedTimer; // never deleted (plain data)
        t->start();
        return t;
    }();
    return timer->elapsed();
}

} // namespace

HostGate::HostGate(std::function<qint64()> clockMs, const Limits& limits)
    : m_clock(clockMs ? std::move(clockMs) : std::function<qint64()>(&defaultClock))
    , m_limits(limits)
{
}

qint64 HostGate::now() const
{
    return m_clock();
}

int HostGate::runningLimit(Lane lane) const
{
    return lane == Lane::Page ? m_limits.pageRunning : m_limits.mediaRunning;
}

int HostGate::spacingOf(const QString& group, Lane lane) const
{
    if (lane == Lane::Page) {
        const int spacing = providers::pageSpacingMs(group);
        return spacing != 1000 ? spacing : m_limits.pageSpacingMs;
    }
    const int spacing = providers::mediaSpacingMs(group);
    return spacing >= 0 ? spacing : m_limits.mediaSpacingMs;
}

int HostGate::burstOf(const QString& group, Lane lane) const
{
    if (lane == Lane::Page)
        return m_limits.pageBurst;
    const int burst = providers::mediaBurst(group);
    return burst > 0 ? burst : m_limits.mediaBurst;
}

void HostGate::prune(LaneState& lane, qint64 now) const
{
    const qint64 since = now - m_limits.burstWindowMs;
    while (!lane.starts.isEmpty() && lane.starts.first().first <= since)
        lane.starts.removeFirst();
}

HostGate::Verdict HostGate::check(const QString& group, const QString& domain, Lane lane, const QString& firstSegment, bool userAsked, qint64 now,
                                  qint64* ms, bool* soft) const
{
    if (soft)
        *soft = false;
    const auto health = m_health.constFind(domain);
    if (health != m_health.constEnd()) {
        if (health->coolUntil > now) {
            *ms = health->coolUntil - now;
            if (soft)
                *soft = health->soft;
            return Verdict::Cooling;
        }
        const qint64 walled = health->walledUntil.value(firstSegment, 0);
        if (walled > now) {
            *ms = walled - now;
            return Verdict::Walled;
        }
        if (health->brokenUntil > now && !userAsked) {
            *ms = health->brokenUntil - now;
            return Verdict::Broken;
        }
    }
    const auto entry = m_groups.constFind(group);
    if (entry == m_groups.constEnd()) {
        *ms = 0;
        return Verdict::Go;
    }
    LaneState state = entry->lanes[laneIndex(lane)];
    prune(state, now);
    qint64 wait = 0;
    if (state.running >= runningLimit(lane))
        wait = qMax<qint64>(wait, qMax(200, spacingOf(group, lane))); // until a running request ends (polled)
    if (state.lastStart >= 0)
        wait = qMax<qint64>(wait, state.lastStart + spacingOf(group, lane) - now);
    if (state.starts.size() >= burstOf(group, lane))
        wait = qMax<qint64>(wait, state.starts.first().first + m_limits.burstWindowMs - now);
    if (wait > 0) {
        *ms = qMin<qint64>(wait, m_limits.burstWindowMs);
        return Verdict::Wait;
    }
    *ms = 0;
    return Verdict::Go;
}

HostGate::Group& HostGate::groupEntry(const QString& group, qint64 now)
{
    auto it = m_groups.find(group);
    if (it == m_groups.end()) {
        if (m_groups.size() >= m_limits.maxKeys)
            evictGroups(now);
        it = m_groups.insert(group, Group());
    }
    it->lastUsed = now;
    return *it;
}

HostGate::Health& HostGate::healthEntry(const QString& domain, qint64 now)
{
    auto it = m_health.find(domain);
    if (it == m_health.end()) {
        if (m_health.size() >= m_limits.maxKeys)
            evictHealth(now);
        it = m_health.insert(domain, Health());
    }
    it->lastUsed = now;
    return *it;
}

void HostGate::evictGroups(qint64 now)
{
    // Idle groups first (nothing running, no start for idleMs), then the least recently used one with nothing running.
    for (auto it = m_groups.begin(); it != m_groups.end();) {
        const bool running = it->lanes[0].running > 0 || it->lanes[1].running > 0;
        if (!running && now - it->lastUsed > m_limits.idleMs)
            it = m_groups.erase(it);
        else
            ++it;
    }
    while (m_groups.size() >= m_limits.maxKeys) {
        auto oldest = m_groups.end();
        for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
            if (it->lanes[0].running > 0 || it->lanes[1].running > 0)
                continue;
            if (oldest == m_groups.end() || it->lastUsed < oldest->lastUsed)
                oldest = it;
        }
        if (oldest == m_groups.end())
            return; // everything is running: let it grow
        m_groups.erase(oldest);
    }
}

void HostGate::evictHealth(qint64 now)
{
    const auto quiet = [now](const Health& h) {
        bool walled = false;
        for (const qint64 until : h.walledUntil)
            walled = walled || until > now;
        return h.coolUntil <= now && h.brokenUntil <= now && !walled && h.failures == 0;
    };
    for (auto it = m_health.begin(); it != m_health.end();) {
        if (quiet(*it) && now - it->lastUsed > m_limits.idleMs)
            it = m_health.erase(it);
        else
            ++it;
    }
    while (m_health.size() >= m_limits.maxKeys) {
        auto oldest = m_health.end();
        for (auto it = m_health.begin(); it != m_health.end(); ++it) {
            if (oldest == m_health.end() || (quiet(*it) && !quiet(*oldest)) || (quiet(*it) == quiet(*oldest) && it->lastUsed < oldest->lastUsed))
                oldest = it;
        }
        m_health.erase(oldest);
    }
}

HostGate::Verdict HostGate::acquire(const QString& host, Lane lane, const QString& firstSegment, bool userAsked, qint64* ms, Ticket* ticket, bool* soft)
{
    const QString group  = spacingGroup(host);
    const QString domain = healthKey(host);
    QMutexLocker  lock(&m_mutex);
    const qint64  t       = now();
    qint64        wait    = 0;
    const Verdict verdict = check(group, domain, lane, firstSegment, userAsked, t, &wait, soft);
    if (ms)
        *ms = wait;
    if (verdict != Verdict::Go)
        return verdict;
    Group&     g     = groupEntry(group, t);
    LaneState& state = g.lanes[laneIndex(lane)];
    prune(state, t);
    const quint64 id = ++m_nextId;
    ++state.running;
    state.lastStart = t;
    state.starts.append(qMakePair(t, id));
    m_running.insert(id, Running{group, lane});
    if (ticket) {
        ticket->group        = group;
        ticket->domain       = domain;
        ticket->firstSegment = firstSegment;
        ticket->lane         = lane;
        ticket->id           = id;
    }
    return Verdict::Go;
}

void HostGate::release(const Ticket& ticket, Outcome outcome, qint64 retryAfterMs, double rateRemaining, qint64 rateResetMs)
{
    QMutexLocker lock(&m_mutex);
    const auto   running = m_running.find(ticket.id);
    if (ticket.id == 0 || running == m_running.end())
        return; // cleared, or never ours
    const qint64 t = now();
    auto         g = m_groups.find(running->group);
    if (g != m_groups.end()) {
        LaneState& state = g->lanes[laneIndex(running->lane)];
        state.running    = qMax(0, state.running - 1);
        g->lastUsed      = t;
    }
    m_running.erase(running);

    Health& health = healthEntry(ticket.domain, t);
    switch (outcome) {
    case Outcome::RateLimited: {
        qint64 cool = retryAfterMs > 0 ? retryAfterMs : (rateResetMs > 0 ? rateResetMs : m_limits.defaultCooldownMs);
        cool        = qBound<qint64>(qMax(m_limits.minCooldownMs, providers::minCooldownMs(ticket.domain)), cool, m_limits.maxCooldownMs);
        if (t + cool > health.coolUntil) {
            health.coolUntil = t + cool;
            health.soft      = false;
        }
        break;
    }
    case Outcome::Walled:
        health.walledUntil.insert(ticket.firstSegment, t + m_limits.walledMs);
        break;
    case Outcome::ServerError:
    case Outcome::Timeout:
    case Outcome::Offline:
        if (++health.failures >= m_limits.breakerFailures) {
            health.brokenUntil = t + m_limits.breakerMs;
            health.failures    = 0;
        }
        break;
    case Outcome::Ok:
        health.failures    = 0;
        health.brokenUntil = 0;
        break;
    case Outcome::Other:
        break;
    }
    // x-ratelimit: a soft cooldown until the reset (any answer but a 429, which is a hard one already).
    if (outcome != Outcome::RateLimited && rateRemaining >= 0 && rateRemaining < 1 && rateResetMs > 0) {
        const qint64 cool = qBound<qint64>(1000, rateResetMs, m_limits.maxCooldownMs);
        if (t + cool > health.coolUntil) {
            health.coolUntil = t + cool;
            health.soft      = true;
        }
    }
}

void HostGate::unused(const Ticket& ticket)
{
    QMutexLocker lock(&m_mutex);
    const auto   running = m_running.find(ticket.id);
    if (ticket.id == 0 || running == m_running.end())
        return;
    auto g = m_groups.find(running->group);
    if (g != m_groups.end()) {
        LaneState& state = g->lanes[laneIndex(running->lane)];
        state.running    = qMax(0, state.running - 1);
        for (int i = state.starts.size() - 1; i >= 0; --i) {
            if (state.starts.at(i).second == ticket.id)
                state.starts.remove(i);
        }
        state.lastStart = -1;
        for (const auto& start : qAsConst(state.starts))
            state.lastStart = qMax(state.lastStart, start.first);
    }
    m_running.erase(running);
}

HostGate::Verdict HostGate::state(const QString& host, Lane lane, const QString& firstSegment, qint64* ms, bool* soft) const
{
    const QString group  = spacingGroup(host);
    const QString domain = healthKey(host);
    QMutexLocker  lock(&m_mutex);
    qint64        wait = 0;
    const Verdict v    = check(group, domain, lane, firstSegment, false, now(), &wait, soft);
    if (ms)
        *ms = wait;
    return v;
}

void HostGate::markWalled(const QString& host, const QString& firstSegment)
{
    const QString domain = healthKey(host);
    if (domain.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    const qint64 t = now();
    healthEntry(domain, t).walledUntil.insert(firstSegment, t + m_limits.walledMs);
}

void HostGate::clear()
{
    QMutexLocker lock(&m_mutex);
    m_groups.clear();
    m_health.clear();
    m_running.clear();
}

void HostGate::forgetFailures()
{
    QMutexLocker lock(&m_mutex);
    for (auto it = m_health.begin(); it != m_health.end(); ++it) {
        it->failures    = 0;
        it->brokenUntil = 0;
        it->walledUntil.clear();
    }
}

QStringList HostGate::describe() const
{
    QMutexLocker lock(&m_mutex);
    const qint64 t = now();
    QStringList  out;
    for (auto it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) {
        for (int lane = 0; lane < 2; ++lane) {
            if (it->lanes[lane].running > 0)
                out << QString::fromLatin1("group %1 %2 running %3").arg(it.key(), QString::fromLatin1(lane == 0 ? "page" : "media")).arg(it->lanes[lane].running);
        }
    }
    for (auto it = m_health.constBegin(); it != m_health.constEnd(); ++it) {
        if (it->coolUntil > t)
            out << QString::fromLatin1("health %1 cooling %2 %3 s").arg(it.key(), QString::fromLatin1(it->soft ? "soft" : "hard")).arg((it->coolUntil - t + 999) / 1000);
        if (it->brokenUntil > t)
            out << QString::fromLatin1("health %1 broken %2 s").arg(it.key()).arg((it->brokenUntil - t + 999) / 1000);
        for (auto w = it->walledUntil.constBegin(); w != it->walledUntil.constEnd(); ++w) {
            if (w.value() > t)
                out << QString::fromLatin1("health %1 walled /%2 %3 s").arg(it.key(), w.key()).arg((w.value() - t + 999) / 1000);
        }
    }
    out.sort();
    return out;
}

int HostGate::runningCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_running.size();
}

// ---- keys -----------------------------------------------------------------------------------------------------------

QString healthKey(const QString& host)
{
    return net::registrableDomain(host);
}

QString spacingGroup(const QString& host)
{
    const QString domain = net::registrableDomain(host);
    struct BrandGroup {
        const char* group;
        const char* domains; // separated by spaces
    };
    static const BrandGroup kGroups[] = {
        {"youtube", "youtube.com youtu.be ytimg.com"},
        {"x", "x.com twitter.com twimg.com"},
        {"reddit", "reddit.com redd.it redditmedia.com redditstatic.com"},
        {"giphy", "giphy.com gph.is"},
        {"discord", "discord.com discord.gg discordapp.com discordapp.net"},
        {"twitch", "twitch.tv jtvnw.net"},
        {"vimeo", "vimeo.com vimeocdn.com"},
        {"pinterest", "pin.it pinimg.com"},
        {"soundcloud", "soundcloud.com sndcdn.com"},
        {"spotify", "spotify.com scdn.co spotifycdn.com"},
        {"steam", "steampowered.com steamcommunity.com steamstatic.com steamusercontent.com s.team"},
    };
    if (domain.startsWith(QLatin1String("pinterest.")))
        return QString::fromLatin1("pinterest");
    for (const BrandGroup& g : kGroups) {
        const QStringList domains = QString::fromLatin1(g.domains).split(QLatin1Char(' '));
        if (domains.contains(domain))
            return QString::fromLatin1(g.group);
    }
    return domain;
}

std::shared_ptr<HostGate> sharedGate()
{
    static QMutex                  mutex;
    static std::weak_ptr<HostGate> current;
    QMutexLocker                   lock(&mutex);
    std::shared_ptr<HostGate>      gate = current.lock();
    if (!gate) {
        gate    = std::make_shared<HostGate>();
        current = gate;
    }
    return gate;
}

} // namespace linkpreview
