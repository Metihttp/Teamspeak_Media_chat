#pragma once

// 2.6 chat proxy: proxy::Control, the one owner of the chat proxy (design 3.8). GUI thread only. It loads and saves
// the proxy keys (proxyconfig.h), owns the session's bridge (created the first time custom mode is used, stopped only
// by proxy::shutdown()), publishes the route snapshot (router.h) whenever the route changes, and keeps route health
// (custom mode) and the Windows-proxy status (Windows mode).
//
// CRASH RULE: one owned QTimer child (probation); no delayed QTimer::singleShot, no metatypes (signals carry no
// arguments), no QStringLiteral in long-lived state. FROZEN API (design 3.1); I2b adds the notices and the UI glue.

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "failure.h"
#include "health.h"
#include "proxyconfig.h"
#include "router.h"

class QTimer;

namespace proxy {

class Bridge;

// What the YouTube player is told (design 3.6.4): the bridge, direct, the Windows settings, or blocked (custom refused;
// or Windows mode whose manual setting names only a SOCKS proxy, S13).
struct PlayerNetwork {
    enum class Kind { Blocked, System, Direct, Bridge };
    Kind       kind       = Kind::Blocked;
    quint16    bridgePort = 0;
    QByteArray realm;
    QByteArray token;
    QByteArray user      = QByteArray("tsm-p");
    Failure    blockedBy = Failure::NotReady;
    quint64    generation = 0;
};

// A result of a Fetcher, the player or the updater, for route health (route-level failures only).
struct RouteReport {
    Failure failure  = Failure::None;
    Step    step     = Step::None;
    bool    decisive = false; // informational: health decides with isDecisive(failure, step)
    QString host;             // the site asked for (its registrable domain counts)
    quint64 generation = 0;
    Via     via        = Via::Unset;
};

// Windows mode (F2): the last failure of the Windows proxy, until a site answers again.
struct WindowsStatus {
    Failure lastFailure = Failure::None;
    qint64  atMs        = -1; // the clock's value when it happened
};

// What a turnOn / turnOff / toggle did (the printed line is I2b's).
enum class Toggle { TurnedOn, TurnedOff, AlreadyOn, AlreadyOff, NotSetUp };

class Control : public QObject
{
    Q_OBJECT

  public:
    struct Options {
        QString                                       settingsFile; // empty: <TeamSpeak config>/plugins/.../settings.ini
        std::function<qint64()>                       clockMs;      // health and the Windows status (default: monotonic)
        // tests: make the bridge (default Bridge::create with the saved port); null result: *error set
        std::function<std::shared_ptr<Bridge>(quint16 preferredPort, int* error)> bridgeFactory;
    };

    // The plugin's one Control (made on first use, GUI thread). proxy::shutdown() deletes it.
    static Control* instance();
    static void     setInstanceForTests(Control* control); // tests: instance() returns it (not owned)
    // I2b: the one that exists, never a new one (nullptr after proxy::shutdown()): the chat, the player and the plugin's
    // late paths ask this one, so nothing after the unload makes a fresh Control.
    static Control* current();

    explicit Control(const Options& options = Options(), QObject* parent = nullptr);
    ~Control() override; // shutdown()

    void          start();                       // plugin init: load the config, the bridge when on, the snapshot, health
    const Config& config() const { return m_config; } // the saved config (never a plain password)
    bool          save(const Config& next);      // the proxy window's OK: saveConfig, then apply()

    enum class Origin { Menu, Hotkey, Command, Card, NoticeLink, Settings, Window };
    Toggle turnOn(Origin origin, quint64 sch);
    Toggle turnOff(Origin origin, quint64 sch);
    Toggle toggle(Origin origin, quint64 sch);

    bool admits(bool userAsked);                  // the Fetchers' gate (5.2); always true outside custom mode
    void report(const RouteReport& report);       // results of Fetchers, player, updater (GUI thread)
    void reportOk(quint64 generation);            // custom: a site answered through the bridge
    void reportSiteResponded();                   // Windows mode: clears the Windows-proxy failure (5.2)
    void noteWaitingForClick(int count);          // ChatLinkPreviews, inside routeChanged (S4 line)
    int  takeWaitingForClick();                   // the count noted since the last take
    PlayerNetwork playerNetwork() const;
    Health::State state() const;                  // custom mode's health; Unknown otherwise
    Failure       lastFailure() const;
    WindowsStatus windowsStatus() const { return m_windows; }
    void          registerWorker(void* thread);   // Test and Find workers (HANDLE), joined at shutdown
    // I2b (design 1.5): a notice's link ("#tsmupd-proxy-<nonce>-off" turns the chat proxy off, "-set" opens the Chat
    // proxy window). True only for this load's nonce (a stale row of an earlier load opens the FAQ instead).
    bool          handleNoticeLink(const QString& href, bool activate);
    void          shutdown();                     // design 3.9: workers joined, the bridge stopped (bounded)

