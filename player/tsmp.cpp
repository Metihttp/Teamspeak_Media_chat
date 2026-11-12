// 2.5 YouTube player: the player helper (tsmedia_player_<arch>.dll), design2.md 3.2-3.6 and 3.12.
//
// Pure Win32 + COM + the WebView2 SDK (WebView2LoaderStatic.lib), static CRT, no Qt. The plugin embeds it, extracts
// it content-addressed and loads it only inside Play, then pins it: its code stays mapped until TeamSpeak exits, so
// no WebView2 completion or event handler (COM objects implemented here) can ever run unmapped code, while the plugin
// DLL stays freely unloadable. The helper never calls the plugin: the plugin polls tsmp_poll.
//
// One window per player, owned by TeamSpeak's top-level window (never a child of TeamSpeak's widgets, which are never
// touched): the popup (a titled window without a minimize box) or the anchored window (a WS_POPUP tool window over the
// card's thumbnail, clipped with a window region). The WebView2 controller is its only child.
//
// Rules: every event handler added is removed by its token at close; every SetWindowPos carries SWP_NOZORDER (a move
// never lifts the player above TeamSpeak's menus); no thread, no hook and no timer of ours; after tsmp_shutdown the
// helper holds no object, window or class: an inert image.

#define TSMP_BUILDING_HELPER
#include "tsmp.h"
#include "tsmpmsg.h"

#include <objbase.h>
#include <shlwapi.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

// ---- state (tsmp_init resets it all) ----------------------------------------------------------------------------

struct Player {
    int                            id         = 0;
    int                            generation = 0;
    HWND                           hwnd       = nullptr;
    bool                           popup      = true;
    bool                           closing    = false; // tsmp_close / tsmp_shutdown / the close box started the destroy
    bool                           wantVisible = false; // the last tsmp_place said visible
    bool                           shownOnce  = false;
    bool                           navigated  = false; // our page's navigation started (the only main-frame one)
    bool                           created    = false;
    bool                           viewMuted  = true; // created muted until the plugin's volume (or an unmute) arrives
    RECT                           rect{};
    RECT                           clip{};
    double                         css = 1.0;
    std::wstring                   page;
    std::wstring                   pageUri;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2>           webview;
    ComPtr<ICoreWebView2_8>         webview8; // IsMuted
    EventRegistrationToken tNav{}, tFrameNav{}, tNavDone{}, tNewWindow{}, tDownload{}, tPermission{}, tAuth{}, tMessage{}, tFailed{},
        tResource{}, tKey{};
};

int                                g_generation = 0;
bool                               g_ready      = false; // tsmp_init succeeded
int                                g_testMode   = 0;
std::wstring                       g_userData, g_origin, g_language;
std::wstring                       g_className;
ATOM                               g_class      = 0;
int                                g_nextId     = 0;
std::map<int, Player*>             g_players;
std::deque<TsmpEvent>              g_events;
// The environment: created at the first open, released by tsmp_releaseEngine.
ComPtr<ICoreWebView2Environment>   g_env;
EventRegistrationToken             g_envExitToken{};
bool                               g_envExitHooked = false;
bool                               g_envExited     = false; // its browser process exited by itself (no webview left)
bool                               g_envCreating   = false;
bool                               g_releaseOnCreate = false; // tsmp_releaseEngine during a creation: dropped when it completes
std::vector<int>                   g_waiting;              // players waiting for the environment
// A released environment whose browser process hasn't exited yet (a new one waits for it, at most 10 s).
ComPtr<ICoreWebView2Environment>   g_released;
EventRegistrationToken             g_releasedToken{};
ULONGLONG                          g_waitDeadline = 0;

constexpr size_t kMaxEvents = 128;
constexpr int    kMinWidthDu = 356, kMinHeightDu = 200;

void post(int player, int kind, int value, const wchar_t* text = nullptr)
{
    if (g_events.size() >= kMaxEvents)
        g_events.pop_front(); // the oldest goes (the plugin polls every 50 ms)
    TsmpEvent e{};
    e.player = player;
    e.kind   = kind;
    e.value  = value;
    if (text)
        wcsncpy_s(e.text, 2049, text, _TRUNCATE);
    g_events.push_back(e);
}

Player* find(int id)
{
    const auto it = g_players.find(id);
    return it == g_players.end() ? nullptr : it->second;
}

bool startsWith(const wchar_t* s, const std::wstring& prefix)
{
    return s && wcsncmp(s, prefix.c_str(), prefix.size()) == 0;
}

