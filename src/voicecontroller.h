#pragma once

// 2.2 voice: recording and sending a voice message (the whole flow; VoicePanel is its view, MicButton the
// chat input's button, HoldStrip the strip above the input). 2.2.1: no settings, no hotkey, no review:
// what you record is sent.
//
// Hold to record (the mic button, 2.2.1, like Telegram): no window.
//   Press     holdPressed(): the same checks as start(), the TeamSpeak microphone muted, the start sound,
//             and the microphone opened at once (no wait for the sound: the first word isn't clipped).
//             The strip above the input shows the time, a live waveform and "Release to send · Move away
//             to cancel"; the button shows the hold.
//   Release   holdReleased(Send): stopped, encoded and sent like the window's Send. Under half a second
//             held it is a tap: nothing is sent and the strip says "Hold to record, release to send";
//             the next press within the double-click time starts nothing.
//   Cancel    released in the cancel zone (more than MicButton::kCancelPx from the button: the strip is
//             red, "Release to cancel"), or Esc while held: discarded, the strip says so. Back within
//             kRearmPx it sends again.
//   5:00      sent by itself while still held (the strip says so); the release then does nothing. Held in
//             the cancel zone it is kept in the window instead (never sent while it says "Release to
//             cancel").
//   Lost      the button no longer held and no release came (TeamSpeak's window deactivated, a popup took
//             the mouse, another server tab hid the input, the button disabled, Host::pointerHeld() up for
//             kHoldUpTicks ticks), or another chat came up in the input's chat tabs (Host::visibleTarget(),
//             checked every kHoldChatTicks ticks and at the release): stopped, never sent by itself,
//             kept in the window (Recording stopped … Send). A lost microphone, a saving error or no
//             connection when sending hand over to the window the same way.
//   Unload    (or the input gone while held): canceled quietly, the microphone given back.
// The Plugins menu and /tsmedia voice use the window (below); a click on the button while the window
// records stops and sends, as before.
//
// The window:
//   start()   checks the visible chat (connected, not a password channel, a known private chat partner),
//             mutes the TeamSpeak microphone (MicGuard), pauses whatever plays, opens the window, plays
//             the start sound and opens the microphone 180 ms later (so the sound isn't recorded). The
//             microphone is TeamSpeak's own capture device, or Windows' default communications
//             microphone when it can't be mapped (the window says so).
//   Send      (the window's Send, Enter, the mic button again) stops the capture (the microphone is
//             given back, the stop sound plays), encodes .m4a and hands it to Core::send() with one
//             voice item (vm, d, wf); the upload panel takes over and the window closes.
//   Cancel    (Cancel, Esc, the close button) discards; from 3 s on it asks first, the recording going
//             on meanwhile. Closing a kept recording (below) of 3 s or more asks too.
//   Limits    5:00 at most: then it is sent by itself (the window says so); 0.5 s at least ("Too short",
//             nothing is sent).
//   Problems  the microphone lost (or the window hidden) after 1 s: the recording stops and the window
//             offers to send what was recorded; not connected when sending: the same, once you are back.
//
// The window (or, for a hold, the strip and the held button) is on screen whenever the microphone is
// open: when it is closed (or deleted by plugin shutdown), the recording is canceled. Every way out gives
// the TeamSpeak microphone back (MicGuard; a connection that dropped meanwhile once it is connected
// again, checked every second).
// Owned by ChatIntegration (its Host), so it is destroyed before the inline players, Core and
// mf::shutdown(). Without ChatIntegration (the tests) the Host is a fake, and so is the microphone.

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

#include "audio/capture.h"
#include "audio/voicerecorder.h"
#include "core.h"
#include "holdstrip.h"
#include "micbutton.h"
#include "voicepanel.h"

class QTimer;
namespace voice {
class MicGuard;
class MicEnvironment;
struct DeviceChoice;
} // namespace voice

class VoiceController : public QObject
{
    Q_OBJECT

  public:
    enum class Origin { Menu, Command, Button };
    enum class Phase { Idle, Starting, Recording, Saving, TooShort, Error };

    static constexpr qint64 kMaxMs         = 5 * 60 * 1000; // product decision: 5 minutes, then it is sent
    static constexpr qint64 kWarnAtMs      = kMaxMs - 30 * 1000;
    static constexpr qint64 kMinMs         = 500;  // shorter: "Too short", nothing is sent
    static constexpr qint64 kConfirmFromMs = 3000; // discarding this much asks first
    static constexpr qint64 kKeepOnLossMs  = 1000; // a lost microphone keeps what was recorded from 1 s on
    static constexpr int    kHoldWatchMs   = 40;   // hold to record: the button, the pointer and the strip
    static constexpr int    kHoldUpTicks   = 3;    // ... Host::pointerHeld() up this many ticks in a row: lost
    static constexpr int    kHoldChatTicks = 5;    // ... every this many ticks: the visible chat checked
    static constexpr int    kNoticeMs      = 2000; // the strip's notice after a hold (longer if Windows keeps
                                                   // notifications longer than 5 s)

