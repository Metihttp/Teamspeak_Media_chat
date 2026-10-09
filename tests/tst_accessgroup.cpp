// 2.2 servergroup: unit tests for the one-click "tsmediachat" server group: the icon and its CRC-32 id,
// the permission plan, missing() / conformant(), finding the group, the Create / Repair state machine
// (every branch driven by fake server answers) and the texts the box, the Details window and the chat
// show. Runs inside tsmedia_tests (tests/testregistry.h).

#include <QImage>
#include <QtTest>

#include <initializer_list>
#include <utility>

#include "accessgroupjob.h"
#include "accessgroupplan.h"
#include "accessgroupview.h"
#include "testregistry.h"

using namespace access;

namespace {

using Row = std::pair<const char*, int>;

PermRows rowsOf(std::initializer_list<Row> rows)
{
    PermRows result;
    for (const Row& r : rows) {
        PermValue v;
        v.value                                 = r.second;
        result[QString::fromLatin1(r.first)]    = v;
    }
    return result;
}

// The stock "Guest" server group (sql/defaults.sql of server 3.13.7, 31 rows).
PermRows guestRows()
{
    return rowsOf({{"b_channel_create_modify_with_codec_opusvoice", 1},
                   {"b_channel_create_temporary", 1},
                   {"b_channel_create_with_maxclients", 1},
                   {"b_channel_create_with_needed_talk_power", 1},
                   {"b_channel_create_with_password", 1},
                   {"b_channel_create_with_topic", 1},
                   {"b_channel_info_view", 1},
                   {"b_channel_join_permanent", 1},
                   {"b_channel_join_semi_permanent", 1},
                   {"b_channel_join_temporary", 1},
                   {"b_client_channel_textmessage_send", 1},
                   {"b_client_info_view", 1},
                   {"b_client_request_talker", 1},
                   {"b_virtualserver_token_use", 1},
                   {"i_channel_create_modify_with_codec_latency_factor_min", 1},
                   {"i_channel_create_modify_with_codec_maxquality", 7},
                   {"i_channel_max_depth", 0},
                   {"i_client_max_avatar_filesize", 200000},
                   {"i_client_max_channel_subscriptions", -1},
                   {"i_client_max_clones_uid", 0},
                   {"i_client_needed_ban_power", 25},
                   {"i_client_needed_kick_from_channel_power", 25},
                   {"i_client_needed_kick_from_server_power", 25},
                   {"i_client_needed_move_power", 25},
                   {"i_client_needed_serverquery_view_power", 75},
                   {"i_ft_file_browse_power", 25},
                   {"i_ft_file_download_power", 25},
                   {"i_ft_quota_mb_download_per_client", -1},
                   {"i_ft_quota_mb_upload_per_client", -1},
                   {"i_group_auto_update_type", 15},
                   {"i_group_needed_modify_power", 75}});
}

// Part of the stock "Server Admin" group: what an icon-id takeover would grab.
PermRows serverAdminRows()
{
    return rowsOf({{"b_virtualserver_servergroup_create", 1},
                   {"b_virtualserver_servergroup_delete", 1},
                   {"i_group_member_add_power", 75},
                   {"i_group_modify_power", 75},
                   {"i_permission_modify_power", 75},
                   {"i_client_permission_modify_power", 75},
                   {"i_needed_modify_power_ft_file_upload_power", 75},
                   {"i_ft_file_upload_power", 75},
                   {"i_ft_file_delete_power", 75},
                   {"i_group_needed_member_add_power", 75},
                   {"i_group_needed_member_remove_power", 75},
                   {"i_group_needed_modify_power", 75},
                   {"b_channel_join_permanent", 1}});
}

// What a group looks like on the server once the plan landed completely.
PermRows applied(const Plan& plan)
{
    PermRows rows;
    for (const PlanItem& i : plan.all())
        rows[i.name] = i.value;
    return rows;
}

GroupInfo group(quint64 id, const char* name, int type = kGroupType, quint32 icon = 0)
{
    GroupInfo g;
    g.id     = id;
    g.name   = QString::fromUtf8(name);
    g.type   = type;
    g.iconId = icon;
    return g;
}

QList<GroupInfo> stockGroups()
{
    return {group(1, "Guest Server Query", 2), group(2, "Admin Server Query", 2), group(3, "Server Admin", 0), group(4, "Normal", 0), group(5, "Guest", 0),
            group(6, "Server Admin", 1, 300), group(7, "Normal", 1), group(8, "Guest", 1)};
}

const PlanItem* find(const QList<PlanItem>& items, const char* name)
{
    for (const PlanItem& i : items) {
        if (i.name == QLatin1String(name))
            return &i;
    }
    return nullptr;
}

QStringList names(const QList<PlanItem>& items)
{
    QStringList result;
    for (const PlanItem& i : items)
        result << i.name;
    return result;
}

Answer rowsAnswer(const PermRows& rows)
{
    Answer a = Answer::success();
    a.rows   = rows;
    return a;
}

Answer groupsAnswer(const QList<GroupInfo>& groups, bool ok = true, unsigned int error = err::ok)
{
    Answer a = ok ? Answer::success() : Answer::failure(error, QStringLiteral("database duplicate entry"));
    a.groups = groups;
    return a;
}

JobInput createInput()
{
    JobInput in;
    in.kind        = JobKind::Create;
    in.defaultSgid = 8;
    for (const GroupInfo& g : stockGroups())
        in.knownGroups.insert(g.id);
    return in;
}

} // namespace

class TestAccessGroup : public QObject
{
    Q_OBJECT

  private slots:
    void crc32Vectors();
    void embeddedIcon();
    void planFromGuest();
    void planProtection();
    void planModifyProtection();
    void planExclusions();
    void planQuotas();
    void missingItems();
    void protectionAndFiles();
    void conformance();
    void serverGroupsParsing();
    void candidates();

    void jobCreateHappyPath();
    void jobProtectionRefusedRemoves();
    void jobProtectionRefusedLeftEmpty();
    void jobDuplicateBecomesRepair();
    void jobNewIdFromList();
    void jobCreatedNotFound();
    void jobNoFilePermissionsRemoves();
    void jobErrors();
    void jobRepairSendsOnlyMissing();
    void jobRepairProtectionRefused();
    void jobIconPaths();
    void jobChunks();
    void jobLateRefusals();

    void classifyStates();
    void viewTexts();
    void viewHostileNames();
    void detailsTexts();
    void chatLines();
    void memberTexts();
};

// ---- icon and CRC -----------------------------------------------------------------------------------

void TestAccessGroup::crc32Vectors()
{
    QCOMPARE(crc32(QByteArray("123456789")), 0xCBF43926u);
    QCOMPARE(crc32(QByteArray()), 0u);
    QCOMPARE(crc32(QByteArray("The quick brown fox jumps over the lazy dog")), 0x414FA339u);
}

void TestAccessGroup::embeddedIcon()
{
    const QByteArray icon = groupIcon16();
    QCOMPARE(icon.size(), kGroupIconBytes);
    QCOMPARE(crc32(icon), kGroupIconId); // the id on the server: must never change
    QCOMPARE(kGroupIconId, 2038059696u);
    QVERIFY(static_cast<qint32>(kGroupIconId) > 0); // i_icon_id and the file name use the same number
    QVERIFY(kGroupIconId >= 1000);                  // ids below 1000 are TeamSpeak's built-in icons
    const QImage small = QImage::fromData(icon);
    QCOMPARE(small.size(), QSize(16, 16));
    const QImage large = QImage::fromData(groupIcon32());
    QCOMPARE(large.size(), QSize(32, 32));
    QCOMPARE(groupIcon32().size(), 1011);
    QCOMPARE(iconFileName(), QStringLiteral("icon_2038059696"));
    QCOMPARE(iconRemotePath(), QStringLiteral("/icon_2038059696"));
    QCOMPARE(groupName(), QStringLiteral("tsmediachat"));
    QVERIFY(groupName().size() <= kMaxGroupNameLength);
    // Owned copies: changing one doesn't touch the next.
    QByteArray a = groupIcon16();
    a[0]         = 'x';
    QCOMPARE(crc32(groupIcon16()), kGroupIconId);
}

