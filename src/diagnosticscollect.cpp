#include "diagnosticscollect.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QScreen>
#include <QWidget>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <objbase.h>

#include <algorithm>
#include <vector>

#include "chatintegration.h"
#include "core.h"
#include "i18n.h"
#include "settings.h"
#include "ts3api.h"
#include "uiutil.h"
#include "version.h"
#include "video/mfvideo.h"

namespace diag {

namespace {

// ---- session counters -----------------------------------------------------------------------------

// Watches Core's change signals and counts finished and failed transfers. Every state change of an
// entry or upload goes through those signals, so a transition is seen exactly once.
class SessionStats : public QObject
{
  public:
    explicit SessionStats(Core* core)
        : QObject(core)
        , m_core(core)
    {
        m_started.start();
        connect(core, &Core::entryChanged, this, [this](const QString& key) { onEntryChanged(key); });
        connect(core, &Core::uploadChanged, this, [this](int id) { onUploadChanged(id); });
    }

    SessionCounts counts() const
    {
        SessionCounts c = m_counts;
        c.sessionMs     = m_started.elapsed();
        return c;
    }

  private:
    void onEntryChanged(const QString& key)
    {
        const MediaEntry* e = m_core ? m_core->entry(key) : nullptr;
        if (!e)
            return;
        const MediaState before = m_entryStates.value(key, MediaState::Idle);
        if (before == e->state)
            return;
        m_entryStates.insert(key, e->state);
        // A cached or own file is Ready without a download: only Queued/Downloading -> Ready counts.
        if (e->state == MediaState::Ready && (before == MediaState::Downloading || before == MediaState::Queued))
            ++m_counts.downloadsOk;
        else if (e->state == MediaState::Failed) {
            ++m_counts.downloadsFailed;
            ++m_counts.downloadErrors[static_cast<int>(e->error)];
        }
    }

    void onUploadChanged(int id)
    {
        const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
        if (!job) {
            m_uploadStates.remove(id); // the job is gone
            return;
        }
        const UploadState before = m_uploadStates.value(id, UploadState::Preparing);
        if (before == job->state)
            return;
        m_uploadStates.insert(id, job->state);
        if (job->state == UploadState::Done)
            ++m_counts.uploadsOk;
        else if (job->state == UploadState::Failed)
            ++m_counts.uploadsFailed;
        else if (job->state == UploadState::Canceled)
            ++m_counts.uploadsCanceled;
    }

    QPointer<Core>              m_core;
    QElapsedTimer               m_started;
    SessionCounts               m_counts;
    QHash<QString, MediaState>  m_entryStates;
    QHash<int, UploadState>     m_uploadStates;
};

QPointer<SessionStats>       g_sessionStats; // GUI thread only
std::vector<SectionProvider> g_sectionProviders;

// ---- Windows ----------------------------------------------------------------------------------------

// Read with Win32 directly (never QSettings: its cache outlives the DLL, see settings.cpp).
class VersionKey
{
  public:
    VersionKey()
    {
        // The 64-bit view also from a 32-bit TeamSpeak.
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY, &m_key) != ERROR_SUCCESS)
            m_key = nullptr;
    }
    ~VersionKey()
    {
        if (m_key)
            RegCloseKey(m_key);
    }

