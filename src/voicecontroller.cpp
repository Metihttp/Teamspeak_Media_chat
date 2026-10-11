#include "voicecontroller.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>

#include "audio/capturedevice.h"
#include "audio/cues.h"
#include "audio/micguard.h"
#include "audio/waveform.h"
#include "audio/wasapicapture.h"
#include "i18n.h"
#include "medialink.h"
#include "micbutton.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"

#include <windows.h> // hold to record: the mouse button's physical state

#ifdef TSMEDIA_TESTHOOKS
#include "audio/fakecapture.h"
#endif

namespace {

constexpr int kOpenDelayMs  = 180; // the start sound plays before the microphone opens
constexpr int kSavingShowMs = 300;
constexpr int kMaxHintLines = 2;
constexpr int kMicRefreshMs = 1000; // the mic buttons follow the visible chat (connection, channel, tab)

qint64 nowMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

QString voiceDir()
{
    return ts3::dataDir() + QStringLiteral("/voice");
}

QString cuePath(bool start)
{
    return voiceDir() + (start ? QStringLiteral("/cue_start.wav") : QStringLiteral("/cue_stop.wav"));
}

// TeamSpeak's own microphone state, through the plugin API.
class TeamSpeakMic : public voice::MicEnvironment
{
  public:
    QList<quint64> connections() const override
    {
        QList<quint64> out;
        for (const uint64 sch : ts3::connections()) {
            if (ts3::isConnected(sch))
                out.append(sch);
        }
        return out;
    }
    bool readVariable(quint64 sch, Variable variable, int* value) const override
    {
        if (!ts3::funcs.getClientSelfVariableAsInt)
            return false;
        const size_t flag = variable == Variable::InputHardware ? CLIENT_INPUT_HARDWARE : CLIENT_INPUT_MUTED;
        return ts3::funcs.getClientSelfVariableAsInt(sch, flag, value) == ERROR_ok;
    }
    bool setInputMuted(quint64 sch, bool muted) override
    {
        if (!ts3::funcs.setClientSelfVariableAsInt || !ts3::funcs.flushClientSelfUpdates)
            return false;
        if (ts3::funcs.setClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, muted ? MUTEINPUT_MUTED : MUTEINPUT_NONE) != ERROR_ok)
            return false;
        const unsigned int flushed = ts3::funcs.flushClientSelfUpdates(sch, nullptr);
        return flushed == ERROR_ok || flushed == ERROR_ok_no_update;
    }
};

// What TeamSpeak captures from on sch (GUI thread).
voice::TeamSpeakCapture teamSpeakCapture(uint64 sch)
{
    voice::TeamSpeakCapture info;
    const TS3Functions&     f = ts3::funcs;
    if (!f.getCurrentCaptureMode || !f.getCurrentCaptureDeviceName || !sch)
        return info;
    char* mode = nullptr;
    if (f.getCurrentCaptureMode(sch, &mode) == ERROR_ok)
        info.mode = ts3::takeString(mode);
    char* device    = nullptr;
    int   isDefault = 0;
    if (f.getCurrentCaptureDeviceName(sch, &device, &isDefault) == ERROR_ok) {
        info.device    = ts3::takeString(device);
        info.isDefault = isDefault != 0;
        info.known     = true;
    }
    char*** list = nullptr;
    if (!info.mode.isEmpty() && f.getCaptureDeviceList && f.getCaptureDeviceList(info.mode.toUtf8().constData(), &list) == ERROR_ok && list) {
        for (int i = 0; list[i]; ++i) {
            const QString name = list[i][0] ? QString::fromUtf8(list[i][0]) : QString();
            const QString id   = list[i][1] ? QString::fromUtf8(list[i][1]) : QString();
            info.devices.append(qMakePair(name, id));
            f.freeMemory(list[i][0]);
            f.freeMemory(list[i][1]);
            f.freeMemory(list[i]);
        }
        f.freeMemory(list);
    }
    return info;
}

// 2.2 diagnostics: the kind of a microphone error, in plain words (English, like the log).
const char* captureErrorName(voice::CaptureError error)
{
    switch (error) {
    case voice::CaptureError::None:
        return "Voice messages unavailable (no Media Foundation)";
    case voice::CaptureError::PrivacyBlocked:
        return "blocked by Windows' microphone privacy setting";
    case voice::CaptureError::NoDevice:
        return "no microphone";
    case voice::CaptureError::Busy:
        return "microphone busy";
    case voice::CaptureError::Disconnected:
        return "microphone disconnected";
    case voice::CaptureError::UnsupportedFormat:
        return "unsupported microphone format";
    case voice::CaptureError::EndOfStream:
        return "the capture ended";
    case voice::CaptureError::Generic:
        break;
    }
    return "recording failed";
}

const char* sourceName(voice::DeviceChoice::Source source)
{
    switch (source) {
    case voice::DeviceChoice::Source::TeamSpeak:
        return "TeamSpeak's microphone";
    case voice::DeviceChoice::Source::DefaultCommunications:
        break;
    }
    return "Windows' default communications microphone";
}

// The mic buttons' gestures (MicButton::Handler).
bool voiceButtonPressed(QObject* receiver, MicButton* button)
{
    auto* voice = qobject_cast<VoiceController*>(receiver);
    return voice && voice->holdPressed(button);
}

void voiceButtonZone(QObject* receiver, bool cancel)
{
    if (auto* voice = qobject_cast<VoiceController*>(receiver))
        voice->holdZoneChanged(cancel);
}

void voiceButtonReleased(QObject* receiver, MicButton::Release how)
{
    if (auto* voice = qobject_cast<VoiceController*>(receiver))
        voice->holdReleased(how);
}

void voiceButtonClicked(QObject* receiver)
{
    if (auto* voice = qobject_cast<VoiceController*>(receiver))
        voice->toggle();
}

void voiceButtonHovered(QObject* receiver)
{
    if (auto* voice = qobject_cast<VoiceController*>(receiver))
        voice->updateMicButtons();
}

// The strip's notices stay about 2 s; longer when Windows is set to keep notifications longer.
int noticeMs()
{
    const int windows = ui::notificationDurationMs();
    return windows > 5000 ? windows : VoiceController::kNoticeMs;
}

} // namespace

