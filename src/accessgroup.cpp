#include "accessgroup.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QVector>

#include <atomic>

#include "i18n.h"

// The pure job interprets these codes; they must be the SDK's.
static_assert(access::err::ok == ERROR_ok, "error code");
static_assert(access::err::connectionLost == ERROR_connection_lost, "error code");
static_assert(access::err::notConnected == ERROR_not_connected, "error code");
static_assert(access::err::databaseEmptyResult == ERROR_database_empty_result, "error code");
static_assert(access::err::databaseDuplicateEntry == ERROR_database_duplicate_entry, "error code");
static_assert(access::err::permissionDuplicate == ERROR_permission_duplicate_entry, "error code");
static_assert(access::err::permissionEmptyResult == ERROR_permission_empty_result, "error code");
static_assert(access::err::clientInsufficient == ERROR_permissions_client_insufficient, "error code");
static_assert(access::err::insufficientGroupPower == ERROR_permissions_insufficient_group_power, "error code");
static_assert(access::err::insufficientPermPower == ERROR_permissions_insufficient_permission_power, "error code");
static_assert(access::err::fileAlreadyExists == ERROR_file_already_exists, "error code");
static_assert(access::err::fileAlreadyInUse == ERROR_file_already_in_use, "error code");
static_assert(access::kMaxGroupNameLength == TS3_MAX_SIZE_GROUP_NAME, "group name limit");

