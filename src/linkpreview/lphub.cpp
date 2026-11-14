#include "lphub.h"

#include <QDateTime>
#include <QFile>
#include <QTimer>

#include "core.h"
#include "fetcher.h"
#include "hostgate.h"
#include "pluginlink.h"
#include "previewstore.h"
#include "reactions.h" // ReactionStore::isClientUid
#include "routereport.h" // 2.6 chat proxy
#include "senderpipeline.h"
#include "settings.h"
#include "textreact.h"
#include "transport.h"
#include "nettrace.h" // live-test builds: the driver's links.net
#include "ts3api.h"

namespace linkpreview {

namespace {

Hub* g_hub = nullptr; // GUI thread only

constexpr int kWaitCheckMs = 1000;

qint64 wallMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 wallSec()
{
    return QDateTime::currentSecsSinceEpoch();
}

quint64 channelOf(quint64 sch, quint16 client)
{
    uint64 channel = 0;
    if (!client || !ts3::funcs.getChannelOfClient || ts3::funcs.getChannelOfClient(sch, client, &channel) != ERROR_ok)
        return 0;
    return channel;
}

bool isQueryClient(quint64 sch, quint16 client)
{
    int type = 0;
    return ts3::funcs.getClientVariableAsInt && ts3::funcs.getClientVariableAsInt(sch, client, CLIENT_TYPE, &type) == ERROR_ok
           && type == ClientType_SERVERQUERY;
}

} // namespace

// The plugin's Fetcher behind the pipeline's FetchPort.
class Hub::FetcherPort : public FetchPort
{
  public:
    explicit FetcherPort(Fetcher* fetcher)
        : m_fetcher(fetcher)
    {
    }
    void load(const Target& target, const Caps& caps, const QString& cacheDir) override
    {
        if (m_fetcher)
            m_fetcher->load(target, caps, cacheDir);
    }
    void cancel(const QString& key) override
    {
        if (m_fetcher)
            m_fetcher->cancel(key);
    }

