#include "control.h"

#include <windows.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTimer>

#include "bridge.h"
#include "linkpreview/hostgate.h"
#include "protect.h"
#include "proxytexts.h"
#include "ts3api.h"
#include "upstream.h"
#include "windowsproxy.h"

namespace proxy {

namespace {

Control* g_instance     = nullptr; // the plugin's (owned through proxy::shutdown())
Control* g_testInstance = nullptr; // tests (not owned)

const char* modeName(Mode mode)
{
    switch (mode) {
    case Mode::Unset:
        return "unset";
    case Mode::System:
        return "windows";
    case Mode::Direct:
        return "direct";
    case Mode::Custom:
        return "custom";
    }
    return "unset";
}

const char* placeName(Place place)
{
    switch (place) {
    case Place::ThisPc:
        return "this PC";
    case Place::LocalNetwork:
        return "local network";
    case Place::Elsewhere:
        break;
    }
    return "elsewhere";
}

} // namespace

Control* Control::instance()
{
    if (g_testInstance)
        return g_testInstance;
    if (!g_instance)
        g_instance = new Control;
    return g_instance;
}

void Control::setInstanceForTests(Control* control)
{
    g_testInstance = control;
}

Control* Control::current()
{
    return g_testInstance ? g_testInstance : g_instance;
}

Control::Control(const Options& options, QObject* parent)
    : QObject(parent)
    , m_options(options)
    , m_health(options.clockMs)
{
    m_timer = new QTimer(this); // owned (CRASH RULE)
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &Control::onTick);
    // The notice links of this load (design 1.5): a row printed by an earlier load opens the FAQ instead.
    const quint64 nonce = QRandomGenerator::system()->generate64();
    m_nonce             = QString::number(nonce, 16).rightJustified(16, QLatin1Char('0'));
}

Control::~Control()
{
    shutdown();
    if (g_testInstance == this)
        g_testInstance = nullptr;
}

qint64 Control::now() const
{
    if (m_options.clockMs)
        return m_options.clockMs();
    QElapsedTimer t;
    t.start();
    return t.msecsSinceReference();
}

QString Control::settingsFile() const
{
    if (!m_options.settingsFile.isEmpty())
        return m_options.settingsFile;
    return ts3::dataDir() + QLatin1String("/settings.ini");
}

void Control::start()
{
    if (m_shutDown)
        return;
    m_config = loadConfig(settingsFile());
    apply();
}

bool Control::save(const Config& next)
{
    if (m_shutDown)
        return false;
    Config c     = next;
    c.bridgePort = m_config.bridgePort; // TS Media's own, not the window's
    if (!c.saved())
        c.on = false;
    const bool    wasOn = m_config.on;
    const quint64 gen   = m_generation;
    m_config            = c;
    saveConfig(settingsFile(), m_config);
    apply();
    if (wasOn != m_config.on)
        say(m_config.on ? Toggle::TurnedOn : Toggle::TurnedOff, Origin::Window, 0); // the window's checkbox is a toggle too
    else
        takeWaitingForClick(); // Windows <-> direct from the window: no line, so no count is kept for a later one
    if (gen == m_generation)
        emit healthChanged(); // the same route, another saved proxy: the status rows say "Saved chat proxy: ..."
    return true;
}

Toggle Control::turnOn(Origin origin, quint64 sch)
{
    if (m_shutDown)
        return Toggle::AlreadyOff;
    if (m_config.on) {
        say(Toggle::AlreadyOn, origin, sch);
        return Toggle::AlreadyOn;
    }
    if (!m_config.saved()) {
        // Nothing changes (design 1.2): the line says so; the menu, the command and a card open the window to set one up,
        // the hotkey never does (it may fire over a full-screen game, F17d).
        say(Toggle::NotSetUp, origin, sch);
        if (origin == Origin::Menu || origin == Origin::Command || origin == Origin::Card)
            openWindow(WindowRequest::SetUp);
        return Toggle::NotSetUp;
    }
    m_config.on = true;
    saveConfig(settingsFile(), m_config);
    apply();
    say(Toggle::TurnedOn, origin, sch);
    return Toggle::TurnedOn;
}

