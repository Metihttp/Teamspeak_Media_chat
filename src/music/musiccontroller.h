#pragma once

// 2.3 live music: a music file from this computer played to everyone in the user's channel, through the
// user's own TeamSpeak voice (music::Engine mixes it into the captured voice, so the user can talk over it,
// and into this computer's playback, so the user hears it too). GUI thread.
//
//   Start      the paperclip menu's "Live music" (attachmenu.h) or /tsmedia music: a file dialog, then
//              start(): the visible server tab's connection and the user's channel. Refused (the bar says
//              why) when not connected, without talk power there, or without Media Foundation.
//   The bar    MusicBar above the chat input (inputbars.h): title, time, seek, play / pause, volume, stop.
//              Every server tab shares TeamSpeak's one chat input: on another tab the bar is compact (2.3),
//              "Title · on <server> › <channel>" with play / pause and stop, so the music can always be played
//              or stopped from there.
//   Other tab  TeamSpeak sends the voice of the server tab in front only (2.3 live test: the track went on
//              unheard while the user looked at another server tab). When the music's tab goes to the
//              background it pauses by itself (and says so in the compact bar), and plays on when that tab
//              is in front again, unless the user paused or stopped it meanwhile. Play in the compact bar
//              brings the music's tab to the front (Host::showConnection) and plays on there.
//   Problems   checked every second while it plays and shown in the bar: no talk power, TeamSpeak's
//              microphone or sound muted, no capture device open. While TeamSpeak's microphone or sound is
//              muted (read every tick) the music stays out of the voice: a muted client is never made to
//              send; with the sound muted it isn't played here either (2.3 review). Push-to-talk: TeamSpeak
//              captures nothing while the key is up, so the plugin keeps TeamSpeak's input open while the
//              music plays on the visible tab (and gives it back on pause, stop, another tab, unload);
//              meanwhile only the music goes out, never the microphone (the plugin can't see the key; a
//              block without music is silence, not sent). Never under voice activation (TeamSpeak's "vad";
//              2.3 review): that input is TeamSpeak's, and the voice goes along. If TeamSpeak keeps closing
//              the input, the plugin stops trying and says so.
//              Voice activation (2.3 live test): TeamSpeak keeps a transmission going on the plugin's "send"
//              but never starts one while its voice activation hears silence, so the channel heard nothing
//              until the user spoke (and nothing again after a pause). While the music should go out and
//              TeamSpeak isn't sending, its voice activation is turned off for a moment (the microphone left
//              out meanwhile) and on again as soon as it sends (or after kVadKickMs); always given back on
//              pause, stop, another tab, unload. If that keeps failing, the plugin stops trying and says so.
//   Stops      Stop (the bar goes), the track's end (the bar goes), a channel switch or a disconnect (a short
//              notice says why), a file that can't be decoded (the notice says why), plugin shutdown.
//
// The volume (0..100, and muted) is remembered in <data dir>/music.ini. Deleting the controller stops the
// music: the callbacks are detached (music::setActive) before the engine's worker is joined, TeamSpeak's
// input state is given back, and the bar leaves TeamSpeak's layout.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "music/musicbar.h"
#include "music/musicengine.h"

class QTextBrowser;
class QTimer;
class QWidget;

class MusicController : public QObject
{
    Q_OBJECT

  public:
    static constexpr int kTickMs        = 200;   // the bar and the push-to-talk input follow at this rate
    static constexpr int kCheckTicks    = 5;     // TeamSpeak's state (channel, mute, talk power) every second
    static constexpr int kCodecHintMs   = 10000; // "sounds best with Opus Music" at the start, this long
    static constexpr int kReopenLimit   = 8;     // TeamSpeak closed the input this often ...
    static constexpr int kReopenWindowMs = 10000; // ... within this long: the plugin stops keeping it open
    static constexpr int kDefaultVolume = 50;
    static constexpr int kVadKickMs     = 600;  // voice activation off this long at most to start a transmission
    static constexpr int kVadKickGapMs  = 1500; // ... and not again sooner than this
    static constexpr int kVadKickLimit  = 6;    // ... this often within kReopenWindowMs, TeamSpeak never sending: given up
    static constexpr int kEntryOrder    = 40; // the paperclip menu (attachmenu.h): after Photo or video, Document, Poll

    struct Host {
        std::function<QWidget*()>                                         input;             // TeamSpeak's chat input on screen
        std::function<quint64()>                                          visibleConnection; // the server tab on screen
        std::function<std::unique_ptr<music::Source>(const QString& path)> source;          // music::fileSource
        std::function<QString(QWidget* parent, const QString& dir)>       pickFile;          // the file dialog
        std::function<qint64()>                                           clock;             // tests: the engine's clock
        // 2.3: where the music plays, for the compact bar on another server tab: the server's and the channel's
        // names (raw; the controller sanitizes them). Empty: "another server tab".
        std::function<QString(quint64 sch, quint64 channel)>              placeName;
        // 2.3 live test: brings sch's server tab to the front (TeamSpeak's current connection may follow
        // later); false: it can't (servertabs::show).
        std::function<bool(quint64 sch)>                                  showConnection;
        QString                                                           settingsFile;      // music.ini (empty: not saved)
    };
    // What the plugin passes: the visible ChatLineEdit, ts3::currentConnection, Media Foundation, QFileDialog,
    // TeamSpeak's server tab bar.
    static Host pluginHost();

    explicit MusicController(Host host, QObject* parent = nullptr);
    ~MusicController() override; // shutdown()

