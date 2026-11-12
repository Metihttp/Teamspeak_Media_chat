#include "ytplayer/ytbackend.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include "pluginlog.h"
#include "ts3api.h"
#include "tsmp.h"
#include "ytplayer/ytpage.h"

namespace ytbackend {

namespace {

// ---- the WebView2 Runtime ----

constexpr const wchar_t* kClientKey = L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

QString readPv(Hive hive)
{
    wchar_t  buffer[128] = {};
    DWORD    bytes       = sizeof(buffer);
    HKEY     root        = hive == Hive::LocalMachine32 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    DWORD    flags       = RRF_RT_REG_SZ | (hive == Hive::LocalMachine32 ? RRF_SUBKEY_WOW6432KEY : 0);
    if (RegGetValueW(root, kClientKey, L"pv", flags, nullptr, buffer, &bytes) != ERROR_SUCCESS)
        return {};
    buffer[(sizeof(buffer) / sizeof(wchar_t)) - 1] = 0;
    return QString::fromWCharArray(buffer).trimmed();
}

RegistryReader& testReader()
{
    static RegistryReader reader;
    return reader;
}

int  g_reads   = 0;
int  g_cached  = -1; // -1: not read; 0 / 1
bool g_refresh = false;

QString hexOf(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

std::wstring wide(const QString& text)
{
    return text.toStdWString();
}

QString lastError()
{
    return QString::number(static_cast<quint32>(GetLastError()));
}

// A handle that closes itself.
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

QByteArray readAll(HANDLE h)
{
    QByteArray    out;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024 * 1024)
        return out;
    out.resize(static_cast<int>(size.QuadPart));
    DWORD got   = 0;
    qint64 done = 0;
    while (done < out.size()) {
        if (!ReadFile(h, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr) || got == 0)
            return {};
        done += got;
    }
    return out;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    const std::wstring w = wide(QDir::toNativeSeparators(path));
    HANDLE             h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    qint64 done = 0;
    DWORD  put  = 0;
    bool   ok   = true;
    while (ok && done < bytes.size()) {
        ok = WriteFile(h, bytes.constData() + done, static_cast<DWORD>(bytes.size() - done), &put, nullptr) && put > 0;
        done += put;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

HANDLE openShared(const QString& path)
{
    const std::wstring w = wide(QDir::toNativeSeparators(path));
    // Read only, and nobody may write or delete it while we hold it (until LoadLibraryExW has mapped it).
    return CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void* thisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

} // namespace

bool runtimePresentWith(const RegistryReader& reader)
{
    for (const Hive hive : {Hive::LocalMachine32, Hive::CurrentUser}) {
        ++g_reads;
        const QString pv = reader ? reader(hive) : readPv(hive);
        if (!pv.isEmpty() && pv != QLatin1String("0.0.0.0"))
            return true;
    }
    return false;
}

bool runtimePresent()
{
    if (g_cached < 0 || g_refresh) {
        g_refresh = false;
        g_cached  = runtimePresentWith(testReader()) ? 1 : 0;
    }
    return g_cached == 1;
}

void refreshRuntime()
{
    g_refresh = true;
}

void setRegistryReaderForTests(RegistryReader reader)
{
    testReader() = std::move(reader);
    g_cached     = -1;
    g_reads      = 0;
}

int registryReadsForTests()
{
    return g_reads;
}

bool helperBuiltIn()
{
#ifdef TSMEDIA_PLAYER_RESOURCE
    return true;
#else
    return false;
#endif
}

QByteArray embeddedHelper()
{
#ifdef TSMEDIA_PLAYER_RESOURCE
    HMODULE module = static_cast<HMODULE>(thisModule());
    HRSRC   info   = module ? FindResourceW(module, L"TSMP", MAKEINTRESOURCEW(10) /* RT_RCDATA */) : nullptr;
    HGLOBAL data   = info ? LoadResource(module, info) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    const DWORD size  = info ? SizeofResource(module, info) : 0;
    if (!bytes || size == 0)
        return {};
    return QByteArray(static_cast<const char*>(bytes), static_cast<int>(size)); // a copy: the plugin DLL may go
#else
    return {};
#endif
}

QString archName()
{
    return sizeof(void*) == 8 ? QString::fromLatin1("win64") : QString::fromLatin1("win32");
}

QString helperFileName(const QByteArray& bytes, const QString& arch)
{
    return QString::fromLatin1("tsmedia_player_") + arch + QLatin1Char('_') + hexOf(bytes).left(16) + QString::fromLatin1(".dll");
}

QString playerDir()
{
    return QDir(ts3::dataDir()).filePath(QString::fromLatin1("player"));
}

QString userDataFolderFor(const QString& localAppData, bool elevated)
{
    if (localAppData.isEmpty())
        return {};
    return QDir::fromNativeSeparators(localAppData) + (elevated ? QString::fromLatin1("/TSMedia/webview2-elevated") : QString::fromLatin1("/TSMedia/webview2"));
}

QStringList userDataFamily(const QString& folder)
{
    const QString normal   = QString::fromLatin1("/webview2");
    const QString elevated = QString::fromLatin1("/webview2-elevated");
    const QString path     = QDir::fromNativeSeparators(folder);
    if (path.endsWith(elevated, Qt::CaseInsensitive) && path.size() > elevated.size())
        return {path, path.left(path.size() - elevated.size()) + normal};
    if (path.endsWith(normal, Qt::CaseInsensitive) && path.size() > normal.size())
        return {path, path.left(path.size() - normal.size()) + elevated};
    return {};
}

bool userDataInUse(const QString& folder)
{
    // Removing a lock file nobody holds (a crashed run's) is harmless: the next browser process makes it again.
    const QString lock = folder + QString::fromLatin1("/EBWebView/lockfile");
    return QFileInfo::exists(lock) && !QFile::remove(lock);
}

bool elevatedToken()
{
    static int cached = -1;
    if (cached < 0) {
        cached       = 0;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elevation{};
            DWORD           got = 0;
            if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &got) && elevation.TokenIsElevated)
                cached = 1;
            CloseHandle(token);
        }
    }
    return cached == 1;
}

QString userDataFolder()
{
    PWSTR   local = nullptr;
    QString out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)) && local)
        out = userDataFolderFor(QString::fromWCharArray(local), elevatedToken());
    if (local)
        CoTaskMemFree(local);
    return out;
}

