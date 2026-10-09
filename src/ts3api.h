#pragma once

// Thin Qt-friendly wrapper around the TeamSpeak 3 client plugin API.

#include <QList>
#include <QString>

#include "plugin_definitions.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_rare_definitions.h"
#include "ts3_functions.h"

namespace ts3 {

extern TS3Functions funcs;
extern QString      pluginId;

// <TeamSpeak config>/plugins/tsmedia (created on demand).
QString dataDir();

QString errorText(unsigned int error);

// Converts a string allocated by the client to QString and frees it.
QString takeString(char* str);

uint64        currentConnection();
QList<uint64> connections();
bool          isConnected(uint64 sch);
uint64        connectionForServerUid(const QString& uid);

anyID   ownClientId(uint64 sch);
uint64  ownChannel(uint64 sch);
QString serverUid(uint64 sch);
QString serverName(uint64 sch);
QString channelName(uint64 sch, uint64 channelId);
bool    channelHasPassword(uint64 sch, uint64 channelId);
anyID   clientIdByNickname(uint64 sch, const QString& nickname);

bool getServerAddress(uint64 sch, QString* host, quint16* port);

QString newReturnCode();

void log(const QString& message, LogLevel level = LogLevel_INFO, uint64 sch = 0);

// Prints a BBCode line into the channel chat tab of the given connection (local only).
void print(uint64 sch, const QString& bbcode);
void printInfo(uint64 sch, const QString& text);
void printWarning(uint64 sch, const QString& text);

} // namespace ts3
