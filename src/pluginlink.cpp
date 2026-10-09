#include "pluginlink.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTimer>

#include <algorithm>

#include "floodgovernor.h"

namespace {

// public_errors.h (this file stays free of the SDK so the unit tests can build it).
constexpr unsigned int kErrorOk            = 0x0000;
constexpr unsigned int kErrorInvalidClient = 0x0200; // a CLIENT target that left: the whole command fails
constexpr unsigned int kErrorFlooding      = 0x020c;
constexpr int          kMaxHiTargets       = 50;

// Our return codes, also asked for on TeamSpeak's callback thread. Static, so a late answer that
// arrives while the link is being torn down is still recognised (and not printed into the chat).
QMutex& codesMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<QString>& codes()
{
    static QSet<QString> set;
    return set;
}

void addCode(const QString& rc)
{
    QMutexLocker lock(&codesMutex());
    codes().insert(rc);
}

void removeCode(const QString& rc)
{
    QMutexLocker lock(&codesMutex());
    codes().remove(rc);
}

} // namespace

PluginLink::PluginLink(Backend& backend, QObject* parent)
    : PluginLink(backend, Limits(), parent)
{
}

PluginLink::PluginLink(Backend& backend, const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
    , m_limits(limits)
{
    // A member timer: nothing of it outlives the link (see the S0 note on functor timers).
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { pump(); });
}

PluginLink::~PluginLink()
{
    m_timer->stop();
    // Answers still on their way stay recognised (isOwnReturnCode) so TeamSpeak doesn't print them.
}

bool PluginLink::isOwnReturnCode(const QString& returnCode)
{
    if (returnCode.isEmpty())
        return false;
    QMutexLocker lock(&codesMutex());
    return codes().contains(returnCode);
}

bool PluginLink::blocked(quint64 sch) const
{
    const auto it = m_conns.constFind(sch);
    return it != m_conns.constEnd() && it->counters.blocked;
}

PluginLink::Counters PluginLink::counters(quint64 sch) const
{
    return m_conns.value(sch).counters;
}

PluginLink::Counters PluginLink::totals() const
{
    Counters sum;
    for (const Conn& c : m_conns) {
        sum.sent += c.counters.sent;
        sum.answeredOk += c.counters.answeredOk;
        sum.failed += c.counters.failed;
        sum.timeouts += c.counters.timeouts;
        sum.floodBackoffs += c.counters.floodBackoffs;
        sum.received += c.counters.received;
        sum.rejected += c.counters.rejected;
        sum.rateLimited += c.counters.rateLimited;
        sum.blocked = sum.blocked || c.counters.blocked;
    }
    return sum;
}

// ---- sending --------------------------------------------------------------------------------------

void PluginLink::send(quint64 sch, const proto::Message& message, const peers::Target& target, Done done)
{
    sendWith(sch, message, target, Priority::Presence, std::move(done));
}

void PluginLink::sendWith(quint64 sch, const proto::Message& message, const peers::Target& target, Priority priority, Done done, const QString& coalesceKey)
{
    const QByteArray payload = proto::serialize(message);
    if (m_closing) {
        // Shutdown (BYE): now or never, and nobody waits for the answer.
        FloodGovernor* governor = m_backend.governor(sch);
        const qint64   now      = m_backend.nowMs();
        if (payload.isEmpty() || !governor || blocked(sch) || !m_backend.isConnected(sch) || governor->commandsPaused(now))
            return;
        const QString rc = m_backend.newReturnCode();
        if (rc.isEmpty())
            return;
        addCode(rc);
        governor->commandSent(now);
        ++m_conns[sch].counters.sent;
        m_backend.sendCommand(sch, payload, target, rc);
        return;
    }
    if (payload.isEmpty()) {
        if (done)
            done(peers::SendResult::Failed);
        return;
    }
    if (blocked(sch)) {
        if (done)
            done(peers::SendResult::Blocked);
        return;
    }
    Item item;
    item.payload  = payload;
    item.type     = message.type;
    item.target   = target;
    item.priority = priority;
    item.done     = std::move(done);
    item.coalesce = coalesceKey;
    enqueue(sch, std::move(item), false);
    pump();
}

