#include "voicecontroller.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#include "audio/capturedevice.h"
#include "audio/cues.h"
#include "audio/micguard.h"
#include "audio/voicerecorder.h"
#include "audio/waveform.h"
#include "audio/wasapicapture.h"
#include "chatintegration.h"
#include "i18n.h"
#include "inlinemedia.h"
#include "settings.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"

#ifdef TSMEDIA_TESTHOOKS
#include "audio/fakecapture.h"
#endif

namespace {

constexpr int    kOpenDelayMs    = 180; // the start sound plays before the microphone opens
constexpr int    kSavingShowMs   = 300;
constexpr int    kReviewTickMs   = 33;
constexpr qint64 kKeepOnLossMs   = 1000; // a lost microphone keeps what was recorded from 1 s on
constexpr int    kMaxHintLines   = 2;
constexpr qint64 kLeftoverAgeSec = 24 * 3600;

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

const char* sourceName(voice::DeviceChoice::Source source)
{
    switch (source) {
    case voice::DeviceChoice::Source::Setting:
        return "the microphone chosen in the settings";
    case voice::DeviceChoice::Source::TeamSpeak:
        return "TeamSpeak's microphone";
    case voice::DeviceChoice::Source::DefaultCommunications:
        break;
    }
    return "Windows' default communications microphone";
}

} // namespace

VoiceController::VoiceController(ChatIntegration* chat, Core* core, QObject* parent)
    : QObject(parent)
    , m_chat(chat)
    , m_core(core)
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
    m_reviewTimer = new QTimer(this);
    m_reviewTimer->setInterval(kReviewTickMs);
    connect(m_reviewTimer, &QTimer::timeout, this, &VoiceController::onPreviewTick);

    connect(m_chat, &ChatIntegration::playbackStarted, this, &VoiceController::onPlaybackStarted);
    cleanupOldRecordings();
}

VoiceController::~VoiceController()
{
    // Plugin shutdown (or the chat going away): no sounds, no chat lines; the microphone comes back.
    m_openTimer->stop();
    m_recorder->cancel();
    m_guard->release();
    closePreview();
    removeRecording();
    if (m_panel) {
        disconnect(m_panel, nullptr, this, nullptr);
        delete m_panel.data();
    }
}

// ---- starting ------------------------------------------------------------------------------------

void VoiceController::start(Origin origin)
{
    if (m_phase != Phase::Idle) {
        if (m_panel) {
            m_panel->show();
            m_panel->raise();
            if (origin != Origin::Hotkey)
                m_panel->activateWindow();
        }
        return;
    }
    ChatTarget target;
    QString    description;
    QWidget*   anchor = nullptr;
    if (!m_chat->voiceTarget(&target, &description, &anchor))
        return; // the chat already says why
    m_origin     = origin;
    m_target     = target;
    m_targetText = description;
    m_anchor     = anchor;
    createPanel(origin == Origin::Hotkey);
    if (!mf::available()) {
        showError(voice::CaptureError::None, 0); // "Voice messages aren't available"
        return;
    }
    beginRecording();
}

