#include "textreacthub.h"

#include <QDateTime>
#include <QFile>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

#include "emojidata.h"
#include "peers.h"
#include "pluginlink.h"
#include "settings.h"
#include "ts3api.h"

namespace {

TextReactHub* g_textHub = nullptr; // GUI thread only

constexpr int kDebounceMs         = 400;   // a reaction goes out this long after the last click on it
constexpr int kSyncWindowMs       = 10000; // answers to our TSYNC are accepted this long
constexpr int kSyncAnswerGapMs    = 60000; // one answer per person and connection per ...
constexpr int kSyncAnswerMinMs    = 200;
constexpr int kSyncAnswerMaxMs    = 1500;
constexpr int kSyncAnswerMessages = 2;
constexpr int kCoalesceMs         = 50;
constexpr int kSaveDelayMs        = 5000;
constexpr int kWaitCheckMs        = 1000;
constexpr int kWaitTtlMs          = 15000; // a reaction for a message that isn't here waits this long
constexpr int kMaxWaiting         = 64;
constexpr int kMaxStoredMessages  = 1000;

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

ReactionStore::Limits storeLimits()
{
    ReactionStore::Limits limits;
    limits.maxMedia = kMaxStoredMessages;
    return limits;
}

} // namespace

TextReactHub::TextReactHub(Core* core, PluginLink* link, peers::PeerDirectory* directory, QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_link(link)
    , m_directory(directory)
{
    m_store = new ReactionStore(storeLimits(), this);
    m_store->setKeyCheck(&textreact::isLocalKey);

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
    m_waitTimer = new QTimer(this);
    m_waitTimer->setInterval(kWaitCheckMs);
    connect(m_waitTimer, &QTimer::timeout, this, [this] { matchWaiting(); });

    connect(m_store, &ReactionStore::changed, this, &TextReactHub::storeChanged);
    if (m_link)
        connect(m_link, &PluginLink::received, this, &TextReactHub::received);
    if (m_directory)
        connect(m_directory, &peers::PeerDirectory::syncDue, this, &TextReactHub::sendSync);
    if (core) {
        // "Clear cache" forgets reactions on media; the ones on messages go with them.
        connect(core, &Core::cacheCleared, this, [this] {
            m_store->clear();
            m_dirty = false;
            m_saveTimer->stop();
            QFile::remove(reactionsPath());
        });
    }
    g_textHub = this;
}

TextReactHub::~TextReactHub()
{
    if (g_textHub == this)
        g_textHub = nullptr;
}

TextReactHub* TextReactHub::instance()
{
    return g_textHub;
}

QString TextReactHub::reactionsPath() const
{
    return ts3::dataDir() + QLatin1String("/text-reactions.json");
}

qint64 TextReactHub::nowMs() const
{
    return m_core ? m_core->floodClockMs() : wallMs();
}

void TextReactHub::start()
{
    m_store->load(reactionsPath(), wallSec());
    m_dirty = false;
}

void TextReactHub::prepareShutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_timer->stop();
    m_coalesce->stop();
    m_waitTimer->stop();
    // Toggles that never reached anyone are not kept as if they had.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const QString uid = ownUid(it->sch);
        if (ReactionStore::isClientUid(uid) && !proto::sameSet(m_store->setOf(it.key(), uid), it->committed)) {
            m_store->setOwn(it.key(), uid, ownName(it->sch), it->committed, wallMs());
            m_dirty = true;
        }
    }
    m_pending.clear();
    m_syncAnswers.clear();
    m_waiting.clear();
    m_saveTimer->stop();
    save();
}

// ---- receiving ------------------------------------------------------------------------------------

