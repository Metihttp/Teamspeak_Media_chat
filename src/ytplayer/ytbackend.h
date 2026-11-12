#pragma once

// 2.5 YouTube player (design2.md 3.2): the backend between YouTubePlayer and the player helper.
//  * PlayerBackend: what YouTubePlayer needs (tests use a fake);
//  * HelperBackend: the real one. Inside Play only, the helper DLL embedded in this plugin DLL (RCDATA "TSMP") is
//    written content-addressed to <TeamSpeak config>/plugins/tsmedia/player/tsmedia_player_<arch>_<16 hex>.dll (or the
//    file there is checked), loaded with LoadLibraryExW while a handle without write sharing is held, pinned (it is
//    never unloaded: no WebView2 callback can ever run unmapped code), its C API resolved and its version checked. Any
//    failing step makes the player unavailable for the session (Play opens the browser; one log line);
//  * runtimePresent(): whether this PC has the WebView2 Runtime, from its documented registry value only (never by
//    loading the helper). Cached; read again when the Settings dialog opens.
// No WebView2 header is ever included here (a source rule in tst_ytplayer).

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ytbackend {

// ---- the WebView2 Runtime (3.2) ---------------------------------------------------------------------------------
enum class Hive { LocalMachine32, CurrentUser };
// The value "pv" under SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5} (empty: none).
using RegistryReader = std::function<QString(Hive)>;
bool runtimePresentWith(const RegistryReader& reader); // a non-empty value that isn't 0.0.0.0, in either hive
bool runtimePresent();                                 // cached for the session
void refreshRuntime();                                 // the next runtimePresent() reads again (the Settings dialog)
void setRegistryReaderForTests(RegistryReader reader); // nullptr: the real registry
int  registryReadsForTests();                          // reads of the reader so far

// This build embeds the helper (x64 with the WebView2 SDK; win32 only with TSMEDIA_PLAYER_WIN32=ON).
bool       helperBuiltIn();
QByteArray embeddedHelper(); // the RCDATA "TSMP" of this DLL (empty: none)
QString    archName();       // "win64" | "win32"

// ---- extraction and loading (3.2 Delivery) ------------------------------------------------------------------------
QString helperFileName(const QByteArray& bytes, const QString& arch); // tsmedia_player_<arch>_<first 16 hex of SHA-256>.dll
QString playerDir();                                                  // ts3::dataDir()/player
// WebView2's user data folder: %LOCALAPPDATA%\TSMedia\webview2, or webview2-elevated when TeamSpeak's token is
// elevated (WebView2's guidance for elevated hosts, author decision 4: an elevated and a normal TeamSpeak never share
// one folder, which ExclusiveUserDataFolderAccess would refuse). Empty: unknown.
QString userDataFolder();
QString userDataFolderFor(const QString& localAppData, bool elevated); // the rule above (pure, tests)
// Both elevations' folders of a user data folder named by the rule above (webview2 and webview2-elevated side by
// side), that folder first; empty for any other folder: nothing else is ever deleted (pure, tests).
QStringList userDataFamily(const QString& folder);
// A WebView2 browser process still runs on that folder: Chromium holds EBWebView/lockfile open (without delete sharing,
// deleted when it closes) for as long as it runs, so it can't be removed then. A folder in use is never deleted in part.
bool    userDataInUse(const QString& folder);
bool    elevatedToken();                                               // TokenElevation, read once
// The process token's integrity level (its RID, e.g. 0x2000 Medium, 0x3000 High) and UIAccess flag (-1: unknown).
void    tokenInfo(int* integrityRid, int* uiAccess);
// A window's client area in screen device px (ClientToScreen); empty for no window.
QRect   clientRectOnScreen(void* hwnd);
// Every tsmedia_player_*.dll in dir except keep (at plugin start; a loaded one fails silently and goes next time).
int     deleteStaleHelpers(const QString& dir, const QString& keep);

struct LoadResult {
    bool    ok    = false;
    QString step;          // the step that failed: resource, write, open, hash, load, pin, export, version
    quint32 error = 0;     // GetLastError() of it
    QString path;
    void*   module = nullptr;
};
struct LoadOptions {
    QByteArray bytes;         // the helper's bytes (embeddedHelper(), or a test DLL)
    QString    dir;           // where it goes
    QString    arch;          // the name's arch part
    int        apiVersion = 1; // TSMP_API_VERSION
    // Tests: called while the verified file is open without write sharing, right before LoadLibraryExW.
    std::function<void(const QString& path)> beforeLoad;
};
// Extract (or check), load, pin, resolve and check the version. Never throws; the file handle is closed either way.
LoadResult extractAndLoad(const LoadOptions& options);

} // namespace ytbackend

// What YouTubePlayer gets from the helper (TsmpEvent, copied).
struct PlayerEvent {
    int     player = 0;
    int     kind   = 0; // TSMP_EV_*
    int     value  = 0;
    QString text;
};

struct PlayerOpen {
    void*   owner = nullptr; // TeamSpeak's top-level window
    bool    popup = true;
    QRect   rect;            // screen device px
    QRect   clip;
    double  cssScale = 1.0;
    QString page;            // ytpage::pageHtml
    QString title;           // the popup's title
};

class PlayerBackend
{
  public:
    virtual ~PlayerBackend() = default;
    // Extracts, loads and initialises the helper the first time (only inside Play). False: unavailable for the session
    // (*why: the failed step, for the log line).
    virtual bool                 load(QString* why) = 0;
    virtual int                  open(const PlayerOpen& open) = 0; // > 0: a player id
    // flags: what the real backend passes with SetWindowPos after the first show (tests check "no z-order change").
    virtual void                 place(int id, const QRect& rect, const QRect& clip, double cssScale, bool visible) = 0;
    virtual void                 command(int id, const QString& command) = 0;
    virtual void                 close(int id) = 0;
    virtual QVector<PlayerEvent> poll() = 0;
    virtual void                 releaseEngine() = 0;
    virtual void                 shutdown() = 0;
    virtual QString              runtimeVersion() = 0; // after load(); empty: unknown
};

// The real backend: the embedded helper and its C API.
class HelperBackend : public PlayerBackend
{
  public:
    HelperBackend();
    ~HelperBackend() override;

    bool                 load(QString* why) override;
    int                  open(const PlayerOpen& open) override;
    void                 place(int id, const QRect& rect, const QRect& clip, double cssScale, bool visible) override;
    void                 command(int id, const QString& command) override;
    void                 close(int id) override;
    QVector<PlayerEvent> poll() override;
    void                 releaseEngine() override;
    void                 shutdown() override;
    QString              runtimeVersion() override;

    // Tests: another helper (a stub DLL's bytes) and folder instead of the embedded one.
    void setSourceForTests(const QByteArray& bytes, const QString& dir);
    bool loadedForTests() const { return m_api.init != nullptr; }

  private:
    struct Api {
        void* apiVersion = nullptr;
        void* runtimeVersion = nullptr;
        void* init = nullptr;
        void* open = nullptr;
        void* place = nullptr;
        void* command = nullptr;
        void* close = nullptr;
        void* poll = nullptr;
        void* releaseEngine = nullptr;
        void* shutdown = nullptr;
    };
    Api        m_api;
    bool       m_failed      = false; // unavailable for the session
    bool       m_initialised = false;
    QString    m_failure;
    QByteArray m_testBytes;
    QString    m_testDir;
    // The strings tsmp_init got (the helper copies them; kept until shutdown anyway).
    std::wstring m_userData, m_origin, m_language;
};
