#include "updateinstaller.h"

#include <QDateTime>
#include <QMap>

#include <windows.h>

#include "crypto.h"
#include "pecheck.h"

namespace upd {

namespace {

constexpr const char* kInstall = "install";

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

QString hex(const QByteArray& raw)
{
    return QString::fromLatin1(raw.toHex());
}

QString shaKey(const QString& name)
{
    return latin("sha.") + name; // SHA-256 of the rollback copy of name
}

QString newKey(const QString& name)
{
    return latin("new.") + name; // SHA-256 of the installed new file
}

QString join(const QString& dir, const QString& name)
{
    return dir + QLatin1Char('/') + name;
}

Arch archOfPluginName(const QString& name)
{
    return name == targetFileName(FileKind::Plugin, Arch::Win32) ? Arch::Win32 : Arch::Win64;
}

// Own architecture first: it is the one that matters, and the one that must not be left half done.
QStringList pluginNamesOwnFirst(const Layout& layout)
{
    QStringList names = pluginFileNames();
    names.removeAll(layout.ownDllName());
    names.prepend(layout.ownDllName());
    return names;
}

bool machineMatches(const QString& path, Arch arch, bool wantDll)
{
    QByteArray data;
    if (!fs::readAll(path, &data, kMaxFileSize))
        return false;
    pe::ImageInfo info;
    return pe::readInfo(data, &info) && info.machine == machineFor(arch) && info.isDll == wantDll;
}

// Held while files are swapped, so two TeamSpeak instances (-nosingleinstance) never install at once.
class InstallLock
{
  public:
    explicit InstallLock(const QString& path)
    {
        m_handle = CreateFileW(fs::native(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                               nullptr);
    }
    ~InstallLock()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }
    InstallLock(const InstallLock&)            = delete;
    InstallLock& operator=(const InstallLock&) = delete;
    bool ok() const { return m_handle != INVALID_HANDLE_VALUE; }

