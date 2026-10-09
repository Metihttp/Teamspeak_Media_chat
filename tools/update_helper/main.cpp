// tsmedia_update_helper.exe: restarts TeamSpeak after TS Media chat installed an update, and puts the
// previous version back if the new one demonstrably fails to start. Win32 only (no Qt), static CRT,
// published as a signed release asset and checked by the plugin before it runs (docs/UPDATES.md).
//
// The plugin writes helper-job.ini next to this exe (in <config>/plugins/tsmedia/update/) and starts it
// without arguments. The helper:
//   1. waits for the TeamSpeak process in the job to exit (same PID and image path);
//   2. starts the same TeamSpeak exe again, with only the data-free flags from the job;
//   3. watches until the new version's plugin writes started-<version> with the new process id.
// It rolls back only on positive evidence: the new TeamSpeak crashed (exit code >= 0xC0000000), a new
// crash dump appeared, TeamSpeak's log says our DLL failed to load or initialise, or the DLL was seen
// loaded and then unloaded without the marker. A clean exit, a slow start or no answer within 10
// minutes is not evidence: the plugin's own boot counter settles those. Only the two plugin file names
// are ever restored, each checked against the SHA-256 recorded at install time. The command line of
// TeamSpeak is never logged.

#include <windows.h>

#include <bcrypt.h>
#include <tlhelp32.h>

#include <cwchar>
#include <string>
#include <vector>

