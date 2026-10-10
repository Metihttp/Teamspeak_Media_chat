#include "peerhub.h"

#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "composehooks.h"
#include "floodgovernor.h"
#include "i18n.h"
#include "pluginlink.h"
#include "presenceline.h"
#include "settings.h"
#include "ts3api.h"
#include "version.h"

namespace {

std::atomic<PeerHub*> g_hub{nullptr};
std::atomic<int>      g_queuedCommands{0};

// TeamSpeak's callbacks run on its own thread: one holds this from reading g_hub until its event is
// posted, and ~PeerHub takes it to clear g_hub, so no callback can post to a hub that is being freed.
std::mutex& hubMutex()
{
    static std::mutex mutex;
    return mutex;
}

constexpr int kMaxPrivateKeys     = 256; // media keys remembered per private chat partner
constexpr int kMaxPrivatePartners = 128; // private chat partners remembered per connection

constexpr int kMaxQueuedCommands  = 256;   // plugin commands waiting for the GUI thread; more are dropped
constexpr int kDebounceMs         = 400;   // a reaction goes out this long after the last click on it
constexpr int kSyncWindowMs       = 10000; // answers to our SYNC are accepted this long
constexpr int kSyncAnswerGapMs    = 60000; // one answer per person and connection per ...
constexpr int kSyncAnswerMinMs    = 200;
constexpr int kSyncAnswerMaxMs    = 1500;
constexpr int kSyncAnswerMessages = 2;
constexpr int kCoalesceMs         = 50;
constexpr int kSaveDelayMs        = 5000;
constexpr int kOrphanCheckMs      = 1000;

// The values the pure modules use without the SDK.
static_assert(TextMessageTarget_CLIENT == 1 && TextMessageTarget_CHANNEL == 2 && TextMessageTarget_SERVER == 3, "peers.cpp target modes");
static_assert(ERROR_client_is_flooding == 0x020c && ERROR_client_invalid_id == 0x0200, "pluginlink.cpp error codes");
static_assert(sizeof(anyID) == sizeof(quint16), "client ids are quint16 in the pure modules");

QString str(const char* s)
{
    return s ? QString::fromUtf8(s) : QString();
}

qint64 wallMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

// ---- TeamSpeak and Core behind the pure modules ---------------------------------------------------

class PeerHub::Env : public peers::Env, public PluginLink::Backend
{
  public:
    explicit Env(Core* core)
        : m_core(core)
    {
    }

    // Both interfaces: the FloodGovernor's clock (monotonic).
    qint64 nowMs() const override { return m_core ? m_core->floodClockMs() : 0; }

    FloodGovernor* governor(quint64 sch) override { return m_core && ts3::isConnected(sch) ? &m_core->floodGovernor(sch) : nullptr; }
    void           floodStateChanged(quint64 sch) override
    {
        if (m_core)
            m_core->floodStateChanged(sch);
    }
    QString newReturnCode() override { return ts3::newReturnCode(); }

    void sendCommand(quint64 sch, const QByteArray& payload, const peers::Target& target, const QString& returnCode) override
    {
        // Sent with the GUID TeamSpeak gave this load; anything else is dropped by the client (S0).
        if (!ts3::funcs.sendPluginCommand || ts3::pluginId.isEmpty())
            return; // no answer will come: the link counts it as not sent
        const QByteArray id   = ts3::pluginId.toUtf8();
        const QByteArray code = returnCode.toUtf8();
        if (target.channel) {
            ts3::funcs.sendPluginCommand(sch, id.constData(), payload.constData(), PluginCommandTarget_CURRENT_CHANNEL, nullptr, code.constData());
            return;
        }
        std::vector<anyID> ids(target.clients.cbegin(), target.clients.cend());
        ids.push_back(0); // 0-terminated (S0)
        ts3::funcs.sendPluginCommand(sch, id.constData(), payload.constData(), PluginCommandTarget_CLIENT, ids.data(), code.constData());
    }

    bool    isConnected(quint64 sch) const override { return ts3::isConnected(sch); }
    bool    clientVisible(quint64 sch, quint16 client) const override { return client != 0 && channelOf(sch, client) != 0; }
    quint16 ownClientId(quint64 sch) const override { return ts3::ownClientId(sch); }
    quint64 ownChannel(quint64 sch) const override { return ts3::ownChannel(sch); }

