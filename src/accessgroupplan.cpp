#include "accessgroupplan.h"

#include <algorithm>

#include "groupicon_data.h" // generated from assets/ by cmake/embed_files.cmake

namespace access {

namespace {

// Built at run time (fromLatin1), never QStringLiteral: these names end up in QSettings, tooltips and
// other Qt state that can outlive the DLL.
QString name(const char* text)
{
    return QString::fromLatin1(text);
}

const char* const kNeededAdd    = "i_group_needed_member_add_power";
const char* const kNeededRemove = "i_group_needed_member_remove_power";
const char* const kNeededModify = "i_group_needed_modify_power";
const char* const kIconId       = "i_icon_id";

const char* const kFileSet[] = {"i_ft_file_upload_power", "i_ft_file_download_power", "i_ft_file_browse_power", "i_ft_directory_create_power"};
const char* const kQuotas[]  = {"i_ft_quota_mb_upload_per_client", "i_ft_quota_mb_download_per_client"};

// Cosmetic permissions an admin may set on the group without making it "someone else's".
const char* const kCosmetic[] = {"i_icon_id", "i_group_sort_id", "i_group_show_name_in_tree"};

// Permission-system powers: copying them would hand members parts of the permission system, and the
// server caps them at the editor's own values (permissiondoc.txt, "Who can edit the permission system").
const char* const kPermissionSystem[] = {"i_permission_modify_power",       "i_group_modify_power",          "i_group_member_add_power",
                                         "i_group_member_remove_power",     "i_client_permission_modify_power", "i_group_auto_update_max_value"};

bool inList(const QString& value, const char* const* begin, const char* const* end)
{
    return std::any_of(begin, end, [&value](const char* item) { return value == QLatin1String(item); });
}

template <size_t N>
bool inList(const QString& value, const char* const (&list)[N])
{
    return inList(value, list, list + N);
}

PlanItem item(const char* n, int value, ItemKind kind)
{
    PlanItem i;
    i.name        = name(n);
    i.value.value = value;
    i.kind        = kind;
    return i;
}

QSet<QString> copyNames(const PermRows& defaultRows)
{
    QSet<QString> names;
    for (auto it = defaultRows.cbegin(); it != defaultRows.cend(); ++it) {
        if (!it->negated && !isCopyExcluded(it.key()))
            names.insert(it.key());
    }
    return names;
}

QByteArray copyOf(const unsigned char* data, size_t size)
{
    return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(size)); // deep copy
}

} // namespace

QString groupName()
{
    return name("tsmediachat");
}

QString iconFileName()
{
    return name("icon_") + QString::number(kGroupIconId);
}

QString iconRemotePath()
{
    return QLatin1Char('/') + iconFileName();
}

