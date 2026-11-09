#include "linkpreview/autoload.h"

#include "serversettings.h"
#include "settings.h"
#include "textreact.h" // kFutureSec

namespace linkpreview {
namespace autoload {

namespace {

qint64 megabytes(int mb)
{
    return static_cast<qint64>(qMax(0, mb)) * 1024 * 1024;
}

bool dataSaverOn(const Settings& s, const QString& serverUid)
{
    if (const ServerOverrides* own = s.overridesFor(serverUid)) {
        if (own->dataSaver)
            return *own->dataSaver;
    }
    return s.dataSaver;
}

} // namespace

Wait waitFor(const Input& in)
{
    const bool chatWithLp = in.chat == Chat::Channel || in.chat == Chat::Private; // the server chat has no LP
    if (in.own) {
        // Our own pipeline works on it, or hasn't seen our fresh message yet (plugin.cpp hands it over on its own
        // schedule). Never gated by the switch: it contacts nothing, and our own download is shown at once.
        if (in.ownRecord == OwnRecord::Pending)
            return Wait::Own;
        if (in.ownRecord == OwnRecord::None && in.makeOn && chatWithLp && in.timeSec >= 0 && in.nowSec - in.timeSec >= 0
            && in.nowSec - in.timeSec <= kOwnGraceSec)
            return Wait::Own;
        return Wait::None;
    }
    if (!in.autoOn)
        return Wait::None; // decision A: off is 2.4.0's click card
    if (!chatWithLp || in.timeSec < 0)
        return Wait::None; // no LP can come (the server chat), or none could be matched to it (no key)
    if (in.nowSec - in.timeSec >= in.budgetSec || in.timeSec - in.nowSec > textreact::kFutureSec)
        return Wait::None; // its time ran out (a message read from history too), or its time can't be right
    if (in.skipped || !in.senderHere)
        return Wait::None; // the sender's plugin won't make it, or the sender left
    if (in.cap == SenderCap::Checking)
        return Wait::Checking; // we are still asking whether they make one (the card doesn't claim they do)
    return in.cap == SenderCap::Yes ? Wait::Sender : Wait::None;
}

bool senderHere(Chat chat, const Presence& presence)
{
    switch (chat) {
    case Chat::Private:
        return presence.client != 0;
    case Chat::Channel:
        return presence.client != 0 && presence.inOurChannel;
    case Chat::Server:
    case Chat::Unknown:
        break;
    }
    return false;
}

Presence PresenceMemo::get(quint64 sch, const QString& uid, qint64 nowMs, const Lookup& lookup)
{
    const QString key = QString::number(sch) + QLatin1Char('/') + uid;
    const auto    it  = m_entries.constFind(key);
    if (it != m_entries.constEnd() && nowMs - it->atMs >= 0 && nowMs - it->atMs < kTtlMs)
        return it->presence;
    Entry e;
    e.atMs     = nowMs;
    e.presence = lookup ? lookup(sch, uid) : Presence();
    if (m_entries.size() >= kMaxEntries)
        m_entries.clear();
    m_entries.insert(key, e);
    return e.presence;
}

int waitSec(Kind kind)
{
    switch (kind) {
    case Kind::YouTube:
        return 30;
    case Kind::Image:
    case Kind::Gif:
        return 45;
    case Kind::Video:
        return 120;
    case Kind::None:
        break;
    }
    return 0;
}

int maxWaitSec(Kind kind)
{
    const int own = waitSec(kind);
    return own > 0 ? kSenderQueueSec + own : 0;
}

int SenderQueue::add(const QString& sender, qint64 timeSec, Kind kind)
{
    const int own = waitSec(kind);
    if (timeSec < 0 || own <= 0)
        return 0;
    const auto   it    = m_busyUntil.constFind(sender);
    const qint64 busy  = it == m_busyUntil.constEnd() ? -1 : it.value();
    qint64       start = timeSec;
    if (busy > start)
        start = qMin(busy, timeSec + kSenderQueueSec); // queued any longer, the job is dropped (and LPNO comes)
    const qint64 done = start + own;
    m_busyUntil.insert(sender, qMax(busy, done));
    return static_cast<int>(done - timeSec);
}

qint64 clickCap(Kind kind)
{
    switch (kind) {
    case Kind::YouTube:
        return 2LL * 1024 * 1024;
    case Kind::Video:
        return 50LL * 1024 * 1024;
    default:
        return 25LL * 1024 * 1024;
    }
}

qint64 autoCap(Kind kind, const Settings& s, const QString& serverUid)
{
    switch (kind) {
    case Kind::YouTube:
        return clickCap(kind); // oEmbed and the thumbnail: a preview, always small (also with data saver)
    case Kind::Image:
    case Kind::Gif:
        if (dataSaverOn(s, serverUid) || !s.autoDownloadImages)
            return 0;
        return qMin(megabytes(s.autoDownloadMaxMB), clickCap(kind));
    case Kind::Video:
        if (dataSaverOn(s, serverUid))
            return 0;
        if (s.videoAutoDownloadMB > 0)
            return qMin(megabytes(s.videoAutoDownloadMB), clickCap(kind));
        // "Download videos automatically" at 0 (the default, "when you press play") is about uploaded videos, which
        // show their poster without the file. A website's video has no poster without its file, so link videos use
        // the picture limit then (15 MB by default): a link video loads by itself, a large one stays a click.
        if (!s.autoDownloadImages)
            return 0;
        return qMin(megabytes(s.autoDownloadMaxMB), clickCap(kind));
    case Kind::None:
        break;
    }
    return 0;
}

// ---- SenderBudget -------------------------------------------------------------------------------------

SenderBudget::SenderBudget()
    : SenderBudget(Limits())
{
}

SenderBudget::SenderBudget(const Limits& limits)
    : m_limits(limits)
{
}

double SenderBudget::tokensAt(const Bucket& b, qint64 nowMs) const
{
    if (b.atMs < 0 || nowMs < b.atMs)
        return m_limits.burst; // new, or the clock went back: full
    const double refill = static_cast<double>(nowMs - b.atMs) / static_cast<double>(qMax<qint64>(1, m_limits.refillMs));
    return qMin(static_cast<double>(m_limits.burst), b.tokens + refill);
}

bool SenderBudget::has(const QString& sender, qint64 nowMs) const
{
    if (m_limits.burst <= 0)
        return false;
    const auto it = m_buckets.constFind(sender);
    if (it != m_buckets.constEnd())
        return tokensAt(it.value(), nowMs) >= 1.0;
    if (m_buckets.size() < m_limits.maxSenders)
        return true; // a new sender starts full
    for (const Bucket& b : m_buckets) {
        if (tokensAt(b, nowMs) >= m_limits.burst)
            return true; // take() forgets a full one to make room
    }
    return false;
}

bool SenderBudget::take(const QString& sender, qint64 nowMs)
{
    if (m_limits.burst <= 0)
        return false;
    if (!m_buckets.contains(sender) && m_buckets.size() >= m_limits.maxSenders) {
        // Bounded: buckets that are full again are forgotten first (they would start full anyway).
        for (auto it = m_buckets.begin(); it != m_buckets.end();) {
            if (tokensAt(it.value(), nowMs) >= m_limits.burst)
                it = m_buckets.erase(it);
            else
                ++it;
        }
        if (m_buckets.size() >= m_limits.maxSenders)
            return false;
    }
    Bucket&      b      = m_buckets[sender];
    const double tokens = tokensAt(b, nowMs);
    if (tokens < 1.0)
        return false;
    b.tokens = tokens - 1.0;
    b.atMs   = nowMs;
    return true;
}

void SenderBudget::clear()
{
    m_buckets.clear();
}

} // namespace autoload
} // namespace linkpreview