    quint64 channelOf(quint64 sch, peers::ClientId client) const override
    {
        uint64 channel = 0;
        if (!ts3::funcs.getChannelOfClient || ts3::funcs.getChannelOfClient(sch, client, &channel) != ERROR_ok)
            return 0;
        return channel;
    }

    QVector<peers::ClientId> channelClients(quint64 sch, quint64 channel) const override
    {
        QVector<peers::ClientId> list;
        anyID*                   clients = nullptr;
        if (!ts3::funcs.getChannelClientList || ts3::funcs.getChannelClientList(sch, channel, &clients) != ERROR_ok || !clients)
            return list;
        for (anyID* it = clients; *it; ++it)
            list.append(*it);
        ts3::funcs.freeMemory(clients);
        return list;
    }

    QString uid(quint64 sch, peers::ClientId client) const override { return clientString(sch, client, CLIENT_UNIQUE_IDENTIFIER); }
    QString nickname(quint64 sch, peers::ClientId client) const override { return clientString(sch, client, CLIENT_NICKNAME); }

    bool isQueryClient(quint64 sch, peers::ClientId client) const override
    {
        int type = 0;
        return ts3::funcs.getClientVariableAsInt && ts3::funcs.getClientVariableAsInt(sch, client, CLIENT_TYPE, &type) == ERROR_ok
               && type == ClientType_SERVERQUERY;
    }

    int randomBetween(int low, int high) override { return low >= high ? low : QRandomGenerator::global()->bounded(low, high + 1); }

  private:
    static QString clientString(quint64 sch, peers::ClientId client, size_t flag)
    {
        char* value = nullptr;
        if (!ts3::funcs.getClientVariableAsString || ts3::funcs.getClientVariableAsString(sch, client, flag, &value) != ERROR_ok)
            return {};
        return ts3::takeString(value);
    }

    Core* m_core;
};

// ---- the hub --------------------------------------------------------------------------------------

PeerHub::PeerHub(Core* core, QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_env(std::make_unique<Env>(core))
{
    m_link      = new PluginLink(*m_env, this);
    m_directory = new peers::PeerDirectory(*m_env, *m_link, this);
    m_store     = new ReactionStore(this);

    // Member timers only (children): nothing of ours can be pending once the hub is gone.
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        flushPending();
        scheduleTimers();
    });
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    connect(m_coalesce, &QTimer::timeout, this, [this] {
        const QSet<QString> keys = m_changedKeys;
        m_changedKeys.clear();
        for (const QString& key : keys)
            emit reactionsChanged(key);
    });
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { save(); });
    m_orphanTimer = new QTimer(this);
    m_orphanTimer->setInterval(kOrphanCheckMs);
    connect(m_orphanTimer, &QTimer::timeout, this, [this] {
        m_store->resolveKnownOrphans(wallMs());
        if (m_store->orphanCount() == 0)
            m_orphanTimer->stop();
    });

    // Reactions are for media this client has seen (in any chat); the rest waits a little.
    m_store->setKnownKey([this](const QString& key, const QString&) { return m_core && m_core->entry(key) != nullptr; });

    connect(m_link, &PluginLink::received, this, &PeerHub::received);
    connect(m_link, &PluginLink::blockedChanged, this, [this](quint64 sch) {
        if (m_link->blocked(sch))
            ts3::log(LogLevel_WARNING, sch, "This server doesn't pass on plugin commands: presence and reactions are off for this connection", {});
        m_directory->commandsBlocked(sch);
        emit presenceChanged(sch);
    });
    connect(m_directory, &peers::PeerDirectory::presenceChanged, this, &PeerHub::presenceChanged);
    connect(m_directory, &peers::PeerDirectory::syncDue, this, &PeerHub::sendSync);
    connect(m_store, &ReactionStore::changed, this, &PeerHub::storeChanged);
    if (core) {
        connect(core, &Core::floodGovernorChanged, this, [this] { m_link->pump(); });
        connect(core, &Core::entryChanged, this, [this](const QString& key) {
            if (m_store->orphanCount() > 0)
                m_store->resolveOrphans(key, wallMs());
        });
        connect(core, &Core::cacheCleared, this, [this] {
            m_store->clear();
            m_dirty = false;
            m_saveTimer->stop();
            QFile::remove(reactionsPath());
        });
    }
    g_hub = this;
}