// ---- plan -------------------------------------------------------------------------------------------

void TestAccessGroup::planFromGuest()
{
    const Plan p = plan(guestRows(), OwnPowers());
    QCOMPARE(p.protection.size(), 2);
    QCOMPARE(p.protection.at(0).name, QStringLiteral("i_group_needed_member_add_power"));
    QCOMPARE(p.protection.at(1).name, QStringLiteral("i_group_needed_member_remove_power"));
    for (const PlanItem& i : p.protection) {
        QCOMPARE(i.value.value, 75);
        QCOMPARE(i.kind, ItemKind::Protection);
    }
    for (const char* n : {"i_ft_file_upload_power", "i_ft_file_download_power", "i_ft_file_browse_power", "i_ft_directory_create_power"}) {
        const PlanItem* i = find(p.body, n);
        QVERIFY2(i, n);
        QCOMPARE(i->value.value, 75);
        QCOMPARE(i->value.skip, false); // like TeamSpeak's own templates
        QCOMPARE(i->value.negated, false);
        QCOMPARE(i->kind, ItemKind::FileSet);
    }
    QVERIFY(!find(p.body, "i_ft_file_delete_power"));
    QVERIFY(!find(p.body, "i_ft_file_rename_power"));
    // 31 Guest rows minus browse, download, the two quotas (the plan's own values) and i_group_needed_modify_power.
    QCOMPARE(p.copies, 26);
    QCOMPARE(p.body.size(), 4 + 2 + 26);
    QVERIFY(find(p.body, "b_channel_join_permanent"));
    QVERIFY(find(p.body, "b_client_channel_textmessage_send"));
    const PlanItem* autoUpdate = find(p.body, "i_group_auto_update_type");
    QVERIFY(autoUpdate); // TeamSpeak's permission updates keep adding new Guest permissions
    QCOMPARE(autoUpdate->value.value, 15);
    QVERIFY(!find(p.body, "i_group_needed_modify_power"));
    QCOMPARE(p.icon.size(), 1);
    QCOMPARE(p.icon.first().name, QStringLiteral("i_icon_id"));
    QCOMPARE(p.icon.first().value.value, static_cast<int>(kGroupIconId));
    QVERIFY(p.modifyProtection.isEmpty()); // own power unknown: left out
    QCOMPARE(p.protectionValue(), 75);
}

void TestAccessGroup::planProtection()
{
    QCOMPARE(protectionValue(std::nullopt), 75);
    QCOMPARE(protectionValue(50), 50);
    QCOMPARE(protectionValue(0), 1); // never 0: 0 >= 0 would let anyone add themselves
    QCOMPARE(protectionValue(-5), 1);
    QCOMPARE(protectionValue(100), 75);
    OwnPowers own;
    own.memberAdd    = 60;
    own.memberRemove = 90;
    const Plan p     = plan({}, own);
    QCOMPARE(p.protection.at(0).value.value, 60);
    QCOMPARE(p.protection.at(1).value.value, 75);
    QCOMPARE(p.protectionValue(), 60);
}

void TestAccessGroup::planModifyProtection()
{
    OwnPowers own;
    own.groupModify = 60;
    QCOMPARE(plan({}, own).modifyProtection.size(), 1);
    QCOMPARE(plan({}, own).modifyProtection.first().value.value, 60);
    QCOMPARE(plan({}, own).modifyProtection.first().name, QStringLiteral("i_group_needed_modify_power"));
    own.groupModify = 100;
    QCOMPARE(plan({}, own).modifyProtection.first().value.value, 75);
    own.groupModify = 0; // would lock you out of later repairs
    QVERIFY(plan({}, own).modifyProtection.isEmpty());
}

void TestAccessGroup::planExclusions()
{
    PermRows rows = rowsOf({{"i_permission_modify_power", 75},
                            {"i_group_modify_power", 75},
                            {"i_group_member_add_power", 75},
                            {"i_group_member_remove_power", 75},
                            {"i_client_permission_modify_power", 75},
                            {"i_group_auto_update_max_value", 45},
                            {"i_needed_modify_power_client_talk_power", 75},
                            {"i_icon_id", 300},
                            {"i_group_sort_id", 10},
                            {"i_group_show_name_in_tree", 1},
                            {"i_group_needed_member_add_power", 5},
                            {"i_ft_file_upload_power", 10},
                            {"i_client_talk_power", 20},
                            {"i_channel_join_power", -1}});
    rows[QStringLiteral("i_channel_join_power")].negated = true; // a "Sticky"-style restriction
    rows[QStringLiteral("i_client_talk_power")].skip     = true;
    const Plan p = plan(rows, OwnPowers());
    QStringList copies;
    for (const PlanItem& i : p.body) {
        if (i.kind == ItemKind::Copy)
            copies << i.name;
    }
    QCOMPARE(copies, QStringList{QStringLiteral("i_client_talk_power")});
    QCOMPARE(find(p.body, "i_client_talk_power")->value.skip, true); // value and skip as in the default group
    QCOMPARE(find(p.body, "i_ft_file_upload_power")->value.value, 75); // the plan's value wins
    QVERIFY(isCopyExcluded(QStringLiteral("i_group_needed_modify_power")));
    QVERIFY(!isCopyExcluded(QStringLiteral("b_channel_join_permanent")));
}

void TestAccessGroup::planQuotas()
{
    const Plan none = plan({}, OwnPowers());
    QCOMPARE(find(none.body, "i_ft_quota_mb_upload_per_client")->value.value, -1);
    QCOMPARE(find(none.body, "i_ft_quota_mb_download_per_client")->value.value, -1);
    const Plan limited = plan(rowsOf({{"i_ft_quota_mb_upload_per_client", 100}, {"i_ft_quota_mb_download_per_client", 500}}), OwnPowers());
    QCOMPARE(find(limited.body, "i_ft_quota_mb_upload_per_client")->value.value, 100);
    QCOMPARE(find(limited.body, "i_ft_quota_mb_download_per_client")->value.value, 500);
    QCOMPARE(find(limited.body, "i_ft_quota_mb_upload_per_client")->kind, ItemKind::Quota);
}

// ---- missing / conformant -----------------------------------------------------------------------------

