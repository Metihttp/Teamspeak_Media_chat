#include "proxytexts.h"

#include "i18n.h"

namespace proxy::texts {

namespace {

bool onThisPc(const Config& saved)
{
    return placeOf(saved.host) == Place::ThisPc;
}

// "host:port" / "[v6]:port" / "host" -> the host (no brackets).
QString hostOfHostPort(const QString& hostPortText)
{
    const QString s = hostPortText.trimmed();
    if (s.startsWith(QLatin1Char('['))) {
        const int close = s.indexOf(QLatin1Char(']'));
        return close > 0 ? s.mid(1, close - 1) : QString();
    }
    const int colon = s.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && s.indexOf(QLatin1Char(':')) == colon)
        return s.left(colon);
    return s;
}

} // namespace

// ---- lines ----------------------------------------------------------------------------------------------------------

QString toggleLine(Toggle toggle, const Config& saved, int waitingForClick)
{
    switch (toggle) {
    case Toggle::TurnedOn:
        return i18n::t("Chat proxy on: %1. Link previews, the YouTube player and update checks use it; TeamSpeak itself doesn't.")
            .arg(describe(saved, Where::ForChat));
    case Toggle::TurnedOff: {
        QString line = saved.whenOff == WhenOff::Direct ? i18n::t("Chat proxy off. TS Media connects directly again.")
                                                        : i18n::t("Chat proxy off. TS Media uses your Windows proxy settings again.");
        if (waitingForClick > 0)
            line += QLatin1Char(' ') + i18n::t("Previews that waited for the proxy load when you click them.");
        return line;
    }
    case Toggle::AlreadyOn:
        return i18n::t("The chat proxy is already on: %1.").arg(describe(saved, Where::ForChat));
    case Toggle::AlreadyOff:
        return i18n::t("The chat proxy is already off.");
    case Toggle::NotSetUp:
        return i18n::t("No chat proxy is set up yet. Enter one in the Chat proxy window.");
    }
    return {};
}

QString statusLine(const Config& saved, Health::State health, Failure lastFailure)
{
    if (saved.on && saved.saved()) {
        QString state;
        if (lastFailure != Failure::None && (health == Health::State::Down || health == Health::State::Probation || isRouteLevel(lastFailure)))
            state = i18n::t("not working: %1").arg(chatShortText(lastFailure, saved));
        else if (health == Health::State::Working)
            state = i18n::t("working");
        else
            state = i18n::t("not checked yet");
        return i18n::t("Chat proxy on: %1 (%2).").arg(describe(saved, Where::ForChat), state);
    }
    QString line = saved.whenOff == WhenOff::Direct ? i18n::t("Chat proxy off. TS Media connects directly.")
                                                    : i18n::t("Chat proxy off. TS Media uses your Windows proxy settings.");
    if (saved.saved())
        line += QLatin1Char(' ') + i18n::t("Saved: %1.").arg(describe(saved, Where::ForChat));
    return line;
}

QString usageLine()
{
    return i18n::t("Use /tsmedia proxy on, /tsmedia proxy off or /tsmedia proxy.");
}

QString helpLine()
{
    return i18n::t("[b]/tsmedia proxy[/b] on | off — turn the chat proxy for link previews, the YouTube player and update checks on or off");
}

QString menuOnText()
{
    return i18n::t("Turn on chat proxy");
}

QString menuOffText()
{
    return i18n::t("Turn off chat proxy");
}

QString hotkeyDescription()
{
    return i18n::t("Turn the chat proxy on or off");
}

QString chatShortText(Failure failure, const Config& saved)
{
    return shortText(failure, onThisPc(saved) && saved.saved() ? hostPort(saved.host, saved.port) : QString());
}

QString windowShortText(Failure failure, const QString& hostPortText)
{
    return shortText(failure, hostPortText);
}

QString windowChatShortText(Failure failure, const QString& hostPortText)
{
    const QString host = hostOfHostPort(hostPortText);
    return shortText(failure, !host.isEmpty() && placeOf(host) == Place::ThisPc ? hostPortText.trimmed() : QString());
}

// ---- notices --------------------------------------------------------------------------------------------------------

Notice noticeFor(Mode mode, Failure failure)
{
    if (!isRouteLevel(failure))
        return Notice::None;
    if (mode == Mode::Custom) {
        if (failure == Failure::PasswordUnreadable)
            return Notice::ChatPassword;
        if (failure == Failure::Intercepted)
            return Notice::ChatIntercepted;
        return Notice::ChatDown;
    }
    if (mode == Mode::System) {
        if (failure == Failure::SocksOnlyWindows)
            return Notice::WindowsSocksOnly;
        if (failure == Failure::UnsupportedWindowsProxy)
            return Notice::WindowsScript;
        return Notice::WindowsFailed;
    }
    return Notice::None;
}