namespace {

std::wstring g_updateDir; // the folder this exe runs from
std::wstring g_log;

std::wstring dirOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring lower(std::wstring text)
{
    for (wchar_t& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// helper.log, appended and capped at 64 KB (the newest 32 KB are kept). UTF-8.
void log(const std::wstring& line)
{
    if (g_log.empty())
        return;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(g_log.c_str(), GetFileExInfoStandard, &data) && data.nFileSizeLow > 64 * 1024) {
        HANDLE in = CreateFileW(g_log.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        std::vector<char> tail(32 * 1024);
        DWORD             read = 0;
        if (in != INVALID_HANDLE_VALUE) {
            SetFilePointer(in, -static_cast<LONG>(tail.size()), nullptr, FILE_END);
            ReadFile(in, tail.data(), static_cast<DWORD>(tail.size()), &read, nullptr);
            CloseHandle(in);
        }
        HANDLE out = CreateFileW(g_log.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(out, tail.data(), read, &written, nullptr);
            CloseHandle(out);
        }
    }
    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t stamp[64] = {};
    swprintf(stamp, 64, L"%04u-%02u-%02uT%02u:%02u:%02uZ ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring text  = stamp + line + L"\r\n";
    const int          bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string        utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
    HANDLE file = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    }
}

std::wstring iniValue(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    std::vector<wchar_t> buffer(4096);
    const DWORD          length = GetPrivateProfileStringW(section, key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    return std::wstring(buffer.data(), length);
}

std::wstring statePath()
{
    return g_updateDir + L"\\state.ini";
}

void setState(const wchar_t* key, const std::wstring& value)
{
    WritePrivateProfileStringW(L"install", key, value.c_str(), statePath().c_str());
}

bool isVersion(const std::wstring& text)
{
    if (text.empty() || text.size() > 11)
        return false;
    int dots = 0;
    for (wchar_t c : text) {
        if (c == L'.')
            ++dots;
        else if (c < L'0' || c > L'9')
            return false;
    }
    return dots == 2;
}

std::wstring hexSha256(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring       result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<unsigned char> buffer(64 * 1024);
            bool                       ok = true;
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                    ok = false;
                    break;
                }
                if (read == 0)
                    break;
                if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            unsigned char digest[32] = {};
            if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
                static const wchar_t* digits = L"0123456789abcdef";
                for (unsigned char b : digest) {
                    result += digits[b >> 4];
                    result += digits[b & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(file);
    return result;
}

// The PE machine of a file (0 if it isn't one).
WORD peMachine(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    unsigned char header[4096] = {};
    DWORD         read         = 0;
    ReadFile(file, header, sizeof(header), &read, nullptr);
    CloseHandle(file);
    if (read < 0x40 || header[0] != 'M' || header[1] != 'Z')
        return 0;
    const DWORD pe = header[0x3c] | (header[0x3d] << 8) | (header[0x3e] << 16) | (static_cast<DWORD>(header[0x3f]) << 24);
    if (pe + 6 > read || header[pe] != 'P' || header[pe + 1] != 'E' || header[pe + 2] != 0 || header[pe + 3] != 0)
        return 0;
    return static_cast<WORD>(header[pe + 4] | (header[pe + 5] << 8));
}

std::wstring imagePath(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD                size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
        return {};
    return std::wstring(buffer.data(), size);
}

struct Job {
    DWORD        pid = 0;
    std::wstring exe;
    std::wstring args;
    std::wstring expect;
    std::wstring from;
    std::wstring dll;
    std::wstring plugins;
    std::wstring config;
    int          waitSec = 60;
    bool         quiet   = false; // no message box (automated tests)
};

bool readJob(Job* job)
{
    const std::wstring file = g_updateDir + L"\\helper-job.ini";
    if (iniValue(file, L"job", L"protocol") != L"1")
        return false;
    job->pid     = static_cast<DWORD>(wcstoul(iniValue(file, L"job", L"pid").c_str(), nullptr, 10));
    job->exe     = iniValue(file, L"job", L"exe");
    job->expect  = iniValue(file, L"job", L"expect");
    job->from    = iniValue(file, L"job", L"from");
    job->dll     = lower(iniValue(file, L"job", L"dll"));
    job->plugins = iniValue(file, L"job", L"plugins");
    job->config  = iniValue(file, L"job", L"config");
    job->waitSec = _wtoi(iniValue(file, L"job", L"wait").c_str());
    job->quiet   = iniValue(file, L"job", L"quiet") == L"1";
    if (job->waitSec < 5 || job->waitSec > 600)
        job->waitSec = 60;

    // Only the allowlisted, data-free flags, whatever the file says.
    const wchar_t* allowed[] = {L"-nosingleinstance", L"-silentstart", L"-nohotkeys", L"-console"};
    std::wstring   args      = iniValue(file, L"job", L"args");
    size_t         pos       = 0;
    while (pos < args.size()) {
        const size_t end   = args.find(L' ', pos);
        std::wstring token = lower(args.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos));
        for (const wchar_t* flag : allowed) {
            if (token == flag && job->args.find(token) == std::wstring::npos)
                job->args += L" " + token;
        }
        if (end == std::wstring::npos)
            break;
        pos = end + 1;
    }

    const bool dllOk = job->dll == L"tsmedia_win64.dll" || job->dll == L"tsmedia_win32.dll";
    const bool exeOk = job->exe.size() > 8 && lower(job->exe.substr(job->exe.size() - 4)) == L".exe" && job->exe[1] == L':';
    return job->pid != 0 && exeOk && dllOk && isVersion(job->expect) && isVersion(job->from) && !job->plugins.empty() && !job->config.empty();
}

std::vector<std::wstring> listFiles(const std::wstring& dir)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW          data{};
    HANDLE                    find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return names;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return names;
}

bool contains(const std::vector<std::wstring>& list, const std::wstring& name)
{
    for (const std::wstring& n : list) {
        if (n == name)
            return true;
    }
    return false;
}

// TeamSpeak's newest log file created after `since`, scanned for its plugin-failure lines naming our DLL.
bool logShowsFailure(const Job& job, const FILETIME& since)
{
    const std::wstring dir = job.config + L"\\logs";
    WIN32_FIND_DATAW   data{};
    HANDLE             find = FindFirstFileW((dir + L"\\ts3client_*.log").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    std::wstring newest;
    FILETIME     newestTime{};
    do {
        if (CompareFileTime(&data.ftCreationTime, &since) >= 0 && CompareFileTime(&data.ftCreationTime, &newestTime) > 0) {
            newest     = data.cFileName;
            newestTime = data.ftCreationTime;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (newest.empty())
        return false;

    HANDLE file = CreateFileW((dir + L"\\" + newest).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::string text(512 * 1024, '\0');
    DWORD       read = 0;
    ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    text.resize(read);

    std::string dll;
    for (wchar_t c : job.dll)
        dll += static_cast<char>(c);
    const char* markers[] = {"Failed to load plugin", "Plugin failed to load", "Plugin reported initialization failure", "Failed to initialize plugin"};
    size_t      start     = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        for (char& c : line)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (line.find(dll) != std::string::npos) {
            for (const char* marker : markers) {
                std::string m = marker;
                for (char& c : m)
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (line.find(m) != std::string::npos)
                    return true;
            }
        }
        start = end + 1;
    }
    return false;
}

// Is our DLL loaded in the process? -1 if it can't be told (snapshot failed).
int moduleLoaded(DWORD pid, const std::wstring& dll)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int found    = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (lower(entry.szModule) == dll) {
                found = 1;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    } else {
        found = -1;
    }
    CloseHandle(snapshot);
    return found;
}

bool markerFrom(const Job& job, DWORD childPid)
{
    const std::wstring marker = g_updateDir + L"\\started-" + job.expect;
    HANDLE             file   = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char  text[32] = {};
    DWORD read     = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    return strtoul(text, nullptr, 10) == childPid;
}

bool g_quiet = false;

void showMessage(const std::wstring& text)
{
    // No MB_SETFOREGROUND: it must not steal the focus from a full-screen game.
    if (!g_quiet)
        MessageBoxW(nullptr, text.c_str(), L"TS Media chat", MB_OK | MB_ICONWARNING);
}

// Puts rollback/<from>/<name>.bak back for both plugin names, own architecture first.
bool rollBack(const Job& job)
{
    const std::wstring state = iniValue(statePath(), L"install", L"status");
    if (state == L"rolledBack" || state == L"done" || state == L"none") {
        log(L"rollback skipped: state is " + state);
        return false;
    }
    std::vector<std::wstring> names = {job.dll, job.dll == L"tsmedia_win64.dll" ? L"tsmedia_win32.dll" : L"tsmedia_win64.dll"};
    bool                      ownDone = false;
    const std::wstring        failed  = g_updateDir + L"\\failed\\" + job.expect;
    CreateDirectoryW((g_updateDir + L"\\failed").c_str(), nullptr);
    CreateDirectoryW(failed.c_str(), nullptr);
    for (const std::wstring& name : names) {
        const bool         own      = name == job.dll;
        const std::wstring backup   = g_updateDir + L"\\rollback\\" + job.from + L"\\" + name + L".bak";
        const std::wstring target   = job.plugins + L"\\" + name;
        const std::wstring expected = lower(iniValue(statePath(), L"install", (L"sha." + name).c_str()));
        const WORD         machine  = name == L"tsmedia_win64.dll" ? 0x8664 : 0x014c;
        if (!exists(backup) || expected.size() != 64 || hexSha256(backup) != expected || peMachine(backup) != machine) {
            log(L"rollback: no valid copy of " + name);
            if (own)
                return false;
            continue;
        }
        const std::wstring parked    = failed + L"\\" + name + L".bak";
        bool               movedAway = false;
        if (exists(target)) {
            if (!MoveFileExW(target.c_str(), parked.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                log(L"rollback: can't move the new " + name + L" away (error " + std::to_wstring(GetLastError()) + L")");
                if (own)
                    return false;
                continue;
            }
            movedAway = true;
        }
        if (!MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            log(L"rollback: can't restore " + name + L" (error " + std::to_wstring(GetLastError()) + L")");
            if (movedAway)
                MoveFileExW(parked.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            if (own)
                return false;
            continue;
        }
        if (own)
            ownDone = true;
    }
    if (ownDone) {
        setState(L"status", L"rolledBack");
        setState(L"notice", L"pending");
        setState(L"bootAttempts", L"0");
        log(L"rolled back to " + job.from);
    }
    return ownDone;
}

int run()
{
    std::vector<wchar_t> self(32768);
    const DWORD          length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size())
        return 1;
    g_updateDir = dirOf(std::wstring(self.data(), length));
    g_log       = g_updateDir + L"\\helper.log";

    Job job;
    if (!readJob(&job)) {
        log(L"invalid or missing helper-job.ini");
        return 2;
    }
    g_quiet = job.quiet;
    log(L"waiting for TeamSpeak to quit (update " + job.from + L" -> " + job.expect + L")");

    // 1. TeamSpeak quits. A PID that now belongs to another program means it has quit already.
    if (HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, job.pid)) {
        if (lower(imagePath(process)) == lower(job.exe)) {
            if (WaitForSingleObject(process, static_cast<DWORD>(job.waitSec) * 1000) == WAIT_TIMEOUT) {
                log(L"TeamSpeak didn't quit; nothing to do (the update starts with the next TeamSpeak start)");
                CloseHandle(process);
                return 0;
            }
        }
        CloseHandle(process);
    }

    // 2. Start it again: same exe, allowlisted flags only, never the old command line.
    const std::wstring marker = g_updateDir + L"\\started-" + job.expect;
    DeleteFileW(marker.c_str());
    const std::wstring        dumps       = job.config + L"\\crashdumps";
    std::vector<std::wstring> dumpsBefore = listFiles(dumps);
    FILETIME                  startTime{};
    GetSystemTimeAsFileTime(&startTime);
    ULARGE_INTEGER since{};
    since.LowPart  = startTime.dwLowDateTime;
    since.HighPart = startTime.dwHighDateTime;
    since.QuadPart -= 20000000ull; // 2 s of clock slack
    startTime.dwLowDateTime  = since.LowPart;
    startTime.dwHighDateTime = since.HighPart;

    std::wstring        commandLine = L"\"" + job.exe + L"\"" + job.args;
    const std::wstring  workDir     = dirOf(job.exe);
    STARTUPINFOW        startup{};
    PROCESS_INFORMATION child{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(job.exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &startup, &child)) {
        log(L"couldn't start TeamSpeak (error " + std::to_wstring(GetLastError()) + L")");
        showMessage(L"TeamSpeak couldn't be restarted automatically.\n\nThe update to TS Media chat " + job.expect +
                    L" is installed. Start TeamSpeak again to finish.");
        return 3;
    }
    CloseHandle(child.hThread);
    log(L"TeamSpeak started again (process " + std::to_wstring(child.dwProcessId) + L")");

    // 3. Watch for up to 10 minutes.
    bool      seenLoaded = false;
    bool      evidence   = false;
    ULONGLONG started    = GetTickCount64();
    ULONGLONG lastLog    = 0;
    ULONGLONG lastModule = 0;
    while (GetTickCount64() - started < 10ull * 60 * 1000) {
        if (markerFrom(job, child.dwProcessId)) {
            log(L"TS Media chat " + job.expect + L" started");
            CloseHandle(child.hProcess);
            return 0;
        }
        if (WaitForSingleObject(child.hProcess, 500) == WAIT_OBJECT_0) {
            if (markerFrom(job, child.dwProcessId)) {
                log(L"TS Media chat " + job.expect + L" started (TeamSpeak has closed again)");
                CloseHandle(child.hProcess);
                return 0;
            }
            DWORD code = 0;
            GetExitCodeProcess(child.hProcess, &code);
            if (code >= 0xC0000000u) {
                log(L"TeamSpeak crashed before the plugin started (exit code " + std::to_wstring(code) + L")");
                evidence = true;
            } else {
                log(L"TeamSpeak closed before the plugin started (exit code " + std::to_wstring(code) + L"); not a failure");
            }
            break;
        }
        for (const std::wstring& name : listFiles(dumps)) {
            if (!contains(dumpsBefore, name)) {
                log(L"a new crash dump appeared");
                evidence = true;
                WaitForSingleObject(child.hProcess, 30000); // let TeamSpeak finish crashing
                break;
            }
        }
        if (evidence)
            break;
        const ULONGLONG now = GetTickCount64();
        if (now - lastLog >= 2000) {
            lastLog = now;
            if (logShowsFailure(job, startTime)) {
                log(L"TeamSpeak's log says the plugin failed to load or initialise");
                evidence = true;
                break;
            }
        }
        if (now - lastModule >= 1000) {
            lastModule      = now;
            const int state = moduleLoaded(child.dwProcessId, job.dll);
            if (state == 1) {
                seenLoaded = true;
            } else if (state == 0 && seenLoaded && !markerFrom(job, child.dwProcessId)) {
                log(L"the plugin was unloaded before it started");
                evidence = true;
                break;
            }
        }
    }
    CloseHandle(child.hProcess);

    if (!evidence) {
        if (iniValue(statePath(), L"install", L"status") == L"applied")
            setState(L"status", L"unverified");
        log(L"no answer from the new version; leaving it to the plugin's own start check");
        return 0;
    }
    if (rollBack(job)) {
        showMessage(L"TS Media chat " + job.expect + L" didn't start, so version " + job.from +
                    L" was restored.\n\nRestart TeamSpeak to use it. If this keeps happening, please report it at "
                    L"github.com/Metihttp/Teamspeak_Media_chat/issues.");
        return 4;
    }
    log(L"rollback not possible");
    return 5;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Never load a DLL from the (user-writable) folder this exe runs in.
    using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
        const auto fn = reinterpret_cast<SetDefaultDllDirectoriesFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetDefaultDllDirectories")));
        if (fn)
            fn(LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return run();
}