  private:
    QPointer<Fetcher> m_fetcher;
};

Hub::Hub(Core* core, PluginLink* link, const textreact::Index* index, QObject* parent)
    : Hub(core, link, index, nullptr, Limits(), parent)
{
}

Hub::Hub(Core* core, PluginLink* link, const textreact::Index* index, FetchPort* fetch, const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_link(link)
    , m_index(index)
    , m_limits(limits)
{
    // No qRegisterMetaType here or anywhere in the plugin: Qt's metatype registry would keep this DLL's copy and
    // delete functions after an unload, and a reload at another address would call into freed code. Every link
    // preview signal is connected directly (they are all emitted on the GUI thread).
    m_store = new PreviewStore(this);
    if (!fetch) {
        // The sender's own Fetcher: its results are the pipeline's only (the chat's clicks and automatic loads have
        // their own). 2.5: both share the plugin's one HostGate (spacing and cooldowns per site, hostgate.h).
        Fetcher::Options options;
        options.gate      = sharedGate();
        options.routeGate = &admitsRoute; // 2.6 chat proxy: our own links' loads are automatic (they wait while it is down)
#ifdef TSMEDIA_TESTHOOKS
        m_fetcher = new Fetcher(nettrace::traced(http::winHttpTransport(), "send"), options, this);
#else
        m_fetcher    = new Fetcher(http::winHttpTransport(), options, this);
#endif
        usePluginLog(m_fetcher, "send");
        m_ownPort = std::make_unique<FetcherPort>(m_fetcher);
        fetch     = m_ownPort.get();
    }
    m_pipeline = new SenderPipeline(core, link, m_store, index, fetch, this);
    if (m_fetcher) {
        // 2.6 chat proxy (design 5.2): route health hears our own links' loads too (before the pipeline acts on them).
        connect(m_fetcher, &Fetcher::finished, this, [](const QString&, const Result& result) { reportRoute(proxy::Control::current(), result, QString()); },
                Qt::DirectConnection);
        connect(m_fetcher, &Fetcher::finished, m_pipeline, &SenderPipeline::fetched, Qt::DirectConnection);
    }
    // 2.6 chat proxy (design 6 step 5): a running fetch of ours starts again on the new route; queued jobs simply use it.
    if (proxy::Control* control = proxy::Control::current())
        connect(control, &proxy::Control::routeChanged, m_pipeline, &SenderPipeline::followRoute);

    // Member timers only (children): nothing of ours can be pending once the hub is gone.
    m_waitTimer = new QTimer(this);
    m_waitTimer->setInterval(kWaitCheckMs);
    connect(m_waitTimer, &QTimer::timeout, this, [this] { matchWaiting(); });
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { save(); });

    connect(m_store, &PreviewStore::changed, this, [this] { scheduleSave(); });
    if (m_link)
        connect(m_link, &PluginLink::received, this, &Hub::received);
    if (core) {
        // "Clear cache" forgets the link previews too (their files go with the cache).
        connect(core, &Core::cacheCleared, this, [this] {
            m_skips.clear();
            m_store->clear();
            m_dirty = false;
            m_saveTimer->stop();
            QFile::remove(storePath());
        });
    }
    g_hub = this;
}

Hub::~Hub()
{
    prepareShutdown();
    if (g_hub == this)
        g_hub = nullptr;
    // The pipeline goes before the Fetcher it was given (both are children; deleted here in this order).
    delete m_pipeline;
    m_pipeline = nullptr;
    if (m_fetcher)
        m_fetcher->shutdown();
    delete m_fetcher;
    m_fetcher = nullptr;
}

Hub* Hub::instance()
{
    return g_hub;
}

void usePluginLog(Fetcher* fetcher, const char* origin)
{
    if (!fetcher)
        return;
    const QString from = QString::fromLatin1(origin);
    fetcher->setLogSink([from](const LogLine& line) {
        ts3::log(LogLevel_INFO, 0, "Link preview (%1, %2) %3: %4%5",
                 {ts3::pub(from), ts3::pub(QString::fromLatin1(kindName(line.kind))), ts3::name(line.address), ts3::pub(line.outcome),
                  ts3::pub(line.viaChatProxy ? QString::fromLatin1(" (via the chat proxy)")
                           : line.viaProxy   ? QString::fromLatin1(" (via the Windows proxy)")
                                             : QString())});
    });
}

QString linksCacheDir(const Core* core)
{
    return core ? core->cacheDir() + QString::fromLatin1("/links") : QString();
}

QString Hub::storePath() const
{
    return ts3::dataDir() + QLatin1String("/link-previews.json");
}

void Hub::start()
{
    if (m_started || m_shuttingDown)
        return;
    m_started = true;
    m_store->load(storePath(), wallSec());
    m_dirty = false;
}

void Hub::prepareShutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_pipeline->shutdown(); // fetch canceled, worker joined, Core's upload canceled, work files removed
    if (m_fetcher)
        m_fetcher->shutdown();
    m_waitTimer->stop();
    m_waiting.clear();
    m_skips.clear();
    m_rates.clear();
    m_xRates.clear();
    m_saveTimer->stop();
    if (m_started)
        save();
}

void Hub::applySettings()
{
    if (m_shuttingDown)
        return;
    m_pipeline->applySettings();
    if (!Settings::instance().showLinkPreviews) {
        m_waiting.clear(); // off: nothing more is accepted, stored or downloaded
        m_skips.clear();
        m_waitTimer->stop();
    }
}

void Hub::scheduleSave()
{
    m_dirty = true;
    if (!m_shuttingDown && !m_saveTimer->isActive())
        m_saveTimer->start(m_limits.saveDelayMs);
}