PeerHub::~PeerHub()
{
    {
        // Waits for a callback that has read g_hub and is posting to us right now (see hubMutex).
        std::lock_guard<std::mutex> lock(hubMutex());
        g_hub = nullptr;
    }
    // Events still queued for us are dropped with this object, uncounted.
    g_queuedCommands = 0;
    compose::setPresenceLineFactory(nullptr);
    // Before m_env goes: they hold references to it.
    delete m_directory;
    m_directory = nullptr;
    delete m_link;
    m_link = nullptr;
    delete m_store;
    m_store = nullptr;
}

PeerHub* PeerHub::instance()
{
    return g_hub.load();
}

qint64 PeerHub::nowMs() const
{
    return m_env->nowMs();
}

QString PeerHub::reactionsPath() const
{
    return ts3::dataDir() + QLatin1String("/reactions.json");
}

void PeerHub::start()
{
    m_store->load(reactionsPath(), QDateTime::currentSecsSinceEpoch());
    m_dirty = false;
    applySettings();
    for (const uint64 sch : ts3::connections()) {
        if (!ts3::isConnected(sch))
            continue;
        ownUid(sch); // cached for drawing
        m_directory->connected(sch);
    }
    compose::setPresenceLineFactory([](QWidget* parent, const ChatTarget& target) -> QWidget* { return new PresenceLine(target, parent); });
}

void PeerHub::prepareShutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    compose::setPresenceLineFactory(nullptr);
    m_timer->stop();
    m_coalesce->stop();
    m_orphanTimer->stop();
    m_directory->setAutoTimer(false);
    // Toggles that never reached anyone are not kept as if they had.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const QString uid = ownUid(it->sch);
        if (ReactionStore::isClientUid(uid) && m_store->maskOf(it.key(), uid) != it->committed) {
            m_store->setOwn(it.key(), uid, ownName(it->sch), it->committed, wallMs());
            m_dirty = true;
        }
    }
    m_pending.clear();
    m_link->setClosing();      // queued commands are dropped; what follows goes out now or never
    m_directory->sayGoodbye(); // BYE, without waiting for an answer
    m_saveTimer->stop();
    save();
}

void PeerHub::applySettings()
{
    const Settings& s = Settings::instance();
    m_directory->setLocal({QString::fromLatin1(TSMEDIA_VERSION), s.sharePresence, s.showReactions});
}

// ---- receiving ------------------------------------------------------------------------------------

void PeerHub::received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message)
{
    if (m_shuttingDown)
        return;
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QString::fromLatin1("[test] peer rx sch %1 from %2 type %3").arg(sch).arg(from).arg(QString::fromLatin1(message.type)));
#endif
    if (message.type == "HELLO" || message.type == "HI" || message.type == "BYE") {
        m_directory->received(sch, from, uid, name, message);
        return;
    }
    if (!Settings::instance().showReactions)
        return; // off: nothing is shown, stored or answered
    if (m_env->isQueryClient(sch, from))
        return; // ServerQuery logins aren't people in a chat: no reactions or questions from them
    if (message.type == "R") {
        if (const std::optional<proto::React> react = proto::readReact(message))
            receiveReact(sch, from, uid, name, *react);
    } else if (message.type == "SYNC") {
        if (const std::optional<proto::Sync> sync = proto::readSync(message))
            receiveSync(sch, from, uid, *sync);
    }
    // Unknown types: a newer version's; ignored.
}

void PeerHub::receiveReact(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::React& react)
{
    // A channel reaction only from someone in our channel right now. A private one only from someone we
    // have a private chat with, and only on media of that chat: anyone on the server can address a
    // plugin command to us (and the target mode isn't reported), so s=p alone proves nothing.
    QVector<proto::ReactItem> items = react.items;
    if (react.scope == proto::Scope::Channel) {
        const quint64 theirs = m_env->channelOf(sch, from);
        if (theirs == 0 || theirs != ts3::ownChannel(sch))
            return;
    } else {
        const QStringList shared = m_privateMedia.value(sch).value(uid);
        items.erase(std::remove_if(items.begin(), items.end(), [&shared](const proto::ReactItem& item) { return !shared.contains(item.key); }), items.end());
        if (items.isEmpty())
            return;
    }
    // Answers to a SYNC only shortly after we asked.
    if (react.syncAnswer) {
        const auto asked = m_syncSentMs.constFind(sch);
        if (asked == m_syncSentMs.constEnd() || nowMs() - asked.value() > kSyncWindowMs)
            return;
    }
    const QString server = ts3::serverUid(sch);
    const qint64  now    = wallMs();
    bool          orphan = false;
    for (const proto::ReactItem& item : qAsConst(items))
        orphan = m_store->applyRemote(server, item.key, uid, name, item.mask, now) == ReactionStore::Apply::Orphaned || orphan;
    if (orphan && !m_orphanTimer->isActive())
        m_orphanTimer->start();
}