void TextReactHub::received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message)
{
    if (m_shuttingDown || (message.type != "TR" && message.type != "TSYNC"))
        return; // everything else is PeerHub's
    if (!Settings::instance().showReactions)
        return; // off: nothing is shown, stored or answered
    if (isQueryClient(sch, from))
        return;
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QString::fromLatin1("[test] text reactions rx sch %1 from %2 type %3").arg(sch).arg(from).arg(QString::fromLatin1(message.type)));
#endif
    if (message.type == "TR") {
        if (const std::optional<proto::React> react = textreact::readReact(message))
            receiveReact(sch, from, uid, name, *react);
    } else if (const std::optional<proto::Sync> sync = textreact::readSync(message)) {
        receiveSync(sch, from, uid, *sync);
    }
}

void TextReactHub::receiveReact(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::React& react)
{
    if (!ReactionStore::isClientUid(uid))
        return;
    // A channel reaction only from someone in our channel right now; a private one is checked against the
    // message (Index::resolve: the reactor's private chat, a message of theirs or ours).
    if (react.scope == proto::Scope::Channel) {
        const quint64 theirs = channelOf(sch, from);
        if (theirs == 0 || theirs != ts3::ownChannel(sch))
            return;
    }
    if (react.syncAnswer) {
        const auto asked = m_syncSentMs.constFind(sch);
        if (asked == m_syncSentMs.constEnd() || nowMs() - asked.value() > kSyncWindowMs)
            return;
    }
    const QString server = ts3::serverUid(sch);
    const QString own    = ownUid(sch);
    const qint64  now    = wallMs();
    for (const proto::ReactItem& item : react.items) {
        QString hash;
        qint64  age = 0;
        if (!textreact::parseWireRef(item.key.toLatin1(), &hash, &age))
            continue;
        textreact::Index::Query q;
        q.serverUid    = server;
        q.scope        = react.scope;
        q.reactorUid   = uid;
        q.ownUid       = own;
        q.hash         = hash;
        q.estimatedSec = now / 1000 - age;
        const QString key = m_index.resolve(q);
        if (!key.isEmpty()) {
            m_store->applyRemote(server, key, uid, name, item.reactions(), now);
            continue;
        }
        // Not here (yet): a later message from the same reactor on the same message replaces this one.
        for (int i = 0; i < m_waiting.size(); ++i) {
            const Waiting& w = m_waiting.at(i);
            if (w.reactorUid == uid && w.hash == hash && qAbs(w.estimatedSec - q.estimatedSec) <= textreact::kToleranceSec) {
                m_waiting.removeAt(i);
                break;
            }
        }
        if (m_waiting.size() >= kMaxWaiting)
            m_waiting.removeFirst();
        m_waiting.append({server, react.scope, uid, own, name, hash, q.estimatedSec, proto::cleanSet(item.reactions()), now});
    }
    if (!m_waiting.isEmpty() && !m_waitTimer->isActive() && !m_shuttingDown)
        m_waitTimer->start();
}

void TextReactHub::indexChanged()
{
    if (!m_waiting.isEmpty())
        matchWaiting();
}

void TextReactHub::matchWaiting()
{
    const qint64 now = wallMs();
    QVector<Waiting> still;
    for (const Waiting& w : qAsConst(m_waiting)) {
        if (now - w.atMs > kWaitTtlMs || now < w.atMs)
            continue; // expired
        textreact::Index::Query q;
        q.serverUid    = w.serverUid;
        q.scope        = w.scope;
        q.reactorUid   = w.reactorUid;
        q.ownUid       = w.ownUid;
        q.hash         = w.hash;
        q.estimatedSec = w.estimatedSec;
        const QString key = m_index.resolve(q);
        if (key.isEmpty())
            still.append(w);
        else
            m_store->applyRemote(w.serverUid, key, w.reactorUid, w.name, w.set, w.atMs);
    }
    m_waiting = still;
    if (m_waiting.isEmpty())
        m_waitTimer->stop();
}

