#include "ts3api.h"

#include <QDir>

#include "chatcolors.h" // 2.7 F6
#include "pluginlog.h"
#include "uploadwait.h"

namespace ts3 {

TS3Functions funcs{};
QString      pluginId;

namespace {
// 2.7 F6: setLineColors: the colours for the chat the lines are printed into (unset: TeamSpeak's white chat).
bool                   linesSet = false;
chatcolors::LineColors lines;

const chatcolors::LineColors& currentLines()
{
    if (!linesSet) {
        lines    = chatcolors::lineColors(chatcolors::teamSpeakLight(), QString());
        linesSet = true;
    }
    return lines;
}

// A URL that can't leave TeamSpeak's [url=...] tag.
bool urlFitsTag(const QString& url)
{
    if (url.isEmpty())
        return false;
    for (const QChar c : url) {
        if (c == QLatin1Char('[') || c == QLatin1Char(']') || c == QLatin1Char('"') || c == QLatin1Char('\'') || c.isSpace() || c.unicode() < 0x20
            || c.unicode() == 0x7f)
            return false;
    }
    return true;
}

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

QString configDir()
{
    char path[1024] = {};
    if (funcs.getConfigPath)
        funcs.getConfigPath(path, sizeof(path));
    const QString dir = QString::fromUtf8(path);
    return dir.isEmpty() ? QString() : QDir::cleanPath(dir);
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

bool canAskFileInfo(uint64 sch)
{
    const auto variable = [sch](size_t flag) -> QString {
        char* value = nullptr;
        if (!funcs.getServerVariableAsString || funcs.getServerVariableAsString(sch, flag, &value) != ERROR_ok || !value)
            return {};
        return takeString(value);
    };
    return funcs.requestFileInfo && uploadwait::canAskFileInfo(variable(VIRTUALSERVER_VERSION), variable(VIRTUALSERVER_PLATFORM));
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

UidPresence uidPresence(uint64 sch, const QString& uid)
{
    UidPresence found;
    anyID*      clients = nullptr;
    if (uid.isEmpty() || !funcs.getClientList || funcs.getClientList(sch, &clients) != ERROR_ok || !clients)
        return found;
    const uint64 own = ownChannel(sch);
    for (anyID* it = clients; *it; ++it) {
        if (clientUid(sch, *it) != uid)
            continue;
        if (!found.client)
            found.client = *it;
        uint64 channel = 0;
        if (own != 0 && funcs.getChannelOfClient && funcs.getChannelOfClient(sch, *it, &channel) == ERROR_ok && channel == own) {
            found.client       = *it;
            found.inOwnChannel = true;
            break;
        }
    }
    funcs.freeMemory(clients);
    return found;
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

void setLineColors(const chatcolors::LineColors& colors)
{
    // Run-time copies (the strings go into BBCode TeamSpeak keeps).
    lines.accent   = QString(colors.accent.constData(), colors.accent.size());
    lines.warning  = QString(colors.warning.constData(), colors.warning.size());
    lines.body     = QString(colors.body.constData(), colors.body.size());
    lines.wrapBody = colors.wrapBody;
    linesSet       = true;
}

chatcolors::LineColors lineColors()
{
    return currentLines();
}

QString linkBBCode(const QString& url, const QString& text)
{
    if (!urlFitsTag(url))
        return {};
    // 2.7 F6 (M7): the link in the accent colour (TeamSpeak's own link colour is the document sheet's, which a custom dark
    // chat may not read). The one place that decides the tags' order (live check L-F6-3).
    return QString::fromLatin1("[url=") + url + QString::fromLatin1("][color=") + currentLines().accent + QString::fromLatin1("]") + escapeBBCode(text)
           + QString::fromLatin1("[/color][/url]");
}

namespace {
// TeamSpeak draws plugin lines in dark blue (#00008b): 1.2:1 on its dark chat (live test 2.2), so where that doesn't
// read (2.7 F6: on the chat's effective background) they get the chat's text colour of their own (prefixes and links
// inside keep theirs).
QByteArray printable(const QString& bbcode)
{
    const chatcolors::LineColors& c = currentLines();
    return (c.wrapBody ? QString::fromLatin1("[color=") + c.body + QString::fromLatin1("]") + bbcode + QString::fromLatin1("[/color]") : bbcode).toUtf8();
}
} // namespace

void print(uint64 sch, const QString& bbcode)
{
    if (!sch)
        return;
    const QByteArray utf8 = printable(bbcode);
    // The tab the user is looking at (a private chat, the server tab) when it belongs to that connection.
    if (sch == currentConnection() && funcs.printMessageToCurrentTab)
        funcs.printMessageToCurrentTab(utf8.constData());
    else if (funcs.printMessage)
        funcs.printMessage(sch, utf8.constData(), PLUGIN_MESSAGE_TARGET_CHANNEL);
}

// Prefix colours keep 4.5:1 on the chat's effective background (2.7 F6; 2.6's exact colours on TeamSpeak's white chat
// and its dark one).
void printInfo(uint64 sch, const QString& text)
{
    print(sch, QString::fromLatin1("[color=%1][b]TS Media chat[/b][/color] ").arg(currentLines().accent) + escapeBBCode(text));
}

void printInfo(uint64 sch, const QString& text, const QString& url, const QString& linkText)
{
    const QString link = linkBBCode(url, linkText);
    print(sch, QString::fromLatin1("[color=%1][b]TS Media chat[/b][/color] ").arg(currentLines().accent) + escapeBBCode(text)
                   + (link.isEmpty() ? QString() : QLatin1Char(' ') + link));
}

void printWarning(uint64 sch, const QString& text)
{
    // Chat warnings put names in curly quotes; the plugin log marks those (logtext.h).
    writeLog(LogLevel_WARNING, sch, quotedNamesLog(text));
    print(sch, QStringLiteral("[color=%1][b]TS Media chat ⚠[/b][/color] ").arg(currentLines().warning) + escapeBBCode(text));
}

void printInfoLink(uint64 sch, const QString& text, const QString& url, const QString& linkText)
{
    if (!sch || !funcs.printMessage)
        return;
    const QString link = linkBBCode(url, linkText);
    if (link.isEmpty())
        return;
    const QString line = QString::fromLatin1("[color=%1][b]TS Media chat[/b][/color] ").arg(currentLines().accent) + escapeBBCode(text) + QLatin1Char(' ') + link;
    // The channel tab even when another tab of sch is in front (print() would use that one).
    funcs.printMessage(sch, printable(line).constData(), PLUGIN_MESSAGE_TARGET_CHANNEL);
}

} // namespace ts3