void PeerHub::receiveSync(quint64 sch, quint16 from, const QString& uid, const proto::Sync& sync)
{
    if (sync.scope != proto::Scope::Channel || m_env->channelOf(sch, from) != ts3::ownChannel(sch))
        return;
    const QString who  = QString::number(sch) + QLatin1Char('/') + uid;
    const qint64  now  = nowMs();
    const auto    last = m_syncAnsweredMs.constFind(who);
    if (last != m_syncAnsweredMs.constEnd() && now - last.value() < kSyncAnswerGapMs)
        return;
    m_syncAnsweredMs.insert(who, now);
    if (m_syncAnsweredMs.size() > 512) { // bounded: forget answers older than the gap
        for (auto it = m_syncAnsweredMs.begin(); it != m_syncAnsweredMs.end();) {
            if (now - it.value() >= kSyncAnswerGapMs)
                it = m_syncAnsweredMs.erase(it);
            else
                ++it;
        }
    }
    // Answers spread out a little, so a late joiner's question doesn't get everyone's at once.
    m_syncAnswers.append({sch, from, sync.keys, now + m_env->randomBetween(kSyncAnswerMinMs, kSyncAnswerMaxMs)});
    scheduleTimers();
}

void PeerHub::sendSync(quint64 sch)
{
    if (m_shuttingDown || !Settings::instance().showReactions || !m_presentKeys || !m_presentKeysOwner)
        return;
    const QStringList keys = m_presentKeys(ts3::serverUid(sch));
    if (keys.isEmpty())
        return;
    // The answer window opens when the SYNC is handed over (its answers can come before our own Ok
    // does, or after an Ok that came too late to count) and again once it is confirmed.
    m_syncSentMs.insert(sch, nowMs());
    QPointer<PeerHub> guard(this);
    m_link->sendWith(sch, proto::makeSync(proto::Scope::Channel, keys), peers::Target::toChannel(), PluginLink::Priority::Background,
                     [guard, sch](peers::SendResult result) {
                         if (guard && (result == peers::SendResult::Ok || result == peers::SendResult::LateOk))
                             guard->m_syncSentMs.insert(sch, guard->nowMs());
                     });
}

void PeerHub::setPresentKeys(QObject* owner, PresentKeys keys)
{
    m_presentKeysOwner = owner;
    m_presentKeys      = std::move(keys);
}

void PeerHub::notePrivateMedia(quint64 sch, const QString& partnerUid, const QStringList& keys)
{
    if (m_shuttingDown || !ReactionStore::isClientUid(partnerUid) || keys.isEmpty())
        return;
    QHash<QString, QStringList>& partners = m_privateMedia[sch];
    if (!partners.contains(partnerUid) && partners.size() >= kMaxPrivatePartners)
        partners.erase(partners.begin()); // bounded: someone else's chat is forgotten
    QStringList& known = partners[partnerUid];
    for (const QString& key : keys) {
        if (!proto::isMediaKey(key))
            continue;
        known.removeAll(key);
        known.append(key); // most recent last
    }
    while (known.size() > kMaxPrivateKeys)
        known.removeFirst();
}

// ---- your reactions -------------------------------------------------------------------------------

PeerHub::ReactError PeerHub::reactBlock(const ChatTarget& target) const
{
    if (!Settings::instance().showReactions)
        return ReactError::Off;
    if (target.mode == TextMessageTarget_SERVER)
        return ReactError::ServerChat;
    if (!ts3::isConnected(target.sch))
        return ReactError::NotConnected;
    if (m_link->blocked(target.sch))
        return ReactError::Blocked;
    if (target.mode == TextMessageTarget_CLIENT && (target.clientId == 0 || !m_env->clientVisible(target.sch, target.clientId)))
        return ReactError::PartnerOffline;
    return ReactError::None;
}