void VoiceController::createPanel(bool fromHotkey)
{
    if (m_panel)
        return;
    auto* panel = new VoicePanel(nullptr); // its own top-level window: visible even when TeamSpeak is minimized
    m_panel     = panel;
    connect(panel, &VoicePanel::sendRequested, this, &VoiceController::requestSend);
    connect(panel, &VoicePanel::stopRequested, this, [this] { stopRecording(false); });
    connect(panel, &VoicePanel::cancelRequested, this, &VoiceController::requestCancel);
    connect(panel, &VoicePanel::rerecordRequested, this, &VoiceController::rerecord);
    connect(panel, &VoicePanel::keepRequested, this, [this] {
        m_confirm = false;
        refresh();
    });
    connect(panel, &VoicePanel::discardConfirmed, this, [this] { discard(false); });
    connect(panel, &VoicePanel::playToggled, this, &VoiceController::togglePreview);
    connect(panel, &VoicePanel::seekRequested, this, &VoiceController::seekPreview);
    connect(panel, &VoicePanel::seekByRequested, this, &VoiceController::seekPreviewBy);
    connect(panel, &VoicePanel::fixRequested, this, &VoiceController::fix);
    connect(panel, &VoicePanel::closeRequested, this, [this] { discard(false); });
    // Deleted by someone else (plugin shutdown): the microphone must not stay open without it.
    connect(panel, &QObject::destroyed, this, [this] {
        if (m_phase != Phase::Idle)
            discard(true);
    });
    if (fromHotkey)
        panel->setAttribute(Qt::WA_ShowWithoutActivating); // over a game: no focus stolen
    m_phase = Phase::Starting;
    refresh();
    panel->placeNear(m_anchor);
    panel->show();
    panel->raise();
    if (!fromHotkey)
        panel->activateWindow();
}

void VoiceController::beginRecording()
{
    m_phase          = Phase::Starting;
    m_sendAfterSave  = false;
    m_confirm        = false;
    m_savingShown    = false;
    m_playbackPaused = false;
    m_limitHit       = false;
    m_deviceLost     = false;
    m_windowHidden   = false;
    m_lengthMs       = 0;
    m_bytes          = 0;
    m_levels.clear();
    m_device.reset();
    m_chat->pauseAllPlayback();
    m_muted = false;
    if (Settings::instance().voiceMuteTeamSpeakMic)
        m_muted = m_guard->engage() > 0;
    ts3::log(LogLevel_INFO, m_target.sch, "Voice message: recording starts (TeamSpeak microphone muted on %1 connection(s))", {ts3::pub(m_guard->mutedConnections().size())});
    refresh();
    playCue(true);
    m_openTimer->start(Settings::instance().voiceSounds ? kOpenDelayMs : 0);
}

void VoiceController::openMicrophone()
{
    if (m_phase != Phase::Starting)
        return;
    if (!m_panel || !m_panel->isVisible()) { // never record without the window
        discard(true);
        return;
    }
    const QString                 setting   = Settings::instance().voiceMicrophone;
    const voice::TeamSpeakCapture teamSpeak = teamSpeakCapture(m_target.sch);
    auto                          device    = std::make_shared<voice::DeviceChoice>();
    m_device                                = device;
    voice::VoiceRecorder::BackendFactory factory = [setting, teamSpeak, device]() -> std::unique_ptr<voice::CaptureBackend> {
        // On the worker (COM is set up there): which microphone, then open it.
        *device = voice::chooseCaptureDevice(setting, teamSpeak, voice::listCaptureEndpoints());
        return std::make_unique<voice::WasapiCapture>(device->endpointId);
    };
#ifdef TSMEDIA_TESTHOOKS
    // Test builds: <data dir>/voice_fake.txt ("<ms>[;send][;silence][;unplug@<ms>]", written by the
    // selftest_voice.txt hook for localhost servers only) records generated sound, never the microphone.
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
            else if (p.startsWith(QLatin1String("unplug@")))
                options.unplugAfterMs = p.mid(7).toLongLong();
        }
        factory = [options, device]() -> std::unique_ptr<voice::CaptureBackend> {
            device->source = voice::DeviceChoice::Source::Setting;
            return std::make_unique<voice::FakeCapture>(options);
        };
        ts3::log("[test] voice message from generated sound");
    }
#endif
    m_recorder->start(factory, kMaxMs);
}

// ---- the recorder ------------------------------------------------------------------------------------

