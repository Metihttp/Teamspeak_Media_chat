#pragma once

// 2.5 YouTube player: what the player page may tell the helper, and what the helper may tell the page. Pure C++
// (no Qt, no WebView2), shared by the helper (player/tsmp.cpp) and the plugin's tests (ytpage::parseMessage), so
// both read exactly the same grammar (design2.md 3.5):
//   page -> helper:  ^(ready|state:-?\d{1,2}|error:\d{1,4}|time:\d{1,6})$   anything else is dropped
//                    (test mode only, tools/player_smoke: ^test:[a-z0-9:._-]{1,40}$)
//   helper -> page:  pause | play | mute | unmute | volume:<0-100>

#include <cstddef>

namespace tsmpmsg {

enum class Kind { None, Ready, State, Error, Time, Test };

struct Message {
    Kind kind  = Kind::None;
    int  value = 0;
};

namespace detail {

inline bool same(const wchar_t* s, std::size_t n, const wchar_t* word)
{
    std::size_t i = 0;
    for (; word[i]; ++i) {
        if (i >= n || s[i] != word[i])
            return false;
    }
    return i == n;
}

inline bool startsWith(const wchar_t* s, std::size_t n, const wchar_t* word, std::size_t* len)
{
    std::size_t i = 0;
    for (; word[i]; ++i) {
        if (i >= n || s[i] != word[i])
            return false;
    }
    *len = i;
    return true;
}

// minDigits..maxDigits ASCII digits, nothing else; *out the value.
inline bool digits(const wchar_t* s, std::size_t n, std::size_t maxDigits, int* out)
{
    if (n < 1 || n > maxDigits)
        return false;
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] < L'0' || s[i] > L'9')
            return false;
        v = v * 10 + (s[i] - L'0');
    }
    *out = v;
    return true;
}

} // namespace detail

// A message of the page (s: n UTF-16 units, no terminator needed). Kind::None: dropped.
inline Message parse(const wchar_t* s, std::size_t n, bool allowTest)
{
    Message m;
    if (!s || n == 0 || n > 64)
        return m;
    std::size_t p = 0;
    int         v = 0;
    if (detail::same(s, n, L"ready")) {
        m.kind = Kind::Ready;
    } else if (detail::startsWith(s, n, L"state:", &p)) {
        const bool negative = p < n && s[p] == L'-';
        if (detail::digits(s + p + (negative ? 1 : 0), n - p - (negative ? 1 : 0), 2, &v)) {
            m.kind  = Kind::State;
            m.value = negative ? -v : v;
        }
    } else if (detail::startsWith(s, n, L"error:", &p)) {
        if (detail::digits(s + p, n - p, 4, &v)) {
            m.kind  = Kind::Error;
            m.value = v;
        }
    } else if (detail::startsWith(s, n, L"time:", &p)) {
        if (detail::digits(s + p, n - p, 6, &v)) {
            m.kind  = Kind::Time;
            m.value = v;
        }
    } else if (allowTest && detail::startsWith(s, n, L"test:", &p) && n > p && n - p <= 40) {
        bool ok = true;
        for (std::size_t i = p; i < n && ok; ++i) {
            const wchar_t c = s[i];
            ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L':' || c == L'.' || c == L'_' || c == L'-';
        }
        if (ok)
            m.kind = Kind::Test;
    }
    return m;
}

// A command the helper passes to the page: only these.
inline bool validCommand(const wchar_t* s, std::size_t n)
{
    if (!s)
        return false;
    if (detail::same(s, n, L"pause") || detail::same(s, n, L"play") || detail::same(s, n, L"mute") || detail::same(s, n, L"unmute"))
        return true;
    std::size_t p = 0;
    int         v = 0;
    return detail::startsWith(s, n, L"volume:", &p) && detail::digits(s + p, n - p, 3, &v) && v <= 100;
}

// The web view starts muted (the page autoplays before the plugin's volume and mute arrive: nothing may start at
// YouTube's default full volume). A valid command that lifts that mute once it has been passed to the page: the
// plugin's volume (sent at Ready, after "pause" when the player must stay paused) or an explicit unmute. A session
// mute of the plugin stays YouTube's own (the embed address's mute=1 and the "mute" command).
inline bool liftsViewMute(const wchar_t* s, std::size_t n)
{
    std::size_t p = 0;
    return validCommand(s, n) && (detail::same(s, n, L"unmute") || detail::startsWith(s, n, L"volume:", &p));
}

// The popup's client rect (screen device px) fitted to its monitor's work area (review fix: at a high chat scale the
// remembered size could put the title bar off screen). frame: the window's border widths around the client rect
// (left, top, right, bottom, from AdjustWindowRectEx). At most 90% of the work area wide and high with the frame (the
// client's aspect kept), then moved so the whole window lies inside it. An empty work area or client: unchanged.
struct Box {
    long left = 0, top = 0, right = 0, bottom = 0;
};

inline Box fitPopup(Box client, Box frame, Box work)
{
    long       cw = client.right - client.left, ch = client.bottom - client.top;
    const long ww = work.right - work.left, wh = work.bottom - work.top;
    if (cw <= 0 || ch <= 0 || ww <= 0 || wh <= 0)
        return client;
    const double maxW = ww * 0.9 - (frame.left + frame.right), maxH = wh * 0.9 - (frame.top + frame.bottom);
    if (maxW >= 1 && maxH >= 1) {
        double f = maxW / cw;
        if (maxH / ch < f)
            f = maxH / ch;
        if (f < 1.0) {
            cw = static_cast<long>(cw * f + 0.5);
            ch = static_cast<long>(ch * f + 0.5);
            if (cw < 1)
                cw = 1;
            if (ch < 1)
                ch = 1;
        }
    }
    long left = (client.left + client.right) / 2 - cw / 2;
    long top  = (client.top + client.bottom) / 2 - ch / 2;
    if (left + cw + frame.right > work.right)
        left = work.right - frame.right - cw;
    if (left - frame.left < work.left)
        left = work.left + frame.left;
    if (top + ch + frame.bottom > work.bottom)
        top = work.bottom - frame.bottom - ch;
    if (top - frame.top < work.top)
        top = work.top + frame.top; // the title bar always on the screen
    Box out;
    out.left   = left;
    out.top    = top;
    out.right  = left + cw;
    out.bottom = top + ch;
    return out;
}

} // namespace tsmpmsg
