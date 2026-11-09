#include "senderpipeline.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

#include "core.h"
#include "lphub.h" // linksCacheDir
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
            Q_UNUSED(id);
            QPointer<SenderPipeline> self(this);
            QMetaObject::invokeMethod(this, [self, batch, ok, uploaded] {
                if (self)
                    self->onUploaded(batch, ok, uploaded);
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

    QVector<Target> targets;
    for (const QUrl& url : message::webLinks(bbcode)) {
        const Target t = classify(url);
        if (t.kind == Kind::None || t.key.isEmpty())
            continue;
        bool seen = false;
        for (const Target& other : qAsConst(targets))
            seen = seen || other.key == t.key;
        if (!seen)
            targets.append(t);
        if (targets.size() >= message::kMaxLinks)
            break;
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
    for (const Target& t : qAsConst(targets)) {
        OwnLink record;
        record.hash        = hash;
        record.receivedSec = receivedSec;
        record.target      = t;
        if (m_jobs.size() >= m_limits.maxQueued || !takeRateToken(sch, base.queuedMs)) {
            ++m_counters.skipped;
            ts3::log(LogLevel_INFO, sch, "No link preview for %1 (%2): too many links at once", {ts3::name(t.host), ts3::pub(QString::fromLatin1(kindName(t.kind)))});
            record.state = OwnLink::State::Skipped; // no notice either: that would exceed the sender's budget
            addOwn(record);
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
    startNext();
}

bool SenderPipeline::takeRateToken(quint64 sch, qint64 now)
{
    QVector<qint64>& starts = m_starts[sch];
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
    setOwn(id, OwnLink::State::Fetching);
    m_fetch->load(target, fetchBytesFor(target.kind), dir); // may answer at once (fetched)
}

qint64 SenderPipeline::fetchBytesFor(Kind kind) const
{
    switch (kind) {
    case Kind::YouTube:
        return m_limits.thumbnailBytes;
    case Kind::Video:
        return m_limits.videoBytes;
    default:
        return m_limits.pictureBytes;
    }
}

maker::Caps SenderPipeline::capsFor(const QString& serverUid) const
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
    if (!result.ok) {
        setOwn(job->id, OwnLink::State::Failed, &result); // our chat: the neutral error with Retry (or its size)
        // The log never gets an address (a query may hold anything): whatever looks like one goes.
        static const QRegularExpression address(QStringLiteral("\\S+://\\S*|\\S+\\?\\S+"));
        QString                         why = result.error.left(200);
        why.replace(address, QStringLiteral("(link)"));
        finish(job->id, false, QStringLiteral("the website didn't give it: ") + why);
        return;
    }
    if (!enabled()) {
        finish(job->id, false, QStringLiteral("switched off"));
        return;
    }
    const QString file = job->target.kind == Kind::YouTube ? result.thumbnailFile : result.localFile;
    if (file.isEmpty() || !QFileInfo(file).isFile()) {
        finish(job->id, false, QStringLiteral("nothing was downloaded"));
        return;
    }
    // Our own chat shows the website's files now, before anything is made or uploaded.
    setOwn(job->id, OwnLink::State::Fetched, &result);
    job = running(); // the signal is queued, but stay safe
    if (!job || job->step != Step::Fetching || job->target.key != key)
        return;
    maker::Input in;
    in.target     = job->target;
    in.fetched    = result;
    in.workDir    = job->dir + QStringLiteral("/out");
    in.mayMove    = isInside(file, job->dir); // 2.4: false for the links folder (copied: the chat shows that file)
    in.caps       = capsFor(job->serverUid);
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

void SenderPipeline::onUploaded(int batch, bool ok, const MediaLink& uploaded)
{
    Job* job = running();
    if (m_closed || !job || job->step != Step::Uploading || job->batch != batch || batch == 0)
        return;
    if (!ok) {
        finish(job->id, false, QStringLiteral("the upload failed"));
        return;
    }
    const maker::Made& made = job->made;
    Announcement       a;
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
    a.durationMs = made.kind == Kind::Video ? made.durationMs : 0;
    a.title      = made.title;
    a.author     = made.author;
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
    const qint64         age     = qMax<qint64>(0, wallSec() - job.receivedSec);
    const proto::Message message = makeMessage(a, job.scope, textreact::wireRef(job.hash, age));
    if (message.type.isEmpty()) {
        finish(id, false, QStringLiteral("the announcement would break the rules"));
        return;
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
    m_link->sendWith(sch, message, target, PluginLink::Priority::Background, [sch, host](peers::SendResult result) {
        if (result != peers::SendResult::Ok && result != peers::SendResult::LateOk)
            ts3::log(LogLevel_INFO, sch, "A link preview (%1) couldn't be announced: the plugin command wasn't sent", {ts3::name(host)});
    });
    ++m_counters.announced;
    finish(id, true);
}

void SenderPipeline::finish(int id, bool announced, const QString& why)
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
        const qint64         age = qMax<qint64>(0, wallSec() - job.receivedSec);
        const proto::Message msg = makeSkip(job.scope, textreact::wireRef(job.hash, age), job.target.key);
        peers::Target        target;
        if (!msg.type.isEmpty() && targetOf(job, &target)) {
            const quint64 sch  = job.sch;
            const QString host = job.target.host;
            m_link->sendWith(sch, msg, target, PluginLink::Priority::Background, [sch, host](peers::SendResult r) {
                if (r != peers::SendResult::Ok && r != peers::SendResult::LateOk)
                    ts3::log(LogLevel_INFO, sch, "A link preview's notice (%1) couldn't be sent", {ts3::name(host)});
            });
            ++m_counters.skipNotices;
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

void SenderPipeline::applySettings()
{
    if (m_closed || enabled())
        return;
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