quint32 crc32(const QByteArray& data)
{
    static const auto table = [] {
        struct Table {
            quint32 v[256];
        } t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t.v[i] = c;
        }
        return t;
    }();
    quint32 crc = 0xFFFFFFFFu;
    for (const char ch : data)
        crc = table.v[(crc ^ static_cast<quint8>(ch)) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

QByteArray groupIcon16()
{
    return copyOf(tsmediachat_group_16, sizeof(tsmediachat_group_16));
}

QByteArray groupIcon32()
{
    return copyOf(tsmediachat_group_32, sizeof(tsmediachat_group_32));
}

QList<PlanItem> Plan::all() const
{
    return protection + body + icon + modifyProtection;
}

int Plan::protectionValue() const
{
    return protection.isEmpty() ? kMaxProtection : protection.first().value.value;
}

int protectionValue(std::optional<int> own)
{
    return own ? qBound(1, *own, kMaxProtection) : kMaxProtection;
}

bool isCopyExcluded(const QString& n)
{
    return n == QLatin1String(kIconId) || n.startsWith(QLatin1String("i_group_needed_")) || inList(n, kCosmetic) || inList(n, kFileSet) || inList(n, kQuotas)
           || inList(n, kPermissionSystem) || n.startsWith(QLatin1String("i_needed_modify_power_"));
}

Plan plan(const PermRows& defaultRows, const OwnPowers& own)
{
    Plan p;
    p.protection << item(kNeededAdd, protectionValue(own.memberAdd), ItemKind::Protection)
                 << item(kNeededRemove, protectionValue(own.memberRemove), ItemKind::Protection);

    for (const char* n : kFileSet)
        p.body << item(n, kFilePower, ItemKind::FileSet); // negated 0, skip 0, like TeamSpeak's own templates
    for (const char* n : kQuotas) {
        const auto it = defaultRows.constFind(name(n));
        p.body << item(n, it != defaultRows.cend() && !it->negated ? it->value : -1, ItemKind::Quota);
    }
    // Negated rows aren't copied: negate takes the lowest value and would restrict members' other groups.
    for (auto it = defaultRows.cbegin(); it != defaultRows.cend(); ++it) {
        if (it->negated || isCopyExcluded(it.key()))
            continue;
        PlanItem copy;
        copy.name          = it.key();
        copy.value.value   = it->value;
        copy.value.skip    = it->skip;
        copy.value.negated = false;
        copy.kind          = ItemKind::Copy;
        p.body << copy;
        ++p.copies;
    }

    p.icon << item(kIconId, static_cast<int>(kGroupIconId), ItemKind::Icon);
    // Last, and only from a known value of your own (>= 1): a guess could lock you out of later repairs.
    if (own.groupModify && *own.groupModify >= 1)
        p.modifyProtection << item(kNeededModify, qMin(*own.groupModify, kMaxProtection), ItemKind::ModifyProtection);
    return p;
}

QStringList requiredNames()
{
    QStringList names;
    names << name(kNeededAdd) << name(kNeededRemove) << name(kNeededModify) << name(kIconId);
    for (const char* n : kFileSet)
        names << name(n);
    for (const char* n : kQuotas)
        names << name(n);
    return names;
}

Missing missing(const PermRows& groupRows, const Plan& plan, const QSet<QString>& refused)
{
    Missing result;
    for (const PlanItem& want : plan.all()) {
        const auto it      = groupRows.constFind(want.name);
        const bool present = it != groupRows.cend();
        bool       lacks   = !present;
        if (present) {
            switch (want.kind) {
            case ItemKind::Protection:
                lacks = it->value < 1;
                break;
            case ItemKind::FileSet:
                lacks = it->value <= 0;
                break;
            case ItemKind::Icon:
                lacks = it->value == 0;
                break;
            case ItemKind::Quota:
            case ItemKind::Copy:
            case ItemKind::ModifyProtection:
                break;
            }
        }
        if (!lacks)
            continue;
        (refused.contains(want.name) ? result.refused : result.sendable).append(want);
    }
    return result;
}

bool isProtected(const PermRows& groupRows)
{
    const auto add    = groupRows.constFind(name(kNeededAdd));
    const auto remove = groupRows.constFind(name(kNeededRemove));
    return add != groupRows.cend() && remove != groupRows.cend() && add->value >= 1 && remove->value >= 1;
}

bool hasAnyFilePermission(const PermRows& rows)
{
    for (const char* n : kFileSet) {
        const auto it = rows.constFind(name(n));
        if (it != rows.cend() && it->value > 0)
            return true;
    }
    return false;
}

bool hasOurIcon(const PermRows& rows)
{
    const auto it = rows.constFind(name(kIconId));
    return it != rows.cend() && static_cast<quint32>(it->value) == kGroupIconId;
}

bool conformant(const PermRows& groupRows, const PermRows& defaultRows)
{
    QSet<QString> allowed = copyNames(defaultRows);
    for (const QString& n : requiredNames())
        allowed.insert(n);
    for (const char* n : kCosmetic)
        allowed.insert(name(n));
    for (auto it = groupRows.cbegin(); it != groupRows.cend(); ++it) {
        if (it->negated || !allowed.contains(it.key()))
            return false;
    }
    return true;
}

bool looksLikeOurs(const PermRows& groupRows)
{
    return hasAnyFilePermission(groupRows) || groupRows.contains(name(kNeededAdd)) || groupRows.contains(name(kNeededRemove));
}

QList<quint64> parseServerGroups(const QString& text)
{
    QList<quint64> ids;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool          ok = false;
        const quint64 id = part.trimmed().toULongLong(&ok);
        if (!ok || id == 0 || ids.contains(id))
            continue;
        ids.append(id);
        if (ids.size() >= 256)
            break;
    }
    return ids;
}

bool isOurName(const QString& n)
{
    return n.compare(groupName(), Qt::CaseInsensitive) == 0;
}

Candidates findCandidates(const QList<GroupInfo>& groups, quint64 defaultGroup, quint64 remembered)
{
    Candidates     c;
    QList<quint64> named;
    QList<quint64> icons;
    bool           rememberedRegular = false;
    for (const GroupInfo& g : groups) {
        if (g.id == 0 || g.id == defaultGroup || g.type != kGroupType)
            continue; // templates and query groups can't be given to people; the default group never
        if (isOurName(g.name))
            named.append(g.id);
        if (g.id == remembered)
            rememberedRegular = true;
        if (g.iconId == kGroupIconId)
            icons.append(g.id);
    }
    std::sort(named.begin(), named.end());
    std::sort(icons.begin(), icons.end());
    if (!named.isEmpty()) {
        c.byName   = named.takeFirst();
        c.sameName = named;
        return c;
    }
    if (rememberedRegular) {
        c.fallback << remembered;
        c.fallbackHow << Match::Remembered;
    }
    for (quint64 id : qAsConst(icons)) {
        if (c.fallback.contains(id))
            continue;
        c.fallback << id;
        c.fallbackHow << Match::Icon;
    }
    return c;
}

} // namespace access
