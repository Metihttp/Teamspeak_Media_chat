#include "senderpipeline.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>

#include "core.h"
#include "linkstate.h" // loadFollowsWebCards
#include "lphub.h"     // linksCacheDir
#include "lpmessage.h"
#include "pluginlink.h"
#include "previewstore.h"
#include "settings.h"
#include "textreact.h"
#include "ts3api.h"

namespace linkpreview {

namespace {

constexpr int    kTickMs      = 1000;
constexpr qint64 kRateWindowMs = 60000;
constexpr qint64 kGifMediaBytes = 8LL * 1024 * 1024; // a GIF goes up whole only up to this

qint64 wallSec()
{
    return QDateTime::currentSecsSinceEpoch();
}

qint64 megabytes(int mb)
{
    return static_cast<qint64>(qMax(0, mb)) * 1024 * 1024;
}

// path is inside dir (both absolute, after resolving "." and "..").
bool isInside(const QString& path, const QString& dir)
{
    if (path.isEmpty() || dir.isEmpty())
        return false;
    const QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    return p.startsWith(d + QLatin1Char('/'), Qt::CaseInsensitive);
}

} // namespace

SenderPipeline::SenderPipeline(Core* core, PluginLink* link, PreviewStore* store, const textreact::Index* index, FetchPort* fetch, QObject* parent)
    : SenderPipeline(core, link, store, index, fetch, Limits(), parent)
{
}

SenderPipeline::SenderPipeline(Core* core, PluginLink* link, PreviewStore* store, const textreact::Index* index, FetchPort* fetch, const Limits& limits,
                               QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_link(link)
    , m_store(store)
    , m_index(index)
    , m_fetch(fetch)
    , m_limits(limits)
{
    m_pool.setMaxThreadCount(1);
    // A member timer (a child): nothing of ours is pending once the pipeline is gone.
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SenderPipeline::onTimer);
    if (core) {
        // Emitted from inside Core's upload steps: taken up on the next turn of the event loop (a posted
        // event, freed with this object), never here.
        connect(core, &Core::backgroundFinished, this, [this](int batch, int id, bool ok, const MediaLink& uploaded) {
            // 2.5: why it failed, read now (the job is Core's and goes later).
            MediaError failure = MediaError::None;
            if (!ok && m_core) {
                if (const UploadJob* job = m_core->upload(id))
                    failure = job->failure;
            }
            QPointer<SenderPipeline> self(this);
            QMetaObject::invokeMethod(this, [self, batch, ok, uploaded, failure] {
                if (self)
                    self->onUploaded(batch, ok, uploaded, failure);
            }, Qt::QueuedConnection);
        });
    }
}

SenderPipeline::~SenderPipeline()
{
    shutdown();
}

qint64 SenderPipeline::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

bool SenderPipeline::enabled() const
{
    const Settings& s = Settings::instance();
    return s.showLinkPreviews && s.makeLinkPreviews;
}

bool SenderPipeline::inItsChannel(const Job& job) const
{
    return job.scope != proto::Scope::Channel || ts3::ownChannel(job.sch) == job.channelId;
}

void SenderPipeline::setWorkRoot(const QString& dir)
{
    m_root        = dir;
    m_rootCleared = false;
}

void SenderPipeline::setVideoProbe(std::function<LocalMediaInfo(const QString&)> probe)
{
    m_probe = std::move(probe);
}

void SenderPipeline::setCacheDir(const QString& dir)
{
    m_cacheDir = dir;
}

QString SenderPipeline::cacheDir() const
{
    return m_cacheDir.isEmpty() ? linksCacheDir(m_core.data()) : m_cacheDir;
}

// ---- 2.4 auto-load: our own links ---------------------------------------------------------------------

void SenderPipeline::addOwn(const OwnLink& record)
{
    if (m_closed)
        return;
    const qint64 now = nowMs();
    for (int i = m_own.size() - 1; i >= 0; --i) {
        if (now - m_own.at(i).changedMs > kOwnLinkTtlMs || m_own.at(i).changedMs > now)
            m_own.removeAt(i);
    }
    OwnLink r   = record;
    r.changedMs = now;
    m_own.append(r);
    while (m_own.size() > kMaxOwnLinks)
        m_own.removeFirst();
    emit ownLinkChanged(r.target.key);
}

void SenderPipeline::setOwn(int jobId, OwnLink::State state, const Result* result)
{
    if (m_closed || jobId == 0)
        return;
    for (OwnLink& r : m_own) {
        if (r.jobId != jobId)
            continue;
        r.state     = state;
        r.changedMs = nowMs();
        if (result)
            r.result = *result;
        emit ownLinkChanged(r.target.key);
        return;
    }
}

const SenderPipeline::OwnLink* SenderPipeline::ownLink(const QString& hash, qint64 timeSec, const QString& urlKey) const
{
    const OwnLink* best = nullptr;
    for (const OwnLink& r : m_own) {
        const qint64 off = qAbs(r.receivedSec - timeSec);
        if (r.hash != hash || r.target.key != urlKey || off > textreact::kToleranceSec)
            continue;
        if (!best || off <= qAbs(best->receivedSec - timeSec))
            best = &r;
    }
    return best;
}

const SenderPipeline::OwnLink* SenderPipeline::latestOwnLink(const QString& urlKey) const
{
    for (int i = m_own.size() - 1; i >= 0; --i) {
        if (m_own.at(i).target.key == urlKey)
            return &m_own.at(i);
    }
    return nullptr;
}

bool SenderPipeline::fetching(const QString& urlKey) const
{
    for (const OwnLink& r : m_own) {
        if (r.target.key == urlKey && r.state == OwnLink::State::Fetching)
            return true;
    }
    return false;
}

bool SenderPipeline::pending(const QString& urlKey) const
{
    for (const OwnLink& r : m_own) {
        if (r.target.key == urlKey && (r.state == OwnLink::State::Queued || r.state == OwnLink::State::Fetching))
            return true;
    }
    return false;
}

// ---- messages -----------------------------------------------------------------------------------------

void SenderPipeline::onTextMessage(quint64 sch, int targetMode, quint16 toId, quint16 fromId, const QString& fromUid, const QString& bbcode)
{
    if (m_closed || !m_core || !enabled())
        return;
    // The server chat has no LP (plugin commands can't address it): receivers load its links themselves.
    if (targetMode != TextMessageTarget_CHANNEL && targetMode != TextMessageTarget_CLIENT)
        return;
    if (!ts3::isConnected(sch))
        return;
    const QString own = ts3::ownUid(sch);
    if (fromId == 0 || fromId != ts3::ownClientId(sch) || own.isEmpty() || fromUid != own)
        return; // only our own messages (TeamSpeak reports them back to us)
    if (message::isMediaMessage(bbcode))
        return; // TS Media's own media messages have no text identity

    // 2.5: the links every receiver gives a slot (the same text, the same functions); a link to another web page
    // gets no job while cards for other web pages are off (our "lw" is off then: nobody waits for it).
    QVector<Target> targets;
    const bool      webCards = Settings::instance().showWebCards;
    for (const Target& t : selectedLinks(bbcode)) {
        if (t.cls == LinkClass::Web && !webCards)
            continue;
        targets.append(t);
    }
    if (targets.isEmpty())
        return;
    const QString hash = message::identityHash(own, bbcode);
    if (hash.isEmpty())
        return;
    const qint64 receivedSec = wallSec();
    // 2.4 auto-load: our own chat stops waiting for links that get no job (the normal rules then).
    const auto skipAll = [this, &targets, &hash, receivedSec] {
        for (const Target& t : qAsConst(targets)) {
            OwnLink r;
            r.state       = OwnLink::State::Skipped;
            r.hash        = hash;
            r.receivedSec = receivedSec;
            r.target      = t;
            addOwn(r);
        }
    };
    if (m_link && m_link->blocked(sch)) {
        ++m_counters.skipped;
        ts3::log(LogLevel_INFO, sch, "No link preview: this server doesn't pass on plugin commands", {});
        skipAll();
        return;
    }

    Job base;
    base.sch         = sch;
    base.serverUid   = ts3::serverUid(sch);
    base.ownUid      = own;
    base.hash        = hash;
    base.receivedSec = receivedSec;
    base.queuedMs    = nowMs();
    if (targetMode == TextMessageTarget_CHANNEL) {
        base.scope     = proto::Scope::Channel;
        base.channelId = ts3::ownChannel(sch);
        if (base.channelId == 0) {
            skipAll();
            return;
        }
    } else {
        base.scope      = proto::Scope::Private;
        base.partnerId  = toId;
        base.partnerUid = ts3::clientUid(sch, toId);
        if (base.partnerUid.isEmpty() || base.partnerUid == own) {
            skipAll();
            return;
        }
    }
    QStringList refused; // 2.5: named in one intake notice
    for (const Target& t : qAsConst(targets)) {
        OwnLink record;
        record.hash        = hash;
        record.receivedSec = receivedSec;
        record.target      = t;
        // Each family its own allowance and queue (Limits): a page link never takes a 2.4 link's place.
        if (queuedOf(t.isLegacy()) >= m_limits.maxQueued || !takeRateToken(sch, base.queuedMs, t.isLegacy())) {
            ++m_counters.skipped;
            ts3::log(LogLevel_INFO, sch, "No link preview for %1 (%2): too many links at once", {ts3::name(t.host), ts3::pub(QString::fromLatin1(kindName(t.kind)))});
            record.state = OwnLink::State::Skipped; // 2.5: one intake notice for all of them (below), never one each
            addOwn(record);
            refused << t.key;
            continue;
        }
        Job job    = base;
        job.id     = ++m_nextId;
        job.target = t;
        m_jobs.append(job);
        record.jobId = job.id;
        record.state = OwnLink::State::Queued;
        addOwn(record);
    }
    if (!refused.isEmpty())
        sendIntakeNotice(base, refused);
    startNext();
}

QVector<Target> SenderPipeline::selectedLinks(const QString& bbcode)
{
    QVector<Target> found;
    QSet<QString>   keys;
    for (const QString& href : message::linkHrefs(bbcode, kMaxScannedLinks)) {
        const Target t = classify(trimLinkEnd(href));
        if (!t.isValid() || t.key.isEmpty() || keys.contains(t.key))
            continue;
        keys.insert(t.key);
        found.append(t);
    }
    return selectLinks(found);
}

void SenderPipeline::sendIntakeNotice(const Job& base, const QStringList& refusedKeys)
{
    // 2.4.0 open item 2: receivers waited ~150 s for links our plugin refused at intake. One LPXNO for the message's
    // refused links (2.4.0 ignores it, 2.5 stops waiting), at most one per connection every intakeNoticeMinMs: a
    // burst never costs more commands than it saves (receivers model the rest: autoload::SenderQueue).
    const qint64 now  = nowMs();
    const auto   last = m_lastIntakeNotice.constFind(base.sch);
    if (last != m_lastIntakeNotice.constEnd() && now - last.value() >= 0 && now - last.value() < m_limits.intakeNoticeMinMs)
        return;
    peers::Target target;
    if (m_closed || !enabled() || !m_link || m_link->blocked(base.sch) || !targetOf(base, &target))
        return;
    const qint64         age = qMax<qint64>(0, wallSec() - base.receivedSec);
    const proto::Message msg = makeLpxSkip(base.scope, textreact::wireRef(base.hash, age), refusedKeys.mid(0, kMaxSkipKeys));
    if (msg.type.isEmpty())
        return;
    const quint64 sch = base.sch;
    m_link->sendWith(sch, msg, target, PluginLink::Priority::Background, [sch](peers::SendResult r) {
        if (r != peers::SendResult::Ok && r != peers::SendResult::LateOk)
            ts3::log(LogLevel_INFO, sch, "A link preview's intake notice couldn't be sent", {});
    });
    m_lastIntakeNotice.insert(base.sch, now);
    ++m_counters.intakeNotices;
}

int SenderPipeline::queuedOf(bool legacy) const
{
    int n = 0;
    for (const Job& job : m_jobs)
        n += job.target.isLegacy() == legacy ? 1 : 0;
    return n;
}

bool SenderPipeline::takeRateToken(quint64 sch, qint64 now, bool legacy)
{
    QVector<qint64>& starts = legacy ? m_starts[sch] : m_startsX[sch];
    starts.erase(std::remove_if(starts.begin(), starts.end(), [now](qint64 at) { return now - at >= kRateWindowMs || at > now; }), starts.end());
    if (starts.size() >= m_limits.maxJobsPerMinute)
        return false;
    starts.append(now);
    return true;
}

// ---- the steps ----------------------------------------------------------------------------------------

SenderPipeline::Job* SenderPipeline::find(int id)
{
    for (Job& job : m_jobs) {
        if (job.id == id)
            return &job;
    }
    return nullptr;
}

SenderPipeline::Job* SenderPipeline::running()
{
    return !m_jobs.isEmpty() && m_jobs.first().step != Step::Queued ? &m_jobs.first() : nullptr;
}

void SenderPipeline::startNext()
{
    while (!m_closed && !m_jobs.isEmpty() && m_jobs.first().step == Step::Queued) {
        Job&         job = m_jobs.first();
        const qint64 now = nowMs();
        if (!enabled() || !ts3::isConnected(job.sch) || ts3::serverUid(job.sch) != job.serverUid || now - job.queuedMs > m_limits.queuedTtlMs) {
            finish(job.id, false, QStringLiteral("dropped before it started"));
            continue;
        }
        if (!inItsChannel(job)) {
            finish(job.id, false, QStringLiteral("you left the channel"));
            continue;
        }
        job.startedMs = now;
        ++m_counters.started;
        // The same link in the same channel a little earlier: its files are on the server already.
        const auto reuse = m_reuse.constFind(reuseKey(job.serverUid, ts3::ownChannel(job.sch), job.target.key));
        if (reuse != m_reuse.constEnd() && now - reuse->atMs >= 0 && now - reuse->atMs <= m_limits.reuseMs) {
            ++m_counters.reused;
            job.step = Step::Announcing;
            setOwn(job.id, OwnLink::State::Reused); // our chat: the store gets our LP now
            announce(job, reuse->files);            // finishes the job
            continue;
        }
        startFetch(job);
        break;
    }
    schedule();
}

void SenderPipeline::startFetch(Job& job)
{
    if (!m_fetch) {
        finish(job.id, false, QStringLiteral("no fetcher"));
        return;
    }
    if (m_root.isEmpty())
        m_root = ts3::dataDir() + QStringLiteral("/linkpreview/send");
    if (!m_rootCleared) {
        QDir(m_root).removeRecursively(); // what a crashed session left
        m_rootCleared = true;
    }
    job.dir = m_root + QLatin1Char('/') + QString::number(job.id);
    QDir(job.dir).removeRecursively();
    // 2.4 auto-load: the website's files go to the chat's links folder (our own chat shows them at once, and a link
    // loaded before costs no request); only what is made from them is ours to work in.
    const QString dir = cacheDir();
    if (dir.isEmpty() || !QDir().mkpath(dir) || !QDir().mkpath(job.dir + QStringLiteral("/out"))) {
        finish(job.id, false, QStringLiteral("no work folder"));
        return;
    }
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    job.step   = Step::Fetching;
    ts3::log(LogLevel_INFO, job.sch, "Making a link preview (%1, %2)", {ts3::name(job.target.host), ts3::pub(QString::fromLatin1(kindName(job.target.kind)))});
    const Target target = job.target;
    const int    id     = job.id;
    const Caps   caps   = fetchCapsFor(target);
    job.webCards        = caps.webCards;
    setOwn(id, OwnLink::State::Fetching);
    m_fetch->load(target, caps, dir); // may answer at once (fetched)
}

Caps SenderPipeline::fetchCapsFor(const Target& target) const
{
    Caps caps;
    caps.page      = net::kMaxPageBytes; // the sender ignores data saver: it makes the preview for everyone
    caps.picture   = qMin<qint64>(net::kMaxThumbnailBytes, 2LL * 1024 * 1024);
    caps.userAsked = false;
    // A share link always gets a job (design2 2.10), but one that turns out to be another web page is no card of ours
    // while cards for other web pages are off: that page is never loaded (kWebOff).
    caps.webCards = Settings::instance().showWebCards;
    switch (target.cls) {
    case LinkClass::YouTube:
        caps.media = m_limits.thumbnailBytes;
        break;
    case LinkClass::Direct:
    case LinkClass::MediaPage:
        caps.media = target.kind == Kind::Video ? m_limits.videoBytes : m_limits.pictureBytes;
        if (target.provider == QLatin1String("nextcloud"))
            caps.video = m_limits.videoBytes; // a share may turn out to be a video (design2 2.10)
        break;
    case LinkClass::Site:
        caps.media = m_limits.pictureBytes; // a Reddit post or a Steam screenshot may turn out to be a picture
        break;
    case LinkClass::Web:
        caps.media = m_limits.videoBytes; // a page that answers with media (the maker then keeps its own caps)
        break;
    case LinkClass::None:
        break;
    }
    return caps;
}

maker::Caps SenderPipeline::makerCapsFor(const QString& serverUid) const
{
    const Settings s      = Settings::instance().forServer(serverUid);
    const qint64   upload = megabytes(s.uploadMaxMB);
    const qint64   autoMb = megabytes(s.autoDownloadMaxMB);
    maker::Caps    caps;
    caps.imageMediaBytes = qMin(autoMb, upload);
    caps.gifMediaBytes   = qMin(kGifMediaBytes, qMin(autoMb, upload));
    caps.videoMediaBytes = qMin(m_limits.videoBytes, upload);
    return caps;
}

void SenderPipeline::fetched(const QString& key, const Result& result)
{
    Job* job = running();
    if (m_closed || !job || job->step != Step::Fetching || job->target.key != key)
        return; // someone else's fetch, or one we gave up on
    if (!result.ok && result.error == QLatin1String(errors::kCanceled))
        return; // the answer to a cancel of ours: a fetch started again (followWebCards) answers for itself
    const bool page = job->target.isNewClass();
    if (!result.ok) {
        setOwn(job->id, OwnLink::State::Failed, &result); // our chat: the neutral error with Retry (or its size)
        // The log never gets an address (a query may hold anything): whatever looks like one goes.
        static const QRegularExpression address(QStringLiteral("\\S+://\\S*|\\S+\\?\\S+"));
        QString                         why = result.error.left(200);
        why.replace(address, QStringLiteral("(link)"));
        // 2.5: the page has nothing to show (no card) or the link is expired: facts about the content, so receivers
        // needn't try either (r=n). A wall (a consent page, a bot check) often depends on the sender (their region,
        // their address): a plain LPXNO, receivers may still load it (their own walled memo bounds the requests).
        const bool nothing = result.error == QLatin1String(errors::kNothing) || result.error == QLatin1String(errors::kExpired);
        if (page && result.error == QLatin1String(errors::kWebOff)) {
            // A share link that went to another website while our cards for other web pages are off: like a Web card
            // from another website (D18), receivers don't load it by themselves (r=x). One that is another page of the
            // same website (or not proven a share): a plain LPXNO, receivers follow their own switch.
            finish(job->id, false, QStringLiteral("cards for other web pages are off"),
                   result.finalHost.isEmpty() ? SkipReason::Unknown : SkipReason::CrossSite);
            return;
        }
        finish(job->id, false, QStringLiteral("the website didn't give it: ") + why, page && nothing ? SkipReason::Nothing : SkipReason::Unknown);
        return;
    }
    if (!enabled()) {
        finish(job->id, false, QStringLiteral("switched off"));
        return;
    }
    // 2.5: a page link is what its load turned out to be; a card's file is its picture (none: a text card).
    const bool    card = page && result.kind == Kind::Card;
    const QString file = job->target.kind == Kind::YouTube ? result.thumbnailFile : card ? result.card.pictureFile : result.localFile;
    if (card ? result.card.title.isEmpty() : (file.isEmpty() || !QFileInfo(file).isFile())) {
        finish(job->id, false, QStringLiteral("nothing was downloaded"));
        return;
    }
    // Our own chat shows the website's files now, before anything is made or uploaded.
    setOwn(job->id, OwnLink::State::Fetched, &result);
    job = running(); // the signal is queued, but stay safe
    if (!job || job->step != Step::Fetching || job->target.key != key)
        return;
    // 2.5 (D18): a web page that went to another website gets no card from us: receivers can't check where it went,
    // and they don't load it by themselves either (r=x). Our own chat shows it (with the host it went to).
    if (card && !result.finalHost.isEmpty()) {
        finish(job->id, false, QStringLiteral("the page went to another website"), SkipReason::CrossSite);
        return;
    }
    maker::Input in;
    in.target     = job->target;
    in.fetched    = result;
    in.workDir    = job->dir + QStringLiteral("/out");
    in.mayMove    = !file.isEmpty() && isInside(file, job->dir); // 2.4: false for the links folder (copied: the chat shows that file)
    in.caps       = makerCapsFor(job->serverUid);
    in.probeVideo = m_probe;
    job->step     = Step::Making;
    const int                id     = job->id;
    const auto               cancel = job->cancel;
    QPointer<SenderPipeline> self(this);
    // shutdown() joins this pool before the pipeline goes, so self is valid while the worker runs.
    m_pool.start([self, id, in, cancel] {
        const maker::Made made = maker::make(in, cancel.get());
        if (SenderPipeline* pipeline = self.data()) {
            QMetaObject::invokeMethod(pipeline, [self, id, made] {
                if (self)
                    self->onMade(id, made);
            }, Qt::QueuedConnection);
        }
    });
}

void SenderPipeline::onMade(int id, const maker::Made& made)
{
    Job* job = find(id);
    if (m_closed || !job || job->step != Step::Making)
        return;
    // Its worker is done: what it made goes with the job's folder (finish() keeps a folder only while Making).
    const auto fail = [this, job, id](const QString& why) {
        job->step = Step::Queued;
        finish(id, false, why);
    };
    if (!made.ok) {
        fail(QStringLiteral("no preview: ") + made.error);
        return;
    }
    if (!enabled() || !m_core || !ts3::isConnected(job->sch) || ts3::serverUid(job->sch) != job->serverUid) {
        fail(QStringLiteral("no longer possible"));
        return;
    }
    // Core uploads into the channel you are in now: a channel message's files only into its own channel (the
    // only one its LP goes to), never into another channel's file browser.
    if (!inItsChannel(*job)) {
        fail(QStringLiteral("you left the channel"));
        return;
    }
    job->made = made;
    // 2.5: a card without a picture has nothing to upload: its text goes out at once.
    if (made.kind == Kind::Card && made.previewFile.isEmpty()) {
        announceTextCard(*job);
        return;
    }
    // 2.5: the channel we'd upload into refused our last preview upload (no permission there, a quota, a password):
    // no new request to the server for a few minutes. A card with its text goes without its picture; media and a
    // card that is only a picture end as if the upload had failed (LPXNO / LPNO).
    job->uploadChannel = ts3::ownChannel(job->sch);
    if (uploadsRefused(job->serverUid, job->uploadChannel)) {
        ++m_counters.uploadsSkipped;
        if (job->target.isNewClass() && made.kind == Kind::Card && !made.card.description.isEmpty()) {
            announceTextCard(*job);
            return;
        }
        fail(QStringLiteral("this channel refused our last upload"));
        return;
    }

    SendRequest request;
    request.background       = true;
    request.target.sch       = job->sch;
    request.target.mode      = job->scope == proto::Scope::Private ? TextMessageTarget_CLIENT : TextMessageTarget_CHANNEL;
    request.target.clientId  = job->partnerId;
    request.target.serverUid = job->serverUid;
    request.target.clientUid = job->partnerUid;
    SendItem item;
    item.ownTemp = true; // Core takes our files over (and removes them with its job)
    item.quality = SendQuality::Original;
    if (!made.mediaFile.isEmpty()) {
        item.path        = made.mediaFile;
        item.previewJpeg = made.previewBytes;
    } else {
        item.path        = made.previewFile;
        item.previewOnly = true;
    }
    request.items.append(item);
    job->step        = Step::Uploading;
    const int batch  = m_core->send(request); // Core's signals answer on a later turn (see the constructor)
    if (Job* again = find(id))
        again->batch = batch;
    if (batch == 0)
        finish(id, false, QStringLiteral("the upload didn't start"));
    schedule();
}

void SenderPipeline::announceTextCard(Job& job)
{
    // A card's text without its picture (none, or it can't go up): it needs its text (makeCard / the callers).
    Announcement a;
    a.extended    = true;
    a.urlKey      = job.target.key;
    a.kind        = Kind::Card;
    a.serverUid   = job.serverUid;
    a.card        = job.made.card;
    a.card.layout = CardLayout::None;
    a.card.play   = false;
    a.sensitive   = job.made.sensitive;
    a.durationMs  = 0;
    job.step      = Step::Announcing;
    announce(job, a);
}

bool SenderPipeline::uploadsRefused(const QString& serverUid, quint64 channelId)
{
    const QString key = serverUid + QLatin1Char('|') + QString::number(channelId);
    const auto    it  = m_refusedUploads.find(key);
    if (it == m_refusedUploads.end())
        return false;
    const qint64 now = nowMs();
    if (now - it.value() >= 0 && now - it.value() < m_limits.refusedUploadMs)
        return true;
    m_refusedUploads.erase(it);
    return false;
}

void SenderPipeline::onUploaded(int batch, bool ok, const MediaLink& uploaded, MediaError failure)
{
    Job* job = running();
    if (m_closed || !job || job->step != Step::Uploading || job->batch != batch || batch == 0)
        return;
    const maker::Made& made = job->made;
    const bool         page = job->target.isNewClass();
    if (!ok) {
        // 2.5: a channel that refuses our uploads (no permission there, a quota, a channel password) is remembered for
        // a few minutes: the next links of this channel make no upload request that would fail again.
        if (failure == MediaError::Permission || failure == MediaError::Quota || failure == MediaError::Password) {
            if (m_refusedUploads.size() >= 64)
                m_refusedUploads.clear();
            m_refusedUploads.insert(job->serverUid + QLatin1Char('|') + QString::number(job->uploadChannel), nowMs());
        }
        // 2.5: a card with its text goes without its picture (no permission, a quota, the server's check); media and
        // a card that is only a picture end with LPXNO.
        if (page && made.kind == Kind::Card && !made.card.description.isEmpty()) {
            announceTextCard(*job);
            return;
        }
        finish(job->id, false, QStringLiteral("the upload failed"));
        return;
    }
    Announcement       a;
    a.extended  = page;
    a.urlKey    = job->target.key;
    a.kind      = made.kind;
    a.serverUid = uploaded.serverUid;
    a.channelId = uploaded.channelId;
    if (!made.mediaFile.isEmpty()) {
        a.previewPath = uploaded.previewFile;
        a.previewSha  = shaText(made.previewSha);
        a.previewSize = made.previewBytes.size();
        a.mediaPath   = uploaded.remoteFile();
        a.mediaSha    = shaText(uploaded.sha256);
        a.mediaSize   = static_cast<qint64>(uploaded.size);
        // What Core uploaded as the preview is what was made here (its "ph" is the start of our sha).
        if (a.previewPath.isEmpty() || (!uploaded.previewSha.isEmpty() && uploaded.previewSha != made.previewSha.left(uploaded.previewSha.size()))) {
            finish(job->id, false, QStringLiteral("the preview didn't go up"));
            return;
        }
    } else {
        a.previewPath = uploaded.remoteFile();
        a.previewSha  = shaText(uploaded.sha256);
        a.previewSize = static_cast<qint64>(uploaded.size);
        if (uploaded.sha256 != made.previewSha) {
            finish(job->id, false, QStringLiteral("the preview on the server isn't the one made"));
            return;
        }
    }
    a.width      = made.width;
    a.height     = made.height;
    a.durationMs = made.kind == Kind::Video || made.kind == Kind::Loop || made.kind == Kind::Card ? made.durationMs : 0;
    if (page) {
        // 2.5: an LPX: its text is the card's (YouTube's title and author stay LP's).
        a.card      = made.card;
        a.sensitive = made.sensitive;
        if (made.kind != Kind::Card)
            a.card = CardData();
    } else {
        a.title  = made.title;
        a.author = made.author;
    }
    job->step    = Step::Announcing;
    announce(*job, a);
}

QString SenderPipeline::ownLocalKey(const Job& job) const
{
    // Our own chat holds the message (ChatTextReactions indexed it): its exact key. Else the time we saw it.
    if (m_index) {
        textreact::Index::Query q;
        q.serverUid    = job.serverUid;
        q.scope        = job.scope;
        q.ownUid       = job.ownUid;
        q.hash         = job.hash;
        q.estimatedSec = job.receivedSec;
        if (job.scope == proto::Scope::Channel) {
            q.reactorUid = job.ownUid;
            q.senderOnly = true;
        } else {
            q.reactorUid = job.partnerUid; // ours, in the private chat known to be with them
        }
        const QString key = m_index->resolve(q);
        if (!key.isEmpty())
            return key;
    }
    return textreact::localKey(job.hash, job.receivedSec);
}

bool SenderPipeline::targetOf(const Job& job, peers::Target* target, QString* why) const
{
    const auto no = [why](const char* text) {
        if (why)
            *why = QString::fromLatin1(text);
        return false;
    };
    if (!m_link || !ts3::isConnected(job.sch) || ts3::serverUid(job.sch) != job.serverUid)
        return no("not connected");
    if (job.scope == proto::Scope::Channel) {
        if (ts3::ownChannel(job.sch) != job.channelId)
            return no("you left the channel");
        *target = peers::Target::toChannel();
        return true;
    }
    quint16 partner = job.partnerId;
    if (ts3::clientUid(job.sch, partner) != job.partnerUid)
        partner = ts3::clientIdByUid(job.sch, job.partnerUid);
    if (partner == 0)
        return no("the partner left");
    *target = peers::Target::toClients({partner});
    return true;
}

bool SenderPipeline::canNotify(const Job& job) const
{
    // Every job in m_jobs was accepted at intake (rate, queue). No notice at unload, after a setting went off (the
    // caps re-announce tells the channel), after leaving the channel or when the partner left.
    peers::Target target;
    return !m_closed && enabled() && m_link && !m_link->blocked(job.sch) && targetOf(job, &target);
}

QString SenderPipeline::reuseKey(const QString& serverUid, quint64 channelId, const QString& urlKey) const
{
    return serverUid + QLatin1Char('|') + QString::number(channelId) + QLatin1Char('|') + urlKey;
}

void SenderPipeline::announce(Job& job, Announcement a)
{
    const int id = job.id;
    // Only where the message went: our channel (if we are still in it), or the partner (found by identity).
    peers::Target target;
    QString       why;
    if (!targetOf(job, &target, &why)) {
        finish(id, false, why);
        return;
    }
    a.messageKey  = QString();
    a.sch         = job.sch;
    a.senderUid   = job.ownUid;
    a.privateChat = job.scope == proto::Scope::Private;
    const qint64 age = qMax<qint64>(0, wallSec() - job.receivedSec);
    // 2.5: the new classes as LPX, within this connection's cap (900 bytes until live check L1; shortened to fit).
    int cap = 0;
    if (a.extended) {
        cap                    = lpxSendCap(m_core && m_core->serverIsTeamSpeakOwn(job.sch));
        m_counters.lpxCapUsed  = cap;
    }
    const proto::Message message = a.extended ? makeLpx(a, job.scope, textreact::wireRef(job.hash, age), cap)
                                              : makeMessage(a, job.scope, textreact::wireRef(job.hash, age));
    if (message.type.isEmpty()) {
        finish(id, false, QStringLiteral("the announcement would break the rules or doesn't fit"));
        return;
    }
    if (a.extended) {
        // Our own copy says what went out (the text shortened to fit, the colour a card may wear).
        if (const std::optional<Incoming> sent = readLpx(message)) {
            const Announcement& b = sent->announcement;
            a.card                = b.card;
        }
    }
    // Our own chat shows it at once (Core's cache already has the files).
    Announcement own = a;
    own.messageKey   = ownLocalKey(job);
    if (m_store)
        m_store->add(own, wallSec());
    // The same link again in this channel soon: these files are announced once more.
    Reuse reuse;
    reuse.files = a;
    reuse.atMs  = nowMs();
    m_reuse.insert(reuseKey(a.serverUid, a.channelId, a.urlKey), reuse);
    while (m_reuse.size() > m_limits.maxReuse) {
        auto oldest = m_reuse.begin();
        for (auto it = m_reuse.begin(); it != m_reuse.end(); ++it) {
            if (it->atMs < oldest->atMs)
                oldest = it;
        }
        m_reuse.erase(oldest);
    }
    const quint64 sch  = job.sch;
    const QString host = job.target.host;
    // 2.5: an LPX whose command failed (dropped by the transport) is followed by an LPXNO, so nobody waits for it.
    proto::Message skip;
    if (a.extended)
        skip = makeLpxSkip(job.scope, textreact::wireRef(job.hash, age), {a.urlKey});
    const QPointer<SenderPipeline> self(this);
    m_link->sendWith(sch, message, target, PluginLink::Priority::Background, [self, sch, host, skip, target](peers::SendResult result) {
        if (result == peers::SendResult::Ok || result == peers::SendResult::LateOk)
            return;
        ts3::log(LogLevel_INFO, sch, "A link preview (%1) couldn't be announced: the plugin command wasn't sent", {ts3::name(host)});
        if (result == peers::SendResult::Failed && self && !self->m_closed && self->m_link && !skip.type.isEmpty()) {
            self->m_link->sendWith(sch, skip, target, PluginLink::Priority::Background, nullptr);
            ++self->m_counters.lpxSkips;
        }
    });
    ++m_counters.announced;
    if (a.extended)
        ++m_counters.lpxSent;
    finish(id, true);
}

void SenderPipeline::finish(int id, bool announced, const QString& why, SkipReason reason)
{
    int at = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).id == id)
            at = i;
    }
    if (at < 0)
        return;
    const Job job = m_jobs.takeAt(at);
    if (job.cancel)
        job.cancel->store(true);
    // 2.4 auto-load: a record still waiting (settings off, shutdown, dropped, no work folder): our chat stops
    // waiting (the normal rules). Fetched / Failed / Reused stay as they are.
    for (const OwnLink& r : qAsConst(m_own)) {
        if (r.jobId == id && (r.state == OwnLink::State::Queued || r.state == OwnLink::State::Fetching)) {
            setOwn(id, OwnLink::State::Skipped);
            break;
        }
    }
    // 2.4 auto-load: the people waiting for our preview load it themselves (one LPNO instead of the LP: the same
    // command budget).
    if (!announced && canNotify(job)) {
        const qint64 age = qMax<qint64>(0, wallSec() - job.receivedSec);
        // 2.4 classes: LPNO (2.4.0 understands it); 2.5 classes: LPXNO with its reason.
        const bool           page = job.target.isNewClass();
        const proto::Message msg  = page ? makeLpxSkip(job.scope, textreact::wireRef(job.hash, age), {job.target.key}, reason)
                                         : makeSkip(job.scope, textreact::wireRef(job.hash, age), job.target.key);
        peers::Target        target;
        if (!msg.type.isEmpty() && targetOf(job, &target)) {
            const quint64 sch  = job.sch;
            const QString host = job.target.host;
            m_link->sendWith(sch, msg, target, PluginLink::Priority::Background, [sch, host](peers::SendResult r) {
                if (r != peers::SendResult::Ok && r != peers::SendResult::LateOk)
                    ts3::log(LogLevel_INFO, sch, "A link preview's notice (%1) couldn't be sent", {ts3::name(host)});
            });
            if (page) {
                ++m_counters.lpxSkips;
                m_counters.lpxSkipsNothing += reason == SkipReason::Nothing ? 1 : 0;
                m_counters.lpxSkipsCrossSite += reason == SkipReason::CrossSite ? 1 : 0;
            } else {
                ++m_counters.skipNotices;
            }
        }
    }
    if (announced) {
        ts3::log(LogLevel_INFO, job.sch, "Link preview announced (%1, %2)", {ts3::name(job.target.host), ts3::pub(QString::fromLatin1(kindName(job.made.kind)))});
    } else {
        ++m_counters.failed;
        ts3::log(LogLevel_INFO, job.sch, "No link preview for %1 (%2): %3",
                 {ts3::name(job.target.host), ts3::pub(QString::fromLatin1(kindName(job.target.kind))), ts3::pub(why)});
    }
    // The worker may still hold a job given up on while it made the files: its folder goes with the next
    // session's clearing then.
    if (!job.dir.isEmpty() && job.step != Step::Making)
        QDir(job.dir).removeRecursively();
    emit jobFinished(job.target.key, announced);
    if (!m_closed) {
        QPointer<SenderPipeline> self(this);
        QMetaObject::invokeMethod(this, [self] {
            if (self)
                self->startNext();
        }, Qt::QueuedConnection);
    }
}