void Hub::save()
{
    if (!m_dirty && QFile::exists(storePath()))
        return;
    if (m_store->count() == 0 && !QFile::exists(storePath()))
        return;
    if (m_store->save(storePath(), wallSec()))
        m_dirty = false;
}

// ---- sending ------------------------------------------------------------------------------------------

void Hub::onTextMessage(quint64 sch, int targetMode, quint16 toId, quint16 fromId, const QString& fromUid, const QString& bbcode)
{
    if (!m_shuttingDown)
        m_pipeline->onTextMessage(sch, targetMode, toId, fromId, fromUid, bbcode);
}

// ---- receiving ----------------------------------------------------------------------------------------

bool Hub::takeToken(quint64 sch, const QString& uid, qint64 now, bool extended)
{
    // 2.5: LPX and LPXNO have a bucket of their own (LP's stays 2.4.0's).
    QHash<QString, Bucket>& rates = extended ? m_xRates : m_rates;
    const int               burst = extended ? m_limits.xPerSenderBurst : m_limits.perSenderBurst;
    const QString who = QString::number(sch) + QLatin1Char('/') + uid;
    if (!rates.contains(who) && rates.size() >= m_limits.maxRateEntries) {
        // Bounded: buckets that are full again are forgotten first (they would start full anyway).
        for (auto it = rates.begin(); it != rates.end();) {
            if (now - it->atMs >= m_limits.perSenderWindowMs || it->atMs > now)
                it = rates.erase(it);
            else
                ++it;
        }
        if (rates.size() >= m_limits.maxRateEntries)
            return false;
    }
    Bucket&      b    = rates[who];
    const double rate = static_cast<double>(burst) / static_cast<double>(qMax<qint64>(1, m_limits.perSenderWindowMs));
    if (b.atMs < 0 || now < b.atMs)
        b.tokens = burst;
    else
        b.tokens = qMin(static_cast<double>(burst), b.tokens + (now - b.atMs) * rate);
    b.atMs = now;
    if (b.tokens < 1.0)
        return false;
    b.tokens -= 1.0;
    return true;
}

