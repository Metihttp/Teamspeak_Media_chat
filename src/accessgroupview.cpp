#include "accessgroupview.h"

#include "i18n.h"

namespace access {

namespace {

// Server-supplied names (groups, channels, servers) are substituted with the multi-argument arg() only:
// a name containing "%2" must not be filled in by the next arg() call.

QString quoted(const QString& name)
{
    return i18n::t("“%1”").arg(name);
}

// “Guest” or "the default group" when its name isn't known.
QString defaultGroupName(const QString& defaultGroup)
{
    return defaultGroup.isEmpty() ? i18n::t("the default group") : quoted(defaultGroup);
}

QString possessive(const QString& defaultGroup)
{
    return defaultGroup.isEmpty() ? i18n::t("the default group's") : i18n::t("“%1”'s").arg(defaultGroup);
}

QString createButtonText()
{
    return i18n::t("Create TS Media c&hat group");
}

QString repairButtonText()
{
    return i18n::t("Repair TS Media c&hat group");
}

bool allOfKind(const QList<PlanItem>& items, ItemKind kind)
{
    if (items.isEmpty())
        return false;
    for (const PlanItem& i : items) {
        if (i.kind != kind)
            return false;
    }
    return true;
}

QString refusedSentence(const QList<PlanItem>& refused)
{
    const QList<PlanItem> visible = visibleMissing(refused);
    if (visible.isEmpty())
        return {};
    QStringList names;
    for (const PlanItem& i : visible)
        names << i.name;
    return i18n::t("%1 can only be added by an admin with higher server permissions (%2).").arg(QString::number(visible.size()), names.join(QLatin1String(", ")));
}

QString valueText(const PermValue& v)
{
    QString text = QString::number(v.value);
    if (v.negated)
        text += i18n::t(" (negated)");
    if (v.skip)
        text += i18n::t(" (skip)");
    return text;
}

} // namespace

State classify(const Facts& f)
{
    if (!f.connected)
        return State::NotConnected;
    if (f.jobRunning)
        return State::Working;
    if (f.error != ErrorKind::None)
        return State::Error;
    if (!f.checked || f.checking)
        return State::Checking;
    if (!f.groupFound)
        return f.canCreate ? State::NoGroup : State::NoGroupCantCreate;
    if (!f.canManage)
        return f.isMember ? State::Member : State::NotMember;
    if (!f.isProtected)
        return State::NotProtected;
    const QList<PlanItem> visible = visibleMissing(f.missing.sendable + f.missing.refused);
    if (visible.isEmpty())
        return State::Ready;
    bool iconRefused = false;
    for (const PlanItem& i : f.missing.refused)
        iconRefused = iconRefused || i.kind == ItemKind::Icon;
    if (allOfKind(visible, ItemKind::Icon) && (f.iconProblem != IconProblem::None || iconRefused))
        return State::ReadyNoIcon;
    return State::NeedsRepair;
}

QList<PlanItem> visibleMissing(const QList<PlanItem>& items)
{
    QList<PlanItem> result;
    for (const PlanItem& i : items) {
        if (i.kind != ItemKind::ModifyProtection)
            result.append(i);
    }
    return result;
}

QString introText()
{
    return i18n::t("For server admins: a server group whose members can send files with TS Media chat. Changes here are made on the server right away.");
}

QString notConnectedServerText()
{
    return i18n::t("Not connected");
}

QString membersText(int members)
{
    if (members < 0)
        return {};
    if (members == 0)
        return i18n::t("no members yet");
    if (members == 1)
        return i18n::t("1 member");
    return i18n::t("%1 members").arg(members);
}

QString stepText(StepKind step)
{
    switch (step) {
    case StepKind::CreateGroup:
        return i18n::t("creating the group");
    case StepKind::AddPermissions:
        return i18n::t("adding permissions");
    case StepKind::AddIcon:
        return i18n::t("adding the icon");
    case StepKind::Checking:
        return i18n::t("checking");
    }
    return {};
}

QString iconProblemText(IconProblem problem, const QString& detail, int maxIconSize)
{
    switch (problem) {
    case IconProblem::None:
        return {};
    case IconProblem::NoManagePermission:
        return i18n::t("you need permission to manage icons (b_icon_manage)");
    case IconProblem::TooLarge:
        return i18n::t("the server allows icons up to %1 bytes (i_max_icon_filesize)").arg(maxIconSize);
    case IconProblem::UploadFailed:
        return i18n::t("the upload failed (%1)").arg(detail);
    case IconProblem::Refused:
        return i18n::t("the server didn't accept it (%1)").arg(detail);
    }
    return {};
}

QString errorText(ErrorKind error, const QString& detail, const QString& defaultGroup)
{
    switch (error) {
    case ErrorKind::None:
        return {};
    case ErrorKind::NoCreatePermission:
        return i18n::t("You don't have permission to create server groups on this server (b_virtualserver_servergroup_create). Ask a server admin.");
    case ErrorKind::CantReadPermissions:
        return i18n::t("You can't read the permissions of %1 (b_virtualserver_servergroup_permission_list). TS Media chat copies them so members keep what they could do before.")
            .arg(defaultGroupName(defaultGroup));
    case ErrorKind::NoDefaultGroup:
        return i18n::t("Couldn't find this server's default group, so TS Media chat can't copy its permissions.");
    case ErrorKind::NotLoaded:
        return i18n::t("TeamSpeak hasn't finished loading this server's permissions. Wait a moment and click Try again.");
    case ErrorKind::ProtectRemoved:
        return i18n::t("The group couldn't be protected (%1), so TS Media chat removed it again. Nothing else was changed.").arg(detail);
    case ErrorKind::ProtectLeftEmpty:
        return i18n::t("The group couldn't be protected (%1) and couldn't be removed. It has no permissions; delete it in Permissions > Server Groups or click Try again.")
            .arg(detail);
    case ErrorKind::NoFilePermsRemoved:
        return detail.isEmpty() ? i18n::t("None of the file permissions could be added, so TS Media chat removed the group again.")
                                : i18n::t("None of the file permissions could be added (%1), so TS Media chat removed the group again.").arg(detail);
    case ErrorKind::Timeout:
        return i18n::t("The server didn't answer. Check your connection and click Try again.");
    case ErrorKind::ConnectionLost:
        return i18n::t("The connection to the server was lost. Reconnect and click Try again.");
    case ErrorKind::CreatedNotFound:
        return i18n::t("The group was created, but TS Media chat couldn't find it. Click Check again.");
    case ErrorKind::Refused:
        return i18n::t("The server refused it: %1").arg(detail);
    }
    return {};
}

QString missingListText(const QList<PlanItem>& items, const QString& defaultGroup)
{
    QStringList parts;
    int         copies = 0;
    const auto  has    = [&items](const char* name) {
        for (const PlanItem& i : items) {
            if (i.name == QLatin1String(name))
                return true;
        }
        return false;
    };
    if (has("i_ft_file_upload_power"))
        parts << i18n::t("uploading files");
    if (has("i_ft_file_download_power"))
        parts << i18n::t("downloading files");
    if (has("i_ft_file_browse_power"))
        parts << i18n::t("browsing files");
    if (has("i_ft_directory_create_power"))
        parts << i18n::t("creating folders");
    if (has("i_ft_quota_mb_upload_per_client"))
        parts << i18n::t("the upload quota");
    if (has("i_ft_quota_mb_download_per_client"))
        parts << i18n::t("the download quota");
    for (const PlanItem& i : items)
        copies += i.kind == ItemKind::Copy ? 1 : 0;
    if (copies == 1)
        parts << i18n::t("1 permission copied from %1").arg(defaultGroupName(defaultGroup));
    else if (copies > 1)
        parts << i18n::t("%1 permissions copied from %2").arg(QString::number(copies), defaultGroupName(defaultGroup));
    if (has("i_group_needed_member_add_power") || has("i_group_needed_member_remove_power"))
        parts << i18n::t("who can give it out");
    if (has("i_icon_id"))
        parts << i18n::t("the icon");
    return parts.join(QLatin1String(", "));
}

View buildView(const ViewInput& in)
{
    View v;
    const QString group = in.group.isEmpty() ? groupName() : in.group;
    v.showIntro         = !in.compact;
    v.showServer        = !in.compact;
    v.showButtons       = !in.compact;

    switch (in.state) {
    case State::NotConnected:
        v.status      = i18n::t("Connect to a server to set up its TS Media chat group.");
        v.primaryText = createButtonText();
        v.primary     = Action::Create;
        v.showIntro = v.showServer = v.showButtons = true;
        break;

    case State::Checking:
        v.status      = i18n::t("Checking the server…");
        v.primaryText = createButtonText();
        v.primary     = Action::Create;
        break;

    case State::NoGroup:
        v.status = i18n::t("This server has no TS Media chat group yet.");
        v.detail = i18n::t("Creates the server group “tsmediachat” with the TS Media chat icon. Members keep what %1 allows and can also upload, download and browse "
                           "files and create folders. Afterwards, right-click anyone and choose Server Groups > tsmediachat to give it to them.")
                       .arg(defaultGroupName(in.defaultGroup));
        if (in.defaultCanUploadHere && !in.channel.isEmpty())
            v.extra = i18n::t("Everyone in %1 can already send files in “%2”. The group is only needed where they can't.").arg(defaultGroupName(in.defaultGroup), in.channel);
        v.primaryText    = createButtonText();
        v.primary        = Action::Create;
        v.primaryEnabled = true;
        v.showDetails    = true;
        break;

    case State::NoGroupCantCreate:
        v.status    = i18n::t("This server has no TS Media chat group yet.");
        v.detail    = i18n::t("Only a server admin can create it: you don't have permission to create server groups here (b_virtualserver_servergroup_create).");
        v.showIntro = v.showServer = v.showButtons = false;
        break;

    case State::Working:
        if (in.job == JobKind::Create) {
            v.status      = i18n::t("Creating “tsmediachat”… step %1 of 4: %2").arg(QString::number(in.step), stepText(in.stepKind));
            v.primaryText = i18n::t("Creating…");
        } else {
            v.status = i18n::t("Repairing “%1”… step %2 of %3: %4").arg(group, QString::number(in.step), QString::number(in.steps), stepText(in.stepKind));
            v.primaryText = i18n::t("Repairing…");
        }
        v.primary   = Action::None;
        v.showIntro = v.showServer = v.showButtons = true;
        break;

    case State::Ready: {
        const QString members = membersText(in.members);
        v.status = members.isEmpty() ? i18n::t("“%1” is ready").arg(group) : i18n::t("“%1” is ready · %2").arg(group, members);
        v.icon   = View::Icon::Group;
        v.detail = i18n::t("Members can send files in channels that need upload power %1 or less. To give access, right-click a person and choose Server Groups > %2.")
                       .arg(QString::number(in.uploadPower), group);
        if (!in.channel.isEmpty() && in.channelNeededUpload > in.uploadPower)
            v.extra = i18n::t("In “%1”, uploading needs power %2, so members can't send files there.").arg(in.channel, QString::number(in.channelNeededUpload));
        v.showDetails    = true;
        v.showCheckAgain = true;
        break;
    }

    case State::ReadyNoIcon: {
        const QString members = membersText(in.members);
        v.status = members.isEmpty() ? i18n::t("“%1” is ready, without its icon").arg(group) : i18n::t("“%1” is ready, without its icon · %2").arg(group, members);
        v.detail = i18n::t("The icon couldn't be added: %1. Everything else works.").arg(iconProblemText(in.iconProblem, in.iconDetail, in.maxIconSize));
        bool iconSendable = false;
        for (const PlanItem& i : in.missing.sendable)
            iconSendable = iconSendable || i.kind == ItemKind::Icon;
        if (iconSendable) {
            v.primaryText    = i18n::t("Add TS Media c&hat icon");
            v.primary        = Action::Repair;
            v.primaryEnabled = true;
        }
        v.showDetails    = true;
        v.showCheckAgain = true;
        break;
    }

    case State::NeedsRepair: {
        const QList<PlanItem> sendable = visibleMissing(in.missing.sendable);
        const QList<PlanItem> all      = visibleMissing(in.missing.sendable + in.missing.refused);
        v.icon = View::Icon::Warning;
        if (allOfKind(all, ItemKind::Copy)) {
            v.status = i18n::t("“%1” is missing some of %2 permissions").arg(group, possessive(in.defaultGroup));
            v.detail = i18n::t("Members who are only in this group can't do everything %1 allows. Repair copies the missing ones again.").arg(defaultGroupName(in.defaultGroup));
        } else {
            v.status = i18n::t("“%1” is missing some permissions").arg(group);
            v.detail = i18n::t("Members may not be able to send files. Missing: %1. Repair adds what's missing and keeps permissions you set yourself.")
                           .arg(missingListText(all, in.defaultGroup));
        }
        const QString refused = refusedSentence(in.missing.refused);
        if (!refused.isEmpty())
            v.detail += QLatin1Char(' ') + refused;
        if (!sendable.isEmpty()) {
            v.primaryText    = repairButtonText();
            v.primary        = Action::Repair;
            v.primaryEnabled = true;
        }
        v.showDetails    = true;
        v.showCheckAgain = true;
        break;
    }

    case State::NotProtected:
        v.icon           = View::Icon::Warning;
        v.detailIsError  = true;
        v.status         = i18n::t("“%1” isn't protected").arg(group);
        v.detail         = i18n::t("Anyone may be able to add themselves to it. Repair sets who can give it out (group power %1 or more).").arg(in.protection);
        v.primaryText    = repairButtonText();
        v.primary        = Action::Repair;
        v.primaryEnabled = true;
        v.showDetails    = true;
        v.showCheckAgain = true;
        break;

    case State::Member:
        v.icon      = View::Icon::Group;
        v.status    = i18n::t("You're in “%1”, so you can send files on this server.").arg(group);
        v.showIntro = v.showServer = v.showButtons = false;
        break;

    case State::NotMember:
        v.status    = i18n::t("This server has a TS Media chat group, “%1”. Ask a server admin to give it to you so you can send files.").arg(group);
        v.showIntro = v.showServer = v.showButtons = false;
        break;

    case State::Error:
        v.icon          = View::Icon::Warning;
        v.detailIsError = true;
        switch (in.errorContext) {
        case ErrorContext::Create:
            v.status  = i18n::t("Couldn't create “tsmediachat”");
            v.primary = Action::RetryCreate;
            break;
        case ErrorContext::Repair:
            v.status  = i18n::t("Couldn't repair “%1”").arg(group);
            v.primary = Action::RetryRepair;
            break;
        case ErrorContext::Check:
            v.status  = i18n::t("Couldn't check the server");
            v.primary = Action::RetryCheck;
            break;
        }
        v.detail         = errorText(in.error, in.errorDetail, in.defaultGroup);
        v.primaryText    = i18n::t("Try again");
        v.primaryEnabled = true;
        v.showCheckAgain = true;
        v.showIntro = v.showServer = v.showButtons = true;
        break;
    }
    if (!v.showButtons) {
        v.primaryText.clear();
        v.primary        = Action::None;
        v.primaryEnabled = false;
        v.showDetails    = false;
        v.showCheckAgain = false;
    }
    return v;
}

Details buildDetails(const DetailsInput& in)
{
    Details d;
    d.title = i18n::t("TS Media chat group");
    QStringList detailed;
    if (in.create) {
        d.text = i18n::t("TS Media chat will create on %1:").arg(in.server);
        QStringList lines;
        lines << i18n::t("Name: tsmediachat (regular server group)");
        lines << i18n::t("Icon: TS Media chat picture icon (16×16, %1 bytes)").arg(kGroupIconBytes);
        lines << i18n::t("File permissions at power %1: upload, download, browse, create folders").arg(kFilePower);
        if (in.planKnown)
            lines << i18n::t("Copied from %1: %2 permissions (restrictions marked negate aren't copied)").arg(defaultGroupName(in.defaultGroup), QString::number(in.plan.copies));
        lines << i18n::t("Only admins with group power %1 or more can give or remove it").arg(in.plan.protectionValue());
        d.informative = lines.join(QLatin1Char('\n'));
        for (const PlanItem& i : in.plan.all())
            detailed << i18n::t("%1 = %2").arg(i.name, valueText(i.value));
    } else {
        d.text = in.status;
        QStringList lines;
        for (const GroupInfo& g : in.sameName)
            lines << i18n::t("Group “%1” (id %2) has the same name. TS Media chat uses the one with id %3.").arg(g.name, QString::number(g.id), QString::number(in.adopted));
        for (const GroupInfo& g : in.ignored)
            lines << i18n::t("Group “%1” (id %2) uses the TS Media chat icon but has other permissions, so TS Media chat ignores it.").arg(g.name, QString::number(g.id));
        d.informative = lines.join(QLatin1Char('\n'));
        if (in.rowsKnown) {
            QSet<QString> listed;
            for (const PlanItem& i : in.plan.all()) {
                listed.insert(i.name);
                const auto it = in.groupRows.constFind(i.name);
                if (it != in.groupRows.cend())
                    detailed << i18n::t("%1 = %2").arg(i.name, valueText(it.value()));
                else if (in.refused.contains(i.name))
                    detailed << i18n::t("%1: needs higher server permissions").arg(i.name);
                else
                    detailed << i18n::t("%1: missing").arg(i.name);
            }
            for (auto it = in.groupRows.cbegin(); it != in.groupRows.cend(); ++it) {
                if (!listed.contains(it.key()))
                    detailed << i18n::t("%1 = %2 (not set by TS Media chat)").arg(it.key(), valueText(it.value()));
            }
        }
    }
    d.detailed = detailed.join(QLatin1Char('\n'));
    return d;
}

ChatLines chatLinesFor(const JobResult& result, JobKind started, const QString& group, const QString& defaultGroup, bool completeAfter)
{
    ChatLines lines;
    const QString name = group.isEmpty() ? groupName() : group;
    if (result.success) {
        if (result.created) {
            if (hasOurIcon(result.groupRows)) {
                lines.info << i18n::t("Created the server group “tsmediachat”. To let someone send files, right-click them and choose Server Groups > tsmediachat.");
            } else {
                QString why = iconProblemText(result.iconProblem, result.iconDetail, result.maxIconSize);
                if (why.isEmpty())
                    why = i18n::t("it couldn't be added");
                lines.info << i18n::t("Created the server group “tsmediachat” without its icon (%1).").arg(why);
            }
        } else if (completeAfter) {
            lines.info << i18n::t("Repaired the server group “%1”.").arg(name);
        }
        return lines;
    }
    if (result.error == ErrorKind::ProtectRemoved) {
        lines.warnings << i18n::t("The server group “tsmediachat” couldn't be protected (%1), so TS Media chat removed it again.").arg(result.errorDetail);
    } else if (started == JobKind::Create && result.kind == JobKind::Create) {
        lines.warnings << i18n::t("Couldn't create the server group “tsmediachat”: %1").arg(errorText(result.error, result.errorDetail, defaultGroup));
    }
    return lines;
}

QString memberText(MemberOutcome outcome, bool give, const QString& person, const QString& group, const QString& detail)
{
    switch (outcome) {
    case MemberOutcome::Added:
        return i18n::t("%1 can now send files (added to “%2”).").arg(person, group);
    case MemberOutcome::AlreadyMember:
        return i18n::t("%1 already has TS Media chat access.").arg(person);
    case MemberOutcome::Removed:
        return i18n::t("%1 no longer has TS Media chat access (removed from “%2”).").arg(person, group);
    case MemberOutcome::NotMember:
        return i18n::t("%1 doesn't have TS Media chat access.").arg(person);
    case MemberOutcome::NoGroup:
        return i18n::t("This server has no TS Media chat group yet. Server admins can create it in TS Media chat settings, under Server access.");
    case MemberOutcome::ServerQuery:
        return i18n::t("%1 is a ServerQuery client and can't get TS Media chat access.").arg(person);
    case MemberOutcome::Gone:
        return i18n::t("That person is no longer on the server.");
    case MemberOutcome::NoPermission:
        return give ? i18n::t("Couldn't give %1 TS Media chat access: your server permissions don't allow it (%2).").arg(person, detail)
                    : i18n::t("Couldn't remove TS Media chat access from %1: your server permissions don't allow it (%2).").arg(person, detail);
    case MemberOutcome::Failed:
        return give ? i18n::t("Couldn't give %1 TS Media chat access: %2").arg(person, detail) : i18n::t("Couldn't remove TS Media chat access from %1: %2").arg(person, detail);
    }
    return {};
}

QString memberMissingCopiesText(const QString& person, const QString& group, const QString& defaultGroup)
{
    return i18n::t("Note: “%1” is missing some of %2 permissions, so %3 may lose some abilities. Repair it in TS Media chat settings.").arg(group, possessive(defaultGroup), person);
}

} // namespace access