namespace {

std::atomic<AccessGroup*> g_instance{nullptr};

constexpr int kTimeoutMs       = 10000; // every request
constexpr int kUploadTimeoutMs = 30000; // the icon upload (335 bytes, but a transfer connection of its own)
constexpr int kGraceMs         = 1500;  // a list's rows may follow its return code: wait this long for its end
constexpr int kServerVarsMs    = 5000;  // waiting for VIRTUALSERVER_DEFAULT_SERVER_GROUP
constexpr int kMaxCandidates   = 4;     // renamed-group candidates read per check

QString str(const char* s)
{
    return s ? QString::fromUtf8(s) : QString();
}

QString settingsFile()
{
    return ts3::dataDir() + QLatin1String("/settings.ini");
}

// [accessGroups] <hex of the server UID's UTF-8> = sgid. Built at run time: QSettings keeps its keys
// and values after the DLL is unloaded (see settings.cpp).
QString rememberKey(const QString& uid)
{
    return QString::fromLatin1("accessGroups/") + QString::fromLatin1(uid.toUtf8().toHex());
}

QString iconRoot()
{
    return ts3::dataDir() + QLatin1String("/groupicon");
}

QByteArray utf8Native(const QString& path)
{
    return QDir::toNativeSeparators(path).toUtf8();
}

bool isPermissionList(access::Command::Type type)
{
    return type == access::Command::Type::ReadPerms;
}

template <typename Hash>
void removeValue(Hash& hash, const QString& rc)
{
    for (auto it = hash.begin(); it != hash.end();) {
        if (it.value() == rc)
            it = hash.erase(it);
        else
            ++it;
    }
}

#ifdef TSMEDIA_TESTHOOKS
bool isLocalServer(uint64 sch)
{
    QString host;
    quint16 port = 0;
    return ts3::getServerAddress(sch, &host, &port)
           && (host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1"));
}

void probeLog(const QString& line)
{
    ts3::log(QString::fromLatin1("[test] access %1 %2").arg(QDateTime::currentDateTime().toString(QString::fromLatin1("HH:mm:ss.zzz")), line));
}
#endif

} // namespace

// ============================================================================================
// Lifetime
// ============================================================================================

AccessGroup::AccessGroup(int menuGive, int menuRemove, QObject* parent)
    : QObject(parent)
    , m_menuGive(menuGive)
    , m_menuRemove(menuRemove)
{
    QDir(iconRoot()).removeRecursively(); // leftovers of a previous session
    g_instance.store(this);
}

AccessGroup::~AccessGroup()
{
    g_instance.store(nullptr);
    // Nothing may keep reading from our folder once the plugin is gone.
    for (auto it = m_ops.begin(); it != m_ops.end(); ++it) {
        if (it->transferActive && ts3::funcs.haltTransfer)
            ts3::funcs.haltTransfer(it->sch, it->transferId, 1, nullptr);
        it->transferActive = false;
    }
    QDir(iconRoot()).removeRecursively();
    // The op timers are children of this object and go with it.
}

AccessGroup* AccessGroup::instance()
{
    return g_instance.load();
}

template <typename Fn>
void AccessGroup::post(Fn&& fn)
{
    AccessGroup* self = g_instance.load();
    if (!self)
        return;
    // Posted to this object: pending calls are dropped with it at shutdown. Events posted from one
    // TeamSpeak thread keep their order (also relative to Core's, which live in the same queue).
    QMetaObject::invokeMethod(
        self,
        [fn = std::forward<Fn>(fn)]() mutable {
            if (AccessGroup* a = g_instance.load())
                fn(*a);
        },
        Qt::QueuedConnection);
}

void AccessGroup::setCollecting(List list, uint64 sch, quint64 id, bool on)
{
    QMutexLocker lock(&m_mutex);
    QSet<QPair<uint64, quint64>>& set = list == List::Perms ? m_collectPerms : list == List::Clients ? m_collectClients : m_collectChannels;
    if (on)
        set.insert(qMakePair(sch, id));
    else
        set.remove(qMakePair(sch, id));
}

bool AccessGroup::isCollecting(List list, uint64 sch, quint64 id) const
{
    QMutexLocker lock(&m_mutex);
    const QSet<QPair<uint64, quint64>>& set = list == List::Perms ? m_collectPerms : list == List::Clients ? m_collectClients : m_collectChannels;
    return set.contains(qMakePair(sch, id));
}

// ============================================================================================
// TeamSpeak callbacks (any thread): copy, then continue on the GUI thread
// ============================================================================================

bool AccessGroup::handleServerError(uint64 sch, const char* message, unsigned int error, const char* returnCode, unsigned int failedPermissionId, bool permissionError)
{
    AccessGroup* self = g_instance.load();
    if (!self || !returnCode || !*returnCode)
        return false;
    const QString rc = str(returnCode);
    {
        QMutexLocker lock(&self->m_mutex);
        if (!self->m_returnCodes.contains(rc))
            return false;
    }
    const QString msg = str(message);
    post([sch, rc, error, msg, failedPermissionId, permissionError](AccessGroup& a) {
#ifdef TSMEDIA_TESTHOOKS
        if (a.m_probe)
            probeLog(QString::fromLatin1("answer rc=%1 error=0x%2 perm=%3 failed=%4 msg=%5")
                         .arg(rc, QString::number(error, 16), QString::number(permissionError ? 1 : 0), QString::number(failedPermissionId), msg));
#endif
        Q_UNUSED(sch);
        a.rcAnswered(rc, error, msg, failedPermissionId, permissionError);
    });
    return true;
}

void AccessGroup::handleTransferStatus(anyID transferId, unsigned int status, const char* message, uint64 sch)
{
    const QString msg = str(message);
    post([transferId, status, msg, sch](AccessGroup& a) { a.onTransferStatus(transferId, status, msg, sch); });
}

void AccessGroup::handleConnectStatus(uint64 sch, int status)
{
    post([sch, status](AccessGroup& a) { a.onConnectStatus(sch, status); });
}

void AccessGroup::handleCurrentConnectionChanged(uint64 sch)
{
    Q_UNUSED(sch);
    post([](AccessGroup& a) {
        if (a.m_watching > 0)
            a.refresh(a.displayConnection(), false);
        a.notify();
    });
}

bool AccessGroup::handleMenuItem(uint64 sch, int menuItemId, uint64 selectedItemId)
{
    AccessGroup* self = g_instance.load();
    if (!self || (menuItemId != self->m_menuGive && menuItemId != self->m_menuRemove))
        return false;
    const bool give = menuItemId == self->m_menuGive;
    if (selectedItemId == 0 || selectedItemId > 0xFFFF) // a client id (anyID)
        return true;
    const anyID client = static_cast<anyID>(selectedItemId);
    post([sch, client, give](AccessGroup& a) { a.giveOrTake(sch, client, give); });
    return true;
}

void AccessGroup::handleServerGroupList(uint64 sch, uint64 sgid, const char* name, int type, int iconId)
{
    access::GroupInfo g;
    g.id     = sgid;
    g.name   = str(name).left(access::kMaxGroupNameLength * 4); // bounded: the server limits names to 30 characters
    g.type   = type;
    g.iconId = static_cast<quint32>(iconId);
    post([sch, g](AccessGroup& a) { a.onServerGroupList(sch, g); });
}

void AccessGroup::handleServerGroupListFinished(uint64 sch)
{
    post([sch](AccessGroup& a) { a.onServerGroupListFinished(sch); });
}

void AccessGroup::handleServerGroupPermList(uint64 sch, uint64 sgid, unsigned int permissionId, int value, int negated, int skip)
{
    AccessGroup* self = g_instance.load();
    if (!self || !self->isCollecting(List::Perms, sch, sgid))
        return; // TeamSpeak's own permission windows read lists too
    access::PermValue v;
    v.value   = value;
    v.negated = negated != 0;
    v.skip    = skip != 0;
    post([sch, sgid, permissionId, v](AccessGroup& a) { a.onServerGroupPermList(sch, sgid, permissionId, v); });
}

void AccessGroup::handleServerGroupPermListFinished(uint64 sch, uint64 sgid)
{
    AccessGroup* self = g_instance.load();
    if (!self || !self->isCollecting(List::Perms, sch, sgid))
        return;
    post([sch, sgid](AccessGroup& a) { a.onServerGroupPermListFinished(sch, sgid); });
}

void AccessGroup::handleServerGroupClientList(uint64 sch, uint64 sgid, uint64 clientDatabaseId)
{
    AccessGroup* self = g_instance.load();
    if (!self || !self->isCollecting(List::Clients, sch, sgid))
        return;
    post([sch, sgid, clientDatabaseId](AccessGroup& a) {
        const QString rc = a.m_clientListOps.value(qMakePair(sch, sgid));
        auto          it = a.m_ops.find(rc);
        if (!rc.isEmpty() && it != a.m_ops.end() && !it->done && it->members.size() < 100000)
            it->members.insert(clientDatabaseId);
    });
}

void AccessGroup::handleServerGroupClientChanged(uint64 sch, anyID clientId, uint64 sgid, bool added)
{
    Q_UNUSED(clientId);
    post([sch, sgid, added](AccessGroup& a) {
        const QString uid = a.uidOf(sch);
        auto          it  = a.m_servers.find(uid);
        if (uid.isEmpty() || it == a.m_servers.end() || it->adopted != sgid)
            return;
        if (it->members >= 0)
            it->members = qMax(0, it->members + (added ? 1 : -1));
        a.notify(); // also your own membership
    });
}

void AccessGroup::handleNeededPermission(uint64 sch, unsigned int permissionId, int value)
{
    post([sch, permissionId, value](AccessGroup& a) {
        Connection& c = a.connection(sch);
        if (c.neededPending.size() < 4096)
            c.neededPending.insert(permissionId, value);
    });
}

void AccessGroup::handleNeededPermissionsFinished(uint64 sch)
{
    post([sch](AccessGroup& a) {
        Connection& c = a.connection(sch);
        // A permission is known only if the server listed it; the fresh list replaces the old one.
        c.needed      = c.neededPending;
        c.neededKnown = true;
        c.neededPending.clear();
#ifdef TSMEDIA_TESTHOOKS
        if (a.m_probe) {
            QStringList values;
            for (auto it = c.needed.cbegin(); it != c.needed.cend(); ++it)
                values << QString::fromLatin1("%1=%2").arg(a.permissionName(sch, it.key()), QString::number(it.value()));
            probeLog(QString::fromLatin1("needed permissions (%1): %2").arg(QString::number(c.needed.size()), values.join(QLatin1Char(' '))));
        }
#endif
        a.notify();
    });
}

void AccessGroup::handleChannelPermList(uint64 sch, uint64 channelId, unsigned int permissionId, int value)
{
    AccessGroup* self = g_instance.load();
    if (!self || !self->isCollecting(List::Channel, sch, channelId))
        return;
    post([sch, channelId, permissionId, value](AccessGroup& a) {
        const QString rc = a.m_channelOps.value(qMakePair(sch, channelId));
        auto          it = a.m_ops.find(rc);
        if (rc.isEmpty() || it == a.m_ops.end() || it->done)
            return;
        access::PermValue v;
        v.value = value;
        it->answer.rows.insert(a.permissionName(sch, permissionId), v);
    });
}

void AccessGroup::handleChannelPermListFinished(uint64 sch, uint64 channelId)
{
    AccessGroup* self = g_instance.load();
    if (!self || !self->isCollecting(List::Channel, sch, channelId))
        return;
    post([sch, channelId](AccessGroup& a) {
        const QString rc = a.m_channelOps.value(qMakePair(sch, channelId));
        auto          it = a.m_ops.find(rc);
        if (rc.isEmpty() || it == a.m_ops.end() || it->done)
            return;
        it->listFinished = true;
        it->answer.ok    = true;
        a.complete(rc);
    });
}

void AccessGroup::handleFileInfo(uint64 sch, uint64 channelId, const char* name, uint64 size)
{
    if (channelId != 0)
        return; // the icon lives at the server level (channel 0)
    const QString file = str(name);
    post([sch, file, size](AccessGroup& a) {
        const QString rc = a.m_fileInfoOps.value(sch);
        auto          it = a.m_ops.find(rc);
        if (rc.isEmpty() || it == a.m_ops.end() || it->done || !file.endsWith(access::iconFileName()))
            return;
        it->answer.fileSize = static_cast<qint64>(qMin<uint64>(size, 1u << 30));
        if (it->answer.ok) // the return code came first
            a.complete(rc);
    });
}

void AccessGroup::handlePermissionList(uint64 sch, unsigned int permissionId, const char* name)
{
    const QString n = str(name);
    if (n.isEmpty() || n.size() > 128)
        return;
    post([sch, permissionId, n](AccessGroup& a) {
        Connection& c = a.connection(sch);
        if (c.permIds.size() < 8192)
            c.permIds.insert(n, permissionId);
    });
}

void AccessGroup::handleServerUpdated(uint64 sch)
{
    post([sch](AccessGroup& a) {
        const QString uid = a.uidOf(sch);
        auto          it  = a.m_servers.find(uid);
        if (uid.isEmpty() || it == a.m_servers.end() || !it->waitingForServerVars || !a.defaultGroupOf(sch))
            return;
        it->waitingForServerVars = false;
        a.startJob(sch, it->jobStarted);
    });
}

// ============================================================================================
// Lookups (GUI thread)
// ============================================================================================

AccessGroup::Connection& AccessGroup::connection(uint64 sch)
{
    return m_connections[sch];
}

AccessGroup::Server& AccessGroup::server(const QString& uid)
{
    return m_servers[uid];
}

const AccessGroup::Server* AccessGroup::findServer(const QString& uid) const
{
    auto it = m_servers.constFind(uid);
    return it == m_servers.cend() ? nullptr : &it.value();
}

QString AccessGroup::uidOf(uint64 sch) const
{
    if (!sch)
        return {};
    const QString uid = ts3::serverUid(sch);
    if (!uid.isEmpty())
        return uid;
    auto it = m_connections.constFind(sch);
    return it == m_connections.cend() ? QString() : it->uid;
}

unsigned int AccessGroup::permissionId(uint64 sch, const QString& name) const
{
    if (name.startsWith(QLatin1Char('#'))) { // a permission the client couldn't name (see permissionName)
        bool               ok = false;
        const unsigned int id = name.mid(1).toUInt(&ok);
        return ok ? id : 0;
    }
    unsigned int id = 0;
    if (ts3::funcs.getPermissionIDByName && ts3::funcs.getPermissionIDByName(sch, name.toUtf8().constData(), &id) == ERROR_ok && id)
        return id;
    auto it = m_connections.constFind(sch);
    return it == m_connections.cend() ? 0 : it->permIds.value(name);
}

QString AccessGroup::permissionName(uint64 sch, unsigned int id)
{
    char buffer[256] = {};
    if (ts3::funcs.getPermissionNameByID && ts3::funcs.getPermissionNameByID(sch, id, buffer, sizeof(buffer)) == ERROR_ok && buffer[0]) {
        const QString name = QString::fromUtf8(buffer);
        connection(sch).permIds.insert(name, id);
        return name;
    }
    const Connection& c = connection(sch);
    for (auto it = c.permIds.cbegin(); it != c.permIds.cend(); ++it) {
        if (it.value() == id)
            return it.key();
    }
    return QString::fromLatin1("#%1").arg(id); // never conformant, still sendable by id
}

std::optional<int> AccessGroup::neededValue(uint64 sch, const char* name) const
{
    const QString n  = QString::fromLatin1(name);
    auto          it = m_connections.constFind(sch);
    if (it != m_connections.cend() && it->neededKnown) {
        const unsigned int id = permissionId(sch, n);
        auto               v  = it->needed.constFind(id);
        if (id && v != it->needed.cend())
            return v.value();
        return std::nullopt; // not listed: unknown, never "0"
    }
    // Plugin enabled mid-session, so the list was missed: the client's own value, where 0 means unknown.
    int value = 0;
    if (ts3::funcs.getClientNeededPermission && ts3::funcs.getClientNeededPermission(sch, name, &value) == ERROR_ok && value != 0)
        return value;
    return std::nullopt;
}

quint64 AccessGroup::defaultGroupOf(uint64 sch) const
{
    uint64 value = 0;
    if (!sch || ts3::funcs.getServerVariableAsUInt64(sch, VIRTUALSERVER_DEFAULT_SERVER_GROUP, &value) != ERROR_ok)
        return 0;
    return value;
}

QString AccessGroup::groupNameOf(uint64 sch, quint64 sgid) const
{
    if (!sgid)
        return {};
    auto c = m_connections.constFind(sch);
    if (c != m_connections.cend()) {
        auto g = c->groups.constFind(sgid);
        if (g != c->groups.cend() && !g->name.isEmpty())
            return g->name;
    }
    char buffer[256] = {};
    if (ts3::funcs.getServerGroupNameByID && sgid <= 0xFFFFFFFFu
        && ts3::funcs.getServerGroupNameByID(sch, static_cast<unsigned int>(sgid), buffer, sizeof(buffer)) == ERROR_ok)
        return QString::fromUtf8(buffer);
    return {};
}

QList<access::GroupInfo> AccessGroup::groupList(uint64 sch) const
{
    auto c = m_connections.constFind(sch);
    return c == m_connections.cend() ? QList<access::GroupInfo>() : c->groups.values();
}

quint64 AccessGroup::rememberedGroup(const QString& uid) const
{
    if (uid.isEmpty())
        return 0;
    const QSettings s(settingsFile(), QSettings::IniFormat);
    return s.value(rememberKey(uid)).toULongLong();
}

void AccessGroup::rememberGroup(const QString& uid, quint64 sgid)
{
    if (uid.isEmpty() || !sgid || rememberedGroup(uid) == sgid)
        return;
    QSettings s(settingsFile(), QSettings::IniFormat);
    s.setValue(rememberKey(uid), QString::number(sgid)); // heap-allocated key and value
    s.sync();
}

bool AccessGroup::isMember(uint64 sch, quint64 sgid) const
{
    const anyID me = ts3::ownClientId(sch);
    char*       groups = nullptr;
    if (!me || !sgid || ts3::funcs.getClientVariableAsString(sch, me, CLIENT_SERVERGROUPS, &groups) != ERROR_ok)
        return false;
    return access::parseServerGroups(ts3::takeString(groups)).contains(sgid);
}

access::OwnPowers AccessGroup::ownPowers(uint64 sch) const
{
    access::OwnPowers own;
    own.memberAdd    = neededValue(sch, "i_group_member_add_power");
    own.memberRemove = neededValue(sch, "i_group_member_remove_power");
    own.groupModify  = neededValue(sch, "i_group_modify_power");
    return own;
}

access::Plan AccessGroup::planFor(uint64 sch, const Server& s) const
{
    return access::plan(s.defaultRows, ownPowers(sch));
}

quint64 AccessGroup::groupFor(uint64 sch) const
{
    const Server* s = findServer(uidOf(sch));
    if (s && s->adopted)
        return s->adopted;
    // Not checked yet (the box wasn't opened): a regular group named tsmediachat, never a guess by icon.
    return access::findCandidates(groupList(sch), defaultGroupOf(sch), 0).byName;
}

// ============================================================================================
// Return codes
// ============================================================================================

QString AccessGroup::newOp(Op op, int timeoutMs)
{
    const QString rc = ts3::newReturnCode();
    {
        QMutexLocker lock(&m_mutex);
        m_returnCodes.insert(rc); // never pruned: late duplicates of an answer stay ours (and unprinted)
    }
    op.timer = new QTimer(this);
    op.timer->setSingleShot(true);
    op.timer->setInterval(timeoutMs);
    connect(op.timer, &QTimer::timeout, this, [this, rc] {
        auto it = m_ops.find(rc);
        if (it == m_ops.end() || it->done)
            return;
        it->answer = access::Answer::timedOut();
        complete(rc);
    });
    op.timer->start();
    m_ops.insert(rc, op);
    return rc;
}

void AccessGroup::finishLater(const QString& rc, const access::Answer& answer)
{
    // Not from inside the request call: the job may hand out its next command right away.
    QTimer::singleShot(0, this, [this, rc, answer] {
        auto it = m_ops.find(rc);
        if (it == m_ops.end() || it->done)
            return;
        it->answer = answer;
        complete(rc);
    });
}

void AccessGroup::rcAnswered(const QString& rc, unsigned int error, const QString& message, unsigned int failedPermissionId, bool permissionError)
{
    auto it = m_ops.find(rc);
    if (it == m_ops.end())
        return;
    if (failedPermissionId) {
        const QString failed = permissionName(it->sch, failedPermissionId);
        it = m_ops.find(rc); // permissionName() may have touched m_connections, not m_ops; re-find anyway
        if (!it->answer.failedPerms.contains(failed))
            it->answer.failedPerms << failed;
        if (it->kind == OpKind::Job && it->command == access::Command::Type::AddPerms) {
            // Refused this session: listed as "needs higher server permissions", never sent again.
            Server& s = server(it->uid);
            s.refused[it->sgid].insert(failed);
            if (s.job && s.jobId == it->jobId)
                s.job->noteRefused(failed);
        }
    }
    if (it->done)
        return; // a later event for the same request only adds refused permissions

    const bool ok = error == ERROR_ok && !permissionError;
    if (!ok) {
        it->answer.ok      = false;
        it->answer.error   = error != ERROR_ok ? error : static_cast<unsigned int>(ERROR_permissions_client_insufficient);
        it->answer.message = message;
        complete(rc);
        return;
    }
    it->answer.ok = true;
    const bool isList = (it->kind == OpKind::Job && (isPermissionList(it->command) || it->command == access::Command::Type::ListGroups))
                        || it->kind == OpKind::CheckDefault || it->kind == OpKind::CheckGroup || it->kind == OpKind::CheckCandidate
                        || it->kind == OpKind::CheckChannel || it->kind == OpKind::CheckGroups;
    const bool fileInfo = it->kind == OpKind::Job && it->command == access::Command::Type::CheckIconFile;
    if (it->kind == OpKind::Job && it->command == access::Command::Type::UploadIcon)
        return; // done when the transfer reports it
    if ((isList && !it->listFinished) || (fileInfo && it->answer.fileSize < 0)) {
        // The rows may still be on their way: wait a moment for the list's end.
        if (!it->grace) {
            it->grace = new QTimer(this);
            it->grace->setSingleShot(true);
            connect(it->grace, &QTimer::timeout, this, [this, rc] { complete(rc); });
            it->grace->start(kGraceMs);
        }
        return;
    }
    complete(rc);
}

void AccessGroup::complete(const QString& rc)
{
    auto it = m_ops.find(rc);
    if (it == m_ops.end() || it->done)
        return;
    it->done = true;
    for (QTimer* t : {it->timer, it->grace}) {
        if (t) {
            t->stop();
            t->deleteLater();
        }
    }
    it->timer = nullptr;
    it->grace = nullptr;
    if (it->transferActive) {
        if (!it->answer.ok && ts3::funcs.haltTransfer) // timed out or the connection went: stop reading our file
            ts3::funcs.haltTransfer(it->sch, it->transferId, 1, nullptr);
        it->transferActive = false;
        m_uploads.remove(it->transferId); // transfer ids are reused
    }
    for (auto key = m_permListOps.begin(); key != m_permListOps.end();) {
        if (key.value() == rc) {
            setCollecting(List::Perms, key.key().first, key.key().second, false);
            key = m_permListOps.erase(key);
        } else {
            ++key;
        }
    }
    for (auto key = m_clientListOps.begin(); key != m_clientListOps.end();) {
        if (key.value() == rc) {
            setCollecting(List::Clients, key.key().first, key.key().second, false);
            key = m_clientListOps.erase(key);
        } else {
            ++key;
        }
    }
    for (auto key = m_channelOps.begin(); key != m_channelOps.end();) {
        if (key.value() == rc) {
            setCollecting(List::Channel, key.key().first, key.key().second, false);
            key = m_channelOps.erase(key);
        } else {
            ++key;
        }
    }
    removeValue(m_groupListOps, rc);
    removeValue(m_fileInfoOps, rc);

    const Op op = it.value();
    it->answer.rows.clear(); // the record stays (late events), the data goes
    it->members.clear();

    switch (op.kind) {
    case OpKind::Job:
        onJobAnswer(op);
        break;
    case OpKind::Member:
        onMemberAnswer(op);
        break;
    case OpKind::Ignore:
        break;
    default:
        onCheckAnswer(op);
        break;
    }
}

// ============================================================================================
// Events (GUI thread)
// ============================================================================================

void AccessGroup::onServerGroupList(uint64 sch, const access::GroupInfo& group)
{
    if (!group.id)
        return;
    Connection& c = connection(sch);
    if (c.uid.isEmpty())
        c.uid = ts3::serverUid(sch);
    if (c.groups.size() >= 4096 && !c.groups.contains(group.id))
        return; // bounded: no real server has this many groups
    c.groups.insert(group.id, group);
    c.pass.insert(group.id);
#ifdef TSMEDIA_TESTHOOKS
    if (m_probe)
        probeLog(QString::fromLatin1("group %1 type %2 icon %3 name %4").arg(QString::number(group.id), QString::number(group.type), QString::number(group.iconId), group.name));
#endif
}

void AccessGroup::onServerGroupListFinished(uint64 sch)
{
    Connection& c = connection(sch);
    if (!c.pass.isEmpty()) { // a complete list: groups that weren't in it are gone
        for (auto it = c.groups.begin(); it != c.groups.end();) {
            if (c.pass.contains(it.key()))
                ++it;
            else
                it = c.groups.erase(it);
        }
    }
    c.pass.clear();
    c.groupsComplete = true;
#ifdef TSMEDIA_TESTHOOKS
    if (m_probe)
        probeLog(QString::fromLatin1("group list finished: %1 groups").arg(c.groups.size()));
#endif
    const QString rc = m_groupListOps.value(sch);
    if (!rc.isEmpty()) {
        auto it = m_ops.find(rc);
        if (it != m_ops.end() && !it->done) {
            it->listFinished = true;
            it->answer.ok    = true;
            complete(rc);
        }
    }
    onGroupsChanged(sch);
}

// The group list changed: a group may have appeared, been renamed or deleted.
void AccessGroup::onGroupsChanged(uint64 sch)
{
    const QString uid = uidOf(sch);
    auto          it  = m_servers.find(uid);
    if (uid.isEmpty() || it == m_servers.end() || it->job || it->checking) {
        notify();
        return;
    }
    const Connection&  c          = connection(sch);
    const quint64      byName     = access::findCandidates(c.groups.values(), defaultGroupOf(sch), 0).byName;
    const bool         vanished   = it->adopted && !it->displayOnly && !c.groups.contains(it->adopted);
    const bool         newName    = byName && byName != it->adopted;
    if (vanished) {
        it->adopted   = 0;
        it->rowsKnown = false;
        it->rows.clear();
        it->members = -1;
    }
    if ((vanished || newName) && m_watching > 0 && sch == displayConnection())
        refresh(sch, false);
    notify();
}

void AccessGroup::onServerGroupPermList(uint64 sch, quint64 sgid, unsigned int permissionId, const access::PermValue& value)
{
    const QString rc = m_permListOps.value(qMakePair(sch, sgid));
    auto          it = m_ops.find(rc);
    if (rc.isEmpty() || it == m_ops.end() || it->done || it->answer.rows.size() >= 4096)
        return;
    const QString name = permissionName(sch, permissionId);
    it                 = m_ops.find(rc);
    it->answer.rows.insert(name, value);
#ifdef TSMEDIA_TESTHOOKS
    if (m_probe)
        probeLog(QString::fromLatin1("perm row sgid %1 %2=%3 neg %4 skip %5")
                     .arg(QString::number(sgid), name, QString::number(value.value), QString::number(value.negated), QString::number(value.skip)));
#endif
}

void AccessGroup::onServerGroupPermListFinished(uint64 sch, quint64 sgid)
{
    const QString rc = m_permListOps.value(qMakePair(sch, sgid));
    auto          it = m_ops.find(rc);
    if (rc.isEmpty() || it == m_ops.end() || it->done)
        return;
#ifdef TSMEDIA_TESTHOOKS
    if (m_probe)
        probeLog(QString::fromLatin1("perm list finished sgid %1 (%2 rows)").arg(QString::number(sgid), QString::number(it->answer.rows.size())));
#endif
    it->listFinished = true;
    it->answer.ok    = true;
    complete(rc);
}

void AccessGroup::onConnectStatus(uint64 sch, int status)
{
    if (status == STATUS_DISCONNECTED) {
        failJobsOn(sch);
        QStringList open;
        for (auto it = m_ops.cbegin(); it != m_ops.cend(); ++it) {
            if (it->sch == sch && !it->done)
                open << it.key();
        }
        for (const QString& rc : qAsConst(open)) {
            auto it = m_ops.find(rc);
            if (it == m_ops.end() || it->done)
                continue;
            it->answer = access::Answer::failure(ERROR_not_connected, ts3::errorText(ERROR_not_connected));
            complete(rc);
        }
        m_connections.remove(sch); // the next connection of this tab may be another server
    } else if (status == STATUS_CONNECTION_ESTABLISHED) {
        connection(sch).uid = ts3::serverUid(sch);
#ifdef TSMEDIA_TESTHOOKS
        QTimer::singleShot(8000, this, [this, sch] { selfTest(sch); });
#endif
        if (m_watching > 0 && sch == displayConnection())
            refresh(sch, false);
    }
    notify();
}

void AccessGroup::onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch)
{
    const QString rc = m_uploads.value(transferId);
    auto          it = m_ops.find(rc);
    if (rc.isEmpty() || it == m_ops.end() || it->sch != sch || it->done)
        return;
    m_uploads.remove(transferId);
    it->transferActive = false;
    if (status == ERROR_file_transfer_complete) {
        it->answer.ok = true;
    } else {
        it->answer.ok      = false;
        it->answer.error   = status; // "already exists" and "in use" count as done (the job decides)
        it->answer.message = message;
    }
    complete(rc);
}