void TestAccessGroup::missingItems()
{
    OwnPowers own;
    own.groupModify = 75;
    const Plan p    = plan(guestRows(), own);
    PermRows   full = applied(p);
    QVERIFY(missing(full, p).isEmpty());

    PermRows rows = full;
    rows.remove(QStringLiteral("i_ft_file_upload_power"));
    rows[QStringLiteral("i_ft_file_download_power")].value = 0;      // zero counts as missing
    rows[QStringLiteral("i_ft_file_browse_power")].value   = 60;     // lowered by an admin: kept
    rows[QStringLiteral("i_group_needed_member_add_power")].value = 0;
    rows.remove(QStringLiteral("b_channel_join_permanent"));
    rows[QStringLiteral("i_icon_id")].value = 0;
    rows.remove(QStringLiteral("i_group_needed_modify_power"));
    rows.remove(QStringLiteral("i_ft_quota_mb_upload_per_client"));
    rows[QStringLiteral("i_client_talk_power")].value = 30; // admin-added: never touched

    Missing m = missing(rows, p);
    QCOMPARE(names(m.sendable),
             (QStringList{QStringLiteral("i_group_needed_member_add_power"), QStringLiteral("i_ft_file_upload_power"), QStringLiteral("i_ft_file_download_power"),
                          QStringLiteral("i_ft_quota_mb_upload_per_client"), QStringLiteral("b_channel_join_permanent"), QStringLiteral("i_icon_id"),
                          QStringLiteral("i_group_needed_modify_power")}));
    QVERIFY(m.refused.isEmpty());
    QCOMPARE(visibleMissing(m.sendable).size(), 6); // the needed modify power alone is never shown

    const QSet<QString> refused{QStringLiteral("b_channel_join_permanent"), QStringLiteral("i_icon_id")};
    m = missing(rows, p, refused);
    QCOMPARE(names(m.refused), (QStringList{QStringLiteral("b_channel_join_permanent"), QStringLiteral("i_icon_id")}));
    QCOMPARE(m.sendable.size(), 5);
}

void TestAccessGroup::protectionAndFiles()
{
    QVERIFY(!isProtected({}));
    QVERIFY(!isProtected(rowsOf({{"i_group_needed_member_add_power", 75}})));
    QVERIFY(!isProtected(rowsOf({{"i_group_needed_member_add_power", 75}, {"i_group_needed_member_remove_power", 0}})));
    QVERIFY(isProtected(rowsOf({{"i_group_needed_member_add_power", 1}, {"i_group_needed_member_remove_power", 1}})));
    QVERIFY(!hasAnyFilePermission({}));
    QVERIFY(!hasAnyFilePermission(rowsOf({{"i_ft_file_upload_power", 0}, {"i_ft_quota_mb_upload_per_client", -1}})));
    QVERIFY(hasAnyFilePermission(rowsOf({{"i_ft_directory_create_power", 75}})));
    QVERIFY(hasOurIcon(rowsOf({{"i_icon_id", static_cast<int>(kGroupIconId)}})));
    QVERIFY(!hasOurIcon(rowsOf({{"i_icon_id", 300}})));
}

void TestAccessGroup::conformance()
{
    const PermRows guest = guestRows();
    const Plan     p     = plan(guest, OwnPowers());
    PermRows       ours  = applied(p);
    QVERIFY(conformant(ours, guest));
    QVERIFY(looksLikeOurs(ours));
    ours[QStringLiteral("i_group_sort_id")].value           = 5; // cosmetic
    ours[QStringLiteral("i_group_show_name_in_tree")].value = 1;
    ours[QStringLiteral("i_group_needed_modify_power")].value = 75;
    QVERIFY(conformant(ours, guest));

    PermRows withExtra = ours;
    withExtra[QStringLiteral("i_client_talk_power")].value = 30; // someone else's group, or a customised one
    QVERIFY(!conformant(withExtra, guest));

    PermRows negated = ours;
    negated[QStringLiteral("b_channel_join_permanent")].negated = true;
    QVERIFY(!conformant(negated, guest));

    PermRows admin = serverAdminRows();
    admin[QStringLiteral("i_icon_id")].value = static_cast<int>(kGroupIconId); // the takeover attempt
    QVERIFY(!conformant(admin, guest));

    QVERIFY(conformant({}, guest)); // empty: harmless, but an icon match also needs looksLikeOurs()
    QVERIFY(!looksLikeOurs({}));
    QVERIFY(!looksLikeOurs(rowsOf({{"i_icon_id", static_cast<int>(kGroupIconId)}})));
    QVERIFY(!conformant(rowsOf({{"#123", 1}}), guest)); // a permission the client couldn't name
}

void TestAccessGroup::serverGroupsParsing()
{
    QCOMPARE(parseServerGroups(QStringLiteral("6,8,12")), (QList<quint64>{6, 8, 12}));
    QCOMPARE(parseServerGroups(QString()), QList<quint64>());
    QCOMPARE(parseServerGroups(QStringLiteral(" 6, x,0,8,6 ,,-3,18446744073709551616")), (QList<quint64>{6, 8}));
    QStringList many;
    for (int i = 1; i <= 300; ++i)
        many << QString::number(i);
    QCOMPARE(parseServerGroups(many.join(QLatin1Char(','))).size(), 256);
}

void TestAccessGroup::candidates()
{
    QList<GroupInfo> groups = stockGroups();
    groups << group(30, "tsmediachat", 0) << group(31, "TSMediaChat", 1) << group(20, "tsmediachat", 1) << group(40, "tsmediachat", 2);
    Candidates c = findCandidates(groups, 8, 6);
    QCOMPARE(c.byName, quint64(20)); // regular groups only, any case, the lowest id
    QCOMPARE(c.sameName, (QList<quint64>{31}));
    QVERIFY(c.fallback.isEmpty()); // a name match never falls back to ids

    // No name match: the remembered id first, then icon matches; they still need conformance.
    QList<GroupInfo> renamed = stockGroups();
    renamed[5].iconId = kGroupIconId; // Server Admin with our icon
    renamed << group(33, "Media", 1, kGroupIconId) << group(35, "Media senders", 1);
    c = findCandidates(renamed, 8, 35);
    QCOMPARE(c.byName, quint64(0));
    QCOMPARE(c.fallback, (QList<quint64>{35, 6, 33}));
    QCOMPARE(c.fallbackHow, (QList<Match>{Match::Remembered, Match::Icon, Match::Icon}));

    // The default group is never ours, whatever its name; a remembered template group is ignored.
    QList<GroupInfo> odd = stockGroups();
    odd[7].name = QStringLiteral("tsmediachat");
    c           = findCandidates(odd, 8, 3);
    QCOMPARE(c.byName, quint64(0));
    QVERIFY(c.fallback.isEmpty());
    QVERIFY(isOurName(QStringLiteral("TsMediaChat")));
    QVERIFY(!isOurName(QStringLiteral("tsmediachat ")));
}

// ---- the job ------------------------------------------------------------------------------------------

void TestAccessGroup::jobCreateHappyPath()
{
    Job job(createInput());
    QCOMPARE(job.steps(), 4);
    Command c = job.start();
    QCOMPARE(c.type, Command::Type::ReadPerms);
    QCOMPARE(c.sgid, quint64(8));
    QCOMPARE(job.step(), 1);
    QCOMPARE(job.stepKind(), StepKind::CreateGroup);

    c = job.next(rowsAnswer(guestRows()));
    QCOMPARE(c.type, Command::Type::AddGroup);

    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat");
    c = job.next(groupsAnswer(after));
    QCOMPARE(c.type, Command::Type::AddPerms); // protection first, on its own
    QCOMPARE(c.sgid, quint64(21));
    QCOMPARE(c.continueOnError, false);
    QCOMPARE(names(c.perms), (QStringList{QStringLiteral("i_group_needed_member_add_power"), QStringLiteral("i_group_needed_member_remove_power")}));
    QCOMPARE(job.step(), 2);
    QCOMPARE(job.stepKind(), StepKind::AddPermissions);

    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(c.continueOnError, true);
    QCOMPARE(c.perms.size(), 32);
    QVERIFY(find(c.perms, "i_ft_file_upload_power"));

    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::UploadIcon);
    QCOMPARE(job.step(), 3);
    QCOMPARE(job.stepKind(), StepKind::AddIcon);

    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(names(c.perms), QStringList{QStringLiteral("i_icon_id")});

    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::ReadPerms); // no own modify power known: no i_group_needed_modify_power
    QCOMPARE(c.sgid, quint64(21));
    QCOMPARE(job.step(), 4);
    QCOMPARE(job.stepKind(), StepKind::Checking);

    const PermRows back = applied(plan(guestRows(), OwnPowers()));
    c                   = job.next(rowsAnswer(back));
    QCOMPARE(c.type, Command::Type::Finished);
    QVERIFY(job.isFinished());
    const JobResult& r = job.result();
    QVERIFY(r.success);
    QVERIFY(r.created);
    QVERIFY(!r.removed);
    QCOMPARE(r.sgid, quint64(21));
    QVERIFY(r.rowsKnown);
    QCOMPARE(r.groupRows, back);
    QVERIFY(r.defaultRowsKnown);
    QCOMPARE(r.iconProblem, IconProblem::None);

    // A second command after the end stays "finished".
    QCOMPARE(job.next(Answer::success()).type, Command::Type::Finished);
}