    // What the controller needs from the chat: ChatIntegration in the plugin, fakes in the tests.
    struct Host {
        // The visible chat as the destination; false if it can't be one (the host says why, at the chat).
        std::function<bool(ChatTarget* target, QString* description, QWidget** anchor)> target;
        // Why the visible chat can't take a voice message now, or empty. No side effects (the mic button).
        std::function<QString()> blockReason;
        std::function<QString(const ChatTarget&)> describe; // "the channel “Lobby”"
        std::function<void()> pauseAllPlayback;
        std::function<void(const SendRequest&)> send; // Core::send
        // The microphone for a message to sch: a factory the recorder calls on its worker, which fills
        // device in. The plugin passes teamSpeakMicrophone; the tests pass generated sound. Missing: the
        // recording fails (never a default microphone).
        std::function<voice::VoiceRecorder::BackendFactory(quint64 sch, std::shared_ptr<voice::DeviceChoice> device)> microphone;
        // Hold to record: whether the primary mouse button is down right now, whichever window has the
        // mouse (the plugin passes primaryButtonHeld; the tests a flag). Missing: only events end a hold.
        std::function<bool()> pointerHeld;
        // Hold to record: the chat the input shows now (its chat tabs share one input), without side effects.
        // Missing: only the press's chat counts.
        std::function<ChatTarget()> visibleTarget;
        bool offscreen = false; // tests: the window is never put on the screen (WA_DontShowOnScreen)
    };

    explicit VoiceController(Host host, QObject* parent = nullptr);
    ~VoiceController() override; // cancels a recording and gives the microphone back, synchronously

    // Menu / command: starts recording in the visible chat, or brings an open window forward.
    void start(Origin origin);
    // The mic button, held (see above). holdPressed(): true when the press started recording (or was
    // taken: an error the window shows); false makes it a plain click (toggle() on its release).
    bool holdPressed(MicButton* button);
    void holdZoneChanged(bool cancel);
    void holdReleased(MicButton::Release how);
    bool       holding() const { return m_hold; } // a held recording is going on (until sent or discarded)
    HoldStrip* strip() const { return m_strip.data(); }
    // A click on the mic button that wasn't a hold: while the window records, stop and send (not the
    // second click of a double click); while it shows something else (sending, an error, a kept
    // recording) bring it forward. Nothing otherwise (a press records).
    void toggle();
    // The recorder window's Send (also Enter, and the Send of a kept recording).
    void requestSend();
    void requestCancel(); // Cancel, Esc, the close button while recording or sending
    void requestClose();  // Close, Esc, the close button of an error, a kept recording or "Too short"

    // An inline player or the viewer started playing (the recorder pauses it again).
    void onPlaybackStarted();

    Phase       phase() const { return m_phase; }
    bool        isActive() const { return m_phase != Phase::Idle; }
    VoicePanel* panel() const { return m_panel.data(); }

    // The real microphone: TeamSpeak's capture device of sch (read here, on the GUI thread), mapped to a
    // Windows endpoint on the worker, else Windows' default communications microphone.
    static voice::VoiceRecorder::BackendFactory teamSpeakMicrophone(quint64 sch, std::shared_ptr<voice::DeviceChoice> device);
    // The primary mouse button's physical state (Windows; swapped buttons too): Host::pointerHeld.
    static bool primaryButtonHeld();
#ifdef TSMEDIA_TESTHOOKS
    // The live-test driver holds the button with synthetic mouse events (the real one stays up): it says
    // whether its hold is still "down" instead (nullptr: only events end a hold).
    void setPointerHeldForTests(std::function<bool()> probe) { m_host.pointerHeld = std::move(probe); }
#endif

    // 2.2 integration: plugin shutdown, first of all (plugin.cpp): a recording is canceled quietly, the
    // capture or encode worker joined, TeamSpeak's microphone given back while the connections still
    // exist, and the window deleted. The destructor does the same again.
    void shutdown();

    // 2.2 diagnostics: the "Voice messages" section: which microphone the last recording used (its kind,
    // never its name), the last microphone error and the MicGuard state. GUI thread.
    static QString diagnosticsTitle();
    QStringList    diagnosticLines() const;

    // The mic buttons' state for the phase and the visible chat (MicButton::setSharedState). Also on a
    // timer while idle, and when the pointer comes onto a button.
    void updateMicButtons();

