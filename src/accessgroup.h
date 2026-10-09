#pragma once

// 2.2 servergroup: the one-click "tsmediachat" server group. AccessGroup is the TeamSpeak side of
// Settings > Server access and of the client menu items "Give / Remove TS Media chat access":
//  * it keeps per-connection caches fed by TeamSpeak's events (the server group list, your needed
//    permissions) and per-server state (the adopted group, its permissions, refusals this session);
//  * it runs access::Job (accessgroupjob.h) against the server, one request at a time, each with its
//    own return code and a 10 s timeout;
//  * it sends network requests only while the settings box is visible or a job runs.
//
// A GUI-thread object. TeamSpeak's callbacks arrive on other threads: the static handle*() functions
// copy what they need and continue on the GUI thread. Created in ts3plugin_init, deleted in the
// shutdown cleanup before Core (it stops its timers and a running icon upload).

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

#include "accessgroupjob.h"
#include "accessgroupplan.h"
#include "accessgroupview.h"
#include "ts3api.h"

class QTimer;

class AccessGroup : public QObject
{
    Q_OBJECT

  public:
    // menuGive / menuRemove: the ids of the client menu items (greyed while they can't work here).
    AccessGroup(int menuGive, int menuRemove, QObject* parent = nullptr);
    ~AccessGroup() override;

    static AccessGroup* instance();

    // ---- TeamSpeak callbacks (any thread) --------------------------------------------------------
    // True when the return code is ours: the caller returns 1 so TeamSpeak doesn't print the error.
    static bool handleServerError(uint64 sch, const char* message, unsigned int error, const char* returnCode, unsigned int failedPermissionId, bool permissionError);
    static void handleTransferStatus(anyID transferId, unsigned int status, const char* message, uint64 sch);
    static void handleConnectStatus(uint64 sch, int status);
    static void handleCurrentConnectionChanged(uint64 sch);
    static bool handleMenuItem(uint64 sch, int menuItemId, uint64 selectedItemId); // true: one of ours
    static void handleServerGroupList(uint64 sch, uint64 sgid, const char* name, int type, int iconId);
    static void handleServerGroupListFinished(uint64 sch);
    static void handleServerGroupPermList(uint64 sch, uint64 sgid, unsigned int permissionId, int value, int negated, int skip);
    static void handleServerGroupPermListFinished(uint64 sch, uint64 sgid);
    static void handleServerGroupClientList(uint64 sch, uint64 sgid, uint64 clientDatabaseId);
    static void handleServerGroupClientChanged(uint64 sch, anyID clientId, uint64 sgid, bool added);
    static void handleNeededPermission(uint64 sch, unsigned int permissionId, int value);
    static void handleNeededPermissionsFinished(uint64 sch);
    static void handleChannelPermList(uint64 sch, uint64 channelId, unsigned int permissionId, int value);
    static void handleChannelPermListFinished(uint64 sch, uint64 channelId);
    static void handleFileInfo(uint64 sch, uint64 channelId, const char* name, uint64 size);
    static void handlePermissionList(uint64 sch, unsigned int permissionId, const char* name);
    static void handleServerUpdated(uint64 sch);

    // ---- the settings box (GUI thread) -----------------------------------------------------------
    // The connection the box shows: the job's while one runs, otherwise the current tab.
    uint64            displayConnection() const;
    access::ViewInput view(uint64 sch) const;
    access::Details   details(uint64 sch) const;
    QString           serverName(uint64 sch) const;
    void              setWatching(bool watching); // counted; while watched, the current server is checked
    void              refresh(uint64 sch, bool explicitCheck);
    void              run(uint64 sch, access::Action action);

  signals:
    void changed(); // anything the box shows may have changed

  private:
    // ---- per connection (sch): what TeamSpeak tells every client ---------------------------------
    struct Connection {
        QString                            uid;
        QHash<quint64, access::GroupInfo>  groups;
        QSet<quint64>                      pass; // ids seen since the last list's end
        bool                               groupsComplete = false;
        QHash<unsigned int, int>           needed;        // your needed permissions (id -> value)
        QHash<unsigned int, int>           neededPending; // filled until the Finished event
        bool                               neededKnown = false;
        QHash<QString, unsigned int>       permIds;       // names seen (fallback when TeamSpeak's map isn't loaded)
        bool                               permListRequested   = false;
        bool                               serverVarsRequested = false;
    };

    // ---- per server (UID): what the plugin found out ----------------------------------------------
    struct Server {
        quint64                     adopted = 0;
        access::Match               how     = access::Match::None;
        bool                        displayOnly = false; // found by name without a group list
        QList<access::GroupInfo>    sameName;
        QList<access::GroupInfo>    ignored;
        bool                        rowsKnown = false;
        access::PermRows            rows;
        bool                        rowsDenied = false; // the permission list was refused: you can't manage it
        bool                        defaultKnown = false;
        access::PermRows            defaultRows;
        quint64                     defaultSgid = 0;
        int                         members = -1;
        QHash<quint64, QSet<QString>> refused; // per group: refused this session, never sent again
        access::IconProblem         iconProblem = access::IconProblem::None;
        QString                     iconDetail;
        int                         maxIconSize = 0;
        uint64                      channel = 0;
        int                         channelNeededUpload = -1;
        // check
        bool                        checking = false;
        bool                        checked  = false;
        bool                        explicitCheck = false;
        bool                        recheck  = false;
        int                         pendingReads = 0;
        quint64                     generation = 0;
        QHash<quint64, access::PermRows> candidateRows; // fallback candidates read during a check
        access::Action              queued = access::Action::None;
        uint64                      queuedSch = 0;
        // error (J): kept until the next user action
        access::ErrorKind           error = access::ErrorKind::None;
        access::ErrorContext        errorContext = access::ErrorContext::Create;
        QString                     errorDetail;
        bool                        checkFailed = false; // the running check timed out
        // job
        std::shared_ptr<access::Job> job; // shared: QHash values must be copyable
        access::JobKind             jobStarted = access::JobKind::Create;
        uint64                      jobSch = 0;
        quint64                     jobId = 0;
        bool                        waitingForServerVars = false; // a job waits for the default group id
    };

