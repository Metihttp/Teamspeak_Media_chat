#pragma once

// 2.7.4 (F1) tests: window messages through the event dispatcher's native event filters, as Qt's window procedure hands
// them over ("windows_generic_MSG"), for darkfirstframe's guard.

#include <QRgb>
#include <QtGlobal>

namespace firstframewin {

struct Erase {
    bool taken  = false; // a filter answered it
    long result = 0;     // its answer
    QRgb pixel  = 0;     // the middle pixel of the erase's 16x16 DC (white before)
};
// A WM_ERASEBKGND for hwnd with a white 16x16 memory DC.
Erase erase(quintptr hwnd);
// A WM_PAINT for hwnd: whether a filter took it.
bool paint(quintptr hwnd);
// A WM_WINDOWPOSCHANGED for hwnd: shown: SWP_SHOWWINDOW, hidden: SWP_HIDEWINDOW, neither: a move (SWP_NOSIZE only).
bool posChanged(quintptr hwnd, bool shown, bool hidden = false);
// A WM_SHOWWINDOW for hwnd (shown: wParam TRUE, else FALSE): whether a filter took it.
bool showWindow(quintptr hwnd, bool shown);
// A WM_SIZE for hwnd (minimized: SIZE_MINIMIZED, else SIZE_RESTORED): whether a filter took it.
bool size(quintptr hwnd, bool minimized);
// A WM_DESTROY for hwnd: whether a filter took it.
bool destroy(quintptr hwnd);

} // namespace firstframewin