    void pick(QWidget* anchor);       // the attach menu: the file dialog, then start()
    bool start(const QString& path);  // plays path to the visible tab's channel (false: the bar says why)
    // Pause, or play (the bar's button). Play while another server tab is in front brings the music's tab to the
    // front first (TeamSpeak sends only from there): the music plays on as soon as that tab is in front.
    void togglePause();
    // 2.3 polish review: /tsmedia music pause and play, never the other way round. pause(): it stays paused, also
    // music waiting for its server tab (back there it doesn't play on); play(): as Play in the bar.
    void pause();
    void play();
    void stop();                      // the music and its bar go
    void seek(qint64 ms);
    void setVolume(int percent);
    void toggleMute();

    // TeamSpeak's events (plugin.cpp queues them to the GUI thread).
    void serverTabChanged(quint64 sch);
    void clientMoved(quint64 sch, quint16 client, quint64 newChannel); // client: TeamSpeak's anyID
    void connectionLost(quint64 sch);
    void shutdown(); // plugin shutdown: stopped, TeamSpeak's input given back, the bar deleted, the menu entry gone

    bool           playing() const { return m_engine != nullptr; }
    quint64        connection() const { return m_sch; }
    music::Engine* engine() const { return m_engine.get(); }
    MusicBar*      bar() const { return m_bar.data(); }
    bool           inputKeptOpen() const { return m_inputForced; }
    bool           voiceActivationPaused() const { return m_vadKicked; } // 2.3 live test: TeamSpeak's "vad" off for a moment
    // 2.3 live test: paused (by itself, or Play pressed on another tab) until its server tab is in front again.
    bool           waitingForItsTab() const { return m_engine && m_resumeOnReturn; }
    int            volume() const { return m_volume; }
    bool           muted() const { return m_muted; }
    void           refreshNow(); // one tick at once (tests; TeamSpeak's events)

    static QString diagnosticsTitle();
    QStringList    diagnosticLines() const;
    static MusicController* current(); // the plugin's (the live-test driver); nullptr if none
    QString                 problem() const; // the bar's problem line now (empty: none)
    QString                 awayLabel() const; // 2.3: "Weekend Squad › Lobby" (sanitized, short), for the bar on another tab

  private:
    struct VoiceState {
        bool    connected   = false;
        quint64 channel     = 0;
        bool    inputMuted  = false;
        bool    outputMuted = false;
        bool    hardware    = true; // a capture device is open
        bool    canTalk     = true; // talk power (or talker) in the channel
        int     codec       = -1;
    };
    static VoiceState readVoiceState(quint64 sch);

    void    tick();
    bool    followTab(const music::Engine::Snapshot& s); // 2.3 live test: paused / played on for the tab in front
    void    pauseMusic(); // the engine paused (faded out for a push-to-talk input it keeps open), the input given back
    bool    checkTeamSpeak(); // false: it stopped
    void    keepInputOpen(const music::Engine::Snapshot& s);
    void    restoreInput();
    void    startVadKick();
    void    endVadKick();
    void    readMutes(); // TeamSpeak's microphone and sound mute, every tick: muted, the music stays out of the voice
    void    stopMusic();
    void    updateBar(const music::Engine::Snapshot& s);
    QString problemText(MusicBar::Problem* kind) const;
    void    showNotice(MusicBar::NoticeIcon icon, const QString& text, quint64 sch);
    void    endNotice();
    void    ensureBar();
    void    placeBar();
    void    keepAtEnd(QTextBrowser* chat); // the bar's height changed: the chat above shows its end again
    QWidget* input();
    bool    canStart(quint64 sch, QString* why) const;
    float   gain() const;
    void    loadSettings();
    void    saveSettingsSoon();
    void    saveSettings();
    qint64  nowMs() const;

    Host                           m_host;
    std::unique_ptr<music::Engine> m_engine;
    QPointer<MusicBar>             m_bar;
    QPointer<QWidget>              m_barInput;   // the input the bar is stacked on
    bool                           m_barOverlay = false; // no layout to go into: it floats
    QPointer<QWidget>              m_input;
    QTimer*                        m_tick        = nullptr;
    QTimer*                        m_noticeTimer = nullptr;
    QTimer*                        m_saveTimer   = nullptr;

    quint64        m_sch       = 0; // the music's connection and channel
    quint64        m_channel   = 0;
    QString        m_title;         // sanitized; the file's name until its tags are read
    QString        m_artist;
    bool           m_tagsRead  = false;
    qint64         m_startedMs = 0;
    int            m_ticks     = 0;
    VoiceState     m_voice;
    bool           m_inputForced = false; // the plugin keeps TeamSpeak's (push-to-talk) input open
    bool           m_pttGaveUp   = false; // ... TeamSpeak kept closing it: not any more
    QVector<qint64> m_reopens;            // when it was opened again (the give-up rule)
    bool           m_vadKicked   = false; // 2.3 live test: the plugin turned TeamSpeak's voice activation off (given back)
    qint64         m_vadKickAt   = 0;
    bool           m_vadGaveUp   = false; // ... TeamSpeak never started sending: not any more
    QVector<qint64> m_vadKicks;           // when it was turned off (the give-up rule)
    bool           m_home           = true;  // 2.3 live test: its server tab was in front at the last look
    bool           m_resumeOnReturn = false; // ... paused for another tab (or Play pressed there): plays on back home
    bool           m_switchFailed   = false; // ... Play couldn't bring its tab to the front (the bar says so)
    int            m_volume = kDefaultVolume;
    bool           m_muted  = false;
    QString        m_lastDir;
    bool           m_notice    = false; // the bar shows a notice
    quint64        m_noticeSch = 0;     // ... on this server tab
    bool           m_shutDown  = false;

    // 2.2 diagnostics style counters (no names, no paths)
    int m_diagStarted = 0;
    int m_diagFailed  = 0;
    int m_diagReopened = 0;
    int m_diagVadKicks = 0;
    int m_diagTabPauses = 0; // 2.3 live test: paused for another server tab
};