// ============================================================================================
// The settings box
// ============================================================================================

uint64 AccessGroup::displayConnection() const
{
    auto latest = m_servers.constFind(m_lastJobUid);
    if (latest != m_servers.cend() && latest->job && ts3::isConnected(latest->jobSch))
        return latest->jobSch;
    for (auto it = m_servers.cbegin(); it != m_servers.cend(); ++it) {
        if (it->job && ts3::isConnected(it->jobSch))
            return it->jobSch;
    }
    return ts3::currentConnection();
}

QString AccessGroup::serverName(uint64 sch) const
{
    return ts3::isConnected(sch) ? ts3::serverName(sch) : QString();
}

access::ViewInput AccessGroup::view(uint64 sch) const
{
    access::ViewInput in;
    access::Facts     f;
    f.connected = ts3::isConnected(sch);
    if (!f.connected) {
        in.state = access::State::NotConnected;
        return in;
    }
    const auto create = neededValue(sch, "b_virtualserver_servergroup_create");
    f.canCreate       = !(create && *create == 0); // only a listed 0 disables Create
    const QString uid = uidOf(sch);
    const Server* s   = findServer(uid);
    // Regular users get one line: they can't create the group and can't manage an existing one.
    in.compact        = !f.canCreate && !(s && s->rowsKnown && !s->displayOnly);
    in.group          = access::groupName();
    in.defaultGroup   = groupNameOf(sch, s && s->defaultSgid ? s->defaultSgid : defaultGroupOf(sch));
    if (!s) {
        in.state = access::classify(f);
        return in;
    }

    f.checked     = s->checked;
    f.checking    = s->checking && (!s->checked || s->explicitCheck);
    f.jobRunning  = s->job != nullptr || s->waitingForServerVars;
    f.error       = s->error;
    f.groupFound  = s->adopted != 0;
    f.canManage   = s->rowsKnown && !s->displayOnly;
    f.isMember    = s->adopted && isMember(sch, s->adopted);
    f.isProtected = access::isProtected(s->rows);
    const access::Plan plan = planFor(sch, *s);
    f.missing     = access::missing(s->rows, plan, s->refused.value(s->adopted));
    f.iconProblem = s->iconProblem;
    in.state      = access::classify(f);

    if (s->adopted) {
        const QString name = groupNameOf(sch, s->adopted);
        if (!name.isEmpty())
            in.group = name;
    }
    if (s->channel) {
        in.channel = ts3::channelName(sch, s->channel);
    }
    in.members = s->members;
    const auto upload = s->rows.constFind(QString::fromLatin1("i_ft_file_upload_power"));
    if (upload != s->rows.cend() && upload->value > 0)
        in.uploadPower = upload->value;
    in.channelNeededUpload = s->channelNeededUpload;
    const auto defaultUpload = s->defaultRows.constFind(QString::fromLatin1("i_ft_file_upload_power"));
    const int  defaultPower  = defaultUpload != s->defaultRows.cend() && !defaultUpload->negated ? defaultUpload->value : 0;
    in.defaultCanUploadHere  = s->defaultKnown && s->channelNeededUpload >= 0 && defaultPower > 0 && defaultPower >= s->channelNeededUpload;
    in.protection            = plan.protectionValue();
    if (s->job) {
        in.job      = s->job->kind();
        in.step     = s->job->step();
        in.steps    = s->job->steps();
        in.stepKind = s->job->stepKind();
    } else if (s->waitingForServerVars) {
        in.job      = s->jobStarted;
        in.step     = 1;
        in.steps    = s->jobStarted == access::JobKind::Create ? 4 : 3;
        in.stepKind = s->jobStarted == access::JobKind::Create ? access::StepKind::CreateGroup : access::StepKind::AddPermissions;
    }
    in.iconProblem  = s->iconProblem;
    in.iconDetail   = s->iconDetail;
    in.maxIconSize  = s->maxIconSize;
    in.missing      = f.missing;
    in.error        = s->error;
    in.errorContext = s->errorContext;
    in.errorDetail  = s->errorDetail;
    return in;
}

