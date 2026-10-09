#pragma once

// 2.2 servergroup: the "Create / Repair TS Media chat group" job as a pure state machine. The job never
// talks to TeamSpeak itself: it hands out one Command at a time, AccessGroup (accessgroup.cpp) carries
// it out and feeds the server's Answer back. That keeps the order of the steps, the rollback rules and
// every error branch unit-testable without a server.
//
// Create: read the default group -> add the group -> find its id -> protect it (required; a group this
// job created is deleted again when that fails) -> file permissions, quotas, copies -> icon -> needed
// modify power -> read everything back (the only truth).
// Repair: the same order, sending only what's missing.

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "accessgroupplan.h"

namespace access {

// TeamSpeak error codes the job interprets (AccessGroup checks them against the SDK headers).
namespace err {
constexpr unsigned int ok                      = 0x0000;
constexpr unsigned int connectionLost          = 0x0701;
constexpr unsigned int notConnected            = 0x0702;
constexpr unsigned int databaseEmptyResult     = 0x0501;
constexpr unsigned int databaseDuplicateEntry  = 0x0502;
constexpr unsigned int permissionDuplicate     = 0x0a01;
constexpr unsigned int permissionEmptyResult   = 0x0a03;
constexpr unsigned int clientInsufficient      = 0x0a08; // permissions_client_insufficient
constexpr unsigned int insufficientGroupPower  = 0x0a09;
constexpr unsigned int insufficientPermPower   = 0x0a0a;
constexpr unsigned int fileAlreadyExists       = 0x0802;
constexpr unsigned int fileAlreadyInUse        = 0x080a;
} // namespace err

enum class JobKind { Create, Repair };
enum class StepKind { CreateGroup, AddPermissions, AddIcon, Checking };

enum class ErrorKind {
    None,
    NoCreatePermission,  // b_virtualserver_servergroup_create refused
    CantReadPermissions, // b_virtualserver_servergroup_permission_list refused
    NoDefaultGroup,      // VIRTUALSERVER_DEFAULT_SERVER_GROUP unknown
    NotLoaded,           // the permission names don't resolve yet
    ProtectRemoved,      // protection refused, the new group was deleted again
    ProtectLeftEmpty,    // protection refused and the delete too
    NoFilePermsRemoved,  // no file permission landed, the new group was deleted again
    Timeout,
    ConnectionLost,
    CreatedNotFound,
    Refused,             // anything else the server refused (detail: its message)
};

enum class IconProblem {
    None,
    NoManagePermission, // b_icon_manage
    TooLarge,           // i_max_icon_filesize < 335
    UploadFailed,       // detail: why
    Refused,            // the upload worked, i_icon_id was refused (detail: the permission)
};

struct Command {
    enum class Type { None, ReadPerms, AddGroup, ListGroups, AddPerms, DeleteGroup, UploadIcon, CheckIconFile, Finished };
    Type            type = Type::None;
    quint64         sgid = 0;
    QList<PlanItem> perms;                   // AddPerms
    bool            continueOnError = false; // AddPerms
};

// The server's answer to the last command.
struct Answer {
    bool             ok      = false;
    bool             timeout = false;
    unsigned int     error   = err::ok;
    QString          message;     // the server's text for error
    QStringList      failedPerms; // permission names from permission errors carrying this request's return code
    PermRows         rows;        // ReadPerms
    QList<GroupInfo> groups;      // AddGroup / ListGroups: the group list as known after the answer
    qint64           fileSize = -1; // CheckIconFile: size of the icon file on the server, -1 = none

    static Answer success();
    static Answer failure(unsigned int error, const QString& message, const QStringList& failedPerms = {});
    static Answer timedOut();
    bool          isEmptyResult() const; // "nothing to list" counts as an empty, successful read
    bool          isPermissionError() const;
};

struct JobInput {
    JobKind        kind        = JobKind::Create;
    quint64        defaultSgid = 0;
    quint64        sgid        = 0;   // Repair: the group
    QSet<quint64>  knownGroups;       // Create: the group ids known before the add
    OwnPowers      own;
    IconProblem    iconSkip    = IconProblem::None; // known before: no b_icon_manage, or icons too large
    int            maxIconSize = 0;
    QSet<QString>  refused;           // refused for this group earlier this session: not sent again
};

struct JobResult {
    bool          success   = false; // ran to the read-back
    ErrorKind     error     = ErrorKind::None;
    QString       errorDetail;       // %1 of the error text
    JobKind       kind      = JobKind::Create; // Repair when Create found the group
    quint64       sgid      = 0;
    bool          created   = false; // this job added the group
    bool          removed   = false; // ... and deleted it again
    bool          rowsKnown = false;
    PermRows      groupRows;         // the read-back
    bool          defaultRowsKnown = false;
    PermRows      defaultRows;
    QSet<QString> refused;           // names the server refused during this job
    IconProblem   iconProblem = IconProblem::None;
    QString       iconDetail;
    int           maxIconSize = 0;
    int           sentPermissions = 0;
};

class Job
{
  public:
    explicit Job(const JobInput& input);

    Command start();
    Command next(const Answer& answer); // the answer to the command handed out last
    Command abort(ErrorKind error, const QString& detail = {}); // e.g. the connection was lost
    void    noteRefused(const QString& permission);              // a late permission error of an earlier request

    bool             isFinished() const { return m_phase == Phase::Done; }
    const JobResult& result() const { return m_result; }
    JobKind          kind() const { return m_kind; }
    quint64          sgid() const { return m_result.sgid; }
    int              step() const;  // 1-based
    int              steps() const; // 4 for Create, 3 for Repair
    StepKind         stepKind() const;

  private:
    enum class Phase { Start, ReadDefault, AddGroup, FindGroup, ReadGroup, Protect, Body, Icon, CheckIcon, IconPerm, ModifyProtect, ReadBack, Delete, Done };
    enum class DeleteReason { Protect, NoFilePermissions };

    Command readGroup();
    Command protect();
    Command body();
    Command icon();
    Command modifyProtect();
    Command readBack();
    Command finish(ErrorKind error, const QString& detail = {});
    Command finishSuccess();
    Command deleteGroup(DeleteReason reason, const QString& detail);
    Command fromRead(const Answer& answer, ErrorKind permissionError);
    void    collectRefused(const Answer& answer, const QList<PlanItem>& sent);
    bool    needs(ItemKind kind) const; // Repair: is something of this kind missing?
    quint64 newGroupIn(const QList<GroupInfo>& groups, bool anyId) const;
    QString detailOf(const Answer& answer) const;

    JobInput        m_input;
    JobKind         m_kind;
    Phase           m_phase = Phase::Start;
    Plan            m_plan;
    Missing         m_missing; // Repair: from the group's rows
    QList<PlanItem> m_pending; // Body: what is still to send
    QList<PlanItem> m_sent;    // the items of the last AddPerms
    bool            m_duplicate = false; // the add said "exists": find the group by name instead
    DeleteReason    m_deleteReason = DeleteReason::Protect;
    QString         m_deleteDetail;
    JobResult       m_result;
};

} // namespace access