void tokenInfo(int* integrityRid, int* uiAccess)
{
    *integrityRid = -1;
    *uiAccess     = -1;
    HANDLE token  = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;
    BYTE  buffer[256] = {};
    DWORD got         = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &got)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
        const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
        if (count > 0)
            *integrityRid = static_cast<int>(*GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(count - 1)));
    }
    DWORD access = 0;
    if (GetTokenInformation(token, TokenUIAccess, &access, sizeof(access), &got))
        *uiAccess = access ? 1 : 0;
    CloseHandle(token);
}

QRect clientRectOnScreen(void* hwnd)
{
    HWND  h      = static_cast<HWND>(hwnd);
    POINT origin = {0, 0};
    RECT  client = {};
    if (!h || !GetClientRect(h, &client) || !ClientToScreen(h, &origin))
        return {};
    return QRect(origin.x, origin.y, client.right - client.left, client.bottom - client.top);
}

int deleteStaleHelpers(const QString& dir, const QString& keep)
{
    int          removed = 0;
    QDirIterator it(dir, {QString::fromLatin1("tsmedia_player_*.dll"), QString::fromLatin1("tsmedia_player_*.dll.part")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!keep.isEmpty() && QFileInfo(path).fileName().compare(keep, Qt::CaseInsensitive) == 0)
            continue;
        if (QFile::remove(path)) // a loaded (pinned) one can't go now: next time
            ++removed;
    }
    return removed;
}