  private:
    // Why a recording stopped by itself (kept in the window): HoldLost and ChatChanged end a hold, and
    // LimitWhileCanceling is a hold in the cancel zone at 5:00.
    enum class Stopped { None, DeviceLost, WindowHidden, HoldLost, ChatChanged, LimitWhileCanceling, Ended };

    void beginRecording();
    // Hold to record.
    void watchHold();
    void holdLost(Stopped why);
    bool visibleChatChanged() const; // the input shows another chat than the hold's (while connected)
    void endHold(HoldStrip::Icon icon, const QString& notice, bool quiet); // discards, then the notice
    void toWindow(); // a held recording goes on in the window (an error, kept)
    void refreshHold();
    void showNotice(HoldStrip::Icon icon, const QString& text);
    void hideStrip();
    void deleteStrip();
    void openMicrophone();
    void onRecorderState();
    void onRecorderTick();
    void stopRecording();
    void encode();
    void doSend();
    void discard(bool quiet);
    void fix();
    void closeWindow();
    void finish();
    void showError(voice::CaptureError error, long code);
    void showSaveError();
    void showKept(); // stopped by itself (or not connected): Send what was recorded, or close
    void releaseMic();
    void playCue(bool start);
    void ensureCues();
    void refresh();
    void createPanel();
    void bringForward(bool activate);
    void setPhase(Phase phase);

    QString newRecordingPath() const;
    void    removeRecording();
    static void cleanupOldRecordings();

    Host   m_host;
    Phase  m_phase  = Phase::Idle;
    Origin m_origin = Origin::Menu;

    voice::VoiceRecorder*                  m_recorder = nullptr;
    std::unique_ptr<voice::MicEnvironment> m_micEnv;
    std::unique_ptr<voice::MicGuard>       m_guard;
    std::shared_ptr<voice::DeviceChoice>   m_device; // written by the worker before it opens the microphone
    QPointer<VoicePanel>                   m_panel;
    QPointer<QWidget>                      m_anchor;

    QTimer* m_openTimer   = nullptr; // the start sound, then the microphone
    QTimer* m_savingTimer = nullptr; // "Sending…" only after 300 ms
    QTimer* m_closeTimer  = nullptr; // "Too short" closes by itself
    QTimer* m_micTimer    = nullptr; // the mic buttons follow the visible chat while idle
    QTimer* m_holdTimer   = nullptr; // hold to record: watchHold() while a hold goes on
    QTimer* m_noticeTimer = nullptr; // the strip's notice goes

    // Hold to record (the strip instead of the window)
    bool                 m_hold        = false; // this recording is a hold (until sent, discarded or in the window)
    bool                 m_holdDown    = false; // ... and the button is still held: the release decides
    bool                 m_holdCancel  = false; // ... with the pointer in the cancel zone
    qint64               m_holdPressMs = 0;     // when it was pressed (shorter than kMinMs: a tap)
    qint64               m_lastTapMs   = -1;    // the last tap (the next press right after starts nothing)
    int                  m_holdUpTicks = 0;     // Host::pointerHeld() up this many ticks in a row
    int                  m_holdTicks   = 0;     // watchHold() ticks of this hold (the chat check's rhythm)
    QPointer<MicButton>  m_holdButton;
    QPointer<QWidget>    m_stripInput; // the input the strip sits on (also for the notice after the hold)
    QPointer<HoldStrip>  m_strip;

    ChatTarget m_target;
    QString    m_targetText;
    bool       m_sendAfterSave  = false; // Send was pressed (or 5:00 reached): it goes once encoded
    bool       m_encoded        = false; // m_path holds the encoded recording
    bool       m_confirm        = false;
    bool       m_savingShown    = false;
    bool       m_playbackPaused = false; // something started playing while recording and was paused
    bool       m_limitHit       = false;
    bool       m_muted          = false; // MicGuard muted at least one connection
    bool       m_notConnected   = false; // the kept recording waits for the connection
    Stopped    m_stopped        = Stopped::None;
    qint64     m_lengthMs       = 0;
    qint64     m_bytes          = 0;
    qint64     m_startedMs      = 0; // when this recording was started (the mic button's double click)
    QByteArray m_levels;
    QString    m_path;           // the encoded file (ours until it is handed to Core)
    qint64     m_targetTextMs   = 0; // when m_targetText was last refreshed
    VoicePanel::View m_errorView; // the error state's texts

    // 2.2 diagnostics (GUI thread copies; the worker's DeviceChoice is only read once it recorded)
    QString m_diagSource;     // "TeamSpeak's microphone", ...
    int     m_diagRate  = 0;  // the capture's sample rate
    QString m_diagError;      // the last microphone or saving error
    QString m_diagMicBack;    // what the last MicGuard release did
    int     m_diagStarted = 0; // recordings started this session
    int     m_diagSent    = 0; // ... and sent
};