PeerHub::ReactError PeerHub::toggle(const QString& key, int reaction, const ChatTarget& target)
{
    if (m_shuttingDown || reaction < 0 || reaction >= proto::kReactionCount || !proto::isMediaKey(key))
        return ReactError::Unknown;
    const ReactError block = reactBlock(target);
    if (block != ReactError::None)
        return block;
    const QString uid = ownUid(target.sch);
    if (!ReactionStore::isClientUid(uid))
        return ReactError::NotConnected;

    auto it = m_pending.find(key);
    if (it == m_pending.end()) {
        it            = m_pending.insert(key, Pending());
        it->committed = m_store->maskOf(key, uid);
    }
    it->sch           = target.sch;
    it->target        = target;
    const quint8 mask = static_cast<quint8>(m_store->maskOf(key, uid) ^ (1u << reaction));
    m_store->setOwn(key, uid, ownName(target.sch), mask, wallMs()); // shown at once
    it->dueMs = nowMs() + kDebounceMs;                              // sent once the clicking stops
    scheduleTimers();
    return ReactError::None;
}

void PeerHub::flushPending()
{
    const qint64 now = nowMs();
    QStringList  due; // first: a callback may run right away and change m_pending
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (it->dueMs >= 0 && now >= it->dueMs)
            due.append(it.key());
    }
    for (const QString& key : qAsConst(due)) {
        auto it = m_pending.find(key);
        if (it == m_pending.end())
            continue;
        Pending& p         = it.value();
        p.dueMs            = -1;
        const QString uid  = ownUid(p.sch);
        const quint8  mask = m_store->maskOf(key, uid);
        if (mask == p.committed && !p.inFlight) {
            m_pending.erase(it); // clicked back to where it was: nothing to tell anyone
            continue;
        }
        const bool                    privateChat = p.target.mode == TextMessageTarget_CLIENT;
        const proto::Scope            scope       = privateChat ? proto::Scope::Private : proto::Scope::Channel;
        const QVector<proto::Message> messages    = proto::makeReacts(scope, false, {{key, mask}});
        if (messages.isEmpty())
            continue;
        const peers::Target target = privateChat ? peers::Target::toClients({p.target.clientId}) : peers::Target::toChannel();
        const quint64       gen    = ++p.generation;
        p.sent                     = mask;
        p.inFlight                 = true;
        const quint64     sch      = p.sch;
        const QString     coalesce = QStringLiteral("R/%1/%2/%3").arg(sch).arg(privateChat ? p.target.clientId : 0).arg(key);
        QPointer<PeerHub> guard(this);
        m_link->sendWith(sch, messages.first(), target, PluginLink::Priority::Reaction,
                         [guard, sch, key, gen, mask](peers::SendResult result) {
                             if (guard)
                                 guard->reactionSent(sch, key, gen, mask, result);
                         },
                         coalesce);
    }
    // SYNC answers that are due.
    for (int i = m_syncAnswers.size() - 1; i >= 0; --i) {
        if (now < m_syncAnswers.at(i).dueMs)
            continue;
        const SyncAnswer answer = m_syncAnswers.takeAt(i);
        if (!ts3::isConnected(answer.sch) || !Settings::instance().showReactions)
            continue;
        // Only our own reactions: nobody speaks for anyone else (no spoofing through relays).
        const QVector<proto::ReactItem> items = m_store->syncAnswer(answer.keys, ownUid(answer.sch));
        if (items.isEmpty())
            continue;
        const QVector<proto::Message> messages = proto::makeReacts(proto::Scope::Channel, true, items);
        for (int m = 0; m < messages.size() && m < kSyncAnswerMessages; ++m)
            m_link->sendWith(answer.sch, messages.at(m), peers::Target::toClients({answer.to}), PluginLink::Priority::Background, {});
    }
}