void TextReactHub::receiveSync(quint64 sch, quint16 from, const QString& uid, const proto::Sync& sync)
{
    if (sync.scope != proto::Scope::Channel || channelOf(sch, from) == 0 || channelOf(sch, from) != ts3::ownChannel(sch) || !ReactionStore::isClientUid(uid))
        return;
    const QString who  = QString::number(sch) + QLatin1Char('/') + uid;
    const qint64  now  = nowMs();
    const auto    last = m_syncAnsweredMs.constFind(who);
    if (last != m_syncAnsweredMs.constEnd() && now - last.value() < kSyncAnswerGapMs)
        return;
    m_syncAnsweredMs.insert(who, now);
    if (m_syncAnsweredMs.size() > 512) { // bounded: answers older than the gap are forgotten
        for (auto it = m_syncAnsweredMs.begin(); it != m_syncAnsweredMs.end();) {
            if (now - it.value() >= kSyncAnswerGapMs)
                it = m_syncAnsweredMs.erase(it);
            else
                ++it;
        }
    }
    const int delay = QRandomGenerator::global()->bounded(kSyncAnswerMinMs, kSyncAnswerMaxMs + 1);
    m_syncAnswers.append({sch, from, uid, sync.keys, now + delay});
    scheduleTimers();
}

void TextReactHub::sendSync(quint64 sch)
{
    if (m_shuttingDown || !m_link || !Settings::instance().showReactions)
        return;
    const qint64      now = wallSec();
    QStringList       refs;
    const QStringList keys = m_index.recentChannelKeys(ts3::serverUid(sch), proto::kMaxItems);
    for (const QString& key : keys) {
        const QByteArray ref = textreact::wireRefOf(key, now);
        if (!ref.isEmpty())
            refs.append(QString::fromLatin1(ref));
    }
    const proto::Message message = textreact::makeSync(proto::Scope::Channel, refs);
    if (!message.has("m"))
        return;
    m_syncSentMs.insert(sch, nowMs());
    QPointer<TextReactHub> guard(this);
    m_link->sendWith(sch, message, peers::Target::toChannel(), PluginLink::Priority::Background, [guard, sch](peers::SendResult result) {
        if (guard && (result == peers::SendResult::Ok || result == peers::SendResult::LateOk))
            guard->m_syncSentMs.insert(sch, guard->nowMs());
    });
}

// ---- your reactions -------------------------------------------------------------------------------

TextReactHub::ReactError TextReactHub::toggle(const QString& key, int reaction, const ChatTarget& target)
{
    if (m_shuttingDown || !emoji::isValid(reaction) || !textreact::isLocalKey(key))
        return ReactError::Unknown;
    PeerHub* peers = PeerHub::instance();
    if (!peers)
        return ReactError::Unknown;
    const ReactError block = peers->reactBlock(target);
    if (block != ReactError::None)
        return block;
    if (textreact::wireRefOf(key, wallSec()).isEmpty())
        return ReactError::Unknown; // older than a month (or from the future): nobody could find it
    const QString uid = ownUid(target.sch);
    if (!ReactionStore::isClientUid(uid))
        return ReactError::NotConnected;

    proto::ReactionSet set = m_store->setOf(key, uid);
    if (set.contains(reaction)) {
        set.removeAll(reaction);
    } else {
        const bool newOnMessage = !m_store->view(key, uid).entry(reaction).count;
        if (!proto::canAdd(set, reaction) || (newOnMessage && m_store->distinctCount(key) >= proto::kMaxDistinctReactions))
            return ReactError::TooMany;
        set.append(reaction);
    }
    auto it = m_pending.find(key);
    if (it == m_pending.end()) {
        it            = m_pending.insert(key, Pending());
        it->committed = m_store->setOf(key, uid);
    }
    it->sch    = target.sch;
    it->target = target;
    m_store->setOwn(key, uid, ownName(target.sch), set, wallMs()); // shown at once
    it->dueMs = nowMs() + kDebounceMs;                              // sent once the clicking stops
    scheduleTimers();
    return ReactError::None;
}

