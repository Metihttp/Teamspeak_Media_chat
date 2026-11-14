#pragma once

// 2.5 YouTube player: the C API of the player helper (tsmedia_player_<arch>.dll, player/tsmp.cpp), shared with the
// plugin (src/ytplayer/ytbackend.cpp), which loads the helper with LoadLibraryExW only inside Play and resolves these
// functions with GetProcAddress. No WebView2 type appears here: the plugin never includes WebView2.h.
//
// Rules (design2.md 3.2):
//  * every call on TeamSpeak's GUI thread (an OLE STA), never re-entrant;
//  * UTF-16 strings; the helper copies every string it keeps;
//  * the helper never calls into the plugin: no callbacks, no window procedures of the plugin, no hooks. The plugin
//    polls tsmp_poll with an owned timer while a player exists, is being created or an engine release is pending;
//  * rect and clip are in screen device pixels.
//
// 2.6 (API version 2, design 3.6): tsmp_setNetwork tells the helper how the next environment's browser reaches the
// web (the Windows settings, direct, or TS Media's loopback bridge for the chat proxy). tsmp_init sets it to UNSET:
// an open without a network fails ("network") and no environment is made. The helper never releases an environment
// by itself: an open whose network would need other browser arguments than the living environment's fails
// ("network") and the plugin releases it (player/tsmpnet.h has the pure rules both sides use).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSMP_API_VERSION 2

// Event kinds (TsmpEvent::kind).
enum {
    TSMP_EV_CREATED       = 1,  // the controller exists (the page starts loading)
    TSMP_EV_READY         = 2,  // the page said "ready" (YouTube's player is ready)
    TSMP_EV_STATE         = 3,  // value: YouTube's state (-1 unstarted, 0 ended, 1 playing, 2 paused, 3 buffering, 5 cued)
    TSMP_EV_ERROR         = 4,  // value: YouTube's error code (2, 5, 100, 101, 150, 153, ...)
    TSMP_EV_TIME          = 5,  // value: the current time in whole seconds (the page reports it by itself)
    TSMP_EV_OPENURL       = 6,  // text: an https address the user asked to open (a link in YouTube's player)
    TSMP_EV_FAILED        = 7,  // value: HRESULT; text: "environment" | "controller" | "process" | "navigation" |
                                // 2.6: "network" (no network set, or the living environment has other arguments),
                                // "network-override" (chat proxy: a WebView2 override mentions a proxy), "busy" (the
                                // old browser process still holds the folder: 0x8007139F at the controller)
    TSMP_EV_CLOSEDBYUSER  = 8,  // text: "WxH", the window's last client size in device px (the popup's close box)
    TSMP_EV_LOST          = 9,  // the window was destroyed without tsmp_close (its owner went); the controller is closed
    TSMP_EV_KEY           = 10, // value: a virtual key (only VK_ESCAPE)
    TSMP_EV_ENGINEEXITED  = 11, // player 0: the answer to tsmp_releaseEngine. value 0: its browser process has exited;
                                // 1: not heard within 10 s (a new environment was made anyway); 2: there was nothing to
                                // release (none was made, or its creation failed); 3: a new open took the environment
                                // that was being made (no release happened)
    TSMP_EV_TEST          = 100 // testMode only (tools/player_smoke): text "referer:<value>", "request:<url>", "page:<msg>",
                                // 2.6 "auth:<answered|cancelled> <uri> <challenge>" (a proxy login request)
};

// 2.6: how the environment's browser reaches the web (TsmpNetwork::mode).
enum { TSMP_NET_UNSET = 0, TSMP_NET_SYSTEM = 1, TSMP_NET_DIRECT = 2, TSMP_NET_BRIDGE = 3 };

// testMode bits (TsmpInit::testMode; the plugin always passes 0).
enum {
    TSMP_TEST_FAKE_API = 1, // the page's iframe_api comes from memory, the embed request is answered with a blank page
    TSMP_TEST_MUTED    = 2  // every web view is muted (player_smoke --youtube)
};

