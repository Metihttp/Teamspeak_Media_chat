#include "proxydiag.h"

#include <QDateTime>

#include <algorithm>

#include "bridge.h"
#include "failure.h"
#include "i18n.h"
#include "router.h"

namespace proxy {

namespace {

QString placeText(Place place)
{
    switch (place) {
    case Place::ThisPc:
        return i18n::t("on this PC");
    case Place::LocalNetwork:
        return i18n::t("on the local network");
    case Place::Elsewhere:
        break;
    }
    return i18n::t("elsewhere");
}

// The host of "host:port" / "[v6]:port" (the Windows proxy's first server), only to say where it is.
QString hostOf(const QString& hostPort)
{
    QString s = hostPort.trimmed();
    if (s.startsWith(QLatin1Char('['))) {
        const int close = s.indexOf(QLatin1Char(']'));
        return close > 0 ? s.mid(1, close - 1) : s;
    }
    const int colon = s.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && s.indexOf(QLatin1Char(':')) == colon)
        s = s.left(colon);
    return s;
}

QString loginText(const Config& c)
{
    return c.user.isEmpty() ? i18n::t("no") : i18n::t("yes");
}

QString failureText(Failure failure)
{
    const QString text = shortText(failure); // never with the host and port: the 1.4 rule without an address
    return text.isEmpty() ? QString::fromLatin1(failureName(failure)) : text;
}

QString windowsLine(const windowsproxy::IeConfig& ie)
{
    if (ie.ok && !ie.autoConfigUrl.isEmpty())
        return ie.autoDetect ? i18n::t("setup script and automatic detection") : i18n::t("setup script");
    // A manual setting also while automatic detection is on (what applies when no WPAD server answers, review fix).
    const windowsproxy::Summary summary = windowsproxy::summarize(ie);
    const QString               detect  = summary.autoDetect ? QLatin1String("; ") + i18n::t("automatic detection on") : QString();
    switch (summary.kind) {
    case windowsproxy::Summary::Kind::HttpProxy:
        return i18n::t("HTTP proxy %1").arg(placeText(placeOf(hostOf(summary.hostPort)))) + detect;
    case windowsproxy::Summary::Kind::SocksOnly:
        return i18n::t("SOCKS only, %1").arg(placeText(placeOf(summary.socksHost))) + detect;
    case windowsproxy::Summary::Kind::Script:
        return i18n::t("automatic detection");
    case windowsproxy::Summary::Kind::None:
        break;
    }
    return i18n::t("none");
}

QString pausedText(int count)
{
    return count == 1 ? i18n::t("1 automatic load paused") : i18n::t("%1 automatic loads paused").arg(count);
}

QString routeLine(const DiagnosticsInput& in)
{
    if (in.refused == Failure::PasswordUnreadable)
        return i18n::t("password unreadable");
    if (in.refused != Failure::None)
        return i18n::t("refused (%1)").arg(failureText(in.refused));
    switch (in.health) {
    case Health::State::Unknown:
        return i18n::t("not checked yet");
    case Health::State::Working:
        return i18n::t("working");
    case Health::State::Down:
        return i18n::t("down for %1 (%2), %3")
            .arg(diagnosticsDuration(in.downForMs), failureText(in.lastFailure), pausedText(in.pausedLoads));
    case Health::State::Probation:
        return i18n::t("trying again after %1 down (%2), %3")
            .arg(diagnosticsDuration(in.downForMs), failureText(in.lastFailure), pausedText(in.pausedLoads));
    }
    return i18n::t("not checked yet");
}

QString bridgeLine(const DiagnosticsInput& in)
{
    switch (in.bridge) {
    case DiagnosticsInput::BridgeState::NotStarted:
        return i18n::t("not started");
    case DiagnosticsInput::BridgeState::Failed:
        return i18n::t("couldn't start");
    case DiagnosticsInput::BridgeState::Running:
        break;
    }
    const DiagnosticsInput::Counters& c = in.counters;
    // The failures in the table's order (failure.h), each with its count; none: "none".
    QList<int> kinds = c.failures.keys();
    std::sort(kinds.begin(), kinds.end());
    QStringList failures;
    for (int kind : qAsConst(kinds)) {
        const quint64 n = c.failures.value(kind);
        if (n)
            failures << QString::fromLatin1("%1 %2").arg(QString::fromLatin1(failureName(static_cast<Failure>(kind)))).arg(n);
    }
    return i18n::t("running, tunnels open %1, since start %2 (refused %3, busy %4, unauthenticated %5); failures: %6")
        .arg(c.tunnelsOpen)
        .arg(c.accepted)
        .arg(c.refused)
        .arg(c.busy)
        .arg(c.unauthenticated)
        .arg(failures.isEmpty() ? i18n::t("none") : failures.join(QLatin1String(", ")));
}

} // namespace

