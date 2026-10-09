#include "serversettings.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QStringList>

#include <algorithm>

#include "i18n.h"
#include "medialink.h"
#include "settings.h"

bool ServerOverrides::isEmpty() const
{
    return !dataSaver && !uploadDirectory && !uploadMaxMB && !addRequiredNotice;
}

bool ServerOverrides::sameValues(const ServerOverrides& other) const
{
    return dataSaver == other.dataSaver && uploadDirectory == other.uploadDirectory && uploadMaxMB == other.uploadMaxMB
           && addRequiredNotice == other.addRequiredNotice;
}

namespace serversettings {

namespace {

// Never QStringLiteral for QSettings keys and values: see the note at the top of settings.cpp.
QString key(const QString& group, const char* name)
{
    return group + QLatin1Char('/') + QString::fromLatin1(name);
}

QString ownedCopy(const QString& text)
{
    return text.isNull() ? QString() : QString(text.constData(), text.size());
}

// "true" / "false" (what QSettings writes) and 1 / 0; anything else is ignored.
std::optional<bool> readBool(const QSettings& s, const QString& name)
{
    const QVariant v = s.value(name);
    if (!v.isValid())
        return std::nullopt;
    const QString text = v.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1"))
        return true;
    if (text == QLatin1String("false") || text == QLatin1String("0"))
        return false;
    return std::nullopt;
}

bool isHexKey(const QString& text)
{
    if (text.size() != 12)
        return false;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    return true;
}

} // namespace

QString serverKey(const QString& serverUid)
{
    if (serverUid.isEmpty())
        return {};
    // The same expression as Core::cachePathFor.
    return QString::fromLatin1(QCryptographicHash::hash(serverUid.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

bool isGroupName(const QString& group, QString* id)
{
    static const QLatin1String prefix("server_");
    if (!group.startsWith(prefix))
        return false;
    const QString rest = group.mid(prefix.size());
    if (!isHexKey(rest))
        return false;
    if (id)
        *id = rest;
    return true;
}

QString groupName(const QString& id)
{
    return QString::fromLatin1("server_") + id;
}

QString sanitizeName(const QString& name)
{
    QString clean = displayFileName(name).simplified();
    if (clean.size() > kMaxNameLength) {
        clean.truncate(kMaxNameLength);
        if (clean.at(clean.size() - 1).isHighSurrogate())
            clean.chop(1);
    }
    return clean;
}

ServerOverrides readGroup(const QSettings& s, const QString& group)
{
    ServerOverrides o;
    o.name              = sanitizeName(s.value(key(group, "name")).toString());
    o.dataSaver         = readBool(s, key(group, "dataSaver"));
    o.addRequiredNotice = readBool(s, key(group, "addRequiredNotice"));

    const QVariant limit = s.value(key(group, "uploadMaxMB"));
    if (limit.isValid()) {
        bool         ok    = false;
        const qint64 value = limit.toString().trimmed().toLongLong(&ok);
        if (ok)
            o.uploadMaxMB = static_cast<int>(qBound<qint64>(Settings::uploadMaxMBRange.min, value, Settings::uploadMaxMBRange.max));
    }

    const QString dir = s.value(key(group, "uploadDirectory")).toString().trimmed();
    if (!dir.isEmpty() && dir.size() <= kMaxUploadDirectoryLength) {
        bool control = false;
        for (const QChar c : dir)
            control = control || c.unicode() < 0x20 || c.unicode() == 0x7f;
        if (!control)
            o.uploadDirectory = Settings::normalizeUploadDirectory(dir);
    }
    return o;
}

void writeGroup(QSettings& s, const QString& group, const ServerOverrides& o)
{
    if (!o.name.isEmpty())
        s.setValue(key(group, "name"), ownedCopy(sanitizeName(o.name)));
    if (o.dataSaver)
        s.setValue(key(group, "dataSaver"), *o.dataSaver);
    if (o.uploadDirectory)
        s.setValue(key(group, "uploadDirectory"), ownedCopy(Settings::normalizeUploadDirectory(*o.uploadDirectory)));
    if (o.uploadMaxMB)
        s.setValue(key(group, "uploadMaxMB"), qBound(Settings::uploadMaxMBRange.min, *o.uploadMaxMB, Settings::uploadMaxMBRange.max));
    if (o.addRequiredNotice)
        s.setValue(key(group, "addRequiredNotice"), *o.addRequiredNotice);
}

QHash<QString, ServerOverrides> readAll(const QSettings& s)
{
    QStringList groups = s.childGroups();
    std::sort(groups.begin(), groups.end()); // which ones are kept above the cap does not depend on the file's order
    QHash<QString, ServerOverrides> servers;
    for (const QString& group : qAsConst(groups)) {
        QString id;
        if (!isGroupName(group, &id))
            continue;
        const ServerOverrides o = readGroup(s, group);
        if (o.isEmpty())
            continue;
        servers.insert(id, o);
        if (servers.size() >= kMaxServers)
            break;
    }
    return servers;
}

void writeAll(QSettings& s, const QHash<QString, ServerOverrides>& servers)
{
    // A forgotten server must really disappear from the file (and so do broken server groups).
    const QStringList groups = s.childGroups();
    for (const QString& group : groups) {
        if (group.startsWith(QLatin1String("server_"), Qt::CaseInsensitive))
            s.remove(group);
    }
    QStringList ids = servers.keys();
    std::sort(ids.begin(), ids.end());
    int written = 0;
    for (const QString& id : qAsConst(ids)) {
        const ServerOverrides& o = servers[id];
        if (!isHexKey(id) || o.isEmpty())
            continue;
        writeGroup(s, groupName(id), o);
        if (++written >= kMaxServers)
            break;
    }
}

void applyOverrides(Settings& s, const ServerOverrides& o)
{
    if (o.dataSaver)
        s.dataSaver = *o.dataSaver;
    if (o.uploadDirectory)
        s.uploadDirectory = Settings::normalizeUploadDirectory(*o.uploadDirectory);
    if (o.uploadMaxMB)
        s.uploadMaxMB = qBound(Settings::uploadMaxMBRange.min, *o.uploadMaxMB, Settings::uploadMaxMBRange.max);
    if (o.addRequiredNotice)
        s.addRequiredNotice = *o.addRequiredNotice;
}

std::optional<bool> toggledOverride(bool global, bool want)
{
    if (global == want)
        return std::nullopt;
    return want;
}

DataSaverCommand parseDataSaverCommand(const QString& arguments)
{
    const QString arg = arguments.trimmed().toLower();
    if (arg.isEmpty())
        return DataSaverCommand::Status;
    if (arg == QLatin1String("on"))
        return DataSaverCommand::On;
    if (arg == QLatin1String("off"))
        return DataSaverCommand::Off;
    if (arg == QLatin1String("default"))
        return DataSaverCommand::Default;
    return DataSaverCommand::Invalid;
}

QString dataSaverChangedText(bool on)
{
    return on ? i18n::t("Data saver is on for this server. Images and videos load when you click them.")
              : i18n::t("Data saver is off for this server. Images and videos download automatically again.");
}

QString dataSaverStatusText(bool on, bool ownSetting)
{
    if (on)
        return ownSetting ? i18n::t("Data saver is on for this server (its own setting).") : i18n::t("Data saver is on for this server (same as all servers).");
    return ownSetting ? i18n::t("Data saver is off for this server (its own setting).") : i18n::t("Data saver is off for this server (same as all servers).");
}

QString dataSaverConnectText()
{
    return i18n::t("Data saver is on for this server. Images and videos load when you click them. Turn it off in Plugins → TS Media chat.");
}

QString dataSaverUsageText()
{
    return i18n::t("Use /tsmedia datasaver on, off or default.");
}

QString dataSaverNotConnectedText()
{
    return i18n::t("Connect to a server first. Data saver is set per server.");
}

} // namespace serversettings