void TestAccessGroup::jobProtectionRefusedRemoves()
{
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(guestRows()));
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat");
    job.next(groupsAnswer(after));
    Command c = job.next(Answer::failure(err::clientInsufficient, QStringLiteral("insufficient client permissions"),
                                         {QStringLiteral("i_needed_modify_power_group_needed_member_add_power")}));
    QCOMPARE(c.type, Command::Type::DeleteGroup); // nothing else is sent after a refused protection
    QCOMPARE(c.sgid, quint64(21));
    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::Finished);
    const JobResult& r = job.result();
    QVERIFY(!r.success);
    QCOMPARE(r.error, ErrorKind::ProtectRemoved);
    QCOMPARE(r.errorDetail, QStringLiteral("i_needed_modify_power_group_needed_member_add_power"));
    QVERIFY(r.removed);
    QVERIFY(r.refused.contains(QStringLiteral("i_needed_modify_power_group_needed_member_add_power")));
}

void TestAccessGroup::jobProtectionRefusedLeftEmpty()
{
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(guestRows()));
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat");
    job.next(groupsAnswer(after));
    QCOMPARE(job.next(Answer::failure(err::insufficientPermPower, QStringLiteral("insufficient permission modify power"))).type, Command::Type::DeleteGroup);
    QCOMPARE(job.next(Answer::failure(err::clientInsufficient, QStringLiteral("no delete"), {QStringLiteral("b_virtualserver_servergroup_delete")})).type,
             Command::Type::Finished);
    QCOMPARE(job.result().error, ErrorKind::ProtectLeftEmpty);
    QCOMPARE(job.result().errorDetail, QStringLiteral("insufficient permission modify power"));
    QVERIFY(!job.result().removed);
}

void TestAccessGroup::jobDuplicateBecomesRepair()
{
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(guestRows()));
    QList<GroupInfo> others = stockGroups();
    others << group(9, "TSMEDIACHAT"); // another admin was faster (it's not in our snapshot either)
    Command c = job.next(groupsAnswer(others, false, err::databaseDuplicateEntry));
    QCOMPARE(c.type, Command::Type::ReadPerms);
    QCOMPARE(c.sgid, quint64(9));
    QCOMPARE(job.result().kind, JobKind::Repair);
    // An empty, unprotected group: protection, then everything else.
    c = job.next(Answer::failure(err::permissionEmptyResult, QStringLiteral("empty result")));
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(c.continueOnError, false);
    QCOMPARE(c.perms.size(), 2);
    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(c.perms.size(), 32);
    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::UploadIcon);
    QVERIFY(!job.result().created); // never deleted by this job
}

void TestAccessGroup::jobNewIdFromList()
{
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(guestRows()));
    Command c = job.next(groupsAnswer(stockGroups())); // the server hasn't pushed the list yet
    QCOMPARE(c.type, Command::Type::ListGroups);
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat", 0) << group(22, "tsmediachat"); // a template of that name is skipped
    c = job.next(groupsAnswer(after));
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(c.sgid, quint64(22));
    QVERIFY(job.result().created);
}

void TestAccessGroup::jobCreatedNotFound()
{
    JobInput in = createInput();
    in.knownGroups.insert(21);
    Job job(in);
    job.start();
    job.next(rowsAnswer(guestRows()));
    job.next(groupsAnswer(stockGroups()));
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat"); // existed before the click: not the new one
    QCOMPARE(job.next(groupsAnswer(after)).type, Command::Type::Finished);
    QCOMPARE(job.result().error, ErrorKind::CreatedNotFound);
}

void TestAccessGroup::jobNoFilePermissionsRemoves()
{
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(guestRows()));
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat");
    job.next(groupsAnswer(after));
    job.next(Answer::success()); // protected
    Command c = job.next(Answer::failure(err::clientInsufficient, QStringLiteral("insufficient"), {QStringLiteral("i_ft_file_upload_power")}));
    QCOMPARE(c.type, Command::Type::UploadIcon);
    c = job.next(Answer::success());
    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::ReadPerms);
    const PermRows protectedOnly = rowsOf({{"i_group_needed_member_add_power", 75}, {"i_group_needed_member_remove_power", 75}});
    c                            = job.next(rowsAnswer(protectedOnly));
    QCOMPARE(c.type, Command::Type::DeleteGroup);
    QCOMPARE(job.next(Answer::success()).type, Command::Type::Finished);
    QCOMPARE(job.result().error, ErrorKind::NoFilePermsRemoved);
    QCOMPARE(job.result().errorDetail, QStringLiteral("i_ft_file_upload_power"));
    QVERIFY(job.result().removed);
}

void TestAccessGroup::jobErrors()
{
    {
        JobInput in    = createInput();
        in.defaultSgid = 0;
        Job job(in);
        QCOMPARE(job.start().type, Command::Type::Finished);
        QCOMPARE(job.result().error, ErrorKind::NoDefaultGroup);
    }
    {
        Job job(createInput());
        job.start();
        job.next(Answer::failure(err::clientInsufficient, QStringLiteral("insufficient"), {QStringLiteral("b_virtualserver_servergroup_permission_list")}));
        QCOMPARE(job.result().error, ErrorKind::CantReadPermissions);
    }
    {
        Job job(createInput());
        job.start();
        QCOMPARE(job.next(Answer::timedOut()).type, Command::Type::Finished);
        QCOMPARE(job.result().error, ErrorKind::Timeout);
    }
    {
        Job job(createInput());
        job.start();
        job.next(rowsAnswer(guestRows()));
        job.next(Answer::failure(err::clientInsufficient, QStringLiteral("insufficient"), {QStringLiteral("b_virtualserver_servergroup_create")}));
        QCOMPARE(job.result().error, ErrorKind::NoCreatePermission);
    }
    {
        Job job(createInput());
        job.start();
        job.next(rowsAnswer(guestRows()));
        job.next(Answer::failure(0x0b01, QStringLiteral("something else")));
        QCOMPARE(job.result().error, ErrorKind::Refused);
        QCOMPARE(job.result().errorDetail, QStringLiteral("something else"));
    }
    {
        Job job(createInput());
        job.start();
        QCOMPARE(job.abort(ErrorKind::ConnectionLost).type, Command::Type::Finished);
        QCOMPARE(job.result().error, ErrorKind::ConnectionLost);
        QCOMPARE(job.abort(ErrorKind::Timeout).type, Command::Type::Finished); // the first reason stays
        QCOMPARE(job.result().error, ErrorKind::ConnectionLost);
    }
    {
        // An empty default group reads as "empty result": an empty copy set, not an error.
        Job job(createInput());
        job.start();
        QCOMPARE(job.next(Answer::failure(err::databaseEmptyResult, QStringLiteral("empty"))).type, Command::Type::AddGroup);
    }
}