QString diagnosticsTitle()
{
    return i18n::t("Chat proxy");
}

QString diagnosticsDuration(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    if (s < 60)
        return i18n::t("%1 s").arg(s);
    const qint64 min = s / 60;
    if (min < 60)
        return i18n::t("%1 min").arg(min);
    return i18n::t("%1 h %2 min").arg(min / 60).arg(min % 60);
}

QStringList diagnosticLines(const DiagnosticsInput& in)
{
    QStringList lines;
    const Config& c = in.saved;
    // Mode: what the published route is now.
    switch (in.mode) {
    case Mode::Unset:
        lines << i18n::t("Mode: not started");
        break;
    case Mode::System:
        lines << i18n::t("Mode: Windows proxy settings");
        break;
    case Mode::Direct:
        lines << i18n::t("Mode: No proxy (direct)");
        break;
    case Mode::Custom:
        lines << i18n::t("Mode: %1 proxy %2, login: %3").arg(typeName(c.type), placeText(placeOf(c.host)), loginText(c));
        break;
    }
    if (c.saved() && !c.on)
        lines << i18n::t("Saved chat proxy: %1 %2, login: %3, off").arg(typeName(c.type), placeText(placeOf(c.host)), loginText(c));
    lines << i18n::t("Windows proxy: %1").arg(windowsLine(in.ie));
    if (in.mode == Mode::Custom)
        lines << i18n::t("Route: %1").arg(routeLine(in));
    if (in.mode == Mode::System) {
        if (in.windows.lastFailure != Failure::None)
            lines << i18n::t("Windows proxy health: last failure %1 ago (%2)")
                         .arg(diagnosticsDuration(in.nowMs - in.windows.atMs), failureText(in.windows.lastFailure));
        else
            lines << i18n::t("Windows proxy health: none");
    }
    if (!in.lastTest.ran) {
        lines << i18n::t("Last test: none");
    } else {
        const QString ago = diagnosticsDuration(in.nowWallMs - in.lastTest.wallMs);
        if (in.lastTest.ok)
            lines << i18n::t("Last test: works, %1 ms, %2 ago").arg(in.lastTest.ms).arg(ago);
        else
            lines << i18n::t("Last test: %1, %2 ago")
                         .arg(in.lastTest.failure == Failure::None ? i18n::t("didn't work") : failureText(in.lastTest.failure), ago);
    }
    lines << i18n::t("Bridge: %1").arg(bridgeLine(in));
    return lines;
}

DiagnosticsInput collectDiagnostics(const Control* control)
{
    DiagnosticsInput in;
    in.ie        = windowsproxy::ieConfig();
    in.nowWallMs = QDateTime::currentMSecsSinceEpoch();
    if (!control)
        return in;
    in.saved       = control->config();
    in.mode        = control->mode();
    in.health      = control->state();
    in.lastFailure = control->lastFailure();
    in.nowMs       = control->clockMs();
    if (control->downSinceMs() >= 0)
        in.downForMs = in.nowMs - control->downSinceMs();
    in.pausedLoads = control->pausedLoads();
    in.windows     = control->windowsStatus();
    in.lastTest    = control->lastTest();
    if (in.mode == Mode::Custom) {
        const std::shared_ptr<const Snapshot> s = snapshot();
        if (s->mode == Mode::Custom)
            in.refused = s->refused;
    }
    if (const std::shared_ptr<Bridge> bridge = control->bridge()) {
        in.bridge                    = bridge->isRunning() ? DiagnosticsInput::BridgeState::Running : DiagnosticsInput::BridgeState::NotStarted;
        const Bridge::Counters c     = bridge->counters();
        in.counters.tunnelsOpen      = c.tunnelsOpen;
        in.counters.accepted         = c.accepted;
        in.counters.refused          = c.refused;
        in.counters.busy             = c.busy;
        in.counters.unauthenticated  = c.unauthenticated;
        in.counters.failures         = c.failures;
    } else if (control->bridgeFailed()) {
        in.bridge = DiagnosticsInput::BridgeState::Failed;
    }
    return in;
}

QStringList currentDiagnosticLines()
{
    return diagnosticLines(collectDiagnostics(Control::current()));
}

} // namespace proxy