Toggle Control::turnOff(Origin origin, quint64 sch)
{
    if (m_shutDown)
        return Toggle::AlreadyOff;
    if (!m_config.on) {
        say(Toggle::AlreadyOff, origin, sch);
        return Toggle::AlreadyOff;
    }
    m_config.on = false;
    saveConfig(settingsFile(), m_config);
    apply();
    say(Toggle::TurnedOff, origin, sch);
    return Toggle::TurnedOff;
}

void Control::say(Toggle toggle, Origin origin, quint64 sch)
{
    // Every toggle prints one TS Media line (design 1.4), local only; the card menu's and the strip's toggles show
    // their own feedback at the card instead. The S4 sentence when links were set to wait for a click.
    const int waiting = toggle == Toggle::TurnedOff || toggle == Toggle::TurnedOn ? takeWaitingForClick() : 0;
    if (origin == Origin::Card)
        return;
    const quint64 where = sch ? sch : ts3::currentConnection();
    if (where)
        ts3::printInfo(where, texts::toggleLine(toggle, m_config, waiting));
}

void Control::setWindowOpener(std::function<void(WindowRequest request)> opener)
{
    m_windowOpener = std::move(opener);
}

void Control::openWindow(WindowRequest request)
{
    if (m_windowOpener && !m_shutDown)
        m_windowOpener(request);
}

void Control::noteTest(bool ok, Failure failure, int ms, bool savedValues)
{
    m_lastTest.ran     = true;
    m_lastTest.ok      = ok;
    m_lastTest.failure = failure;
    m_lastTest.ms      = ms;
    m_lastTest.wallMs  = QDateTime::currentMSecsSinceEpoch();
    ts3::log(LogLevel_INFO, 0, "[proxy] test: %1%2",
             {ts3::pub(QString::fromLatin1(ok ? "works" : failureName(failure))), ts3::pub(ok ? QString::fromLatin1(", %1 ms").arg(ms) : QString())});
    if (ok && savedValues && m_config.on)
        reportOk(m_generation); // a passed test of the saved values: the route works (5.2)
    emit healthChanged();     // the status rows show the test
}

QString Control::noticeUrl(bool turnOff) const
{
    return QString::fromLatin1("https://github.com/Metihttp/Teamspeak_Media_chat/blob/main/docs/FAQ.md#tsmupd-proxy-") + m_nonce
           + QString::fromLatin1(turnOff ? "-off" : "-set");
}

void Control::notice(int key, int kind, const QString& text, const QString& linkText, bool turnOff)
{
    Q_UNUSED(kind);
    // Never twice for the same key within 10 minutes (clock values, no timer); only once it reached a chat.
    const qint64 t    = now();
    const auto   last = m_noticeAt.constFind(key);
    if (last != m_noticeAt.constEnd() && t - last.value() < texts::kWindowsFailureMs)
        return;
    const quint64 sch = ts3::currentConnection();
    if (!sch)
        return;
    m_noticeAt.insert(key, t);
    ts3::printInfoLink(sch, text, noticeUrl(turnOff), linkText);
}

Toggle Control::toggle(Origin origin, quint64 sch)
{
    return m_config.on ? turnOff(origin, sch) : turnOn(origin, sch);
}

bool Control::ensureBridge()
{
    if (m_bridge)
        return true;
    int error = 0;
    if (m_options.bridgeFactory) {
        m_bridge = m_options.bridgeFactory(m_config.bridgePort, &error);
    } else {
        Bridge::Options options;
        options.preferredPort = m_config.bridgePort;
        m_bridge              = Bridge::create(options, &error);
    }
    if (!m_bridge) {
        m_bridgeFailed = true;
        ts3::log(LogLevel_WARNING, 0, "[proxy] bridge couldn't start (bind %1)", {ts3::pub(error)});
        return false;
    }
    m_bridgeFailed = false;
    setLiveRecords(m_bridge);
    ts3::log(LogLevel_INFO, 0, "[proxy] bridge started", {});
    if (m_bridge->port() != m_config.bridgePort) {
        // The same port across loads keeps the player's browser arguments identical.
        m_config.bridgePort = m_bridge->port();
        saveConfig(settingsFile(), m_config);
    }
    return true;
}