  private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// Can we create files in dir? (The swap creates a new name there.)
bool canWrite(const QString& dir, unsigned long* error)
{
    const std::wstring probe = fs::native(join(dir, latin("tsmedia_write_probe.tmp")));
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    CloseHandle(file);
    return true;
}

QByteArray stateOf(const QString& path)
{
    if (!fs::exists(path))
        return QByteArrayLiteral("absent");
    const QByteArray sha = fs::sha256(path, nullptr, kMaxFileSize);
    return sha.isEmpty() ? QByteArrayLiteral("unreadable") : sha;
}

} // namespace

// ---- apply ---------------------------------------------------------------------------------------

ApplyResult applyUpdate(const Layout& layout, const QVector<StagedFile>& plugins, const Version& from, const Version& to)
{
    ApplyResult result;
    if (plugins.isEmpty() || plugins.first().arch != layout.arch || plugins.first().kind != FileKind::Plugin || !from.isValid() || !to.isValid()) {
        result.error = ApplyError::MoveFailed;
        return result;
    }
    fs::ensureDir(layout.updateDir);
    InstallLock lock(layout.lockFile());
    if (!lock.ok()) {
        result.error    = ApplyError::Locked;
        result.winError = GetLastError();
        return result;
    }

    // Preflight: nothing has been touched yet.
    if (!fs::sameVolume(layout.stagingDir(to), layout.pluginsDir)) {
        result.error = ApplyError::OtherVolume;
        return result;
    }
    if (!canWrite(layout.pluginsDir, &result.winError)) {
        result.error = ApplyError::NotWritable;
        return result;
    }
    for (const StagedFile& f : plugins) {
        qint64 size = -1;
        if (f.kind != FileKind::Plugin || fs::sha256(f.path, &size, kMaxFileSize) != f.sha256 || size != f.size) {
            result.error = ApplyError::StagedChanged;
            return result;
        }
    }

    StateFile st(layout.stateFile());
    // An update already waits for a restart: the running version's files are in rollback/ already,
    // so the waiting (never loaded) files are replaced and the rollback copy is kept.
    const bool    pending     = st.value(kInstall, "status") == status::applied() && st.versionValue(kInstall, "to") > from;
    const QString rollbackDir = layout.rollbackDir(from);
    const QString replacedDir = layout.updateDir + latin("/replaced/") + to.toString();
    if (!pending)
        fs::removeTree(rollbackDir);
    fs::removeTree(replacedDir);
    if (!fs::ensureDir(rollbackDir) || !fs::ensureDir(replacedDir)) {
        result.error = ApplyError::NotWritable;
        return result;
    }

    QMap<QString, QByteArray> before;
    for (const QString& name : pluginFileNames())
        before.insert(name, stateOf(join(layout.pluginsDir, name)));

    const QString previousStatus = st.value(kInstall, "status");
    st.setValue(kInstall, "status", latin("applying"));
    st.setValue(kInstall, "from", from.toString());
    st.setValue(kInstall, "to", to.toString());
    st.setValue(kInstall, "journal", QString());

    struct Step {
        bool    placed;  // false: moved away
        QString name;
        QString path;    // where it went (moved) or came from (placed)
    };
    QVector<Step> steps;
    QStringList   journal;
    const auto    record = [&](const Step& step) {
        steps.append(step);
        journal.append((step.placed ? latin("placed:") : latin("moved:")) + step.name);
        st.setValue(kInstall, "journal", journal.join(QLatin1Char(';')));
    };

    unsigned long error  = 0;
    ApplyError    failed = ApplyError::None;
    for (const StagedFile& f : plugins) {
        const QString name   = targetFileName(FileKind::Plugin, f.arch);
        const QString target = join(layout.pluginsDir, name);
        if (fs::exists(target)) {
            // The first displaced copy of a name is the rollback; later ones (an update replacing a
            // waiting update) are only kept until this one is in place.
            const bool    toRollback = !fs::exists(join(rollbackDir, name + latin(".bak")));
            const QString displaced  = join(toRollback ? rollbackDir : replacedDir, name + latin(".bak"));
            if (!fs::moveFile(target, displaced, true, &error)) {
                const bool locked = f.arch == layout.arch && (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION);
                failed            = locked ? ApplyError::InUse : ApplyError::MoveFailed;
                break;
            }
            if (toRollback)
                st.setValue(kInstall, shaKey(name), hex(before.value(name)));
            record({false, name, displaced});
        }
        if (!fs::moveFile(f.path, target, false, &error)) {
            failed = ApplyError::MoveFailed;
            break;
        }
        record({true, name, f.path});
    }
    if (failed == ApplyError::None) {
        for (const StagedFile& f : plugins) {
            if (fs::sha256(join(layout.pluginsDir, targetFileName(FileKind::Plugin, f.arch)), nullptr, kMaxFileSize) != f.sha256) {
                failed = ApplyError::VerifyFailed;
                break;
            }
        }
    }

    if (failed != ApplyError::None) {
        // Undo in reverse order: new files back to staging, old files back into place.
        for (int i = steps.size() - 1; i >= 0; --i) {
            const Step&   step   = steps.at(i);
            const QString target = join(layout.pluginsDir, step.name);
            if (step.placed)
                fs::moveFile(target, step.path, true);
            else
                fs::moveFile(step.path, target, true);
        }
        for (const QString& name : pluginFileNames()) {
            const QByteArray now = stateOf(join(layout.pluginsDir, name));
            if (now == before.value(name))
                continue;
            result.unchanged = false;
            if (!fs::exists(join(layout.pluginsDir, name)) && result.missingFile.isEmpty()) {
                result.missingFile = name;
                for (const Step& step : steps) {
                    if (!step.placed && step.name == name)
                        result.copyPath = step.path;
                }
            }
        }
        st.setValue(kInstall, "status", previousStatus.isEmpty() ? status::none() : previousStatus);
        if (!pending) {
            st.remove(kInstall, "from");
            st.remove(kInstall, "to");
        }
        st.remove(kInstall, "journal");
        result.error    = failed;
        result.winError = error;
        return result;
    }

    for (const StagedFile& f : plugins)
        st.setValue(kInstall, newKey(targetFileName(FileKind::Plugin, f.arch)), hex(f.sha256));
    st.setValue(kInstall, "rollback", latin("rollback/") + from.toString());
    st.setTime(kInstall, "appliedUtc", QDateTime::currentDateTimeUtc());
    st.setInt(kInstall, "bootAttempts", 0);
    st.setValue(kInstall, "notice", QString());
    st.remove(kInstall, "journal");
    st.setValue(kInstall, "status", status::applied());
    fs::removeTree(replacedDir);
    return result;
}

// ---- self-rollback -------------------------------------------------------------------------------

bool restorePrevious(const Layout& layout, const Version& from, const Version& to, QString* why)
{
    StateFile     st(layout.stateFile());
    const QString failedDir = layout.failedDir(to);
    bool          ownDone   = false;
    for (const QString& name : pluginNamesOwnFirst(layout)) {
        const bool    own    = name == layout.ownDllName();
        const QString backup = join(layout.rollbackDir(from), name + latin(".bak"));
        const QString target = join(layout.pluginsDir, name);
        const auto    give   = [&](const char* reason) {
            if (why && why->isEmpty())
                *why = name + latin(": ") + latin(reason);
            return !own; // a problem with the other architecture's file doesn't stop the restore
        };
        if (!fs::exists(backup)) {
            if (!give("no rollback copy"))
                return false;
            continue;
        }
        const QString expected = st.value(kInstall, shaKey(name));
        if (expected.isEmpty() || hex(fs::sha256(backup, nullptr, kMaxFileSize)) != expected) {
            if (!give("rollback copy doesn't match its recorded SHA-256"))
                return false;
            continue;
        }
        if (!machineMatches(backup, archOfPluginName(name), true)) {
            if (!give("rollback copy is for another CPU"))
                return false;
            continue;
        }
        fs::ensureDir(failedDir);
        const QString parked    = join(failedDir, name + latin(".bak"));
        bool          movedAway = false;
        if (fs::exists(target)) {
            if (!fs::moveFile(target, parked, true)) { // also works for the loaded DLL (rename)
                if (!give("can't move the new file away"))
                    return false;
                continue;
            }
            movedAway = true;
        }
        if (!fs::moveFile(backup, target, false)) {
            if (movedAway)
                fs::moveFile(parked, target, false);
            if (!give("can't put the rollback copy back"))
                return false;
            continue;
        }
        if (own)
            ownDone = true;
    }
    return ownDone;
}

BootGuard bootGuard(const Layout& layout, const Version& current)
{
    StateFile st(layout.stateFile());
    if (!fs::exists(st.path()))
        return BootGuard::Continue;
    const QString state = st.value(kInstall, "status");
    if ((state != status::applied() && state != latin("applying") && state != status::unverified()) || st.versionValue(kInstall, "to") != current)
        return BootGuard::Continue;

    const int attempts = st.intValue(kInstall, "bootAttempts", 0, 0, 1000) + 1;
    st.setInt(kInstall, "bootAttempts", attempts);
    if (attempts < 2)
        return BootGuard::Continue; // the first start of the new version

    // The previous start of this version never reached onStarted(): restore the old one.
    const Version from = st.versionValue(kInstall, "from");
    QString       why;
    if (from.isValid() && restorePrevious(layout, from, current, &why)) {
        st.setValue(kInstall, "status", status::rolledBack());
        st.setValue(kInstall, "notice", latin("pending"));
        st.setInt(kInstall, "bootAttempts", 0);
        return BootGuard::RolledBack;
    }
    // Don't try again on every start; keep running and let the user decide.
    st.setValue(kInstall, "status", status::rollbackFailed());
    QString ascii;
    for (QChar c : why) {
        if (c.unicode() >= 0x20 && c.unicode() < 0x7f)
            ascii += c;
    }
    st.setValue(kInstall, "rollbackError", ascii.left(200));
    return BootGuard::Continue;
}

// ---- start, notices, cleanup ---------------------------------------------------------------------

StartupNotice onStarted(const Layout& layout, const Version& current, unsigned long pid)
{
    StartupNotice notice;
    fs::ensureDir(layout.updateDir);
    fs::writeAll(layout.markerFile(current), QByteArray::number(static_cast<qulonglong>(pid)));

    StateFile st(layout.stateFile());
    if (!fs::exists(st.path()))
        return notice;
    const QString state  = st.value(kInstall, "status");
    const Version to     = st.versionValue(kInstall, "to");
    const Version from   = st.versionValue(kInstall, "from");
    const bool    unseen = st.value(kInstall, "notice") == latin("pending");

    if ((state == status::applied() || state == latin("applying") || state == status::unverified()) && to == current) {
        st.setValue(kInstall, "status", status::done());
        st.setInt(kInstall, "bootAttempts", 0);
        st.setValue(kInstall, "notice", latin("pending"));
        st.remove(kInstall, "journal");
        notice.kind    = StartupNotice::Updated;
        notice.version = current;
    } else if (state == latin("applying")) {
        // An install was cut off before this version's DLL was replaced.
        st.setValue(kInstall, "status", status::none());
        st.remove(kInstall, "journal");
    } else if (state == status::applied() && to != current && pe::fileVersion(Layout::ownModulePath()) == current.toString()) {
        // Waiting for a version that will never start: this version was installed again by hand.
        st.setValue(kInstall, "status", status::none());
    } else if (state == status::done() && unseen && to == current) {
        notice.kind    = StartupNotice::Updated; // not shown last time (no chat was open)
        notice.version = current;
    } else if (state == status::rolledBack() && unseen && to.isValid()) {
        notice.kind     = StartupNotice::RolledBack;
        notice.version  = to;
        notice.restored = from.isValid() ? from : current;
    }
    return notice;
}

void markNoticeShown(const Layout& layout)
{
    StateFile(layout.stateFile()).setValue(kInstall, "notice", latin("shown"));
}

Version effectiveInstalled(const Layout& layout, const Version& current)
{
    return restartPending(layout, current) ? StateFile(layout.stateFile()).versionValue(kInstall, "to") : current;
}

bool restartPending(const Layout& layout, const Version& current)
{
    const StateFile st(layout.stateFile());
    return st.value(kInstall, "status") == status::applied() && st.versionValue(kInstall, "to") > current;
}

void cleanup(const Layout& layout, const Version& current, bool keepDownload)
{
    if (!fs::isDir(layout.updateDir))
        return;
    const StateFile st(layout.stateFile());
    const QString   state = st.value(kInstall, "status");
    if (state == status::applied() || state == latin("applying"))
        return; // an update waits for a restart: everything may still be needed

    fs::removeTree(layout.updateDir + latin("/staging"));
    fs::removeTree(layout.updateDir + latin("/replaced"));
    fs::removeTree(layout.updateDir + latin("/failed"));
    if (!keepDownload)
        fs::removeTree(layout.downloadDir());
    fs::removeFile(layout.helperExe()); // fails harmlessly while a helper still runs
    fs::removeFile(layout.helperJob());
    for (const QString& name : fs::entries(layout.updateDir, false)) {
        if (name.startsWith(latin("started-")) && name != latin("started-") + current.toString())
            fs::removeFile(join(layout.updateDir, name));
        if (name.endsWith(latin(".tmp")))
            fs::removeFile(join(layout.updateDir, name));
    }

    // Rollback generations: keep the newest and the one state.ini refers to.
    const QString rollbackRoot = layout.updateDir + latin("/rollback");
    const Version referenced   = st.versionValue(kInstall, "from");
    Version       newest;
    for (const QString& name : fs::entries(rollbackRoot, true)) {
        const Version v = Version::parse(name);
        if (v.isValid() && v > newest)
            newest = v;
    }
    for (const QString& name : fs::entries(rollbackRoot, true)) {
        const Version v = Version::parse(name);
        if (!v.isValid() || (v != newest && v != referenced))
            fs::removeTree(join(rollbackRoot, name));
    }
}

// ---- helper --------------------------------------------------------------------------------------

LaunchError startHelper(const Layout& layout, const QString& stagedHelper, const QByteArray& sha256, const HelperJob& job, unsigned long* winError)
{
    if (winError)
        *winError = 0;
    QByteArray image;
    if (!fs::readAll(stagedHelper, &image, kMaxFileSize) || crypto::Sha256::hash(image) != sha256)
        return LaunchError::HashMismatch;
    pe::ImageInfo info;
    if (!pe::readInfo(image, &info) || info.isDll || info.machine != machineFor(layout.arch))
        return LaunchError::HashMismatch;

    const std::wstring exe = fs::native(layout.helperExe());
    HANDLE             out = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        if (winError)
            *winError = GetLastError();
        return LaunchError::WriteFailed;
    }
    DWORD written = 0;
    const bool wroteAll = WriteFile(out, image.constData(), static_cast<DWORD>(image.size()), &written, nullptr) && written == static_cast<DWORD>(image.size());
    FlushFileBuffers(out);
    CloseHandle(out);
    if (!wroteAll)
        return LaunchError::WriteFailed;

