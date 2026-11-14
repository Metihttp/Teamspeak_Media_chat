#include "health.h"

#include <QElapsedTimer>
#include <QSet>

namespace proxy {

Health::Health(std::function<qint64()> clockMs)
    : m_clock(std::move(clockMs))
{
}

qint64 Health::now() const
{
    if (m_clock)
        return m_clock();
    QElapsedTimer t;
    t.start();
    return t.msecsSinceReference();
}

void Health::reset(quint64 generation, bool proxyOnThisPc)
{
    m_generation  = generation;
    m_onThisPc    = proxyOnThisPc;
    m_state       = State::Unknown;
    m_lastFailure = Failure::None;
    m_downAt      = 0;
    m_lastOkAt    = -1;
    m_slotTakenAt = -1;
    m_ambiguous.clear();
}

void Health::goDown(Failure failure)
{
    m_state       = State::Down;
    m_lastFailure = failure;
    m_downAt      = now();
    m_slotTakenAt = -1;
    m_ambiguous.clear();
}

bool Health::report(quint64 generation, Failure failure, Step step, const QString& site)
{
    if (generation != m_generation || !isRouteLevel(failure))
        return false;
    const qint64 t = now();
    if (m_state == State::Probation) {
        goDown(failure); // the probe (or anything else in probation) failed at the route: down again, the same delay
        return true;
    }
    if (m_state == State::Down) {
        m_lastFailure = failure; // stays down; the delay isn't restarted (no backoff, F21)
        return false;
    }
    if (isDecisive(failure, step)) {
        goDown(failure);
        return true;
    }
    m_ambiguous.append(qMakePair(t, site));
    while (!m_ambiguous.isEmpty() && t - m_ambiguous.first().first > kWindowMs)
        m_ambiguous.removeFirst();
    QSet<QString> sites;
    for (const auto& entry : qAsConst(m_ambiguous))
        sites.insert(entry.second);
    const bool recentOk = m_lastOkAt >= 0 && t - m_lastOkAt <= kWindowMs;
    if (m_ambiguous.size() >= kAmbiguousCount && sites.size() >= kAmbiguousSites && !recentOk) {
        goDown(failure);
        return true;
    }
    m_lastFailure = failure; // shown only once down; kept for the status row
    return false;
}

bool Health::reportOk(quint64 generation)
{
    if (generation != m_generation)
        return false;
    m_lastOkAt         = now();
    const bool changed = m_state != State::Working;
    m_state            = State::Working;
    m_slotTakenAt      = -1;
    m_ambiguous.clear();
    if (changed)
        m_lastFailure = Failure::None;
    return changed;
}

bool Health::tick()
{
    const qint64 t = now();
    if (m_state == State::Down && t - m_downAt >= probationDelayMs()) {
        m_state       = State::Probation;
        m_slotTakenAt = -1;
        return true;
    }
    if (m_state == State::Probation && m_slotTakenAt >= 0 && t - m_slotTakenAt >= kProbeSlotMs) {
        // The probe ended without a report (canceled, cached, busy, held by its site): another may try. Review fix: a
        // change too, so the links paused meanwhile go back to the on-screen pass (nothing else would wake them).
        m_slotTakenAt = -1;
        return true;
    }
    return false;
}

bool Health::admits(bool userAsked)
{
    if (userAsked)
        return true;
    tick();
    switch (m_state) {
    case State::Unknown:
    case State::Working:
        return true;
    case State::Down:
        return false;
    case State::Probation:
        if (m_slotTakenAt >= 0)
            return false;
        m_slotTakenAt = now();
        return true;
    }
    return true;
}

qint64 Health::msUntilTick() const
{
    const qint64 t = now();
    if (m_state == State::Down)
        return qMax<qint64>(0, m_downAt + probationDelayMs() - t);
    if (m_state == State::Probation && m_slotTakenAt >= 0)
        return qMax<qint64>(0, m_slotTakenAt + kProbeSlotMs - t);
    return -1;
}

} // namespace proxy