void TestAccessGroup::jobRepairSendsOnlyMissing()
{
    OwnPowers own;
    own.groupModify = 75;
    const Plan p    = plan(guestRows(), own);
    PermRows   rows = applied(p);
    rows.remove(QStringLiteral("i_ft_file_upload_power"));
    rows.remove(QStringLiteral("b_channel_join_permanent"));
    rows.remove(QStringLiteral("b_client_info_view"));
    rows[QStringLiteral("i_client_talk_power")].value = 30;  // admin-added
    rows[QStringLiteral("i_ft_file_browse_power")].value = 60; // lowered

    JobInput in;
    in.kind        = JobKind::Repair;
    in.defaultSgid = 8;
    in.sgid        = 21;
    in.own         = own;
    in.refused     = {QStringLiteral("b_client_info_view")}; // refused earlier: not sent again
    Job job(in);
    QCOMPARE(job.steps(), 3);
    Command c = job.start();
    QCOMPARE(c.type, Command::Type::ReadPerms);
    QCOMPARE(c.sgid, quint64(8));
    QCOMPARE(job.step(), 1);
    QCOMPARE(job.stepKind(), StepKind::AddPermissions);
    c = job.next(rowsAnswer(guestRows()));
    QCOMPARE(c.type, Command::Type::ReadPerms);
    QCOMPARE(c.sgid, quint64(21));
    c = job.next(rowsAnswer(rows));
    QCOMPARE(c.type, Command::Type::AddPerms); // protected already: straight to the missing body
    QCOMPARE(names(c.perms), (QStringList{QStringLiteral("i_ft_file_upload_power"), QStringLiteral("b_channel_join_permanent")}));
    c = job.next(Answer::success());
    QCOMPARE(c.type, Command::Type::ReadPerms); // icon and needed modify power present: nothing else
    QCOMPARE(job.step(), 3);
    QCOMPARE(job.stepKind(), StepKind::Checking);
    QCOMPARE(job.next(rowsAnswer(rows)).type, Command::Type::Finished);
    QVERIFY(job.result().success);
    QVERIFY(!job.result().created);
}

void TestAccessGroup::jobRepairProtectionRefused()
{
    JobInput in;
    in.kind        = JobKind::Repair;
    in.defaultSgid = 8;
    in.sgid        = 21;
    Job job(in);
    job.start();
    job.next(rowsAnswer(guestRows()));
    Command c = job.next(rowsAnswer(rowsOf({{"i_ft_file_upload_power", 75}}))); // a hand-made, unprotected group
    QCOMPARE(c.type, Command::Type::AddPerms);
    QCOMPARE(c.perms.size(), 2);
    QCOMPARE(c.perms.first().kind, ItemKind::Protection);
    c = job.next(Answer::failure(err::insufficientGroupPower, QStringLiteral("insufficient group modify power")));
    QCOMPARE(c.type, Command::Type::Finished); // never deleted: this job didn't create it
    QCOMPARE(job.result().error, ErrorKind::Refused);
    QVERIFY(!job.result().removed);
}

void TestAccessGroup::jobIconPaths()
{
    const auto toIcon = [](Job& job) {
        job.start();
        job.next(rowsAnswer(guestRows()));
        QList<GroupInfo> after = stockGroups();
        after << group(21, "tsmediachat");
        job.next(groupsAnswer(after));
        job.next(Answer::success());
        return job.next(Answer::success());
    };
    {
        JobInput in    = createInput();
        in.iconSkip    = IconProblem::TooLarge;
        in.maxIconSize = 200;
        Job job(in);
        QCOMPARE(toIcon(job).type, Command::Type::ReadPerms); // no upload at all
        QCOMPARE(job.result().iconProblem, IconProblem::TooLarge);
        QCOMPARE(job.result().maxIconSize, 200);
    }
    {
        JobInput in = createInput();
        in.iconSkip = IconProblem::NoManagePermission;
        Job job(in);
        QCOMPARE(toIcon(job).type, Command::Type::CheckIconFile);
        Answer present   = Answer::success();
        present.fileSize = kGroupIconBytes; // already on the server: only the permission
        Command c        = job.next(present);
        QCOMPARE(c.type, Command::Type::AddPerms);
        QCOMPARE(names(c.perms), QStringList{QStringLiteral("i_icon_id")});
    }
    {
        JobInput in = createInput();
        in.iconSkip = IconProblem::NoManagePermission;
        Job job(in);
        toIcon(job);
        QCOMPARE(job.next(Answer::failure(0x0803, QStringLiteral("file not found"))).type, Command::Type::ReadPerms);
        QCOMPARE(job.result().iconProblem, IconProblem::NoManagePermission);
    }
    {
        Job job(createInput());
        QCOMPARE(toIcon(job).type, Command::Type::UploadIcon);
        QCOMPARE(job.next(Answer::failure(err::fileAlreadyExists, QStringLiteral("file already exists"))).type, Command::Type::AddPerms); // same name, same bytes
    }
    {
        Job job(createInput());
        toIcon(job);
        QCOMPARE(job.next(Answer::failure(err::clientInsufficient, QStringLiteral("insufficient"), {QStringLiteral("b_icon_manage")})).type, Command::Type::ReadPerms);
        QCOMPARE(job.result().iconProblem, IconProblem::NoManagePermission);
    }
    {
        Job job(createInput());
        toIcon(job);
        QCOMPARE(job.next(Answer::failure(0x080b, QStringLiteral("could not open connection"))).type, Command::Type::ReadPerms);
        QCOMPARE(job.result().iconProblem, IconProblem::UploadFailed);
        QCOMPARE(job.result().iconDetail, QStringLiteral("could not open connection"));
    }
    {
        Job job(createInput());
        toIcon(job);
        job.next(Answer::success()); // uploaded
        QCOMPARE(job.next(Answer::failure(err::clientInsufficient, QString(), {QStringLiteral("i_icon_id")})).type, Command::Type::ReadPerms);
        QCOMPARE(job.result().iconProblem, IconProblem::Refused);
        QVERIFY(job.result().refused.contains(QStringLiteral("i_icon_id")));
    }
}

void TestAccessGroup::jobChunks()
{
    PermRows big;
    for (int i = 0; i < 100; ++i) {
        PermValue v;
        v.value                                      = 1;
        big[QStringLiteral("b_custom_%1").arg(i, 3, 10, QLatin1Char('0'))] = v;
    }
    Job job(createInput());
    job.start();
    job.next(rowsAnswer(big));
    QList<GroupInfo> after = stockGroups();
    after << group(21, "tsmediachat");
    job.next(groupsAnswer(after));
    Command c = job.next(Answer::success());
    QList<int> sizes;
    while (c.type == Command::Type::AddPerms) {
        sizes << c.perms.size();
        QVERIFY(c.perms.size() <= kMaxPermsPerRequest);
        c = job.next(Answer::failure(err::clientInsufficient, QString(), {c.perms.first().name})); // a refused chunk doesn't stop the rest
    }
    QCOMPARE(sizes, (QList<int>{40, 40, 26})); // 4 file + 2 quotas + 100 copies
    QCOMPARE(c.type, Command::Type::UploadIcon);
    QCOMPARE(job.result().refused.size(), 3);
}

