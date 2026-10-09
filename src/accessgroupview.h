#pragma once

// 2.2 servergroup: what the "Server access" box shows. Every state, its exact copy, which buttons it
// offers and the texts of the Details window and of the chat lines, as pure functions of plain data so
// the unit tests can check them. AccessGroup fills ViewInput, AccessGroupBox renders the View.

#include <QList>
#include <QString>
#include <QStringList>

#include "accessgroupjob.h"
#include "accessgroupplan.h"

namespace access {

enum class State {
    NotConnected,      // A
    Checking,          // B
    NoGroup,           // C: you can create it (or it's unknown whether you can)
    NoGroupCantCreate, // D
    Working,           // E
    Ready,             // F
    ReadyNoIcon,       // G
    NeedsRepair,       // H
    Member,            // I: the group exists, you can't manage it, you're in it
    NotMember,         // I: ... and you're not in it
    Error,             // J
    NotProtected,      // L
};

enum class ErrorContext { Create, Repair, Check };

// What the primary button does.
enum class Action { None, Create, Repair, RetryCreate, RetryRepair, RetryCheck };

// Everything classify() needs, straight from AccessGroup's per-server state.
struct Facts {
    bool        connected    = false;
    bool        checked      = false; // a check of this server finished at least once
    bool        checking     = false; // and one runs now that should show as "Checking" (first or explicit)
    bool        jobRunning   = false;
    ErrorKind   error        = ErrorKind::None;
    bool        groupFound   = false;
    bool        canManage    = false; // the group's permissions could be read
    bool        canCreate    = true;  // false only when b_virtualserver_servergroup_create is known 0
    bool        isMember     = false;
    bool        isProtected  = false;
    Missing     missing;
    IconProblem iconProblem  = IconProblem::None;
};
State classify(const Facts& facts);

// The visible part of the missing permissions: i_group_needed_modify_power alone never makes a group
// "missing something", it is only added along with a repair.
QList<PlanItem> visibleMissing(const QList<PlanItem>& items);

struct ViewInput {
    State        state = State::NotConnected;
    QString      group;              // the group's name on the server ("tsmediachat" before it exists)
    QString      defaultGroup;       // the default group's name ("Guest"); empty when unknown
    QString      channel;            // your channel's name; empty when unknown
    int          members = -1;       // -1 = unknown
    int          uploadPower = kFilePower;    // the group's i_ft_file_upload_power
    int          channelNeededUpload = -1;    // your channel's i_ft_needed_file_upload_power, -1 = unknown
    bool         defaultCanUploadHere = false; // the default group can already upload in your channel
    bool         compact = false;              // regular users: no intro, no buttons
    int          protection = kMaxProtection;  // the power the protection sets
    // Working
    JobKind      job = JobKind::Create;
    int          step = 1;
    int          steps = 4;
    StepKind     stepKind = StepKind::CreateGroup;
    // ReadyNoIcon
    IconProblem  iconProblem = IconProblem::None;
    QString      iconDetail;
    int          maxIconSize = 0;
    // NeedsRepair
    Missing      missing;
    // Error
    ErrorKind    error = ErrorKind::None;
    ErrorContext errorContext = ErrorContext::Create;
    QString      errorDetail;
};

struct View {
    enum class Icon { None, Group, Warning };
    QString status;
    QString detail;
    QString extra;            // an optional second hint line
    Icon    icon          = Icon::None;
    bool    detailIsError = false;
    bool    showIntro     = true;
    bool    showServer    = true;
    bool    showButtons   = true;
    QString primaryText;      // empty: no primary button
    Action  primary       = Action::None;
    bool    primaryEnabled = false;
    bool    showDetails    = false;
    bool    showCheckAgain = false;
};
View buildView(const ViewInput& in);

QString introText();
QString notConnectedServerText(); // the Server row without a connection
QString membersText(int members); // "no members yet", "1 member", "3 members"; empty when unknown
QString stepText(StepKind step);
QString iconProblemText(IconProblem problem, const QString& detail, int maxIconSize);
QString errorText(ErrorKind error, const QString& detail, const QString& defaultGroup);
// "uploading files, the upload quota, 12 permissions copied from “Guest”"
QString missingListText(const QList<PlanItem>& items, const QString& defaultGroup);

// The Details window (a plain-text message box).
struct DetailsInput {
    bool             create = true;  // nothing exists yet: what Create will make
    QString          server;
    QString          defaultGroup;
    QString          status;         // Repair / Ready: the status line
    Plan             plan;
    bool             planKnown = false; // the default group could be read
    PermRows         groupRows;
    bool             rowsKnown = false;
    QSet<QString>    refused;
    QList<GroupInfo> ignored;        // icon / remembered matches with other permissions
    QList<GroupInfo> sameName;       // further groups named tsmediachat
    quint64          adopted = 0;
};
struct Details {
    QString title;
    QString text;
    QString informative;
    QString detailed;
};
Details buildDetails(const DetailsInput& in);

// Chat lines after a job, printed into that server's chat.
struct ChatLines {
    QStringList info;
    QStringList warnings;
};
ChatLines chatLinesFor(const JobResult& result, JobKind started, const QString& group, const QString& defaultGroup, bool completeAfter);

// Give / Remove TS Media chat access (client context menu).
enum class MemberOutcome { Added, AlreadyMember, Removed, NotMember, NoGroup, ServerQuery, Gone, NoPermission, Failed };
QString memberText(MemberOutcome outcome, bool give, const QString& person, const QString& group, const QString& detail = {});
QString memberMissingCopiesText(const QString& person, const QString& group, const QString& defaultGroup);

} // namespace access
