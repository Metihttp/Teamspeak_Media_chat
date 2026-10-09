#pragma once

// 2.2 servergroup: the pure logic behind the one-click "tsmediachat" server group (Settings > Server
// access). QtCore only and no TeamSpeak SDK headers, so the unit tests can use it: what the group gets
// (plan), what an existing group lacks (missing), whether a renamed group can be trusted (conformant),
// how the group is found, the embedded icon and its id.
//
// Permissions are handled by name here; AccessGroup (accessgroup.cpp) maps them to the server's ids.

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace access {

// The icon's id on the server is the CRC-32 of its bytes (TeamSpeak names icon files icon_<crc32>).
// 0x797A52B0: positive as int32, so the permission value and the file name use the same number.
constexpr quint32 kGroupIconId    = 2038059696u;
constexpr int     kGroupIconBytes = 335;
constexpr int     kFilePower      = 75; // what Server Admin has (defaults.sql); Normal has 50, Guest 25 without upload
constexpr int     kMaxProtection  = 75; // Server Admin protects itself with 75
constexpr int     kGroupType      = 1;  // regular (0 = template, 2 = query)
constexpr int     kMaxPermsPerRequest = 40;

QString groupName();      // "tsmediachat"
QString iconRemotePath(); // "/icon_2038059696"
QString iconFileName();   // "icon_2038059696"

// IEEE 802.3 CRC-32 (the one zlib and TeamSpeak use).
quint32 crc32(const QByteArray& data);

// The group icon (16 px, uploaded to the server) and its 32 px version for the plugin's own HiDPI UI.
// Owned copies of the embedded bytes.
QByteArray groupIcon16();
QByteArray groupIcon32();

struct PermValue {
    int  value   = 0;
    bool negated = false;
    bool skip    = false;
};
inline bool operator==(const PermValue& a, const PermValue& b)
{
    return a.value == b.value && a.negated == b.negated && a.skip == b.skip;
}
inline bool operator!=(const PermValue& a, const PermValue& b)
{
    return !(a == b);
}
using PermRows = QMap<QString, PermValue>; // permission name -> value

// Your own powers, when the server reported them (clientneededpermissions).
struct OwnPowers {
    std::optional<int> memberAdd;    // i_group_member_add_power
    std::optional<int> memberRemove; // i_group_member_remove_power
    std::optional<int> groupModify;  // i_group_modify_power
};

enum class ItemKind {
    Protection,       // i_group_needed_member_add_power / _remove_power: who can give the group out
    FileSet,          // upload, download, browse, create folders
    Quota,            // the upload / download quotas
    Copy,             // copied from the default group
    Icon,             // i_icon_id
    ModifyProtection, // i_group_needed_modify_power, sent last
};

struct PlanItem {
    QString   name;
    PermValue value;
    ItemKind  kind = ItemKind::Copy;
};

struct Plan {
    QList<PlanItem> protection;       // sent first, on its own, and required
    QList<PlanItem> body;             // file set, quotas, copies
    QList<PlanItem> icon;             // i_icon_id
    QList<PlanItem> modifyProtection; // only when your own i_group_modify_power is known
    int             copies = 0;       // how many of body are copies

    QList<PlanItem> all() const;
    int             protectionValue() const; // the needed member add power it sets
};

// clamp(own, 1, 75), or 75 when unknown.
int protectionValue(std::optional<int> own);

// Default-group permissions that are never copied: negated rows aside, the group's own identity and
// protection, the names the plan sets itself, and permission-system powers (they hit the server's caps).
bool isCopyExcluded(const QString& name);

Plan plan(const PermRows& defaultRows, const OwnPowers& own);

// The names that must resolve to server ids before anything is sent.
QStringList requiredNames();

// What a group lacks compared with the plan. Never "lowered" values: an admin may have changed them.
// Protection counts as missing when absent or < 1, a file permission when absent or <= 0, the icon when
// absent or 0, everything else when absent. Items refused earlier this session are listed apart.
struct Missing {
    QList<PlanItem> sendable;
    QList<PlanItem> refused;
    bool            isEmpty() const { return sendable.isEmpty() && refused.isEmpty(); }
};
Missing missing(const PermRows& groupRows, const Plan& plan, const QSet<QString>& refused = {});

bool isProtected(const PermRows& groupRows);    // both needed member powers >= 1
bool hasAnyFilePermission(const PermRows& rows); // at least one of the file set > 0
bool hasOurIcon(const PermRows& rows);

// A group found by its remembered id or by the icon is trusted only when every permission it has is one
// the plan sets, one copied from the default group, or cosmetic (icon, sort id, name in tree), and none
// is negated. Keeps a public icon id from turning Server Admin into "the TS Media chat group".
bool conformant(const PermRows& groupRows, const PermRows& defaultRows);
// For icon matches also: it carries protection or a file permission (an otherwise empty group isn't ours).
bool looksLikeOurs(const PermRows& groupRows);

// CLIENT_SERVERGROUPS ("6,8,12") -> ids. Garbage is skipped, duplicates dropped, at most 256 ids.
QList<quint64> parseServerGroups(const QString& text);

struct GroupInfo {
    quint64 id     = 0;
    QString name;
    int     type   = kGroupType;
    quint32 iconId = 0;
};

enum class Match { None, Name, Remembered, Icon };

struct Candidates {
    quint64          byName = 0;  // a regular group named tsmediachat (any case), the lowest id
    QList<quint64>   sameName;    // further groups with that name (listed in Details)
    QList<quint64>   fallback;    // remembered id first, then icon matches: adopted only if conformant
    QList<Match>     fallbackHow; // how each fallback matched
};
Candidates findCandidates(const QList<GroupInfo>& groups, quint64 defaultGroup, quint64 remembered);
bool       isOurName(const QString& name);

// Server side limits on names (TS3_MAX_SIZE_GROUP_NAME).
constexpr int kMaxGroupNameLength = 30;

} // namespace access
