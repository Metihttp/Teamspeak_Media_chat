#include "ts3api.h"

#include <QDir>

#include "pluginlog.h"

namespace ts3 {

TS3Functions funcs{};
QString      pluginId;

namespace {
bool chatDark = false; // setChatDark: the chat the lines are printed into has a dark theme

QString escapeBBCode(QString text)
{
    // TeamSpeak 3.6 drops [noparse] tags but still parses their content (and printMessage shows them
    // literally). A backslash before a bracket makes it literal in chat messages and printMessage alike,
    // so user supplied names can't turn into links or formatting.
    text.replace(QLatin1Char('['), QLatin1String("\\["));
    text.replace(QLatin1Char(']'), QLatin1String("\\]"));
    return text;
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

// 2.2 album
QString ownUid(uint64 sch)
{
    char* value = nullptr;
    if (!funcs.getClientSelfVariableAsString || funcs.getClientSelfVariableAsString(sch, CLIENT_UNIQUE_IDENTIFIER, &value) != ERROR_ok)
        return {};
    return takeString(value);
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

QString clientUid(uint64 sch, anyID client)
{
    char* value = nullptr;
    if (!client || !funcs.getClientVariableAsString || funcs.getClientVariableAsString(sch, client, CLIENT_UNIQUE_IDENTIFIER, &value) != ERROR_ok)
        return {};
    return takeString(value);
}

anyID clientIdByUid(uint64 sch, const QString& uid)
{
    anyID* clients = nullptr;
    if (uid.isEmpty() || !funcs.getClientList || funcs.getClientList(sch, &clients) != ERROR_ok || !clients)
        return 0;
    anyID found = 0;
    for (anyID* it = clients; *it; ++it) {
        if (clientUid(sch, *it) == uid) {
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

namespace {
void writeLog(LogLevel level, uint64 sch, const LogText& text)
{
    if (funcs.logMessage)
        funcs.logMessage(text.plain.toUtf8().constData(), level, "TSMedia", sch);
    plog::write(static_cast<int>(level), text.marked);
}
} // namespace

void log(LogLevel level, uint64 sch, const char* format, std::initializer_list<LogArg> args)
{
    writeLog(level, sch, formatLog(format, args));
}

void log(const char* fixedText, LogLevel level, uint64 sch)
{
    writeLog(level, sch, formatLog(fixedText, {}));
}

void log(const QString& message, LogLevel level, uint64 sch)
{
    writeLog(level, sch, unclassifiedLog(message));
}

void setChatDark(bool dark)
{
    chatDark = dark;
}

void print(uint64 sch, const QString& bbcode)
{
    if (!sch)
        return;
    const QByteArray utf8 = bbcode.toUtf8();
    // The tab the user is looking at (a private chat, the server tab) when it belongs to that connection.
    if (sch == currentConnection() && funcs.printMessageToCurrentTab)
        funcs.printMessageToCurrentTab(utf8.constData());
    else if (funcs.printMessage)
        funcs.printMessage(sch, utf8.constData(), PLUGIN_MESSAGE_TARGET_CHANNEL);
}

// Prefix colours keep 4.5:1 on TeamSpeak's white chat and on its dark one (#2b2d31).
void printInfo(uint64 sch, const QString& text)
{
    const QString color = QString::fromLatin1(chatDark ? "#949cf7" : "#4752c4");
    print(sch, QStringLiteral("[color=%1][b]TS Media chat[/b][/color] ").arg(color) + escapeBBCode(text));
}

void printWarning(uint64 sch, const QString& text)
{
    // Chat warnings put names in curly quotes; the plugin log marks those (logtext.h).
    writeLog(LogLevel_WARNING, sch, quotedNamesLog(text));
    const QString color = QString::fromLatin1(chatDark ? "#f0b232" : "#b54708");
    print(sch, QStringLiteral("[color=%1][b]TS Media chat ⚠[/b][/color] ").arg(color) + escapeBBCode(text));
}

} // namespace ts3