    // Re-open without write or delete sharing and check what is on disk through that handle: nobody
    // can swap the file between the check and CreateProcessW (an elevated TeamSpeak runs it).
    HANDLE guard = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (guard == INVALID_HANDLE_VALUE) {
        if (winError)
            *winError = GetLastError();
        return LaunchError::WriteFailed;
    }
    crypto::Sha256 check;
    QByteArray     buffer(64 * 1024, '\0');
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(guard, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0)
            break;
        check.add(buffer.constData(), read);
    }
    if (check.finish() != sha256) {
        CloseHandle(guard);
        return LaunchError::HashMismatch;
    }

    // The job file: UTF-16LE with a BOM, so GetPrivateProfileStringW in the helper reads Unicode paths.
    QString ini = latin("[job]\r\nprotocol=1\r\n");
    ini += latin("pid=") + QString::number(static_cast<qulonglong>(job.pid)) + latin("\r\n");
    ini += latin("exe=") + QString::fromStdWString(fs::native(job.exe)) + latin("\r\n");
    ini += latin("args=") + job.flags.join(QLatin1Char(' ')) + latin("\r\n");
    ini += latin("expect=") + job.expect.toString() + latin("\r\n");
    ini += latin("from=") + job.from.toString() + latin("\r\n");
    ini += latin("dll=") + layout.ownDllName() + latin("\r\n");
    ini += latin("plugins=") + QString::fromStdWString(fs::native(layout.pluginsDir)) + latin("\r\n");
    ini += latin("config=") + QString::fromStdWString(fs::native(layout.configDir)) + latin("\r\n");
    ini += latin("wait=") + QString::number(qBound(5, job.waitSec, 600)) + latin("\r\n");
    QByteArray bytes("\xff\xfe", 2);
    bytes.append(reinterpret_cast<const char*>(ini.utf16()), ini.size() * 2);
    if (!fs::writeAll(layout.helperJob(), bytes)) {
        CloseHandle(guard);
        return LaunchError::WriteFailed;
    }

    std::wstring        commandLine = L"\"" + exe + L"\"";
    const std::wstring  workDir     = fs::native(layout.updateDir);
    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    // Outside TeamSpeak's job object if allowed, so the helper survives TeamSpeak's exit.
    BOOL  started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, workDir.c_str(), &startup, &process);
    DWORD error   = started ? 0 : GetLastError();
    if (!started && error == ERROR_ACCESS_DENIED) {
        started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.c_str(), &startup, &process);
        error   = started ? 0 : GetLastError();
    }
    CloseHandle(guard);
    if (winError)
        *winError = error;
    if (!started) {
        // Access denied, virus detected (225), potentially unwanted (226), blocked by policy (1260),
        // blocked by WDAC / Smart App Control (4551).
        if (error == ERROR_ACCESS_DENIED || error == 225 || error == 226 || error == 1260 || error == 4551)
            return LaunchError::Blocked;
        return LaunchError::Failed;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return LaunchError::None;
}

} // namespace upd