bool isHttps(const wchar_t* s)
{
    return s && _wcsnicmp(s, L"https://", 8) == 0;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

// ---- the fake IFrame API of the smoke test (testMode only) --------------------------------------------------------

const char* kFakeApi =
    "(function () {\n"
    "  function tellTest(s) { window.chrome.webview.postMessage('test:' + s); }\n"
    "  function Player(id, o) {\n"
    "    var self = this, st = -1, t = 0;\n"
    "    var ev = (o && o.events) || {};\n"
    "    var src = (document.getElementById(id) || {}).src || '';\n"
    "    self.getPlayerState = function () { return st; };\n"
    "    self.getCurrentTime = function () { return t; };\n"
    "    self.pauseVideo = function () { st = 2; tellTest('pause'); if (ev.onStateChange) ev.onStateChange({ data: 2 }); };\n"
    "    self.playVideo = function () { st = 1; tellTest('play'); if (ev.onStateChange) ev.onStateChange({ data: 1 }); };\n"
    "    self.mute = function () { tellTest('mute'); };\n"
    "    self.unMute = function () { tellTest('unmute'); };\n"
    "    self.setVolume = function (v) { tellTest('volume:' + v); };\n"
    "    setTimeout(function () {\n"
    "      if (src.indexOf('/embed/AAAAAAAAAAA') >= 0) { if (ev.onError) ev.onError({ data: 150 }); return; }\n"
    "      if (ev.onReady) ev.onReady({});\n"
    "      t = 7; st = 1; if (ev.onStateChange) ev.onStateChange({ data: 1 });\n"
    "    }, 150);\n"
    "  }\n"
    "  window.YT = { Player: Player };\n"
    "  if (typeof onYouTubeIframeAPIReady === 'function') onYouTubeIframeAPIReady();\n"
    "})();\n";

// ---- responses from memory ----------------------------------------------------------------------------------------

ComPtr<ICoreWebView2WebResourceResponse> respond(int status, const wchar_t* reason, const std::string& body, const wchar_t* type)
{
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (!g_env)
        return response;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()), static_cast<UINT>(body.size())));
    std::wstring headers = std::wstring(L"Content-Type: ") + type + L"\r\nCache-Control: no-store\r\nReferrer-Policy: strict-origin-when-cross-origin";
    g_env->CreateWebResourceResponse(stream.Get(), status, reason, headers.c_str(), &response);
    return response;
}

// ---- windows ------------------------------------------------------------------------------------------------------

void applyRegion(Player* p)
{
    if (!p->hwnd || p->popup)
        return;
    const LONG w = p->rect.right - p->rect.left, h = p->rect.bottom - p->rect.top;
    RECT       c = p->clip;
    OffsetRect(&c, -p->rect.left, -p->rect.top);
    if (c.left <= 0 && c.top <= 0 && c.right >= w && c.bottom >= h) {
        SetWindowRgn(p->hwnd, nullptr, TRUE);
        return;
    }
    HRGN region = CreateRectRgn(c.left, c.top, c.right, c.bottom);
    if (region && !SetWindowRgn(p->hwnd, region, TRUE)) // the system owns the region now (on success)
        DeleteObject(region);
}

void applyZoom(Player* p)
{
    if (!p->controller)
        return;
    double              raster = 1.0;
    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(p->controller.As(&c3)) && c3)
        c3->get_RasterizationScale(&raster);
    if (raster <= 0)
        raster = 1.0;
    double zoom = p->css / raster;
    if (zoom < 0.25)
        zoom = 0.25;
    if (zoom > 5.0)
        zoom = 5.0;
    p->controller->put_ZoomFactor(zoom);
}

void fitController(Player* p)
{
    if (!p->controller || !p->hwnd)
        return;
    RECT client{};
    GetClientRect(p->hwnd, &client);
    p->controller->put_Bounds(client);
}

void removeHandlers(Player* p)
{
    if (p->webview) {
        p->webview->remove_NavigationStarting(p->tNav);
        p->webview->remove_FrameNavigationStarting(p->tFrameNav);
        p->webview->remove_NavigationCompleted(p->tNavDone);
        p->webview->remove_NewWindowRequested(p->tNewWindow);
        p->webview->remove_PermissionRequested(p->tPermission);
        p->webview->remove_WebMessageReceived(p->tMessage);
        p->webview->remove_ProcessFailed(p->tFailed);
        p->webview->remove_WebResourceRequested(p->tResource);
        ComPtr<ICoreWebView2_4> w4;
        if (SUCCEEDED(p->webview.As(&w4)) && w4)
            w4->remove_DownloadStarting(p->tDownload);
        ComPtr<ICoreWebView2_10> w10;
        if (SUCCEEDED(p->webview.As(&w10)) && w10)
            w10->remove_BasicAuthenticationRequested(p->tAuth);
    }
    if (p->controller)
        p->controller->remove_AcceleratorKeyPressed(p->tKey);
}

// The controller and its handlers go; the window stays (the caller destroys it, or it is being destroyed).
void releaseController(Player* p)
{
    removeHandlers(p);
    if (p->controller)
        p->controller->Close();
    p->webview8.Reset();
    p->webview.Reset();
    p->controller.Reset();
}