typedef struct TsmpInit {
    const wchar_t* userDataFolder; // WebView2's user data folder (created by WebView2)
    const wchar_t* appOrigin;      // "https://io.github.metihttp.tsmediachat"
    const wchar_t* language;       // a BCP 47 tag for WebView2, or nullptr / empty
    int            testMode;       // TSMP_TEST_* bits; 0 in the plugin
} TsmpInit;

typedef struct TsmpOpen {
    void*          ownerHwnd;   // TeamSpeak's top-level window (the player window is owned by it)
    int            popup;       // 1: a titled window; 0: the anchored window (WS_POPUP tool window over the card)
    RECT           rect;        // the client area, screen device px
    RECT           clip;        // the visible part of rect (anchored), screen device px
    double         cssScale;    // CSS px of the page = device px / cssScale (s x Qt's dpr)
    const wchar_t* pageHtml;    // the document served at <appOrigin>/player
    const wchar_t* windowTitle; // the popup's title
} TsmpOpen;

typedef struct TsmpNetwork {
    int            mode;       // TSMP_NET_*
    int            bridgePort; // BRIDGE: 1024..65535
    const wchar_t* realm;      // BRIDGE: "tsmedia-<16 hex>"
    const wchar_t* user;       // BRIDGE: "tsm-p"
    const wchar_t* token;      // BRIDGE: the bridge's token (never the user's password)
} TsmpNetwork;

typedef struct TsmpEvent {
    int     player; // the player id (0: the engine)
    int     kind;   // TSMP_EV_*
    int     value;
    wchar_t text[2049];
} TsmpEvent;

typedef int(__cdecl* tsmp_apiVersion_fn)(void);
typedef int(__cdecl* tsmp_runtimeVersion_fn)(wchar_t* out, int chars);
typedef int(__cdecl* tsmp_init_fn)(const TsmpInit* init);
typedef int(__cdecl* tsmp_open_fn)(const TsmpOpen* open);
typedef void(__cdecl* tsmp_place_fn)(int id, RECT rect, RECT clip, double cssScale, int visible);
typedef void(__cdecl* tsmp_command_fn)(int id, const wchar_t* command);
typedef void(__cdecl* tsmp_close_fn)(int id);
typedef int(__cdecl* tsmp_poll_fn)(TsmpEvent* out, int max);
typedef void(__cdecl* tsmp_releaseEngine_fn)(void);
typedef void(__cdecl* tsmp_shutdown_fn)(void);
typedef int(__cdecl* tsmp_setNetwork_fn)(const TsmpNetwork* net);

#ifdef TSMP_BUILDING_HELPER
#define TSMP_EXPORT __declspec(dllexport)
// The helper's exports (player/tsmp.cpp, and player/tsmp_stub.cpp for the tests).
TSMP_EXPORT int __cdecl  tsmp_apiVersion(void);
TSMP_EXPORT int __cdecl  tsmp_runtimeVersion(wchar_t* out, int chars); // 0 ok; no runtime: < 0
TSMP_EXPORT int __cdecl  tsmp_init(const TsmpInit* init);               // 0 ok; resets every global (a plugin reload calls it again)
TSMP_EXPORT int __cdecl  tsmp_open(const TsmpOpen* open);               // a player id > 0 (creation goes on asynchronously); <= 0: refused
TSMP_EXPORT void __cdecl tsmp_place(int id, RECT rect, RECT clip, double cssScale, int visible);
TSMP_EXPORT void __cdecl tsmp_command(int id, const wchar_t* command);  // "pause" "play" "mute" "unmute" "volume:<0-100>"; "front" (popup)
TSMP_EXPORT void __cdecl tsmp_close(int id);                            // synchronous
TSMP_EXPORT int __cdecl  tsmp_poll(TsmpEvent* out, int max);            // events in order; never blocks
TSMP_EXPORT void __cdecl tsmp_releaseEngine(void);                      // no player left: release the environment
TSMP_EXPORT void __cdecl tsmp_shutdown(void);                           // synchronous: every player closed, everything released
TSMP_EXPORT int __cdecl  tsmp_setNetwork(const TsmpNetwork* net);       // 0 ok; < 0 refused (unknown mode; BRIDGE without a
                                                                         // valid port, realm, user or token): UNSET then
#endif

#ifdef __cplusplus
}
#endif