void PeerHub::reactionSent(quint64 sch, const QString& key, quint64 generation, quint8 mask, peers::SendResult result)
{
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QString::fromLatin1("[test] peer reaction sent sch %1 key %2 mask %3 result %4").arg(sch).arg(key.left(16)).arg(mask).arg(static_cast<int>(result)));
#endif
    if (result == peers::SendResult::Superseded)
        return; // a newer state of the same reaction replaced it before it went out
    if (result == peers::SendResult::LateOk) {
        // It counted as not sent (no answer in time), but it did reach the others: they have mask.
        auto pending = m_pending.find(key);
        if (pending != m_pending.end()) {
            pending->committed = mask; // a newer click is on its way and says the latest anyway
            return;
        }
        // It was undone here when it seemed lost: show what everyone else sees again.
        const QString uid = ownUid(sch);
        if (m_shuttingDown || !ReactionStore::isClientUid(uid) || m_store->maskOf(key, uid) == mask)
            return;
        m_store->setOwn(key, uid, ownName(sch), mask, wallMs());
        ts3::log(LogLevel_INFO, sch, "A reaction that seemed lost arrived after all; it is shown again", {});
        return;
    }
    auto it = m_pending.find(key);
    if (it == m_pending.end())
        return;
    Pending& p = it.value();
    if (generation != p.generation) {
        if (result == peers::SendResult::Ok)
            p.committed = mask; // an older send arrived; the newer one is still on its way
        return;
    }
    p.inFlight = false;
    if (result == peers::SendResult::Ok) {
        p.committed = mask;
        if (p.dueMs < 0)
            m_pending.erase(it);
        return;
    }
    if (p.dueMs >= 0)
        return; // a newer click is about to be sent anyway
    // Back to what the others have, and say why where it happened.
    const quint64 on        = p.sch;
    const quint8  committed = p.committed;
    m_pending.erase(it);
    const QString uid = ownUid(on);
    if (ReactionStore::isClientUid(uid))
        m_store->setOwn(key, uid, ownName(on), committed, wallMs());
    ts3::log(LogLevel_INFO, on, "A reaction couldn't be sent (%1) and was undone", {ts3::pub(static_cast<int>(result))});
    emit reactionFailed(key, sendErrorText(result));
}

