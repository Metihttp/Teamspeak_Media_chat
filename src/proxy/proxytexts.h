#pragma once

// 2.6 chat proxy: the texts the user sees about the chat proxy outside its window's form (design 1.3.4, 1.4, 1.5).
// Pure QtCore + i18n: the lines TS Media prints after a toggle and for /tsmedia proxy, the notices with their links,
// the status row of the Chat proxy window and the Settings group, and the failure's short text with the chat rule.
//
// Chat lines (TeamSpeak keeps chat logs on disk, F24) name a proxy's host:port only when it is on this PC; the window
// and the Settings group (never logged) always show it. Never the user name or anything of the password.

#include <QString>

#include "control.h"
#include "failure.h"
#include "health.h"
#include "proxyconfig.h"
#include "windowsproxy.h"

namespace proxy::texts {

// ---- lines (design 1.4) -------------------------------------------------------------------------------------------

// The line after a turn on / off / toggle (waitingForClick: links that waited for a proxy now wait for a click, S4).
QString toggleLine(Toggle toggle, const Config& saved, int waitingForClick);
// /tsmedia proxy: "Chat proxy on: SOCKS5 proxy 127.0.0.1:2080 (working)." / "Chat proxy off. TS Media uses ...".
QString statusLine(const Config& saved, Health::State health, Failure lastFailure);
QString usageLine();
QString helpLine(); // "[b]/tsmedia proxy[/b] on | off — ..."
QString menuOnText();
QString menuOffText();
QString hotkeyDescription();

// The failure's short text (5.1) for a chat line: "can't reach the proxy at 127.0.0.1:2080" for a proxy on this PC,
// "can't reach the proxy" elsewhere.
QString chatShortText(Failure failure, const Config& saved);
// ... for the window and the Settings group (always host:port), and for a Windows proxy (hostPort: Windows' proxy).
QString windowShortText(Failure failure, const QString& hostPort);
// ... for a chat line about the Windows proxy (hostPort: Windows' "host:port" or "[v6]:port"): the chat rule, so its
// address only when it is on this PC (review fix, design 1.5 / F24).
QString windowChatShortText(Failure failure, const QString& hostPort);

// ---- notices (design 1.5) -----------------------------------------------------------------------------------------

enum class Notice { None, ChatDown, ChatPassword, ChatIntercepted, WindowsFailed, WindowsSocksOnly, WindowsScript };
// The notice a route failure gets: custom mode's Down by its failure; Windows mode's by its kind.
Notice  noticeFor(Mode mode, Failure failure);
QString noticeText(Notice notice, const QString& shortText);
QString noticeLinkText(Notice notice); // "Turn off chat proxy" / "Proxy settings"
bool    noticeTurnsOff(Notice notice); // the link turns the chat proxy off (else it opens the window)

// ---- the status row (design 1.3.4) --------------------------------------------------------------------------------

struct StatusInput {
    Config                 saved;
    Mode                   mode   = Mode::System; // the published effective mode
    Health::State          health = Health::State::Unknown;
    Failure                lastFailure = Failure::None; // custom: the route's (or its refusal); Windows: the Windows status
    WindowsStatus          windows;
    qint64                 nowMs = 0;                    // the Control's clock (the Windows status' 10 minutes)
    windowsproxy::Summary  summary;                      // what Windows names (one cheap read)
    QString                lastTestWhen;                 // "today at 12:40" (a test of the saved values this session)
    bool                   group = false;                // the Settings group: "No chat proxy is set up." first
};
struct StatusRow {
    enum class Icon { None, Check, Warning };
    Icon    icon  = Icon::None;
    QString text;
    bool    useIt = false; // "Use it as the chat proxy" (Windows names only a SOCKS proxy)
};
StatusRow statusRow(const StatusInput& in);

constexpr qint64 kWindowsFailureMs = 10 * 60 * 1000; // a Windows-proxy failure shows this long (no site answer since)

} // namespace proxy::texts