// Everything of p goes (tsmp_close, the close box, tsmp_shutdown). p is deleted.
void destroyPlayer(Player* p)
{
    p->closing = true;
    releaseController(p);
    g_players.erase(p->id);
    for (size_t i = 0; i < g_waiting.size();) {
        if (g_waiting[i] == p->id)
            g_waiting.erase(g_waiting.begin() + static_cast<long long>(i));
        else
            ++i;
    }
    HWND hwnd = p->hwnd;
    p->hwnd   = nullptr;
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
    delete p;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Player* p = find(static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    if (p && p->hwnd != hwnd)
        p = nullptr;
    switch (msg) {
    case WM_SIZE:
        if (p)
            fitController(p);
        break;
    case WM_MOVE:
        if (p && p->controller)
            p->controller->NotifyParentWindowPositionChanged();
        break;
    case WM_GETMINMAXINFO:
        if (p && p->popup) {
            // At least 356 x 200 du of client area (YouTube's player needs 200 x 200 CSS px).
            RECT r{0, 0, static_cast<LONG>(kMinWidthDu * p->css), static_cast<LONG>(kMinHeightDu * p->css)};
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
            auto* info              = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x  = r.right - r.left;
            info->ptMinTrackSize.y  = r.bottom - r.top;
            return 0;
        }
        break;
    case WM_SHOWWINDOW:
        // The owner restored: Windows shows its owned windows again. A hidden player stays hidden (never at a stale place).
        if (wp && lp == SW_PARENTOPENING && p && !p->wantVisible)
            return 0;
        break;
    case WM_SETFOCUS:
    case WM_ACTIVATE:
        if (p && p->controller && (msg == WM_SETFOCUS || LOWORD(wp) != WA_INACTIVE))
            p->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
    case WM_DPICHANGED:
        if (p && p->popup) {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (p) {
            // The popup's close box: its client size goes to the plugin, then everything goes.
            RECT client{};
            GetClientRect(hwnd, &client);
            wchar_t size[32];
            swprintf_s(size, L"%ldx%ld", client.right - client.left, client.bottom - client.top);
            const int id = p->id;
            destroyPlayer(p);
            post(id, TSMP_EV_CLOSEDBYUSER, 0, size);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (p && !p->closing) {
            // Not ours: the owner went (TeamSpeak re-created its main window). The controller closes, the plugin hears Lost.
            p->closing = true;
            releaseController(p);
            const int id = p->id;
            p->hwnd      = nullptr;
            g_players.erase(id);
            delete p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            post(id, TSMP_EV_LOST, 0);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool registerClass()
{
    if (g_class)
        return true;
    // The class name carries this image's own file name (its content hash): two helper images never share a class.
    wchar_t path[MAX_PATH] = {};
    HMODULE self           = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&windowProc), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    g_className         = std::wstring(L"TSMediaPlayer_") + (base ? base + 1 : L"helper");
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = self;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = g_className.c_str();
    g_class          = RegisterClassExW(&wc);
    return g_class != 0;
}

void unregisterClass()
{
    if (!g_class)
        return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&windowProc), &self);
    UnregisterClassW(g_className.c_str(), self);
    g_class = 0;
}

// ---- the controller ----------------------------------------------------------------------------------------------

void addHandlers(Player* p)
{
    const int id  = p->id;
    const int gen = g_generation;
    ICoreWebView2* w = p->webview.Get();

    // The main frame: only our page, once. Anything else is cancelled; a link the user clicked opens in the browser.
    w->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            Player* q = gen == g_generation ? find(id) : nullptr;
            LPWSTR  uri = nullptr;
            args->get_Uri(&uri);
            const bool ours = q && !q->navigated && uri && q->pageUri == uri;
            if (ours) {
                q->navigated = true;
            } else {
                args->put_Cancel(TRUE);
                BOOL user = FALSE;
                args->get_IsUserInitiated(&user);
                if (q && user && isHttps(uri))
                    post(id, TSMP_EV_OPENURL, 0, uri);
            }
            if (uri)
                CoTaskMemFree(uri);
            return S_OK;
        }).Get(),
        &p->tNav);
    w->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            if (!isHttps(uri) && !(uri && wcscmp(uri, L"about:blank") == 0))
                args->put_Cancel(TRUE); // frames: https only
            if (uri)
                CoTaskMemFree(uri);
            return S_OK;
        }).Get(),
        &p->tFrameNav);
    w->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            Player* q = gen == g_generation ? find(id) : nullptr;
            BOOL    ok = TRUE;
            args->get_IsSuccess(&ok);
            if (q && !ok && q->navigated) {
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_WebErrorStatus(&status);
                if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
                    post(id, TSMP_EV_FAILED, static_cast<int>(status), L"navigation");
            }
            return S_OK;
        }).Get(),
        &p->tNavDone);
    // Player links must work (YouTube's RMF): a new window the user asked for opens in the browser; never a window here.
    w->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            LPWSTR uri  = nullptr;
            BOOL   user = FALSE;
            args->get_Uri(&uri);
            args->get_IsUserInitiated(&user);
            if (gen == g_generation && find(id) && user && isHttps(uri))
                post(id, TSMP_EV_OPENURL, 0, uri);
            if (uri)
                CoTaskMemFree(uri);
            return S_OK;
        }).Get(),
        &p->tNewWindow);
    w->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                   args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                   return S_OK;
                               }).Get(),
                               &p->tPermission);
    w->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            Player* q = gen == g_generation ? find(id) : nullptr;
            if (!q)
                return S_OK;
            LPWSTR source = nullptr;
            args->get_Source(&source);
            const bool fromPage = source && q->pageUri == source;
            if (source)
                CoTaskMemFree(source);
            LPWSTR text = nullptr;
            if (!fromPage || FAILED(args->TryGetWebMessageAsString(&text)) || !text)
                return S_OK;
            const size_t           n = wcslen(text);
            const tsmpmsg::Message m = tsmpmsg::parse(text, n, (g_testMode & TSMP_TEST_FAKE_API) != 0);
            switch (m.kind) {
            case tsmpmsg::Kind::Ready:
                post(id, TSMP_EV_READY, 0);
                break;
            case tsmpmsg::Kind::State:
                post(id, TSMP_EV_STATE, m.value);
                break;
            case tsmpmsg::Kind::Error:
                post(id, TSMP_EV_ERROR, m.value);
                break;
            case tsmpmsg::Kind::Time:
                post(id, TSMP_EV_TIME, m.value);
                break;
            case tsmpmsg::Kind::Test: {
                std::wstring t = L"page:";
                t += text + 5;
                post(id, TSMP_EV_TEST, 0, t.c_str());
                break;
            }
            case tsmpmsg::Kind::None:
                break; // anything else is dropped
            }
            CoTaskMemFree(text);
            return S_OK;
        }).Get(),
        &p->tMessage);
    w->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                             COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                             args->get_ProcessFailedKind(&kind);
                             const bool fatal = kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED
                                                || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
                                                || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE
                                                || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED;
                             if (fatal && gen == g_generation && find(id))
                                 post(id, TSMP_EV_FAILED, static_cast<int>(kind), L"process");
                             return S_OK;
                         }).Get(),
                         &p->tFailed);
    // Our origin is answered from memory (never looked up): the page, the smoke test's fake API, 404 for the rest.
    w->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>([id, gen](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            Player* q = gen == g_generation ? find(id) : nullptr;
            if (!q)
                return S_OK;
            ComPtr<ICoreWebView2WebResourceRequest> request;
            if (FAILED(args->get_Request(&request)) || !request)
                return S_OK;
            LPWSTR uri = nullptr;
            request->get_Uri(&uri);
            std::wstring u = uri ? uri : L"";
            if (uri)
                CoTaskMemFree(uri);
            ComPtr<ICoreWebView2WebResourceResponse> response;
            if (startsWith(u.c_str(), g_origin + L"/")) {
                if (u == q->pageUri)
                    response = respond(200, L"OK", utf8(q->page), L"text/html; charset=utf-8");
                else if ((g_testMode & TSMP_TEST_FAKE_API) && u == g_origin + L"/test_api.js")
                    response = respond(200, L"OK", kFakeApi, L"text/javascript; charset=utf-8");
                else
                    response = respond(404, L"Not Found", "", L"text/plain; charset=utf-8");
            } else if (g_testMode & TSMP_TEST_FAKE_API) {
                // The smoke test: the embed request's Referer is recorded, never sent (a blank page answers it).
                ComPtr<ICoreWebView2HttpRequestHeaders> headers;
                LPWSTR                                  referer = nullptr;
                if (SUCCEEDED(request->get_Headers(&headers)) && headers)
                    headers->GetHeader(L"Referer", &referer);
                std::wstring t = L"referer:";
                t += referer ? referer : L"";
                if (referer)
                    CoTaskMemFree(referer);
                post(id, TSMP_EV_TEST, 0, t.c_str());
                post(id, TSMP_EV_TEST, 0, (L"request:" + u).c_str());
                response = respond(200, L"OK", "<!doctype html><title>embed</title>", L"text/html; charset=utf-8");
            }
            if (response)
                args->put_Response(response.Get());
            return S_OK;
        }).Get(),
        &p->tResource);
    ComPtr<ICoreWebView2_4> w4;
    if (SUCCEEDED(p->webview.As(&w4)) && w4) {
        w4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                                     args->put_Cancel(TRUE);
                                     return S_OK;
                                 }).Get(),
                                 &p->tDownload);
    }
    ComPtr<ICoreWebView2_10> w10;
    if (SUCCEEDED(p->webview.As(&w10)) && w10) {
        w10->add_BasicAuthenticationRequested(
            Callback<ICoreWebView2BasicAuthenticationRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2BasicAuthenticationRequestedEventArgs* args) -> HRESULT {
                args->put_Cancel(TRUE);
                return S_OK;
            }).Get(),
            &p->tAuth);
    }
    // Esc while the page has focus (the popup closes).
    p->controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>([id, gen](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP;
            UINT                        key  = 0;
            COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
            args->get_KeyEventKind(&kind);
            args->get_VirtualKey(&key);
            args->get_PhysicalKeyStatus(&status);
            if (key == VK_ESCAPE && (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) && !status.WasKeyDown) {
                args->put_Handled(TRUE);
                if (gen == g_generation && find(id))
                    post(id, TSMP_EV_KEY, VK_ESCAPE);
            }
            return S_OK;
        }).Get(),
        &p->tKey);
}