void PluginLink::enqueue(quint64 sch, Item item, bool front)
{
    Conn& c = m_conns[sch];
    if (!item.coalesce.isEmpty()) {
        for (Item& queued : c.queue) {
            if (queued.coalesce != item.coalesce)
                continue;
            if (queued.done)
                m_pendingCallbacks.append({queued.done, peers::SendResult::Superseded});
            queued.payload = item.payload;
            queued.type    = item.type;
            queued.target  = item.target;
            queued.done    = std::move(item.done);
            return;
        }
    }
    // Answers to several people merge into one command (every id is checked again before sending).
    if (item.type == "HI" && !item.target.channel) {
        for (Item& queued : c.queue) {
            if (queued.type != "HI" || queued.target.channel || queued.payload != item.payload)
                continue;
            QVector<quint16> merged = queued.target.clients;
            for (quint16 id : qAsConst(item.target.clients)) {
                if (!merged.contains(id))
                    merged.append(id);
            }
            if (merged.size() > kMaxHiTargets)
                continue;
            queued.target.clients = merged;
            if (item.done)
                m_pendingCallbacks.append({item.done, peers::SendResult::Superseded});
            return;
        }
    }
    item.seq = ++m_seq;
    int at   = 0;
    if (front) {
        while (at < c.queue.size() && c.queue.at(at).priority < item.priority)
            ++at;
    } else {
        at = c.queue.size();
        while (at > 0 && c.queue.at(at - 1).priority > item.priority)
            --at;
    }
    c.queue.insert(at, std::move(item));
    if (c.queue.size() > m_limits.maxQueued) {
        Item dropped = c.queue.takeLast(); // the newest of the least urgent kind
        ++c.counters.failed;
        finish(dropped, peers::SendResult::Failed);
    }
}

void PluginLink::finish(Item& item, peers::SendResult result)
{
    if (item.done)
        m_pendingCallbacks.append({item.done, result});
    item.done = nullptr;
}

void PluginLink::failQueued(quint64 sch, peers::SendResult result)
{
    auto it = m_conns.find(sch);
    if (it == m_conns.end())
        return;
    QList<Item> queue = std::move(it->queue);
    it->queue.clear();
    for (Item& item : queue)
        finish(item, result);
}

void PluginLink::setBlocked(quint64 sch)
{
    Conn& c = m_conns[sch];
    if (c.counters.blocked)
        return;
    c.counters.blocked = true;
    failQueued(sch, peers::SendResult::Blocked);
    emit blockedChanged(sch);
}

void PluginLink::setClosing()
{
    m_closing = true;
    m_timer->stop();
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it)
        it->queue.clear(); // no callbacks: their owners are going away
    m_pendingCallbacks.clear();
}