void TextReactHub::flushPending()
{
    if (!m_link)
        return;
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
        Pending& p                   = it.value();
        p.dueMs                      = -1;
        const QString            uid = ownUid(p.sch);
        const proto::ReactionSet set = m_store->setOf(key, uid);
        if (proto::sameSet(set, p.committed) && !p.inFlight) {
            m_pending.erase(it); // clicked back to where it was
            continue;
        }
        // The age is worked out now, right before the command is queued (relative: clocks don't matter).
        const QByteArray              ref         = textreact::wireRefOf(key, wallSec());
        const bool                    privateChat = p.target.mode == TextMessageTarget_CLIENT;
        const proto::Scope            scope       = privateChat ? proto::Scope::Private : proto::Scope::Channel;
        const QVector<proto::Message> messages    = ref.isEmpty() ? QVector<proto::Message>()
                                                                  : textreact::makeReacts(scope, false, {{QString::fromLatin1(ref), proto::legacyMask(set), set}});
        if (messages.isEmpty()) {
            reactionSent(p.sch, key, p.generation, set, peers::SendResult::Failed);
            continue;
        }
        const peers::Target target = privateChat ? peers::Target::toClients({p.target.clientId}) : peers::Target::toChannel();
        const quint64       gen    = ++p.generation;
        p.inFlight                 = true;
        const quint64       sch    = p.sch;
        const QString coalesce     = QString::fromLatin1("TR/%1/%2/%3").arg(sch).arg(privateChat ? p.target.clientId : 0).arg(key);
        QPointer<TextReactHub> guard(this);
        m_link->sendWith(sch, messages.first(), target, PluginLink::Priority::Reaction,
                         [guard, sch, key, gen, set](peers::SendResult result) {
                             if (guard)
                                 guard->reactionSent(sch, key, gen, set, result);
                         },
                         coalesce);
    }
    // TSYNC answers that are due: only our own reactions, each with our own age of the message.
    for (int i = m_syncAnswers.size() - 1; i >= 0; --i) {
        if (now < m_syncAnswers.at(i).dueMs)
            continue;
        const SyncAnswer answer = m_syncAnswers.takeAt(i);
        if (!ts3::isConnected(answer.sch) || !Settings::instance().showReactions)
            continue;
        const QString own    = ownUid(answer.sch);
        const QString server = ts3::serverUid(answer.sch);
        const qint64  nowSec = wallSec();
        QVector<proto::ReactItem> items;
        QStringList               seen;
        for (const QString& refText : answer.refs) {
            QString hash;
            qint64  age = 0;
            if (!textreact::parseWireRef(refText.toLatin1(), &hash, &age))
                continue;
            textreact::Index::Query q;
            q.serverUid    = server;
            q.scope        = proto::Scope::Channel;
            q.reactorUid   = answer.uid;
            q.ownUid       = own;
            q.hash         = hash;
            q.estimatedSec = nowSec - age; // a little late (the answer's delay): within the tolerance
            const QString key = m_index.resolve(q);
            if (key.isEmpty() || seen.contains(key))
                continue;
            seen.append(key);
            const proto::ReactionSet set = m_store->setOf(key, own);
            const QByteArray         ref = textreact::wireRefOf(key, nowSec);
            if (!set.isEmpty() && !ref.isEmpty())
                items.append({QString::fromLatin1(ref), proto::legacyMask(set), set});
        }
        if (items.isEmpty())
            continue;
        const QVector<proto::Message> messages = textreact::makeReacts(proto::Scope::Channel, true, items);
        for (int m = 0; m < messages.size() && m < kSyncAnswerMessages; ++m)
            m_link->sendWith(answer.sch, messages.at(m), peers::Target::toClients({answer.to}), PluginLink::Priority::Background, {});
    }
}