void TestAccessGroup::jobLateRefusals()
{
    Job job(createInput());
    job.noteRefused(QStringLiteral("b_channel_join_permanent"));
    job.noteRefused(QString());
    QCOMPARE(job.result().refused, QSet<QString>{QStringLiteral("b_channel_join_permanent")});
}

// ---- states and texts ---------------------------------------------------------------------------------

void TestAccessGroup::classifyStates()
{
    Facts f;
    QCOMPARE(classify(f), State::NotConnected);
    f.connected = true;
    QCOMPARE(classify(f), State::Checking); // not checked yet
    f.checked = true;
    QCOMPARE(classify(f), State::NoGroup);
    f.canCreate = false;
    QCOMPARE(classify(f), State::NoGroupCantCreate);
    f.groupFound = true;
    QCOMPARE(classify(f), State::NotMember);
    f.isMember = true;
    QCOMPARE(classify(f), State::Member);
    f.canManage = true;
    QCOMPARE(classify(f), State::NotProtected);
    f.isProtected = true;
    QCOMPARE(classify(f), State::Ready);

    const Plan p = plan(guestRows(), OwnPowers());
    f.missing    = missing(rowsOf({{"i_group_needed_member_add_power", 75}, {"i_group_needed_member_remove_power", 75}}), p);
    QCOMPARE(classify(f), State::NeedsRepair);
    PermRows noIcon = applied(p);
    noIcon.remove(QStringLiteral("i_icon_id"));
    f.missing = missing(noIcon, p);
    QCOMPARE(classify(f), State::NeedsRepair); // no known reason: listed as missing
    f.iconProblem = IconProblem::NoManagePermission;
    QCOMPARE(classify(f), State::ReadyNoIcon);
    f.iconProblem = IconProblem::None;
    f.missing     = missing(noIcon, p, {QStringLiteral("i_icon_id")});
    QCOMPARE(classify(f), State::ReadyNoIcon);

    OwnPowers own;
    own.groupModify = 75;
    const Plan withModify = plan(guestRows(), own);
    PermRows   noModify   = applied(withModify);
    noModify.remove(QStringLiteral("i_group_needed_modify_power"));
    f.missing = missing(noModify, withModify);
    QCOMPARE(classify(f), State::Ready); // the needed modify power alone isn't "missing something"

    f.error = ErrorKind::Timeout;
    QCOMPARE(classify(f), State::Error);
    f.jobRunning = true;
    QCOMPARE(classify(f), State::Working);
    f.connected = false;
    QCOMPARE(classify(f), State::NotConnected);
}

