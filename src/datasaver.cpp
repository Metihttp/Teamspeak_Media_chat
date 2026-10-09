#include "datasaver.h"

#include <QSet>

#include "settings.h"

namespace datasaver {

namespace {

// Servers (by key) whose "data saver is on" notice was shown in this TeamSpeak session.
QSet<QString>& notified()
{
    static QSet<QString> keys;
    return keys;
}

uint64 orCurrent(uint64 sch)
{
    return sch ? sch : ts3::currentConnection();
}

// Gives the server of sch this data saver value of its own (std::nullopt: the one for all servers)
// and saves. False if nothing changed.
bool store(uint64 sch, const std::optional<bool>& value)
{
    const QString key = serversettings::serverKey(ts3::serverUid(sch));
    if (key.isEmpty())
        return false;
    Settings&       s   = Settings::instance();
    const bool      had = s.servers.contains(key);
    ServerOverrides own = s.servers.value(key);
    if (own.dataSaver == value)
        return false;
    if (!had && value && s.servers.size() >= serversettings::kMaxServers)
        return false; // the file keeps at most this many servers
    own.dataSaver      = value;
    const QString name = serversettings::sanitizeName(ts3::serverName(sch));
    if (!name.isEmpty())
        own.name = name;
    if (own.isEmpty())
        s.servers.remove(key); // servers without own values are not remembered
    else
        s.servers.insert(key, own);
    s.save();
    return true;
}

bool ownSetting(uint64 sch)
{
    const ServerOverrides* own = Settings::instance().overridesFor(ts3::serverUid(sch));
    return own && own->dataSaver.has_value();
}

// After the user changed it: no "data saver is on" notice later in this session for that server.
void markNotified(uint64 sch)
{
    const QString key = serversettings::serverKey(ts3::serverUid(sch));
    if (!key.isEmpty())
        notified().insert(key);
}

} // namespace

bool isOn(uint64 sch)
{
    return Settings::instance().forServer(ts3::serverUid(sch)).dataSaver;
}

void updateMenu(int pauseMenuId, int resumeMenuId)
{
    if (!ts3::funcs.setPluginMenuEnabled || ts3::pluginId.isEmpty())
        return;
    const uint64     sch       = ts3::currentConnection();
    const bool       connected = ts3::isConnected(sch);
    const bool       on        = connected && isOn(sch);
    const QByteArray id        = ts3::pluginId.toUtf8();
    ts3::funcs.setPluginMenuEnabled(id.constData(), pauseMenuId, connected && !on ? 1 : 0);
    ts3::funcs.setPluginMenuEnabled(id.constData(), resumeMenuId, on ? 1 : 0);
}

bool setFromMenu(uint64 sch, bool pause)
{
    sch = orCurrent(sch);
    if (!ts3::isConnected(sch)) {
        ts3::printInfo(sch ? sch : ts3::currentConnection(), serversettings::dataSaverNotConnectedText());
        return false;
    }
    const bool changed = store(sch, serversettings::toggledOverride(Settings::instance().dataSaver, pause));
    markNotified(sch);
    ts3::printInfo(sch, serversettings::dataSaverChangedText(isOn(sch)));
    return changed;
}

bool runCommand(uint64 sch, const QString& arguments)
{
    sch                = orCurrent(sch);
    const auto command = serversettings::parseDataSaverCommand(arguments);
    if (command == serversettings::DataSaverCommand::Invalid) {
        ts3::printWarning(sch, serversettings::dataSaverUsageText());
        return false;
    }
    if (!ts3::isConnected(sch)) {
        ts3::printInfo(sch, serversettings::dataSaverNotConnectedText());
        return false;
    }

    const bool global  = Settings::instance().dataSaver;
    bool       changed = false;
    switch (command) {
    case serversettings::DataSaverCommand::On:
    case serversettings::DataSaverCommand::Off:
        changed = store(sch, serversettings::toggledOverride(global, command == serversettings::DataSaverCommand::On));
        markNotified(sch);
        ts3::printInfo(sch, serversettings::dataSaverChangedText(isOn(sch)));
        return changed;
    case serversettings::DataSaverCommand::Default:
        changed = store(sch, std::nullopt);
        markNotified(sch);
        break;
    case serversettings::DataSaverCommand::Status:
    case serversettings::DataSaverCommand::Invalid:
        break;
    }
    ts3::printInfo(sch, serversettings::dataSaverStatusText(isOn(sch), ownSetting(sch)));
    return changed;
}

void onConnectionEstablished(uint64 sch)
{
    const QString key = serversettings::serverKey(ts3::serverUid(sch));
    if (key.isEmpty())
        return;
    // Only servers with own settings are stored; their name is kept current for Settings → Servers.
    Settings& s  = Settings::instance();
    auto      it = s.servers.find(key);
    if (it != s.servers.end()) {
        const QString name = serversettings::sanitizeName(ts3::serverName(sch));
        if (!name.isEmpty() && name != it->name) {
            it->name = name;
            s.save();
        }
    }
    if (isOn(sch) && !notified().contains(key)) {
        notified().insert(key);
        ts3::printInfo(sch, serversettings::dataSaverConnectText());
    }
}

QVector<ConnectedServer> connectedServers()
{
    QVector<ConnectedServer> servers;
    QSet<QString>            seen; // the same server open in two tabs is listed once
    const uint64             current = ts3::currentConnection();
    QList<uint64>            handlers = ts3::connections();
    if (handlers.removeAll(current) > 0)
        handlers.prepend(current);
    for (uint64 sch : qAsConst(handlers)) {
        if (!ts3::isConnected(sch))
            continue;
        const QString key = serversettings::serverKey(ts3::serverUid(sch));
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        ConnectedServer server;
        server.key     = key;
        server.name    = serversettings::sanitizeName(ts3::serverName(sch));
        server.current = sch == current;
        servers.append(server);
    }
    return servers;
}

} // namespace datasaver
