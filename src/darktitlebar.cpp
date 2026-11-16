#include "darktitlebar.h"

#include <QGuiApplication>

#include <windows.h>

// 2.7 Dark mode (design_dark 1.7), like composeui::applyWindowCaption: dwmapi looked up at run time.

namespace darktitlebar {

namespace {

using RealSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

HMODULE      g_loaded    = nullptr; // a dwmapi.dll we loaded ourselves (freed by shutdown)
SetAttribute g_fake      = nullptr; // tests
int          g_immersive = 0;       // the immersive dark attribute this Windows accepted (20 or 19), 0: none yet

constexpr unsigned kImmersive     = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr unsigned kImmersiveOld  = 19; // the same before Windows 10 20H1
constexpr unsigned kBorderColor   = 34; // DWMWA_BORDER_COLOR (Windows 11)
constexpr unsigned kCaptionColor  = 35; // DWMWA_CAPTION_COLOR
constexpr unsigned kTextColor     = 36; // DWMWA_TEXT_COLOR
constexpr DWORD    kColorDefault  = 0xFFFFFFFFu; // DWMWA_COLOR_DEFAULT

bool onWindows()
{
    return QGuiApplication::platformName() == QLatin1String("windows");
}

RealSet realSet()
{
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm) {
        if (!g_loaded)
            g_loaded = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        dwm = g_loaded;
    }
    if (!dwm)
        return nullptr;
    return reinterpret_cast<RealSet>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
}

long set(quintptr hwnd, unsigned attribute, const void* value, unsigned size)
{
    if (g_fake)
        return g_fake(reinterpret_cast<void*>(hwnd), attribute, value, size);
    if (!onWindows())
        return E_FAIL;
    const RealSet fn = realSet();
    return fn ? fn(reinterpret_cast<HWND>(hwnd), attribute, value, size) : E_FAIL;
}

// Windows 10 repaints a visible window's frame only when told to.
void repaintFrame(quintptr hwnd)
{
    if (g_fake || !onWindows())
        return;
    const HWND h = reinterpret_cast<HWND>(hwnd);
    if (IsWindow(h) && IsWindowVisible(h))
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

DWORD colorRef(const QColor& c)
{
    return RGB(c.red(), c.green(), c.blue());
}

} // namespace

bool apply(quintptr hwnd, const Colors& colors)
{
    if (!hwnd)
        return false;
    const BOOL on = TRUE;
    bool       ok = SUCCEEDED(set(hwnd, kImmersive, &on, sizeof(on)));
    if (ok) {
        g_immersive = static_cast<int>(kImmersive);
    } else {
        ok = SUCCEEDED(set(hwnd, kImmersiveOld, &on, sizeof(on)));
        if (ok)
            g_immersive = static_cast<int>(kImmersiveOld);
    }
    // Windows 11 only (Windows 10 refuses them).
    const DWORD caption = colorRef(colors.caption);
    const DWORD text    = colorRef(colors.text);
    const DWORD border  = colorRef(colors.border);
    set(hwnd, kCaptionColor, &caption, sizeof(caption));
    set(hwnd, kTextColor, &text, sizeof(text));
    set(hwnd, kBorderColor, &border, sizeof(border));
    repaintFrame(hwnd);
    return ok;
}

void revert(quintptr hwnd)
{
    if (!hwnd)
        return;
    const BOOL off = FALSE;
    set(hwnd, g_immersive ? static_cast<unsigned>(g_immersive) : kImmersive, &off, sizeof(off));
    const DWORD standard = kColorDefault;
    set(hwnd, kCaptionColor, &standard, sizeof(standard));
    set(hwnd, kTextColor, &standard, sizeof(standard));
    set(hwnd, kBorderColor, &standard, sizeof(standard));
    repaintFrame(hwnd);
}

void shutdown()
{
    if (g_loaded) {
        FreeLibrary(g_loaded);
        g_loaded = nullptr;
    }
}

void setSetAttributeForTests(SetAttribute fn)
{
    g_fake = fn;
}

} // namespace darktitlebar