    QString text(const wchar_t* name) const
    {
        wchar_t buffer[256] = {};
        DWORD   size        = sizeof(buffer) - sizeof(wchar_t);
        if (!m_key || RegGetValueW(m_key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
            return {};
        return QString::fromWCharArray(buffer);
    }

    int number(const wchar_t* name) const
    {
        DWORD value = 0;
        DWORD size  = sizeof(value);
        if (!m_key || RegGetValueW(m_key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return 0;
        return static_cast<int>(qMin<DWORD>(value, 0x7FFFFFFF));
    }

  private:
    Q_DISABLE_COPY(VersionKey)
    HKEY m_key = nullptr;
};

QString machineName(USHORT machine)
{
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64:
        return QString::fromLatin1("x64");
    case IMAGE_FILE_MACHINE_ARM64:
        return QString::fromLatin1("ARM64");
    case IMAGE_FILE_MACHINE_I386:
        return QString::fromLatin1("x86");
    default:
        return QString::fromLatin1("machine 0x%1").arg(machine, 4, 16, QLatin1Char('0'));
    }
}

void collectWindows(Facts* f)
{
    const VersionKey key;
    f->windowsProduct        = key.text(L"ProductName");
    f->windowsDisplayVersion = key.text(L"DisplayVersion");
    if (f->windowsDisplayVersion.isEmpty())
        f->windowsDisplayVersion = key.text(L"ReleaseId"); // before Windows 10 20H2
    f->windowsEdition = key.text(L"EditionID");
    f->windowsBuild   = key.text(L"CurrentBuildNumber").toInt();
    f->windowsUbr     = key.number(L"UBR");

    // IsWow64Process2 (Windows 10 1709+) also tells an ARM64 machine; GetNativeSystemInfo before that.
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel    = GetModuleHandleW(L"kernel32.dll");
    const auto    isWow64   = kernel ? reinterpret_cast<IsWow64Process2Fn>(reinterpret_cast<void*>(GetProcAddress(kernel, "IsWow64Process2"))) : nullptr;
    USHORT        process = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT        native  = IMAGE_FILE_MACHINE_UNKNOWN;
    if (isWow64 && isWow64(GetCurrentProcess(), &process, &native)) {
        f->nativeArch = machineName(native);
        f->wow64      = process != IMAGE_FILE_MACHINE_UNKNOWN;
        return;
    }
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        f->nativeArch = QString::fromLatin1("x64");
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        f->nativeArch = QString::fromLatin1("ARM64");
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        f->nativeArch = QString::fromLatin1("x86");
        break;
    default:
        break;
    }
    f->wow64 = sizeof(void*) == 4 && f->nativeArch != QLatin1String("x86") && !f->nativeArch.isEmpty();
}

// ---- Media Foundation -------------------------------------------------------------------------------

// The plugin delay-loads mfplat.dll / mfreadwrite.dll (Windows N editions may lack them): nothing may
// call into Media Foundation before this returned true (the same check as mfvideo.cpp).
bool mediaFoundationPresent()
{
    for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll"}) {
        const HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return false;
        FreeLibrary(module);
    }
    return true;
}

QString hexCode(HRESULT hr)
{
    return QString::fromLatin1("0x") + QString::number(static_cast<quint32>(hr), 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

// Whether a transform for subtype is registered: as input for decoders, as output for encoders.
Codec findTransform(const GUID& category, const GUID& major, const GUID& subtype, bool decoder, const char* label, HRESULT* error)
{
    Codec codec;
    codec.label = QString::fromLatin1(label);
    MFT_REGISTER_TYPE_INFO type{major, subtype};
    IMFActivate**          activates = nullptr;
    UINT32                 count     = 0;
    const HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, decoder ? &type : nullptr, decoder ? nullptr : &type, &activates, &count);
    if (FAILED(hr)) {
        if (SUCCEEDED(*error))
            *error = hr;
        return codec;
    }
    codec.found = count > 0;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 flags = 0;
        if (activates[i] && SUCCEEDED(activates[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &flags)) && (flags & MFT_ENUM_FLAG_HARDWARE))
            codec.hardware = true;
        if (activates[i])
            activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return codec;
}

void collectCodecs(Facts* f)
{
    f->mediaFoundationPresent = mediaFoundationPresent();
    if (!f->mediaFoundationPresent)
        return;
    CodecReport& report = f->codecs;
    report.checked      = true;
    QElapsedTimer timer;
    timer.start();

    // Balanced on this pool thread; RPC_E_CHANGED_MODE: the thread already has the other apartment.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        report.error = hexCode(com);
        return;
    }
    const HRESULT started = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(started)) {
        report.error = hexCode(started);
    } else {
        HRESULT error = S_OK;
        struct Wanted {
            const GUID* subtype;
            const char* label;
        };
        const Wanted video[] = {{&MFVideoFormat_H264, "H.264"}, {&MFVideoFormat_HEVC, "HEVC"}, {&MFVideoFormat_VP90, "VP9"},
                                {&MFVideoFormat_AV1, "AV1"},     {&MFVideoFormat_MP4V, "MPEG-4 Part 2"}};
        const Wanted audio[] = {{&MFAudioFormat_AAC, "AAC"}, {&MFAudioFormat_MP3, "MP3"}, {&MFAudioFormat_Opus, "Opus"}, {&MFAudioFormat_FLAC, "FLAC"}};
        for (const Wanted& w : video)
            report.videoDecoders << findTransform(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, *w.subtype, true, w.label, &error);
        for (const Wanted& w : audio)
            report.audioDecoders << findTransform(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, *w.subtype, true, w.label, &error);
        report.encoders << findTransform(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_H264, false, "H.264", &error);
        report.encoders << findTransform(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC, false, "AAC", &error);
        if (FAILED(error))
            report.error = hexCode(error);
        MFShutdown();
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    report.elapsedMs = timer.elapsed();
}

// ---- TeamSpeak ----------------------------------------------------------------------------------------

bool teamSpeakAvailable()
{
    return ts3::funcs.getServerConnectionHandlerList && ts3::funcs.getConnectionStatus && ts3::funcs.freeMemory;
}

QString teamSpeakVersion(bool* fromLib)
{
    *fromLib = false;
    if (teamSpeakAvailable() && ts3::funcs.getClientSelfVariableAsString) {
        // The client's own version (CLIENT_VERSION), readable while a server tab is connected.
        QList<uint64> tabs = ts3::connections();
        tabs.prepend(ts3::currentConnection());
        for (uint64 sch : qAsConst(tabs)) {
            char* value = nullptr;
            if (ts3::isConnected(sch) && ts3::funcs.getClientSelfVariableAsString(sch, CLIENT_VERSION, &value) == ERROR_ok && value) {
                const QString version = ts3::takeString(value);
                if (!version.isEmpty())
                    return version;
            }
        }
    }
    char* lib = nullptr;
    if (ts3::funcs.getClientLibVersion && ts3::funcs.freeMemory && ts3::funcs.getClientLibVersion(&lib) == ERROR_ok && lib) {
        *fromLib = true;
        return ts3::takeString(lib);
    }
    return {};
}

QString serverVariable(uint64 sch, size_t flag)
{
    char* value = nullptr;
    if (!ts3::funcs.getServerVariableAsString || ts3::funcs.getServerVariableAsString(sch, flag, &value) != ERROR_ok || !value)
        return {};
    return ts3::takeString(value);
}

QString configDir()
{
    char path[1024] = {};
    if (!ts3::funcs.getConfigPath)
        return {};
    ts3::funcs.getConfigPath(path, sizeof(path));
    return QString::fromUtf8(path);
}

// TeamSpeak's dark skins colour everything by style sheet; once polished, the palette shows them.
bool isDark(const QPalette& palette)
{
    return palette.color(QPalette::WindowText).lightness() > 170 || palette.color(QPalette::Window).lightness() < 128;
}

} // namespace