access::Details AccessGroup::details(uint64 sch) const
{
    access::DetailsInput in;
    in.server       = serverName(sch);
    const Server* s = findServer(uidOf(sch));
    in.defaultGroup = groupNameOf(sch, s && s->defaultSgid ? s->defaultSgid : defaultGroupOf(sch));
    if (s) {
        in.create    = s->adopted == 0;
        in.plan      = planFor(sch, *s);
        in.planKnown = s->defaultKnown;
        in.groupRows = s->rows;
        in.rowsKnown = s->rowsKnown;
        in.refused   = s->refused.value(s->adopted);
        in.ignored   = s->ignored;
        in.sameName  = s->sameName;
        in.adopted   = s->adopted;
    } else {
        in.plan = access::plan({}, ownPowers(sch));
    }
    in.status = access::buildView(view(sch)).status;
    return access::buildDetails(in);
}

void AccessGroup::setWatching(bool watching)
{
    m_watching = qMax(0, m_watching + (watching ? 1 : -1));
    if (watching && m_watching == 1)
        refresh(displayConnection(), false);
}

void AccessGroup::run(uint64 sch, access::Action action)
{
    if (!ts3::isConnected(sch))
        return;
    const QString uid = uidOf(sch);
    if (uid.isEmpty())
        return;
    Server& s = server(uid);
    if (s.checking && action != access::Action::RetryCheck) {
        s.queued    = action; // runs when the check ends (a click never waits on a disabled button)
        s.queuedSch = sch;
        notify();
        return;
    }
    switch (action) {
    case access::Action::None:
        break;
    case access::Action::Create:
    case access::Action::RetryCreate:
        startJob(sch, access::JobKind::Create);
        break;
    case access::Action::Repair:
    case access::Action::RetryRepair:
        startJob(sch, access::JobKind::Repair);
        break;
    case access::Action::RetryCheck:
        refresh(sch, true);
        break;
    }
}