// The settings of 3.6: every one of them is required (controllerReady checked each interface up front), so the
// player never runs with SmartScreen, autofill or the browser's keys left on (review fix).
struct Settings {
    ComPtr<ICoreWebView2Settings>  s;
    ComPtr<ICoreWebView2Settings3> s3;
    ComPtr<ICoreWebView2Settings4> s4;
    ComPtr<ICoreWebView2Settings5> s5;
    ComPtr<ICoreWebView2Settings6> s6;
    ComPtr<ICoreWebView2Settings8> s8;
};

bool querySettings(Player* p, Settings* out)
{
    return SUCCEEDED(p->webview->get_Settings(&out->s)) && out->s && SUCCEEDED(out->s.As(&out->s3)) && out->s3 && SUCCEEDED(out->s.As(&out->s4))
           && out->s4 && SUCCEEDED(out->s.As(&out->s5)) && out->s5 && SUCCEEDED(out->s.As(&out->s6)) && out->s6 && SUCCEEDED(out->s.As(&out->s8))
           && out->s8;
}

void applySettings(const Settings& all)
{
    ICoreWebView2Settings* s = all.s.Get();
    s->put_IsScriptEnabled(TRUE);
    s->put_IsWebMessageEnabled(TRUE);
    s->put_AreDefaultScriptDialogsEnabled(FALSE);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_AreDevToolsEnabled(FALSE);
    s->put_AreDefaultContextMenusEnabled(FALSE); // YouTube's own menu stays
    s->put_AreHostObjectsAllowed(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
    s->put_IsBuiltInErrorPageEnabled(FALSE);
    all.s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    all.s4->put_IsPasswordAutosaveEnabled(FALSE);
    all.s4->put_IsGeneralAutofillEnabled(FALSE);
    all.s5->put_IsPinchZoomEnabled(FALSE);
    all.s6->put_IsSwipeNavigationEnabled(FALSE);
    all.s8->put_IsReputationCheckingRequired(FALSE); // SmartScreen would send the pages to Microsoft
}

void controllerReady(Player* p, ICoreWebView2Controller* controller)
{
    p->controller = controller;
    if (FAILED(controller->get_CoreWebView2(&p->webview)) || !p->webview) {
        p->controller.Reset();
        post(p->id, TSMP_EV_FAILED, E_NOINTERFACE, L"controller");
        return;
    }
    // Every interface this needs, or the browser (a runtime too old for one of them, design2 3.6): the events, the
    // monitor scale, the mute, and every setting of 3.6 (SmartScreen's included).
    ComPtr<ICoreWebView2_10> w10;
    ComPtr<ICoreWebView2Controller3> c3;
    Settings settings;
    if (FAILED(p->webview.As(&w10)) || FAILED(p->controller.As(&c3)) || FAILED(p->webview.As(&p->webview8)) || !p->webview8
        || !querySettings(p, &settings)) {
        releaseController(p);
        post(p->id, TSMP_EV_FAILED, E_NOINTERFACE, L"controller");
        return;
    }
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(p->controller.As(&c2)) && c2)
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0, 0, 0});
    c3->put_ShouldDetectMonitorScaleChanges(TRUE);
    // Muted from the start: the page autoplays before the plugin's volume (and its mute) can reach it, and nothing may
    // start at YouTube's default full volume. The plugin's volume command lifts it (tsmp_command); in the smoke test's
    // muted mode it never is.
    p->webview8->put_IsMuted(TRUE);
    p->viewMuted = true;
    applySettings(settings);
    addHandlers(p);
    p->webview->AddWebResourceRequestedFilter((g_origin + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    if (g_testMode & TSMP_TEST_FAKE_API)
        p->webview->AddWebResourceRequestedFilter(L"https://www.youtube-nocookie.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    fitController(p);
    applyZoom(p);
    p->controller->put_IsVisible(TRUE);
    p->created = true;
    p->webview->Navigate(p->pageUri.c_str());
    post(p->id, TSMP_EV_CREATED, 0);
}

void createController(Player* p)
{
    ComPtr<ICoreWebView2Environment10> env10;
    if (!g_env || FAILED(g_env.As(&env10)) || !env10) {
        post(p->id, TSMP_EV_FAILED, E_NOINTERFACE, L"controller");
        return;
    }
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (FAILED(env10->CreateCoreWebView2ControllerOptions(&options)) || !options) {
        post(p->id, TSMP_EV_FAILED, E_FAIL, L"controller");
        return;
    }
    options->put_ProfileName(L"tsmedia-player");
    options->put_IsInPrivateModeEnabled(TRUE);
    const int  id  = p->id;
    const int  gen = g_generation;
    const HRESULT hr = env10->CreateCoreWebView2ControllerWithOptions(
        p->hwnd, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([id, gen](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            Player* q = gen == g_generation ? find(id) : nullptr;
            if (!q || !q->hwnd) {
                // Closed (or the helper shut down) meanwhile: what we got goes at once.
                if (controller)
                    controller->Close();
                return S_OK;
            }
            if (FAILED(result) || !controller) {
                post(id, TSMP_EV_FAILED, static_cast<int>(result), L"controller");
                return S_OK;
            }
            controllerReady(q, controller);
            return S_OK;
        }).Get());
    if (FAILED(hr))
        post(id, TSMP_EV_FAILED, static_cast<int>(hr), L"controller");
}

void onEnvironmentExited(ICoreWebView2Environment* sender);

void hookExit(ICoreWebView2Environment* env, EventRegistrationToken* token)
{
    ComPtr<ICoreWebView2Environment5> env5;
    if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env5))) || !env5)
        return;
    const int gen = g_generation;
    env5->add_BrowserProcessExited(Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                                       [gen](ICoreWebView2Environment* sender, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
                                           if (gen == g_generation)
                                               onEnvironmentExited(sender);
                                           return S_OK;
                                       })
                                       .Get(),
                                   token);
}

