// 2.7.4 (F1): the first frame of TeamSpeak's main window at a start with dark mode on (darkfirstframe.h).
#include "darkfirstframe.h"

#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimerEvent>
#include <QWidget>

#include <vector>

#include <windows.h>

namespace darkfirstframe {

namespace {

using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

constexpr DWORD kCloak      = 13;  // DWMWA_CLOAK (Windows 8 and later; older ones refuse it: the fill alone)
constexpr int   kCloakMaxMs = 300; // the cloak's last resort (it normally ends a few ms after the show, at the erase)

CloakHook g_hook = nullptr; // tests: instead of DWM

// A window of this process (never another's), on Windows' own platform (never the offscreen one's made-up handles).
bool ownWindow(HWND hwnd)
{
    if (!hwnd || QGuiApplication::platformName() != QLatin1String("windows") || !IsWindow(hwnd))
        return false;
    DWORD pid = 0;
    return GetWindowThreadProcessId(hwnd, &pid) != 0 && pid == GetCurrentProcessId();
}

bool setCloak(HWND hwnd, bool on)
{
    if (g_hook)
        return g_hook(reinterpret_cast<quintptr>(hwnd), on);
    if (!ownWindow(hwnd))
        return false;
    // dwmapi is loaded in every process with a window (darktitlebar loads it for the title bar before this): looked up at
    // run time, so the plugin imports nothing new.
    const HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    const auto    set = dwm ? reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) : nullptr;
    const BOOL    value = on ? TRUE : FALSE;
    return set && SUCCEEDED(set(hwnd, kCloak, &value, sizeof value));
}

class Guard final : public QObject, public QAbstractNativeEventFilter
{
  public:
    HWND          hwnd    = nullptr; // nullptr once the window went (WM_DESTROY): its handle may be another window's then
    HWND          cloakOf = nullptr; // the cloaked window's handle (for the uncloak)
    COLORREF      colour  = 0;
    int           fills   = 0;
    bool          filling = true; // until disarm(): then a guard left for its uncloak leaves the erases to Qt
    bool          cloaked = false;
    Uncloak       how     = Uncloak::None;
    LARGE_INTEGER cloakAt = {};
    double        cloakMs = 0;
    int           timer   = 0;

    void cloak()
    {
        // Only a window Windows doesn't show yet (Qt sends the Show event before its ShowWindow): a shown one stays shown.
        if (!g_hook && IsWindowVisible(hwnd))
            return;
        timer = startTimer(kCloakMaxMs); // first: no cloak without its last resort
        if (!timer)
            return;
        if (!setCloak(hwnd, true)) {
            killTimer(timer); // no cloak (no DWM, or refused): the fill alone
            timer = 0;
            return;
        }
        cloaked = true;
        cloakOf = hwnd;
        QueryPerformanceCounter(&cloakAt);
    }

    // last: release()'s try (the plugin goes): nothing tries again after it.
    void uncloak(Uncloak why, bool last = false)
    {
        if (timer) {
            killTimer(timer);
            timer = 0;
        }
        if (!cloaked)
            return;
        const QPointer<QObject> alive(this);
        const bool              done = setCloak(cloakOf, false);
        if (!alive || !cloaked)
            return; // (never seen) the call ended the guard or the cloak itself
        if (!done && !last && (g_hook || IsWindow(cloakOf))) {
            // Refused (never seen): again by the timer, every 300 ms until DWM takes it, also after disarm() (the guard
            // stays for it); only release() gives up. A window that went has no cloak left.
            timer = startTimer(kCloakMaxMs);
            if (timer)
                return;
        }
        cloaked = false;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        cloakMs = double(now.QuadPart - cloakAt.QuadPart) * 1000.0 / double(freq.QuadPart);
        how     = why;
    }

