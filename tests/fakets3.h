#pragma once

// A fake TeamSpeak client for tests that run Core itself (2.2 sha, tst_coresha.cpp). It fills
// ts3::funcs: one connection (kConnection) to one server (kServerUid), the own client in channel
// kChannel, and a file store standing in for the server's files. Transfers never finish by themselves:
// a test writes what "arrives" (deliver / completeUpload) and then tells Core as TeamSpeak would
// (Core::onTransferStatus). Everything is recorded for assertions. Not thread-safe: Core calls it on
// the GUI thread only.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "teamspeak/public_definitions.h"

namespace fakets3 {

constexpr uint64 kConnection = 1;
constexpr uint64 kChannel    = 5;
constexpr anyID  kOwnClient  = 7;
constexpr char   kServerUid[] = "fakeServerUid+/0=";
constexpr char   kHost[]      = "127.0.0.1";
constexpr quint16 kPort       = 9987;

struct Transfer {
    anyID   id = 0;
    bool    upload = false;
    QString remotePath; // "/tsmedia/a.png"
    QString localDir;   // where a download goes, or where an upload is read from
    QString returnCode;
    bool    halted = false;
    bool    overwrite = false; // 2.3.2: requestFile's flags
    bool    resume    = false;
};

struct Message {
    QByteArray text;
    uint64     channel = 0;
    QString    returnCode;
    int        mode   = 0; // 2.5 GIFs: TextMessageTarget_CHANNEL / CLIENT / SERVER (which requestSend*TextMsg)
    anyID      client = 0; // TextMessageTarget_CLIENT: to whom
};

struct Directory {
    QString path; // "/tsmedia"
    QString returnCode;
};

// 2.3.2: a requestFileInfo (Core asks for a file's size). Nobody answers: the test does, as TeamSpeak
// would (Core::onFileInfo with the size, Core::onServerError with the return code).
struct InfoRequest {
    uint64  channel = 0;
    QString path;
    QString returnCode;
};

// Fills ts3::funcs and forgets everything recorded before. configDir: what getConfigPath returns
// (ts3::dataDir() is <configDir>/plugins/tsmedia).
void install(const QString& configDir);

void setConnected(bool connected);

// The server's files.
void       putFile(const QString& remotePath, const QByteArray& data);
QByteArray serverFile(const QString& remotePath);
bool       hasServerFile(const QString& remotePath);

QList<Transfer> downloads(); // every requestFile, in order
QList<Transfer> uploads();   // every sendFile, in order
Transfer        transfer(anyID id);
QList<Message>  messages();  // chat messages sent
QList<Directory> directories(); // every requestCreateDirectory, in order (nobody answers: the test does)
// 2.3.2: requestFileInfo is there only after enableFileInfo(true) (install() leaves it out: Core then
// posts a message as soon as its upload is reported complete, as before 2.3.2).
void               enableFileInfo(bool enabled);
QList<InfoRequest> fileInfoRequests(); // every requestFileInfo, in order
// 2.3.2: what getServerVariableAsString says for VIRTUALSERVER_VERSION / VIRTUALSERVER_PLATFORM
// (install(): "3.13.7 [Build: 1655727713]" on "Linux").
void setServerVersion(const QString& version, const QString& platform);
// 2.3.2: a server that can't seek: a resumed download gets the whole file again (appended to what the
// local file has), as if the resume were ignored. install(): it can.
void setResumeSupported(bool supported);
// 2.3.2: a server that ignores a resumed download's offset and sends the file from its start; TeamSpeak
// stops once it has as many bytes as it asked for (the local file gets the file's size, but its end is
// the file's beginning). install(): it doesn't.
void setResumeFromStart(bool fromStart);
// 2.3.2: files deleted (requestDeleteFile) while a content check's download of them wasn't halted.
QStringList deletedWhileChecking();
QStringList     logLines();  // what Core wrote to TeamSpeak's log
bool            logContains(const QString& text);
// 2.3.4: the lines printed with printMessage(..., PLUGIN_MESSAGE_TARGET_CHANNEL) (not into the current tab).
QStringList     channelPrints();

// 2.4 link previews: other clients on the server. Without any (install()), every client is in kChannel and
// every uid reads "Tester", as before. With some: getChannelOfClient, getClientList and the client
// variables answer from this table (the own client too: setOwnUid), unknown clients are not visible.
struct Client {
    anyID   id = 0;
    QString uid;
    uint64  channel = kChannel;
    QString nickname;
    bool    serverQuery = false;
};
void addClient(const Client& client);
void removeClient(anyID id);
void moveClient(anyID id, uint64 channel);
// The own identity (getClientSelfVariableAsString CLIENT_UNIQUE_IDENTIFIER; empty until set) and channel.
void setOwnUid(const QString& uid);
void setOwnChannel(uint64 channel);
// 2.5 GIFs: whether the own channel has a password (getChannelVariableAsInt CHANNEL_FLAG_PASSWORD; install(): no).
void setChannelPassword(bool password);
// 2.4 link previews: plugin commands sent (sendPluginCommand), in order. Nobody answers: the test does.
struct PluginCommand {
    QByteArray   payload;
    int          targetMode = 0;
    QList<anyID> clients;
    QString      returnCode;
};
QList<PluginCommand> pluginCommands();

// Writes bytes into a download's destination as TeamSpeak would (<localDir>/<file name>). The test
// then calls core.onTransferStatus(id, ERROR_file_transfer_complete, ...).
QString deliver(anyID id, const QByteArray& bytes);
// The server's bytes for that download. 2.3.2: a download with resume continues the local file where it
// ends, as TeamSpeak does (the server sends the bytes from there on; see setResumeSupported).
QString deliverServerFile(anyID id);
// 2.3.2: the sender's content checks among the downloads (into <data dir>/servercheck), in order.
QList<Transfer> checkDownloads();
// Reads an upload's local file into the server's files. The test then reports completion to Core.
bool completeUpload(anyID id);

} // namespace fakets3