void unhookExit(ICoreWebView2Environment* env, const EventRegistrationToken& token)
{
    ComPtr<ICoreWebView2Environment5> env5;
    if (env && SUCCEEDED(env->QueryInterface(IID_PPV_ARGS(&env5))) && env5)
        env5->remove_BrowserProcessExited(token);
}

void createEnvironment();
void releaseEnvironment(bool keepForExit);

void startWaiting()
{
    // Players that waited for the environment get their controllers (in order).
    const std::vector<int> waiting = g_waiting;
    g_waiting.clear();
    for (const int id : waiting) {
        if (Player* p = find(id))
            createController(p);
    }
}

void dropReleased(bool exited)
{
    if (!g_released)
        return;
    unhookExit(g_released.Get(), g_releasedToken);
    g_released.Reset();
    g_waitDeadline = 0;
    post(0, TSMP_EV_ENGINEEXITED, exited ? 0 : 1);
}

void onEnvironmentExited(ICoreWebView2Environment* sender)
{
    if (g_released && sender == g_released.Get()) {
        dropReleased(true);
        if (!g_waiting.empty())
            createEnvironment();
        return;
    }
    if (g_env && sender == g_env.Get())
        g_envExited = true; // no webview was left: a new open makes a new environment
}

void createEnvironment()
{
    if (g_envCreating)
        return;
    if (g_released) {
        // A release is pending: the new environment waits for the old browser process (at most 10 s, tsmp_poll).
        if (!g_waitDeadline)
            g_waitDeadline = GetTickCount64() + 10000;
        return;
    }
    g_envCreating = true;
    auto options  = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");
    options->put_AllowSingleSignOnUsingOSPrimaryAccount(FALSE);
    if (!g_language.empty())
        options->put_Language(g_language.c_str());
    ComPtr<ICoreWebView2EnvironmentOptions2> options2;
    if (SUCCEEDED(options.As(&options2)) && options2)
        options2->put_ExclusiveUserDataFolderAccess(TRUE);
    const int     gen = g_generation;
    const HRESULT hr  = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, g_userData.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([gen](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (gen != g_generation)
                return S_OK; // the helper shut down meanwhile: what we got goes with this scope
            g_envCreating = false;
            // Released while it was being made (the switch went off, the engine timer): dropped at once, unless a new
            // open is waiting for it (then the release is void: EngineExited 3, the plugin stops waiting for it).
            const bool released = g_releaseOnCreate;
            const bool drop     = released && g_waiting.empty();
            g_releaseOnCreate   = false;
            if (released && !drop)
                post(0, TSMP_EV_ENGINEEXITED, 3);
            if (FAILED(result) || !env) {
                const std::vector<int> waiting = g_waiting;
                g_waiting.clear();
                for (const int id : waiting) {
                    if (find(id))
                        post(id, TSMP_EV_FAILED, static_cast<int>(result), L"environment");
                }
                if (drop)
                    post(0, TSMP_EV_ENGINEEXITED, 2); // nothing was made: nothing to wait for
                return S_OK;
            }
            g_env       = env;
            g_envExited = false;
            hookExit(env, &g_envExitToken);
            g_envExitHooked = true;
            if (drop) {
                releaseEnvironment(true); // kept only to hear its browser process exit (EngineExited)
                return S_OK;
            }
            startWaiting();
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        g_envCreating                  = false;
        const std::vector<int> waiting = g_waiting;
        g_waiting.clear();
        for (const int id : waiting) {
            if (find(id))
                post(id, TSMP_EV_FAILED, static_cast<int>(hr), L"environment");
        }
    }
}

void releaseEnvironment(bool keepForExit)
{
    if (!g_env) {
        // Nothing to release now. Still an answer for the plugin, which waits (and polls) for EngineExited: one being
        // made goes when its creation completes; none at all (it was never made, or failed): at once (review fix).
        if (keepForExit) {
            if (g_envCreating)
                g_releaseOnCreate = true;
            else
                post(0, TSMP_EV_ENGINEEXITED, 2);
        }
        return;
    }
    if (g_envExitHooked && !keepForExit)
        unhookExit(g_env.Get(), g_envExitToken);
    if (keepForExit && !g_envExited) {
        // Kept (only) to hear BrowserProcessExited: a new environment on the same folder waits for it.
        dropReleased(false);
        g_released      = g_env;
        g_releasedToken = g_envExitToken;
    } else {
        if (keepForExit && g_envExitHooked)
            unhookExit(g_env.Get(), g_envExitToken);
        if (keepForExit)
            post(0, TSMP_EV_ENGINEEXITED, 0); // already exited by itself
    }
    g_envExitHooked = false;
    g_env.Reset();
    g_envExited = false;
}

void shutdownAll()
{
    ++g_generation; // pending completions see it and drop what they get
    std::vector<Player*> all;
    for (auto& kv : g_players)
        all.push_back(kv.second);
    for (Player* p : all)
        destroyPlayer(p);
    g_players.clear();
    g_waiting.clear();
    if (g_env && g_envExitHooked)
        unhookExit(g_env.Get(), g_envExitToken);
    g_envExitHooked = false;
    g_env.Reset();
    if (g_released)
        unhookExit(g_released.Get(), g_releasedToken);
    g_released.Reset();
    g_waitDeadline    = 0;
    g_envCreating     = false;
    g_releaseOnCreate = false;
    g_envExited       = false;
    g_events.clear();
    unregisterClass();
    g_ready = false;
}

} // namespace