    enum class OpKind { Job, CheckGroups, CheckDefault, CheckGroup, CheckCandidate, CheckMembers, CheckChannel, Member, Ignore };
    struct Op {
        OpKind                 kind = OpKind::Job;
        access::Command::Type  command = access::Command::Type::None;
        uint64                 sch = 0;
        QString                uid;
        quint64                sgid = 0;
        quint64                jobId = 0;
        quint64                generation = 0;
        bool                   done = false;
        bool                   listFinished = false; // the list's Finished event arrived
        access::Answer         answer;
        QSet<quint64>          members;   // CheckMembers
        QTimer*                timer = nullptr;
        QTimer*                grace = nullptr;
        // Member
        bool                   give = true;
        QString                person;
        QString                groupName;
        anyID                  transferId = 0; // UploadIcon
        bool                   transferActive = false;
        QString                iconDir;
    };

    template <typename Fn>
    static void post(Fn&& fn);

    Connection&       connection(uint64 sch);
    Server&           server(const QString& uid);
    const Server*     findServer(const QString& uid) const;
    QString           uidOf(uint64 sch) const;
    std::optional<int> neededValue(uint64 sch, const char* name) const; // known values only
    unsigned int      permissionId(uint64 sch, const QString& name) const;
    QString           permissionName(uint64 sch, unsigned int id);
    quint64           defaultGroupOf(uint64 sch) const;
    QString           groupNameOf(uint64 sch, quint64 sgid) const;
    QList<access::GroupInfo> groupList(uint64 sch) const;
    quint64           rememberedGroup(const QString& uid) const;
    void              rememberGroup(const QString& uid, quint64 sgid);
    bool              isMember(uint64 sch, quint64 sgid) const;
    access::OwnPowers ownPowers(uint64 sch) const;
    access::Plan      planFor(uint64 sch, const Server& s) const;
    quint64           groupFor(uint64 sch) const; // the adopted group, else a name match in the cache

    QString newOp(Op op, int timeoutMs = 10000);
    void    complete(const QString& rc);
    void    rcAnswered(const QString& rc, unsigned int error, const QString& message, unsigned int failedPermissionId, bool permissionError);
    void    finishLater(const QString& rc, const access::Answer& answer);

    // checks
    void startCheck(uint64 sch, Server& s, const QString& uid);
    void continueCheck(uint64 sch, const QString& uid, quint64 generation);
    void readForCheck(uint64 sch, const QString& uid, OpKind kind, quint64 sgid);
    void onCheckAnswer(const Op& op);
    void finishCheck(uint64 sch, const QString& uid);

    // jobs
    void startJob(uint64 sch, access::JobKind kind);
    void execute(const QString& uid, const access::Command& command);
    void onJobAnswer(const Op& op);
    void finishJob(const QString& uid);
    void failJobsOn(uint64 sch);
    bool writeIcon(const QString& dir);

    // members (client menu)
    void giveOrTake(uint64 sch, anyID clientId, bool give);
    void onMemberAnswer(const Op& op);

    // events (GUI thread)
    void onServerGroupList(uint64 sch, const access::GroupInfo& group);
    void onServerGroupListFinished(uint64 sch);
    void onGroupsChanged(uint64 sch);
    void onServerGroupPermList(uint64 sch, quint64 sgid, unsigned int permissionId, const access::PermValue& value);
    void onServerGroupPermListFinished(uint64 sch, quint64 sgid);
    void onConnectStatus(uint64 sch, int status);
    void onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch);

    void updateMenus();
    void notify(); // emits changed() and updates the menus

#ifdef TSMEDIA_TESTHOOKS
    void selfTest(uint64 sch);
    bool m_probe = false;
#endif

    int                        m_menuGive;
    int                        m_menuRemove;
    QHash<uint64, Connection>  m_connections;
    QHash<QString, Server>     m_servers;
    QHash<QString, Op>         m_ops;
    QHash<QPair<uint64, quint64>, QString> m_permListOps;   // (sch, sgid) -> rc collecting its rows
    QHash<QPair<uint64, quint64>, QString> m_clientListOps; // (sch, sgid)
    QHash<QPair<uint64, quint64>, QString> m_channelOps;    // (sch, channel)
    QHash<uint64, QString>     m_groupListOps;              // sch
    QHash<uint64, QString>     m_fileInfoOps;               // sch
    QHash<anyID, QString>      m_uploads;                   // transfer id -> rc
    int                        m_watching = 0;
    quint64                    m_nextJob  = 0;

    QString                    m_lastJobUid;                // the box follows the latest job's server

    // Read from TeamSpeak's threads.
    enum class List { Perms, Clients, Channel };
    void                       setCollecting(List list, uint64 sch, quint64 id, bool on);
    bool                       isCollecting(List list, uint64 sch, quint64 id) const;
    mutable QMutex             m_mutex;
    QSet<QString>              m_returnCodes; // every rc issued this session (late duplicates stay ours)
    QSet<QPair<uint64, quint64>> m_collectPerms;    // server group permission lists we wait for
    QSet<QPair<uint64, quint64>> m_collectClients;  // server group client lists
    QSet<QPair<uint64, quint64>> m_collectChannels; // channel permission lists
};