void VoiceController::onRecorderState()
{
    using State = voice::VoiceRecorder::State;
    switch (m_recorder->state()) {
    case State::Recording:
        if (m_phase == Phase::Starting) {
            m_phase = Phase::Recording;
            const voice::CaptureFormat format = m_recorder->format();
            ts3::log(LogLevel_INFO, m_target.sch, "Voice message: recording from %1 at %2 Hz", {ts3::pub(QString::fromLatin1(m_device ? sourceName(m_device->source) : "?")), ts3::pub(format.sampleRate)});
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
        m_lengthMs   = m_recorder->elapsedMs();
        m_limitHit   = m_recorder->limitReached();
        m_deviceLost = m_recorder->deviceLost();
        if (m_lengthMs < kMinMs || (m_deviceLost && m_lengthMs < kKeepOnLossMs)) {
            const bool lost = m_deviceLost;
            m_recorder->cancel();
            if (lost) {
                showError(voice::CaptureError::Disconnected, 0);
            } else {
                m_phase   = Phase::TooShort;
                m_confirm = false;
                refresh();
                m_closeTimer->start(ui::notificationDurationMs());
            }
            return;
        }
        m_levels = m_recorder->waveformLevels();
        m_path   = newRecordingPath();
        m_recorder->encode(m_path);
        m_phase       = Phase::Saving;
        m_savingShown = false;
        m_savingTimer->start(kSavingShowMs);
        refresh();
        break;
    }
    case State::Encoded:
        m_savingTimer->stop();
        m_bytes = m_recorder->encodedBytes();
        ts3::log(LogLevel_INFO, m_target.sch, "Voice message: %1 ms encoded, %2 bytes", {ts3::pub(m_lengthMs), ts3::pub(m_bytes)});
        if (m_sendAfterSave && !m_confirm)
            doSend();
        else
            enterReview();
        break;
    case State::EncodeFailed:
        m_savingTimer->stop();
        ts3::log(LogLevel_WARNING, m_target.sch, "Voice message: encoding failed: %1", {ts3::pub(m_recorder->encodeError())});
        m_path.clear(); // the writer removed its partial file
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
    if (!m_panel) {
        discard(true);
        return;
    }
    if (!m_panel->isVisible() || m_panel->isMinimized()) {
        // The window must be on screen while the microphone is open: stop and keep what was said.
        m_windowHidden = true;
        stopRecording(false);
        m_panel->showNormal();
        m_panel->raise();
        return;
    }
    refresh();
}

void VoiceController::stopRecording(bool send)
{
    if (m_phase == Phase::Saving) {
        m_sendAfterSave = m_sendAfterSave || send;
        return;
    }
    if (m_phase != Phase::Starting && m_phase != Phase::Recording)
        return;
    m_sendAfterSave = send;
    if (m_openTimer->isActive() || m_recorder->state() == voice::VoiceRecorder::State::Idle) {
        // Stopped before the microphone was even open: nothing to send.
        m_openTimer->stop();
        m_recorder->cancel();
        releaseMic();
        m_phase = Phase::TooShort;
        refresh();
        m_closeTimer->start(ui::notificationDurationMs());
        return;
    }
    m_recorder->stop(); // -> Captured (onRecorderState)
}

// ---- sending -------------------------------------------------------------------------------------

void VoiceController::requestSend()
{
    switch (m_phase) {
    case Phase::Starting:
    case Phase::Recording:
    case Phase::Saving:
        stopRecording(true);
        break;
    case Phase::Review:
        doSend();
        break;
    case Phase::Idle:
    case Phase::TooShort:
    case Phase::Error:
        break;
    }
}

void VoiceController::doSend()
{
    if (m_path.isEmpty() || !QFileInfo(m_path).isFile()) {
        showSaveError();
        return;
    }
    if (!ts3::isConnected(m_target.sch)) {
        // Kept: "Not connected to this server. Reconnect, then press Send." (refresh)
        m_sendAfterSave = false;
        if (m_phase != Phase::Review)
            enterReview();
        else
            refresh();
        return;
    }
    closePreview(); // Core links or moves the file: nothing of ours may hold it open

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

    m_recorder->cancel(); // frees the recording in memory
    closeWindow();
    finish();
    m_core->send(request);
}

// ---- canceling ----------------------------------------------------------------------------------

void VoiceController::requestCancel()
{
    if (m_confirm)
        return;
    const bool hasSound = m_phase == Phase::Recording || m_phase == Phase::Saving || m_phase == Phase::Review;
    const qint64 length = m_phase == Phase::Recording ? m_recorder->elapsedMs() : m_lengthMs;
    if (hasSound && length >= kConfirmFromMs) {
        m_confirm = true; // the recording goes on meanwhile; Keep continues it
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
    closePreview();
    removeRecording();
    closeWindow();
    finish();
}

void VoiceController::rerecord()
{
    if (m_phase != Phase::Review && m_phase != Phase::Error)
        return;
    closePreview();
    removeRecording();
    m_recorder->cancel();
    if (!m_chat || !ts3::isConnected(m_target.sch)) {
        refresh();
        return;
    }
    beginRecording();
}

void VoiceController::fix()
{
    const VoicePanel::Fix fix = m_errorView.fix;
    switch (fix) {
    case VoicePanel::Fix::TryAgain:
        if (m_recorder->state() == voice::VoiceRecorder::State::EncodeFailed) {
            // The recording is still in memory: encode it again.
            m_path = newRecordingPath();
            m_recorder->encode(m_path);
            m_phase       = Phase::Saving;
            m_savingShown = false;
            m_savingTimer->start(kSavingShowMs);
            refresh();
        } else {
            beginRecording();
        }
        break;
    case VoicePanel::Fix::WindowsPrivacy:
        QDesktopServices::openUrl(QUrl(QString::fromLatin1("ms-settings:privacy-microphone"))); // a local settings page
        break;
    case VoicePanel::Fix::SoundSettings:
        QDesktopServices::openUrl(QUrl(QString::fromLatin1("ms-settings:sound")));
        break;
    case VoicePanel::Fix::PluginSettings:
        emit settingsRequested();
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
    m_phase          = Phase::Idle;
    m_confirm        = false;
    m_sendAfterSave  = false;
    m_savingShown    = false;
    m_previewWanted  = false;
    m_pendingSeek    = -1.0;
    m_reviewTimer->stop();
}

// ---- review ------------------------------------------------------------------------------------------

void VoiceController::enterReview()
{
    m_phase         = Phase::Review;
    m_sendAfterSave = false;
    closePreview();
    m_preview = new mf::VideoPlayer(this);
    m_preview->setVolume(Settings::instance().videoVolume / 100.0);
    connect(m_preview, &mf::VideoPlayer::loaded, this, [this] {
        if (!m_preview)
            return;
        if (m_pendingSeek >= 0.0)
            m_preview->seek(qRound64(m_pendingSeek * static_cast<double>(m_lengthMs)));
        m_pendingSeek = -1.0;
        if (m_previewWanted) {
            m_previewWanted = false;
            m_chat->pauseAllPlayback();
            m_preview->play();
        }
        refresh();
    });
    connect(m_preview, &mf::VideoPlayer::failed, this, [this](const QString& error) {
        ts3::log(LogLevel_WARNING, m_target.sch, "Voice message: the review player failed: %1", {ts3::pub(error)});
        m_previewWanted = false;
        refresh();
    });
    m_preview->open(m_path, mf::OpenMode::AudioOnly);
    m_reviewTimer->start();
    refresh();
}

void VoiceController::togglePreview()
{
    if (m_phase != Phase::Review || !m_preview)
        return;
    if (!m_preview->isLoaded()) {
        m_previewWanted = !m_previewWanted;
        refresh();
        return;
    }
    if (m_preview->isPlaying()) {
        m_preview->pause();
    } else {
        m_chat->pauseAllPlayback(); // one thing at a time
        if (m_preview->isEnded())
            m_preview->seek(0);
        m_preview->play();
    }
    refresh();
}

void VoiceController::seekPreview(double fraction)
{
    if (m_phase != Phase::Review || !m_preview)
        return;
    fraction = qBound(0.0, fraction, 1.0);
    if (!m_preview->isLoaded()) {
        m_pendingSeek = fraction;
        return;
    }
    const qint64 length = m_preview->duration() > 0 ? m_preview->duration() : m_lengthMs;
    m_preview->seek(qMin(length, qRound64(fraction * static_cast<double>(length))));
    refresh();
}

void VoiceController::seekPreviewBy(qint64 deltaMs)
{
    if (m_phase != Phase::Review || !m_preview || !m_preview->isLoaded())
        return;
    const qint64 length = m_preview->duration() > 0 ? m_preview->duration() : m_lengthMs;
    m_preview->seek(qBound<qint64>(0, m_preview->position() + deltaMs, length));
    refresh();
}

void VoiceController::closePreview()
{
    m_reviewTimer->stop();
    if (!m_preview)
        return;
    mf::VideoPlayer* player = m_preview;
    m_preview               = nullptr;
    disconnect(player, nullptr, this, nullptr);
    delete player; // shuts the engine down synchronously and lets go of the file
}

void VoiceController::onPlaybackStarted()
{
    if (m_phase == Phase::Starting || m_phase == Phase::Recording) {
        // Nothing plays while recording (it would end up in the message).
        m_chat->pauseAllPlayback();
        m_playbackPaused = true;
        refresh();
    } else if (m_phase == Phase::Review && m_preview && m_preview->isPlaying()) {
        m_preview->pause(); // something in the chat or the viewer started: one thing at a time
        refresh();
    }
}

void VoiceController::onPreviewTick()
{
    if (m_phase == Phase::Review)
        refresh();
}

// ---- errors ------------------------------------------------------------------------------------------

void VoiceController::showError(voice::CaptureError error, long code)
{
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
        v.errorBody  = i18n::t("Its audio format (%1 Hz) isn't supported. Choose another microphone in TS Media settings.").arg(code);
        v.fix        = VoicePanel::Fix::PluginSettings;
        v.fixText    = i18n::t("Open TS Media &settings");
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
    m_phase     = Phase::Error;
    m_confirm   = false;
    refresh();
}

void VoiceController::showSaveError()
{
    VoicePanel::View v;
    v.mode       = VoicePanel::Mode::Error;
    v.errorTitle = i18n::t("Couldn't save the recording");
    v.errorBody  = i18n::t("Check free disk space, then try again. Your recording is kept until you close this window.");
    v.fix        = VoicePanel::Fix::TryAgain;
    v.fixText    = i18n::t("&Try again");
    m_errorView  = v;
    m_phase      = Phase::Error;
    m_confirm    = false;
    refresh();
}

// ---- TeamSpeak: microphone, sounds ---------------------------------------------------------------------

void VoiceController::releaseMic()
{
    if (!m_guard->engaged())
        return;
    const voice::MicGuard::Released released = m_guard->release();
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
    if (!Settings::instance().voiceSounds || !ts3::funcs.playWaveFile || !m_target.sch)
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
    // Recordings handed to Core live as long as their upload job; a leftover from an earlier session
    // (TeamSpeak closed mid-upload) goes after a day.
    const QDir      dir(voiceDir());
    const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-kLeftoverAgeSec);
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("voice_*.m4a")}, QDir::Files)) {
        if (fi.lastModified().toUTC() < limit)
            QFile::remove(fi.absoluteFilePath());
    }
}