// ============================================================================================
// Checking a server
// ============================================================================================

void AccessGroup::refresh(uint64 sch, bool explicitCheck)
{
    if (!ts3::isConnected(sch)) {
        notify();
        return;
    }
    const QString uid = uidOf(sch);
    if (uid.isEmpty()) {
        notify();
        return;
    }
    Server& s = server(uid);
    if (explicitCheck) {
        s.error         = access::ErrorKind::None; // Check again / Try again: a fresh look
        s.explicitCheck = true;
    }
    if (s.job || s.waitingForServerVars) {
        notify();
        return;
    }
    if (s.checking) {
        s.recheck = true;
        notify();
        return;
    }
    startCheck(sch, s, uid);
}

void AccessGroup::startCheck(uint64 sch, Server& s, const QString& uid)
{
    s.checking     = true;
    s.checkFailed  = false;
    s.pendingReads = 0;
    s.candidateRows.clear();
    ++s.generation;
    s.defaultSgid = defaultGroupOf(sch);
    Connection& c = connection(sch);
    if (!s.defaultSgid && !c.serverVarsRequested && ts3::funcs.requestServerVariables) {
        c.serverVarsRequested = true;
        ts3::funcs.requestServerVariables(sch); // the next check (or the job) sees the value
    }
    if (c.groups.isEmpty()) {
        // Enabled mid-session: TeamSpeak's connect-time list was missed.
        Op op;
        op.kind       = OpKind::CheckGroups;
        op.sch        = sch;
        op.uid        = uid;
        op.generation = s.generation;
        const QString rc = newOp(op);
        m_groupListOps.insert(sch, rc);
        ++s.pendingReads;
        const unsigned int err = ts3::funcs.requestServerGroupList(sch, rc.toUtf8().constData());
        if (err != ERROR_ok)
            finishLater(rc, access::Answer::failure(err, ts3::errorText(err)));
        notify();
        return;
    }
    continueCheck(sch, uid, s.generation);
    notify();
}

void AccessGroup::continueCheck(uint64 sch, const QString& uid, quint64 generation)
{
    auto it = m_servers.find(uid);
    if (it == m_servers.end() || it->generation != generation)
        return;
    Server&                  s      = it.value();
    const Connection&        c      = connection(sch);
    const access::Candidates found  = access::findCandidates(c.groups.values(), s.defaultSgid, rememberedGroup(uid));
    s.sameName.clear();
    for (quint64 id : found.sameName)
        s.sameName << c.groups.value(id);
    s.ignored.clear();
    s.displayOnly = false;

    if (found.byName) {
        if (s.adopted != found.byName) {
            s.rowsKnown = false;
            s.rows.clear();
            s.members = -1;
        }
        s.adopted = found.byName;
        s.how     = access::Match::Name;
    } else if (c.groups.isEmpty()) {
        // No list and none could be read: the client's name lookup, for display only. It ignores the
        // group type, so it never leads to a repair.
        unsigned int id = 0;
        const QByteArray name = access::groupName().toUtf8();
        if (ts3::funcs.getServerGroupIDByName && ts3::funcs.getServerGroupIDByName(sch, name.constData(), &id) == ERROR_ok && id) {
            s.adopted     = id;
            s.how         = access::Match::Name;
            s.displayOnly = true;
        } else {
            s.adopted = 0;
        }
        s.rowsKnown = false;
        s.rows.clear();
    } else if (s.how != access::Match::Name && s.adopted && found.fallback.contains(s.adopted)) {
        // Adopted after a rename: checked for conformance again below, like any candidate.
        s.adopted = 0;
    } else {
        s.adopted   = 0;
        s.rowsKnown = false;
        s.rows.clear();
    }

    const auto list    = neededValue(sch, "b_virtualserver_servergroup_permission_list");
    const bool canList = !(list && *list == 0);
    if (!canList)
        s.rowsDenied = true;
    if (canList && !s.displayOnly) {
        if (s.defaultSgid)
            readForCheck(sch, uid, OpKind::CheckDefault, s.defaultSgid);
        if (s.adopted) {
            readForCheck(sch, uid, OpKind::CheckGroup, s.adopted);
        } else {
            for (int i = 0; i < found.fallback.size() && i < kMaxCandidates; ++i)
                readForCheck(sch, uid, OpKind::CheckCandidate, found.fallback.at(i));
        }
    }
    const auto clients = neededValue(sch, "b_virtualserver_servergroup_client_list");
    if (s.adopted && !s.displayOnly && !(clients && *clients == 0))
        readForCheck(sch, uid, OpKind::CheckMembers, s.adopted);
    s.channel                = ts3::ownChannel(sch);
    const auto channelList   = neededValue(sch, "b_virtualserver_channel_permission_list");
    if (s.channel && !(channelList && *channelList == 0))
        readForCheck(sch, uid, OpKind::CheckChannel, s.channel);
    else
        s.channelNeededUpload = -1;
    if (s.pendingReads == 0)
        finishCheck(sch, uid);
}

void AccessGroup::readForCheck(uint64 sch, const QString& uid, OpKind kind, quint64 id)
{
    Server& s = server(uid);
    Op      op;
    op.kind       = kind;
    op.sch        = sch;
    op.uid        = uid;
    op.sgid       = id;
    op.generation = s.generation;
    const QString    rc   = newOp(op);
    const QByteArray rc8  = rc.toUtf8();
    const auto       key  = qMakePair(sch, id);
    unsigned int     err  = ERROR_ok;
    switch (kind) {
    case OpKind::CheckDefault:
    case OpKind::CheckGroup:
    case OpKind::CheckCandidate:
        m_ops[rc].command = access::Command::Type::ReadPerms;
        m_permListOps.insert(key, rc);
        setCollecting(List::Perms, sch, id, true);
        err = ts3::funcs.requestServerGroupPermList(sch, id, rc8.constData());
        break;
    case OpKind::CheckMembers:
        m_clientListOps.insert(key, rc);
        setCollecting(List::Clients, sch, id, true);
        err = ts3::funcs.requestServerGroupClientList(sch, id, 0, rc8.constData());
        break;
    case OpKind::CheckChannel:
        m_channelOps.insert(key, rc);
        setCollecting(List::Channel, sch, id, true);
        err = ts3::funcs.requestChannelPermList(sch, id, rc8.constData());
        break;
    default:
        break;
    }
    ++s.pendingReads;
    if (err != ERROR_ok)
        finishLater(rc, access::Answer::failure(err, ts3::errorText(err)));
}

void AccessGroup::onCheckAnswer(const Op& op)
{
    auto it = m_servers.find(op.uid);
    if (it == m_servers.end() || it->generation != op.generation || !it->checking)
        return;
    Server&               s     = it.value();
    const access::Answer& a     = op.answer;
    const bool            empty = a.isEmptyResult();
    const bool            lost  = a.error == ERROR_not_connected || a.error == ERROR_connection_lost;

    switch (op.kind) {
    case OpKind::CheckGroups:
        --s.pendingReads;
        if (a.timeout || lost)
            s.checkFailed = true;
        continueCheck(op.sch, op.uid, op.generation);
        return;
    case OpKind::CheckDefault:
        if (a.ok || empty) {
            s.defaultRows  = a.ok ? a.rows : access::PermRows();
            s.defaultKnown = true;
        } else if (a.isPermissionError()) {
            s.rowsDenied = true;
        } else if (a.timeout || lost) {
            s.checkFailed = true;
        }
        break;
    case OpKind::CheckGroup:
        if (a.ok || empty) {
            s.rows       = a.ok ? a.rows : access::PermRows();
            s.rowsKnown  = true;
            s.rowsDenied = false;
        } else if (a.isPermissionError()) {
            s.rowsKnown  = false;
            s.rowsDenied = true;
            s.rows.clear();
        } else if (a.timeout || lost) {
            s.checkFailed = true;
        }
        break;
    case OpKind::CheckCandidate:
        if (a.ok || empty)
            s.candidateRows.insert(op.sgid, a.ok ? a.rows : access::PermRows());
        break;
    case OpKind::CheckMembers:
        s.members = a.ok ? op.members.size() : empty ? 0 : -1;
        break;
    case OpKind::CheckChannel:
        if (a.ok || empty) {
            const auto needed     = a.rows.constFind(QString::fromLatin1("i_ft_needed_file_upload_power"));
            s.channelNeededUpload = needed != a.rows.cend() ? needed->value : 0; // unset is 0
        } else {
            s.channelNeededUpload = -1;
        }
        break;
    default:
        break;
    }
    if (--s.pendingReads <= 0)
        finishCheck(op.sch, op.uid);
}