LoadResult extractAndLoad(const LoadOptions& options)
{
    LoadResult r;
    const auto fail = [&r](const char* step, quint32 error) {
        r.step  = QString::fromLatin1(step);
        r.error = error;
        return r;
    };
    if (options.bytes.size() < 64)
        return fail("resource", 0);
    if (!QDir().mkpath(options.dir))
        return fail("write", 0);
    const QString name = helperFileName(options.bytes, options.arch);
    r.path             = QDir(options.dir).filePath(name);
    Handle file;
    file.h = openShared(r.path);
    bool good = false;
    if (file.ok())
        good = readAll(file.h) == options.bytes;
    if (!good) {
        // Missing or not ours: written next to it, then renamed over it (never a half-written helper under its name).
        if (file.ok()) {
            CloseHandle(file.h);
            file.h = INVALID_HANDLE_VALUE;
        }
        const QString part = r.path + QString::fromLatin1(".part");
        if (!writeFile(part, options.bytes))
            return fail("write", GetLastError());
        const std::wstring from = wide(QDir::toNativeSeparators(part)), to = wide(QDir::toNativeSeparators(r.path));
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const quint32 e = GetLastError();
            DeleteFileW(from.c_str());
            return fail("write", e);
        }
        file.h = openShared(r.path);
        if (!file.ok())
            return fail("open", GetLastError());
        if (readAll(file.h) != options.bytes)
            return fail("hash", 0);
    }
    if (options.beforeLoad)
        options.beforeLoad(r.path);
    // With the handle still open nobody can have changed the file in between. Its own imports come from System32 only.
    const std::wstring path   = wide(QDir::toNativeSeparators(r.path));
    HMODULE            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return fail("load", GetLastError());
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path.c_str(), &pinned) || pinned != module)
        return fail("pin", GetLastError()); // stays loaded anyway (never FreeLibrary'd)
    r.module = module;
    const auto version = reinterpret_cast<tsmp_apiVersion_fn>(GetProcAddress(module, "tsmp_apiVersion"));
    for (const char* symbol : {"tsmp_apiVersion", "tsmp_runtimeVersion", "tsmp_init", "tsmp_open", "tsmp_place", "tsmp_command", "tsmp_close", "tsmp_poll",
                               "tsmp_releaseEngine", "tsmp_shutdown"}) {
        if (!GetProcAddress(module, symbol))
            return fail("export", GetLastError());
    }
    if (!version || version() != options.apiVersion)
        return fail("version", 0);
    r.ok = true;
    return r;
}

} // namespace ytbackend

// ---- HelperBackend ---------------------------------------------------------------------------------------------

HelperBackend::HelperBackend() = default;

HelperBackend::~HelperBackend()
{
    shutdown();
}

void HelperBackend::setSourceForTests(const QByteArray& bytes, const QString& dir)
{
    m_testBytes = bytes;
    m_testDir   = dir;
}

bool HelperBackend::load(QString* why)
{
    if (m_failed) {
        if (why)
            *why = m_failure;
        return false;
    }
    if (m_initialised)
        return true;
    if (!m_api.init) {
        ytbackend::LoadOptions options;
        options.bytes      = m_testBytes.isEmpty() ? ytbackend::embeddedHelper() : m_testBytes;
        options.dir        = m_testDir.isEmpty() ? ytbackend::playerDir() : m_testDir;
        options.arch       = ytbackend::archName();
        options.apiVersion = TSMP_API_VERSION;
        const ytbackend::LoadResult r = ytbackend::extractAndLoad(options);
        if (!r.ok) {
            m_failed  = true;
            m_failure = r.step + QLatin1Char(' ') + QString::number(r.error);
            ts3::log(LogLevel_WARNING, 0, "[ytplayer] helper unavailable: %1 %2", {ts3::pub(r.step), ts3::pub(QString::number(r.error))});
            if (why)
                *why = m_failure;
            return false;
        }
        HMODULE m               = static_cast<HMODULE>(r.module);
        m_api.apiVersion        = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_apiVersion"));
        m_api.runtimeVersion    = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_runtimeVersion"));
        m_api.init              = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_init"));
        m_api.open              = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_open"));
        m_api.place             = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_place"));
        m_api.command           = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_command"));
        m_api.close             = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_close"));
        m_api.poll              = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_poll"));
        m_api.releaseEngine     = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_releaseEngine"));
        m_api.shutdown          = reinterpret_cast<void*>(GetProcAddress(m, "tsmp_shutdown"));
    }
    // %LOCALAPPDATA%\TSMedia\webview2: local, never roaming (WebView2's own data: 3.6).
    m_userData = QDir::toNativeSeparators(ytbackend::userDataFolder()).toStdWString();
    m_origin = QString::fromLatin1(ytpage::kAppOrigin).toStdWString();
    m_language.clear();
    TsmpInit init{};
    init.userDataFolder = m_userData.empty() ? nullptr : m_userData.c_str();
    init.appOrigin      = m_origin.c_str();
    init.language       = nullptr;
    init.testMode       = 0;
    const int rc        = reinterpret_cast<tsmp_init_fn>(m_api.init)(&init);
    if (rc != 0 || m_userData.empty()) {
        m_failed  = true;
        m_failure = QString::fromLatin1("init ") + QString::number(rc);
        ts3::log(LogLevel_WARNING, 0, "[ytplayer] helper unavailable: init %1", {ts3::pub(QString::number(rc))});
        if (why)
            *why = m_failure;
        return false;
    }
    m_initialised = true;
    return true;
}

