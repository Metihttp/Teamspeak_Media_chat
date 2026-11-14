// 2.5 YouTube player: player_smoke drives the player helper (tsmedia_player_<arch>.dll next to it) the way the plugin
// does, outside TeamSpeak (design2.md 4.4). Never run by the unit tests and never during a live test: it starts
// WebView2's processes and is CPU-heavy for a few seconds. Every web view is MUTED (TSMP_TEST_MUTED).
//
//   player_smoke                 the offline run: the page from memory, a fake IFrame API (no YouTube, no network), the
//                                embed request's Referer recorded and answered with a blank page (never sent), the
//                                page's events and commands, error 150, the anchored window's clip region, an owner
//                                minimized and restored, an owner destroyed (Lost), Esc, close, shutdown while the
//                                environment is still being created, init again on the pinned image, the engine
//                                released (EngineExited, its processes gone) and an open right after a release.
//   player_smoke --youtube <id>  YouTube's real player for 10 s in a visible window (network), muted: the event sequence.
//   player_smoke --youtube <id> --bridge socks5|http host:port
//                                2.6 chat proxy (design 3.6.5, the F3 check): the same through an in-process bridge to that
//                                proxy (the plugin's own Bridge): every proxy login request the helper saw (address and
//                                challenge, never the token), two players in one environment, the bridge's records of the
//                                player and its counters.
//
// 2.6: the offline run also checks the helper's network rules (an open without a network fails "network", a bad bridge
// network is refused, an open whose network needs other arguments than the living environment's fails "network").
//
// WebView2's user data folder is <this exe's folder>/player_smoke_data (never %LOCALAPPDATA%\TSMedia).
// Exit code: 0 all passed, 1 a check failed, 2 the helper couldn't be loaded.

#include <QCoreApplication>
#include <QString>

#include "proxy/bridge.h"
#include "proxy/proxyconfig.h"
#include "proxy/upstream.h"
#include "tsmp.h"
#include "ytplayer/ytpage.h"

#include <objbase.h>
#include <tlhelp32.h>

#include <climits>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#define TSMP_WIDE2(x) L##x
#define TSMP_WIDE(x) TSMP_WIDE2(x)

namespace {

struct Api {
    tsmp_apiVersion_fn     apiVersion     = nullptr;
    tsmp_runtimeVersion_fn runtimeVersion = nullptr;
    tsmp_init_fn           init           = nullptr;
    tsmp_open_fn           open           = nullptr;
    tsmp_place_fn          place          = nullptr;
    tsmp_command_fn        command        = nullptr;
    tsmp_close_fn          close          = nullptr;
    tsmp_poll_fn           poll           = nullptr;
    tsmp_releaseEngine_fn  releaseEngine  = nullptr;
    tsmp_shutdown_fn       shutdown       = nullptr;
    tsmp_setNetwork_fn     setNetwork     = nullptr; // 2.6
} api;

std::vector<TsmpEvent> events; // everything polled so far
int                    failures = 0;
int                    skips    = 0;
std::wstring           dataDir;
const std::wstring     origin = L"https://io.github.metihttp.tsmediachat";

const char* kindName(int kind)
{
    switch (kind) {
    case TSMP_EV_CREATED:
        return "Created";
    case TSMP_EV_READY:
        return "Ready";
    case TSMP_EV_STATE:
        return "State";
    case TSMP_EV_ERROR:
        return "Error";
    case TSMP_EV_TIME:
        return "Time";
    case TSMP_EV_OPENURL:
        return "OpenUrl";
    case TSMP_EV_FAILED:
        return "Failed";
    case TSMP_EV_CLOSEDBYUSER:
        return "ClosedByUser";
    case TSMP_EV_LOST:
        return "Lost";
    case TSMP_EV_KEY:
        return "Key";
    case TSMP_EV_ENGINEEXITED:
        return "EngineExited";
    case TSMP_EV_TEST:
        return "Test";
    default:
        return "?";
    }
}

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok)
        ++failures;
}