void Hub::received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message)
{
    Q_UNUSED(name);
    Type type = Type::Lp;
    if (message.type == "LP")
        type = Type::Lp;
    else if (message.type == "LPNO")
        type = Type::Lpno; // 2.4 auto-load: the sender's plugin won't announce this one
    else if (message.type == "LPX")
        type = Type::Lpx; // 2.5: a page link's preview
    else if (message.type == "LPXNO")
        type = Type::Lpxno;
    else
        return; // everything else is PeerHub's or TextReactHub's
    if (m_shuttingDown)
        return;
    const bool extended = type == Type::Lpx || type == Type::Lpxno;
    switch (type) {
    case Type::Lp:
        ++m_counters.received;
        break;
    case Type::Lpno:
        ++m_counters.skipsReceived;
        break;
    case Type::Lpx:
        ++m_counters.lpxReceived;
        break;
    case Type::Lpxno:
        ++m_counters.lpxSkipsReceived;
        break;
    }
    if (!Settings::instance().showLinkPreviews)
        return; // off: nothing is accepted, stored or downloaded
    if (!ReactionStore::isClientUid(uid) || isQueryClient(sch, from)) {
        ++m_counters.rejected; // ServerQuery logins aren't people in a chat
        return;
    }
    if (!takeToken(sch, uid, wallMs(), extended)) { // LP and LPNO share one limit per sender; LPX and LPXNO another
        ++m_counters.rateLimited;
        return;
    }
    Waiting w;
    w.type = type;
    if (type == Type::Lpno || type == Type::Lpxno) {
        proto::Scope scope = proto::Scope::Channel;
        if (type == Type::Lpno) {
            const std::optional<Skip> in = readSkip(message);
            if (!in) {
                ++m_counters.rejected;
                return;
            }
            scope          = in->scope;
            w.keys         = QStringList{in->urlKey};
            w.hash         = in->hash;
            w.estimatedSec = wallSec() - in->ageSec;
        } else {
            const std::optional<SkipX> in = readLpxSkip(message);
            if (!in) {
                ++m_counters.rejected;
                return;
            }
            scope          = in->scope;
            w.keys         = in->urlKeys;
            w.reason       = in->reason;
            w.hash         = in->hash;
            w.estimatedSec = wallSec() - in->ageSec;
        }
        // A channel notice only from someone in our channel right now (it names no files).
        if (scope == proto::Scope::Channel) {
            const quint64 theirs = channelOf(sch, from);
            if (theirs == 0 || theirs != ts3::ownChannel(sch)) {
                ++m_counters.rejected;
                return;
            }
        }
        w.scope    = scope;
        w.a.urlKey = w.keys.value(0);
    } else {
        const std::optional<Incoming> in = type == Type::Lpx ? readLpx(message) : readMessage(message);
        if (!in) {
            ++m_counters.rejected;
            return;
        }
        // A channel announcement only from someone in our channel right now, for files in our channel (an LPX card
        // without a picture names no files).
        const quint64 own = ts3::ownChannel(sch);
        if (in->scope == proto::Scope::Channel) {
            const quint64 theirs = channelOf(sch, from);
            const bool    files  = in->announcement.channelId != 0 || !in->announcement.previewPath.isEmpty();
            if (theirs == 0 || theirs != own || (files && in->announcement.channelId != own)) {
                ++m_counters.rejected;
                return;
            }
        }
        w.a            = in->announcement;
        w.scope        = in->scope;
        w.hash         = in->hash;
        w.estimatedSec = wallSec() - in->ageSec;
    }
    w.a.sch       = sch;
    w.a.senderUid = uid; // the server's, never the payload's
    w.a.serverUid = ts3::serverUid(sch);
    w.atMs        = wallMs();
    if (w.a.serverUid.isEmpty()) {
        ++m_counters.rejected;
        return;
    }
    const QString key = resolve(w);
    if (!key.isEmpty()) {
        if (w.skip()) {
            acceptSkip(w, key);
        } else {
            w.a.messageKey = key;
            accept(w.a);
        }
        return;
    }
    // Not in a chat (yet): a newer LP of the same sender for the same link of the same message replaces it.
    for (int i = 0; i < m_waiting.size(); ++i) {
        const Waiting& o = m_waiting.at(i);
        if (o.type == w.type && o.a.senderUid == uid && o.a.sch == sch && o.hash == w.hash && o.a.urlKey == w.a.urlKey && o.keys.join(QLatin1Char(',')) == w.keys.join(QLatin1Char(',')) /* joined: no QList == (MSVC) */
            && qAbs(o.estimatedSec - w.estimatedSec) <= textreact::kToleranceSec) {
            m_waiting.removeAt(i);
            break;
        }
    }
    if (m_waiting.size() >= m_limits.maxWaiting) {
        m_waiting.removeFirst();
        ++m_counters.unmatched;
    }
    m_waiting.append(w);
    if (!m_waitTimer->isActive())
        m_waitTimer->start();
}

QString Hub::resolve(const Waiting& w) const
{
    if (!m_index)
        return {};
    textreact::Index::Query q;
    q.serverUid    = w.a.serverUid;
    q.scope        = w.scope;
    q.reactorUid   = w.a.senderUid;
    q.ownUid       = ts3::ownUid(w.a.sch);
    q.hash         = w.hash;
    q.estimatedSec = w.estimatedSec;
    q.toleranceSec = qMax<qint64>(textreact::kToleranceSec, m_limits.matchToleranceSec); // Limits: the transport's delay
    q.senderOnly   = true; // the announcer's own message, nobody else's
    return m_index->resolve(q);
}

