#include "ytplayer/ytplayer.h"

#include <QAbstractScrollArea>
#include <QDir>
#include <QEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

#include "i18n.h"
#include "settings.h"
#include "ts3api.h"
#include "tsmp.h"
#include "tsmpnet.h" // 2.6 chat proxy: the browser arguments per network
#include "ytplayer/ytpage.h"

namespace {

constexpr int kPollMs     = 50;
constexpr int kCreationMs = 20000; // no Created from open(): the engine failed (3.7 step 6)
constexpr int kReadyMs    = 15000; // no Ready from Created: "Can't play this video here" (YouTube's load only)
constexpr int kHiddenMs   = 60000; // anchored, hidden this long: closed
constexpr int kSettleMs   = 150;   // anchored: shown again this long after the last scroll or window move
constexpr int kRetryMs    = 1000;  // the one engine retry
constexpr int kEngineMs   = 60000; // the engine released this long after the last player closed
constexpr int kBusyMs     = 3000;  // 2.6: the "busy" retry (the old browser process still holds the folder)
constexpr int kMaxBusy    = 3;
constexpr int kMaxTimes   = 256;   // remembered positions (video ids)
constexpr int kEscape     = 0x1B;  // VK_ESCAPE
// The Y4-spike's decision (design2 3.6): true would make the player unavailable whenever TeamSpeak's token has
// UIAccess (Play opens the browser). The spike on a standard account hasn't run yet (author-2026-11-11 item 4); the
// live run decides (Y-L16).
constexpr bool kRefuseUnderUIAccess = false;

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

const char* stateName(YouTubePlayer::State s)
{
    switch (s) {
    case YouTubePlayer::State::None:
        return "none";
    case YouTubePlayer::State::Consent:
        return "consent";
    case YouTubePlayer::State::Starting:
        return "starting";
    case YouTubePlayer::State::Open:
        return "open";
    case YouTubePlayer::State::Popup:
        return "popup";
    case YouTubePlayer::State::Hidden:
        return "hidden";
    case YouTubePlayer::State::Note:
        return "note";
    }
    return "none";
}

// 2.6 chat proxy: the browser arguments a network needs (tsmpnet.h); two networks with the same arguments share an engine.
std::wstring argsOf(const proxy::PlayerNetwork& net)
{
    switch (net.kind) {
    case proxy::PlayerNetwork::Kind::System:
        return tsmpnet::argsFor(TSMP_NET_SYSTEM, 0);
    case proxy::PlayerNetwork::Kind::Direct:
        return tsmpnet::argsFor(TSMP_NET_DIRECT, 0);
    case proxy::PlayerNetwork::Kind::Bridge:
        return tsmpnet::argsFor(TSMP_NET_BRIDGE, net.bridgePort);
    case proxy::PlayerNetwork::Kind::Blocked:
        break;
    }
    return {};
}

// The chat proxy is the route (in use, or refused in custom mode).
bool isCustom(const proxy::PlayerNetwork& net)
{
    return net.kind == proxy::PlayerNetwork::Kind::Bridge || net.blockedBy == proxy::Failure::BridgeUnavailable
           || net.blockedBy == proxy::Failure::PasswordUnreadable;
}

bool uiAccessToken()
{
    static int cached = -2;
    if (cached == -2) {
        int rid = -1;
        ytbackend::tokenInfo(&rid, &cached);
    }
    return cached == 1;
}

} // namespace