void drain()
{
    TsmpEvent batch[8];
    for (;;) {
        const int n = api.poll(batch, 8);
        for (int i = 0; i < n; ++i) {
            std::printf("      event player %d %s %d \"%ls\"\n", batch[i].player, kindName(batch[i].kind), batch[i].value, batch[i].text);
            events.push_back(batch[i]);
        }
        if (n < 8)
            break;
    }
    std::fflush(stdout);
}

// The message loop (WebView2's completions arrive through it) until done() or ms pass; the helper polled every 20 ms.
bool pump(DWORD ms, const std::function<bool()>& done)
{
    const ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        drain();
        if (done && done())
            return true;
        if (GetTickCount64() >= end)
            return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
}

bool has(int player, int kind, int value = INT_MIN, const wchar_t* text = nullptr)
{
    for (const TsmpEvent& e : events) {
        if (e.player != player || e.kind != kind)
            continue;
        if (value != INT_MIN && e.value != value)
            continue;
        if (text && wcsstr(e.text, text) == nullptr)
            continue;
        return true;
    }
    return false;
}

bool waitFor(int player, int kind, DWORD ms, int value = INT_MIN, const wchar_t* text = nullptr)
{
    return pump(ms, [=] { return has(player, kind, value, text); });
}

LRESULT CALLBACK ownerProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

HWND makeOwner()
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ownerProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
        wc.lpszClassName = L"TSMediaPlayerSmokeOwner";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND h = CreateWindowExW(0, L"TSMediaPlayerSmokeOwner", L"player_smoke owner", WS_OVERLAPPEDWINDOW, 80, 80, 900, 640, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

// The player windows the helper owns under owner (their class starts with "TSMediaPlayer_").
std::vector<HWND> playersOf(HWND owner)
{
    struct Find {
        HWND              owner;
        std::vector<HWND> found;
    } find{owner, {}};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto*   f = reinterpret_cast<Find*>(p);
            wchar_t cls[128];
            GetClassNameW(h, cls, 128);
            if (wcsncmp(cls, L"TSMediaPlayer_", 14) == 0 && GetWindow(h, GW_OWNER) == f->owner)
                f->found.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.found;
}

int countWebViewProcesses()
{
    int    count = 0;
    HANDLE snap  = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"msedgewebview2.exe") == 0)
            ++count;
    }
    CloseHandle(snap);
    return count;
}

int openPlayer(HWND owner, bool popup, RECT rect, RECT clip, const QString& page, const wchar_t* title = L"player_smoke")
{
    const std::wstring html = page.toStdWString();
    TsmpOpen           o{};
    o.ownerHwnd   = owner;
    o.popup       = popup ? 1 : 0;
    o.rect        = rect;
    o.clip        = clip;
    o.cssScale    = 1.0;
    o.pageHtml    = html.c_str();
    o.windowTitle = title;
    return api.open(&o);
}

int setNetwork(int mode, int port = 0, const wchar_t* realm = nullptr, const wchar_t* user = nullptr, const wchar_t* token = nullptr)
{
    TsmpNetwork net{};
    net.mode       = mode;
    net.bridgePort = port;
    net.realm      = realm;
    net.user       = user;
    net.token      = token;
    return api.setNetwork(&net);
}

// tsmp_init, then (2.6) the Windows settings as the network: tsmp_init leaves it UNSET.
bool initHelper(int testMode, bool withNetwork = true)
{
    TsmpInit init{};
    init.userDataFolder = dataDir.c_str();
    init.appOrigin      = origin.c_str();
    init.language       = L"en-US";
    init.testMode       = testMode;
    const bool ok       = api.init(&init) == 0;
    return ok && (!withNetwork || setNetwork(TSMP_NET_SYSTEM) == 0);
}

