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
};

struct Message {
    QByteArray text;
    uint64     channel = 0;
    QString    returnCode;
};

struct Directory {
    QString path; // "/tsmedia"
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
QStringList     logLines();  // what Core wrote to TeamSpeak's log
bool            logContains(const QString& text);

// Writes bytes into a download's destination as TeamSpeak would (<localDir>/<file name>). The test
// then calls core.onTransferStatus(id, ERROR_file_transfer_complete, ...).
QString deliver(anyID id, const QByteArray& bytes);
// The server's bytes for that download.
QString deliverServerFile(anyID id);
// Reads an upload's local file into the server's files. The test then reports completion to Core.
bool completeUpload(anyID id);

} // namespace fakets3
