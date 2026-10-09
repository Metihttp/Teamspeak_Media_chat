#include "floodgovernor.h"

#include <cmath>

FloodGovernor::FloodGovernor()
    : FloodGovernor(Limits())
{
}

FloodGovernor::FloodGovernor(const Limits& limits)
    : m_limits(limits)
{
    m_limits.postRetries      = qMax(0, m_limits.postRetries);
    m_limits.commandBurst     = qMax(1, m_limits.commandBurst);
    m_limits.commandRefillMs  = qMax(1, m_limits.commandRefillMs);
    m_limits.commandBackoffMs = qMax(0, m_limits.commandBackoffMs);
    m_limits.commandBackoffMaxMs = qMax(m_limits.commandBackoffMs, m_limits.commandBackoffMaxMs);
}

void FloodGovernor::setPendingPosts(int count)
{
    m_pendingPosts = qMax(0, count);
}

bool FloodGovernor::postReady(qint64 nowMs) const
{
    if (postsPaused(nowMs))
        return false;
    if (m_inFlight != 0 && nowMs - m_inFlightSince < m_limits.postAnswerWaitMs)
        return false;
    if (m_limits.postMinSpacingMs > 0 && m_lastPostMs >= 0 && nowMs - m_lastPostMs < m_limits.postMinSpacingMs)
        return false;
    return true;
}

quint64 FloodGovernor::postSent(qint64 nowMs)
{
    m_inFlight      = ++m_nextTicket;
    m_inFlightSince = nowMs;
    m_lastPostMs    = nowMs;
    ++m_counters.postsSent;
    spendToken(nowMs); // the same budget on the server; a post never waits for a token
    return m_inFlight;
}

bool FloodGovernor::postAnswered(quint64 ticket, qint64 nowMs, bool flooded, int attempts)
{
    if (ticket != 0 && ticket == m_inFlight)
        m_inFlight = 0;
    if (!flooded) {
        answeredOk(nowMs);
        return false;
    }
    ++m_counters.postFloods;
    this->flooded(nowMs);
    return attempts <= m_limits.postRetries;
}

bool FloodGovernor::commandReady(qint64 nowMs) const
{
    if (m_pendingPosts > 0 || commandsPaused(nowMs))
        return false;
    if (m_inFlight != 0 && nowMs - m_inFlightSince < m_limits.postAnswerWaitMs)
        return false;
    return tokens(nowMs) >= 1.0;
}

void FloodGovernor::commandSent(qint64 nowMs)
{
    ++m_counters.commandsSent;
    spendToken(nowMs);
}

void FloodGovernor::commandFlooded(qint64 nowMs)
{
    ++m_counters.commandFloods;
    flooded(nowMs);
}

void FloodGovernor::answeredOk(qint64 nowMs)
{
    if (m_counters.lastFloodMs >= 0 && nowMs - m_counters.lastFloodMs >= m_limits.commandCalmResetMs)
        m_counters.currentBackoffMs = 0;
}

void FloodGovernor::flooded(qint64 nowMs)
{
    const bool recent = m_counters.lastFloodMs >= 0 && nowMs - m_counters.lastFloodMs < m_limits.commandCalmResetMs;
    if (recent && m_counters.currentBackoffMs > 0)
        m_counters.currentBackoffMs = qMin(m_counters.currentBackoffMs * 2, m_limits.commandBackoffMaxMs);
    else
        m_counters.currentBackoffMs = m_limits.commandBackoffMs;
    m_counters.lastFloodMs = nowMs;
    m_postsPausedUntil     = qMax(m_postsPausedUntil, nowMs + m_limits.postFloodPauseMs);
    m_commandsPausedUntil  = qMax(m_commandsPausedUntil, nowMs + m_counters.currentBackoffMs);
    // The server is counting: start the bucket from empty.
    m_tokens   = 0.0;
    m_tokensAt = nowMs;
}

double FloodGovernor::tokens(qint64 nowMs) const
{
    if (m_tokensAt < 0)
        return m_limits.commandBurst;
    const double refilled = m_tokens + static_cast<double>(qMax<qint64>(0, nowMs - m_tokensAt)) / m_limits.commandRefillMs;
    return qMin(static_cast<double>(m_limits.commandBurst), refilled);
}

void FloodGovernor::spendToken(qint64 nowMs)
{
    m_tokens   = qMax(0.0, tokens(nowMs) - 1.0);
    m_tokensAt = nowMs;
}

qint64 FloodGovernor::nextPostCheckMs(qint64 nowMs) const
{
    qint64 at = nowMs;
    if (postsPaused(nowMs))
        at = qMax(at, m_postsPausedUntil);
    if (m_inFlight != 0 && nowMs - m_inFlightSince < m_limits.postAnswerWaitMs)
        at = qMax(at, m_inFlightSince + m_limits.postAnswerWaitMs);
    if (m_limits.postMinSpacingMs > 0 && m_lastPostMs >= 0 && nowMs - m_lastPostMs < m_limits.postMinSpacingMs)
        at = qMax(at, m_lastPostMs + m_limits.postMinSpacingMs);
    return at;
}

qint64 FloodGovernor::nextCommandCheckMs(qint64 nowMs) const
{
    if (m_pendingPosts > 0)
        return -1; // until the posts are out (setPendingPosts)
    qint64 at = nowMs;
    if (commandsPaused(nowMs))
        at = qMax(at, m_commandsPausedUntil);
    if (m_inFlight != 0 && nowMs - m_inFlightSince < m_limits.postAnswerWaitMs)
        at = qMax(at, m_inFlightSince + m_limits.postAnswerWaitMs);
    const double have = tokens(at);
    if (have < 1.0)
        at += static_cast<qint64>(std::ceil((1.0 - have) * m_limits.commandRefillMs));
    return at;
}
