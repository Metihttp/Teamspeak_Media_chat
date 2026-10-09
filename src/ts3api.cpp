#include "ts3api.h"

#include <QDir>

namespace ts3 {

TS3Functions funcs{};
QString      pluginId;

namespace {
QString escapeBBCode(QString text)
{
    // [noparse] is supported by the TeamSpeak chat and keeps user supplied names from being interpreted.
    text.replace(QStringLiteral("[/noparse]"), QStringLiteral("[ /noparse]"));
    return QStringLiteral("[noparse]") + text + QStringLiteral("[/noparse]");
}
} // namespace

QString dataDir()
{
    char path[1024] = {};
    if (funcs.getConfigPath)
        funcs.getConfigPath(path, sizeof(path));
    QString dir = QDir::cleanPath(QString::fromUtf8(path) + QStringLiteral("/plugins/tsmedia"));
    QDir().mkpath(dir);
    return dir;
}

QString errorText(unsigned int error)
{
    char* msg = nullptr;
    if (funcs.getErrorMessage && funcs.getErrorMessage(error, &msg) == ERROR_ok && msg)
        return takeString(msg);
    return QStringLiteral("error 0x%1").arg(error, 4, 16, QLatin1Char('0'));
}

QString takeString(char* str)
{
    if (!str)
        return {};
    QString result = QString::fromUtf8(str);
    funcs.freeMemory(str);
    return result;
}

uint64 currentConnection()
{
    return funcs.getCurrentServerConnectionHandlerID ? funcs.getCurrentServerConnectionHandlerID() : 0;
}

QList<uint64> connections()
{
    QList<uint64> result;
    uint64*       list = nullptr;
    if (funcs.getServerConnectionHandlerList(&list) != ERROR_ok || !list)
        return result;
    for (uint64* it = list; *it; ++it)
        result.append(*it);
    funcs.freeMemory(list);
    return result;
}

bool isConnected(uint64 sch)
{
    int status = STATUS_DISCONNECTED;
    if (!sch || funcs.getConnectionStatus(sch, &status) != ERROR_ok)
        return false;
    return status == STATUS_CONNECTION_ESTABLISHED;
}

uint64 connectionForServerUid(const QString& uid)
{
    if (uid.isEmpty())
        return 0;
    // Prefer the tab the user is looking at when connected to the same server twice.
    const uint64 current = currentConnection();
    if (isConnected(current) && serverUid(current) == uid)
        return current;
    for (uint64 sch : connections()) {
        if (isConnected(sch) && serverUid(sch) == uid)
            return sch;
    }
    return 0;
}

anyID ownClientId(uint64 sch)
{
    anyID id = 0;
    if (funcs.getClientID(sch, &id) != ERROR_ok)
        return 0;
    return id;
}

uint64 ownChannel(uint64 sch)
{
    const anyID me = ownClientId(sch);
    uint64      channel = 0;
    if (!me || funcs.getChannelOfClient(sch, me, &channel) != ERROR_ok)
        return 0;
    return channel;
}

QString serverUid(uint64 sch)
{
    char* value = nullptr;
    if (funcs.getServerVariableAsString(sch, VIRTUALSERVER_UNIQUE_IDENTIFIER, &value) != ERROR_ok)
        return {};
    return takeString(value);
}

QString serverName(uint64 sch)
{
    char* value = nullptr;
    if (funcs.getServerVariableAsString(sch, VIRTUALSERVER_NAME, &value) != ERROR_ok)
        return {};
    return takeString(value);
}

QString channelName(uint64 sch, uint64 channelId)
{
    char* value = nullptr;
    if (funcs.getChannelVariableAsString(sch, channelId, CHANNEL_NAME, &value) != ERROR_ok)
        return {};
    return takeString(value);
}

bool channelHasPassword(uint64 sch, uint64 channelId)
{
    int value = 0;
    if (funcs.getChannelVariableAsInt(sch, channelId, CHANNEL_FLAG_PASSWORD, &value) != ERROR_ok)
        return false;
    return value != 0;
}

anyID clientIdByNickname(uint64 sch, const QString& nickname)
{
    anyID* clients = nullptr;
    if (nickname.isEmpty() || funcs.getClientList(sch, &clients) != ERROR_ok || !clients)
        return 0;
    anyID found = 0;
    for (anyID* it = clients; *it; ++it) {
        char* name = nullptr;
        if (funcs.getClientVariableAsString(sch, *it, CLIENT_NICKNAME, &name) == ERROR_ok && takeString(name) == nickname) {
            found = *it;
            break;
        }
    }
    funcs.freeMemory(clients);
    return found;
}

bool getServerAddress(uint64 sch, QString* host, quint16* port)
{
    char           hostBuf[512] = {};
    char           passBuf[512] = {};
    unsigned short p            = 0;
    if (!funcs.getServerConnectInfo || funcs.getServerConnectInfo(sch, hostBuf, &p, passBuf, sizeof(hostBuf)) != ERROR_ok)
        return false;
    *host = QString::fromUtf8(hostBuf);
    *port = p;
    return true;
}

QString newReturnCode()
{
    char buffer[128] = {};
    const QByteArray id = pluginId.toUtf8();
    funcs.createReturnCode(id.constData(), buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

void log(const QString& message, LogLevel level, uint64 sch)
{
    if (funcs.logMessage)
        funcs.logMessage(message.toUtf8().constData(), level, "TSMedia", sch);
}

void print(uint64 sch, const QString& bbcode)
{
    if (!funcs.printMessage || !sch)
        return;
    funcs.printMessage(sch, bbcode.toUtf8().constData(), PLUGIN_MESSAGE_TARGET_CHANNEL);
}

void printInfo(uint64 sch, const QString& text)
{
    print(sch, QStringLiteral("[color=#3a7bd5][b]TS Media chat[/b][/color] ") + escapeBBCode(text));
}

void printWarning(uint64 sch, const QString& text)
{
    log(text, LogLevel_WARNING, sch);
    print(sch, QStringLiteral("[color=#d35400][b]TS Media chat ⚠[/b][/color] ") + escapeBBCode(text));
}

} // namespace ts3
