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

constexpr qint64 kKiB = 1024;
constexpr qint64 kMiB = 1024 * 1024;

} // namespace

bool dataSaverOn(const Settings& s, const QString& serverUid)
{
    if (const ServerOverrides* own = s.overridesFor(serverUid)) {
        if (own->dataSaver)
            return *own->dataSaver;
    }
    return s.dataSaver;
}

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

bool worthAsking(Input in, const Target& target)
{
    in.cap       = SenderCap::Yes;
    in.budgetSec = qMax(in.budgetSec, waitSec(target));
    return waitFor(in) == Wait::Sender;
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

int waitSec(const Target& target)
{
    switch (target.cls) {
    case LinkClass::YouTube:
    case LinkClass::Direct:
        return waitSec(target.kind);
    case LinkClass::MediaPage:
        // A Nextcloud share may turn out to be a video (its sender downloads, probes and uploads it, like any link
        // video: design2 2.10/2.11), whatever its expected kind.
        return target.kind == Kind::Video || target.provider == QLatin1String("nextcloud") ? 120 : 60;
    case LinkClass::Site:
    case LinkClass::Web:
        return 40;
    case LinkClass::None:
        break;
    }
    return 0;
}

int maxWaitSec(const Target& target)
{
    const int own = waitSec(target);
    return own > 0 ? kSenderQueueSec + own : 0;
}

const char* capFor(LinkClass cls)
{
    switch (cls) {
    case LinkClass::YouTube:
    case LinkClass::Direct:
        return "l";
    case LinkClass::MediaPage:
    case LinkClass::Site:
        return "lx";
    case LinkClass::Web:
        return "lw";
    case LinkClass::None:
        break;
    }
    return nullptr;
}

int SenderQueue::add(const QString& sender, qint64 timeSec, Kind kind)
{
    return addWait(sender, timeSec, waitSec(kind));
}

int SenderQueue::add(const QString& sender, qint64 timeSec, const Target& target, bool senderMakesIt)
{
    const int own = waitSec(target);
    if (timeSec < 0 || own <= 0 || !senderMakesIt)
        return 0; // a link the sender doesn't make: nothing to wait for, and it isn't in their queue
    // The intake model (2.4.0 open item 2): the sender's plugin takes kSenderIntakePerMinute links a minute; one more
    // within that minute was refused there and never comes. 2.5 senders count the 2.4 classes and the new ones apart
    // (SenderPipeline::Limits), so this does too.
    QVector<qint64>& times = m_intake[target.isLegacy() ? sender : sender + QLatin1String("\x01x")];
    int              recent = 0;
    for (const qint64 t : qAsConst(times))
        recent += t > timeSec - kIntakeWindowSec && t <= timeSec ? 1 : 0;
    if (recent >= kSenderIntakePerMinute)
        return 0;
    times.append(timeSec);
    if (times.size() > 64)
        times.remove(0, times.size() - 64);
    return addWait(sender, timeSec, own);
}

int SenderQueue::addWait(const QString& sender, qint64 timeSec, int own)
{
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
    default:
        break;
    }
    return 0;
}

// ---- 2.5: caps per target ---------------------------------------------------------------------------------------

namespace {

// The automatic media cap of a kind (2.4.0's rules; a loop is a GIF, a card target's media is a picture).
qint64 autoMedia(Kind kind, const Settings& s, const QString& serverUid)
{
    switch (kind) {
    case Kind::YouTube:
        return clickCap(Kind::YouTube);
    case Kind::Video:
        return autoCap(Kind::Video, s, serverUid);
    case Kind::Image:
    case Kind::Gif:
    case Kind::Loop:
    case Kind::Card:
        return autoCap(Kind::Image, s, serverUid);
    case Kind::None:
        break;
    }
    return 0;
}

qint64 clickMedia(Kind kind)
{
    switch (kind) {
    case Kind::YouTube:
        return clickCap(Kind::YouTube);
    case Kind::Video:
        return clickCap(Kind::Video);
    default:
        return clickCap(Kind::Image);
    }
}

} // namespace

// design2 2.11: a Nextcloud share may turn out to be a video whatever its expected kind: its video has a video's caps.
static bool mayBeVideo(const Target& t)
{
    return t.cls == LinkClass::MediaPage && t.provider == QLatin1String("nextcloud");
}

Caps clickCaps(const Target& t)
{
    Caps caps;
    caps.media     = t.cls == LinkClass::None ? 0 : clickMedia(t.kind);
    caps.picture   = 2 * kMiB;
    caps.page      = 512 * kKiB;
    caps.userAsked = true;
    if (mayBeVideo(t))
        caps.video = clickMedia(Kind::Video);
    return caps;
}

Caps autoCaps(const Target& t, const Settings& s, const QString& serverUid)
{
    Caps caps;
    caps.userAsked = false;
    caps.webCards  = s.showWebCards; // a share link that turns out to be another web page: not loaded with it off
    if (t.cls == LinkClass::None || (t.cls == LinkClass::Web && !s.showWebCards)) {
        caps.media   = 0;
        caps.picture = 0;
        caps.page    = 0;
        return caps;
    }
    const bool saver = dataSaverOn(s, serverUid);
    caps.media       = autoMedia(t.kind, s, serverUid);
    if (mayBeVideo(t))
        caps.video = autoMedia(Kind::Video, s, serverUid); // videoAutoDownloadMB as for other link videos
    // A card's picture and its page read: always (a preview, like YouTube's), smaller with data saver.
    caps.picture = saver ? 512 * kKiB : 2 * kMiB;
    caps.page    = saver ? 128 * kKiB : 512 * kKiB;
    return caps;
}

qint64 autoCap(const Target& t, const Settings& s, const QString& serverUid)
{
    const Caps caps = autoCaps(t, s, serverUid);
    return t.kind == Kind::Card ? caps.picture : caps.media;
}

bool isMediaKind(Kind kind)
{
    return kind == Kind::Image || kind == Kind::Gif || kind == Kind::Video || kind == Kind::Loop;
}

qint64 overLimitCap(const Target& t, Kind found, const Settings& s, const QString& serverUid)
{
    if (isMediaKind(found))
        return mediaCapOf(autoCaps(t, s, serverUid), found);
    return autoCap(t, s, serverUid);
}

qint64 mediaCapOf(const Caps& caps, Kind found)
{
    const bool video = found == Kind::Video || found == Kind::Loop;
    return video && caps.video >= 0 ? caps.video : caps.media;
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

bool SenderBudget::has(const QString& sender, qint64 nowMs, int n) const
{
    n = qMax(1, n);
    if (m_limits.burst <= 0 || n > m_limits.burst)
        return false;
    const auto it = m_buckets.constFind(sender);
    if (it != m_buckets.constEnd())
        return tokensAt(it.value(), nowMs) >= n;
    if (m_buckets.size() < m_limits.maxSenders)
        return true; // a new sender starts full
    for (const Bucket& b : m_buckets) {
        if (tokensAt(b, nowMs) >= m_limits.burst)
            return true; // take() forgets a full one to make room
    }
    return false;
}

bool SenderBudget::take(const QString& sender, qint64 nowMs, int n)
{
    n = qMax(1, n);
    if (m_limits.burst <= 0 || n > m_limits.burst)
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
    if (tokens < n)
        return false;
    b.tokens = tokens - n;
    b.atMs   = nowMs;
    return true;
}

void SenderBudget::clear()
{
    m_buckets.clear();
}

} // namespace autoload
} // namespace linkpreview