void TestAccessGroup::viewTexts()
{
    ViewInput in;
    View      v = buildView(in);
    QCOMPARE(v.status, QStringLiteral("Connect to a server to set up its TS Media chat group."));
    QCOMPARE(v.primaryText, QStringLiteral("Create TS Media c&hat group"));
    QVERIFY(!v.primaryEnabled);
    QVERIFY(v.showIntro);
    QCOMPARE(introText(), QStringLiteral("For server admins: a server group whose members can send files with TS Media chat. Changes here are made on the server right away."));

    in.state = State::Checking;
    QCOMPARE(buildView(in).status, QStringLiteral("Checking the server…"));
    QVERIFY(!buildView(in).primaryEnabled);

    in.state        = State::NoGroup;
    in.defaultGroup = QStringLiteral("Guest");
    v               = buildView(in);
    QCOMPARE(v.status, QStringLiteral("This server has no TS Media chat group yet."));
    QCOMPARE(v.detail, QStringLiteral("Creates the server group “tsmediachat” with the TS Media chat icon. Members keep what “Guest” allows and can also upload, "
                                      "download and browse files and create folders. Afterwards, right-click anyone and choose Server Groups > tsmediachat to give it to them."));
    QVERIFY(v.extra.isEmpty());
    QVERIFY(v.primaryEnabled);
    QCOMPARE(v.primary, Action::Create);
    QVERIFY(v.showDetails);
    QVERIFY(!v.showCheckAgain);
    in.defaultCanUploadHere = true;
    in.channel              = QStringLiteral("Lobby");
    QCOMPARE(buildView(in).extra, QStringLiteral("Everyone in “Guest” can already send files in “Lobby”. The group is only needed where they can't."));

    in.state = State::NoGroupCantCreate;
    v        = buildView(in);
    QCOMPARE(v.detail, QStringLiteral("Only a server admin can create it: you don't have permission to create server groups here (b_virtualserver_servergroup_create)."));
    QVERIFY(!v.showIntro);
    QVERIFY(!v.showButtons);
    QVERIFY(v.primaryText.isEmpty());

    in.state    = State::Working;
    in.job      = JobKind::Create;
    in.step     = 2;
    in.stepKind = StepKind::AddPermissions;
    v           = buildView(in);
    QCOMPARE(v.status, QStringLiteral("Creating “tsmediachat”… step 2 of 4: adding permissions"));
    QCOMPARE(v.primaryText, QStringLiteral("Creating…"));
    QVERIFY(!v.primaryEnabled);
    in.job      = JobKind::Repair;
    in.group    = QStringLiteral("Media");
    in.step     = 2;
    in.steps    = 3;
    in.stepKind = StepKind::AddIcon;
    QCOMPARE(buildView(in).status, QStringLiteral("Repairing “Media”… step 2 of 3: adding the icon"));
    QCOMPARE(buildView(in).primaryText, QStringLiteral("Repairing…"));

    in.group   = QString();
    in.state   = State::Ready;
    in.members = 0;
    v          = buildView(in);
    QCOMPARE(v.status, QStringLiteral("“tsmediachat” is ready · no members yet"));
    QCOMPARE(v.icon, View::Icon::Group);
    QCOMPARE(v.detail, QStringLiteral("Members can send files in channels that need upload power 75 or less. To give access, right-click a person and choose Server Groups > tsmediachat."));
    QVERIFY(v.primaryText.isEmpty());
    QVERIFY(v.showCheckAgain);
    QVERIFY(v.showDetails);
    in.members = 1;
    QCOMPARE(buildView(in).status, QStringLiteral("“tsmediachat” is ready · 1 member"));
    in.members = 7;
    QCOMPARE(buildView(in).status, QStringLiteral("“tsmediachat” is ready · 7 members"));
    in.members = -1;
    QCOMPARE(buildView(in).status, QStringLiteral("“tsmediachat” is ready"));
    in.channelNeededUpload = 80;
    QCOMPARE(buildView(in).extra, QStringLiteral("In “Lobby”, uploading needs power 80, so members can't send files there."));
    in.channelNeededUpload = 75;
    QVERIFY(buildView(in).extra.isEmpty());

    in.state       = State::ReadyNoIcon;
    in.members     = 2;
    in.iconProblem = IconProblem::TooLarge;
    in.maxIconSize = 200;
    PlanItem icon;
    icon.name = QStringLiteral("i_icon_id");
    icon.kind = ItemKind::Icon;
    in.missing.sendable << icon;
    v = buildView(in);
    QCOMPARE(v.status, QStringLiteral("“tsmediachat” is ready, without its icon · 2 members"));
    QCOMPARE(v.detail, QStringLiteral("The icon couldn't be added: the server allows icons up to 200 bytes (i_max_icon_filesize). Everything else works."));
    QCOMPARE(v.primaryText, QStringLiteral("Add TS Media c&hat icon"));
    QCOMPARE(v.primary, Action::Repair);
    in.iconProblem = IconProblem::NoManagePermission;
    QCOMPARE(buildView(in).detail, QStringLiteral("The icon couldn't be added: you need permission to manage icons (b_icon_manage). Everything else works."));
    in.iconProblem = IconProblem::UploadFailed;
    in.iconDetail  = QStringLiteral("timeout");
    QCOMPARE(buildView(in).detail, QStringLiteral("The icon couldn't be added: the upload failed (timeout). Everything else works."));

    // Needs repair: the list, refused items, the button only while something can be sent.
    const Plan p    = plan(guestRows(), OwnPowers());
    PermRows   rows = applied(p);
    rows.remove(QStringLiteral("i_ft_file_upload_power"));
    rows.remove(QStringLiteral("i_ft_quota_mb_download_per_client"));
    rows.remove(QStringLiteral("b_channel_join_permanent"));
    rows.remove(QStringLiteral("b_client_info_view"));
    rows.remove(QStringLiteral("b_channel_info_view"));
    in.state   = State::NeedsRepair;
    in.members = -1;
    in.missing = missing(rows, p, {QStringLiteral("b_channel_info_view")});
    v          = buildView(in);
    QCOMPARE(v.icon, View::Icon::Warning);
    QCOMPARE(v.status, QStringLiteral("“tsmediachat” is missing some permissions"));
    QCOMPARE(v.detail, QStringLiteral("Members may not be able to send files. Missing: uploading files, the download quota, 3 permissions copied from “Guest”. Repair adds "
                                      "what's missing and keeps permissions you set yourself. 1 can only be added by an admin with higher server permissions (b_channel_info_view)."));
    QCOMPARE(v.primaryText, QStringLiteral("Repair TS Media c&hat group"));
    QVERIFY(v.primaryEnabled);

    PermRows copiesOnly = applied(p);
    copiesOnly.remove(QStringLiteral("b_channel_join_permanent"));
    in.missing = missing(copiesOnly, p);
    v          = buildView(in);
    QCOMPARE(v.status, QStringLiteral("“tsmediachat” is missing some of “Guest”'s permissions"));
    QCOMPARE(v.detail, QStringLiteral("Members who are only in this group can't do everything “Guest” allows. Repair copies the missing ones again."));
    in.missing = missing(copiesOnly, p, {QStringLiteral("b_channel_join_permanent")});
    QVERIFY(buildView(in).primaryText.isEmpty()); // nothing sendable left

    in.state      = State::NotProtected;
    in.protection = 60;
    v             = buildView(in);
    QCOMPARE(v.status, QStringLiteral("“tsmediachat” isn't protected"));
    QCOMPARE(v.detail, QStringLiteral("Anyone may be able to add themselves to it. Repair sets who can give it out (group power 60 or more)."));
    QVERIFY(v.detailIsError);
    QCOMPARE(v.icon, View::Icon::Warning);
    QCOMPARE(v.primary, Action::Repair);

    in.state = State::Member;
    v        = buildView(in);
    QCOMPARE(v.status, QStringLiteral("You're in “tsmediachat”, so you can send files on this server."));
    QVERIFY(!v.showButtons);
    QCOMPARE(v.icon, View::Icon::Group);
    in.state = State::NotMember;
    QCOMPARE(buildView(in).status, QStringLiteral("This server has a TS Media chat group, “tsmediachat”. Ask a server admin to give it to you so you can send files."));

    in.state        = State::Error;
    in.errorContext = ErrorContext::Create;
    in.error        = ErrorKind::ProtectRemoved;
    in.errorDetail  = QStringLiteral("i_group_needed_member_add_power");
    v               = buildView(in);
    QCOMPARE(v.status, QStringLiteral("Couldn't create “tsmediachat”"));
    QCOMPARE(v.detail, QStringLiteral("The group couldn't be protected (i_group_needed_member_add_power), so TS Media chat removed it again. Nothing else was changed."));
    QCOMPARE(v.primaryText, QStringLiteral("Try again"));
    QCOMPARE(v.primary, Action::RetryCreate);
    QVERIFY(v.detailIsError);
    QVERIFY(v.showCheckAgain);
    in.errorContext = ErrorContext::Repair;
    QCOMPARE(buildView(in).status, QStringLiteral("Couldn't repair “tsmediachat”"));
    QCOMPARE(buildView(in).primary, Action::RetryRepair);
    in.errorContext = ErrorContext::Check;
    QCOMPARE(buildView(in).status, QStringLiteral("Couldn't check the server"));
    QCOMPARE(buildView(in).primary, Action::RetryCheck);

    QCOMPARE(errorText(ErrorKind::NoCreatePermission, {}, {}),
             QStringLiteral("You don't have permission to create server groups on this server (b_virtualserver_servergroup_create). Ask a server admin."));
    QCOMPARE(errorText(ErrorKind::CantReadPermissions, {}, QStringLiteral("Guest")),
             QStringLiteral("You can't read the permissions of “Guest” (b_virtualserver_servergroup_permission_list). TS Media chat copies them so members keep what they could do before."));
    QCOMPARE(errorText(ErrorKind::NoDefaultGroup, {}, {}), QStringLiteral("Couldn't find this server's default group, so TS Media chat can't copy its permissions."));
    QCOMPARE(errorText(ErrorKind::NotLoaded, {}, {}), QStringLiteral("TeamSpeak hasn't finished loading this server's permissions. Wait a moment and click Try again."));
    QCOMPARE(errorText(ErrorKind::ProtectLeftEmpty, QStringLiteral("x"), {}),
             QStringLiteral("The group couldn't be protected (x) and couldn't be removed. It has no permissions; delete it in Permissions > Server Groups or click Try again."));
    QCOMPARE(errorText(ErrorKind::NoFilePermsRemoved, QStringLiteral("i_ft_file_upload_power"), {}),
             QStringLiteral("None of the file permissions could be added (i_ft_file_upload_power), so TS Media chat removed the group again."));
    QCOMPARE(errorText(ErrorKind::Timeout, {}, {}), QStringLiteral("The server didn't answer. Check your connection and click Try again."));
    QCOMPARE(errorText(ErrorKind::ConnectionLost, {}, {}), QStringLiteral("The connection to the server was lost. Reconnect and click Try again."));
    QCOMPARE(errorText(ErrorKind::CreatedNotFound, {}, {}), QStringLiteral("The group was created, but TS Media chat couldn't find it. Click Check again."));
    QCOMPARE(errorText(ErrorKind::Refused, QStringLiteral("invalid parameter"), {}), QStringLiteral("The server refused it: invalid parameter"));
    QCOMPARE(stepText(StepKind::CreateGroup), QStringLiteral("creating the group"));
    QCOMPARE(stepText(StepKind::Checking), QStringLiteral("checking"));
}

void TestAccessGroup::viewHostileNames()
{
    // Names come from the server: substituted literally, never as further placeholders.
    ViewInput in;
    in.state        = State::Ready;
    in.group        = QStringLiteral("<b>x</b>%2 %1");
    in.members      = 3;
    View v          = buildView(in);
    QCOMPARE(v.status, QStringLiteral("“<b>x</b>%2 %1” is ready · 3 members"));
    in.state        = State::NoGroup;
    in.defaultGroup = QStringLiteral("%2");
    in.channel      = QStringLiteral("%1");
    in.defaultCanUploadHere = true;
    QCOMPARE(buildView(in).extra, QStringLiteral("Everyone in “%2” can already send files in “%1”. The group is only needed where they can't."));
    in.defaultGroup.clear();
    QVERIFY(buildView(in).detail.contains(QStringLiteral("Members keep what the default group allows")));
}