// ============================================================================================

Facts collectOnGui(const Environment& env, WorkerInput* input)
{
    Facts f;
    f.created       = QDateTime::currentDateTime();
    f.pluginName    = QString::fromLatin1(TSMEDIA_NAME);
    f.pluginVersion = QString::fromLatin1(TSMEDIA_VERSION);
    f.pluginBits    = static_cast<int>(sizeof(void*) * 8);
    f.qtRuntime     = QString::fromLatin1(qVersion());
    f.qtBuilt       = QString::fromLatin1(QT_VERSION_STR);
    f.pluginApi     = env.pluginApi;

    f.teamSpeakVersion = teamSpeakVersion(&f.teamSpeakVersionFromLib);
    const QString config = configDir();
    if (!config.isEmpty()) {
        const QString standard = QDir::cleanPath(qEnvironmentVariable("APPDATA") + QLatin1String("/TS3Client"));
        f.configFolder         = QDir::cleanPath(config).compare(standard, Qt::CaseInsensitive) == 0 ? 1 : 2;
        input->teamSpeakLogsDir = QDir::cleanPath(config + QLatin1String("/logs"));
    }

    f.mediaFoundationStarted = env.mediaFoundationStarted;
    f.videoDevice            = static_cast<int>(mf::lastVideoDevice());

    QWidget*       window = env.chat ? env.chat->mainWindow() : nullptr;
    const QScreen* screen = window ? window->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        f.devicePixelRatio = screen->devicePixelRatio();
        f.scalePercent     = qRound(screen->logicalDotsPerInch() / 96.0 * screen->devicePixelRatio() * 100.0);
    }
    f.screens    = QGuiApplication::screens().size();
    f.animations = ui::animationsEnabled();
    f.theme      = isDark(window ? window->palette() : QApplication::palette()) ? 2 : 1;

    if (teamSpeakAvailable()) {
        for (uint64 sch : ts3::connections()) {
            if (!ts3::isConnected(sch))
                continue;
            ++f.connections;
            const QString version  = serverVariable(sch, VIRTUALSERVER_VERSION);
            const QString platform = serverVariable(sch, VIRTUALSERVER_PLATFORM);
            if (!version.isEmpty())
                f.serverVersions << (platform.isEmpty() ? version : i18n::t("%1 on %2").arg(version, platform));
        }
    }
    if (env.chat) {
        f.chatViews  = env.chat->hookedChatViews();
        f.chatInputs = env.chat->hookedInputs();
    }

    f.settings = Settings::instance();
    f.session  = sessionCounts();
    if (env.core)
        input->cacheDir = env.core->cacheDir();

    for (SectionProvider provider : g_sectionProviders) {
        Section section = provider();
        if (!section.title.isEmpty())
            f.extra << section;
    }
    return f;
}

