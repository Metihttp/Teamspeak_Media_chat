#include "fakets3.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdlib>
#include <cstring>

#include "ts3api.h"

namespace fakets3 {

namespace {

struct State {
    QString                    configDir;
    bool                       connected = true;
    QHash<QString, QByteArray> files;
    QList<Transfer>            transfers;
    QList<Message>             messages;
    QList<Directory>           directories;
    QList<InfoRequest>         infoRequests; // 2.3.2
    QStringList                log;
    anyID                      nextTransfer   = 100;
    int                        nextReturnCode = 0;
    QString                    serverVersion  = QStringLiteral("3.13.7 [Build: 1655727713]"); // 2.3.2
    QString                    serverPlatform = QStringLiteral("Linux");
    bool                       resumeSupported = true;
    bool                       resumeFromStart = false;
    QStringList                deletedWhileChecking;
    QStringList                channelPrints; // 2.3.4
    // 2.4 link previews
    QHash<anyID, Client>       clients;
    QString                    ownUid;
    uint64                     ownChannel = kChannel;
    QList<PluginCommand>       pluginCommands;
};

State& state()
{
    static State s;
    return s;
}

char* copyString(const QByteArray& text)
{
    char* out = static_cast<char*>(std::malloc(static_cast<size_t>(text.size()) + 1));
    std::memcpy(out, text.constData(), static_cast<size_t>(text.size()));
    out[text.size()] = '\0';
    return out;
}

Transfer* findTransfer(anyID id)
{
    for (Transfer& t : state().transfers) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

// ---- the TeamSpeak functions Core and ts3api call ------------------------------------------------

unsigned int getErrorMessage(unsigned int errorCode, char** error)
{
    *error = copyString(QByteArray("fake error ") + QByteArray::number(errorCode, 16));
    return ERROR_ok;
}

unsigned int freeMemory(void* pointer)
{
    std::free(pointer);
    return ERROR_ok;
}

unsigned int logMessage(const char* message, enum LogLevel, const char*, uint64)
{
    state().log.append(QString::fromUtf8(message));
    return ERROR_ok;
}

void getConfigPath(char* path, size_t maxLen)
{
    const QByteArray dir = QDir::toNativeSeparators(state().configDir + QLatin1Char('/')).toUtf8();
    qstrncpy(path, dir.constData(), static_cast<uint>(maxLen));
}

uint64 getCurrentServerConnectionHandlerID()
{
    return kConnection;
}

unsigned int getServerConnectionHandlerList(uint64** result)
{
    auto* list = static_cast<uint64*>(std::malloc(2 * sizeof(uint64)));
    list[0]    = kConnection;
    list[1]    = 0;
    *result    = list;
    return ERROR_ok;
}

unsigned int getConnectionStatus(uint64 sch, int* result)
{
    *result = sch == kConnection && state().connected ? STATUS_CONNECTION_ESTABLISHED : STATUS_DISCONNECTED;
    return ERROR_ok;
}

unsigned int getServerVariableAsString(uint64 sch, size_t flag, char** result)
{
    if (sch != kConnection)
        return ERROR_parameter_invalid;
    if (flag == VIRTUALSERVER_VERSION)
        *result = copyString(state().serverVersion.toUtf8());
    else if (flag == VIRTUALSERVER_PLATFORM)
        *result = copyString(state().serverPlatform.toUtf8());
    else
        *result = copyString(flag == VIRTUALSERVER_UNIQUE_IDENTIFIER ? QByteArray(kServerUid) : QByteArray("Fake server"));
    return ERROR_ok;
}

unsigned int getClientID(uint64, anyID* result)
{
    *result = kOwnClient;
    return ERROR_ok;
}

unsigned int getChannelOfClient(uint64, anyID client, uint64* result)
{
    if (client == kOwnClient) {
        *result = state().ownChannel;
        return ERROR_ok;
    }
    if (!state().clients.isEmpty()) { // 2.4 link previews: the client table
        const auto found = state().clients.constFind(client);
        if (found == state().clients.constEnd())
            return ERROR_client_invalid_id;
        *result = found->channel;
        return ERROR_ok;
    }
    *result = kChannel;
    return ERROR_ok;
}

unsigned int getChannelVariableAsInt(uint64, uint64, size_t, int* result)
{
    *result = 0; // no password
    return ERROR_ok;
}

unsigned int getChannelVariableAsString(uint64, uint64, size_t, char** result)
{
    *result = copyString("Fake channel");
    return ERROR_ok;
}

unsigned int getClientList(uint64, anyID** result)
{
    const QList<anyID> others = state().clients.keys();
    auto*              list   = static_cast<anyID*>(std::malloc((static_cast<size_t>(others.size()) + 2) * sizeof(anyID)));
    int                n      = 0;
    list[n++]                 = kOwnClient;
    for (const anyID id : others)
        list[n++] = id;
    list[n] = 0;
    *result = list;
    return ERROR_ok;
}

unsigned int getClientVariableAsString(uint64, anyID client, size_t flag, char** result)
{
    if (!state().clients.isEmpty() || !state().ownUid.isEmpty()) { // 2.4 link previews: the client table
        if (client == kOwnClient) {
            *result = copyString((flag == CLIENT_UNIQUE_IDENTIFIER ? state().ownUid : QStringLiteral("Tester")).toUtf8());
            return ERROR_ok;
        }
        const auto found = state().clients.constFind(client);
        if (found == state().clients.constEnd())
            return ERROR_client_invalid_id;
        *result = copyString((flag == CLIENT_UNIQUE_IDENTIFIER ? found->uid : found->nickname).toUtf8());
        return ERROR_ok;
    }
    *result = copyString("Tester");
    return ERROR_ok;
}

// 2.4 link previews
unsigned int getClientVariableAsInt(uint64, anyID client, size_t flag, int* result)
{
    const auto found = state().clients.constFind(client);
    if (client != kOwnClient && found == state().clients.constEnd())
        return ERROR_client_invalid_id;
    *result = flag == CLIENT_TYPE && client != kOwnClient && found->serverQuery ? ClientType_SERVERQUERY : 0;
    return ERROR_ok;
}

unsigned int getClientSelfVariableAsString(uint64, size_t flag, char** result)
{
    if (flag != CLIENT_UNIQUE_IDENTIFIER || state().ownUid.isEmpty())
        return ERROR_parameter_invalid;
    *result = copyString(state().ownUid.toUtf8());
    return ERROR_ok;
}

void sendPluginCommand(uint64, const char*, const char* command, int targetMode, const anyID* targetIDs, const char* returnCode)
{
    PluginCommand c;
    c.payload    = QByteArray(command);
    c.targetMode = targetMode;
    for (const anyID* it = targetIDs; it && *it; ++it)
        c.clients.append(*it);
    c.returnCode = QString::fromUtf8(returnCode);
    state().pluginCommands.append(c);
}

unsigned int getServerConnectInfo(uint64, char* host, unsigned short* port, char* password, size_t maxLen)
{
    qstrncpy(host, kHost, static_cast<uint>(maxLen));
    if (password && maxLen)
        password[0] = '\0';
    *port = kPort;
    return ERROR_ok;
}

void createReturnCode(const char*, char* returnCode, size_t maxLen)
{
    const QByteArray rc = "fake_rc_" + QByteArray::number(++state().nextReturnCode);
    qstrncpy(returnCode, rc.constData(), static_cast<uint>(maxLen));
}

unsigned int requestFile(uint64 sch, uint64 channelID, const char*, const char* file, int overwrite, int resume, const char* destinationDirectory, anyID* result,
                         const char* returnCode)
{
    if (sch != kConnection || channelID != kChannel)
        return ERROR_parameter_invalid;
    Transfer t;
    t.id         = ++state().nextTransfer;
    t.remotePath = QString::fromUtf8(file);
    t.localDir   = QDir::fromNativeSeparators(QString::fromUtf8(destinationDirectory));
    t.returnCode = QString::fromUtf8(returnCode);
    t.overwrite  = overwrite != 0;
    t.resume     = resume != 0;
    state().transfers.append(t);
    *result = t.id;
    return ERROR_ok;
}

unsigned int sendFile(uint64 sch, uint64 channelID, const char*, const char* file, int, int, const char* sourceDirectory, anyID* result, const char* returnCode)
{
    if (sch != kConnection || channelID != kChannel)
        return ERROR_parameter_invalid;
    if (state().files.contains(QString::fromUtf8(file)))
        return ERROR_file_already_exists;
    Transfer t;
    t.id         = ++state().nextTransfer;
    t.upload     = true;
    t.remotePath = QString::fromUtf8(file);
    t.localDir   = QDir::fromNativeSeparators(QString::fromUtf8(sourceDirectory));
    t.returnCode = QString::fromUtf8(returnCode);
    state().transfers.append(t);
    *result = t.id;
    return ERROR_ok;
}

unsigned int haltTransfer(uint64, anyID transferID, int, const char*)
{
    if (Transfer* t = findTransfer(transferID))
        t->halted = true;
    return ERROR_ok;
}

unsigned int getTransferFileSize(anyID transferID, uint64* result)
{
    const Transfer* t = findTransfer(transferID);
    if (!t || t->upload || !state().files.contains(t->remotePath))
        return ERROR_file_invalid_transfer_id;
    *result = static_cast<uint64>(state().files.value(t->remotePath).size());
    return ERROR_ok;
}

unsigned int getTransferFileSizeDone(anyID transferID, uint64* result)
{
    *result = 0; // nothing arrives until a test delivers it
    return findTransfer(transferID) ? ERROR_ok : ERROR_file_invalid_transfer_id;
}

unsigned int requestFileInfo(uint64 sch, uint64 channelID, const char*, const char* file, const char* returnCode)
{
    if (sch != kConnection)
        return ERROR_parameter_invalid;
    if (!state().connected)
        return ERROR_not_connected;
    state().infoRequests.append({channelID, QString::fromUtf8(file), QString::fromUtf8(returnCode)});
    return ERROR_ok;
}

unsigned int requestDeleteFile(uint64, uint64, const char*, const char** file, const char*)
{
    for (const char** it = file; it && *it; ++it) {
        const QString path = QString::fromUtf8(*it);
        // 2.3.2: a sender's content check of the file not halted yet (the server would refuse the delete).
        for (const Transfer& t : qAsConst(state().transfers)) {
            if (!t.upload && !t.halted && t.remotePath == path && t.localDir.contains(QLatin1String("/servercheck/")))
                state().deletedWhileChecking.append(path);
        }
        state().files.remove(path);
    }
    return ERROR_ok;
}

unsigned int requestCreateDirectory(uint64, uint64, const char*, const char* directoryPath, const char* returnCode)
{
    state().directories.append({QString::fromUtf8(directoryPath), QString::fromUtf8(returnCode)});
    return ERROR_ok;
}

unsigned int requestSendChannelTextMsg(uint64 sch, const char* message, uint64 targetChannelID, const char* returnCode)
{
    if (sch != kConnection)
        return ERROR_parameter_invalid;
    state().messages.append({QByteArray(message), targetChannelID, QString::fromUtf8(returnCode)});
    return ERROR_ok;
}

unsigned int requestSendPrivateTextMsg(uint64, const char* message, anyID, const char* returnCode)
{
    state().messages.append({QByteArray(message), 0, QString::fromUtf8(returnCode)});
    return ERROR_ok;
}

unsigned int requestSendServerTextMsg(uint64, const char* message, const char* returnCode)
{
    state().messages.append({QByteArray(message), 0, QString::fromUtf8(returnCode)});
    return ERROR_ok;
}

void printMessage(uint64, const char* message, enum PluginMessageTarget target)
{
    state().log.append(QStringLiteral("[chat] ") + QString::fromUtf8(message));
    if (target == PLUGIN_MESSAGE_TARGET_CHANNEL)
        state().channelPrints.append(QString::fromUtf8(message));
}

void printMessageToCurrentTab(const char* message)
{
    state().log.append(QStringLiteral("[chat] ") + QString::fromUtf8(message));
}

} // namespace

void install(const QString& configDir)
{
    state() = State();
    state().configDir = configDir;

    TS3Functions& f                  = ts3::funcs;
    f                                = TS3Functions{};
    f.getErrorMessage                = getErrorMessage;
    f.freeMemory                     = freeMemory;
    f.logMessage                     = logMessage;
    f.getConfigPath                  = getConfigPath;
    f.getCurrentServerConnectionHandlerID = getCurrentServerConnectionHandlerID;
    f.getServerConnectionHandlerList = getServerConnectionHandlerList;
    f.getConnectionStatus            = getConnectionStatus;
    f.getServerVariableAsString      = getServerVariableAsString;
    f.getClientID                    = getClientID;
    f.getChannelOfClient             = getChannelOfClient;
    f.getChannelVariableAsInt        = getChannelVariableAsInt;
    f.getChannelVariableAsString     = getChannelVariableAsString;
    f.getClientList                  = getClientList;
    f.getClientVariableAsString      = getClientVariableAsString;
    f.getClientVariableAsInt         = getClientVariableAsInt;         // 2.4 link previews
    f.getClientSelfVariableAsString  = getClientSelfVariableAsString;  // 2.4 link previews
    f.sendPluginCommand              = sendPluginCommand;              // 2.4 link previews
    f.getServerConnectInfo           = getServerConnectInfo;
    f.createReturnCode               = createReturnCode;
    f.requestFile                    = requestFile;
    f.sendFile                       = sendFile;
    f.haltTransfer                   = haltTransfer;
    f.getTransferFileSize            = getTransferFileSize;
    f.getTransferFileSizeDone        = getTransferFileSizeDone;
    f.requestDeleteFile              = requestDeleteFile;
    f.requestCreateDirectory         = requestCreateDirectory;
    f.requestSendChannelTextMsg      = requestSendChannelTextMsg;
    f.requestSendPrivateTextMsg      = requestSendPrivateTextMsg;
    f.requestSendServerTextMsg       = requestSendServerTextMsg;
    f.printMessage                   = printMessage;
    f.printMessageToCurrentTab       = printMessageToCurrentTab;
    ts3::pluginId                    = QString::fromLatin1("{00000000-0000-0000-0000-000000000000}");
}

void setConnected(bool connected)
{
    state().connected = connected;
}

// 2.4 link previews
void addClient(const Client& client)
{
    state().clients.insert(client.id, client);
}

void removeClient(anyID id)
{
    state().clients.remove(id);
}

void moveClient(anyID id, uint64 channel)
{
    if (id == kOwnClient)
        state().ownChannel = channel;
    else if (state().clients.contains(id))
        state().clients[id].channel = channel;
}

void setOwnUid(const QString& uid)
{
    state().ownUid = uid;
}

void setOwnChannel(uint64 channel)
{
    state().ownChannel = channel;
}

QList<PluginCommand> pluginCommands()
{
    return state().pluginCommands;
}

void enableFileInfo(bool enabled)
{
    ts3::funcs.requestFileInfo = enabled ? requestFileInfo : nullptr;
}

QList<InfoRequest> fileInfoRequests()
{
    return state().infoRequests;
}

void setServerVersion(const QString& version, const QString& platform)
{
    state().serverVersion  = version;
    state().serverPlatform = platform;
}

void setResumeSupported(bool supported)
{
    state().resumeSupported = supported;
}

void setResumeFromStart(bool fromStart)
{
    state().resumeFromStart = fromStart;
}

QStringList deletedWhileChecking()
{
    return state().deletedWhileChecking;
}

QList<Transfer> checkDownloads()
{
    QList<Transfer> out;
    for (const Transfer& t : qAsConst(state().transfers)) {
        if (!t.upload && t.localDir.contains(QLatin1String("/servercheck/")))
            out.append(t);
    }
    return out;
}

void putFile(const QString& remotePath, const QByteArray& data)
{
    state().files.insert(remotePath, data);
}

QByteArray serverFile(const QString& remotePath)
{
    return state().files.value(remotePath);
}

bool hasServerFile(const QString& remotePath)
{
    return state().files.contains(remotePath);
}

QList<Transfer> downloads()
{
    QList<Transfer> out;
    for (const Transfer& t : qAsConst(state().transfers)) {
        if (!t.upload)
            out.append(t);
    }
    return out;
}

QList<Transfer> uploads()
{
    QList<Transfer> out;
    for (const Transfer& t : qAsConst(state().transfers)) {
        if (t.upload)
            out.append(t);
    }
    return out;
}

Transfer transfer(anyID id)
{
    const Transfer* t = findTransfer(id);
    return t ? *t : Transfer();
}

QList<Message> messages()
{
    return state().messages;
}

QList<Directory> directories()
{
    return state().directories;
}

QStringList logLines()
{
    return state().log;
}

QStringList channelPrints()
{
    return state().channelPrints;
}

bool logContains(const QString& text)
{
    for (const QString& line : qAsConst(state().log)) {
        if (line.contains(text))
            return true;
    }
    return false;
}

QString deliver(anyID id, const QByteArray& bytes)
{
    const Transfer* t = findTransfer(id);
    if (!t || t->upload)
        return {};
    QDir().mkpath(t->localDir);
    const QString path = t->localDir + QLatin1Char('/') + QFileInfo(t->remotePath).fileName();
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        return {};
    return path;
}

QString deliverServerFile(anyID id)
{
    const Transfer* t = findTransfer(id);
    if (!t)
        return {};
    const QByteArray bytes = state().files.value(t->remotePath);
    if (!t->resume)
        return deliver(id, bytes);
    // 2.3.2: TeamSpeak's resume: the local file is continued where it ends (the server sends the rest;
    // one that can't seek sends all of it, or the bytes from the start up to the length asked for).
    const QString path = t->localDir + QLatin1Char('/') + QFileInfo(t->remotePath).fileName();
    QDir().mkpath(t->localDir);
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return {};
    const qint64 have  = file.size();
    const int    after = static_cast<int>(qMin<qint64>(have, bytes.size()));
    QByteArray   rest  = bytes.mid(after);
    if (!state().resumeSupported)
        rest = bytes;
    else if (state().resumeFromStart)
        rest = bytes.left(bytes.size() - after);
    if (!file.seek(have) || file.write(rest) != rest.size())
        return {};
    return path;
}

bool completeUpload(anyID id)
{
    const Transfer* t = findTransfer(id);
    if (!t || !t->upload)
        return false;
    QFile file(t->localDir + QLatin1Char('/') + QFileInfo(t->remotePath).fileName());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    state().files.insert(t->remotePath, file.readAll());
    return true;
}

} // namespace fakets3
