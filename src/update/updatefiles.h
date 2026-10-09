#pragma once

// Folders, files and state.ini of the updater. Win32 file APIs only (wide paths, no QObject), so the
// worker thread can use them too; no TeamSpeak API, so the unit tests and the harness can.
//
// <data>/update/             data = <TeamSpeak config>/plugins/tsmedia
//   state.ini                machine state (not settings): [check], [install], [trust]
//   download/                *.part files while downloading
//   staging/<ver>/           verified files waiting to be installed: <name>.new
//   rollback/<ver>/          the previous version's DLLs: <name>.bak
//   failed/<ver>/            a version that rolled itself back
//   started-<ver>            written by every start of <ver> (content: its process id)
//   tsmedia_update_helper.exe, helper-job.ini, helper.log, lock

#include <QByteArray>
#include <QDateTime>
#include <QSet>
#include <QString>
#include <QStringList>

#include "updatemanifest.h"

namespace upd {

struct Layout {
    QString pluginsDir; // the folder of the running plugin DLL (authoritative, also for portable installs)
    QString updateDir;  // <data>/update
    QString configDir;  // TeamSpeak's config folder (crashdumps/, logs/), for the helper
    Arch    arch = runningArch();

    QString ownDllName() const { return targetFileName(FileKind::Plugin, arch); }
    QString ownDllPath() const { return pluginsDir + QLatin1Char('/') + ownDllName(); }
    QString stateFile() const { return updateDir + QLatin1String("/state.ini"); }
    QString downloadDir() const { return updateDir + QLatin1String("/download"); }
    QString stagingDir(const Version& v) const { return updateDir + QLatin1String("/staging/") + v.toString(); }
    QString rollbackDir(const Version& v) const { return updateDir + QLatin1String("/rollback/") + v.toString(); }
    QString failedDir(const Version& v) const { return updateDir + QLatin1String("/failed/") + v.toString(); }
    QString markerFile(const Version& v) const { return updateDir + QLatin1String("/started-") + v.toString(); }
    QString helperExe() const { return updateDir + QLatin1String("/tsmedia_update_helper.exe"); }
    QString helperJob() const { return updateDir + QLatin1String("/helper-job.ini"); }
    QString helperLog() const { return updateDir + QLatin1String("/helper.log"); }
    QString lockFile() const { return updateDir + QLatin1String("/lock"); }

    // pluginsDir from GetModuleFileNameW of the module containing this code; updateDir/configDir from
    // the plugin data folder. (GetModuleFileNameW keeps the original path after the DLL was renamed.)
    static Layout forRunningPlugin(const QString& dataDir);
    static QString ownModulePath();
};

// Both architectures' plugin DLLs: the only names the installer, the self-rollback and the helper
// ever move into the plugins folder.
QStringList pluginFileNames();

namespace fs {

std::wstring native(const QString& path); // backslashes; \\?\ prefix for long absolute paths

bool    exists(const QString& path);
bool    isDir(const QString& path);
qint64  fileSize(const QString& path); // -1 if missing
bool    ensureDir(const QString& path);
bool    removeFile(const QString& path);
bool    removeTree(const QString& path); // best effort; true if nothing is left
QStringList entries(const QString& dir, bool dirs); // names of files (or folders) in dir

// MoveFileExW with MOVEFILE_WRITE_THROUGH (a rename on the same volume, which also works for the
// loaded plugin DLL). Retries 5 x 200 ms on sharing and access errors (antivirus scans). *error is
// the last GetLastError().
bool moveFile(const QString& from, const QString& to, bool replace, unsigned long* error = nullptr);

// SHA-256 of a file, read with Win32 (empty on error). maxBytes guards against huge files.
QByteArray sha256(const QString& path, qint64* size = nullptr, qint64 maxBytes = 64 * 1024 * 1024);
bool       readAll(const QString& path, QByteArray* data, qint64 maxBytes);
bool       writeAll(const QString& path, const QByteArray& data); // via <path>.tmp + rename

bool    sameVolume(const QString& a, const QString& b);
quint64 freeBytes(const QString& dir); // 0 if unknown

} // namespace fs

// state.ini through GetPrivateProfileStringW / WritePrivateProfileStringW: no Qt caching (the helper
// writes it too) and nothing of ours stays in Qt state. Values are ASCII only.
class StateFile
{
  public:
    explicit StateFile(const QString& path)
        : m_path(path)
    {
    }

    QString   value(const char* section, const QString& key) const;
    QString   value(const char* section, const char* key) const { return value(section, QString::fromLatin1(key)); }
    int       intValue(const char* section, const char* key, int fallback, int min, int max) const;
    QDateTime timeValue(const char* section, const char* key) const; // UTC, invalid if missing
    Version   versionValue(const char* section, const char* key) const;

    bool setValue(const char* section, const QString& key, const QString& value);
    bool setValue(const char* section, const char* key, const QString& value) { return setValue(section, QString::fromLatin1(key), value); }
    bool setInt(const char* section, const char* key, int value);
    bool setTime(const char* section, const char* key, const QDateTime& utc);
    bool remove(const char* section, const char* key);
    bool clearSection(const char* section);

    QSet<int> revokedKeys() const;
    bool      addRevokedKeys(const QSet<int>& ids);

    const QString& path() const { return m_path; }

  private:
    QString m_path;
};

// [install] status values
namespace status {
inline QString none() { return QString::fromLatin1("none"); }
inline QString applied() { return QString::fromLatin1("applied"); }       // installed, not started yet
inline QString done() { return QString::fromLatin1("done"); }             // the new version started
inline QString rolledBack() { return QString::fromLatin1("rolledBack"); } // the old version is back
inline QString unverified() { return QString::fromLatin1("unverified"); } // the helper couldn't tell
inline QString rollbackFailed() { return QString::fromLatin1("rollbackFailed"); }
} // namespace status

} // namespace upd