void Control::apply()
{
    if (m_shutDown)
        return;
    const Mode mode = m_config.effectiveMode();
    // The same route as published (whenOff while on, the bridge port): nothing new. A custom route the bridge refused is
    // tried again.
    const bool bridgeRetry = m_published && m_published->mode == Mode::Custom && m_published->refused == Failure::BridgeUnavailable;
    if (m_hasApplied && m_published && m_applied.sameRoute(m_config) && !bridgeRetry) {
        m_applied = m_config;
        return;
    }
    auto next = std::make_shared<Snapshot>();
    next->mode      = mode;
    next->refused   = Failure::None;
    std::unique_ptr<UpstreamSpec> spec;
    if (mode == Mode::Custom) {
        next->proxyOnThisPc = placeOf(m_config.host) == Place::ThisPc;
        // The bridge exists before the custom snapshot is published (or the route is refused, never direct).
        if (!ensureBridge()) {
            next->refused = Failure::BridgeUnavailable;
        } else {
            next->bridgePort = m_bridge->port();
            next->token      = m_bridge->token();
            next->realm      = m_bridge->realm();
            spec             = std::make_unique<UpstreamSpec>();
            spec->type       = m_config.type;
            spec->host       = m_config.host;
            spec->port       = m_config.port;
            spec->user       = m_config.user;
            if (!m_config.user.isEmpty() && !m_config.passwordBlob.isEmpty()) {
                // Opened once per generation, here: an unreadable password refuses the route before any request; the
                // bridge keeps it under CryptProtectMemory. A transient error leaves the blob to the first tunnel.
                auto                      plain  = std::make_shared<protect::SecretBytes>();
                const protect::OpenResult opened = protect::open(m_config.passwordBlob, m_config.endpoint(), plain.get());
                if (opened == protect::OpenResult::Unreadable)
                    next->refused = Failure::PasswordUnreadable;
                else if (opened == protect::OpenResult::Ok)
                    spec->opened = plain;
                else
                    spec->passwordBlob = m_config.passwordBlob;
            }
        }
    }
    if (bridgeRetry && m_applied.sameRoute(m_config) && next->refused == Failure::BridgeUnavailable) {
        m_applied = m_config; // still no bridge: the same refused route
        return;
    }
    ++m_generation;
    next->generation = m_generation;
    if (spec && next->refused == Failure::None) {
        spec->generation = m_generation;
        m_bridge->setUpstream(std::move(spec)); // before the snapshot (entering custom, or custom -> custom)
    }
    m_published = next;
    publish(next);
    if (m_bridge && (mode != Mode::Custom || next->refused != Failure::None))
        m_bridge->setUpstream(nullptr); // after the snapshot: new CONNECTs get 502 off, old results are dropped
    if (mode == Mode::Custom)
        markProxyRouteChange(QDateTime::currentMSecsSinceEpoch());
    m_applied    = m_config;
    m_hasApplied = true;
    m_health.reset(m_generation, next->proxyOnThisPc);
    m_episodeAt   = -1;
    m_pausedLoads = 0;
    m_windows = WindowsStatus();
    m_timer->stop();
    if (std::shared_ptr<linkpreview::HostGate> gate = linkpreview::sharedGate())
        gate->forgetFailures();
    if (mode == Mode::Custom) {
        ts3::log(LogLevel_INFO, 0, "[proxy] mode custom (%1, %2, login %3)%4",
                 {ts3::pub(QString::fromLatin1(m_config.type == Type::Http ? "http" : "socks5")), ts3::pub(QString::fromLatin1(placeName(placeOf(m_config.host)))),
                  ts3::pub(QString::fromLatin1(m_config.user.isEmpty() ? "no" : "yes")),
                  ts3::pub(next->refused == Failure::None ? QString() : QString::fromLatin1(" refused: ") + QString::fromLatin1(failureName(next->refused)))});
    } else {
        ts3::log(LogLevel_INFO, 0, "[proxy] mode %1", {ts3::pub(QString::fromLatin1(modeName(mode)))});
    }
    emit routeChanged();
}

