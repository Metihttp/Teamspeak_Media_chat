#pragma once

// 2.2 per-server settings and data saver: the TeamSpeak side (which server a tab is on, the Plugins
// menu pair, /tsmedia datasaver, the notice when connecting). GUI thread only. The settings
// themselves are in serversettings.* / Settings.

#include <QString>
#include <QVector>

#include "serversettings.h"
#include "ts3api.h"

class Core;

namespace datasaver {

// The data saver applies to the server of connection sch (its own setting or the one for all).
bool isOn(uint64 sch);

// Plugins menu: "Pause automatic downloads on this server" and "Resume ..." are an enable/disable
// pair (the SDK can't change menu texts or check marks). Exactly one is enabled while the current
// tab is connected, none otherwise.
void updateMenu(int pauseMenuId, int resumeMenuId);

// The menu items and the command. Return true if a setting changed: the caller then tells Core
// (onDataSaverChanged), updates the menu and refreshes an open settings dialog.
bool setFromMenu(uint64 sch, bool pause);
bool runCommand(uint64 sch, const QString& arguments); // "on", "off", "default" or "" (status)

// After a connection is established: keeps the stored name of a server with own settings current,
// and says once per TeamSpeak session that the data saver is on there.
void onConnectionEstablished(uint64 sch);

// Servers the user is connected to, for Settings → Servers (the current tab first).
QVector<ConnectedServer> connectedServers();

} // namespace datasaver
