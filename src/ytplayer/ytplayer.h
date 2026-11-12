#pragma once

// 2.5 YouTube player (design2.md 3.7-3.12): YouTube's own player for a YouTube card, in a window of the player helper
// (Microsoft Edge WebView2, player/tsmp.cpp). One per plugin (owned by ChatLinkPreviews), one player at a time.
//  * State per card: None, Consent (the one-time notice drawn on the card), Starting, Open (anchored), Popup, Hidden
//    (anchored, paused and hidden), Note (embedding disabled, removed, any other failure).
//  * Play loads nothing before the click: the helper is extracted and loaded inside the first Play of the session
//    (PlayerBackend::load); a missing runtime, a refused helper or the switch off open the browser as in 2.4.
//  * The card is always looked up again by its slot id (Host::locate) when an event arrives; never held as a pointer.
//  * Owned timers only (children of this object), no functor singleShot, no metatype, no dialog (the notice is a card
//    state): the CRASH RULE. shutdown() stops everything and calls the backend's shutdown exactly once.
//  * The anchored window (over the card's thumbnail) is behind ytpage::kAnchoredPlayer (false in 2.5.0 until its live
//    checks pass); until then every Play is the popup player.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSizeF>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

#include "ytplayer/ytbackend.h"

class QScrollBar;
class QTimer;
class QWidget;

class YouTubePlayer : public QObject
{
    Q_OBJECT

  public:
    enum class State { None, Consent, Starting, Open, Popup, Hidden, Note };

    // Where a slot's card is, as the chat knows it now.
    struct Spot {
        QPointer<QWidget> viewport;      // the chat's viewport (its QTextBrowser is its parent)
        QRect             thumb;         // the card's thumbnail, screen device px
        QRect             view;          // the viewport, screen device px (clip = thumb & view)
        QRect             owner;         // TeamSpeak's top-level window's client area, screen device px
        void*             ownerHwnd = nullptr;
        QSizeF            thumbDu;
        double            s = 1.0, dpr = 1.0, sysScale = 1.0;
        double            popupS = 0.0;  // the popup's scale: the chat's without the narrow-column fit (0: s)
        QString           title;         // the card's cleaned title (the popup's title)
        bool              shown = true;  // the chat is on screen (its tab, its window not minimized)
    };
    struct Host {
        std::function<bool(const QString& slotId, Spot* spot)> locate;   // false: the slot is gone
        std::function<void(const QString& slotId)>              changed;  // draw that card again
        std::function<void(const QUrl& url)>                    openUrl;  // the browser (https only)
        std::function<void()>                                   pauseOthers; // TS Media's own players
        std::function<bool()>                                   muted;    // TS Media's session mute
        std::function<void(const QString& slotId)>              scrollIntoView; // anchored: the whole thumbnail
    };
    // What a card shows (ChatLinkPreviews::cardFor).
    struct Card {
        State   state    = State::None;
        bool    playable = false;
        QString note;
        qint64  resumeMs = 0; // a time pill
    };

    YouTubePlayer(Host host, std::unique_ptr<PlayerBackend> backend, QObject* parent = nullptr);
    ~YouTubePlayer() override;

    // Play goes to our player: the switch on, Show link previews on, the WebView2 Runtime in the registry, the helper
    // built in and not marked unavailable this session. Loads nothing.
    bool playable() const;
    Card cardOf(const QString& slotId, const QString& videoId) const;

    // The card's thumbnail (Zone::Play): Play, Resume (Hidden), bring the window to the front (Popup), or nothing.
    // link: the link as written (the browser fallback; its start time).
    void play(const QString& slotId, const QString& videoId, const QUrl& link, bool forcePopup = false);
    void consentPlay(const QString& slotId);    // the Consent state's Play pill (the user's gesture)
    void consentBrowser(const QString& slotId); // ... its Open in browser pill
    void stop(const QString& slotId);           // the (x) while a player is open
    void tryAgain(const QString& slotId, const QString& videoId, const QUrl& link); // the Note card's menu
    QUrl watchUrlOf(const QString& slotId, const QString& videoId, const QUrl& link) const; // with the time it stopped at

    // The chat moved things: re-place the anchored player, close a player whose message went, drop a Consent whose
    // card went. Synchronous (no deferred functor).
    void relayout();
    // A scale pass or another visit of the chat: notes that aren't remembered go (the card is None again).
    void forgetShownNotes();
    void settingsChanged();  // the switch or Show link previews off: close (and delete WebView2's folder)
    void playbackStarted();  // a TS Media video, audio or voice message started: pause
    void shutdown();         // plugin unload: every timer, filter and connection gone; the backend shut down once
    bool isShutDown() const { return m_down; }
    bool hasPlayer() const { return m_active.id > 0 || m_active.retrying; }
    QString activeSlot() const { return m_active.slotId; }

