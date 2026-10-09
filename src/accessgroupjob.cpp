#include "accessgroupjob.h"

namespace access {

namespace {

Command command(Command::Type type, quint64 sgid = 0)
{
    Command c;
    c.type = type;
    c.sgid = sgid;
    return c;
}

QList<PlanItem> ofKinds(const QList<PlanItem>& items, std::initializer_list<ItemKind> kinds)
{
    QList<PlanItem> result;
    for (const PlanItem& i : items) {
        for (ItemKind k : kinds) {
            if (i.kind == k) {
                result.append(i);
                break;
            }
        }
    }
    return result;
}

bool contains(const QStringList& names, const char* name)
{
    return names.contains(QString::fromLatin1(name));
}

} // namespace

// ---- Answer ---------------------------------------------------------------------------------------

Answer Answer::success()
{
    Answer a;
    a.ok = true;
    return a;
}

Answer Answer::failure(unsigned int error, const QString& message, const QStringList& failedPerms)
{
    Answer a;
    a.error       = error;
    a.message     = message;
    a.failedPerms = failedPerms;
    return a;
}

Answer Answer::timedOut()
{
    Answer a;
    a.timeout = true;
    return a;
}

bool Answer::isEmptyResult() const
{
    return !ok && !timeout && (error == err::databaseEmptyResult || error == err::permissionEmptyResult);
}

bool Answer::isPermissionError() const
{
    return !ok && !timeout
           && (error == err::clientInsufficient || error == err::insufficientGroupPower || error == err::insufficientPermPower || !failedPerms.isEmpty());
}

// ---- Job ------------------------------------------------------------------------------------------

Job::Job(const JobInput& input)
    : m_input(input)
    , m_kind(input.kind == JobKind::Repair && input.sgid == 0 ? JobKind::Create : input.kind)
{
    m_result.kind        = m_kind;
    m_result.sgid        = m_kind == JobKind::Repair ? input.sgid : 0;
    m_result.maxIconSize = input.maxIconSize;
}

Command Job::start()
{
    if (m_phase != Phase::Start)
        return command(Command::Type::None);
    if (m_input.defaultSgid == 0)
        return finish(ErrorKind::NoDefaultGroup);
    m_phase = Phase::ReadDefault;
    return command(Command::Type::ReadPerms, m_input.defaultSgid);
}

Command Job::abort(ErrorKind error, const QString& detail)
{
    if (isFinished())
        return command(Command::Type::Finished);
    return finish(error, detail);
}

void Job::noteRefused(const QString& permission)
{
    if (!permission.isEmpty())
        m_result.refused.insert(permission);
}

Command Job::next(const Answer& answer)
{
    switch (m_phase) {
    case Phase::Start:
    case Phase::Done:
        return command(isFinished() ? Command::Type::Finished : Command::Type::None);

    case Phase::ReadDefault: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (!answer.ok && !answer.isEmptyResult())
            return finish(answer.isPermissionError() ? ErrorKind::CantReadPermissions : ErrorKind::Refused, detailOf(answer));
        m_result.defaultRows      = answer.ok ? answer.rows : PermRows();
        m_result.defaultRowsKnown = true;
        m_plan                    = plan(m_result.defaultRows, m_input.own);
        if (m_kind == JobKind::Repair)
            return readGroup();
        m_phase = Phase::AddGroup;
        return command(Command::Type::AddGroup);
    }

    case Phase::AddGroup: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (answer.ok) {
            if (const quint64 id = newGroupIn(answer.groups, false)) {
                m_result.created = true;
                m_result.sgid    = id;
                return protect();
            }
            m_phase = Phase::FindGroup; // the server hasn't pushed the new list yet: ask for it
            return command(Command::Type::ListGroups);
        }
        if (answer.error == err::databaseDuplicateEntry || answer.error == err::permissionDuplicate) {
            // Another admin or tab was faster: continue as a repair of that group.
            m_duplicate         = true;
            m_result.kind       = JobKind::Repair;
            m_deleteDetail      = detailOf(answer);
            if (const quint64 id = newGroupIn(answer.groups, true)) {
                m_result.sgid = id;
                return readGroup();
            }
            m_phase = Phase::FindGroup;
            return command(Command::Type::ListGroups);
        }
        return finish(answer.isPermissionError() ? ErrorKind::NoCreatePermission : ErrorKind::Refused, detailOf(answer));
    }

