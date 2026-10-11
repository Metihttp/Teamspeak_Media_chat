#pragma once

// 2.2.1 mic: the microphone button in TeamSpeak's chat input (ChatLineEdit, a QTextEdit). It sits in the
// input's right-edge slot: the margin EmojiInput reserves inside the input (the viewport is narrowed, so
// text never runs under it), where 2.2.0 had its emoji button. EmojiInput::layout() calls placeIn() with
// that strip, and removeFrom() when it gives the input back.
//
//   Ready        a grey HD microphone; darker and a little larger under the pointer. Hold: record.
//   Unavailable  dimmed and disabled; the tool tip says why (not connected, a password-protected
//                channel, a private chat whose partner is unknown, no Media Foundation).
//   Holding      held to record: a red disc with a white microphone and a soft pulse (no pulse with
//                Windows animations off); grey while the pointer is in the cancel zone.
//   Recording    the recorder window records (the Plugins menu, /tsmedia voice): a red disc with a white
//                stop square and the pulse. Click: stop and send.
//   Busy         the recorder window is open but not recording (sending, an error, a kept recording), or
//                a held recording is being sent: the microphone in the accent colour. Click: shows the
//                window.
//
// Hold to record (like Telegram): a left press on the icon asks the handler (pressed()); when it starts a
// hold, the button follows the pointer until the release: more than kCancelPx from the icon's centre is
// the cancel zone (zoneChanged()), back within kRearmPx leaves it again, and the hold ends with released():
// Send (let go outside the cancel zone), Cancel (inside it), Escape (Esc while held) or Lost (the window
// was deactivated, or the button hidden or disabled: no release will come). After Escape, Lost or
// abandonHold() the real release does nothing, and the second press of a double click never starts a
// hold. A press the handler doesn't take is an ordinary click (clicked() on a release over the icon),
// unless the handler disabled the button meanwhile (the press then ends there).
//
// Every button shows the same state: VoiceController sets it (setSharedState) and receives the gestures
// (setHandler). The button never takes the focus (typing stays in the input); the keyboard ways are the
// Plugins menu and /tsmedia voice. Drawn with QPainter paths: sharp at any scale, light and dark.
// objectName "tsmediaMicButton", so plugin shutdown's sweep finds it among TeamSpeak's widgets.

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QPoint>
#include <QString>

class QTextEdit;
class QTimer;

class MicButton : public QAbstractButton
{
    Q_OBJECT

  public:
    enum class Mode { Ready, Unavailable, Holding, Recording, Busy };

    struct State {
        Mode    mode = Mode::Ready;
        QString reason;         // Unavailable: why (the tool tip and the accessible description); Busy: the
                                // tool tip (empty: "Show the voice message window")
        bool    cancel = false; // Holding: the pointer is in the cancel zone

        bool operator==(const State& other) const { return mode == other.mode && reason == other.reason && cancel == other.cancel; }
        bool operator!=(const State& other) const { return !(*this == other); }
    };

    static constexpr int kIcon     = 24; // logical px: the icon's box inside the strip
    static constexpr int kCancelPx = 72; // the cancel zone: this far from the icon's centre (any direction)
    static constexpr int kRearmPx  = 56; // ... and left again this close (no flicker at the edge)

    explicit MicButton(QWidget* input);
    ~MicButton() override;

    // ---- EmojiInput's slot ---------------------------------------------------------------------------
    // input's button (made on first use) in strip (input coordinates), shown and raised.
    static MicButton* placeIn(QTextEdit* input, const QRect& strip);
    static void       removeFrom(QTextEdit* input); // deletes input's button, if any
    static MicButton* of(const QWidget* input);     // input's button, or nullptr

    // ---- every button --------------------------------------------------------------------------------
    static void         setSharedState(const State& state); // all buttons, and the ones made later
    static const State& sharedState();
    static bool         anyVisible();

    // How a hold ended.
    enum class Release { Send, Cancel, Escape, Lost };
    // Where the gestures go. Plain functions: the receiver is held weakly, so nothing is called once it is
    // gone. clearHandler(receiver) forgets it.
    struct Handler {
        bool (*pressed)(QObject* receiver, MicButton* button) = nullptr; // a left press: true starts a hold
        void (*zoneChanged)(QObject* receiver, bool cancel)   = nullptr; // the hold's pointer entered / left the cancel zone
        void (*released)(QObject* receiver, Release how)      = nullptr; // the hold ended
        void (*clicked)(QObject* receiver)                    = nullptr; // a press that wasn't a hold, released on the icon
        void (*hovered)(QObject* receiver)                    = nullptr; // the pointer came onto a button (may set a new state first)
    };
    static void setHandler(QObject* receiver, const Handler& handler);
    static void clearHandler(QObject* receiver);
    static void abandonHolds(); // abandonHold() on every button (the controller's shutdown)

    // ---- this button (tests, the harness) -----------------------------------------------------------
    void         setState(const State& state);
    const State& state() const { return m_state; }
    void         place(const QRect& strip); // the strip right of the text, as high as the viewport
    QRect        iconRect() const { return QRect(m_icon, QSize(kIcon, kIcon)); }
    bool         pulsing() const; // the recording pulse runs
    bool         holding() const { return m_hold; } // between a hold's press and its end
    bool         inCancelZone() const { return m_hold && m_cancelZone; }
    // The hold ends without a word to the handler (it ended the recording itself: 5:00, a problem, unload):
    // no more zone changes, and the coming release does nothing.
    void         abandonHold();

    // The pictures, for render tools: the button as it is drawn in a box of kIcon x kIcon logical px.
    struct Look {
        bool hover   = false;
        bool pressed = false;
        bool dark    = false;
        bool cancel  = false; // Holding: in the cancel zone
        qreal pulse  = -1.0; // 0..1: the pulse's phase; < 0: none
    };
    static void paintIcon(QPainter& p, const QRectF& box, Mode mode, const Look& look);

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override; // Esc while held (on the application)
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void apply();
    void updatePulse();
    bool dark() const;
    void beginHold(const QPoint& globalPos);
    void endHold(Release how); // tells the handler (unless abandoned), then forgets the hold
    void stopTracking();       // the filter goes, the button comes up
    bool zoneAt(const QPoint& globalPos) const;

    State         m_state;
    QPoint        m_icon;
    QTimer*       m_pulse = nullptr; // a child: it dies with the button
    QElapsedTimer m_pulseClock;
    bool          m_hold       = false; // a hold is going on (pressed, not yet ended)
    bool          m_swallow    = false; // the left button is still down, but its release means nothing
    bool          m_cancelZone = false;
    bool          m_filtering  = false; // our filter is on the application
    QPoint        m_center;             // the icon's centre at the press, global
};