void TextReactHub::reactionSent(quint64 sch, const QString& key, quint64 generation, const proto::ReactionSet& set, peers::SendResult result)
{
#ifdef TSMEDIA_TESTHOOKS
    ts3::log(QString::fromLatin1("[test] text reaction sent sch %1 key %2 set [%3] result %4")
                 .arg(sch)
                 .arg(key.left(16))
                 .arg(QString::fromLatin1(proto::setToEmojiCodes(set)))
                 .arg(static_cast<int>(result)));
#endif
    if (result == peers::SendResult::Superseded)
        return; // a newer state of the same reaction replaced it before it went out
    if (result == peers::SendResult::LateOk) {
        auto pending = m_pending.find(key);
        if (pending != m_pending.end()) {
            pending->committed = set;
            return;
        }
        const QString uid = ownUid(sch);
        if (m_shuttingDown || !ReactionStore::isClientUid(uid) || proto::sameSet(m_store->setOf(key, uid), set))
            return;
        m_store->setOwn(key, uid, ownName(sch), set, wallMs()); // what everyone else sees
        return;
    }
    auto it = m_pending.find(key);
    if (it == m_pending.end())
        return;
    Pending& p = it.value();
    if (generation != p.generation) {
        if (result == peers::SendResult::Ok)
            p.committed = set;
        return;
    }
    p.inFlight = false;
    if (result == peers::SendResult::Ok) {
        p.committed = set;
        if (p.dueMs < 0)
            m_pending.erase(it);
        return;
    }
    if (p.dueMs >= 0)
        return; // a newer click is about to be sent anyway
    const quint64            on        = p.sch;
    const proto::ReactionSet committed = p.committed;
    m_pending.erase(it);
    const QString uid = ownUid(on);
    if (ReactionStore::isClientUid(uid))
        m_store->setOwn(key, uid, ownName(on), committed, wallMs());
    ts3::log(LogLevel_INFO, on, "A reaction on a message couldn't be sent (%1) and was undone", {ts3::pub(static_cast<int>(result))});
    const QString text = PeerHub::sendErrorText(result);
    emit reactionFailed(key, text.isEmpty() ? PeerHub::errorText(ReactError::Unknown) : text);
}

void TextReactHub::scheduleTimers()
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

void TextReactHub::storeChanged(const QString& key)
{
    m_changedKeys.insert(key);
    m_dirty = true;
    if (m_shuttingDown)
        return;
    if (!m_coalesce->isActive())
        m_coalesce->start(kCoalesceMs);
    if (!m_saveTimer->isActive())
        m_saveTimer->start(kSaveDelayMs);
}

void TextReactHub::save()
{
    if (!m_dirty)
        return;
    if (m_store->save(reactionsPath(), wallSec()))
        m_dirty = false;
    else
        ts3::log("Couldn't save text-reactions.json", LogLevel_WARNING);
}

ReactionView TextReactHub::view(const QString& key) const
{
    // Usually one identity for every server; with several, the one that reacted counts as "you".
    const uint64 current = ts3::currentConnection();
    QString      own;
    for (auto it = m_ownUids.constBegin(); it != m_ownUids.constEnd(); ++it) {
        if (!m_store->setOf(key, it.value()).isEmpty()) {
            own = it.value();
            break;
        }
        if (own.isEmpty() || it.key() == current)
            own = it.value();
    }
    if (own.isEmpty())
        own = ownUid(current);
    return m_store->view(key, own);
}

QString TextReactHub::ownUid(quint64 sch) const
{
    if (!sch)
        return {};
    const auto it = m_ownUids.constFind(sch);
    if (it != m_ownUids.constEnd() && ts3::isConnected(sch))
        return it.value();
    const QString uid = ts3::ownUid(sch);
    if (uid.isEmpty())
        m_ownUids.remove(sch);
    else
        m_ownUids.insert(sch, uid);
    return uid;
}

QString TextReactHub::ownName(quint64 sch) const
{
    char* name = nullptr;
    const anyID own = ts3::ownClientId(sch);
    if (!own || !ts3::funcs.getClientVariableAsString || ts3::funcs.getClientVariableAsString(sch, own, CLIENT_NICKNAME, &name) != ERROR_ok)
        return {};
    return ts3::takeString(name);
}