YouTubePlayer::YouTubePlayer(Host host, std::unique_ptr<PlayerBackend> backend, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
    , m_backend(std::move(backend))
    , m_gate(ytpage::kAnchoredPlayer)
{
    m_userData = ytbackend::userDataFolder();
    m_poll     = makeTimer(kPollMs, false, "poll");
    connect(m_poll, &QTimer::timeout, this, &YouTubePlayer::onPoll);
    m_creation = makeTimer(kCreationMs, true, "creation");
    connect(m_creation, &QTimer::timeout, this, [this] { engineFailure("no Created within 20 s"); });
    m_ready = makeTimer(kReadyMs, true, "ready");
    connect(m_ready, &QTimer::timeout, this, [this] {
        ts3::log(LogLevel_INFO, 0, "[ytplayer] no ready within 15 s", {});
        // 2.6 chat proxy (design 3.6.5): through the bridge, a route-level failure the bridge recorded for the player since
        // this open says why: "Can't play through the proxy", and route health hears it.
        if (m_active.net.kind == proxy::PlayerNetwork::Kind::Bridge && m_host.playerRecords) {
            const QVector<proxy::Record> records = m_host.playerRecords(m_active.openTick);
            for (int i = records.size() - 1; i >= 0; --i) {
                const proxy::Record& r = records.at(i);
                if (!proxy::isRouteLevel(r.failure))
                    continue;
                if (m_host.report) {
                    proxy::RouteReport report;
                    report.failure    = r.failure;
                    report.step       = r.step;
                    report.decisive   = proxy::isDecisive(r.failure, r.step);
                    report.host       = r.host;
                    report.generation = m_active.net.generation;
                    report.via        = proxy::Via::Bridge;
                    m_host.report(report);
                }
                noteActive(ytpage::Note::Proxy, proxy::failureName(r.failure));
                return;
            }
        }
        showNote(-1);
    });
    m_hidden = makeTimer(kHiddenMs, true, "hidden");
    connect(m_hidden, &QTimer::timeout, this, [this] { closeActive("hidden for 60 s"); });
    m_settle = makeTimer(kSettleMs, true, "settle");
    connect(m_settle, &QTimer::timeout, this, [this] {
        m_moving = false;
        placeActive(true);
    });
    m_retry = makeTimer(kRetryMs, true, "retry");
    connect(m_retry, &QTimer::timeout, this, [this] {
        m_active.retrying = false;
        if (m_active.slotId.isEmpty() || m_down)
            return;
        ts3::log(LogLevel_INFO, 0, "[ytplayer] retrying the engine", {});
        reopen();
    });
    // 2.6: the "busy" retry (an owned timer, F20): never counted toward the engine failures that disable the player.
    m_busy = makeTimer(kBusyMs, true, "busy");
    connect(m_busy, &QTimer::timeout, this, [this] {
        m_active.waiting = false;
        if (m_active.slotId.isEmpty() || m_down)
            return;
        ts3::log(LogLevel_INFO, 0, "[ytplayer] retrying the open (busy %1)", {ts3::pub(QString::number(m_active.busyRetries))});
        reopen();
    });
    m_engine = makeTimer(kEngineMs, true, "engine");
    connect(m_engine, &QTimer::timeout, this, [this] {
        if (hasPlayer() || m_down || !m_engineStarted || !m_engineLive)
            return;
        m_backend->releaseEngine();
        m_engineLive     = false;
        m_releasePending = true;
        m_releaseClock.restart();
        ts3::log(LogLevel_INFO, 0, "[ytplayer] engine released", {});
        updatePolling();
    });
}

YouTubePlayer::~YouTubePlayer()
{
    shutdown();
}

QTimer* YouTubePlayer::makeTimer(int ms, bool single, const char* name)
{
    auto* t = new QTimer(this); // owned: stops and goes with us
    t->setInterval(ms);
    t->setSingleShot(single);
    t->setObjectName(latin(name));
    return t;
}

QTimer* YouTubePlayer::timerForTests(const char* name) const
{
    for (QTimer* t : {m_poll, m_creation, m_ready, m_hidden, m_settle, m_retry, m_engine, m_busy}) {
        if (t && t->objectName() == latin(name))
            return t;
    }
    return nullptr;
}

bool YouTubePlayer::playable() const
{
    const Settings& s = Settings::instance();
    if (m_down || m_unavailable || !m_backend || !s.playYouTubeInChat || !s.showLinkPreviews || !s.inlinePreviews)
        return false;
    if constexpr (kRefuseUnderUIAccess) {
        if (uiAccessToken())
            return false;
    }
    return ytbackend::runtimePresent();
}

YouTubePlayer::Card YouTubePlayer::cardOf(const QString& slotId, const QString& videoId) const
{
    Card c;
    c.playable = playable();
    if (hasPlayer() && slotId == m_active.slotId) {
        c.state = m_active.state == State::None ? State::Starting : m_active.state;
        if (c.state == State::Hidden && m_active.lastSec > 0)
            c.resumeMs = qint64(m_active.lastSec) * 1000;
        return c;
    }
    if (!m_consentSlot.isEmpty() && slotId == m_consentSlot) {
        c.state = State::Consent;
        return c;
    }
    const auto note = m_notes.constFind(slotId);
    if (note != m_notes.constEnd()) {
        c.state = State::Note;
        c.note  = note->text;
        return c;
    }
    const int sec = m_times.value(videoId, -1);
    if (sec > 0)
        c.resumeMs = qint64(sec) * 1000;
    return c;
}

QUrl YouTubePlayer::watchUrlOf(const QString& slotId, const QString& videoId, const QUrl& link) const
{
    int sec = m_times.value(videoId, -1);
    if (hasPlayer() && slotId == m_active.slotId && m_active.lastSec > 0)
        sec = m_active.lastSec;
    if (sec > 0) {
        const QUrl url = ytpage::watchUrl(videoId, sec);
        if (url.isValid())
            return url;
    }
    return link;
}

void YouTubePlayer::toBrowser(const QUrl& link, const QString& videoId, int sec)
{
    QUrl url = sec > 0 ? ytpage::watchUrl(videoId, sec) : link;
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        url = ytpage::watchUrl(videoId, 0);
    if (m_host.openUrl && url.isValid())
        m_host.openUrl(url);
}

void YouTubePlayer::play(const QString& slotId, const QString& videoId, const QUrl& link, bool forcePopup)
{
    if (m_down || slotId.isEmpty())
        return;
    // The card's thumbnail does what its state says.
    if (hasPlayer() && slotId == m_active.slotId) {
        if (m_active.id <= 0)
            return; // starting (the retry): nothing
        if (m_active.state == State::Popup) {
            m_backend->command(m_active.id, latin("front")); // brings the window to the front
        } else if (m_active.state == State::Hidden) {
            // Resume: the click is the user's gesture.
            m_moving = false;
            placeActive(true);
            m_backend->command(m_active.id, latin("play"));
        }
        return;
    }
    if (m_notes.contains(slotId)) {
        toBrowser(link, videoId, m_times.value(videoId, -1)); // the note's "Watch on YouTube"
        return;
    }
    if (!playable()) {
        if (m_backend && !ytbackend::runtimePresent() && !m_runtimeLogged) {
            m_runtimeLogged = true;
            ts3::log(LogLevel_INFO, 0, "[ytplayer] no WebView2 Runtime: videos open in the browser", {});
        }
        toBrowser(link, videoId, m_times.value(videoId, -1));
        return;
    }
    if (m_remembered.contains(videoId)) {
        toBrowser(link, videoId, m_times.value(videoId, -1)); // a fact about the video (100, 101, 150)
        return;
    }
    if (!Settings::instance().youTubeNoticeShown) {
        // The one-time notice is a state of the card (never a dialog: nothing runs a nested event loop here).
        const QString old = m_consentSlot;
        m_consentSlot     = slotId;
        m_consentVideo    = videoId;
        m_consentLink     = link;
        m_consentPopup    = forcePopup;
        if (!old.isEmpty() && old != slotId && m_host.changed)
            m_host.changed(old);
        if (m_host.changed)
            m_host.changed(slotId);
        return;
    }
    start(slotId, videoId, link, forcePopup);
}

void YouTubePlayer::consentPlay(const QString& slotId)
{
    if (m_down || slotId.isEmpty() || slotId != m_consentSlot)
        return;
    Settings::instance().youTubeNoticeShown = true;
    Settings::instance().save();
    const QString video = m_consentVideo;
    const QUrl    link  = m_consentLink;
    const bool    popup = m_consentPopup;
    m_consentSlot.clear();
    if (!playable()) {
        if (m_host.changed)
            m_host.changed(slotId);
        toBrowser(link, video, m_times.value(video, -1));
        return;
    }
    start(slotId, video, link, popup);
}

void YouTubePlayer::consentBrowser(const QString& slotId)
{
    if (m_down || slotId.isEmpty() || slotId != m_consentSlot)
        return;
    const QString video = m_consentVideo;
    const QUrl    link  = m_consentLink;
    m_consentSlot.clear(); // asked again next time
    if (m_host.changed)
        m_host.changed(slotId);
    toBrowser(link, video, m_times.value(video, -1));
}

void YouTubePlayer::stop(const QString& slotId)
{
    if (hasPlayer() && slotId == m_active.slotId)
        closeActive("stop");
}

void YouTubePlayer::tryAgain(const QString& slotId, const QString& videoId, const QUrl& link)
{
    if (m_down)
        return;
    m_remembered.remove(videoId);
    m_notes.remove(slotId);
    if (m_host.changed)
        m_host.changed(slotId);
    play(slotId, videoId, link);
}

void YouTubePlayer::start(const QString& slotId, const QString& videoId, const QUrl& link, bool forcePopup)
{
    if (!ytpage::validId(videoId)) {
        toBrowser(link, videoId, -1);
        return;
    }
    // 3: one player at a time (its last reported time is remembered).
    if (hasPlayer())
        closeActive("another Play");
    m_notes.remove(slotId);
    // 4: TS Media's own videos, audio and voice messages pause.
    if (m_host.pauseOthers)
        m_host.pauseOthers();
    Spot spot;
    if (!m_host.locate || !m_host.locate(slotId, &spot))
        return;
    const bool anchored = !forcePopup && ytpage::canPlayInPlace(m_gate, spot.thumbDu, spot.s, spot.sysScale);
    // 5: the anchored player needs the whole thumbnail on screen.
    if (anchored && m_host.scrollIntoView) {
        m_host.scrollIntoView(slotId);
        if (!m_host.locate(slotId, &spot))
            return;
    }
    // 6: only now the helper is extracted and loaded (the first Play of the session).
    QString why;
    if (!m_backend->load(&why)) {
        m_unavailable = true; // for the session: every Play opens the browser
        m_lastFailure = why;
        toBrowser(link, videoId, m_times.value(videoId, -1));
        if (m_host.changed)
            m_host.changed(slotId);
        return;
    }
    logTokenOnce();
    if (!m_engineStarted) {
        m_engineStarted = true;
        const QString version = m_backend->runtimeVersion();
        ts3::log(LogLevel_INFO, 0, "[ytplayer] helper loaded, WebView2 %1", {ts3::pub(version.isEmpty() ? latin("unknown") : version)});
    }
    m_engine->stop();
    m_deleteAfterExit = false; // a Play: the switch is on (a pending release's answer never deletes the folder)
    int startSec = m_times.value(videoId, -1);
    if (startSec < 0)
        startSec = ytpage::startOf(link);
    m_active          = Active();
    m_active.slotId   = slotId;
    m_active.videoId  = videoId;
    m_active.link     = link;
    m_active.popup    = !anchored;
    m_active.state    = State::Starting;
    m_active.lastSec  = startSec > 0 ? startSec : -1;
    m_active.viewport = spot.viewport;
    m_active.browser  = spot.viewport ? spot.viewport->parentWidget() : nullptr;
    m_active.window   = spot.viewport ? spot.viewport->window() : nullptr;
    PlayerOpen& o     = m_active.open;
    o.owner           = spot.ownerHwnd;
    o.popup           = !anchored;
    // The popup is a window of its own: a narrow chat's column fit (s down to 0.5) would only make it and YouTube's page
    // in it smaller (live run), so it takes the chat's scale without that fit (Preview size and the text size stay).
    const double s    = anchored || spot.popupS <= 0 ? spot.s : spot.popupS;
    o.cssScale        = ytpage::cssScale(s, spot.dpr);
    if (anchored) {
        o.rect = spot.thumb;
        o.clip = spot.thumb.intersected(spot.view);
    } else {
        const QSize du = ytpage::popupSizeDu(Settings::instance().youTubeWindowSize);
        o.rect         = ytpage::popupRect(du, s, spot.dpr, spot.owner);
        o.clip         = o.rect;
    }
    // TS Media's session mute ("start videos muted" too) reaches YouTube before anything can play (mute=1); the helper's
    // web view itself stays muted until the volume command (review fix: never YouTube's default full volume first).
    o.page  = ytpage::pageHtml(videoId, startSec, !anchored, QString(), m_host.muted && m_host.muted());
    o.title = spot.title.isEmpty() ? latin("YouTube") : QString::fromUtf8("YouTube \xC2\xB7 ") + spot.title;
    m_lastClip = o.clip;
    const OpenResult opened = openActive();
    if (opened == OpenResult::Blocked) {
        noteActive(ytpage::Note::Proxy, "the route is refused"); // 2.6 (S9, S13): never an engine failure
        return;
    }
    if (opened == OpenResult::Refused) {
        engineFailure("open refused");
        return;
    }
    watch(true);
    const QSizeF du = anchored ? spot.thumbDu : QSizeF(ytpage::popupSizeDu(Settings::instance().youTubeWindowSize));
    ts3::log(LogLevel_INFO, 0, "[ytplayer] open %1 %2x%3 du s %4 css %5",
             {ts3::pub(anchored ? latin("anchored") : latin("popup")), ts3::pub(QString::number(qRound(du.width()))),
              ts3::pub(QString::number(qRound(du.height()))), ts3::pub(QString::number(s, 'f', 2)), ts3::pub(QString::number(o.cssScale, 'f', 2))});
    if (m_host.changed)
        m_host.changed(slotId);
}

proxy::PlayerNetwork YouTubePlayer::currentNetwork() const
{
    if (m_host.network)
        return m_host.network();
    proxy::PlayerNetwork blocked; // nobody said: nothing opens (fail closed, S9)
    blocked.kind      = proxy::PlayerNetwork::Kind::Blocked;
    blocked.blockedBy = proxy::Failure::NotReady;
    return blocked;
}

YouTubePlayer::OpenResult YouTubePlayer::openActive()
{
    // 2.6 chat proxy (design 3.6.3, 3.6.4): the single open site. The network first: a refused route opens nothing; an
    // engine made for other browser arguments is released and the open follows its EngineExited (not an engine failure);
    // the helper gets the network right before the open.
    const proxy::PlayerNetwork net = currentNetwork();
    m_active.net            = net;
    m_active.waiting        = false;
    m_active.waitingRelease = false;
    if (net.kind == proxy::PlayerNetwork::Kind::Blocked) {
        ts3::log(LogLevel_INFO, 0, "[ytplayer] not opened: the route is refused (%1)", {ts3::pub(latin(proxy::failureName(net.blockedBy)))});
        return OpenResult::Blocked;
    }
    if (m_engineLive && argsOf(net) != argsOf(m_engineNet)) {
        releaseEngineNow("another network");
        m_active.waiting        = true;
        m_active.waitingRelease = true;
        updatePolling();
        return OpenResult::Waiting;
    }
    if (m_backend->setNetwork(net) < 0) {
        ts3::log(LogLevel_WARNING, 0, "[ytplayer] not opened: the helper refused the network", {});
        return OpenResult::Blocked;
    }
    const int id = m_backend->open(m_active.open);
    if (id <= 0)
        return OpenResult::Refused;
    m_engineLive      = true;
    m_engineNet       = net;
    m_active.id       = id;
    m_active.shown    = false;
    m_active.openTick = GetTickCount64();
    m_creation->start();
    updatePolling();
    return OpenResult::Opened;
}

void YouTubePlayer::reopen()
{
    if (m_down || m_active.slotId.isEmpty())
        return;
    switch (openActive()) {
    case OpenResult::Opened:
    case OpenResult::Waiting:
        break;
    case OpenResult::Blocked:
        noteActive(ytpage::Note::Proxy, "the route is refused");
        break;
    case OpenResult::Refused:
        engineFailure("open refused");
        break;
    }
}

void YouTubePlayer::releaseEngineNow(const char* why)
{
    m_engine->stop();
    if (!m_engineLive || m_down)
        return;
    m_backend->releaseEngine();
    m_engineLive     = false;
    m_releasePending = true;
    m_releaseClock.restart();
    ts3::log(LogLevel_INFO, 0, "[ytplayer] engine released (%1)", {ts3::pub(latin(why))});
    updatePolling();
}

void YouTubePlayer::noteActive(ytpage::Note note, const char* why)
{
    // A note without an engine failure (2.6: the proxy; "busy" and "network" given up): the active slot shows it.
    if (m_active.slotId.isEmpty())
        return;
    const QString slot  = m_active.slotId;
    const QString video = m_active.videoId;
    ts3::log(LogLevel_INFO, 0, "[ytplayer] note: %1 (%2)", {ts3::pub(latin(note == ytpage::Note::Proxy ? "proxy" : "can't play")), ts3::pub(latin(why))});
    if (hasPlayer()) {
        closeActive("note", true);
    } else {
        watch(false);
        m_active = Active();
        if (!m_down && m_engineStarted && m_engineLive)
            m_engine->start(); // no player: a living engine goes in 60 s as after any close
    }
    NoteState n;
    n.videoId = video;
    n.text    = ytpage::noteText(note);
    m_notes.insert(slot, n);
    if (m_host.changed)
        m_host.changed(slot);
}

void YouTubePlayer::networkChanged()
{
    if (m_down)
        return;
    const proxy::PlayerNetwork now    = currentNetwork();
    const bool                 custom = isCustom(now);
    // The chat proxy entered or left (F15): the video would stall anyway (leaving custom closes every bridge tunnel).
    if (hasPlayer() && isCustom(m_active.net) != custom) {
        const QString slot = m_active.slotId;
        closeActive("network changed");
        if (m_host.feedback)
            m_host.feedback(slot, i18n::t("Closed: the chat proxy changed. Press Play again."));
    }
    if (!hasPlayer() && m_engineLive && isCustom(m_engineNet) != custom)
        releaseEngineNow("the chat proxy changed");
    // "Can't play through the proxy" said something about the route before: those cards can be played again.
    const QString proxyText = ytpage::noteText(ytpage::Note::Proxy);
    QStringList   cleared;
    for (auto it = m_notes.begin(); it != m_notes.end();) {
        if (it->text == proxyText) {
            cleared << it.key();
            it = m_notes.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString& slot : qAsConst(cleared)) {
        if (m_host.changed)
            m_host.changed(slot);
    }
}

void YouTubePlayer::engineFailure(const char* why)
{
    if (m_active.slotId.isEmpty())
        return;
    m_creation->stop();
    m_ready->stop();
    if (m_active.id > 0) {
        m_backend->close(m_active.id);
        m_active.id = 0;
    }
    ts3::log(LogLevel_WARNING, 0, "[ytplayer] engine failure: %1", {ts3::pub(latin(why))});
    if (!m_active.retried) {
        // Environment or controller: one retry after 1 s (the card keeps its ring).
        m_active.retried  = true;
        m_active.retrying = true;
        m_retry->start();
        updatePolling();
        return;
    }
    // The second one: the browser for this click, no note (an engine failure is not the video's fault).
    ++m_engineFailures;
    const Active a = m_active;
    watch(false);
    m_active = Active();
    if (m_engineFailures >= 3 && !m_unavailable) {
        m_unavailable = true;
        ts3::log(LogLevel_WARNING, 0, "[ytplayer] unavailable for the session after 3 engine failures", {});
    }
    toBrowser(a.link, a.videoId, a.lastSec);
    if (!m_down)
        m_engine->start();
    updatePolling();
    if (m_host.changed)
        m_host.changed(a.slotId);
}

void YouTubePlayer::showNote(int error)
{
    if (m_active.slotId.isEmpty())
        return;
    const QString slot  = m_active.slotId;
    const QString video = m_active.videoId;
    closeActive("error");
    NoteState n;
    n.videoId    = video;
    n.text       = ytpage::noteText(ytpage::noteFor(error));
    n.remembered = ytpage::rememberedError(error);
    if (n.remembered)
        m_remembered.insert(video, error);
    m_notes.insert(slot, n);
    if (m_host.changed)
        m_host.changed(slot);
}

void YouTubePlayer::closeActive(const char* why, bool keepCard)
{
    if (!hasPlayer())
        return;
    watch(false);
    const Active a = m_active;
    m_active       = Active();
    m_moving       = false;
    for (QTimer* t : {m_creation, m_ready, m_hidden, m_settle, m_retry, m_busy})
        t->stop();
    if (a.lastSec >= 0)
        rememberTime(a.videoId, a.lastSec);
    if (a.id > 0)
        m_backend->close(a.id);
    ts3::log(LogLevel_INFO, 0, "[ytplayer] close (%1)", {ts3::pub(latin(why))});
    if (!m_down && m_engineStarted)
        m_engine->start(); // no player left: the engine goes in 60 s
    updatePolling();
    if (!keepCard && m_host.changed)
        m_host.changed(a.slotId);
}

void YouTubePlayer::rememberTime(const QString& videoId, int sec)
{
    if (videoId.isEmpty() || sec < 0)
        return;
    if (!m_times.contains(videoId))
        m_timeOrder.append(videoId);
    m_times.insert(videoId, sec);
    while (m_timeOrder.size() > kMaxTimes)
        m_times.remove(m_timeOrder.takeFirst());
}

void YouTubePlayer::setState(State state)
{
    if (m_active.state == state)
        return;
    m_active.state = state;
    if (m_host.changed)
        m_host.changed(m_active.slotId);
}

void YouTubePlayer::updatePolling()
{
    const bool want = !m_down && (m_active.id > 0 || m_active.retrying || m_releasePending);
    if (want && !m_poll->isActive())
        m_poll->start();
    else if (!want)
        m_poll->stop();
}

void YouTubePlayer::onPoll()
{
    if (m_down) {
        m_poll->stop();
        return;
    }
    const QVector<PlayerEvent> events = m_backend->poll();
    for (const PlayerEvent& e : events) {
        if (m_down)
            break;
        handle(e);
    }
    if (m_releasePending && m_releaseClock.isValid() && m_releaseClock.elapsed() >= m_releaseWaitMs) {
        // The helper always answers a release (EngineExited); should that answer never come, the poll doesn't go on
        // for the whole session (a switched-off folder is tried now, else at the next start).
        m_releasePending = false;
        ts3::log(LogLevel_WARNING, 0, "[ytplayer] no EngineExited after a release", {});
        folderAfterRelease(false);
        if (m_active.waitingRelease) {
            m_active.waitingRelease = false;
            m_active.waiting        = false;
            reopen(); // 2.6: the open that waited for it goes on (the helper refuses it if the old engine still lives)
        }
    }
    updatePolling();
}

void YouTubePlayer::folderAfterRelease(bool taken)
{
    if (!m_deleteAfterExit)
        return;
    // Review fix: the switch may have gone on again before this answer, and a Play may have made a new environment on
    // the same folder since the release: never deleted under it. On again (or a player): the folder stays.
    if (Settings::instance().playYouTubeInChat || hasPlayer()) {
        m_deleteAfterExit = false;
        return;
    }
    if (taken || (m_engineStarted && m_engine->isActive())) {
        // Off again, but a player came and went since the release (its engine timer runs; 3: its open took the
        // environment): that engine is released now, and the folder goes after its answer.
        m_engine->stop();
        m_backend->releaseEngine();
        m_engineLive     = false;
        m_releasePending = true;
        m_releaseClock.restart();
        updatePolling();
        return;
    }
    deleteUserData();
}

void YouTubePlayer::handle(const PlayerEvent& e)
{
    if (e.kind == TSMP_EV_ENGINEEXITED) {
        m_releasePending = false;
        // 0 exited, 1 not heard in 10 s, 2 nothing to release, 3 a new Play took the environment (still in use).
        ts3::log(LogLevel_INFO, 0, "[ytplayer] engine exited (%1)", {ts3::pub(QString::number(e.value))});
        if (e.value == 3)
            m_engineLive = true;
        folderAfterRelease(e.value == 3);
        if (m_active.waitingRelease && !m_down) {
            // 2.6 (F7): the open that waited for the engine of another network goes on now.
            m_active.waitingRelease = false;
            m_active.waiting        = false;
            reopen();
        }
        return;
    }
    if (m_active.id <= 0 || e.player != m_active.id)
        return; // a closed player's late event
    switch (e.kind) {
    case TSMP_EV_CREATED:
        m_creation->stop();
        m_ready->start(); // YouTube's load only (not the engine's first start)
        m_active.shown = true;
        if (m_active.popup) {
            // TeamSpeak minimized while it was being made (Windows only hides owned windows that were visible then):
            // not shown over the desktop and never played; shown, still paused, when TeamSpeak comes back (3.8).
            const bool minimized = m_active.window && m_active.window->isMinimized();
            m_active.deferredShow = minimized;
            m_active.pauseOnReady = m_active.pauseOnReady || minimized;
            m_backend->place(m_active.id, m_active.open.rect, m_active.open.clip, m_active.open.cssScale, !minimized);
            if (minimized)
                m_backend->command(m_active.id, latin("pause")); // the page may already autoplay (its view is muted)
        } else {
            placeActive(true);
        }
        break;
    case TSMP_EV_READY: {
        m_ready->stop();
        // TeamSpeak minimized while the page was being made: paused, and the view's own mute stays until TeamSpeak is
        // back (live run: YouTube's autoplay can start after a pause sent at Ready, so the pause alone isn't enough).
        const bool hold       = m_active.pauseOnReady || (m_active.window && m_active.window->isMinimized());
        m_active.pauseOnReady = false;
        if (hold) {
            m_backend->command(m_active.id, latin("pause"));
            m_active.volumeHeld = true;
        } else {
            applyVolume();
        }
        // 2.6: through the bridge, a ready player says the route works (route health).
        if (m_active.net.kind == proxy::PlayerNetwork::Kind::Bridge && m_host.reportOk)
            m_host.reportOk(m_active.net.generation);
        if (m_active.popup)
            setState(State::Popup);
        else
            setState(m_active.hiddenByUs ? State::Hidden : State::Open);
        break;
    }
    case TSMP_EV_STATE:
        ts3::log(LogLevel_DEBUG, 0, "[ytplayer] state %1", {ts3::pub(QString::number(e.value))});
        if (e.value == 1 && m_active.window && m_active.window->isMinimized()) {
            // Never played in the background (3.8): an autoplay that started after the pause is paused again.
            ts3::log(LogLevel_INFO, 0, "[ytplayer] paused: TeamSpeak is minimized", {});
            m_backend->command(m_active.id, latin("pause"));
            break;
        }
        if (e.value == 1 && m_host.pauseOthers)
            m_host.pauseOthers(); // the user may have resumed in the player
        break;
    case TSMP_EV_TIME:
        m_active.lastSec = e.value;
        rememberTime(m_active.videoId, e.value);
        break;
    case TSMP_EV_ERROR:
        if (e.value == 153)
            ts3::log(LogLevel_WARNING, 0, "[ytplayer] error 153: YouTube refused the player's identity", {});
        else
            ts3::log(LogLevel_INFO, 0, "[ytplayer] error %1", {ts3::pub(QString::number(e.value))});
        showNote(e.value);
        break;
    case TSMP_EV_OPENURL: {
        const QUrl url(e.text, QUrl::StrictMode);
        if (url.isValid() && url.scheme() == QLatin1String("https") && m_host.openUrl)
            m_host.openUrl(url); // the player stays
        break;
    }
    case TSMP_EV_FAILED:
        ts3::log(LogLevel_WARNING, 0, "[ytplayer] failed: %1 0x%2",
                 {ts3::pub(e.text.left(16)), ts3::pub(QString::number(static_cast<quint32>(e.value), 16))});
        if (e.text == QLatin1String("network-override")) {
            // 2.6 (S9): a WebView2 override would replace the bridge: the chat proxy doesn't play.
            ts3::log(LogLevel_WARNING, 0, "[ytplayer] proxy arguments overridden by WEBVIEW2 settings", {});
            noteActive(ytpage::Note::Proxy, "WebView2 override");
        } else if (e.text == QLatin1String("network")) {
            // 2.6 (F7): the helper's environment has other arguments (or no network reached it): released and opened
            // once more; a second one in a row is a note (the proxy's in custom mode). Not an engine failure.
            if (++m_active.networkFailures >= 2) {
                noteActive(isCustom(m_active.net) ? ytpage::Note::Proxy : ytpage::Note::CantPlay, "network refused twice");
            } else {
                m_creation->stop();
                m_backend->close(m_active.id);
                m_active.id = 0;
                m_engineLive = true; // whatever the helper holds goes
                releaseEngineNow("the helper has another network");
                m_active.waiting        = true;
                m_active.waitingRelease = true;
                updatePolling();
            }
        } else if (e.text == QLatin1String("busy")) {
            // 2.6 (3.6.3): the old browser process still holds the folder: retried after 3 s, at most 3 times.
            m_creation->stop();
            m_ready->stop();
            m_backend->close(m_active.id);
            m_active.id = 0;
            if (m_active.busyRetries < kMaxBusy) {
                ++m_active.busyRetries;
                m_active.waiting = true;
                m_busy->start();
                updatePolling();
            } else {
                m_active.waiting = true; // still the active slot for the note
                noteActive(ytpage::Note::CantPlay, "still busy");
            }
        } else if (e.text == QLatin1String("environment") || e.text == QLatin1String("controller")) {
            engineFailure("environment or controller");
        } else {
            showNote(-2); // the process or the navigation: "Can't play this video here", not remembered
        }
        break;
    case TSMP_EV_CLOSEDBYUSER: {
        // The popup's close box: its size is remembered (du).
        const QSize du = ytpage::sizeDuFromDevice(e.text, m_active.open.cssScale, 1.0);
        if (m_active.popup && du.isValid()) {
            Settings::instance().youTubeWindowSize = ytpage::popupSizeText(du);
            Settings::instance().save();
        }
        closeActive("closed by the user");
        break;
    }
    case TSMP_EV_LOST:
        closeActive("the window was lost"); // the card goes back to None with the last time
        break;
    case TSMP_EV_KEY:
        if (e.value == kEscape && m_active.popup)
            closeActive("Esc");
        break;
    default:
        break;
    }
}

void YouTubePlayer::applyVolume()
{
    m_active.volumeHeld = false;
    m_backend->command(m_active.id, latin("volume:") + QString::number(qBound(0, Settings::instance().videoVolume, 100)));
    if (m_host.muted && m_host.muted())
        m_backend->command(m_active.id, latin("mute"));
}

void YouTubePlayer::placeActive(bool show)
{
    if (m_active.id <= 0 || !m_active.shown || m_active.popup)
        return; // the popup is the user's to move; Windows shows and hides it with TeamSpeak's window
    Spot spot;
    if (!m_host.locate || !m_host.locate(m_active.slotId, &spot)) {
        closeActive("the message went");
        return;
    }
    const QRect  clip = spot.thumb.intersected(spot.view);
    const double v    = ytpage::visibleFraction(spot.thumb, clip);
    const double css  = ytpage::cssScale(spot.s, spot.dpr);
    m_lastClip        = clip;
    if (v < 0.5 || !spot.shown) {
        // Never played in the background: paused and hidden (3.8).
        if (!m_active.hiddenByUs && (m_active.state == State::Open || m_active.state == State::Starting))
            m_backend->command(m_active.id, latin("pause"));
        m_active.hiddenByUs = true;
        m_backend->place(m_active.id, spot.thumb, clip, css, false);
        if (m_active.state == State::Open)
            setState(State::Hidden);
        if (!m_hidden->isActive())
            m_hidden->start();
        return;
    }
    if (m_moving || !show) {
        m_backend->place(m_active.id, spot.thumb, clip, css, false); // hidden while things move (no pause)
        return;
    }
    // Shown again, still paused when it was paused (no automatic resume).
    m_active.hiddenByUs = false;
    m_hidden->stop();
    m_backend->place(m_active.id, spot.thumb, clip, css, true);
    if (m_active.state == State::Hidden)
        setState(State::Open);
}

void YouTubePlayer::watch(bool on)
{
    QWidget* window  = m_active.window;
    QWidget* browser = m_active.browser;
    if (on) {
        if (window)
            window->installEventFilter(this);
        if (browser)
            browser->installEventFilter(this);
        if (!m_active.popup) {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(browser)) {
                for (QScrollBar* bar : {area->verticalScrollBar(), area->horizontalScrollBar()}) {
                    if (!bar)
                        continue;
                    connect(bar, &QScrollBar::valueChanged, this, [this] {
                        if (m_down || m_active.popup || m_active.id <= 0)
                            return;
                        // Hidden while it scrolls (the out-of-process content trails Qt's repaint); the 0.5 rule at once.
                        m_moving = true;
                        placeActive(false);
                        m_settle->start();
                    });
                    m_bars.append(bar);
                }
            }
        }
        return;
    }
    if (window)
        window->removeEventFilter(this);
    if (browser)
        browser->removeEventFilter(this);
    for (const QPointer<QScrollBar>& bar : qAsConst(m_bars)) {
        if (bar)
            disconnect(bar.data(), nullptr, this, nullptr);
    }
    m_bars.clear();
}

bool YouTubePlayer::eventFilter(QObject* watched, QEvent* event)
{
    if (m_down || m_active.id <= 0)
        return false;
    const QEvent::Type type = event->type();
    if (watched == m_active.window) {
        if (type == QEvent::WindowStateChange) {
            auto* w = static_cast<QWidget*>(watched);
            const bool ready = m_active.shown && m_active.state != State::Starting; // Ready has come
            if (w->isMinimized()) {
                // TeamSpeak minimized: paused (the popup is hidden by Windows; the anchored one by us). Before Ready
                // the page can't take it: Ready pauses it (pauseOnReady) and holds the volume.
                m_backend->command(m_active.id, latin("pause"));
                if (!ready)
                    m_active.pauseOnReady = true;
                if (!m_active.popup)
                    placeActive(false);
            } else {
                if (!ready)
                    m_active.pauseOnReady = false; // back before the page was ready: it starts as any Play
                if (!m_active.popup) {
                    placeActive(true);
                } else if (m_active.deferredShow) {
                    // The popup made while TeamSpeak was minimized: shown now, paused (no automatic play).
                    m_active.deferredShow = false;
                    m_backend->place(m_active.id, m_active.open.rect, m_active.open.clip, m_active.open.cssScale, true);
                }
                if (m_active.volumeHeld && ready) {
                    // Still paused; the volume (and the session mute) only now, which lifts the view's own mute.
                    m_backend->command(m_active.id, latin("pause"));
                    applyVolume();
                }
            }
        } else if (!m_active.popup) {
            if (type == QEvent::Move) {
                m_moving = true;
                placeActive(false);
                m_settle->start();
            } else if (type == QEvent::Resize || type == QEvent::Show || type == QEvent::Hide || type == QEvent::ScreenChangeInternal) {
                placeActive(true);
            }
        }
        return false;
    }
    if (watched == m_active.browser && !m_active.popup) {
        if (type == QEvent::Hide || type == QEvent::Show || type == QEvent::Resize)
            placeActive(true); // a chat or server tab switch: paused and hidden; back: shown, still paused
    }
    return false;
}

void YouTubePlayer::relayout()
{
    if (m_down)
        return;
    Spot spot;
    if (!m_consentSlot.isEmpty() && m_host.locate && !m_host.locate(m_consentSlot, &spot))
        m_consentSlot.clear(); // its message left the document
    if (!hasPlayer())
        return;
    if (!m_host.locate || !m_host.locate(m_active.slotId, &spot)) {
        closeActive("the message went");
        return;
    }
    if (!m_active.popup && !m_moving)
        placeActive(true);
}

void YouTubePlayer::forgetShownNotes()
{
    for (auto it = m_notes.begin(); it != m_notes.end();) {
        if (it->remembered)
            ++it;
        else
            it = m_notes.erase(it);
    }
}

void YouTubePlayer::settingsChanged()
{
    if (m_down)
        return;
    const Settings& s = Settings::instance();
    if (s.playYouTubeInChat)
        m_deleteAfterExit = false; // on again before a release's answer: the folder stays (review fix)
    if (s.playYouTubeInChat && s.showLinkPreviews && s.inlinePreviews)
        return;
    closeActive("switched off");
    if (!m_consentSlot.isEmpty()) {
        const QString slot = m_consentSlot;
        m_consentSlot.clear();
        if (m_host.changed)
            m_host.changed(slot);
    }
    if (s.playYouTubeInChat)
        return;
    // The switch itself went off: WebView2's folder goes once its processes have exited.
    if (m_engineStarted && !m_releasePending && m_engine->isActive()) {
        m_engine->stop();
        m_backend->releaseEngine();
        m_engineLive      = false;
        m_releasePending  = true;
        m_deleteAfterExit = true;
        m_releaseClock.restart();
        updatePolling();
    } else if (m_releasePending) {
        m_deleteAfterExit = true;
    } else if (!m_engineStarted || !m_engine->isActive()) {
        deleteUserData();
    }
}

void YouTubePlayer::deleteUserData()
{
    m_deleteAfterExit = false;
    // Both elevations' folders (review fix): the switch is one for the profile, and the other one (an elevated
    // TeamSpeak's, or a normal one's) would otherwise stay for good. A folder a browser process still runs on (ours not
    // exited yet, another TeamSpeak's player) is left whole: it goes at a later start with the switch off.
    for (const QString& folder : ytbackend::userDataFamily(m_userData)) {
        QDir dir(folder);
        if (!dir.exists())
            continue;
        const QString name = folder.section(QLatin1Char('/'), -1);
        if (ytbackend::userDataInUse(folder)) {
            ts3::log(LogLevel_INFO, 0, "[ytplayer] WebView2 folder in use, kept (%1)", {ts3::pub(name)});
            continue;
        }
        const bool ok = dir.removeRecursively();
        ts3::log(LogLevel_INFO, 0, "[ytplayer] WebView2 folder %1 (%2)", {ts3::pub(ok ? latin("deleted") : latin("not deleted (in use)")), ts3::pub(name)});
    }
}

void YouTubePlayer::playbackStarted()
{
    if (m_down || m_active.id <= 0)
        return;
    m_backend->command(m_active.id, latin("pause"));
}

void YouTubePlayer::logTokenOnce()
{
    if (m_tokenLogged)
        return;
    m_tokenLogged = true;
    int rid = -1, ui = -1;
    ytbackend::tokenInfo(&rid, &ui);
    // Y-L16/Y-L17: the live run records which folder an elevated or a normal TeamSpeak used.
    ts3::log(LogLevel_INFO, 0, "[ytplayer] token integrity 0x%1 uiaccess %2 elevated %3",
             {ts3::pub(QString::number(rid, 16)), ts3::pub(QString::number(ui)), ts3::pub(ytbackend::elevatedToken() ? latin("1") : latin("0"))});
}

void YouTubePlayer::shutdown()
{
    if (m_down)
        return;
    // Every owned timer, filter and connection goes, then the helper closes everything synchronously.
    m_down = true;
    for (QTimer* t : {m_poll, m_creation, m_ready, m_hidden, m_settle, m_retry, m_engine, m_busy}) {
        if (t)
            t->stop();
    }
    watch(false);
    m_active = Active();
    m_consentSlot.clear();
    if (m_backend)
        m_backend->shutdown();
}

QString YouTubePlayer::spike(void* owner, const QRect& ownerRect)
{
    if (m_down)
        return latin("shut down");
    if (hasPlayer())
        closeActive("spike");
    QString why;
    if (!m_backend || !m_backend->load(&why))
        return latin("helper unavailable ") + why;
    logTokenOnce();
    m_engineStarted = true;
    m_engine->stop();
    m_active        = Active();
    m_active.slotId = latin("spike");
    m_active.popup  = true;
    m_active.state  = State::Starting;
    PlayerOpen& o   = m_active.open;
    o.owner         = owner;
    o.popup         = true;
    o.rect          = ytpage::popupRect(QSize(ytpage::kPopupWidthDu, ytpage::kPopupHeightDu), 1.0, 1.0, ownerRect);
    o.clip          = o.rect;
    o.cssScale      = 1.0;
    o.page          = ytpage::spikePage();
    o.title         = latin("TS Media player spike");
    if (openActive() != OpenResult::Opened) {
        m_active = Active();
        return latin("open refused");
    }
    ts3::log(LogLevel_INFO, 0, "[ytplayer] spike opened", {});
    return latin("opened; ") + describe();
}

QString YouTubePlayer::describe() const
{
    QString engine = latin("off");
    if (m_releasePending)
        engine = latin("releasing");
    else if (m_engineStarted)
        engine = m_engine->isActive() ? latin("idle") : latin("on");
    return latin("state=%1 slot=%2 id=%3 popup=%4 time=%5 consent=%6 notes=%7 playable=%8 unavailable=%9 engine=")
               .arg(latin(stateName(hasPlayer() ? m_active.state : State::None)), m_active.slotId.isEmpty() ? latin("-") : m_active.slotId)
               .arg(m_active.id)
               .arg(m_active.popup ? 1 : 0)
               .arg(m_active.lastSec)
               .arg(m_consentSlot.isEmpty() ? 0 : 1)
               .arg(m_notes.size())
               .arg(playable() ? 1 : 0)
               .arg(m_unavailable ? 1 : 0)
           + engine;
}

QStringList YouTubePlayer::diagnosticLines() const
{
    QStringList lines;
    const Settings& s = Settings::instance();
    lines << latin("YouTube player: %1, WebView2 Runtime %2, helper %3%4")
                 .arg(s.playYouTubeInChat ? latin("on") : latin("off"), ytbackend::runtimePresent() ? latin("present") : latin("missing"),
                      ytbackend::helperBuiltIn() ? (m_unavailable ? latin("unavailable") : m_engineStarted ? latin("loaded") : latin("not loaded"))
                                                 : latin("not built in"),
                      m_lastFailure.isEmpty() ? QString() : latin(" (") + m_lastFailure + latin(")"));
    return lines;
}