void PluginLink::schedule(qint64 wakeMs)
{
    if (m_closing || wakeMs < 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(static_cast<int>(qBound<qint64>(0, wakeMs - m_backend.nowMs(), 60 * 60 * 1000)));
}

void PluginLink::pump()
{
    if (m_closing)
        return;
    if (m_pumping) {
        m_repump = true;
        return;
    }
    m_pumping = true;
    qint64 wake = -1;
    do {
        m_repump         = false;
        wake             = -1;
        const qint64 now = m_backend.nowMs();
        const auto   take = [&wake](qint64 at) {
            if (at >= 0)
                wake = wake < 0 ? at : qMin(wake, at);
        };

        // Commands without an answer were not sent (S0: the client drops some silently).
        for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
            if (now - it->sentMs < m_limits.answerTimeoutMs) {
                take(it->sentMs + m_limits.answerTimeoutMs);
                ++it;
                continue;
            }
            InFlight flight = it.value();
            m_linger.insert(it.key(), now + m_limits.returnCodeLingerMs);
            it      = m_inFlight.erase(it);
            Conn& c = m_conns[flight.sch];
            ++c.counters.failed;
            ++c.counters.timeouts;
            finish(flight.item, peers::SendResult::Failed);
        }
        for (auto it = m_linger.begin(); it != m_linger.end();) {
            if (now >= it.value()) {
                removeCode(it.key());
                it = m_linger.erase(it);
            } else {
                take(it.value());
                ++it;
            }
        }

        for (const quint64 sch : m_conns.keys()) {
            Conn& c = m_conns[sch];
            if (c.queue.isEmpty())
                continue;
            if (!m_backend.isConnected(sch)) {
                failQueued(sch, peers::SendResult::Failed);
                continue;
            }
            while (!c.queue.isEmpty()) {
                FloodGovernor* governor = m_backend.governor(sch);
                if (!governor) {
                    failQueued(sch, peers::SendResult::Failed);
                    break;
                }
                if (!governor->commandReady(now)) {
                    // -1 while chat posts are pending: Core says when they are out (pump()).
                    const qint64 at = governor->nextCommandCheckMs(now);
                    if (at >= 0)
                        take(qMax(at, now + 1));
                    break;
                }
                Item item = c.queue.takeFirst();
                if (!item.target.channel) {
                    QVector<quint16> visible;
                    for (quint16 id : qAsConst(item.target.clients)) {
                        if (id != 0 && m_backend.clientVisible(sch, id) && !visible.contains(id))
                            visible.append(id);
                    }
                    if (visible.isEmpty()) {
                        ++c.counters.failed;
                        finish(item, peers::SendResult::Failed);
                        continue;
                    }
                    item.target.clients = visible;
                }
                const QString rc = m_backend.newReturnCode();
                if (rc.isEmpty()) {
                    ++c.counters.failed;
                    finish(item, peers::SendResult::Failed);
                    continue;
                }
                addCode(rc);
                governor->commandSent(now);
                ++item.attempts;
                ++c.counters.sent;
                const QByteArray    payload = item.payload;
                const peers::Target target  = item.target;
                m_inFlight.insert(rc, {sch, std::move(item), now});
                take(now + m_limits.answerTimeoutMs);
                m_backend.sendCommand(sch, payload, target, rc);
            }
        }
    } while (m_repump);
    m_pumping = false;
    schedule(wake);
    runCallbacks();
}

void PluginLink::runCallbacks()
{
    while (!m_pendingCallbacks.isEmpty()) {
        const auto call = m_pendingCallbacks.takeFirst();
        if (call.first)
            call.first(call.second);
    }
}

// ---- answers --------------------------------------------------------------------------------------