extern "C" {

int __cdecl tsmp_apiVersion(void)
{
    return TSMP_API_VERSION;
}

int __cdecl tsmp_runtimeVersion(wchar_t* out, int chars)
{
    if (!out || chars < 2)
        return -1;
    out[0]        = 0;
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version)
        return -2;
    wcsncpy_s(out, static_cast<size_t>(chars), version, _TRUNCATE);
    CoTaskMemFree(version);
    return 0;
}

int __cdecl tsmp_init(const TsmpInit* init)
{
    shutdownAll(); // a plugin reload calls it again: everything of before goes
    if (!init || !init->appOrigin || !init->userDataFolder || !*init->userDataFolder)
        return -1;
    // WebView2's completions arrive through this thread's message loop: it must be an STA (Qt's GUI thread is).
    APTTYPE          type      = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    if (FAILED(CoGetApartmentType(&type, &qualifier)) || (type != APTTYPE_STA && type != APTTYPE_MAINSTA))
        return -2;
    g_userData = init->userDataFolder;
    g_origin   = init->appOrigin;
    while (!g_origin.empty() && g_origin.back() == L'/')
        g_origin.pop_back();
    if (!isHttps(g_origin.c_str()))
        return -1;
    g_language = init->language ? init->language : L"";
    g_testMode = init->testMode;
    if (!registerClass())
        return -3;
    g_ready = true;
    return 0;
}