Mode Control::mode() const
{
    return m_published ? m_published->mode : Mode::Unset;
}

bool Control::admits(bool userAsked)
{
    if (mode() != Mode::Custom || userAsked)
        return true;
    if (m_health.tick())
        emit healthChanged();
    const bool admitted = m_health.admits(userAsked);
    if (!admitted)
        ++m_pausedLoads; // the diagnostics' "N automatic loads paused" (design 2.4)
    scheduleTick();
    return admitted;
}

void Control::report(const RouteReport& report)
{
    if (m_shutDown || report.generation != m_generation || !isRouteLevel(report.failure))
        return;
    const Mode current = mode();
    if (current == Mode::System) {
        if (report.via == Via::System || report.via == Via::Refused) {
            m_windows.lastFailure = report.failure;
            m_windows.atMs        = now();
            // One notice per kind in 10 minutes (design 1.5, F2): no health, no pausing in Windows mode.
            // The chat rule (1.4, F24): the Windows proxy's address only when it is on this PC (TeamSpeak keeps chat logs).
            const texts::Notice kind = texts::noticeFor(Mode::System, report.failure);
            notice(1000 + static_cast<int>(kind), static_cast<int>(kind),
                   texts::noticeText(kind, texts::windowChatShortText(report.failure, windowsproxy::windowsProxySummary().hostPort)),
                   texts::noticeLinkText(kind), texts::noticeTurnsOff(kind));
            emit healthChanged();
        }
        return;
    }
    if (current != Mode::Custom || (report.via != Via::Bridge && report.via != Via::Refused))
        return;
    const Health::State before = m_health.state();
    if (m_health.report(report.generation, report.failure, report.step, linkpreview::healthKey(report.host))) {
        if (m_health.state() == Health::State::Down && before != Health::State::Down) {
            const bool newEpisode = before != Health::State::Probation; // a failed probe stays in the same episode
            if (newEpisode) {
                m_episodeAt   = now();
                m_pausedLoads = 0;
            }
            ts3::log(LogLevel_INFO, 0, "[proxy] route down: %1 (custom)", {ts3::pub(QString::fromLatin1(failureName(report.failure)))});
            // One notice per Down episode (review fix: not again after each failed probe of a long outage), and never
            // twice for the same failure within 10 minutes (design 1.5).
            if (newEpisode) {
                const texts::Notice kind = texts::noticeFor(Mode::Custom, report.failure);
                notice(static_cast<int>(report.failure), static_cast<int>(kind), texts::noticeText(kind, texts::chatShortText(report.failure, m_config)),
                       texts::noticeLinkText(kind), texts::noticeTurnsOff(kind));
            }
        }
        scheduleTick();
        emit healthChanged();
    }
}

void Control::reportOk(quint64 generation)
{
    if (m_shutDown || generation != m_generation || mode() != Mode::Custom)
        return;
    const Health::State before = m_health.state();
    if (m_health.reportOk(generation)) {
        if (before == Health::State::Down || before == Health::State::Probation)
            ts3::log(LogLevel_INFO, 0, "[proxy] route works again", {});
        m_timer->stop();
        emit healthChanged();
    }
}

void Control::reportSiteResponded()
{
    if (mode() != Mode::System || m_windows.lastFailure == Failure::None)
        return;
    m_windows = WindowsStatus();
    emit healthChanged();
}

void Control::noteWaitingForClick(int count)
{
    m_waitingForClick += qMax(0, count);
}

int Control::takeWaitingForClick()
{
    const int count   = m_waitingForClick;
    m_waitingForClick = 0;
    return count;
}