    // The Y4-spike (TESTHOOKS yt.spike): one popup with a page from memory (no YouTube, no network): creation, rendering,
    // a click on its box (it answers "ready") and Esc (closes it). owner: TeamSpeak's top-level window, ownerRect: its
    // client area in screen device px.
    QString spike(void* owner, const QRect& ownerRect);
    // The live-test driver (yt.state) and the diagnostic info.
    QString describe() const;
    QStringList diagnosticLines() const;

    // ---- tests ----
    void   setAnchoredGateForTests(bool on) { m_gate = on; }
    void   setUserDataFolderForTests(const QString& dir) { m_userData = dir; }
    void   setReleaseWaitForTests(int ms) { m_releaseWaitMs = ms; }
    QTimer* timerForTests(const char* name) const;
    bool   unavailableForTests() const { return m_unavailable; }
    int    engineFailuresForTests() const { return m_engineFailures; }
    QRect  lastClipForTests() const { return m_lastClip; }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Active {
        int     id = 0;
        QString slotId;
        QString videoId;
        QUrl    link;
        bool    popup    = true;
        State   state    = State::None;
        int     lastSec  = -1;   // the page's last Time
        bool    retried  = false; // the one engine retry was used
        bool    retrying = false; // the retry timer runs (no id)
        bool    shown    = false; // the window was shown (Created)
        bool    hiddenByUs = false; // anchored: hidden (moving, scrolled away, tab, minimized)
        bool    deferredShow = false; // popup: Created while TeamSpeak was minimized: shown (paused) on restore
        bool    pauseOnReady = false; // it may not play (TeamSpeak minimized before the page was ready): "pause" at Ready
        bool    volumeHeld = false;   // Ready came while it may not play: the volume (which lifts the view's own mute)
                                      // waits until TeamSpeak is back, so an autoplay that ignores the pause stays silent
        PlayerOpen open;          // what open() got (the retry)
        QPointer<QWidget> viewport;
        QPointer<QWidget> window;
        QPointer<QWidget> browser;
    };
    struct NoteState {
        QString videoId;
        QString text;
        bool    remembered = false;
    };

    void   onPoll();
    void   handle(const PlayerEvent& e);
    void   start(const QString& slotId, const QString& videoId, const QUrl& link, bool forcePopup);
    bool   openActive();
    void   closeActive(const char* why, bool keepCard = false);
    void   engineFailure(const char* why);
    void   showNote(int error);
    void   toBrowser(const QUrl& link, const QString& videoId, int sec);
    void   setState(State state);
    void   applyVolume();
    void   placeActive(bool show);
    void   watch(bool on);
    void   rememberTime(const QString& videoId, int sec);
    void   updatePolling();
    void   deleteUserData();
    void   folderAfterRelease(bool taken); // a release's answer (or none in 20 s) while the folder should go
    void   logTokenOnce();
    QTimer* makeTimer(int ms, bool single, const char* name);

    Host                           m_host;
    std::unique_ptr<PlayerBackend> m_backend;
    Active                         m_active;
    QString                        m_consentSlot;
    QString                        m_consentVideo;
    QUrl                           m_consentLink;
    bool                           m_consentPopup = false;
    QHash<QString, NoteState>      m_notes;       // slot id -> its note
    QHash<QString, int>            m_remembered;  // video id -> error (100, 101, 150) for the session
    QHash<QString, int>            m_times;       // video id -> seconds (at most 256)
    QStringList                    m_timeOrder;
    QTimer*                        m_poll     = nullptr; // 50 ms while a player exists, is created or the engine goes
    QTimer*                        m_creation = nullptr; // 20 s from open(): no Created
    QTimer*                        m_ready    = nullptr; // 15 s from Created: no Ready
    QTimer*                        m_hidden   = nullptr; // 60 s hidden (anchored): close
    QTimer*                        m_settle   = nullptr; // 150 ms after the last scroll / window move (anchored)
    QTimer*                        m_retry    = nullptr; // 1 s: the engine retry
    QTimer*                        m_engine   = nullptr; // 60 s after the last player closed: release the engine
    QVector<QPointer<QScrollBar>>  m_bars;
    bool                           m_engineStarted   = false; // load() succeeded this session
    bool                           m_releasePending  = false; // released, EngineExited not seen yet
    QElapsedTimer                  m_releaseClock;            // since the release (the poll gives up after m_releaseWaitMs)
    int                            m_releaseWaitMs = 20000;   // no EngineExited this long after a release: the poll stops
    bool                           m_deleteAfterExit = false; // the switch went off: the folder goes after EngineExited
    bool                           m_unavailable     = false;
    bool                           m_runtimeLogged   = false;
    bool                           m_tokenLogged     = false;
    bool                           m_down            = false;
    bool                           m_gate            = false; // ytpage::kAnchoredPlayer (tests may turn it on)
    bool                           m_moving          = false;
    int                            m_engineFailures  = 0;
    QString                        m_userData;
    QRect                          m_lastClip;
    QString                        m_lastFailure;
};
