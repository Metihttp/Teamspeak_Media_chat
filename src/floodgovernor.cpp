#include "floodgovernor.h"

#include <QRegularExpression>

#include <cmath>

namespace {

constexpr int kMaxHintMs = 2 * 60 * 1000;

} // namespace

// ---- buckets ------------------------------------------------------------------------------------

double FloodGovernor::Bucket::points(qint64 nowMs) const
{
    const qint64 elapsed = qMax<qint64>(0, nowMs - at);
    return qMax(0.0, value - static_cast<double>(elapsed) * rate / 1000.0);
}

void FloodGovernor::Bucket::add(double cost, qint64 nowMs)
{
    value = points(nowMs) + cost;
    at    = qMax(at, nowMs);
}

qint64 FloodGovernor::Bucket::timeUntil(double level, qint64 nowMs) const
{
    if (points(nowMs) <= level)
        return nowMs;
    // The value drains from `at` on (a held value only starts draining then).
    return qMax(nowMs, at + static_cast<qint64>(std::ceil((value - level) * 1000.0 / rate)));
}

// ---- governor -----------------------------------------------------------------------------------

FloodGovernor::FloodGovernor()
    : FloodGovernor(Limits())
{
}

FloodGovernor::FloodGovernor(const Limits& limits)
    : m_limits(limits)
{
    m_limits.postRetries         = qMax(0, m_limits.postRetries);
    m_limits.generalRefillPerSec = qMax(0.1, m_limits.generalRefillPerSec);
    m_limits.pluginRefillPerSec  = qMax(0.1, m_limits.pluginRefillPerSec);
    m_limits.generalReserve      = qBound(0, m_limits.generalReserve, qMax(0, m_limits.generalCapacity - m_limits.costText));
    m_limits.pluginReserve       = qBound(0, m_limits.pluginReserve, qMax(0, m_limits.pluginCapacity - m_limits.costPluginCommand));
    m_general.rate               = m_limits.generalRefillPerSec;
    m_plugin.rate                = m_limits.pluginRefillPerSec;
}

int FloodGovernor::retryHintMs(const QString& extraMessage)
{
    static const QRegularExpression hint(QStringLiteral("retry in\\s+(\\d{1,9})\\s*ms"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = hint.match(extraMessage);
    if (!match.hasMatch())
        return -1;
    return qMin(match.captured(1).toInt(), kMaxHintMs);
}

bool FloodGovernor::postReady(qint64 nowMs) const
{
    return nextPostCheckMs(nowMs) <= nowMs;
}

quint64 FloodGovernor::postSent(qint64 nowMs)
{
    m_inFlight      = ++m_nextTicket;
    m_inFlightSince = nowMs;
    ++m_counters.postsSent;
    m_general.add(m_limits.costText, nowMs);
    return m_inFlight;
}

bool FloodGovernor::postAnswered(quint64 ticket, qint64 nowMs, bool flooded, int attempts, int retryHintMs)
{
    if (ticket != 0 && ticket == m_inFlight)
        m_inFlight = 0;
    if (!flooded) {
        answeredOk(nowMs);
        return false;
    }
    ++m_counters.postFloods;
    this->flooded(nowMs, retryHintMs, m_limits.textFloodPauseMs, m_general, m_limits.generalCapacity - m_limits.generalReserve - m_limits.costText);
    return attempts <= m_limits.postRetries;
}

void FloodGovernor::charge(Cost cost, qint64 nowMs)
{
    int points = m_limits.costTransfer;
    if (cost == Cost::FileInfo)
        points = m_limits.costFileInfo;
    else if (cost == Cost::Mkdir)
        points = m_limits.costMkdir;
    m_general.add(points, nowMs);
}

void FloodGovernor::generalFlooded(qint64 nowMs, int retryHintMs)
{
    ++m_counters.otherFloods;
    flooded(nowMs, retryHintMs, m_limits.commandFloodPauseMs, m_general, m_limits.generalCapacity - m_limits.generalReserve - m_limits.costText);
}

bool FloodGovernor::commandReady(qint64 nowMs) const
{
    return nextCommandCheckMs(nowMs) <= nowMs;
}

void FloodGovernor::commandSent(qint64 nowMs)
{
    ++m_counters.commandsSent;
    m_plugin.add(m_limits.costPluginCommand, nowMs);
}

void FloodGovernor::commandFlooded(qint64 nowMs, int retryHintMs)
{
    ++m_counters.commandFloods;
    flooded(nowMs, retryHintMs, m_limits.commandFloodPauseMs, m_plugin, m_limits.pluginCapacity - m_limits.pluginReserve - m_limits.costPluginCommand);
}

void FloodGovernor::answeredOk(qint64 nowMs)
{
    Q_UNUSED(nowMs);
}

void FloodGovernor::flooded(qint64 nowMs, int retryHintMs, int fallbackMs, Bucket& bucket, double oneFits)
{
    const int pause            = retryHintMs >= 0 ? qMin(retryHintMs, kMaxHintMs) + m_limits.floodHintMarginMs : fallbackMs;
    const qint64 until         = nowMs + pause;
    m_counters.lastFloodMs     = nowMs;
    m_counters.lastPauseMs     = pause;
    m_postsPausedUntil         = qMax(m_postsPausedUntil, until);
    m_commandsPausedUntil      = qMax(m_commandsPausedUntil, until);
    // The server is at its limit: when the pause ends, exactly one command fits, then the steady rate.
    bucket.value = qMax(bucket.points(until), qMax(0.0, oneFits));
    bucket.at    = qMax(bucket.at, until);
}

qint64 FloodGovernor::nextPostCheckMs(qint64 nowMs) const
{
    qint64 at = nowMs;
    if (postsPaused(nowMs))
        at = qMax(at, m_postsPausedUntil);
    if (m_inFlight != 0 && nowMs - m_inFlightSince < m_limits.postAnswerWaitMs)
        at = qMax(at, m_inFlightSince + m_limits.postAnswerWaitMs);
    const double room = m_limits.generalCapacity - m_limits.generalReserve - m_limits.costText;
    return qMax(at, m_general.timeUntil(room, nowMs));
}

qint64 FloodGovernor::nextCommandCheckMs(qint64 nowMs) const
{
    qint64 at = nowMs;
    if (commandsPaused(nowMs))
        at = qMax(at, m_commandsPausedUntil);
    const double room = m_limits.pluginCapacity - m_limits.pluginReserve - m_limits.costPluginCommand;
    return qMax(at, m_plugin.timeUntil(room, nowMs));
}