    case Phase::FindGroup: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        const quint64 id = newGroupIn(answer.groups, m_duplicate);
        if (!id)
            return m_duplicate ? finish(ErrorKind::Refused, m_deleteDetail) : finish(ErrorKind::CreatedNotFound);
        m_result.sgid = id;
        if (m_duplicate)
            return readGroup();
        m_result.created = true;
        return protect();
    }

    case Phase::ReadGroup: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (!answer.ok && !answer.isEmptyResult())
            return finish(answer.isPermissionError() ? ErrorKind::CantReadPermissions : ErrorKind::Refused, detailOf(answer));
        m_result.groupRows = answer.ok ? answer.rows : PermRows();
        m_result.rowsKnown = true;
        m_missing          = missing(m_result.groupRows, m_plan, m_input.refused);
        return protect();
    }

    case Phase::Protect: {
        collectRefused(answer, m_sent);
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (answer.ok && answer.failedPerms.isEmpty())
            return body();
        // Nothing else is sent after a refused protection: an unprotected group with file permissions
        // could be an upload ticket for anyone who can add themselves to it.
        if (m_result.created)
            return deleteGroup(DeleteReason::Protect, detailOf(answer));
        return finish(ErrorKind::Refused, detailOf(answer));
    }

    case Phase::Body:
        collectRefused(answer, m_sent);
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        return body(); // a refused chunk doesn't stop the rest: the read-back tells what landed

    case Phase::Icon: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (answer.ok || answer.error == err::fileAlreadyExists || answer.error == err::fileAlreadyInUse) {
            m_phase = Phase::IconPerm; // the same name means the same CRC, so the same bytes
            m_sent  = m_plan.icon;
            ++m_result.sentPermissions;
            Command c         = command(Command::Type::AddPerms, m_result.sgid);
            c.perms           = m_sent;
            c.continueOnError = false;
            return c;
        }
        if (contains(answer.failedPerms, "b_icon_manage"))
            m_result.iconProblem = IconProblem::NoManagePermission;
        else if (contains(answer.failedPerms, "i_max_icon_filesize"))
            m_result.iconProblem = IconProblem::TooLarge;
        else {
            m_result.iconProblem = IconProblem::UploadFailed;
            m_result.iconDetail  = detailOf(answer);
        }
        return modifyProtect();
    }

    case Phase::CheckIcon:
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (answer.ok && answer.fileSize == kGroupIconBytes) {
            // Someone else uploaded it already: only the permission is needed, and no b_icon_manage.
            m_phase = Phase::IconPerm;
            m_sent  = m_plan.icon;
            ++m_result.sentPermissions;
            Command c = command(Command::Type::AddPerms, m_result.sgid);
            c.perms   = m_sent;
            return c;
        }
        m_result.iconProblem = IconProblem::NoManagePermission;
        return modifyProtect();

    case Phase::IconPerm:
        collectRefused(answer, m_sent);
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (!answer.ok || !answer.failedPerms.isEmpty()) {
            m_result.iconProblem = IconProblem::Refused;
            m_result.iconDetail  = detailOf(answer);
        } else {
            m_result.iconProblem = IconProblem::None;
            m_result.iconDetail.clear();
        }
        return modifyProtect();

    case Phase::ModifyProtect:
        collectRefused(answer, m_sent);
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        return readBack();

    case Phase::ReadBack: {
        if (answer.timeout)
            return finish(ErrorKind::Timeout);
        if (!answer.ok && !answer.isEmptyResult())
            return finish(answer.isPermissionError() ? ErrorKind::CantReadPermissions : ErrorKind::Refused, detailOf(answer));
        m_result.groupRows = answer.ok ? answer.rows : PermRows();
        m_result.rowsKnown = true;
        if (m_result.created && !hasAnyFilePermission(m_result.groupRows)) {
            QStringList refusedFiles;
            for (const PlanItem& i : m_plan.body) {
                if (i.kind == ItemKind::FileSet && m_result.refused.contains(i.name))
                    refusedFiles << i.name;
            }
            return deleteGroup(DeleteReason::NoFilePermissions, refusedFiles.join(QLatin1String(", ")));
        }
        return finishSuccess();
    }

    case Phase::Delete:
        if (answer.ok) {
            m_result.removed   = true;
            m_result.rowsKnown = false;
            m_result.groupRows.clear();
            return finish(m_deleteReason == DeleteReason::Protect ? ErrorKind::ProtectRemoved : ErrorKind::NoFilePermsRemoved, m_deleteDetail);
        }
        if (m_deleteReason == DeleteReason::Protect)
            return finish(ErrorKind::ProtectLeftEmpty, m_deleteDetail);
        return finishSuccess(); // protected but without file permissions: the box shows what's missing
    }
    return command(Command::Type::None);
}

Command Job::readGroup()
{
    m_phase = Phase::ReadGroup;
    return command(Command::Type::ReadPerms, m_result.sgid);
}

Command Job::protect()
{
    const QList<PlanItem> items = m_result.created ? m_plan.protection : ofKinds(m_missing.sendable, {ItemKind::Protection});
    if (items.isEmpty())
        return body();
    m_phase = Phase::Protect;
    m_sent  = items;
    ++m_result.sentPermissions;
    Command c         = command(Command::Type::AddPerms, m_result.sgid);
    c.perms           = items;
    c.continueOnError = false; // both or nothing
    return c;
}