VoiceController::VoiceController(Host host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host))
{
    m_recorder = new voice::VoiceRecorder(this);
    connect(m_recorder, &voice::VoiceRecorder::stateChanged, this, &VoiceController::onRecorderState);
    connect(m_recorder, &voice::VoiceRecorder::tick, this, &VoiceController::onRecorderTick);
    m_micEnv = std::make_unique<TeamSpeakMic>();
    m_guard  = std::make_unique<voice::MicGuard>(*m_micEnv);

    // Child timers only (never a functor single shot): they die with this object, before the DLL goes.
    m_openTimer = new QTimer(this);
    m_openTimer->setSingleShot(true);
    connect(m_openTimer, &QTimer::timeout, this, &VoiceController::openMicrophone);
    m_savingTimer = new QTimer(this);
    m_savingTimer->setSingleShot(true);
    connect(m_savingTimer, &QTimer::timeout, this, [this] {
        m_savingShown = true;
        refresh();
    });
    m_closeTimer = new QTimer(this);
    m_closeTimer->setSingleShot(true);
    connect(m_closeTimer, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::TooShort)
            discard(true);
    });
    m_micTimer = new QTimer(this);
    m_micTimer->setInterval(kMicRefreshMs);
    connect(m_micTimer, &QTimer::timeout, this, [this] {
        // A TeamSpeak microphone that couldn't be given back (the connection dropped while recording)
        // is unmuted once TeamSpeak is connected there again.
        if (m_guard->hasPending() && !m_guard->engaged()) {
            if (const int back = m_guard->retryPending()) {
                m_diagMicBack = i18n::t("given back on %1 connection(s) once connected again").arg(back); // 2.2 diagnostics
                ts3::log(LogLevel_INFO, m_target.sch, "Voice message: TeamSpeak microphone given back on %1 connection(s) once connected again", {ts3::pub(back)});
            }
        }
        if ((m_phase == Phase::Idle || m_phase == Phase::TooShort) && MicButton::anyVisible())
            updateMicButtons();
    });
    m_micTimer->start();
    m_holdTimer = new QTimer(this);
    m_holdTimer->setInterval(kHoldWatchMs);
    connect(m_holdTimer, &QTimer::timeout, this, &VoiceController::watchHold);
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    connect(m_noticeTimer, &QTimer::timeout, this, [this] {
        if (!m_hold)
            hideStrip();
    });
    MicButton::Handler handler;
    handler.pressed     = &voiceButtonPressed;
    handler.zoneChanged = &voiceButtonZone;
    handler.released    = &voiceButtonReleased;
    handler.clicked     = &voiceButtonClicked;
    handler.hovered     = &voiceButtonHovered;
    MicButton::setHandler(this, handler);

    cleanupOldRecordings();
    updateMicButtons();
}

void VoiceController::shutdown()
{
    MicButton::clearHandler(this);
    MicButton::abandonHolds(); // a held button: its application filter goes, its release does nothing
    m_micTimer->stop();
    m_openTimer->stop();
    m_savingTimer->stop();
    m_closeTimer->stop();
    m_holdTimer->stop();
    m_noticeTimer->stop();
    m_recorder->cancel(); // joins the capture or encode worker
    releaseMic();
    m_guard->retryPending(); // a dropped connection that is back by now: its microphone too
    removeRecording();
    if (m_panel) {
        disconnect(m_panel, nullptr, this, nullptr);
        delete m_panel.data();
    }
    deleteStrip(); // it lives in TeamSpeak's window
    finish();
}

QString VoiceController::diagnosticsTitle()
{
    return i18n::t("Voice messages");
}

QStringList VoiceController::diagnosticLines() const
{
    QStringList lines;
    lines << i18n::t("This session: %1 recordings started, %2 sent").arg(m_diagStarted).arg(m_diagSent);
    if (m_diagSource.isEmpty())
        lines << i18n::t("Last recording: none this session");
    else
        lines << i18n::t("Last recording: from %1 at %2 Hz").arg(m_diagSource).arg(m_diagRate);
    lines << i18n::t("Last microphone error: %1").arg(m_diagError.isEmpty() ? i18n::t("none this session") : m_diagError);
    QString mic = m_guard->engaged()      ? i18n::t("TeamSpeak's microphone: muted for a recording on %1 connection(s)").arg(m_guard->mutedConnections().size())
                  : m_guard->hasPending() ? i18n::t("TeamSpeak's microphone: still muted where the connection dropped, given back once connected again")
                                          : i18n::t("TeamSpeak's microphone: not muted by TS Media");
    if (!m_diagMicBack.isEmpty())
        mic += i18n::t("; last time %1").arg(m_diagMicBack);
    lines << mic;
    return lines;
}

VoiceController::~VoiceController()
{
    // Plugin shutdown (or the chat going away): no sounds, no chat lines; the microphone comes back.
    MicButton::clearHandler(this);
    MicButton::abandonHolds();
    m_openTimer->stop();
    m_holdTimer->stop();
    m_noticeTimer->stop();
    m_recorder->cancel();
    m_guard->release();
    m_guard->retryPending(); // connected again since the connection dropped mid-recording: given back now
    removeRecording();
    if (m_panel) {
        disconnect(m_panel, nullptr, this, nullptr);
        delete m_panel.data();
    }
    deleteStrip();
    MicButton::setSharedState(MicButton::State()); // no button goes on saying "Recording"
}

// ---- the mic buttons ---------------------------------------------------------------------------------

void VoiceController::updateMicButtons()
{
    MicButton::State state;
    switch (m_phase) {
    case Phase::Idle:
    case Phase::TooShort: { // "Too short" closes by itself; a click records again (toggle)
        QString reason;
        if (!mf::available())
            reason = i18n::t("Voice messages aren't available: Windows Media Foundation is missing.");
        else if (m_host.blockReason)
            reason = m_host.blockReason();
        state.mode   = reason.isEmpty() ? MicButton::Mode::Ready : MicButton::Mode::Unavailable;
        state.reason = reason;
        break;
    }
    case Phase::Starting:
    case Phase::Recording:
        if (m_hold && m_holdDown) {
            state.mode   = MicButton::Mode::Holding;
            state.cancel = m_holdCancel;
        } else if (m_hold) { // released: stopping, then sent (or kept in the window)
            state.mode   = MicButton::Mode::Busy;
            state.reason = m_sendAfterSave ? i18n::t("Sending the voice message…") : i18n::t("Saving the voice message…");
        } else {
            state.mode = MicButton::Mode::Recording; // the window records: a click stops and sends
        }
        break;
    case Phase::Saving:
    case Phase::Error:
        state.mode = MicButton::Mode::Busy; // a click shows the window (toggle)
        if (m_hold) // no window: what happens instead
            state.reason = m_sendAfterSave ? i18n::t("Sending the voice message…") : i18n::t("Saving the voice message…");
        break;
    }
    MicButton::setSharedState(state);
}

void VoiceController::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    updateMicButtons();
}

void VoiceController::toggle()
{
    if (m_hold)
        return; // a held recording being stopped or sent: there is no window to show, nothing to stop
    switch (m_phase) {
    case Phase::Idle:
    case Phase::TooShort:
        break; // a press records (holdPressed); this was one it didn't take (right after a tap)
    case Phase::Starting:
    case Phase::Recording:
        // A double click on the button starts and would stop at once: its second click is ignored.
        if (nowMs() - m_startedMs < qMax(500, QGuiApplication::styleHints()->mouseDoubleClickInterval())) {
            bringForward(false);
            break;
        }
        // Stop and send. Still Starting (a microphone slow to open): nothing was recorded, so the window
        // says "Too short" instead of the click doing nothing.
        requestSend();
        break;
    case Phase::Saving: // Busy: the window shows what happens (it may offer Send rather than send)
    case Phase::Error:
        bringForward(true);
        break;
    }
}