PlayerNetwork Control::playerNetwork() const
{
    PlayerNetwork net;
    const std::shared_ptr<const Snapshot> s = m_published ? m_published : snapshot();
    net.generation                          = s->generation;
    switch (s->mode) {
    case Mode::Unset:
        net.blockedBy = Failure::NotReady;
        return net;
    case Mode::Direct:
        net.kind      = PlayerNetwork::Kind::Direct;
        net.blockedBy = Failure::None;
        return net;
    case Mode::System:
        if (windowsproxy::windowsProxySummary().kind == windowsproxy::Summary::Kind::SocksOnly) {
            net.blockedBy = Failure::SocksOnlyWindows; // Chromium would read it as SOCKS4 with local DNS (S13)
            return net;
        }
        net.kind      = PlayerNetwork::Kind::System;
        net.blockedBy = Failure::None;
        return net;
    case Mode::Custom:
        if (s->refused != Failure::None) {
            net.blockedBy = s->refused;
            return net;
        }
        net.kind       = PlayerNetwork::Kind::Bridge;
        net.blockedBy  = Failure::None;
        net.bridgePort = s->bridgePort;
        net.realm      = s->realm;
        net.token      = s->token;
        return net;
    }
    return net;
}

qint64 Control::downSinceMs() const
{
    const Health::State s = state();
    return s == Health::State::Down || s == Health::State::Probation ? m_episodeAt : -1;
}

Health::State Control::state() const
{
    return mode() == Mode::Custom ? m_health.state() : Health::State::Unknown;
}

Failure Control::lastFailure() const
{
    if (m_published && m_published->mode == Mode::Custom && m_published->refused != Failure::None)
        return m_published->refused;
    if (mode() == Mode::Custom)
        return m_health.state() == Health::State::Down || m_health.state() == Health::State::Probation ? m_health.lastFailure() : Failure::None;
    if (mode() == Mode::System)
        return m_windows.lastFailure;
    return Failure::None;
}

void Control::registerWorker(void* thread)
{
    proxy::registerWorker(thread);
}

bool Control::handleNoticeLink(const QString& href, bool activate)
{
    const QString off = noticeUrl(true), settings = noticeUrl(false);
    if (m_shutDown || (href != off && href != settings))
        return false; // not ours, or a row of an earlier load (the FAQ opens)
    if (!activate)
        return true;
    if (href == off) {
        ts3::log(LogLevel_INFO, 0, "[proxy] the notice's Turn off chat proxy was clicked", {});
        turnOff(Origin::NoticeLink, ts3::currentConnection()); // the links it paused then wait for a click (S4)
    } else {
        openWindow(WindowRequest::Open);
    }
    return true;
}

void Control::scheduleTick()
{
    const qint64 ms = m_health.msUntilTick();
    if (ms < 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(static_cast<int>(qBound<qint64>(1, ms + 1, 10 * 60 * 1000)));
}

void Control::onTick()
{
    if (m_shutDown)
        return;
    if (m_health.tick())
        emit healthChanged();
    scheduleTick();
}

void Control::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    m_timer->stop();
    QElapsedTimer clock;
    clock.start();
    joinWorkers(2000); // the Test and Find workers (bounded; a late one keeps its pin)
    if (m_bridge) {
        const bool done = m_bridge->stop(2000); // the listener first, then the tunnels
        ts3::log(LogLevel_INFO, 0, "[proxy] bridge stopped in %1 ms%2", {ts3::pub(clock.elapsed()), ts3::pub(QString::fromLatin1(done ? "" : " (a thread ends by itself)"))});
    }
    setLiveRecords(nullptr);
    m_bridge.reset();
    if (m_published)
        publish(std::make_shared<const Snapshot>()); // anything late fails closed
    m_published.reset();
}

void shutdown()
{
    Control* control = g_instance;
    g_instance       = nullptr;
    if (control) {
        control->shutdown();
        delete control;
    }
}

} // namespace proxy
