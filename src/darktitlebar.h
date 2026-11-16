#pragma once

// 2.7 Dark mode: the title bars of TeamSpeak's windows (design_dark 1.7). Per-window DWM attributes only, never a Windows
// setting: the immersive dark mode (attribute 20; 19 on Windows 10 1809-1909) and, on Windows 11, the caption, caption text
// and border colours (35, 36, 34) in the theme's colours. revert() puts each window back to the system's own title bar.
// DwmSetWindowAttribute is looked up at run time (dwmapi.dll is in every desktop process; nothing new is imported); a
// dwmapi.dll this module had to load itself is freed by shutdown(). Outside the Windows platform (tests: offscreen)
// nothing happens unless a test installed its own function.

#include <QColor>
#include <QtGlobal>

namespace darktitlebar {

// A window that draws its own dark caption (the media viewer, the send window) carries this dynamic property (true):
// dark mode leaves its title bar alone. QObject::setProperty copies the name.
constexpr const char kOwnCaption[] = "tsmediaOwnCaption";

struct Colors {
    QColor caption; // the title bar (Windows 11)
    QColor text;    // its text
    QColor border;  // the window's 1 px border
};

// True when the immersive dark attribute was accepted (20, else 19). Windows 11's colours are set too (ignored by
// Windows 10). A window that is already visible repaints its frame at once.
bool apply(quintptr hwnd, const Colors& colors);
// The attribute that was accepted set back to FALSE, the colours to DWMWA_COLOR_DEFAULT, the frame repainted.
void revert(quintptr hwnd);
// Frees a dwmapi.dll this module loaded (plugin unload). Idempotent.
void shutdown();

// Tests: every DwmSetWindowAttribute call goes to fn instead (nullptr: the real one again). fn returns an HRESULT.
using SetAttribute = long (*)(void* hwnd, unsigned attribute, const void* value, unsigned size);
void setSetAttributeForTests(SetAttribute fn);

} // namespace darktitlebar