    // ---- I2b: the UI glue (design 1.2, 1.3.5, 1.4, 1.5) ----
    // The Chat proxy window, opened by a turn on with nothing saved (menu, command, card; never the hotkey) and by a
    // notice's "Proxy settings" link. SetUp: the checkbox checked, the server 127.0.0.1, the focus on Find my VPN app.
    enum class WindowRequest { Open, SetUp };
    void setWindowOpener(std::function<void(WindowRequest request)> opener);
    void openWindow(WindowRequest request);
    // A Test of the window: its outcome for the status row ("last test: today at 12:40") and the diagnostics. A passed
    // test of the saved values while the chat proxy is on marks the route working (design 1.3.5).
    struct LastTest {
        bool    ran     = false;
        bool    ok      = false;
        Failure failure = Failure::None;
        int     ms      = 0;
        qint64  wallMs  = 0; // when (ms since the epoch)
    };
    void     noteTest(bool ok, Failure failure, int ms, bool savedValues);
    LastTest lastTest() const { return m_lastTest; }
    QString  noticeUrl(bool turnOff) const; // the notice link of this load
    qint64   clockMs() const { return now(); }  // the clock of health and the Windows status (the status row)
    void     setNoticeNonceForTests(const QString& nonce) { m_nonce = nonce; }

    // I3 (design 2.4): the diagnostics' route line. Since when (the Control's clock) the route has been down in this episode
    // (Down, then Probation and Down again; -1 when it isn't down), and how many automatic loads the gate held back in it.
    qint64 downSinceMs() const;
    int    pausedLoads() const { return m_pausedLoads; }
    void   notePausedLoad() { ++m_pausedLoads; } // ChatLinkPreviews paused one without asking the gate (Down: never admitted)

    Mode                    mode() const;         // the published effective mode
    quint64                 generation() const { return m_generation; }
    std::shared_ptr<Bridge> bridge() const { return m_bridge; }
    bool                    bridgeFailed() const { return m_bridgeFailed; }

  signals:
    void routeChanged(); // no arguments (no metatypes)
    void healthChanged();

  private:
    void    apply();
    void    say(Toggle toggle, Origin origin, quint64 sch); // the toggle's line (1.4); a card shows its own feedback
    void    notice(int key, int kind, const QString& text, const QString& linkText, bool turnOff); // one per key in 10 min
    bool    ensureBridge();
    void    scheduleTick();
    void    onTick();
    qint64  now() const;
    QString settingsFile() const;

    Options                         m_options;
    Config                          m_config;
    Config                          m_applied;      // the config of the published route
    bool                            m_hasApplied = false;
    Health                          m_health;
    std::shared_ptr<Bridge>         m_bridge;
    bool                            m_bridgeFailed = false;
    quint64                         m_generation   = 0;
    std::shared_ptr<const Snapshot> m_published;
    QTimer*                         m_timer = nullptr; // owned: probation and the probe slot
    WindowsStatus                   m_windows;
    int                             m_waitingForClick = 0;
    bool                            m_shutDown = false;
    // I2b
    std::function<void(WindowRequest)> m_windowOpener;
    LastTest                        m_lastTest;
    QString                         m_nonce;        // 16 hex, new per plugin load (the notice links)
    QHash<int, qint64>              m_noticeAt;     // when a notice of that key was printed (the Control's clock)
    // I3
    qint64                          m_episodeAt   = -1; // when the route went down (not Probation -> Down again)
    int                             m_pausedLoads = 0;  // automatic loads admits() refused since then
};

// ts3plugin_shutdown, after our windows are gone and before the chat is deleted: Control::shutdown(), then the Control
// deleted; the snapshot back to the refusing one.
void shutdown();

} // namespace proxy
