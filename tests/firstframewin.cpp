// 2.7.4 (F1) tests: see firstframewin.h.
#include "firstframewin.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>

#include <cstring>

#include <windows.h>

namespace firstframewin {

namespace {

bool filter(MSG& msg, long* result)
{
    QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance();
    return dispatcher && dispatcher->filterNativeEvent(QByteArray("windows_generic_MSG"), &msg, result);
}

bool send(quintptr hwnd, UINT message, WPARAM wParam = 0, LPARAM lParam = 0)
{
    MSG msg     = {};
    msg.hwnd    = reinterpret_cast<HWND>(hwnd);
    msg.message = message;
    msg.wParam  = wParam;
    msg.lParam  = lParam;
    long result = 0;
    return filter(msg, &result);
}

} // namespace

Erase erase(quintptr hwnd)
{
    Erase      out;
    HDC        dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize     = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth    = 16;
    bi.bmiHeader.biHeight   = -16;
    bi.bmiHeader.biPlanes   = 1;
    bi.bmiHeader.biBitCount = 32;
    void*   bits            = nullptr;
    HBITMAP bmp             = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp)
            DeleteObject(bmp);
        if (dc)
            DeleteDC(dc);
        return out;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    std::memset(bits, 0xff, 16 * 16 * 4);
    MSG msg     = {};
    msg.hwnd    = reinterpret_cast<HWND>(hwnd);
    msg.message = WM_ERASEBKGND;
    msg.wParam  = reinterpret_cast<WPARAM>(dc);
    long result = 0;
    out.taken   = filter(msg, &result);
    out.result  = result;
    GdiFlush();
    out.pixel = static_cast<const quint32*>(bits)[8 * 16 + 8] | 0xff000000u; // a 32-bit DIB pixel is 0x00RRGGBB
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

bool paint(quintptr hwnd)
{
    return send(hwnd, WM_PAINT);
}

bool posChanged(quintptr hwnd, bool shown, bool hidden)
{
    WINDOWPOS pos = {};
    pos.hwnd      = reinterpret_cast<HWND>(hwnd);
    pos.flags     = SWP_NOMOVE | SWP_NOSIZE | (shown ? SWP_SHOWWINDOW : 0) | (hidden ? SWP_HIDEWINDOW : 0);
    if (!shown && !hidden)
        pos.flags = SWP_NOSIZE; // a move
    return send(hwnd, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&pos));
}

bool showWindow(quintptr hwnd, bool shown)
{
    return send(hwnd, WM_SHOWWINDOW, shown ? TRUE : FALSE);
}

bool size(quintptr hwnd, bool minimized)
{
    return send(hwnd, WM_SIZE, minimized ? SIZE_MINIMIZED : SIZE_RESTORED, MAKELPARAM(1000, 760));
}

bool destroy(quintptr hwnd)
{
    return send(hwnd, WM_DESTROY);
}

} // namespace firstframewin