void AccessGroup::finishCheck(uint64 sch, const QString& uid)
{
    auto it = m_servers.find(uid);
    if (it == m_servers.end())
        return;
    Server& s = it.value();
    const Connection& c = connection(sch);

    // A renamed group: the remembered id first, then icon matches, each only when its permissions are
    // ours (conformant) - so the public icon id can't make Server Admin "the TS Media chat group".
    if (!s.adopted && !s.candidateRows.isEmpty()) {
        const access::Candidates found = access::findCandidates(c.groups.values(), s.defaultSgid, rememberedGroup(uid));
        for (int i = 0; i < found.fallback.size(); ++i) {
            const quint64 id = found.fallback.at(i);
            if (!s.candidateRows.contains(id))
                continue;
            const access::PermRows rows = s.candidateRows.value(id);
            const bool trusted = s.defaultKnown && access::conformant(rows, s.defaultRows)
                                 && (found.fallbackHow.at(i) == access::Match::Remembered || access::looksLikeOurs(rows));
            if (trusted && !s.adopted) {
                s.adopted    = id;
                s.how        = found.fallbackHow.at(i);
                s.rows       = rows;
                s.rowsKnown  = true;
                s.rowsDenied = false;
            } else if (!trusted) {
                s.ignored << c.groups.value(id);
                ts3::log(QString::fromLatin1("Server access: group %1 uses the TS Media chat icon or a remembered id but has other permissions; ignored").arg(id),
                         LogLevel_INFO, sch);
            }
        }
    }
    s.candidateRows.clear();
    if (!s.adopted) {
        s.rowsKnown = false;
        s.rows.clear();
    }
    if (s.adopted && s.rowsKnown && !s.displayOnly)
        rememberGroup(uid, s.adopted);

    s.checking      = false;
    s.checked       = true;
    s.explicitCheck = false;
    if (s.checkFailed) {
        if (s.error == access::ErrorKind::None) {
            s.error        = access::ErrorKind::Timeout;
            s.errorContext = access::ErrorContext::Check;
            s.errorDetail.clear();
        }
    } else if (s.errorContext == access::ErrorContext::Check) {
        s.error = access::ErrorKind::None;
    }
    if (s.queued != access::Action::None) {
        const access::Action action = s.queued;
        const uint64         where  = s.queuedSch;
        s.queued                    = access::Action::None;
        run(where, action);
        return; // run() notifies
    }
    if (s.recheck) {
        s.recheck = false;
        refresh(sch, false);
        return;
    }
    notify();
}

// ============================================================================================
// Create / Repair
// ============================================================================================

void AccessGroup::startJob(uint64 sch, access::JobKind kind)
{
    if (!ts3::isConnected(sch))
        return;
    const QString uid = uidOf(sch);
    if (uid.isEmpty())
        return;
    Server&     s = server(uid);
    Connection& c = connection(sch);
    if (s.job)
        return; // one job per server, whichever tab clicked
    s.error      = access::ErrorKind::None;
    s.jobStarted = kind;
    const auto fail = [&](access::ErrorKind error) {
        s.waitingForServerVars = false;
        s.error                = error;
        s.errorContext         = kind == access::JobKind::Create ? access::ErrorContext::Create : access::ErrorContext::Repair;
        s.errorDetail.clear();
        notify();
    };

    // Preflight, nothing sent yet: the default group's id ...
    const quint64 defaultSgid = defaultGroupOf(sch);
    if (!defaultSgid) {
        if (!s.waitingForServerVars && !c.serverVarsRequested && ts3::funcs.requestServerVariables) {
            c.serverVarsRequested  = true;
            s.waitingForServerVars = true; // shown as the job's first step
            ts3::funcs.requestServerVariables(sch);
            QTimer::singleShot(kServerVarsMs, this, [this, sch, uid, kind] {
                auto it = m_servers.find(uid);
                if (it == m_servers.end() || !it->waitingForServerVars)
                    return;
                it->waitingForServerVars = false;
                startJob(sch, kind); // with the variables requested once, a 0 now is an error
            });
            notify();
            return;
        }
        fail(access::ErrorKind::NoDefaultGroup); // never guessed (e.g. by the name "Guest")
        return;
    }
    s.waitingForServerVars = false;
    s.defaultSgid          = defaultSgid;

    // ... and every name of the plan resolves to an id.
    for (const QString& name : access::requiredNames()) {
        if (permissionId(sch, name))
            continue;
        if (!c.permListRequested && ts3::funcs.requestPermissionList) {
            c.permListRequested = true;
            Op op;
            op.kind          = OpKind::Ignore;
            op.sch           = sch;
            op.uid           = uid;
            const QString rc = newOp(op);
            ts3::funcs.requestPermissionList(sch, rc.toUtf8().constData()); // fills our own name map
        }
        fail(access::ErrorKind::NotLoaded);
        return;
    }

    // Looked up again: a group found by name (another admin was faster) turns the click into a repair.
    const access::Candidates found = access::findCandidates(c.groups.values(), defaultSgid, 0);
    quint64 target = found.byName;
    if (!target && s.adopted && !s.displayOnly && s.rowsKnown && c.groups.contains(s.adopted))
        target = s.adopted; // a renamed group that was checked for conformance

    access::JobInput in;
    in.kind        = target ? access::JobKind::Repair : access::JobKind::Create;
    in.defaultSgid = defaultSgid;
    in.sgid        = target;
    for (auto it = c.groups.cbegin(); it != c.groups.cend(); ++it)
        in.knownGroups.insert(it.key());
    in.own = ownPowers(sch);
    const auto manage   = neededValue(sch, "b_icon_manage");
    const auto iconSize = neededValue(sch, "i_max_icon_filesize");
    if (iconSize && *iconSize >= 0 && *iconSize < access::kGroupIconBytes) {
        in.iconSkip    = access::IconProblem::TooLarge;
        in.maxIconSize = *iconSize;
    } else if (manage && *manage == 0) {
        in.iconSkip = access::IconProblem::NoManagePermission; // only the "already on the server" path
    }
    in.refused = s.refused.value(target);

    s.job        = std::make_shared<access::Job>(in);
    s.jobSch     = sch;
    s.jobId      = ++m_nextJob;
    m_lastJobUid = uid;
    ts3::log(QString::fromLatin1("Server access: %1 started").arg(in.kind == access::JobKind::Create ? QLatin1String("create") : QLatin1String("repair")), LogLevel_INFO, sch);
    execute(uid, s.job->start());
    notify();
}