Command Job::body()
{
    if (m_phase != Phase::Body) {
        m_pending = m_result.created ? m_plan.body : ofKinds(m_missing.sendable, {ItemKind::FileSet, ItemKind::Quota, ItemKind::Copy});
        m_phase   = Phase::Body;
    }
    if (m_pending.isEmpty())
        return icon();
    m_sent = m_pending.mid(0, kMaxPermsPerRequest);
    m_pending.erase(m_pending.begin(), m_pending.begin() + m_sent.size());
    ++m_result.sentPermissions;
    Command c         = command(Command::Type::AddPerms, m_result.sgid);
    c.perms           = m_sent;
    c.continueOnError = true;
    return c;
}

Command Job::icon()
{
    if (!m_result.created && !needs(ItemKind::Icon))
        return modifyProtect();
    if (m_input.iconSkip == IconProblem::TooLarge) {
        m_result.iconProblem = IconProblem::TooLarge;
        return modifyProtect();
    }
    if (m_input.iconSkip == IconProblem::NoManagePermission) {
        m_phase = Phase::CheckIcon;
        return command(Command::Type::CheckIconFile, m_result.sgid);
    }
    m_phase = Phase::Icon;
    return command(Command::Type::UploadIcon, m_result.sgid);
}

Command Job::modifyProtect()
{
    QList<PlanItem> items = m_result.created ? m_plan.modifyProtection : ofKinds(m_missing.sendable, {ItemKind::ModifyProtection});
    for (int i = items.size() - 1; i >= 0; --i) {
        if (m_result.refused.contains(items.at(i).name))
            items.removeAt(i);
    }
    if (items.isEmpty())
        return readBack();
    m_phase = Phase::ModifyProtect;
    m_sent  = items;
    ++m_result.sentPermissions;
    Command c = command(Command::Type::AddPerms, m_result.sgid);
    c.perms   = items;
    return c;
}

Command Job::readBack()
{
    m_phase = Phase::ReadBack;
    return command(Command::Type::ReadPerms, m_result.sgid);
}

Command Job::deleteGroup(DeleteReason reason, const QString& detail)
{
    m_phase        = Phase::Delete;
    m_deleteReason = reason;
    m_deleteDetail = detail;
    return command(Command::Type::DeleteGroup, m_result.sgid);
}

Command Job::finish(ErrorKind error, const QString& detail)
{
    m_phase              = Phase::Done;
    m_result.success     = false;
    m_result.error       = error;
    m_result.errorDetail = detail;
    return command(Command::Type::Finished);
}

Command Job::finishSuccess()
{
    m_phase          = Phase::Done;
    m_result.success = true;
    m_result.error   = ErrorKind::None;
    m_result.errorDetail.clear();
    return command(Command::Type::Finished);
}

void Job::collectRefused(const Answer& answer, const QList<PlanItem>& sent)
{
    Q_UNUSED(sent);
    for (const QString& name : answer.failedPerms)
        noteRefused(name);
}

bool Job::needs(ItemKind kind) const
{
    for (const PlanItem& i : m_missing.sendable) {
        if (i.kind == kind)
            return true;
    }
    return false;
}

quint64 Job::newGroupIn(const QList<GroupInfo>& groups, bool anyId) const
{
    quint64 best = 0;
    for (const GroupInfo& g : groups) {
        if (g.type != kGroupType || g.id == 0 || g.id == m_input.defaultSgid || !isOurName(g.name))
            continue;
        if (!anyId && m_input.knownGroups.contains(g.id))
            continue;
        if (!best || g.id < best)
            best = g.id;
    }
    return best;
}

QString Job::detailOf(const Answer& answer) const
{
    if (!answer.failedPerms.isEmpty())
        return answer.failedPerms.join(QLatin1String(", "));
    if (!answer.message.isEmpty())
        return answer.message;
    return QString::fromLatin1("error 0x%1").arg(answer.error, 4, 16, QLatin1Char('0'));
}

int Job::step() const
{
    int create = 1;
    switch (m_phase) {
    case Phase::Start:
    case Phase::ReadDefault:
    case Phase::AddGroup:
    case Phase::FindGroup:
        create = 1;
        break;
    case Phase::ReadGroup:
    case Phase::Protect:
    case Phase::Body:
        create = 2;
        break;
    case Phase::Icon:
    case Phase::CheckIcon:
    case Phase::IconPerm:
        create = 3;
        break;
    case Phase::ModifyProtect:
    case Phase::ReadBack:
    case Phase::Delete:
    case Phase::Done:
        create = 4;
        break;
    }
    return m_kind == JobKind::Create ? create : qMax(1, create - 1);
}

int Job::steps() const
{
    return m_kind == JobKind::Create ? 4 : 3;
}

StepKind Job::stepKind() const
{
    const int s = m_kind == JobKind::Create ? step() : step() + 1;
    switch (s) {
    case 1:
        return StepKind::CreateGroup;
    case 2:
        return StepKind::AddPermissions;
    case 3:
        return StepKind::AddIcon;
    default:
        return StepKind::Checking;
    }
}

} // namespace access
