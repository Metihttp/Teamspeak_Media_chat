#include "proxycommands.h"

#include "control.h"
#include "proxytexts.h"
#include "ts3api.h"

namespace proxy::commands {

QVector<MenuItem> menuItems()
{
    return {{kMenuOn, texts::menuOnText()}, {kMenuOff, texts::menuOffText()}};
}

void updateMenu()
{
    Control* control = Control::current();
    if (!control || !ts3::funcs.setPluginMenuEnabled || ts3::pluginId.isEmpty())
        return;
    const QByteArray id = ts3::pluginId.toUtf8();
    const bool       on = control->config().on;
    ts3::funcs.setPluginMenuEnabled(id.constData(), kMenuOn, on ? 0 : 1);
    ts3::funcs.setPluginMenuEnabled(id.constData(), kMenuOff, on ? 1 : 0);
}

bool handleMenu(int id, uint64 sch)
{
    if (id != kMenuOn && id != kMenuOff)
        return false;
    if (Control* control = Control::current()) {
        if (id == kMenuOn)
            control->turnOn(Control::Origin::Menu, sch);
        else
            control->turnOff(Control::Origin::Menu, sch);
    }
    updateMenu(); // e.g. a stale pair after a reload
    return true;
}

bool handleHotkey(const QString& keyword, uint64 sch)
{
    if (keyword != QLatin1String(kHotkey))
        return false;
    if (Control* control = Control::current())
        control->toggle(Control::Origin::Hotkey, sch ? sch : ts3::currentConnection());
    updateMenu();
    return true;
}

void runCommand(uint64 sch, const QString& arguments)
{
    Control* control = Control::current();
    if (!control)
        return;
    const QString arg = arguments.trimmed().toLower();
    if (arg.isEmpty()) {
        ts3::printInfo(sch, texts::statusLine(control->config(), control->state(), control->lastFailure()));
    } else if (arg == QLatin1String("on")) {
        control->turnOn(Control::Origin::Command, sch);
    } else if (arg == QLatin1String("off")) {
        control->turnOff(Control::Origin::Command, sch);
    } else {
        ts3::printWarning(sch, texts::usageLine());
    }
    updateMenu();
}

} // namespace proxy::commands