QString testPage(const char* id)
{
    return ytpage::pageHtml(QString::fromLatin1(id), 0, true, QString::fromLatin1("https://io.github.metihttp.tsmediachat/test_api.js"));
}

int offlineRun()
{
    const int baseline = countWebViewProcesses();
    check(initHelper(TSMP_TEST_FAKE_API | TSMP_TEST_MUTED, false), "tsmp_init (STA, fake API, muted)");
    HWND owner = makeOwner();

    // 0. 2.6: no network after init: an open fails "network" and makes nothing; a bad bridge network is refused.
    const RECT r0{140, 140, 140 + 640, 140 + 360};
    const int  p0 = openPlayer(owner, true, r0, r0, testPage("M7lc1UVf-VE"));
    check(p0 > 0 && waitFor(p0, TSMP_EV_FAILED, 2000, INT_MIN, L"network"), "no network: the open fails \"network\"");
    check(playersOf(owner).empty(), "no network: no window");
    check(setNetwork(TSMP_NET_BRIDGE, 80, L"tsmedia-0123456789abcdef", L"tsm-p", L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") < 0,
          "a bridge on port 80 is refused");
    check(setNetwork(TSMP_NET_BRIDGE, 40000, L"realm", L"tsm-p", L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") < 0, "a bridge with a bad realm is refused");
    check(setNetwork(9) < 0, "an unknown mode is refused");
    const int p00 = openPlayer(owner, true, r0, r0, testPage("M7lc1UVf-VE"));
    check(waitFor(p00, TSMP_EV_FAILED, 2000, INT_MIN, L"network"), "a refused network leaves none: the open fails \"network\"");
    check(setNetwork(TSMP_NET_SYSTEM) == 0, "the Windows settings as the network");

    // 1. A popup with the page from memory and the fake IFrame API.
    const RECT r1{140, 140, 140 + 640, 140 + 360};
    const int  p1 = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE"));
    check(p1 > 0, "tsmp_open popup");
    check(waitFor(p1, TSMP_EV_CREATED, 30000), "Created");
    api.place(p1, r1, r1, 1.0, 1); // shown (the plugin does this at Created)
    check(waitFor(p1, TSMP_EV_TEST, 15000, INT_MIN, L"referer:"), "the embed request was intercepted (never sent)");
    check(has(p1, TSMP_EV_TEST, INT_MIN, L"referer:https://io.github.metihttp.tsmediachat/"), "its Referer is our app's origin");
    check(waitFor(p1, TSMP_EV_READY, 15000), "Ready");
    check(waitFor(p1, TSMP_EV_STATE, 5000, 1), "State(1) playing");
    check(waitFor(p1, TSMP_EV_TIME, 5000, 7), "Time(7) arrives without being asked");
    api.command(p1, L"pause");
    check(waitFor(p1, TSMP_EV_TEST, 5000, INT_MIN, L"page:pause") && waitFor(p1, TSMP_EV_STATE, 5000, 2), "pause arrives (State 2)");
    api.command(p1, L"play");
    check(waitFor(p1, TSMP_EV_TEST, 5000, INT_MIN, L"page:play"), "play arrives");
    api.command(p1, L"volume:50");
    check(waitFor(p1, TSMP_EV_TEST, 5000, INT_MIN, L"page:volume:50"), "volume:50 arrives");
    api.command(p1, L"mute");
    check(waitFor(p1, TSMP_EV_TEST, 5000, INT_MIN, L"page:mute"), "mute arrives");
    api.command(p1, L"eval:alert(1)");
    pump(400, nullptr);
    check(!has(p1, TSMP_EV_TEST, INT_MIN, L"page:eval"), "an unknown command never reaches the page");
    const std::vector<HWND> popups = playersOf(owner);
    check(popups.size() == 1, "one player window, owned by the owner");
    if (!popups.empty()) {
        const LONG_PTR style = GetWindowLongPtrW(popups[0], GWL_STYLE);
        check((style & WS_MINIMIZEBOX) == 0 && (style & WS_MAXIMIZEBOX) != 0 && (style & WS_CAPTION) != 0, "popup: a title bar, maximize, no minimize box");
    }

    // 2. Esc in the popup (focus in the page).
    if (!popups.empty()) {
        SetForegroundWindow(popups[0]);
        pump(800, nullptr);
        if (GetForegroundWindow() == popups[0]) {
            INPUT in[2]      = {};
            in[0].type       = INPUT_KEYBOARD;
            in[0].ki.wVk     = VK_ESCAPE;
            in[1]            = in[0];
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            check(waitFor(p1, TSMP_EV_KEY, 3000, VK_ESCAPE), "Esc in the popup gives Key(VK_ESCAPE)");
        } else {
            ++skips;
            std::printf("SKIP  Esc: this console can't bring the popup to the foreground (Windows' foreground rule)\n");
        }
    }
    // 2b. 2.6: the living environment (p1 still open) has the Windows settings' arguments: an open for direct fails
    // "network" and makes no window (the plugin releases the engine first; the helper never does by itself).
    check(setNetwork(TSMP_NET_DIRECT) == 0, "direct as the network");
    const int pd = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE"));
    check(waitFor(pd, TSMP_EV_FAILED, 2000, INT_MIN, L"network"), "another network than the living environment's: \"network\"");
    check(playersOf(owner).size() == 1, "... and no window of its own");
    check(setNetwork(TSMP_NET_SYSTEM) == 0, "the Windows settings again");
    api.close(p1);
    pump(300, nullptr);
    check(playersOf(owner).empty(), "tsmp_close: the window is gone");

    // 3. Error 150 (the fake API refuses the id AAAAAAAAAAA).
    const int p2 = openPlayer(owner, true, r1, r1, testPage("AAAAAAAAAAA"));
    check(waitFor(p2, TSMP_EV_CREATED, 20000), "second player Created");
    api.place(p2, r1, r1, 1.0, 1);
    check(waitFor(p2, TSMP_EV_ERROR, 15000, 150), "Error(150)");
    api.close(p2);

    // 4. The anchored window: clipped by its region; a hidden one stays hidden over an owner minimize and restore.
    const RECT r3{200, 200, 200 + 362, 200 + 204};
    const RECT c3{200, 200 + 102, 200 + 362, 200 + 204}; // its lower half visible
    const int  p3 = openPlayer(owner, false, r3, c3, testPage("M7lc1UVf-VE"));
    check(waitFor(p3, TSMP_EV_CREATED, 20000), "anchored Created");
    api.place(p3, r3, c3, 1.0, 1);
    pump(300, nullptr);
    const std::vector<HWND> anchored = playersOf(owner);
    if (!anchored.empty()) {
        RECT      box{};
        const int kind = GetWindowRgnBox(anchored[0], &box);
        check(kind == SIMPLEREGION && box.top == 102 && box.bottom == 204 && box.left == 0 && box.right == 362, "the window region is the clip");
        const LONG_PTR ex = GetWindowLongPtrW(anchored[0], GWL_EXSTYLE);
        check((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_NOACTIVATE), "anchored: a tool window that never activates");
        api.place(p3, r3, r3, 1.0, 1);
        pump(200, nullptr);
        check(GetWindowRgnBox(anchored[0], &box) == ERROR, "fully visible: no region");
        api.place(p3, r3, c3, 1.0, 0); // hidden (scrolled away)
        pump(200, nullptr);
        ShowWindow(owner, SW_MINIMIZE);
        pump(600, nullptr);
        ShowWindow(owner, SW_RESTORE);
        pump(800, nullptr);
        check(!IsWindowVisible(anchored[0]), "owner restored: a hidden player stays hidden (SW_PARENTOPENING refused)");
        api.place(p3, r3, r3, 1.0, 1);
        pump(200, nullptr);
        ShowWindow(owner, SW_MINIMIZE);
        pump(600, nullptr);
        ShowWindow(owner, SW_RESTORE);
        pump(800, nullptr);
        check(IsWindowVisible(anchored[0]) != FALSE, "owner restored: a shown player is back");
    } else {
        check(false, "anchored window found");
    }

    // 5. The owner destroyed: Lost, no crash.
    DestroyWindow(owner);
    check(waitFor(p3, TSMP_EV_LOST, 3000), "owner destroyed: Lost");

    // 6. Shutdown while the environment is still being created; then init again on the pinned image.
    api.shutdown();
    check(initHelper(TSMP_TEST_FAKE_API | TSMP_TEST_MUTED), "init again (a plugin reload)");
    owner        = makeOwner();
    const int p4 = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE"));
    api.shutdown(); // right away: the environment's completion comes after this
    pump(4000, nullptr);
    check(playersOf(owner).empty(), "shutdown during creation: no window left, no crash");
    check(!has(p4, TSMP_EV_CREATED), "shutdown during creation: nothing reported afterwards");
    check(initHelper(TSMP_TEST_FAKE_API | TSMP_TEST_MUTED), "init again after that");
    const int p5 = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE"));
    check(waitFor(p5, TSMP_EV_CREATED, 30000), "a player after the reload is Created");
    api.close(p5);

    // 7. Release: EngineExited, the processes go; an open right after a release waits for it, then is created.
    api.releaseEngine();
    check(waitFor(0, TSMP_EV_ENGINEEXITED, 15000), "releaseEngine: EngineExited");
    check(pump(5000, [baseline] { return countWebViewProcesses() <= baseline; }), "WebView2's processes exited within 5 s");
    const int p6 = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE"));
    check(waitFor(p6, TSMP_EV_CREATED, 30000), "open after a release: Created");
    api.close(p6);
    events.clear();
    api.releaseEngine();
    const int p7 = openPlayer(owner, true, r1, r1, testPage("M7lc1UVf-VE")); // during the pending release
    check(waitFor(0, TSMP_EV_ENGINEEXITED, 15000), "the pending release ends (EngineExited)");
    check(waitFor(p7, TSMP_EV_CREATED, 30000), "the queued open is created after it");
    api.close(p7);
    events.clear();
    api.releaseEngine();
    waitFor(0, TSMP_EV_ENGINEEXITED, 15000);
    api.shutdown();
    DestroyWindow(owner);
    pump(500, nullptr);
    return failures ? 1 : 0;
}

int countOf(int player, int kind, const wchar_t* text)
{
    int n = 0;
    for (const TsmpEvent& e : events) {
        if (e.player == player && e.kind == kind && (!text || wcsstr(e.text, text)))
            ++n;
    }
    return n;
}

// One real player for ms, muted; its id.
int playFor(HWND owner, const QString& id, DWORD ms)
{
    const RECT r{140, 140, 140 + 640, 140 + 360};
    const int  p = openPlayer(owner, true, r, r, ytpage::pageHtml(id, 0, true), L"player_smoke: YouTube (muted)");
    check(waitFor(p, TSMP_EV_CREATED, 30000), "Created");
    api.place(p, r, r, 1.0, 1);
    const bool ready = waitFor(p, TSMP_EV_READY, 20000);
    check(ready || has(p, TSMP_EV_ERROR), "Ready (or an Error)");
    if (ready) {
        api.command(p, L"mute");
        api.command(p, L"volume:0");
    }
    pump(ms, nullptr); // the real player, muted
    check(has(p, TSMP_EV_STATE, 1) || has(p, TSMP_EV_ERROR), "State(1) playing (or an Error)");
    check(has(p, TSMP_EV_TIME) || has(p, TSMP_EV_ERROR), "Time reported by the page");
    api.close(p);
    return p;
}

QString recordLine(const proxy::Record& r)
{
    return QString::fromLatin1("  record %1%2 host=%3 step=%4 failure=%5 code=%6 up=%7 down=%8")
        .arg(QChar::fromLatin1(r.client ? r.client : '-'))
        .arg(r.id)
        .arg(r.host.isEmpty() ? QString::fromLatin1("-") : r.host)
        .arg(QString::fromLatin1(proxy::stepName(r.step)), QString::fromLatin1(proxy::failureName(r.failure)))
        .arg(r.code)
        .arg(r.up)
        .arg(r.down);
}

int youtubeRun(const QString& id, const QString& bridgeType, const QString& bridgeTarget)
{
    if (!ytpage::validId(id)) {
        std::printf("not a video id\n");
        return 1;
    }
    check(initHelper(TSMP_TEST_MUTED, bridgeType.isEmpty()), "tsmp_init (muted)");
    std::shared_ptr<proxy::Bridge> bridge;
    if (!bridgeType.isEmpty()) {
        // 2.6 (the F3 check): the plugin's own bridge to the given proxy, the helper told to use it.
        const int colon = bridgeTarget.lastIndexOf(QLatin1Char(':'));
        auto      spec  = std::make_unique<proxy::UpstreamSpec>();
        spec->type       = bridgeType == QLatin1String("http") ? proxy::Type::Http : proxy::Type::Socks5;
        spec->host       = proxy::normalizeHost(bridgeTarget.left(colon));
        spec->port       = static_cast<quint16>(bridgeTarget.mid(colon + 1).toUInt());
        spec->generation = 1;
        int error        = 0;
        bridge           = proxy::Bridge::create(proxy::Bridge::Options(), &error);
        if (!bridge || spec->host.isEmpty() || !spec->port) {
            std::printf("the bridge couldn't start (%d) or the proxy is wrong\n", error);
            return 2;
        }
        bridge->setUpstream(std::move(spec));
        const std::wstring realm = QString::fromLatin1(bridge->realm()).toStdWString();
        const std::wstring token = QString::fromLatin1(bridge->token()).toStdWString();
        check(setNetwork(TSMP_NET_BRIDGE, bridge->port(), realm.c_str(), L"tsm-p", token.c_str()) == 0, "the bridge as the network");
        std::printf("      bridge on 127.0.0.1:%u, realm tsmedia-<16 hex>\n", unsigned(bridge->port()));
    }
    HWND      owner = makeOwner();
    const int p1    = playFor(owner, id, 10000);
    int       p2    = 0;
    if (bridge) {
        p2 = playFor(owner, id, 6000); // a second player in the same environment
        std::printf("      login requests: player 1 %d (answered %d), player 2 %d (answered %d)\n", countOf(p1, TSMP_EV_TEST, L"auth:"),
                    countOf(p1, TSMP_EV_TEST, L"auth:answered"), countOf(p2, TSMP_EV_TEST, L"auth:"), countOf(p2, TSMP_EV_TEST, L"auth:answered"));
        for (const TsmpEvent& e : events) {
            if (e.kind == TSMP_EV_TEST && wcsncmp(e.text, L"auth:", 5) == 0)
                std::printf("      player %d %ls\n", e.player, e.text);
        }
        int hosts = 0, failed = 0;
        for (const proxy::Record& r : bridge->recent('p', 0)) {
            ++hosts;
            failed += r.failure != proxy::Failure::None ? 1 : 0;
            std::printf("%s\n", recordLine(r).toUtf8().constData());
        }
        for (const proxy::Record& r : bridge->recent(0, 0))
            std::printf("%s (not authenticated)\n", recordLine(r).toUtf8().constData());
        const proxy::Bridge::Counters c = bridge->counters();
        std::printf("      bridge: accepted %llu, refused %llu, busy %llu, unauthenticated %llu; player tunnels %d, failed %d\n",
                    static_cast<unsigned long long>(c.accepted), static_cast<unsigned long long>(c.refused), static_cast<unsigned long long>(c.busy),
                    static_cast<unsigned long long>(c.unauthenticated), hosts, failed);
        check(hosts > 0, "the player's requests went through the bridge");
    }
    api.releaseEngine();
    waitFor(0, TSMP_EV_ENGINEEXITED, 15000);
    api.shutdown();
    DestroyWindow(owner);
    if (bridge)
        bridge->stop(2000);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); // only for ytpage (QString); no Qt event loop runs
    wchar_t          exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir          = exe;
    dir                       = dir.substr(0, dir.find_last_of(L'\\'));
    dataDir                   = dir + L"\\player_smoke_data";
    const std::wstring helper = dir + L"\\" TSMP_WIDE(TSMP_HELPER_FILE);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("no STA\n");
        return 2;
    }
    HMODULE module = LoadLibraryExW(helper.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE pinned = nullptr;
    if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, helper.c_str(), &pinned)) {
        std::printf("can't load %ls (%lu)\n", helper.c_str(), GetLastError());
        return 2;
    }
    api.apiVersion     = reinterpret_cast<tsmp_apiVersion_fn>(GetProcAddress(module, "tsmp_apiVersion"));
    api.runtimeVersion = reinterpret_cast<tsmp_runtimeVersion_fn>(GetProcAddress(module, "tsmp_runtimeVersion"));
    api.init           = reinterpret_cast<tsmp_init_fn>(GetProcAddress(module, "tsmp_init"));
    api.open           = reinterpret_cast<tsmp_open_fn>(GetProcAddress(module, "tsmp_open"));
    api.place          = reinterpret_cast<tsmp_place_fn>(GetProcAddress(module, "tsmp_place"));
    api.command        = reinterpret_cast<tsmp_command_fn>(GetProcAddress(module, "tsmp_command"));
    api.close          = reinterpret_cast<tsmp_close_fn>(GetProcAddress(module, "tsmp_close"));
    api.poll           = reinterpret_cast<tsmp_poll_fn>(GetProcAddress(module, "tsmp_poll"));
    api.releaseEngine  = reinterpret_cast<tsmp_releaseEngine_fn>(GetProcAddress(module, "tsmp_releaseEngine"));
    api.shutdown       = reinterpret_cast<tsmp_shutdown_fn>(GetProcAddress(module, "tsmp_shutdown"));
    api.setNetwork     = reinterpret_cast<tsmp_setNetwork_fn>(GetProcAddress(module, "tsmp_setNetwork"));
    if (!api.apiVersion || !api.runtimeVersion || !api.init || !api.open || !api.place || !api.command || !api.close || !api.poll || !api.releaseEngine
        || !api.shutdown || !api.setNetwork) {
        std::printf("missing exports\n");
        return 2;
    }
    check(api.apiVersion() == TSMP_API_VERSION, "tsmp_apiVersion");
    wchar_t   version[128] = {};
    const int rv           = api.runtimeVersion(version, 128);
    std::printf("      WebView2 Runtime: %ls (%d)\n", version, rv);
    check(rv == 0, "tsmp_runtimeVersion");
    int rc = 0;
    if (argc >= 6 && std::string(argv[1]) == "--youtube" && std::string(argv[3]) == "--bridge")
        rc = youtubeRun(QString::fromLatin1(argv[2]), QString::fromLatin1(argv[4]), QString::fromLatin1(argv[5]));
    else if (argc >= 3 && std::string(argv[1]) == "--youtube")
        rc = youtubeRun(QString::fromLatin1(argv[2]), QString(), QString());
    else
        rc = offlineRun();
    std::printf("%s (%d failed, %d skipped)\n", rc == 0 ? "ALL PASSED" : "FAILED", failures, skips);
    CoUninitialize();
    return rc;
}