// ---- the window's content ------------------------------------------------------------------------------

void VoiceController::refresh()
{
    if (!m_panel)
        return;
    VoicePanel::View v = m_phase == Phase::Error ? m_errorView : VoicePanel::View();
    if (nowMs() - m_targetTextMs >= 1000) { // a channel switch changes where the message goes
        m_targetTextMs = nowMs();
        if (ts3::isConnected(m_target.sch))
            m_targetText = m_chat->voiceTargetText(m_target);
    }
    v.target           = m_targetText;
    v.limitMs          = kMaxMs;
    v.animate          = ui::animationsEnabled();
    v.confirmDiscard   = m_confirm;
    v.device           = m_recorder->deviceName();
    const bool connected = ts3::isConnected(m_target.sch);
    v.canSend            = connected;

    QStringList problems;
    QStringList notes;
    const QString notConnected = i18n::t("Not connected to this server. Reconnect, then press Send.");
    switch (m_phase) {
    case Phase::Idle:
        return;
    case Phase::Starting:
        v.mode = VoicePanel::Mode::Starting;
        if (m_muted)
            notes << i18n::t("Your TeamSpeak microphone is muted while you record.");
        break;
    case Phase::Recording:
    case Phase::Saving: {
        v.mode        = m_phase == Phase::Recording ? VoicePanel::Mode::Recording : VoicePanel::Mode::Saving;
        v.savingShown = m_savingShown;
        const qint64 elapsed = m_phase == Phase::Recording ? m_recorder->elapsedMs() : m_lengthMs;
        v.timeMs      = elapsed;
        const int count = m_recorder->binCount();
        v.liveBins    = m_recorder->bins(qMax(0, count - 100));
        v.timeWarning = elapsed >= kWarnAtMs;
        if (!connected)
            problems << notConnected;
        if (m_phase == Phase::Recording && elapsed >= 3000 && m_recorder->loudestDb() < waveform::kQuietDb)
            problems << i18n::t("No sound from the microphone. Check that it isn't muted.");
        if (v.timeWarning && elapsed < kMaxMs)
            problems << i18n::t("30 seconds left");
        if (m_playbackPaused)
            notes << i18n::t("Playback was paused while you record.");
        if (m_device && m_device->settingMissing)
            notes << i18n::t("The microphone chosen in TS Media settings isn't connected. Recording from Windows' default communications microphone.");
        else if (m_device && m_device->source == voice::DeviceChoice::Source::DefaultCommunications)
            notes << i18n::t("Recording from Windows' default communications microphone.");
        if (m_muted)
            notes << i18n::t("Your TeamSpeak microphone is muted while you record.");
        break;
    }
    case Phase::Review: {
        v.mode     = VoicePanel::Mode::Review;
        v.levels   = m_levels;
        v.lengthMs = m_lengthMs;
        v.bytes    = m_bytes;
        if (m_preview && m_preview->isLoaded()) {
            v.playing = m_preview->isPlaying();
            v.timeMs  = m_preview->isEnded() ? 0 : qMin(m_lengthMs, m_preview->position());
            v.started = v.playing || v.timeMs > 0;
        }
        if (!connected)
            problems << notConnected;
        if (m_limitHit)
            notes << i18n::t("Maximum length reached (%1).").arg(formatDuration(kMaxMs));
        else if (m_deviceLost)
            notes << i18n::t("Recording stopped: the microphone was disconnected.");
        else if (m_windowHidden)
            notes << i18n::t("Recording stopped because this window was hidden.");
        notes << i18n::t("%1 · %2").arg(formatDuration(m_lengthMs), formatSize(static_cast<quint64>(qMax<qint64>(0, m_bytes))));
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

// ---- the hotkey ---------------------------------------------------------------------------------------

void VoiceController::hotkey()
{
    const qint64 now = nowMs();
    if (m_lastHotkeyMs > 0 && now - m_lastHotkeyMs < kHotkeyDebounce)
        return; // key bounce / auto-repeat
    m_lastHotkeyMs = now;
    switch (m_phase) {
    case Phase::Idle:
        start(Origin::Hotkey);
        break;
    case Phase::Starting:
    case Phase::Recording:
        stopRecording(!Settings::instance().voiceReview);
        break;
    case Phase::Saving:
        if (!Settings::instance().voiceReview)
            m_sendAfterSave = true;
        break;
    case Phase::Review:
        if (!m_confirm)
            doSend();
        break;
    case Phase::TooShort:
    case Phase::Error:
        if (m_panel)
            m_panel->raise();
        break;
    }
}