// ---- starting ------------------------------------------------------------------------------------

void VoiceController::start(Origin origin)
{
    if (m_phase == Phase::TooShort)
        discard(true); // nothing was recorded: no reason to wait until "Too short" closes by itself
    if (m_phase != Phase::Idle) {
        bringForward(true);
        return;
    }
    ChatTarget target;
    QString    description;
    QWidget*   anchor = nullptr;
    if (!m_host.target || !m_host.target(&target, &description, &anchor)) {
        updateMicButtons(); // the chat says why; the buttons say it too
        return;
    }
    m_origin       = origin;
    m_target       = target;
    m_targetText   = description;
    m_targetTextMs = nowMs();
    m_anchor       = anchor;
    hideStrip(); // a hold's notice
    setPhase(Phase::Starting);
    createPanel();
    if (!mf::available()) {
        showError(voice::CaptureError::None, 0); // "Voice messages aren't available"
        return;
    }
    beginRecording();
}

void VoiceController::bringForward(bool activate)
{
    if (!m_panel)
        return;
    m_panel->show();
    m_panel->raise();
    if (activate)
        m_panel->activateWindow();
}

// ---- hold to record (the mic button) ----------------------------------------------------------------

bool VoiceController::primaryButtonHeld()
{
    // The physical buttons: with swapped buttons (left-handed) the primary one is the right one.
    const int key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool VoiceController::holdPressed(MicButton* button)
{
    if (!button)
        return false;
    // The second press of a double click, or a press right after a tap: the hint stays, nothing starts.
    const qint64 rapid = qMax<qint64>(400, QGuiApplication::styleHints()->mouseDoubleClickInterval());
    if (m_lastTapMs >= 0 && nowMs() - m_lastTapMs < rapid)
        return false;
    if (m_phase == Phase::TooShort)
        discard(true); // the window's "Too short": nothing in it to keep
    if (m_phase != Phase::Idle)
        return false; // the window's flow: a click (toggle) stops and sends, or shows the window
    ChatTarget target;
    QString    description;
    QWidget*   anchor = nullptr;
    if (!m_host.target || !m_host.target(&target, &description, &anchor)) {
        updateMicButtons(); // the chat says why; the buttons say it too
        return false;
    }
    QWidget* input = button->parentWidget();
    m_origin       = Origin::Button;
    m_target       = target;
    m_targetText   = description;
    m_targetTextMs = nowMs();
    m_anchor       = input ? input : anchor; // a window taking over sits above this input
    m_hold         = true;
    m_holdDown     = true;
    m_holdCancel   = false;
    m_holdPressMs  = nowMs();
    m_holdUpTicks  = 0;
    m_holdTicks    = 0;
    m_holdButton   = button;
    m_stripInput   = input;
    m_noticeTimer->stop();
    ts3::log("Voice message: the microphone button is held", LogLevel_INFO, m_target.sch);
    if (!mf::available()) {
        showError(voice::CaptureError::None, 0); // the window says why (the button is greyed out anyway)
        return true;
    }
    beginRecording(); // the microphone opens at once
    if (m_hold)       // (no microphone at all: the window took over)
        m_holdTimer->start();
    return true;
}

void VoiceController::holdZoneChanged(bool cancel)
{
    if (!m_hold || !m_holdDown || m_holdCancel == cancel)
        return;
    m_holdCancel = cancel;
    updateMicButtons();
    refresh();
}

void VoiceController::holdReleased(MicButton::Release how)
{
    if (!m_hold || !m_holdDown)
        return; // it ended already (5:00, a problem, the window took over): this release means nothing
    if (how == MicButton::Release::Lost) {
        holdLost(Stopped::HoldLost);
        return;
    }
    const bool tap = nowMs() - m_holdPressMs < kMinMs; // the button is held to record
    if (how == MicButton::Release::Send && !tap && visibleChatChanged()) {
        holdLost(Stopped::ChatChanged); // let go after another chat came up (before the watch saw it): not sent there
        return;
    }
    m_holdDown   = false;
    m_holdCancel = false;
    if (how == MicButton::Release::Cancel || how == MicButton::Release::Escape) {
        ts3::log(how == MicButton::Release::Escape ? "Voice message: canceled with Esc" : "Voice message: canceled (released away from the button)", LogLevel_INFO,
                 m_target.sch);
        endHold(HoldStrip::Icon::Canceled, i18n::t("Canceled — nothing was sent."), false);
        return;
    }
    if (tap) {
        m_lastTapMs = nowMs();
        ts3::log("Voice message: a tap on the microphone button, nothing recorded", LogLevel_INFO, m_target.sch);
        endHold(HoldStrip::Icon::Info, i18n::t("Hold to record, release to send"), true);
        return;
    }
    m_confirm       = false;
    m_sendAfterSave = true; // like the window's Send: stopped, encoded, sent at once
    updateMicButtons();
    stopRecording();
    refresh();
}

void VoiceController::holdLost(Stopped why)
{
    if (!m_hold || !m_holdDown)
        return;
    m_holdDown   = false;
    m_holdCancel = false;
    if (m_holdButton)
        m_holdButton->abandonHold(); // a release that comes after all does nothing
    if (why == Stopped::ChatChanged)
        ts3::log("Voice message: another chat came up while the microphone button was held; kept, not sent", LogLevel_INFO, m_target.sch);
    else
        ts3::log("Voice message: the microphone button was let go where TeamSpeak didn't see it; kept, not sent", LogLevel_INFO, m_target.sch);
    m_stopped       = why;
    m_sendAfterSave = false; // never sent by itself: the window offers Send (showKept)
    m_confirm       = false;
    updateMicButtons();
    stopRecording(); // too short: the strip says so
    refresh();
}

void VoiceController::watchHold()
{
    if (!m_hold) {
        m_holdTimer->stop();
        return;
    }
    if (m_holdDown) {
        if (!m_holdButton) { // its input went (TeamSpeak closing): nothing to keep it for
            ts3::log("Voice message: the chat input went while the button was held; canceled", LogLevel_INFO, m_target.sch);
            discard(true);
            return;
        }
        bool lost = QApplication::activePopupWidget() != nullptr; // a menu took the mouse
        if (m_host.pointerHeld) {
            m_holdUpTicks = m_host.pointerHeld() ? 0 : m_holdUpTicks + 1;
            lost          = lost || m_holdUpTicks >= kHoldUpTicks; // up, and no release came meanwhile
        }
        if (lost) {
            holdLost(Stopped::HoldLost);
            return;
        }
        // The input's chat tabs share it: another chat may have come up (a key, the wheel on the tabs).
        if (++m_holdTicks % kHoldChatTicks == 0 && visibleChatChanged()) {
            holdLost(Stopped::ChatChanged);
            return;
        }
    }
    if (m_strip && m_stripInput && m_strip->isVisible())
        m_strip->placeAbove(m_stripInput); // TeamSpeak's window resized meanwhile
}

bool VoiceController::visibleChatChanged() const
{
    // Disconnected, TeamSpeak's tabs tell nothing reliable (and the release keeps the recording anyway).
    if (!m_host.visibleTarget || !ts3::isConnected(m_target.sch))
        return false;
    const ChatTarget now = m_host.visibleTarget();
    if (now.sch != m_target.sch || now.mode != m_target.mode)
        return true;
    if (m_target.mode != TextMessageTarget_CLIENT)
        return false;
    if (!m_target.clientUid.isEmpty() && !now.clientUid.isEmpty())
        return now.clientUid != m_target.clientUid; // client ids are reused: the person counts
    return now.clientId != m_target.clientId;
}

void VoiceController::endHold(HoldStrip::Icon icon, const QString& notice, bool quiet)
{
    discard(quiet); // the microphone given back; finish() ends the hold
    if (m_hold)
        finish();
    showNotice(icon, notice);
}

void VoiceController::toWindow()
{
    if (!m_hold)
        return; // the window's own flow
    if (m_holdButton)
        m_holdButton->abandonHold(); // first: the window takes the activation, which would end it as Lost
    m_hold        = false;
    m_holdDown    = false;
    m_holdCancel  = false;
    m_holdButton  = nullptr;
    m_holdUpTicks = 0;
    m_holdTicks   = 0;
    m_holdTimer->stop();
    hideStrip();
    createPanel(); // above the held input (m_anchor), activated: Enter sends, Esc closes
}

void VoiceController::refreshHold()
{
    if (!m_hold || !m_stripInput)
        return;
    QWidget* input = m_stripInput.data();
    if (!input->isVisible()) { // another server tab hid it (the hold is lost): never over another chat
        hideStrip();
        return;
    }
    if (!m_strip)
        m_strip = new HoldStrip(input->window());
    HoldStrip::View v;
    v.limitMs            = kMaxMs;
    v.animate            = ui::animationsEnabled();
    const bool   live    = m_phase == Phase::Starting || m_phase == Phase::Recording;
    const qint64 elapsed = live ? m_recorder->elapsedMs() : m_lengthMs;
    v.timeMs             = elapsed;
    v.timeWarning        = elapsed >= kWarnAtMs;
    v.liveBins           = m_recorder->bins(qMax(0, m_recorder->binCount() - 200));
    if (live && m_holdDown) {
        v.mode = m_holdCancel ? HoldStrip::Mode::Cancel : HoldStrip::Mode::Recording;
        if (!ts3::isConnected(m_target.sch))
            v.problem = i18n::t("Not connected · release to keep it");
        else if (m_phase == Phase::Recording && elapsed >= 3000 && m_recorder->loudestDb() < waveform::kQuietDb)
            v.problem = i18n::t("No sound from the microphone");
        else if (v.timeWarning)
            v.problem = i18n::t("Sent by itself at %1 · release to send").arg(formatDuration(kMaxMs));
    } else {
        v.mode = HoldStrip::Mode::Saving;
        v.text = !m_sendAfterSave ? i18n::t("Saving…") // kept in the window once saved
                 : m_limitHit     ? i18n::t("Reached the %1 limit, sending…").arg(formatDuration(kMaxMs))
                                  : i18n::t("Sending…");
    }
    m_strip->setView(v);
    m_strip->placeAbove(input);
    if (!m_strip->isVisible())
        m_strip->show();
}

void VoiceController::showNotice(HoldStrip::Icon icon, const QString& text)
{
    QWidget* input = m_stripInput.data();
    if (!input || !input->isVisible())
        return;
    if (!m_strip)
        m_strip = new HoldStrip(input->window());
    HoldStrip::View v;
    v.mode    = HoldStrip::Mode::Notice;
    v.icon    = icon;
    v.text    = text;
    v.animate = ui::animationsEnabled();
    m_strip->setView(v);
    m_strip->placeAbove(input);
    m_strip->show();
    m_noticeTimer->start(noticeMs());
}

void VoiceController::hideStrip()
{
    m_noticeTimer->stop();
    if (m_strip && m_strip->isVisible())
        m_strip->hide();
}

void VoiceController::deleteStrip()
{
    m_noticeTimer->stop();
    if (m_strip)
        delete m_strip.data(); // a child of TeamSpeak's window: it must not outlive the DLL
    m_strip = nullptr;
}

void VoiceController::createPanel()
{
    if (m_panel)
        return;
    auto* panel = new VoicePanel(nullptr); // its own top-level window: visible even when TeamSpeak is minimized
    if (m_host.offscreen)
        panel->setAttribute(Qt::WA_DontShowOnScreen);
    m_panel = panel;
    connect(panel, &VoicePanel::sendRequested, this, &VoiceController::requestSend);
    connect(panel, &VoicePanel::cancelRequested, this, &VoiceController::requestCancel);
    connect(panel, &VoicePanel::keepRequested, this, [this] {
        m_confirm = false;
        if (!m_encoded)
            refresh(); // recording (or saving) goes on
        else if (m_sendAfterSave)
            doSend(); // Send was pressed, or 5:00 came, while it asked: it goes now
        else
            showKept(); // it stopped by itself while it asked: Send or close
    });
    connect(panel, &VoicePanel::discardConfirmed, this, [this] { discard(false); });
    connect(panel, &VoicePanel::fixRequested, this, &VoiceController::fix);
    connect(panel, &VoicePanel::closeRequested, this, &VoiceController::requestClose);
    // Deleted by someone else (plugin shutdown): the microphone must not stay open without it.
    connect(panel, &QObject::destroyed, this, [this] {
        if (m_phase != Phase::Idle)
            discard(true);
    });
    refresh(); // start() set Starting; a hold handed over (toWindow) keeps its phase
    panel->placeNear(m_anchor);
    panel->show();
    panel->raise();
    panel->activateWindow(); // Enter sends, Esc cancels
}

void VoiceController::beginRecording()
{
    ++m_diagStarted; // 2.2 diagnostics
    removeRecording();
    m_sendAfterSave  = false;
    m_encoded        = false;
    m_confirm        = false;
    m_savingShown    = false;
    m_playbackPaused = false;
    m_limitHit       = false;
    m_notConnected   = false;
    m_stopped        = Stopped::None;
    m_lengthMs       = 0;
    m_bytes          = 0;
    m_startedMs      = nowMs();
    m_levels.clear();
    m_device.reset();
    setPhase(Phase::Starting);
    if (m_host.pauseAllPlayback)
        m_host.pauseAllPlayback();
    m_muted = m_guard->engage() > 0; // always: people in the channel must not hear you live
    ts3::log(LogLevel_INFO, m_target.sch, "Voice message: recording starts (TeamSpeak microphone muted on %1 connection(s))", {ts3::pub(m_guard->mutedConnections().size())});
    refresh();
    playCue(true);
    if (m_hold)
        openMicrophone(); // held: at once, never waiting for the sound (the first word must not be clipped)
    else
        m_openTimer->start(kOpenDelayMs);
}

voice::VoiceRecorder::BackendFactory VoiceController::teamSpeakMicrophone(quint64 sch, std::shared_ptr<voice::DeviceChoice> device)
{
    const voice::TeamSpeakCapture teamSpeak = teamSpeakCapture(sch);
    return [teamSpeak, device]() -> std::unique_ptr<voice::CaptureBackend> {
        // On the worker (COM is set up there): which microphone, then open it.
        *device = voice::chooseCaptureDevice(teamSpeak, voice::listCaptureEndpoints());
        return std::make_unique<voice::WasapiCapture>(device->endpointId);
    };
}

void VoiceController::openMicrophone()
{
    if (m_phase != Phase::Starting)
        return;
    if (m_hold ? !m_holdButton : (!m_panel || !m_panel->isVisible())) { // never record without the window (or the held button)
        discard(true);
        return;
    }
    auto device = std::make_shared<voice::DeviceChoice>();
    m_device    = device;
    voice::VoiceRecorder::BackendFactory factory;
    if (m_host.microphone)
        factory = m_host.microphone(m_target.sch, device);
#ifdef TSMEDIA_TESTHOOKS
    // Test builds: <data dir>/voice_fake.txt ("<ms>[;send][;silence][;unplug@<ms>][;fast]", written by the
    // selftest_voice.txt hook for localhost servers only) records generated sound, never the microphone.
    // "fast": the sound comes as fast as it is read (5:00 in a moment: the limit without the wait).
    QFile fake(ts3::dataDir() + QStringLiteral("/voice_fake.txt"));
    if (fake.open(QIODevice::ReadOnly)) {
        const QStringList parts = QString::fromUtf8(fake.readAll()).trimmed().split(QLatin1Char(';'));
        fake.close();
        fake.remove();
        if (parts.contains(QStringLiteral("send")))
            m_sendAfterSave = true;
        voice::FakeCapture::Options options;
        options.levelDb    = -18.0;
        options.endAfterMs = parts.value(0).toLongLong();
        for (const QString& p : parts) {
            if (p == QLatin1String("silence"))
                options.signal = voice::FakeCapture::Signal::Silence;
            else if (p == QLatin1String("fast"))
                options.realtime = false;
            else if (p.startsWith(QLatin1String("unplug@")))
                options.unplugAfterMs = p.mid(7).toLongLong();
        }
        factory = [options, device]() -> std::unique_ptr<voice::CaptureBackend> {
            device->source = voice::DeviceChoice::Source::TeamSpeak;
            return std::make_unique<voice::FakeCapture>(options);
        };
        ts3::log("[test] voice message from generated sound");
    } else {
        // Test builds never record from a real microphone (a click on the mic button during a live test
        // must not pick up the room): no voice_fake.txt, no recording ("Recording failed").
        factory = nullptr;
        ts3::log("[test] no voice_fake.txt: a test build never records from the microphone");
    }
#endif
    if (!factory) {
        ts3::log("Voice message: no microphone to record from", LogLevel_WARNING, m_target.sch);
        releaseMic(); // nothing records: TeamSpeak's microphone comes back now, not when the error closes
        showError(voice::CaptureError::Generic, 0);
        return;
    }
    m_recorder->start(factory, kMaxMs);
}

// ---- the recorder ------------------------------------------------------------------------------------

void VoiceController::onRecorderState()
{
    using State = voice::VoiceRecorder::State;
    switch (m_recorder->state()) {
    case State::Recording:
        if (m_phase == Phase::Starting) {
            setPhase(Phase::Recording);
            const voice::CaptureFormat format = m_recorder->format();
            m_diagSource = QString::fromLatin1(m_device ? sourceName(m_device->source) : "?"); // 2.2 diagnostics
            m_diagRate   = format.sampleRate;
            ts3::log(LogLevel_INFO, m_target.sch, "Voice message: recording from %1 at %2 Hz", {ts3::pub(m_diagSource), ts3::pub(format.sampleRate)});
        }
        refresh();
        break;
    case State::CaptureFailed: {
        releaseMic();
        const voice::CaptureResult error = m_recorder->captureError();
        ts3::log(LogLevel_WARNING, m_target.sch, "Voice message: the microphone failed (%1, error %2)", {ts3::pub(error.detail), ts3::pub(QString::number(static_cast<quint32>(error.code), 16))});
        m_recorder->cancel();
        showError(error.error == voice::CaptureError::None ? voice::CaptureError::Generic : error.error, error.code);
        break;
    }
    case State::Captured: {
        releaseMic();
        playCue(false);
        m_lengthMs     = m_recorder->elapsedMs();
        m_limitHit     = m_recorder->limitReached();
        const bool lost      = m_recorder->deviceLost();
        const bool canceling = m_hold && m_holdDown && m_holdCancel; // held in the cancel zone right now
        if (m_hold && m_holdDown) {
            // Ended while the button is still held (5:00, the microphone gone): its release means nothing now.
            m_holdDown   = false;
            m_holdCancel = false;
            if (m_holdButton)
                m_holdButton->abandonHold();
        }
        if (m_lengthMs < kMinMs || (lost && m_lengthMs < kKeepOnLossMs)) {
            m_recorder->cancel();
            if (lost) {
                showError(voice::CaptureError::Disconnected, 0);
            } else if (m_hold) {
                endHold(HoldStrip::Icon::Info, i18n::t("Too short to send — nothing was sent."), true); // the stop sound played
            } else {
                m_confirm = false;
                setPhase(Phase::TooShort);
                refresh();
                m_closeTimer->start(ui::notificationDurationMs());
            }
            return;
        }
        if (lost)
            m_stopped = Stopped::DeviceLost;
        else if (m_limitHit && canceling)
            m_stopped = Stopped::LimitWhileCanceling; // the strip said "Release to cancel": kept, never sent by itself
        else if (m_limitHit)
            m_sendAfterSave = true; // 5:00: it is sent by itself (the window says so)
        else if (!m_sendAfterSave && m_stopped == Stopped::None)
            m_stopped = Stopped::Ended; // the sound ended by itself (generated sound in test builds)
        if (m_stopped != Stopped::None)
            m_sendAfterSave = false; // not asked for: the window offers to send it
        m_levels = m_recorder->waveformLevels();
        encode();
        break;
    }
    case State::Encoded:
        m_savingTimer->stop();
        m_bytes   = m_recorder->encodedBytes();
        m_encoded = true;
        ts3::log(LogLevel_INFO, m_target.sch, "Voice message: %1 ms encoded, %2 bytes", {ts3::pub(m_lengthMs), ts3::pub(m_bytes)});
        if (m_confirm)
            refresh(); // "Discard this voice message?" is open: Keep sends it, Discard deletes it
        else if (m_sendAfterSave)
            doSend();
        else
            showKept();
        break;
    case State::EncodeFailed:
        m_savingTimer->stop();
        ts3::log(LogLevel_WARNING, m_target.sch, "Voice message: encoding failed: %1", {ts3::pub(m_recorder->encodeError())});
        m_path.clear(); // the writer removed its partial file
        m_encoded = false;
        showSaveError();
        break;
    case State::Idle:
    case State::Opening:
    case State::Encoding:
        break;
    }
}

void VoiceController::onRecorderTick()
{
    if (m_phase != Phase::Recording)
        return;
    if (m_hold) { // held: watchHold() looks after the button
        refresh();
        return;
    }
    if (!m_panel) {
        discard(true);
        return;
    }
    if (!m_panel->isVisible() || m_panel->isMinimized()) {
        // The window must be on screen while the microphone is open: stop and keep what was said.
        m_stopped       = Stopped::WindowHidden;
        m_sendAfterSave = false;
        stopRecording();
        m_panel->showNormal();
        m_panel->raise();
        return;
    }
    refresh();
}

void VoiceController::stopRecording()
{
    if (m_phase != Phase::Starting && m_phase != Phase::Recording)
        return;
    if (m_openTimer->isActive() || m_recorder->state() == voice::VoiceRecorder::State::Idle) {
        // Stopped before the microphone was even open: nothing to send.
        m_openTimer->stop();
        m_recorder->cancel();
        releaseMic();
        m_confirm = false;
        if (m_hold) {
            endHold(HoldStrip::Icon::Info, i18n::t("Too short to send — nothing was sent."), true);
            return;
        }
        setPhase(Phase::TooShort);
        refresh();
        m_closeTimer->start(ui::notificationDurationMs());
        return;
    }
    m_recorder->stop(); // -> Captured (onRecorderState)
}

void VoiceController::encode()
{
    removeRecording();
    m_encoded = false;
    m_path    = newRecordingPath();
    m_recorder->encode(m_path);
    setPhase(Phase::Saving);
    m_savingShown = false;
    m_savingTimer->start(kSavingShowMs);
    refresh();
}

// ---- sending -------------------------------------------------------------------------------------

void VoiceController::requestSend()
{
    switch (m_phase) {
    case Phase::Starting:
    case Phase::Recording:
        m_confirm       = false;
        m_sendAfterSave = true;
        stopRecording();
        break;
    case Phase::Saving:
        m_confirm       = false;
        m_sendAfterSave = true;
        if (m_encoded)
            doSend();
        else
            refresh();
        break;
    case Phase::Error:
        if (m_encoded) { // a kept recording: Send (again)
            m_sendAfterSave = true;
            doSend();
        }
        break;
    case Phase::Idle:
    case Phase::TooShort:
        break;
    }
}

void VoiceController::doSend()
{
    if (!m_encoded || m_path.isEmpty() || !QFileInfo(m_path).isFile()) {
        showSaveError();
        return;
    }
    if (!ts3::isConnected(m_target.sch)) {
        // Kept: "Not connected to this server … press Send" (the window), sent once you are back.
        m_notConnected  = true;
        m_sendAfterSave = false;
        showKept();
        return;
    }

    const bool held  = m_hold;
    const bool limit = m_limitHit;
    SendRequest request;
    request.target = m_target;
    SendItem item;
    item.path        = m_path;
    item.displayName = i18n::t("Voice message");
    item.ownTemp     = true; // Core deletes it with the job (a failed one is kept for a retry)
    item.voice       = true;
    item.durationMs  = m_lengthMs;
    item.waveform    = m_levels;
    request.items.append(item);
    m_path.clear(); // Core's now
    m_encoded = false;

    m_recorder->cancel(); // frees the recording in memory
    closeWindow();
    finish();
    ++m_diagSent; // 2.2 diagnostics
    ts3::log(LogLevel_INFO, m_target.sch, "Voice message: sent for upload (%1 ms)", {ts3::pub(m_lengthMs)});
    if (m_host.send)
        m_host.send(request);
    if (held && limit) // 5:00 sent it while the button was held: the strip says why it went
        showNotice(HoldStrip::Icon::Sent, i18n::t("Reached the %1 limit, so it was sent.").arg(formatDuration(kMaxMs)));
}

// ---- canceling ----------------------------------------------------------------------------------

void VoiceController::requestCancel()
{
    if (m_confirm)
        return;
    const bool   hasSound = m_phase == Phase::Recording || m_phase == Phase::Saving;
    const qint64 length   = m_phase == Phase::Recording ? m_recorder->elapsedMs() : m_lengthMs;
    if (hasSound && length >= kConfirmFromMs) {
        m_confirm = true; // the recording goes on meanwhile; Keep continues it
        refresh();
        return;
    }
    discard(false);
}

void VoiceController::requestClose()
{
    if (m_confirm)
        return;
    // A kept recording (stopped by itself, not connected, or not saved yet) is discarded by Close, Esc or
    // the close button: from 3 s on it asks first, like Cancel while recording.
    const bool kept = m_phase == Phase::Error && (m_encoded || m_recorder->state() == voice::VoiceRecorder::State::EncodeFailed);
    if (kept && m_lengthMs >= kConfirmFromMs) {
        m_confirm = true; // Keep shows the recording's window again
        refresh();
        return;
    }
    discard(false);
}

void VoiceController::discard(bool quiet)
{
    if (m_phase == Phase::Idle && !m_panel)
        return;
    const bool micOpen = m_phase == Phase::Starting || m_phase == Phase::Recording;
    m_openTimer->stop();
    m_savingTimer->stop();
    m_closeTimer->stop();
    m_recorder->cancel();
    releaseMic();
    if (micOpen && !quiet)
        playCue(false);
    removeRecording();
    closeWindow();
    finish();
}

void VoiceController::fix()
{
    switch (m_errorView.fix) {
    case VoicePanel::Fix::TryAgain:
        if (m_recorder->state() == voice::VoiceRecorder::State::EncodeFailed || m_recorder->state() == voice::VoiceRecorder::State::Encoded) {
            // The recording is still in memory (saving failed, or the saved file went missing before
            // it was sent): encode it again; it is sent once saved if Send was pressed.
            encode();
        } else {
            beginRecording();
        }
        break;
    case VoicePanel::Fix::Send:
        requestSend();
        break;
    case VoicePanel::Fix::WindowsPrivacy:
        QDesktopServices::openUrl(QUrl(QString::fromLatin1("ms-settings:privacy-microphone"))); // a local settings page
        break;
    case VoicePanel::Fix::SoundSettings:
        QDesktopServices::openUrl(QUrl(QString::fromLatin1("ms-settings:sound")));
        break;
    case VoicePanel::Fix::None:
        break;
    }
}

void VoiceController::closeWindow()
{
    if (!m_panel)
        return;
    VoicePanel* panel = m_panel.data();
    m_panel           = nullptr;
    disconnect(panel, nullptr, this, nullptr);
    panel->hide();
    panel->deleteLater(); // we may be inside one of its signals; plugin shutdown deletes it otherwise
}

void VoiceController::finish()
{
    m_confirm       = false;
    m_sendAfterSave = false;
    m_encoded       = false;
    m_savingShown   = false;
    m_notConnected  = false;
    m_stopped       = Stopped::None;
    // Hold to record: over (a notice may show the strip again).
    if (m_holdButton)
        m_holdButton->abandonHold(); // 5:00 sent it while still held: the release does nothing
    m_hold        = false;
    m_holdDown    = false;
    m_holdCancel  = false;
    m_holdButton  = nullptr;
    m_holdUpTicks = 0;
    m_holdTicks   = 0;
    m_holdTimer->stop();
    hideStrip();
    setPhase(Phase::Idle);
}

// ---- errors ------------------------------------------------------------------------------------------

void VoiceController::showError(voice::CaptureError error, long code)
{
    toWindow(); // a held recording: the window says it
    m_diagError = code ? QStringLiteral("%1 (0x%2)").arg(QString::fromLatin1(captureErrorName(error))).arg(static_cast<quint32>(code), 8, 16, QLatin1Char('0'))
                       : QString::fromLatin1(captureErrorName(error)); // 2.2 diagnostics
    m_encoded = false;
    VoicePanel::View v;
    v.mode = VoicePanel::Mode::Error;
    switch (error) {
    case voice::CaptureError::PrivacyBlocked:
        v.errorTitle = i18n::t("Microphone access is blocked");
        v.errorBody  = i18n::t("Windows doesn't let desktop apps use the microphone. Turn on “Let desktop apps access your microphone”, then try again.");
        v.fix        = VoicePanel::Fix::WindowsPrivacy;
        v.fixText    = i18n::t("Open &Windows settings");
        break;
    case voice::CaptureError::NoDevice:
        v.errorTitle = i18n::t("No microphone found");
        v.errorBody  = i18n::t("Connect a microphone, or choose one in Windows sound settings, then try again.");
        v.fix        = VoicePanel::Fix::SoundSettings;
        v.fixText    = i18n::t("Open &sound settings");
        break;
    case voice::CaptureError::Busy:
        v.errorTitle = i18n::t("The microphone is busy");
        v.errorBody  = i18n::t("Another app is using the microphone exclusively. Close that app, then try again.");
        v.fix        = VoicePanel::Fix::TryAgain;
        v.fixText    = i18n::t("&Try again");
        break;
    case voice::CaptureError::Disconnected:
        v.errorTitle = i18n::t("The microphone was disconnected");
        v.errorBody  = i18n::t("Reconnect it, then try again.");
        v.fix        = VoicePanel::Fix::TryAgain;
        v.fixText    = i18n::t("&Try again");
        break;
    case voice::CaptureError::UnsupportedFormat:
        v.errorTitle = i18n::t("This microphone can't be used");
        v.errorBody  = i18n::t("Its audio format (%1 Hz) isn't supported. Choose another microphone in TeamSpeak, or set this one to 48000 Hz in Windows sound settings.").arg(code);
        v.fix        = VoicePanel::Fix::SoundSettings;
        v.fixText    = i18n::t("Open &sound settings");
        break;
    case voice::CaptureError::None: // Media Foundation is missing
        v.errorTitle = i18n::t("Voice messages aren't available");
        v.errorBody  = i18n::t("Windows Media Foundation is missing. On Windows N editions, install the Media Feature Pack.");
        break;
    case voice::CaptureError::Generic:
    case voice::CaptureError::EndOfStream:
        v.errorTitle = i18n::t("Recording failed");
        v.errorBody  = i18n::t("Something went wrong with the microphone (error 0x%1). Try again.").arg(QString::number(static_cast<quint32>(code), 16).toUpper().rightJustified(8, QLatin1Char('0')));
        v.fix        = VoicePanel::Fix::TryAgain;
        v.fixText    = i18n::t("&Try again");
        break;
    }
    m_errorView = v;
    m_confirm   = false;
    setPhase(Phase::Error);
    refresh();
}

void VoiceController::showSaveError()
{
    toWindow();
    m_diagError = QStringLiteral("saving the recording failed"); // 2.2 diagnostics
    m_encoded   = false;
    VoicePanel::View v;
    v.mode       = VoicePanel::Mode::Error;
    v.errorTitle = i18n::t("Couldn't save the recording");
    v.errorBody  = i18n::t("Check free disk space, then try again. Your recording is kept until you close this window.");
    v.fix        = VoicePanel::Fix::TryAgain;
    v.fixText    = i18n::t("&Try again");
    m_errorView  = v;
    m_confirm    = false;
    setPhase(Phase::Error);
    refresh();
}

void VoiceController::showKept()
{
    toWindow(); // held: the window keeps it (never sent by itself)
    const QString length = formatDuration(m_lengthMs);
    VoicePanel::View v;
    v.mode = VoicePanel::Mode::Error;
    if (m_notConnected) {
        v.errorTitle = i18n::t("Not connected to this server");
        v.errorBody  = i18n::t("Your voice message (%1) is kept. Reconnect, then press Send. Close this window to discard it.").arg(length);
    } else if (m_stopped == Stopped::DeviceLost) {
        v.errorTitle = i18n::t("The microphone was disconnected");
        v.errorBody  = i18n::t("Recording stopped at %1. Send what was recorded, or close this window to discard it.").arg(length);
    } else if (m_stopped == Stopped::WindowHidden) {
        v.errorTitle = i18n::t("Recording stopped");
        v.errorBody  = i18n::t("This window was hidden, so recording stopped at %1. Send what was recorded, or close this window to discard it.").arg(length);
    } else if (m_stopped == Stopped::HoldLost) {
        v.errorTitle = i18n::t("Recording stopped");
        v.errorBody  = i18n::t("The microphone button was let go outside TeamSpeak (or another window took the mouse), so recording stopped at %1 and "
                               "nothing was sent. Send it, or close this window to discard it.")
                           .arg(length);
    } else if (m_stopped == Stopped::ChatChanged) {
        v.errorTitle = i18n::t("Recording stopped");
        v.errorBody  = i18n::t("Another chat came up while you held the microphone button, so recording stopped at %1 and nothing was sent. "
                               "Send it to %2, or close this window to discard it.")
                           .arg(length, m_targetText);
    } else if (m_stopped == Stopped::LimitWhileCanceling) {
        v.errorTitle = i18n::t("Reached the %1 limit").arg(formatDuration(kMaxMs));
        v.errorBody  = i18n::t("The pointer was away from the microphone to cancel when the recording reached %1, so nothing was sent. "
                               "Send it, or close this window to discard it.")
                           .arg(length);
    } else {
        v.errorTitle = i18n::t("Recording stopped");
        v.errorBody  = i18n::t("Recording stopped at %1. Send what was recorded, or close this window to discard it.").arg(length);
    }
    v.fix       = VoicePanel::Fix::Send;
    v.fixText   = i18n::t("&Send");
    m_errorView = v;
    m_confirm   = false;
    setPhase(Phase::Error);
    refresh();
}

// ---- TeamSpeak: microphone, sounds ---------------------------------------------------------------------

void VoiceController::releaseMic()
{
    if (!m_guard->engaged())
        return;
    const voice::MicGuard::Released released = m_guard->release();
    m_diagMicBack = i18n::t("given back on %1 connection(s), %2 left as they were").arg(released.restored).arg(released.kept); // 2.2 diagnostics
    ts3::log(LogLevel_INFO, m_target.sch, "Voice message: TeamSpeak microphone given back on %1 connection(s), %2 left as they were",
             {ts3::pub(released.restored), ts3::pub(released.kept)});
}

void VoiceController::ensureCues()
{
    QDir().mkpath(voiceDir());
    for (const bool start : {true, false}) {
        const QByteArray wav = voice::cueWav(start ? voice::Cue::Start : voice::Cue::Stop);
        QFile            file(cuePath(start));
        if (file.exists() && file.size() == wav.size())
            continue;
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(wav);
    }
}

void VoiceController::playCue(bool start)
{
    if (!ts3::funcs.playWaveFile || !m_target.sch)
        return;
    ensureCues();
    const QByteArray path = QDir::toNativeSeparators(cuePath(start)).toUtf8();
    ts3::funcs.playWaveFile(m_target.sch, path.constData()); // TeamSpeak's own playback device
}

// ---- files -------------------------------------------------------------------------------------------

QString VoiceController::newRecordingPath() const
{
    QDir().mkpath(voiceDir());
    const quint32 random = QRandomGenerator::global()->generate();
    return voiceDir() + QStringLiteral("/voice_%1.m4a").arg(random, 8, 16, QLatin1Char('0'));
}

void VoiceController::removeRecording()
{
    if (!m_path.isEmpty())
        QFile::remove(m_path);
    m_path.clear();
}

void VoiceController::cleanupOldRecordings()
{
    // Recordings handed to Core live as long as their upload job (a failed one until it is dismissed or
    // retried), and no job outlives the plugin: whatever is left from an earlier session (TeamSpeak closed
    // or crashed mid-upload) goes at start, like the paste and edit folders (Core::start). The start and
    // stop sounds (cue_*.wav) stay.
    const QDir dir(voiceDir());
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("voice_*.m4a")}, QDir::Files))
        QFile::remove(fi.absoluteFilePath());
}

