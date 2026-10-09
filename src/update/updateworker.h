#pragma once

// The updater's background jobs: fetch and check the manifest, or download, verify and stage an
// update. Each job runs on its own Win32 thread (not a QThread or a Qt pool) that pins the plugin DLL
// with GetModuleHandleEx and leaves through FreeLibraryAndExitThread, so it can finish its last
// network read safely even if TeamSpeak unloads the plugin meanwhile. Results go back with a queued
// call to the receiver, under a mutex the receiver clears when it is destroyed. The thread creates no
// QObject; files are written with Win32 calls.

#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "httpclient.h"
#include "updatefiles.h"
#include "updateinstaller.h"
#include "updatemanifest.h"

namespace upd {

struct WorkerLink {
    QMutex            mutex;
    QPointer<QObject> receiver; // cleared (under mutex) by the receiver's destructor
    std::atomic_bool  cancel{false};
};

struct CheckOutcome {
    http::Error   httpError     = http::Error::None;
    int           httpStatus    = 0;
    qint64        retryAfterSec = 0;
    unsigned long winError      = 0;
    QString       host;
    ManifestError manifestError = ManifestError::None;
    QString       detail;
    Manifest      manifest; // valid only if ok()

    bool ok() const { return httpError == http::Error::None && manifestError == ManifestError::None; }
};

struct PrepareOutcome {
    enum class Failure {
        None,
        DiskSpace,    // neededBytes free bytes needed in updateDir
        Disk,         // can't create or write the files
        Network,      // httpError says why
        Mismatch,     // a download doesn't match the signed size or SHA-256
        WrongMachine, // the file isn't for this CPU, or isn't a DLL / exe as named
        Quarantined,  // a staged file changed or vanished after 1.5 s (antivirus)
        Imports,      // the new DLL imports something this TeamSpeak doesn't have
        Canceled,
    };
    Failure             failure       = Failure::None;
    http::Error         httpError     = http::Error::None;
    qint64              retryAfterSec = 0;
    unsigned long       winError      = 0;
    quint64             neededBytes   = 0;
    QString             fileName;       // the file a failure is about (no paths)
    QStringList         missingImports; // "Qt5Core.dll!symbol"
    QVector<StagedFile> plugins;        // own architecture first
    bool                hasHelper = false;
    StagedFile          helper;

    bool ok() const { return failure == Failure::None; }
};

class UpdateWorker
{
  public:
    // Both return the thread handle (the caller waits on it at shutdown and closes it), or nullptr if
    // no thread could be started. done/progress run on the receiver's thread.
    static void* startCheck(const std::shared_ptr<WorkerLink>& link, const QVector<TrustedKey>& keys, const QSet<int>& revoked,
                            std::function<void(const CheckOutcome&)> done);
    static void* startPrepare(const std::shared_ptr<WorkerLink>& link, const Layout& layout, const Manifest& manifest,
                              std::function<void(qint64 done, qint64 total)> progress, std::function<void(const PrepareOutcome&)> done);

    // Bytes downloaded for an update of this manifest on this architecture (DLL(s) and helper).
    static qint64 downloadSize(const Manifest& manifest, Arch arch);
    // Seconds the worker waits after staging before it re-hashes (antivirus quarantine check).
    static constexpr int kQuarantineWaitMs = 1500;
};

} // namespace upd
