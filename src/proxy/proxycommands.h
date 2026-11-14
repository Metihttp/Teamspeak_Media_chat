#pragma once

// 2.6 chat proxy: the Plugins menu pair, the hotkey and the /tsmedia proxy command (design 1.4). The plugin's entry
// points (plugin.cpp) only forward here, on the GUI thread; the lines come from proxy::Control (every toggle prints
// one TS Media line, proxytexts.h). The SDK can't check-mark a menu item: the state shows as which of the two items is
// enabled. TeamSpeak's own menus and actions are never touched.

#include <QString>
#include <QVector>

#include "teamspeak/public_definitions.h"

namespace proxy::commands {

constexpr int  kMenuOn  = 8; // "Turn on chat proxy": enabled while off (also with nothing saved: it opens the window)
constexpr int  kMenuOff = 9; // "Turn off chat proxy": enabled while on
constexpr char kHotkey[] = "tsmedia_proxy";

struct MenuItem {
    int     id = 0;
    QString text;
};
QVector<MenuItem> menuItems(); // in this order, after the data saver pair

// The pair's enabled state from the saved config (setPluginMenuEnabled; nothing without a plugin id).
void updateMenu();
// A menu item of ours (true) turns the chat proxy on or off for the current server tab's chat line.
bool handleMenu(int id, uint64 sch);
// The hotkey: on if off, else off; with nothing saved only the line (never a window, F17d).
bool handleHotkey(const QString& keyword, uint64 sch);
// "/tsmedia proxy [on|off]": the status, on, off; anything else prints the usage warning.
void runCommand(uint64 sch, const QString& arguments);

} // namespace proxy::commands
