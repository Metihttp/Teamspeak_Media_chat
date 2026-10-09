#pragma once

// 2.2 voice: recording, reviewing and sending a voice message (the whole flow; VoicePanel is its view).
//
//   start()  checks the visible chat (connected, not a password channel, a known private chat partner),
//            mutes the TeamSpeak microphone (MicGuard, if the setting is on), pauses whatever plays,
//            opens the window, plays the start sound and opens the microphone 180 ms later (so the sound
//            isn't recorded).
//   Stop     ends the capture (the microphone is given back, the stop sound plays) and encodes .m4a; then
//            the review: listen, Re-record, Discard, Send. Send while recording stops and sends at once.
//   Send     Core::send() with one voice item (vm, d, wf); the upload toast takes over.
//   Limits   5:00 at most (stops by itself and goes to the review), 0.5 s at least ("Too short").
//
// The window is on screen whenever the microphone is open: when it is closed (or deleted by plugin
// shutdown), the recording is canceled. Every way out gives the TeamSpeak microphone back (MicGuard).
// Owned by ChatIntegration, so it is destroyed before the inline players, Core and mf::shutdown().

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

#include "audio/capture.h"
#include "core.h"
#include "voicepanel.h"

class ChatIntegration;
class QTimer;
namespace mf {
class VideoPlayer;
}
namespace voice {
class MicGuard;
class MicEnvironment;
class VoiceRecorder;
struct DeviceChoice;
} // namespace voice

class VoiceController : public QObject
{
    Q_OBJECT

  public:
    enum class Origin { Menu, Command, Hotkey };

    static constexpr qint64 kMaxMs          = 5 * 60 * 1000; // product decision: 5 minutes
    static constexpr qint64 kWarnAtMs       = kMaxMs - 30 * 1000;
    static constexpr qint64 kMinMs          = 500;  // shorter: "Too short", nothing is sent
    static constexpr qint64 kConfirmFromMs  = 3000; // discarding this much asks first
    static constexpr int    kHotkeyDebounce = 300;

    VoiceController(ChatIntegration* chat, Core* core, QObject* parent = nullptr);
    ~VoiceController() override; // cancels a recording and gives the microphone back, synchronously

    // Menu / command: starts recording in the visible chat, or brings an open window forward.
    void start(Origin origin);
    // The record hotkey: start; stop for the review (or send, without "listen before sending"); send.
    void hotkey();

    bool isActive() const { return m_phase != Phase::Idle; }

    // 2.2 integration: plugin shutdown, first of all (plugin.cpp): a recording is canceled quietly, the
    // capture or encode worker joined, TeamSpeak's microphone given back while the connections still
    // exist, and the window deleted. The destructor does the same again.
    void shutdown();

    // 2.2 diagnostics: the "Voice messages" section: the settings, which microphone the last recording
    // used (its kind, never its name), the last microphone error and the MicGuard state. GUI thread.
    static QString diagnosticsTitle();
    QStringList    diagnosticLines() const;

  signals:
    void settingsRequested(); // "Open TS Media settings" in an error

  private:
    enum class Phase { Idle, Starting, Recording, Saving, Review, TooShort, Error };
    enum class Problem { None, CaptureFailed, NoMediaFoundation, SaveFailed, TooShortDeviceLost };

    void beginRecording();
    void openMicrophone();
    void onRecorderState();
    void onRecorderTick();
    void stopRecording(bool send);
    void requestSend();
    void doSend();
    void requestCancel();
    void discard(bool quiet);
    void rerecord();
    void fix();
    void closeWindow();
    void finish();

    void enterReview();
    void togglePreview();
    void seekPreview(double fraction);
    void seekPreviewBy(qint64 deltaMs);
    void closePreview();
    void onPlaybackStarted();
    void onPreviewTick();

    void showError(voice::CaptureError error, long code);
    void showSaveError();
    void releaseMic();
    void playCue(bool start);
    void ensureCues();
    void refresh();
    void createPanel(bool fromHotkey);

    QString newRecordingPath() const;
    void    removeRecording();
    static void cleanupOldRecordings();

    ChatIntegration* m_chat;
    Core*            m_core;
    Phase            m_phase  = Phase::Idle;
    Origin           m_origin = Origin::Menu;

    voice::VoiceRecorder*                  m_recorder = nullptr;
    std::unique_ptr<voice::MicEnvironment> m_micEnv;
    std::unique_ptr<voice::MicGuard>       m_guard;
    std::shared_ptr<voice::DeviceChoice>   m_device; // written by the worker before it opens the microphone
    QPointer<VoicePanel>                   m_panel;
    QPointer<QWidget>                      m_anchor;
    mf::VideoPlayer*                       m_preview = nullptr;

    QTimer* m_openTimer   = nullptr; // the start sound, then the microphone
    QTimer* m_savingTimer = nullptr; // "Saving…" only after 300 ms
    QTimer* m_closeTimer  = nullptr; // "Too short" closes by itself
    QTimer* m_reviewTimer = nullptr; // playback position, connection state

    ChatTarget m_target;
    QString    m_targetText;
    bool       m_sendAfterSave  = false;
    bool       m_confirm        = false;
    bool       m_savingShown    = false;
    bool       m_playbackPaused = false; // something started playing while recording and was paused
    bool       m_limitHit       = false;
    bool       m_deviceLost     = false;
    bool       m_windowHidden   = false; // recording stopped because the window was hidden
    bool       m_muted          = false; // MicGuard muted at least one connection
    bool       m_previewWanted  = false; // play once the preview is loaded
    double     m_pendingSeek    = -1.0;
    qint64     m_lengthMs       = 0;
    qint64     m_bytes          = 0;
    QByteArray m_levels;
    QString    m_path;           // the encoded file (ours until it is handed to Core)
    qint64     m_lastHotkeyMs   = 0;
    qint64     m_targetTextMs   = 0; // when m_targetText was last refreshed
    VoicePanel::View m_errorView; // the error state's texts

    // 2.2 diagnostics (GUI thread copies; the worker's DeviceChoice is only read once it recorded)
    QString m_diagSource;              // "TeamSpeak's microphone", ...
    int     m_diagRate           = 0;  // the capture's sample rate
    bool    m_diagSettingMissing = false;
    QString m_diagError;               // the last microphone or saving error
    QString m_diagMicBack;             // what the last MicGuard release did
    int     m_diagStarted        = 0;  // recordings started this session
    int     m_diagSent           = 0;  // ... and sent
};
