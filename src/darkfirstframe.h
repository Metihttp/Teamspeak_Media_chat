#pragma once

// 2.7.4 (F1): the first frame of TeamSpeak's main window at a start with dark mode on.
// Windows shows the window inside Qt's ShowWindow (right after the window's QEvent::Show) with an unpainted, white
// surface, erases its client area (WM_ERASEBKGND, sent inside ShowWindow after WM_NCPAINT), and Qt answers that erase
// without drawing (its backing store paints everything at its first paint). TeamSpeak's GUI thread is busy for 55-60 ms
// after the show with its own work, and Qt's first paint takes about 25 ms more (2.7.3 added dark mode's measure in the
// style, about 36 ms), so the white surface was on the screen for 70-150 ms (2.7.2 too, longer with 2.7.3).
// The guard, for that one window from its first Show at a dark start (DarkMode arms it):
//   - cloaks it (DWMWA_CLOAK) before Windows shows it, so the white surface is never composed;
//   - answers its erases with a fill in dark mode's window colour; the first one (inside ShowWindow) uncloaks it right
//     after the fill, so the first frame Windows composes is dark;
//   - uncloaks it at the latest when the show is over (its WM_WINDOWPOSCHANGED with SWP_SHOWWINDOW, also inside
//     ShowWindow), when it hides, minimizes or goes, at disarm() and after an owned 300 ms timer: the cloak never outlives
//     ShowWindow unless all of those fail, and never the timer unless DWM refuses the uncloak (never seen): a refused
//     uncloak is tried again every 300 ms by that timer until DWM takes it or the window goes, also after disarm(); only
//     release() (the plugin goes) gives up after one last try;
//   - ends when DarkMode sees the window's first QEvent::Paint and calls disarm() (TeamSpeak 3.6.2's Qt does not hand that
//     window's WM_PAINT to the native event filters, so the end is Qt's Paint event, not WM_PAINT).
// It cloaks only a window of this process that Windows doesn't show yet, never hides, moves or resizes anything, and
// touches no other window and no system setting; without DWM's cloak (or on another platform) it only fills.
// GUI thread only. CRASH RULE: the native event filter (and its timer) is this module's own object; disarm() uncloaks,
// removes and deletes it (DarkMode calls it at the window's first paint, off and pause), unless DWM refused the uncloak:
// then the guard stays for the uncloak alone until release(), which DarkMode calls at every shutdown (TeamSpeak's Quit
// included) and which deletes every guard; its base classes' destructors remove the filter and kill the timer too.

#include <QColor>
#include <QtGlobal>

class QWidget;

namespace darkfirstframe {

// How the last cloak ended.
enum class Uncloak {
    None,   // not cloaked (yet), or the cloak was refused
    Erase,  // right after the fill of the first erase
    Shown,  // when the show was over (WM_WINDOWPOSCHANGED with SWP_SHOWWINDOW)
    Hidden, // the window hid, minimized or went
    Timer,  // the owned timer
    Disarm, // disarm() (or release())
};

// Guards window (created: its native handle exists; TeamSpeak's main window at its first QEvent::Show) with fill. Replaces
// an earlier arm.
void arm(QWidget* window, const QColor& fill);
// The guard's fill ends (idempotent): uncloaked, the filter removed and deleted; true. If DWM refuses the uncloak (never
// seen), the guard stays for the uncloak alone (its filter for the window's show, hide and end, its timer trying again every
// 300 ms) until DWM takes it, the window goes or release(); false.
bool disarm();
// The plugin goes (DarkMode's shutdown): disarm(), and every guard disarm() left for its uncloak tries one last time and
// goes, so nothing of this module outlives the plugin (a window whose uncloak DWM refuses even then stays cloaked).
void release();
// Armed (between arm and disarm).
bool armed();
// A window is held cloaked: by the armed guard, or by one disarm() left for its uncloak.
bool cloaked();
// Guards disarm() left for their uncloak (also once it is done) until release(): 0 unless DWM refused an uncloak.
int leftOver();
// Since the last arm: erases answered with the fill (0 when not armed).
int fills();
// The last cloak (the current guard's, else the last disarmed one's, also while it is left for its uncloak): how it ended
// and how long it lasted (ms).
Uncloak lastUncloak();
double  lastCloakMs();
// Tests: cloak and uncloak through hook (hwnd, on; true: done) instead of DWM; nullptr: DWM again.
using CloakHook = bool (*)(quintptr hwnd, bool on);
void setCloakHookForTests(CloakHook hook);

} // namespace darkfirstframe