void SenderPipeline::cancelJob(Job& job)
{
    if (job.cancel)
        job.cancel->store(true);
    switch (job.step) {
    case Step::Fetching:
        if (m_fetch)
            m_fetch->cancel(job.target.key);
        break;
    case Step::Uploading:
        if (m_core && job.batch != 0) {
            for (const int upload : m_core->uploadIds()) {
                const UploadJob* u = m_core->upload(upload);
                if (u && u->background && u->batch == job.batch && m_core->canCancelUpload(upload))
                    m_core->cancelUpload(upload);
            }
        }
        break;
    default:
        break;
    }
}

void SenderPipeline::schedule()
{
    if (m_closed || m_jobs.isEmpty())
        m_timer->stop();
    else if (!m_timer->isActive())
        m_timer->start(kTickMs);
}

void SenderPipeline::onTimer()
{
    if (m_closed)
        return;
    const qint64 now = nowMs();
    if (Job* job = running()) {
        const qint64 ran     = now - job->startedMs;
        const bool   fetchTO = job->step == Step::Fetching && ran > m_limits.fetchTimeoutMs;
        if (fetchTO || ran > m_limits.jobTimeoutMs) {
            const int id = job->id;
            if (fetchTO) {
                Result timedOut;
                timedOut.error = QString::fromLatin1(errors::kTimeout);
                setOwn(id, OwnLink::State::Failed, &timedOut);
            }
            cancelJob(*job);
            finish(id, false, fetchTO ? QStringLiteral("the website took too long") : QStringLiteral("it took too long"));
        }
    }
    // Queued jobs past their time go when they come up (startNext); the queue is short.
    schedule();
}

