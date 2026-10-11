#pragma once

// Installs a verified update and keeps the plugin recoverable. Win32 file APIs, no TeamSpeak API, so
// the unit tests and the harness run it against a fake plugins folder.
//
// The swap uses the rename trick: a loaded DLL can't be deleted or overwritten, but it can be renamed
// on the same volume. So plugins/tsmedia_win64.dll (loaded) becomes update/rollback/<old>/
// tsmedia_win64.dll.bak and the new file takes its name; TeamSpeak loads it at the next start. Each
// step is journaled in state.ini, every move is retried on sharing errors, and a failed step is undone.
//
// The new version protects itself: bootGuard() runs first in ts3plugin_init and counts starts while
// the update is unconfirmed. If a start never reached onStarted() (a crash in init), the next start
// restores the previous DLLs and returns "rolled back", and ts3plugin_init returns 1 so TeamSpeak
// unloads the plugin and stays usable.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include "updatefiles.h"
#include "updatemanifest.h"

namespace upd {

struct StagedFile {
    FileKind   kind = FileKind::Plugin;
    Arch       arch = Arch::Win64;
    QString    path;   // staging/<ver>/<name>.new
    QByteArray sha256; // as the manifest says
    qint64     size = 0;
};

enum class ApplyError {
    None,
    Locked,        // another TeamSpeak instance is installing
    OtherVolume,   // staging and plugins folder on different drives: no atomic rename
    NotWritable,   // no permission to change the plugins folder
    StagedChanged, // a staged file changed or vanished (antivirus)
    InUse,         // the loaded DLL can't be renamed (FAT32/exFAT, or a lock)
    MoveFailed,
    VerifyFailed,  // the installed file doesn't match afterwards
};

struct ApplyResult {
    ApplyError    error    = ApplyError::None;
    unsigned long winError = 0;
    // Only on failure: true if the plugins folder is byte-identical to before ("Nothing was changed").
    // False means the undo failed too: missingFile is gone from pluginsDir and its copy is copyPath.
    bool    unchanged = true;
    QString missingFile;
    QString copyPath;

    bool ok() const { return error == ApplyError::None; }
};

// plugins: the staged plugin DLLs, the running architecture's first. from = the running version.
ApplyResult applyUpdate(const Layout& layout, const QVector<StagedFile>& plugins, const Version& from, const Version& to);

enum class BootGuard { Continue, RolledBack };
BootGuard bootGuard(const Layout& layout, const Version& current);

// Puts rollback/<from>/*.dll.bak back (only the two plugin names, each checked against the SHA-256
// recorded at apply time and its PE machine). The files they replace go to failed/<to>/.
bool restorePrevious(const Layout& layout, const Version& from, const Version& to, QString* why = nullptr);

struct StartupNotice {
    enum Kind { None, Updated, RolledBack } kind = None;
    Version version;  // the update (to)
    Version restored; // RolledBack: the version that runs again (from)
};

// End of a successful start (plugin.cpp, after Core and Chat started): writes started-<current> with
// pid, settles the install state (applied -> done) and reconciles stale state. Returns the chat line
// to show once; call markNoticeShown() after printing it.
StartupNotice onStarted(const Layout& layout, const Version& current, unsigned long pid);
void          markNoticeShown(const Layout& layout);

// The version that runs after the next restart: "to" while an update is applied but not started.
Version effectiveInstalled(const Layout& layout, const Version& current);
bool    restartPending(const Layout& layout, const Version& current);

// Removes staging/download leftovers (download/ is kept while keepDownload), old markers, failed/,
// the helper once it is done, and rollback generations other than the newest and the referenced one.
void cleanup(const Layout& layout, const Version& current, bool keepDownload);

struct HelperJob {
    unsigned long pid = 0;
    QString       exe;   // GetModuleFileNameW(nullptr)
    QStringList   flags; // relaunchFlags()
    Version       expect;
    Version       from;
    int           waitSec = 60;
    bool          quiet   = false; // automated tests only: the helper shows no message box
};

enum class LaunchError { None, Blocked, HashMismatch, WriteFailed, Failed };
// Copies the staged helper to update/tsmedia_update_helper.exe (checking its hash through a handle
// that stays open, so it can't be swapped before it runs), writes helper-job.ini and starts it.
// Blocked: Windows policy or antivirus refused to run it (errors 5, 225, 226, 1260, 4551).
LaunchError startHelper(const Layout& layout, const QString& stagedHelper, const QByteArray& sha256, const HelperJob& job,
                        unsigned long* winError = nullptr);

} // namespace upd
