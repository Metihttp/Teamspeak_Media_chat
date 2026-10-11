#pragma once

// Thin Qt-friendly wrapper around the TeamSpeak 3 client plugin API.

#include <QList>
#include <QString>

#include <initializer_list>

#include "logtext.h" // 2.2: LogArg, pub(), file(), local(), name()
#include "plugin_definitions.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

namespace ts3 {

extern TS3Functions funcs;
// From ts3plugin_registerPluginID: a random GUID ("{8-4-4-4-12}") on every load (S0). Use it to send
// plugin commands and for setPluginMenuEnabled; receivers see the plugin's name "tsmedia" instead.
extern QString pluginId;

// <TeamSpeak config>/plugins/tsmedia (created on demand).
QString dataDir();
// Chat redesign: TeamSpeak's config folder itself (its cache of avatars is in <config>/cache). Empty if unknown.
QString configDir();

QString errorText(unsigned int error);

// Converts a string allocated by the client to QString and frees it.
QString takeString(char* str);

uint64        currentConnection();
QList<uint64> connections();
bool          isConnected(uint64 sch);
uint64        connectionForServerUid(const QString& uid);

anyID   ownClientId(uint64 sch);
uint64  ownChannel(uint64 sch);
QString ownUid(uint64 sch); // 2.2 album: our own unique identifier on sch (empty if not connected)
QString serverUid(uint64 sch);
QString serverName(uint64 sch);
QString channelName(uint64 sch, uint64 channelId);
bool    channelHasPassword(uint64 sch, uint64 channelId);
anyID   clientIdByNickname(uint64 sch, const QString& nickname);
QString clientUid(uint64 sch, anyID client);           // CLIENT_UNIQUE_IDENTIFIER, empty if unknown
anyID   clientIdByUid(uint64 sch, const QString& uid); // 0 if no such client is on the server (now)

bool getServerAddress(uint64 sch, QString* host, quint16* port);

QString newReturnCode();

// 2.2 structured log: every new line uses this, with each value tagged (logtext.h), e.g.
//   ts3::log(LogLevel_WARNING, sch, "Couldn't move %1 to %2", {ts3::file(remote), ts3::local(path)});
// TeamSpeak's log gets the plain text, the plugin log (pluginlog.h) the marked one.
void log(LogLevel level, uint64 sch, const char* format, std::initializer_list<LogArg> args);
// A constant line (nothing private in it).
void log(const char* fixedText, LogLevel level = LogLevel_INFO, uint64 sch = 0);
// 2.1's free-text lines: the plugin log keeps the whole line as unclassified (private) text. Don't use
// it for new lines.
void log(const QString& message, LogLevel level = LogLevel_INFO, uint64 sch = 0);

// Prints a BBCode line into the chat tab the user is looking at when sch is the current connection,
// otherwise into the channel tab of sch (local only).
void print(uint64 sch, const QString& bbcode);
// The chat's theme (ChatIntegration knows it): printInfo / printWarning pick prefix colours that are
// readable on it.
void setChatDark(bool dark);
void printInfo(uint64 sch, const QString& text);
void printWarning(uint64 sch, const QString& text);

} // namespace ts3
