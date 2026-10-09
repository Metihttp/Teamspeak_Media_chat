#pragma once

// 2.2 per-server settings: the few settings a single server can have its own value for, kept in
// settings.ini as [server_<id>] groups. Pure QtCore (unit-tested): no TeamSpeak calls.
//
// <id> is serverKey(VIRTUALSERVER_UNIQUE_IDENTIFIER): the first 12 hex digits of its SHA-1, the
// same id as the server's cache/<id>/ folder. Only servers with at least one own value are stored.

#include <QHash>
#include <QString>

#include <optional>

class QSettings;
struct Settings;

// One server's own values; an empty optional means "same as all servers".
struct ServerOverrides {
    QString             name; // the server's name when last seen (display only, sanitised)
    std::optional<bool> dataSaver;
    std::optional<QString> uploadDirectory; // normalised (Settings::normalizeUploadDirectory)
    std::optional<int>  uploadMaxMB;        // within Settings::uploadMaxMBRange
    std::optional<bool> addRequiredNotice;

    // No own value at all (the name alone does not count): such a server is not stored.
    bool isEmpty() const;
    bool sameValues(const ServerOverrides& other) const; // ignores the name
};

// A server the user is connected to right now (Settings → Servers lists them first).
struct ConnectedServer {
    QString key;  // serversettings::serverKey(uid)
    QString name; // sanitizeName(VIRTUALSERVER_NAME)
    bool    current = false; // the server of the tab the user is looking at
};

namespace serversettings {

// At most this many servers are kept (the rest of a hand-edited file is ignored).
constexpr int kMaxServers = 200;
// TS3_MAX_SIZE_VIRTUALSERVER_NAME
constexpr int kMaxNameLength = 64;
// Longer upload folders are not used.
constexpr int kMaxUploadDirectoryLength = 256;

// The id of a server: SHA-1 of its unique identifier, first 12 lower-case hex digits (Core's cache
// folder uses the same). Empty for an empty uid.
QString serverKey(const QString& serverUid);

// "server_<id>" groups of settings.ini; *key gets the id. False for any other group name.
bool    isGroupName(const QString& group, QString* key = nullptr);
QString groupName(const QString& key);

// A server name for display: control and bidi characters removed, trimmed, at most 64 characters.
QString sanitizeName(const QString& name);

// Reads / writes one [server_<id>] group (group = groupName(key)). Values are validated: garbage is
// ignored, numbers clamped, folders normalised. Keys are built at run time (never QStringLiteral:
// QSettings keeps them after the plugin is unloaded, see settings.cpp).
ServerOverrides readGroup(const QSettings& settings, const QString& group);
void            writeGroup(QSettings& settings, const QString& group, const ServerOverrides& overrides);

// Every valid server group of settings (at most kMaxServers, empty ones dropped).
QHash<QString, ServerOverrides> readAll(const QSettings& settings);
// Removes every server group, then writes the non-empty ones of servers (at most kMaxServers).
void writeAll(QSettings& settings, const QHash<QString, ServerOverrides>& servers);

// settings with the server's own values applied.
void applyOverrides(Settings& settings, const ServerOverrides& overrides);

// The override a menu toggle or command sets so the server ends up with want: none when that is
// what all servers do anyway (global == want), else want.
std::optional<bool> toggledOverride(bool global, bool want);

// "/tsmedia datasaver [on|off|default]".
enum class DataSaverCommand { Status, On, Off, Default, Invalid };
DataSaverCommand parseDataSaverCommand(const QString& arguments);

// Chat lines for the data saver (local only): after a change, for the status command, and once per
// session when connecting to a server where it is on.
QString dataSaverChangedText(bool on);
QString dataSaverStatusText(bool on, bool ownSetting);
QString dataSaverConnectText();
QString dataSaverUsageText();
QString dataSaverNotConnectedText();

} // namespace serversettings