int HelperBackend::open(const PlayerOpen& o)
{
    if (!m_initialised)
        return 0;
    const std::wstring page  = o.page.toStdWString();
    const std::wstring title = o.title.toStdWString();
    TsmpOpen           t{};
    t.ownerHwnd   = o.owner;
    t.popup       = o.popup ? 1 : 0;
    t.rect        = RECT{o.rect.left(), o.rect.top(), o.rect.left() + o.rect.width(), o.rect.top() + o.rect.height()};
    t.clip        = RECT{o.clip.left(), o.clip.top(), o.clip.left() + o.clip.width(), o.clip.top() + o.clip.height()};
    t.cssScale    = o.cssScale;
    t.pageHtml    = page.c_str();
    t.windowTitle = title.c_str();
    return reinterpret_cast<tsmp_open_fn>(m_api.open)(&t);
}

void HelperBackend::place(int id, const QRect& rect, const QRect& clip, double cssScale, bool visible)
{
    if (!m_initialised)
        return;
    const RECT r{rect.left(), rect.top(), rect.left() + rect.width(), rect.top() + rect.height()};
    const RECT c{clip.left(), clip.top(), clip.left() + clip.width(), clip.top() + clip.height()};
    reinterpret_cast<tsmp_place_fn>(m_api.place)(id, r, c, cssScale, visible ? 1 : 0);
}

void HelperBackend::command(int id, const QString& command)
{
    // The page's commands, and "front" (the popup to the front: the helper's own, never passed to the page).
    if (!m_initialised || !(ytpage::validCommand(command) || command == QLatin1String("front")))
        return;
    const std::wstring w = command.toStdWString();
    reinterpret_cast<tsmp_command_fn>(m_api.command)(id, w.c_str());
}

void HelperBackend::close(int id)
{
    if (m_initialised)
        reinterpret_cast<tsmp_close_fn>(m_api.close)(id);
}

QVector<PlayerEvent> HelperBackend::poll()
{
    QVector<PlayerEvent> out;
    if (!m_initialised)
        return out;
    TsmpEvent events[8];
    for (int round = 0; round < 8; ++round) {
        const int n = reinterpret_cast<tsmp_poll_fn>(m_api.poll)(events, 8);
        for (int i = 0; i < n && i < 8; ++i) {
            events[i].text[2048] = 0;
            PlayerEvent e;
            e.player = events[i].player;
            e.kind   = events[i].kind;
            e.value  = events[i].value;
            e.text   = QString::fromWCharArray(events[i].text);
            out.append(e);
        }
        if (n < 8)
            break;
    }
    return out;
}

void HelperBackend::releaseEngine()
{
    if (m_initialised)
        reinterpret_cast<tsmp_releaseEngine_fn>(m_api.releaseEngine)();
}

void HelperBackend::shutdown()
{
    if (m_initialised)
        reinterpret_cast<tsmp_shutdown_fn>(m_api.shutdown)();
    m_initialised = false; // a later load() calls tsmp_init again on the same (pinned) image
}

QString HelperBackend::runtimeVersion()
{
    if (!m_api.runtimeVersion)
        return {};
    wchar_t buffer[128] = {};
    if (reinterpret_cast<tsmp_runtimeVersion_fn>(m_api.runtimeVersion)(buffer, 128) != 0)
        return {};
    buffer[127] = 0;
    return QString::fromWCharArray(buffer);
}