// ---- the window's content ------------------------------------------------------------------------------

void VoiceController::onPlaybackStarted()
{
    if (m_phase == Phase::Starting || m_phase == Phase::Recording) {
        // Nothing plays while recording (it would end up in the message).
        if (m_host.pauseAllPlayback)
            m_host.pauseAllPlayback();
        m_playbackPaused = true;
        refresh();
    }
}

void VoiceController::refresh()
{
    refreshHold(); // held: the strip (there is no window)
    if (!m_panel)
        return;
    VoicePanel::View v = m_phase == Phase::Error ? m_errorView : VoicePanel::View();
    if (nowMs() - m_targetTextMs >= 1000) { // a channel switch changes where the message goes
        m_targetTextMs = nowMs();
        if (ts3::isConnected(m_target.sch) && m_host.describe)
            m_targetText = m_host.describe(m_target);
    }
    v.target         = m_targetText;
    v.limitMs        = kMaxMs;
    v.animate        = ui::animationsEnabled();
    v.confirmDiscard = m_confirm;
    v.device         = m_recorder->deviceName();
    const bool connected = ts3::isConnected(m_target.sch);

    QStringList problems;
    QStringList notes;
    const QString muted = i18n::t("Your TeamSpeak microphone is muted while you record.");
    switch (m_phase) {
    case Phase::Idle:
        return;
    case Phase::Starting:
        v.mode = VoicePanel::Mode::Starting;
        if (m_muted)
            notes << muted;
        break;
    case Phase::Recording:
    case Phase::Saving: {
        v.mode         = m_phase == Phase::Recording ? VoicePanel::Mode::Recording : VoicePanel::Mode::Saving;
        v.sendingShown = m_savingShown;
        v.sending      = m_sendAfterSave;
        const qint64 elapsed = m_phase == Phase::Recording ? m_recorder->elapsedMs() : m_lengthMs;
        v.timeMs       = elapsed;
        const int count = m_recorder->binCount();
        v.liveBins     = m_recorder->bins(qMax(0, count - 100));
        v.timeWarning  = elapsed >= kWarnAtMs;
        if (!connected)
            problems << i18n::t("Not connected to this server. Reconnect to send it.");
        if (m_phase == Phase::Recording && elapsed >= 3000 && m_recorder->loudestDb() < waveform::kQuietDb)
            problems << i18n::t("No sound from the microphone. Check that it isn't muted.");
        if (m_phase == Phase::Recording && v.timeWarning && elapsed < kMaxMs)
            problems << i18n::t("30 seconds left. It's sent by itself at %1.").arg(formatDuration(kMaxMs));
        if (m_phase == Phase::Saving && m_limitHit)
            notes << i18n::t("Reached the %1 limit, so it's sent now.").arg(formatDuration(kMaxMs));
        if (m_playbackPaused)
            notes << i18n::t("Playback was paused while you record.");
        if (m_device && m_device->source == voice::DeviceChoice::Source::DefaultCommunications) {
            notes << (m_device->unmatched ? i18n::t("TeamSpeak's microphone wasn't found in Windows. Recording from Windows' default communications microphone.")
                                          : i18n::t("Recording from Windows' default communications microphone."));
        }
        if (m_muted && m_phase == Phase::Recording)
            notes << muted;
        break;
    }
    case Phase::TooShort:
        v.mode = VoicePanel::Mode::TooShort;
        notes << i18n::t("Too short to send — nothing was sent.");
        break;
    case Phase::Error:
        v.mode = VoicePanel::Mode::Error;
        break;
    }
    v.hintError = !problems.isEmpty();
    v.hints     = (problems + notes).mid(0, kMaxHintLines);
    m_panel->setView(v);
}