bool PluginLink::onServerError(quint64 sch, unsigned int error, const QString& returnCode, const QString& extraMessage, bool permissionError)
{
    if (!isOwnReturnCode(returnCode))
        return false;
    removeCode(returnCode);
    const qint64   now      = m_backend.nowMs();
    FloodGovernor* governor = m_backend.governor(sch);
    const bool     flooded  = error == kErrorFlooding && !permissionError;
    const auto     pause    = [&](Conn& c) {
        // S0: both buckets pause for the server's hint + its margin (or the fallback without one); then
        // exactly one command fits. The governor is the one place that knows.
        if (governor)
            governor->commandFlooded(now, FloodGovernor::retryHintMs(extraMessage));
        ++c.counters.floodBackoffs;
        m_backend.floodStateChanged(sch);
    };

    auto it = m_inFlight.find(returnCode);
    if (it == m_inFlight.end()) {
        // A late answer to a command that timed out.
        m_linger.remove(returnCode);
        if (flooded && !m_closing)
            pause(m_conns[sch]);
        pump();
        return true;
    }
    InFlight flight = it.value();
    m_inFlight.erase(it);
    Conn& c = m_conns[flight.sch];

    if (permissionError) {
        ++c.counters.failed;
        finish(flight.item, peers::SendResult::Blocked);
        setBlocked(flight.sch);
    } else if (error == kErrorOk) {
        if (governor)
            governor->answeredOk(now);
        ++c.counters.answeredOk;
        finish(flight.item, peers::SendResult::Ok);
    } else if (flooded) {
        pause(c);
        if (flight.item.priority == Priority::Presence && flight.item.attempts <= m_limits.presenceRetries) {
            enqueue(flight.sch, std::move(flight.item), true);
        } else {
            ++c.counters.failed;
            finish(flight.item, peers::SendResult::Flooded);
        }
    } else if (error == kErrorInvalidClient && !flight.item.target.channel && flight.item.target.clients.size() > 1 && flight.item.attempts <= 1) {
        enqueue(flight.sch, std::move(flight.item), true); // someone left meanwhile: the others again
    } else {
        ++c.counters.failed;
        // Our own channel always exists: an error on the HELLO means this server doesn't let us.
        if (flight.item.type == "HELLO" && flight.item.target.channel) {
            finish(flight.item, peers::SendResult::Blocked);
            setBlocked(flight.sch);
        } else {
            finish(flight.item, peers::SendResult::Failed);
        }
    }
    pump();
    return true;
}

void PluginLink::connectionLost(quint64 sch)
{
    failQueued(sch, peers::SendResult::Failed);
    for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
        if (it->sch != sch) {
            ++it;
            continue;
        }
        m_linger.insert(it.key(), m_backend.nowMs() + m_limits.returnCodeLingerMs);
        finish(it->item, peers::SendResult::Failed);
        it = m_inFlight.erase(it);
    }
    const bool wasBlocked = blocked(sch);
    m_conns.remove(sch); // a new connection starts unblocked
    if (wasBlocked)
        emit blockedChanged(sch);
    runCallbacks();
}

// ---- receiving ------------------------------------------------------------------------------------

bool PluginLink::takeInboundToken(Conn& conn, const QString& uid, qint64 now)
{
    if (conn.inbound.size() > m_limits.maxRateEntries) {
        for (auto it = conn.inbound.begin(); it != conn.inbound.end();) {
            if (now - it->atMs >= m_limits.inboundWindowMs)
                it = conn.inbound.erase(it);
            else
                ++it;
        }
        if (conn.inbound.size() > m_limits.maxRateEntries)
            conn.inbound.clear();
    }
    RateBucket&  bucket = conn.inbound[uid];
    const double burst  = m_limits.inboundBurst;
    if (bucket.atMs < 0) {
        bucket.tokens = burst;
    } else {
        const double refill = static_cast<double>(qMax<qint64>(0, now - bucket.atMs)) * burst / qMax(1, m_limits.inboundWindowMs);
        bucket.tokens       = qMin(burst, bucket.tokens + refill);
    }
    bucket.atMs = now;
    if (bucket.tokens < 1.0)
        return false;
    bucket.tokens -= 1.0;
    return true;
}

void PluginLink::onCommand(quint64 sch, quint16 invoker, const QString& uid, const QString& name, const QByteArray& payload)
{
    if (m_closing || invoker == 0 || uid.isEmpty())
        return;
    if (invoker == m_backend.ownClientId(sch))
        return; // channel commands come back to the sender
    Conn& c = m_conns[sch];
    if (payload.size() > proto::kMaxRecvBytes) {
        ++c.counters.rejected;
        return;
    }
    if (!takeInboundToken(c, uid, m_backend.nowMs())) {
        ++c.counters.rateLimited;
        return;
    }
    const std::optional<proto::Message> message = proto::parse(payload);
    if (!message) {
        ++c.counters.rejected;
        return;
    }
    ++c.counters.received;
    emit received(sch, invoker, uid, name, *message);
}