    void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() == timer)
            uncloak(Uncloak::Timer);
        else
            QObject::timerEvent(event);
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override
    {
        if (!hwnd || !message || eventType != "windows_generic_MSG")
            return false;
        const MSG* m = static_cast<const MSG*>(message);
        if (m->hwnd != hwnd)
            return false;
        switch (m->message) {
        case WM_ERASEBKGND: {
            if (!filling)
                return false; // disarmed: Qt's, as before (a guard left for its uncloak)
            // The erase's DC: its clip box is what Windows wants erased (the client area at the first show).
            HDC  dc = reinterpret_cast<HDC>(m->wParam);
            RECT rc;
            if (!dc || GetClipBox(dc, &rc) == ERROR)
                return false; // Qt's answer; the show's WM_WINDOWPOSCHANGED uncloaks
            const COLORREF before = SetDCBrushColor(dc, colour);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (before != CLR_INVALID)
                SetDCBrushColor(dc, before);
            GdiFlush(); // in the window's surface before it is uncloaked
            ++fills;
            if (result)
                *result = 1; // erased (Qt answers 1 too, without drawing)
            uncloak(Uncloak::Erase);
            return true;
        }
        case WM_WINDOWPOSCHANGED: {
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(m->lParam);
            if (pos && (pos->flags & SWP_SHOWWINDOW))
                uncloak(Uncloak::Shown); // the show is over: never longer than ShowWindow itself
            else if (pos && (pos->flags & SWP_HIDEWINDOW))
                uncloak(Uncloak::Hidden);
            return false;
        }
        case WM_SHOWWINDOW:
            if (!m->wParam)
                uncloak(Uncloak::Hidden);
            return false;
        case WM_SIZE:
            if (m->wParam == SIZE_MINIMIZED)
                uncloak(Uncloak::Hidden);
            return false;
        case WM_DESTROY:
            uncloak(Uncloak::Hidden);
            hwnd = nullptr; // its handle can be another window's from now on: nothing more to fill
            return false;
        default:
            return false;
        }
    }
};

Guard*  g_guard = nullptr;
Uncloak g_how   = Uncloak::None;
double  g_ms    = 0;
// Guards disarm() left for their uncloak (DWM refused it; never seen), until release(); g_last: the last disarmed one if it
// is one of them (lastUncloak follows it).
std::vector<Guard*> g_left;
Guard*              g_last = nullptr;

void destroy(Guard* guard)
{
    if (QCoreApplication* app = QCoreApplication::instance())
        app->removeNativeEventFilter(guard);
    delete guard; // ~QAbstractNativeEventFilter removes it from the dispatcher too; ~QObject kills its timer
}

} // namespace

void arm(QWidget* window, const QColor& fill)
{
    disarm();
    g_how                 = Uncloak::None; // this arm's (also when it arms nothing)
    g_ms                  = 0;
    g_last                = nullptr;
    QCoreApplication* app = QCoreApplication::instance();
    // Native event filters run on the GUI thread, and not at all in a plugin application (Qt::AA_PluginApplication): then
    // nothing would ever see the erase or the show, so nothing is armed.
    if (!window || !app || QThread::currentThread() != app->thread() || QCoreApplication::testAttribute(Qt::AA_PluginApplication)
        || !QAbstractEventDispatcher::instance(app->thread()) || !window->testAttribute(Qt::WA_WState_Created) || !window->internalWinId())
        return;
    auto* guard   = new Guard;
    guard->hwnd   = reinterpret_cast<HWND>(window->internalWinId());
    guard->colour = RGB(fill.red(), fill.green(), fill.blue());
    g_guard       = guard;
    app->installNativeEventFilter(guard);
    guard->cloak();
}

bool disarm()
{
    Guard* guard = g_guard;
    if (!guard)
        return true;
    g_guard        = nullptr; // first: a call back from inside (never seen) finds nothing armed
    guard->filling = false;
    guard->uncloak(Uncloak::Disarm);
    if (guard->cloaked) {
        // DWM refused the uncloak (never seen): the window must not stay invisible, so the guard stays for the uncloak alone
        // (its timer tries again every 300 ms) until release().
        g_left.push_back(guard);
        g_last = guard;
        return false;
    }
    g_how  = guard->how;
    g_ms   = guard->cloakMs;
    g_last = nullptr;
    destroy(guard);
    return true;
}

void release()
{
    disarm();
    std::vector<Guard*> left;
    left.swap(g_left); // first: a call back from inside (never seen) finds none
    for (Guard* guard : left) {
        guard->uncloak(Uncloak::Disarm, true);
        if (guard == g_last) {
            g_how  = guard->how;
            g_ms   = guard->cloakMs;
            g_last = nullptr;
        }
        destroy(guard);
    }
}

bool armed()
{
    return g_guard != nullptr;
}

bool cloaked()
{
    if (g_guard && g_guard->cloaked)
        return true;
    for (const Guard* guard : g_left) {
        if (guard->cloaked)
            return true;
    }
    return false;
}

int leftOver()
{
    return int(g_left.size());
}

int fills()
{
    return g_guard ? g_guard->fills : 0;
}

Uncloak lastUncloak()
{
    return g_guard ? g_guard->how : g_last ? g_last->how : g_how;
}

double lastCloakMs()
{
    return g_guard ? g_guard->cloakMs : g_last ? g_last->cloakMs : g_ms;
}

void setCloakHookForTests(CloakHook hook)
{
    g_hook = hook;
}

} // namespace darkfirstframe