int __cdecl tsmp_open(const TsmpOpen* open)
{
    if (!g_ready || !open || !open->pageHtml || !*open->pageHtml)
        return 0;
    auto* p    = new Player();
    p->id      = ++g_nextId;
    p->generation = g_generation;
    p->popup   = open->popup != 0;
    p->rect    = open->rect;
    p->clip    = open->clip;
    p->css     = open->cssScale > 0.1 ? open->cssScale : 1.0;
    p->page    = open->pageHtml;
    p->pageUri = g_origin + L"/player";
    const DWORD style   = p->popup ? (WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX) : (WS_POPUP | WS_CLIPCHILDREN);
    const DWORD exStyle = p->popup ? 0 : (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    RECT        frame   = p->rect;
    if (p->popup) {
        AdjustWindowRectEx(&frame, style, FALSE, exStyle);
        // Within the work area of the monitor it opens on (the owner's): a large chat scale or a remembered size never
        // puts the title bar (or the window) off the screen (review fix).
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        HMONITOR on    = open->ownerHwnd ? MonitorFromWindow(static_cast<HWND>(open->ownerHwnd), MONITOR_DEFAULTTONEAREST)
                                         : MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST);
        if (on && GetMonitorInfoW(on, &monitor)) {
            tsmpmsg::Box client{p->rect.left, p->rect.top, p->rect.right, p->rect.bottom};
            tsmpmsg::Box border{p->rect.left - frame.left, p->rect.top - frame.top, frame.right - p->rect.right, frame.bottom - p->rect.bottom};
            tsmpmsg::Box work{monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom};
            const tsmpmsg::Box fitted = tsmpmsg::fitPopup(client, border, work);
            p->rect = RECT{fitted.left, fitted.top, fitted.right, fitted.bottom};
            p->clip = p->rect;
            frame   = p->rect;
            AdjustWindowRectEx(&frame, style, FALSE, exStyle);
        }
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&windowProc), &self);
    p->hwnd = CreateWindowExW(exStyle, g_className.c_str(), open->windowTitle ? open->windowTitle : L"YouTube", style, frame.left, frame.top,
                              frame.right - frame.left, frame.bottom - frame.top, static_cast<HWND>(open->ownerHwnd), nullptr, self, nullptr);
    if (!p->hwnd) {
        delete p;
        return 0;
    }
    g_players[p->id] = p;
    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, p->id);
    applyRegion(p);
    if (g_env && g_envExited)
        releaseEnvironment(false); // its browser process went by itself: a new one
    if (g_env) {
        createController(p);
    } else {
        g_waiting.push_back(p->id);
        createEnvironment();
    }
    return p->id;
}

