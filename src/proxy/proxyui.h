#pragma once

// 2.6 chat proxy: how the rest of the plugin opens the Chat proxy window (proxywindow.h has the window itself; this
// header keeps Windows' headers out of the files that only open it).

class QWidget;

namespace proxy {
class Control;
} // namespace proxy

namespace proxy::ui {

// How the window opens: as saved; to set one up (the checkbox checked, the server 127.0.0.1, the focus on Find my VPN
// app; nothing saved until OK); or filled from Windows' SOCKS proxy ("Use it as the chat proxy").
enum class Request { Open, SetUp, UseIt };

// How "Chat proxy settings…", "Set up proxy…" and a card's "Proxy settings…" open it: as saved, or the set-up form
// while no proxy is saved, so that Find, Test and OK turn the new proxy on (design 1.2; review fix: it opened unticked
// and OK saved the proxy off).
inline Request settingsRequest(bool proxySaved)
{
    return proxySaved ? Request::Open : Request::SetUp;
}

// The one window (raised when it is open). parent: the Settings dialog (then window-modal to it) or TeamSpeak's main
// window. control: the plugin's (Control::current()).
QWidget* showProxyWindow(QWidget* parent, Request request, Control* control = nullptr);

} // namespace proxy::ui
