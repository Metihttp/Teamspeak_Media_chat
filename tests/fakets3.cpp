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
    QStringList                log;
    anyID                      nextTransfer   = 100;
    int                        nextReturnCode = 0;
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
    *result = copyString(flag == VIRTUALSERVER_UNIQUE_IDENTIFIER ? QByteArray(kServerUid) : QByteArray("Fake server"));
    return ERROR_ok;
}

unsigned int getClientID(uint64, anyID* result)
{
    *result = kOwnClient;
    return ERROR_ok;
}

unsigned int getChannelOfClient(uint64, anyID, uint64* result)
{
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
    auto* list = static_cast<anyID*>(std::malloc(2 * sizeof(anyID)));
    list[0]    = kOwnClient;
    list[1]    = 0;
    *result    = list;
    return ERROR_ok;
}

unsigned int getClientVariableAsString(uint64, anyID, size_t, char** result)
{
    *result = copyString("Tester");
    return ERROR_ok;
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

unsigned int requestFile(uint64 sch, uint64 channelID, const char*, const char* file, int, int, const char* destinationDirectory, anyID* result, const char* returnCode)
{
    if (sch != kConnection || channelID != kChannel)
        return ERROR_parameter_invalid;
    Transfer t;
    t.id         = ++state().nextTransfer;
    t.remotePath = QString::fromUtf8(file);
    t.localDir   = QDir::fromNativeSeparators(QString::fromUtf8(destinationDirectory));
    t.returnCode = QString::fromUtf8(returnCode);
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

unsigned int requestDeleteFile(uint64, uint64, const char*, const char** file, const char*)
{
    for (const char** it = file; it && *it; ++it)
        state().files.remove(QString::fromUtf8(*it));
    return ERROR_ok;
}

unsigned int requestCreateDirectory(uint64, uint64, const char*, const char*, const char*)
{
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

void printMessage(uint64, const char* message, enum PluginMessageTarget)
{
    state().log.append(QStringLiteral("[chat] ") + QString::fromUtf8(message));
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

QStringList logLines()
{
    return state().log;
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
    return t ? deliver(id, state().files.value(t->remotePath)) : QString();
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