void PeerHub::scheduleTimers()
{
    if (m_shuttingDown)
        return;
    qint64 wake = -1;
    for (const Pending& p : qAsConst(m_pending)) {
        if (p.dueMs >= 0)
            wake = wake < 0 ? p.dueMs : qMin(wake, p.dueMs);
    }
    for (const SyncAnswer& a : qAsConst(m_syncAnswers))
        wake = wake < 0 ? a.dueMs : qMin(wake, a.dueMs);
    if (wake < 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(static_cast<int>(qBound<qint64>(0, wake - nowMs(), 60 * 60 * 1000)));
}

void PeerHub::storeChanged(const QString& key)
{
    m_changedKeys.insert(key);
    if (!m_coalesce->isActive() && !m_shuttingDown)
        m_coalesce->start(kCoalesceMs);
    m_dirty = true;
    if (!m_saveTimer->isActive() && !m_shuttingDown)
        m_saveTimer->start(kSaveDelayMs);
}

void PeerHub::save()
{
    if (!m_dirty)
        return;
    if (m_store->save(reactionsPath(), QDateTime::currentSecsSinceEpoch()))
        m_dirty = false;
    else
        ts3::log("Couldn't save reactions.json", LogLevel_WARNING);
}

ReactionView PeerHub::view(const QString& key) const
{
    return m_store->view(key, ownUidFor(key));
}

QString PeerHub::ownUid(quint64 sch) const
{
    if (!sch)
        return {};
    auto it = m_ownUids.constFind(sch);
    if (it != m_ownUids.constEnd())
        return it.value();
    const anyID own = ts3::ownClientId(sch);
    if (!own)
        return {};
    const QString uid = m_env->uid(sch, own);
    if (!uid.isEmpty())
        m_ownUids.insert(sch, uid);
    return uid;
}

QString PeerHub::ownName(quint64 sch) const
{
    return m_env->nickname(sch, ts3::ownClientId(sch));
}

// Usually one identity for every server; with several, the one that reacted to key counts as "you".
// Called for every frame of a playing video with a row: only the cached identities, no TeamSpeak calls.
QString PeerHub::ownUidFor(const QString& key) const
{
    const uint64 current = ts3::currentConnection();
    QString      fallback;
    for (auto it = m_ownUids.constBegin(); it != m_ownUids.constEnd(); ++it) {
        if (m_store->maskOf(key, it.value()) != 0)
            return it.value();
        if (fallback.isEmpty() || it.key() == current)
            fallback = it.value();
    }
    return fallback.isEmpty() ? ownUid(current) : fallback;
}

QString PeerHub::errorText(ReactError error)
{
    switch (error) {
    case ReactError::Off:
        return i18n::t("Reactions are turned off in the TS Media settings.");
    case ReactError::ServerChat:
        return i18n::t("Reactions aren't available in the server chat.");
    case ReactError::NotConnected:
        return i18n::t("Your reaction wasn't sent: you're not connected to this server.");
    case ReactError::Blocked:
        return i18n::t("This server doesn't allow plugin messages, so reactions can't be sent here.");
    case ReactError::PartnerOffline:
        return i18n::t("The person you're chatting with is offline, so your reaction can't be delivered.");
    case ReactError::Unknown:
        return i18n::t("Your reaction wasn't sent.");
    case ReactError::None:
        break;
    }
    return {};
}

QString PeerHub::sendErrorText(peers::SendResult result)
{
    switch (result) {
    case peers::SendResult::Flooded:
        return i18n::t("Your reaction wasn't sent: TeamSpeak is limiting messages right now. Try again in a few seconds.");
    case peers::SendResult::Blocked:
        return errorText(ReactError::Blocked);
    case peers::SendResult::Failed:
        return i18n::t("Your reaction wasn't sent. Check your connection and try again.");
    case peers::SendResult::Ok:
    case peers::SendResult::Superseded:
    case peers::SendResult::LateOk:
        break;
    }
    return {};
}

QString PeerHub::diagnosticsLine(quint64 sch) const
{
    const PluginLink::Counters c       = m_link->counters(sch);
    const QHash<QString, int>  version = m_directory->versions(sch);
    QStringList                parts;
    int                        people  = 0;
    for (auto it = version.constBegin(); it != version.constEnd(); ++it) {
        parts << i18n::t("%1 x%2").arg(it.key()).arg(it.value());
        people += it.value();
    }
    parts.sort();
    return i18n::t("Peers: %1 with TS Media (%2); plugin commands sent %3, ok %4, failed %5 (timeouts %6), received %7, rejected %8, rate-limited %9, "
                   "flood backoffs %10; commands blocked: %11")
        .arg(people)
        .arg(parts.isEmpty() ? i18n::t("none") : parts.join(i18n::t(", ")))
        .arg(c.sent)
        .arg(c.answeredOk)
        .arg(c.failed)
        .arg(c.timeouts)
        .arg(c.received)
        .arg(c.rejected)
        .arg(c.rateLimited)
        .arg(c.floodBackoffs)
        .arg(c.blocked ? i18n::t("yes") : i18n::t("no"));
}

QString PeerHub::diagnosticsTitle()
{
    return i18n::t("Presence and reactions");
}

QStringList PeerHub::diagnosticLines() const
{
    const Settings& s = Settings::instance();
    QStringList     lines;
    lines << i18n::t("Presence shared: %1; reactions shown: %2; reactions stored: %3 media, %4 waiting for their media")
                 .arg(s.sharePresence ? i18n::t("yes") : i18n::t("no"))
                 .arg(s.showReactions ? i18n::t("yes") : i18n::t("no"))
                 .arg(m_store ? m_store->keys().size() : 0)
                 .arg(m_store ? m_store->orphanCount() : 0);
    int number = 0;
    for (const uint64 sch : ts3::connections()) {
        if (!ts3::isConnected(sch))
            continue;
        ++number;
        lines << i18n::t("Connection %1: %2").arg(number).arg(diagnosticsLine(sch));
        if (m_core) {
            const qint64                   now = m_core->floodClockMs();
            const FloodGovernor&           g   = m_core->floodGovernor(sch);
            const FloodGovernor::Counters& f   = g.counters();
            lines << i18n::t("Connection %1 anti-flood: chat posts %2 (flooded %3), plugin commands %4 (flooded %5), other floods %6, "
                             "last pause %7 ms; points now %8 of %9 (chat), %10 of %11 (plugin commands)")
                         .arg(number)
                         .arg(f.postsSent)
                         .arg(f.postFloods)
                         .arg(f.commandsSent)
                         .arg(f.commandFloods)
                         .arg(f.otherFloods)
                         .arg(f.lastPauseMs)
                         .arg(qRound(g.generalPoints(now)))
                         .arg(g.limits().generalCapacity)
                         .arg(qRound(g.pluginPoints(now)))
                         .arg(g.limits().pluginCapacity);
        }
    }
    if (number == 0)
        lines << i18n::t("Not connected to a server");
    return lines;
}

// ---- TeamSpeak callbacks (any thread) -------------------------------------------------------------
// Each holds hubMutex() from reading g_hub until its event is posted (see there).

void PeerHub::onPluginCommand(quint64 sch, const char* pluginName, const char* command, quint16 invoker, const char* invokerName, const char* invokerUid)
{
    // Ours only (pluginName is the DLL name without its platform suffix), and checked before anything
    // of an untrusted buffer is copied.
    if (!pluginName || std::strcmp(pluginName, proto::kPluginName) != 0 || !proto::looksLikeOurs(command))
        return;
    if (!g_hub.load())
        return;
    if (g_queuedCommands.fetch_add(1) >= kMaxQueuedCommands) {
        g_queuedCommands.fetch_sub(1);
        return; // the GUI thread is behind: a flood of commands is dropped here
    }
    const QByteArray            payload(command);
    const QString               name = str(invokerName);
    const QString               uid  = str(invokerUid);
    std::lock_guard<std::mutex> lock(hubMutex());
    PeerHub*                    hub = g_hub.load();
    if (!hub) {
        g_queuedCommands.fetch_sub(1);
        return;
    }
    QMetaObject::invokeMethod(
        hub,
        [sch, payload, invoker, name, uid] {
            g_queuedCommands.fetch_sub(1);
            if (PeerHub* h = g_hub.load())
                h->m_link->onCommand(sch, invoker, uid, name, payload);
        },
        Qt::QueuedConnection);
}

bool PeerHub::onServerError(quint64 sch, unsigned int error, const char* returnCode, const char* extraMessage, bool permissionError)
{
    const QString code = str(returnCode);
    if (code.isEmpty() || !PluginLink::isOwnReturnCode(code))
        return false;
    const QString               extra = str(extraMessage);
    std::lock_guard<std::mutex> lock(hubMutex());
    if (PeerHub* hub = g_hub.load()) {
        QMetaObject::invokeMethod(
            hub,
            [sch, error, code, extra, permissionError] {
                if (PeerHub* h = g_hub.load())
                    h->m_link->onServerError(sch, error, code, extra, permissionError);
            },
            Qt::QueuedConnection);
    }
    return true;
}

void PeerHub::onConnectStatus(quint64 sch, int status)
{
    if (status != STATUS_CONNECTION_ESTABLISHED && status != STATUS_DISCONNECTED)
        return;
    std::lock_guard<std::mutex> lock(hubMutex());
    PeerHub*                    hub = g_hub.load();
    if (!hub)
        return;
    QMetaObject::invokeMethod(
        hub,
        [sch, status] {
            PeerHub* h = g_hub.load();
            if (!h || h->m_shuttingDown)
                return;
            h->m_ownUids.remove(sch);
            if (status == STATUS_CONNECTION_ESTABLISHED) {
                h->ownUid(sch); // cached for drawing
                h->m_directory->connected(sch);
            } else {
                h->m_directory->disconnected(sch);
                h->m_link->connectionLost(sch);
                h->m_syncSentMs.remove(sch);
                h->m_privateMedia.remove(sch);
            }
        },
        Qt::QueuedConnection);
}

void PeerHub::onClientMove(quint64 sch, quint16 client, quint64 oldChannel, quint64 newChannel, int visibility)
{
    Q_UNUSED(visibility); // leaving the view comes with newChannel 0
    std::lock_guard<std::mutex> lock(hubMutex());
    PeerHub*                    hub = g_hub.load();
    if (!hub)
        return;
    QMetaObject::invokeMethod(
        hub,
        [sch, client, oldChannel, newChannel] {
            PeerHub* h = g_hub.load();
            if (h && !h->m_shuttingDown)
                h->m_directory->clientMoved(sch, client, oldChannel, newChannel);
        },
        Qt::QueuedConnection);
}

void PeerHub::onClientRenamed(quint64 sch, quint16 client, const char* displayName)
{
    const QString               name = str(displayName);
    std::lock_guard<std::mutex> lock(hubMutex());
    PeerHub*                    hub = g_hub.load();
    if (!hub)
        return;
    QMetaObject::invokeMethod(
        hub,
        [sch, client, name] {
            PeerHub* h = g_hub.load();
            if (h && !h->m_shuttingDown)
                h->m_directory->clientRenamed(sch, client, name);
        },
        Qt::QueuedConnection);
}