void collectOnWorker(Facts* f, const WorkerInput& input)
{
    collectWindows(f);
    collectCodecs(f);

    if (!input.cacheDir.isEmpty()) {
        QDirIterator it(input.cacheDir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            f->cacheBytes += static_cast<quint64>(qMax<qint64>(0, it.fileInfo().size()));
            ++f->cacheFiles;
        }
        f->cacheKnown = true;
    }

    // The plugin's own log when it is wired, else TeamSpeak's client log.
    LogTail tail;
    if (const LogTailProvider provider = logTailProvider())
        tail = provider(kMaxLogLines);
    if (tail.source == LogTail::Source::None) {
        LogTail fallback = readTeamSpeakLog(input.teamSpeakLogsDir, kMaxLogLines);
        if (fallback.source != LogTail::Source::None || tail.problem.isEmpty())
            tail = fallback;
    }
    f->log = tail;
}

LogTail readTeamSpeakLog(const QString& logsDir, int maxLines)
{
    LogTail none;
    if (logsDir.isEmpty()) {
        none.problem = i18n::t("TeamSpeak's log folder is unknown");
        return none;
    }
    const QFileInfoList logs = QDir(logsDir).entryInfoList({QString::fromLatin1("*.log")}, QDir::Files, QDir::Time); // newest first
    if (logs.isEmpty()) {
        none.problem = i18n::t("no TeamSpeak log found");
        return none;
    }
    QFile file(logs.first().absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        none.problem = i18n::t("couldn't read TeamSpeak's log");
        return none;
    }
    constexpr qint64 kMaxRead = 2 * 1024 * 1024;
    const qint64     size     = file.size();
    const bool       partial  = size > kMaxRead;
    if (partial)
        file.seek(size - kMaxRead);
    QByteArray data = file.read(kMaxRead);
    if (partial) {
        const int firstBreak = data.indexOf('\n');
        data.remove(0, firstBreak < 0 ? data.size() : firstBreak + 1); // a cut first line
    }
    LogTail tail = parseTeamSpeakLog(data, maxLines);
    tail.partial = partial;
    return tail;
}

void addSectionProvider(SectionProvider provider)
{
    if (provider && std::find(g_sectionProviders.begin(), g_sectionProviders.end(), provider) == g_sectionProviders.end())
        g_sectionProviders.push_back(provider);
}

void startSessionStats(Core* core)
{
    if (core && !g_sessionStats)
        g_sessionStats = new SessionStats(core);
}

SessionCounts sessionCounts()
{
    return g_sessionStats ? g_sessionStats->counts() : SessionCounts();
}

} // namespace diag
