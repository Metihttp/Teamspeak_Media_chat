// 2.5 YouTube player: a stand-in for the player helper, for tests/tst_playerload.cpp (extraction, loading, pinning,
// the API check). It implements the C API of player/tsmp.h without WebView2 and without any window: tsmp_open
// queues a Created event, nothing else happens. TSMP_STUB_API_VERSION lets a second build report another version.
// 2.6 (API 2): tsmp_setNetwork with the helper's rules (tsmpnet.h); an open without a network fails "network".

#define TSMP_BUILDING_HELPER
#include "tsmp.h"
#include "tsmpnet.h"

#include <cwchar>

#ifndef TSMP_STUB_API_VERSION
#define TSMP_STUB_API_VERSION TSMP_API_VERSION
#endif

namespace {

int       g_next   = 0;
int       g_inits  = 0;
TsmpEvent g_queue[16];
int       g_count  = 0;
int       g_mode   = TSMP_NET_UNSET; // 2.6: the network tsmp_setNetwork accepted

void post(int player, int kind, int value, const wchar_t* text = nullptr)
{
    if (g_count >= 16)
        return;
    TsmpEvent& e = g_queue[g_count++];
    e.player     = player;
    e.kind       = kind;
    e.value      = value;
    e.text[0]    = 0;
    if (text)
        wcsncpy_s(e.text, 2049, text, _TRUNCATE);
}

} // namespace

extern "C" {

int __cdecl tsmp_apiVersion(void)
{
    return TSMP_STUB_API_VERSION;
}

int __cdecl tsmp_runtimeVersion(wchar_t* out, int chars)
{
    if (!out || chars < 8)
        return -1;
    wcscpy_s(out, static_cast<size_t>(chars), L"stub");
    return 0;
}

int __cdecl tsmp_init(const TsmpInit* init)
{
    if (!init)
        return -1;
    ++g_inits;
    g_count = 0;
    g_mode  = TSMP_NET_UNSET;
    return 0;
}

int __cdecl tsmp_open(const TsmpOpen* open)
{
    if (!open || !open->pageHtml)
        return 0;
    const int id = ++g_next;
    post(id, g_mode == TSMP_NET_UNSET ? TSMP_EV_FAILED : TSMP_EV_CREATED, 0, g_mode == TSMP_NET_UNSET ? L"network" : nullptr);
    return id;
}

void __cdecl tsmp_place(int, RECT, RECT, double, int) {}

void __cdecl tsmp_command(int, const wchar_t*) {}

void __cdecl tsmp_close(int) {}

int __cdecl tsmp_poll(TsmpEvent* out, int max)
{
    int n = 0;
    while (n < max && n < g_count) {
        out[n] = g_queue[n];
        ++n;
    }
    for (int i = n; i < g_count; ++i)
        g_queue[i - n] = g_queue[i];
    g_count -= n;
    return n;
}

void __cdecl tsmp_releaseEngine(void)
{
    post(0, TSMP_EV_ENGINEEXITED, 0);
}

void __cdecl tsmp_shutdown(void)
{
    g_count = 0;
    g_mode  = TSMP_NET_UNSET;
}

int __cdecl tsmp_setNetwork(const TsmpNetwork* net)
{
    g_mode = TSMP_NET_UNSET;
    if (!net)
        return -1;
    tsmpnet::Net next;
    next.mode       = net->mode;
    next.bridgePort = net->bridgePort;
    next.realm      = net->realm ? net->realm : L"";
    next.user       = net->user ? net->user : L"";
    next.token      = net->token ? net->token : L"";
    if (!tsmpnet::validNetwork(next))
        return -2;
    g_mode = next.mode;
    return 0;
}

} // extern "C"