void Hub::accept(Announcement a)
{
    const PreviewStore::Add result = m_store->check(a);
    if (result == PreviewStore::Add::Rejected) {
        ++m_counters.rejected;
        return;
    }
    if (result == PreviewStore::Add::Unchanged)
        return;
    if (a.extended) {
        // 2.5: an LPX's files are asked of Core by the chat when its slot is on screen (paced: a history full of
        // cards never floods the connection).
        ++m_counters.lpxAccepted;
    } else {
        ++m_counters.accepted;
        requestFiles(m_core, a); // Core knows the files before the chat hears of them ...
    }
    m_store->add(a, wallSec()); // ... from announced()
}

void Hub::acceptSkip(const Waiting& w, const QString& messageKey)
{
    QString hash;
    qint64  timeSec = 0;
    if (!textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return;
    const qint64 now = wallMs();
    for (const QString& urlKey : w.keys) {
        if (m_store->find(messageKey, urlKey))
            continue; // its LP / LPX is here already: that wins
        for (int i = m_skips.size() - 1; i >= 0; --i) {
            const SkipEntry& e = m_skips.at(i);
            if (now - e.atMs > kSkipTtlMs || now < e.atMs || (e.hash == hash && e.urlKey == urlKey && e.timeSec == timeSec))
                m_skips.removeAt(i); // old, or the same notice again (kept once, as the newest)
        }
        while (m_skips.size() >= kMaxSkips)
            m_skips.removeFirst();
        m_skips.append({hash, timeSec, urlKey, now, w.reason});
        if (w.type == Type::Lpxno)
            ++m_counters.lpxSkipsAccepted;
        else
            ++m_counters.skipsAccepted;
        emit skipped(urlKey);
    }
}

bool Hub::isSkipped(const QString& messageKey, const QString& urlKey) const
{
    QString hash;
    qint64  timeSec = 0;
    if (m_skips.isEmpty() || urlKey.isEmpty() || !textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return false;
    const qint64 now = wallMs();
    for (const SkipEntry& e : m_skips) {
        if (e.hash == hash && e.urlKey == urlKey && qAbs(e.timeSec - timeSec) <= textreact::kToleranceSec && now - e.atMs <= kSkipTtlMs && now >= e.atMs)
            return true;
    }
    return false;
}

SkipReason Hub::skipReason(const QString& messageKey, const QString& urlKey) const
{
    QString hash;
    qint64  timeSec = 0;
    if (m_skips.isEmpty() || urlKey.isEmpty() || !textreact::parseLocalKey(messageKey, &hash, &timeSec))
        return SkipReason::Unknown;
    const qint64 now = wallMs();
    // The newest notice for that link wins.
    for (int i = m_skips.size() - 1; i >= 0; --i) {
        const SkipEntry& e = m_skips.at(i);
        if (e.hash == hash && e.urlKey == urlKey && qAbs(e.timeSec - timeSec) <= textreact::kToleranceSec && now - e.atMs <= kSkipTtlMs && now >= e.atMs)
            return e.reason;
    }
    return SkipReason::Unknown;
}

void Hub::indexChanged()
{
    if (!m_waiting.isEmpty())
        matchWaiting();
}

void Hub::matchWaiting()
{
    const qint64     now = wallMs();
    QVector<Waiting> still;
    const QVector<Waiting> waiting = m_waiting;
    m_waiting.clear();
    for (const Waiting& w : waiting) {
        if (now - w.atMs > m_limits.waitTtlMs || now < w.atMs || !Settings::instance().showLinkPreviews) {
            ++m_counters.unmatched;
            continue;
        }
        const QString key = resolve(w);
        if (key.isEmpty()) {
            still.append(w);
            continue;
        }
        if (w.skip()) {
            acceptSkip(w, key);
            continue;
        }
        Announcement a = w.a;
        a.messageKey   = key;
        accept(a);
    }
    m_waiting = still + m_waiting; // accept() never adds, but keep anything that came meanwhile
    if (m_waiting.isEmpty())
        m_waitTimer->stop();
}

} // namespace linkpreview