void __cdecl tsmp_place(int id, RECT rect, RECT clip, double cssScale, int visible)
{
    Player* p = find(id);
    if (!p || !p->hwnd)
        return;
    p->wantVisible = visible != 0;
    if (cssScale > 0.1 && cssScale != p->css) {
        p->css = cssScale;
        applyZoom(p);
    }
    if (!p->popup) {
        // The anchored window: over the card's thumbnail, clipped to the chat. Never a z-order change.
        p->rect = rect;
        p->clip = clip;
        SetWindowPos(p->hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        applyRegion(p);
        if (p->controller)
            p->controller->NotifyParentWindowPositionChanged();
    }
    // The popup is the user's to move after its first show: only its visibility follows.
    if (visible) {
        if (!IsWindowVisible(p->hwnd)) {
            if (p->popup && !p->shownOnce)
                ShowWindow(p->hwnd, SW_SHOWNORMAL); // its first show activates it (Esc and YouTube's keys work)
            else
                ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
        }
        p->shownOnce = true;
    } else if (IsWindowVisible(p->hwnd)) {
        ShowWindow(p->hwnd, SW_HIDE);
    }
    if (p->controller)
        p->controller->put_IsVisible(visible ? TRUE : FALSE);
}

void __cdecl tsmp_command(int id, const wchar_t* command)
{
    Player* p = find(id);
    if (!p || !command)
        return;
    if (wcscmp(command, L"front") == 0) {
        // The popup's card was clicked: its window comes to the front (the user's own gesture).
        if (p->popup && p->hwnd) {
            if (!IsWindowVisible(p->hwnd))
                ShowWindow(p->hwnd, SW_SHOW);
            SetForegroundWindow(p->hwnd);
        }
        return;
    }
    const size_t length = wcslen(command);
    if (!tsmpmsg::validCommand(command, length) || !p->webview)
        return;
    p->webview->PostWebMessageAsString(command);
    // The view's own mute (from its creation) goes once the plugin's volume, or an unmute, is on its way to the page.
    if (p->viewMuted && p->webview8 && !(g_testMode & TSMP_TEST_MUTED) && tsmpmsg::liftsViewMute(command, length)) {
        p->webview8->put_IsMuted(FALSE);
        p->viewMuted = false;
    }
}

void __cdecl tsmp_close(int id)
{
    if (Player* p = find(id))
        destroyPlayer(p);
}

int __cdecl tsmp_poll(TsmpEvent* out, int max)
{
    // A new environment waited 10 s for the released one's browser process: it is made anyway.
    if (g_released && g_waitDeadline && GetTickCount64() > g_waitDeadline) {
        dropReleased(false);
        if (!g_waiting.empty())
            createEnvironment();
    }
    int n = 0;
    while (out && n < max && !g_events.empty()) {
        out[n++] = g_events.front();
        g_events.pop_front();
    }
    return n;
}

void __cdecl tsmp_releaseEngine(void)
{
    if (!g_players.empty())
        return; // never while a player exists
    releaseEnvironment(true);
}

void __cdecl tsmp_shutdown(void)
{
    shutdownAll();
}

} // extern "C"