void AccessGroup::execute(const QString& uid, const access::Command& command)
{
    auto sit = m_servers.find(uid);
    if (sit == m_servers.end() || !sit->job)
        return;
    const std::shared_ptr<access::Job> job = sit->job;
    const uint64                       sch = sit->jobSch;
    using Type                             = access::Command::Type;
    if (command.type == Type::Finished || command.type == Type::None) {
        finishJob(uid);
        return;
    }
    // Before every step: still that server. A tab's handle can reconnect to another server.
    if (!ts3::isConnected(sch) || ts3::serverUid(sch) != uid) {
        execute(uid, job->abort(access::ErrorKind::ConnectionLost));
        return;
    }

    Op op;
    op.kind    = OpKind::Job;
    op.command = command.type;
    op.sch     = sch;
    op.uid     = uid;
    op.sgid    = command.sgid;
    op.jobId   = sit->jobId;

    QString      rc;
    unsigned int err = ERROR_ok;
    switch (command.type) {
    case Type::ReadPerms:
        rc = newOp(op);
        m_permListOps.insert(qMakePair(sch, command.sgid), rc);
        setCollecting(List::Perms, sch, command.sgid, true);
        err = ts3::funcs.requestServerGroupPermList(sch, command.sgid, rc.toUtf8().constData());
        break;
    case Type::AddGroup:
        rc  = newOp(op);
        err = ts3::funcs.requestServerGroupAdd(sch, access::groupName().toUtf8().constData(), access::kGroupType, rc.toUtf8().constData());
        break;
    case Type::ListGroups:
        rc = newOp(op);
        m_groupListOps.insert(sch, rc);
        err = ts3::funcs.requestServerGroupList(sch, rc.toUtf8().constData());
        break;
    case Type::AddPerms: {
        QVector<unsigned int> ids;
        QVector<int>          values;
        QVector<int>          negated;
        QVector<int>          skip;
        QStringList           unresolved;
        for (const access::PlanItem& item : command.perms) {
            const unsigned int id = permissionId(sch, item.name);
            if (!id) {
                unresolved << item.name;
                continue;
            }
            ids << id;
            values << item.value.value;
            negated << (item.value.negated ? 1 : 0);
            skip << (item.value.skip ? 1 : 0);
        }
        rc = newOp(op);
        if (!unresolved.isEmpty()) {
            Server& s = server(uid);
            for (const QString& n : qAsConst(unresolved))
                s.refused[command.sgid].insert(n);
            m_ops[rc].answer.failedPerms = unresolved;
        }
        if (ids.isEmpty()) {
            finishLater(rc, access::Answer::failure(ERROR_permissions_client_insufficient, QString(), unresolved));
            return;
        }
        err = ts3::funcs.requestServerGroupAddPerm(sch, command.sgid, command.continueOnError ? 1 : 0, ids.constData(), values.constData(), negated.constData(),
                                                   skip.constData(), ids.size(), rc.toUtf8().constData());
        break;
    }
    case Type::DeleteGroup:
        rc  = newOp(op);
        err = ts3::funcs.requestServerGroupDel(sch, command.sgid, 0, rc.toUtf8().constData()); // force 0: refused if it has members
        break;
    case Type::UploadIcon: {
        // A folder per job: another tab's job never deletes the file this transfer reads. For a root
        // path both of the client lib's layouts are <dir>/<name> (see Core's staging comment).
        const QString dir = iconRoot() + QLatin1Char('/') + QString::number(sit->jobId);
        op.iconDir        = dir;
        rc                = newOp(op, kUploadTimeoutMs);
        if (!writeIcon(dir)) {
            finishLater(rc, access::Answer::failure(ERROR_file_io_error, i18n::t("the icon file couldn't be written")));
            return;
        }
        anyID tid = 0;
        // overwrite=1: the same name means the same CRC, so the same bytes, and it repairs a damaged file.
        err = ts3::funcs.sendFile(sch, 0, "", access::iconRemotePath().toUtf8().constData(), 1, 0, utf8Native(dir).constData(), &tid, rc.toUtf8().constData());
        if (err == ERROR_ok) {
            Op& stored            = m_ops[rc];
            stored.transferId     = tid;
            stored.transferActive = true;
            m_uploads.insert(tid, rc);
        }
        break;
    }
    case Type::CheckIconFile:
        rc = newOp(op);
        m_fileInfoOps.insert(sch, rc);
        err = ts3::funcs.requestFileInfo(sch, 0, "", access::iconRemotePath().toUtf8().constData(), rc.toUtf8().constData());
        break;
    case Type::Finished:
    case Type::None:
        break;
    }
    if (err != ERROR_ok && !rc.isEmpty())
        finishLater(rc, access::Answer::failure(err, ts3::errorText(err)));
    notify(); // the step text
}

bool AccessGroup::writeIcon(const QString& dir)
{
    if (!QDir().mkpath(dir))
        return false;
    QSaveFile file(dir + QLatin1Char('/') + access::iconFileName());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray icon = access::groupIcon16();
    return file.write(icon) == icon.size() && file.commit();
}

void AccessGroup::onJobAnswer(const Op& op)
{
    auto it = m_servers.find(op.uid);
    if (it == m_servers.end() || !it->job || it->jobId != op.jobId)
        return; // an answer for a job that already ended (timed out, connection lost)
    if (!op.iconDir.isEmpty())
        QDir(op.iconDir).removeRecursively();
    access::Answer answer = op.answer;
    if (op.command == access::Command::Type::AddGroup || op.command == access::Command::Type::ListGroups)
        answer.groups = groupList(op.sch);
#ifdef TSMEDIA_TESTHOOKS
    if (m_probe)
        probeLog(QString::fromLatin1("job answer cmd %1 ok %2 timeout %3 error 0x%4 failed [%5] rows %6")
                     .arg(QString::number(static_cast<int>(op.command)), QString::number(answer.ok), QString::number(answer.timeout), QString::number(answer.error, 16),
                          answer.failedPerms.join(QLatin1Char(',')), QString::number(answer.rows.size())));
#endif
    const std::shared_ptr<access::Job> job = it->job;
    execute(op.uid, job->next(answer));
}

void AccessGroup::failJobsOn(uint64 sch)
{
    QStringList uids;
    for (auto it = m_servers.cbegin(); it != m_servers.cend(); ++it) {
        if ((it->job || it->waitingForServerVars) && it->jobSch == sch)
            uids << it.key();
    }
    for (const QString& uid : qAsConst(uids)) {
        auto it = m_servers.find(uid);
        if (it == m_servers.end())
            continue;
        if (it->waitingForServerVars && !it->job) {
            it->waitingForServerVars = false;
            it->error                = access::ErrorKind::ConnectionLost;
            continue;
        }
        const std::shared_ptr<access::Job> job = it->job;
        execute(uid, job->abort(access::ErrorKind::ConnectionLost));
    }
}

void AccessGroup::finishJob(const QString& uid)
{
    auto it = m_servers.find(uid);
    if (it == m_servers.end() || !it->job)
        return;
    Server&                 s      = it.value();
    const access::JobResult result = s.job->result();
    const uint64            sch    = s.jobSch;
    const quint64           jobId  = s.jobId;
    s.job.reset();
    QDir(iconRoot() + QLatin1Char('/') + QString::number(jobId)).removeRecursively();

    if (result.defaultRowsKnown) {
        s.defaultRows  = result.defaultRows;
        s.defaultKnown = true;
    }
    if (result.sgid)
        s.refused[result.sgid] += result.refused;
    if (result.removed) {
        s.adopted   = 0;
        s.rowsKnown = false;
        s.rows.clear();
        s.members = -1;
    } else if (result.sgid && result.rowsKnown) {
        if (s.adopted != result.sgid)
            s.members = result.created ? 0 : -1;
        s.adopted     = result.sgid;
        s.how         = access::Match::Name;
        s.displayOnly = false;
        s.rows        = result.groupRows;
        s.rowsKnown   = true;
        s.rowsDenied  = false;
        rememberGroup(uid, result.sgid);
    }
    s.iconProblem = result.iconProblem;
    s.iconDetail  = result.iconDetail;
    s.maxIconSize = result.maxIconSize;
    if (!result.success) {
        s.error        = result.error;
        s.errorContext = result.kind == access::JobKind::Create ? access::ErrorContext::Create : access::ErrorContext::Repair;
        s.errorDetail  = result.errorDetail;
    }

    // One line in that server's chat: a record of the change, also when the dialog was closed meanwhile.
    const QString    group    = result.sgid && !result.removed ? groupNameOf(sch, result.sgid) : access::groupName();
    const QString    fallback = groupNameOf(sch, s.defaultSgid);
    const access::Missing left = access::missing(s.rows, planFor(sch, s), s.refused.value(result.sgid));
    const bool       complete = result.success && access::visibleMissing(left.sendable).isEmpty() && access::isProtected(s.rows);
    const access::ChatLines lines = access::chatLinesFor(result, s.jobStarted, group, fallback, complete);
    for (const QString& line : lines.info)
        ts3::printInfo(sch, line);
    for (const QString& line : lines.warnings)
        ts3::printWarning(sch, line);
    ts3::log(QString::fromLatin1("Server access: job finished, success %1, error %2, group %3, created %4, removed %5, refused %6")
                 .arg(QString::number(result.success), QString::number(static_cast<int>(result.error)), QString::number(result.sgid),
                      QString::number(result.created), QString::number(result.removed), QString::number(result.refused.size())),
             LogLevel_INFO, sch);

    if (m_watching > 0 && ts3::isConnected(sch))
        refresh(sch, false); // members and the channel line; keeps the job's error
    notify();
}

// ============================================================================================
// Give / Remove TS Media chat access (client context menu)
// ============================================================================================