void TestAccessGroup::detailsTexts()
{
    DetailsInput in;
    in.server       = QStringLiteral("My server");
    in.defaultGroup = QStringLiteral("Guest");
    in.plan         = plan(guestRows(), OwnPowers());
    in.planKnown    = true;
    Details d       = buildDetails(in);
    QCOMPARE(d.title, QStringLiteral("TS Media chat group"));
    QCOMPARE(d.text, QStringLiteral("TS Media chat will create on My server:"));
    QCOMPARE(d.informative, QStringLiteral("Name: tsmediachat (regular server group)\n"
                                           "Icon: TS Media chat picture icon (16×16, 335 bytes)\n"
                                           "File permissions at power 75: upload, download, browse, create folders\n"
                                           "Copied from “Guest”: 26 permissions (restrictions marked negate aren't copied)\n"
                                           "Only admins with group power 75 or more can give or remove it"));
    QVERIFY(d.detailed.contains(QStringLiteral("i_ft_file_upload_power = 75\n")));
    QVERIFY(d.detailed.contains(QStringLiteral("i_icon_id = 2038059696")));
    QCOMPARE(d.detailed.count(QLatin1Char('\n')) + 1, in.plan.all().size());

    in.create    = false;
    in.status    = QStringLiteral("“tsmediachat” is ready");
    in.rowsKnown = true;
    in.groupRows = applied(in.plan);
    in.groupRows.remove(QStringLiteral("i_ft_file_upload_power"));
    in.groupRows.remove(QStringLiteral("b_client_info_view"));
    in.groupRows[QStringLiteral("i_client_talk_power")].value = 30;
    in.refused  = {QStringLiteral("b_client_info_view")};
    in.adopted  = 20;
    in.sameName = {group(31, "TSMediaChat")};
    in.ignored  = {group(6, "Server Admin", 1, kGroupIconId)};
    d           = buildDetails(in);
    QCOMPARE(d.text, in.status);
    QCOMPARE(d.informative, QStringLiteral("Group “TSMediaChat” (id 31) has the same name. TS Media chat uses the one with id 20.\n"
                                           "Group “Server Admin” (id 6) uses the TS Media chat icon but has other permissions, so TS Media chat ignores it."));
    QVERIFY(d.detailed.contains(QStringLiteral("i_ft_file_upload_power: missing")));
    QVERIFY(d.detailed.contains(QStringLiteral("b_client_info_view: needs higher server permissions")));
    QVERIFY(d.detailed.contains(QStringLiteral("i_ft_file_download_power = 75")));
    QVERIFY(d.detailed.contains(QStringLiteral("i_client_talk_power = 30 (not set by TS Media chat)")));
}

void TestAccessGroup::chatLines()
{
    JobResult r;
    r.success   = true;
    r.created   = true;
    r.groupRows = rowsOf({{"i_icon_id", static_cast<int>(kGroupIconId)}});
    ChatLines l = chatLinesFor(r, JobKind::Create, QStringLiteral("tsmediachat"), QStringLiteral("Guest"), true);
    QCOMPARE(l.info, QStringList{QStringLiteral("Created the server group “tsmediachat”. To let someone send files, right-click them and choose Server Groups > tsmediachat.")});
    QVERIFY(l.warnings.isEmpty());

    r.groupRows.clear();
    r.iconProblem = IconProblem::NoManagePermission;
    l             = chatLinesFor(r, JobKind::Create, QStringLiteral("tsmediachat"), QStringLiteral("Guest"), false);
    QCOMPARE(l.info, QStringList{QStringLiteral("Created the server group “tsmediachat” without its icon (you need permission to manage icons (b_icon_manage)).")});

    r.created = false;
    l         = chatLinesFor(r, JobKind::Repair, QStringLiteral("Media"), QStringLiteral("Guest"), true);
    QCOMPARE(l.info, QStringList{QStringLiteral("Repaired the server group “Media”.")});
    QVERIFY(chatLinesFor(r, JobKind::Repair, QStringLiteral("Media"), {}, false).info.isEmpty()); // only a complete repair

    JobResult failed;
    failed.error       = ErrorKind::ProtectRemoved;
    failed.errorDetail = QStringLiteral("i_group_needed_member_add_power");
    l                  = chatLinesFor(failed, JobKind::Create, {}, {}, false);
    QCOMPARE(l.warnings, QStringList{QStringLiteral("The server group “tsmediachat” couldn't be protected (i_group_needed_member_add_power), so TS Media chat removed it again.")});
    failed.error = ErrorKind::Timeout;
    l            = chatLinesFor(failed, JobKind::Create, {}, {}, false);
    QCOMPARE(l.warnings, QStringList{QStringLiteral("Couldn't create the server group “tsmediachat”: The server didn't answer. Check your connection and click Try again.")});
    failed.kind = JobKind::Repair;
    QVERIFY(chatLinesFor(failed, JobKind::Repair, {}, {}, false).warnings.isEmpty()); // the box says it
}

void TestAccessGroup::memberTexts()
{
    const QString alice = QStringLiteral("Alice %2");
    QCOMPARE(memberText(MemberOutcome::Added, true, alice, QStringLiteral("tsmediachat")), QStringLiteral("Alice %2 can now send files (added to “tsmediachat”)."));
    QCOMPARE(memberText(MemberOutcome::AlreadyMember, true, alice, {}), QStringLiteral("Alice %2 already has TS Media chat access."));
    QCOMPARE(memberText(MemberOutcome::Removed, false, alice, QStringLiteral("tsmediachat")),
             QStringLiteral("Alice %2 no longer has TS Media chat access (removed from “tsmediachat”)."));
    QCOMPARE(memberText(MemberOutcome::NotMember, false, alice, {}), QStringLiteral("Alice %2 doesn't have TS Media chat access."));
    QCOMPARE(memberText(MemberOutcome::NoGroup, true, alice, {}),
             QStringLiteral("This server has no TS Media chat group yet. Server admins can create it in TS Media chat settings, under Server access."));
    QCOMPARE(memberText(MemberOutcome::ServerQuery, true, alice, {}), QStringLiteral("Alice %2 is a ServerQuery client and can't get TS Media chat access."));
    QCOMPARE(memberText(MemberOutcome::Gone, true, {}, {}), QStringLiteral("That person is no longer on the server."));
    QCOMPARE(memberText(MemberOutcome::NoPermission, true, alice, {}, QStringLiteral("i_group_member_add_power")),
             QStringLiteral("Couldn't give Alice %2 TS Media chat access: your server permissions don't allow it (i_group_member_add_power)."));
    QCOMPARE(memberText(MemberOutcome::NoPermission, false, alice, {}, QStringLiteral("i_group_member_remove_power")),
             QStringLiteral("Couldn't remove TS Media chat access from Alice %2: your server permissions don't allow it (i_group_member_remove_power)."));
    QCOMPARE(memberText(MemberOutcome::Failed, true, alice, {}, QStringLiteral("x")), QStringLiteral("Couldn't give Alice %2 TS Media chat access: x"));
    QCOMPARE(memberMissingCopiesText(alice, QStringLiteral("tsmediachat"), QStringLiteral("Guest")),
             QStringLiteral("Note: “tsmediachat” is missing some of “Guest”'s permissions, so Alice %2 may lose some abilities. Repair it in TS Media chat settings."));
}

TSMEDIA_REGISTER_TEST(TestAccessGroup);

#include "tst_accessgroup.moc"