QString noticeText(Notice notice, const QString& shortText)
{
    switch (notice) {
    case Notice::ChatDown:
        return i18n::t("The chat proxy isn't working: %1. Link previews wait until it works.").arg(shortText);
    case Notice::ChatPassword:
        return i18n::t("The chat proxy's saved password can't be read on this PC. Enter it again in the Chat proxy window.");
    case Notice::ChatIntercepted:
        return i18n::t("The chat proxy is intercepting secure connections, so previews don't load.");
    case Notice::WindowsFailed:
        return i18n::t("The proxy in your Windows settings isn't working: %1. Previews that need it load when you click them.").arg(shortText);
    case Notice::WindowsSocksOnly:
        return i18n::t("Your Windows settings name only a SOCKS proxy, which TS Media can't use this way. You can use it as the chat proxy.");
    case Notice::WindowsScript:
        return i18n::t("Your Windows proxy setup script names a proxy TS Media can't use.");
    case Notice::None:
        break;
    }
    return {};
}

QString noticeLinkText(Notice notice)
{
    return noticeTurnsOff(notice) ? i18n::t("Turn off chat proxy") : i18n::t("Proxy settings");
}

bool noticeTurnsOff(Notice notice)
{
    return notice == Notice::ChatDown || notice == Notice::ChatIntercepted;
}

// ---- the status row ---------------------------------------------------------------------------------------------------

StatusRow statusRow(const StatusInput& in)
{
    StatusRow  row;
    const bool saved = in.saved.saved();
    const QString described = saved ? describe(in.saved, Where::Window) : QString();
    if (in.saved.on && saved) {
        if (in.lastFailure == Failure::PasswordUnreadable) {
            row.icon = StatusRow::Icon::Warning;
            row.text = i18n::t("In use: %1 · the saved password can't be read on this PC. Enter it again.").arg(described);
        } else if (in.lastFailure != Failure::None && isRouteLevel(in.lastFailure)
                   && (in.health == Health::State::Down || in.health == Health::State::Probation || in.mode != Mode::Custom
                       || in.lastFailure == Failure::BridgeUnavailable)) {
            row.icon = StatusRow::Icon::Warning;
            row.text = i18n::t("In use: %1 · not working: %2. Previews wait until it works.")
                           .arg(described, windowShortText(in.lastFailure, hostPort(in.saved.host, in.saved.port)));
        } else if (in.health == Health::State::Working) {
            row.icon = StatusRow::Icon::Check;
            row.text = i18n::t("In use: %1 · working").arg(described);
            if (!in.lastTestWhen.isEmpty())
                row.text += QLatin1Char(' ') + i18n::t("(last test: %1)").arg(in.lastTestWhen);
        } else {
            row.text = i18n::t("In use: %1. Not checked yet.").arg(described);
        }
        return row;
    }
    // Off: what is in use instead.
    if (in.saved.whenOff == WhenOff::Direct) {
        row.text = i18n::t("TS Media connects directly. Sites your network blocks won't load.");
    } else {
        const bool recentFailure = in.windows.lastFailure != Failure::None && in.windows.atMs >= 0 && in.nowMs - in.windows.atMs < kWindowsFailureMs;
        if (in.summary.kind == windowsproxy::Summary::Kind::SocksOnly) {
            row.icon  = StatusRow::Icon::Warning;
            row.useIt = true;
            row.text  = i18n::t("Your Windows settings name only a SOCKS proxy (%1). TS Media can't use it this way, so previews and the YouTube player "
                               "don't load.")
                           .arg(in.summary.hostPort);
        } else if (recentFailure) {
            row.icon = StatusRow::Icon::Warning;
            row.text = i18n::t("The proxy in your Windows settings didn't work for the last preview: %1.")
                           .arg(windowShortText(in.windows.lastFailure, in.summary.hostPort));
        } else if (in.summary.kind == windowsproxy::Summary::Kind::HttpProxy) {
            row.text = i18n::t("TS Media uses the proxy in your Windows settings (%1).").arg(in.summary.hostPort);
        } else if (in.summary.kind == windowsproxy::Summary::Kind::Script) {
            row.text = i18n::t("TS Media follows your Windows proxy setup script or automatic detection.");
        } else {
            row.text = i18n::t("Windows names no proxy, so TS Media connects directly.");
        }
    }
    if (saved)
        row.text += QLatin1Char(' ') + i18n::t("Saved chat proxy: %1 (off).").arg(described);
    else if (in.group)
        row.text = i18n::t("No chat proxy is set up.") + QLatin1Char(' ') + row.text;
    return row;
}

} // namespace proxy::texts
