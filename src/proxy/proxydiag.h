#pragma once

// 2.6 chat proxy: the "Chat proxy" section of the diagnostic info (design 2.4, D18). Categories only: the mode, where
// the proxy is ("on this PC", "on the local network", "elsewhere"), whether a login is set, route health, the Windows
// setting's kind, the last Test and the bridge's counters. NEVER the proxy's host, port, user name or password, the
// sealed blob, the bridge's token, realm or port, nor the Windows proxy's address: a test fills all of them with
// distinctive strings and checks the whole report.
//
// diagnosticLines() is pure (tests); collectDiagnostics() reads proxy::Control and Windows' settings on the GUI thread
// (one WinHttpGetIEProxyConfigForCurrentUser call, no script run).

#include <QHash>
#include <QString>
#include <QStringList>

#include "control.h"
#include "health.h"
#include "proxyconfig.h"
#include "windowsproxy.h"

namespace proxy {

struct DiagnosticsInput {
    Config                 saved;                        // the saved config (its blob is never read here)
    Mode                   mode    = Mode::Unset;        // the published effective mode
    Failure                refused = Failure::None;      // custom: the route's refusal (PasswordUnreadable, BridgeUnavailable)
    Health::State          health  = Health::State::Unknown;
    Failure                lastFailure = Failure::None;  // custom: the down episode's last failure
    qint64                 downForMs   = -1;             // custom, Down or Probation: how long (-1: not down)
    int                    pausedLoads = 0;              // custom: automatic loads the gate held back in this episode
    WindowsStatus          windows;                      // Windows mode: the last Windows-proxy failure
    qint64                 nowMs = 0;                    // the Control's clock (windows.atMs)
    windowsproxy::IeConfig ie;                           // what Windows names
    Control::LastTest      lastTest;                     // the window's Test this session
    qint64                 nowWallMs = 0;                // ms since the epoch (lastTest.wallMs)
    enum class BridgeState { NotStarted, Running, Failed };
    BridgeState            bridge = BridgeState::NotStarted;
    struct Counters {                                    // Bridge::Counters (bridge.h stays out of the plugin's headers)
        int                 tunnelsOpen     = 0;
        quint64             accepted        = 0;
        quint64             refused         = 0;
        quint64             busy            = 0;
        quint64             unauthenticated = 0;
        QHash<int, quint64> failures;                    // by Failure
    };
    Counters               counters;
};

QString          diagnosticsTitle();                                  // "Chat proxy"
QStringList      diagnosticLines(const DiagnosticsInput& in);         // pure
DiagnosticsInput collectDiagnostics(const Control* control);          // GUI thread; control may be null (not started)
QStringList      currentDiagnosticLines();                            // collectDiagnostics(Control::current())

// "3 min", "45 s", "2 h 5 min" (the diagnostics' durations).
QString diagnosticsDuration(qint64 ms);

} // namespace proxy