void SenderPipeline::shutdown()
{
    if (m_closed)
        return;
    m_closed = true;
    m_timer->stop();
    for (Job& job : m_jobs)
        cancelJob(job);
    // The worker making a preview stops at its next check; joined here, before anything it uses goes.
    m_pool.clear();
    m_pool.waitForDone();
    while (!m_jobs.isEmpty()) {
        Job& job = m_jobs.first();
        job.step = Step::Queued; // its worker is done: its folder may go
        finish(job.id, false, QStringLiteral("stopped"));
    }
    m_own.clear(); // no signal: nobody listens any more
}

void SenderPipeline::followWebCards()
{
    // Review fix: Show cards for other web pages as it is now. Off: a Web link's job that hasn't loaded its page yet
    // stops (that page is never loaded for our own message; its LPXNO lets receivers follow their own switch).
    const bool   web = Settings::instance().showWebCards;
    QVector<int> stop;
    for (const Job& job : qAsConst(m_jobs)) {
        if (!web && job.target.cls == LinkClass::Web && (job.step == Step::Queued || job.step == Step::Fetching))
            stop << job.id;
    }
    for (const int id : qAsConst(stop)) {
        if (Job* job = find(id)) {
            cancelJob(*job);
            finish(id, false, QStringLiteral("cards for other web pages are off"));
        }
    }
    // A share link's running fetch holds the switch as it started (Caps::webCards: another web page it turns out to be
    // is loaded with it on, not with it off): it starts again under the switch as it is now.
    Job* job = running();
    if (!m_fetch || !job || job->step != Step::Fetching || job->webCards == web || !linkstate::loadFollowsWebCards(job->target))
        return;
    ts3::log(LogLevel_INFO, job->sch, "Making a link preview again (%1): cards for other web pages %2",
             {ts3::name(job->target.host), ts3::pub(web ? QStringLiteral("on") : QStringLiteral("off"))});
    m_fetch->cancel(job->target.key);
    const Target target = job->target;
    const Caps   caps   = fetchCapsFor(target);
    job->webCards       = caps.webCards;
    m_fetch->load(target, caps, cacheDir()); // may answer at once (fetched)
}

void SenderPipeline::applySettings()
{
    if (m_closed)
        return;
    if (enabled()) {
        followWebCards();
        return;
    }
    // Switched off: every job stops (fetch canceled, worker joined, upload canceled); new ones start once on.
    for (Job& job : m_jobs)
        cancelJob(job);
    m_pool.clear();
    m_pool.waitForDone();
    while (!m_jobs.isEmpty()) {
        Job& job = m_jobs.first();
        job.step = Step::Queued;
        finish(job.id, false, QStringLiteral("switched off"));
    }
    m_timer->stop();
}

} // namespace linkpreview