void AccessGroup::giveOrTake(uint64 sch, anyID clientId, bool give)
{
    using access::MemberOutcome;
    if (!ts3::isConnected(sch))
        return;
    char* nick = nullptr;
    if (ts3::funcs.getClientVariableAsString(sch, clientId, CLIENT_NICKNAME, &nick) != ERROR_ok) {
        ts3::printWarning(sch, access::memberText(MemberOutcome::Gone, give, {}, {}));
        return;
    }
    const QString person = ts3::takeString(nick).left(64);
    int           type   = ClientType_NORMAL;
    if (ts3::funcs.getClientVariableAsInt(sch, clientId, CLIENT_TYPE, &type) == ERROR_ok && type == ClientType_SERVERQUERY) {
        ts3::printInfo(sch, access::memberText(MemberOutcome::ServerQuery, give, person, {}));
        return;
    }
    uint64 dbid = 0;
    if (ts3::funcs.getClientVariableAsUInt64(sch, clientId, CLIENT_DATABASE_ID, &dbid) != ERROR_ok || !dbid) {
        ts3::printWarning(sch, access::memberText(MemberOutcome::Gone, give, person, {}));
        return;
    }
    const quint64 sgid = groupFor(sch);
    if (!sgid) {
        ts3::printInfo(sch, access::memberText(MemberOutcome::NoGroup, give, person, {}));
        return;
    }
    QString group = groupNameOf(sch, sgid);
    if (group.isEmpty())
        group = access::groupName();
    char* groups = nullptr;
    bool  member = false;
    if (ts3::funcs.getClientVariableAsString(sch, clientId, CLIENT_SERVERGROUPS, &groups) == ERROR_ok)
        member = access::parseServerGroups(ts3::takeString(groups)).contains(sgid);
    if (give && member) {
        ts3::printInfo(sch, access::memberText(MemberOutcome::AlreadyMember, give, person, group));
        return;
    }
    if (!give && !member) {
        ts3::printInfo(sch, access::memberText(MemberOutcome::NotMember, give, person, group));
        return;
    }

    Op op;
    op.kind      = OpKind::Member;
    op.sch       = sch;
    op.uid       = uidOf(sch);
    op.sgid      = sgid;
    op.give      = give;
    op.person    = person;
    op.groupName = group;
    const QString    rc  = newOp(op);
    const QByteArray rc8 = rc.toUtf8();
    // TeamSpeak's server checks your powers (i_group_member_add/remove_power against the group's needed
    // values, i_client_permission_modify_power against the person's).
    const unsigned int err = give ? ts3::funcs.requestServerGroupAddClient(sch, sgid, dbid, rc8.constData())
                                  : ts3::funcs.requestServerGroupDelClient(sch, sgid, dbid, rc8.constData());
    if (err != ERROR_ok)
        finishLater(rc, access::Answer::failure(err, ts3::errorText(err)));
}

void AccessGroup::onMemberAnswer(const Op& op)
{
    using access::MemberOutcome;
    const access::Answer& a = op.answer;
    if (a.ok) {
        ts3::printInfo(op.sch, access::memberText(op.give ? MemberOutcome::Added : MemberOutcome::Removed, op.give, op.person, op.groupName));
        auto it = m_servers.find(op.uid);
        if (it != m_servers.end() && it->adopted == op.sgid) {
            if (it->members >= 0)
                it->members = qMax(0, it->members + (op.give ? 1 : -1));
            if (op.give && it->rowsKnown && it->defaultKnown) {
                // TeamSpeak takes a Guest out of the default group: say so if the copies are incomplete.
                const access::Missing left = access::missing(it->rows, planFor(op.sch, it.value()));
                bool                  copies = false;
                for (const access::PlanItem& i : left.sendable + left.refused)
                    copies = copies || i.kind == access::ItemKind::Copy;
                if (copies)
                    ts3::printWarning(op.sch, access::memberMissingCopiesText(op.person, op.groupName, groupNameOf(op.sch, it->defaultSgid)));
            }
        }
    } else if (a.timeout) {
        ts3::printWarning(op.sch, access::memberText(MemberOutcome::Failed, op.give, op.person, op.groupName, access::errorText(access::ErrorKind::Timeout, {}, {})));
    } else if (op.give && (a.error == ERROR_database_duplicate_entry || a.error == ERROR_permission_duplicate_entry)) {
        ts3::printInfo(op.sch, access::memberText(MemberOutcome::AlreadyMember, op.give, op.person, op.groupName));
    } else if (a.isPermissionError()) {
        const QString detail = a.failedPerms.isEmpty() ? ts3::errorText(a.error) : a.failedPerms.join(QLatin1String(", "));
        ts3::printWarning(op.sch, access::memberText(MemberOutcome::NoPermission, op.give, op.person, op.groupName, detail));
    } else {
        ts3::printWarning(op.sch, access::memberText(MemberOutcome::Failed, op.give, op.person, op.groupName, a.message.isEmpty() ? ts3::errorText(a.error) : a.message));
    }
    notify();
}

// ============================================================================================
// Menus and notifications
// ============================================================================================

void AccessGroup::updateMenus()
{
    if (!ts3::funcs.setPluginMenuEnabled || ts3::pluginId.isEmpty())
        return;
    const uint64  sch      = ts3::currentConnection();
    const quint64 sgid     = ts3::isConnected(sch) ? groupFor(sch) : 0;
    bool          canGive  = sgid != 0;
    bool          canTake  = sgid != 0;
    if (sgid) {
        // Greyed only when your power is known to be too low; the server decides the rest.
        const Server* s      = findServer(uidOf(sch));
        const auto    needed = [s](const char* name) -> int {
            if (!s || !s->rowsKnown)
                return 1;
            const auto it = s->rows.constFind(QString::fromLatin1(name));
            return it == s->rows.cend() ? 1 : qMax(1, it->value);
        };
        const auto add    = neededValue(sch, "i_group_member_add_power");
        const auto remove = neededValue(sch, "i_group_member_remove_power");
        canGive           = !(add && *add < needed("i_group_needed_member_add_power"));
        canTake           = !(remove && *remove < needed("i_group_needed_member_remove_power"));
    }
    // The state applies the next time a menu opens (plugin.c).
    const QByteArray id = ts3::pluginId.toUtf8();
    ts3::funcs.setPluginMenuEnabled(id.constData(), m_menuGive, canGive ? 1 : 0);
    ts3::funcs.setPluginMenuEnabled(id.constData(), m_menuRemove, canTake ? 1 : 0);
}

void AccessGroup::notify()
{
    updateMenus();
    emit changed();
}

// ============================================================================================
// Test hooks
// ============================================================================================

#ifdef TSMEDIA_TESTHOOKS
// <data dir>/selftest_access.txt, read 8 s after connecting to a localhost server:
//   probe          logs what the server reports (needed permissions, default group, name map, groups)
//                  and every event that follows, with timestamps
//   create|repair  runs the job
//   give <nick> | remove <nick>
void AccessGroup::selfTest(uint64 sch)
{
    QFile trigger(ts3::dataDir() + QLatin1String("/selftest_access.txt"));
    if (!trigger.exists())
        return;
    if (!isLocalServer(sch)) {
        ts3::log(QString::fromLatin1("[test] access self-test skipped: not a localhost server"));
        return;
    }
    if (!trigger.open(QIODevice::ReadOnly))
        return;
    const QStringList lines = QString::fromUtf8(trigger.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    trigger.close();
    trigger.remove();
    for (QString line : lines) {
        line = line.trimmed();
        if (line == QLatin1String("probe")) {
            m_probe = true;
            const char* const names[] = {"b_virtualserver_servergroup_create", "i_group_member_add_power", "i_group_modify_power", "b_icon_manage", "i_max_icon_filesize",
                                         "b_virtualserver_servergroup_permission_list"};
            for (const char* n : names) {
                int        value = 0;
                const auto rc    = ts3::funcs.getClientNeededPermission(sch, n, &value);
                const auto known = neededValue(sch, n);
                probeLog(QString::fromLatin1("needed %1: getClientNeededPermission rc 0x%2 value %3, map %4")
                             .arg(QString::fromLatin1(n), QString::number(rc, 16), QString::number(value), known ? QString::number(*known) : QString::fromLatin1("unknown")));
            }
            probeLog(QString::fromLatin1("default group %1").arg(defaultGroupOf(sch)));
            for (const QString& n : access::requiredNames())
                probeLog(QString::fromLatin1("permission id %1 = %2").arg(n, QString::number(permissionId(sch, n))));
            const Connection& c = connection(sch);
            probeLog(QString::fromLatin1("cached groups %1, complete %2, needed list %3 (%4)")
                         .arg(QString::number(c.groups.size()), QString::number(c.groupsComplete), QString::number(c.neededKnown), QString::number(c.needed.size())));
            char* groups = nullptr;
            if (ts3::funcs.getClientVariableAsString(sch, ts3::ownClientId(sch), CLIENT_SERVERGROUPS, &groups) == ERROR_ok)
                probeLog(QString::fromLatin1("own CLIENT_SERVERGROUPS '%1'").arg(ts3::takeString(groups)));
        } else if (line == QLatin1String("create")) {
            startJob(sch, access::JobKind::Create);
        } else if (line == QLatin1String("repair")) {
            refresh(sch, true);
            QTimer::singleShot(3000, this, [this, sch] { startJob(sch, access::JobKind::Repair); });
        } else if (line.startsWith(QLatin1String("give ")) || line.startsWith(QLatin1String("remove "))) {
            const bool  give   = line.startsWith(QLatin1String("give "));
            const anyID client = ts3::clientIdByNickname(sch, line.section(QLatin1Char(' '), 1).trimmed());
            if (client)
                giveOrTake(sch, client, give);
            else
                probeLog(QString::fromLatin1("no client named '%1'").arg(line.section(QLatin1Char(' '), 1)));
        }
    }
}
#endif
