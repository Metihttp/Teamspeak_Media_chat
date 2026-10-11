#pragma once

// 2.2 voice: the "Voice message" window. It is on screen for as long as the microphone is open (the
// recording never runs without it). 2.2.1: no review step; what you record is sent.
//
//   Recording  a red dot and "Recording", the destination, a live waveform of the last seconds (the
//              level meter), the time and the 5:00 limit; Cancel (discard) and a big Send (stop and
//              send at once). From 4:30 the time is red and the window says it is sent at 5:00.
//   Saving     "Sending…" (after 300 ms) while the recording is encoded; then the upload panel takes
//              over and the window closes. At 5:00 it gets here by itself and says so.
//   TooShort   "Too short to send", nothing was sent; closes by itself.
//   Error      what went wrong and a way to fix it (Try again, Windows settings, Send what was kept).
//   An inline "Discard this voice message?" (Keep / Discard) for recordings of 3 seconds or more.
//
// A view only: VoiceController decides what it shows (setView) and acts on its signals. Plain text
// everywhere, strings through i18n::t, style sheets with QString::fromLatin1. objectName
// "tsmediaVoiceRecorder", so plugin shutdown finds it. A top-level window without a parent, but never one
// that keeps TeamSpeak running (no Qt::WA_QuitOnClose): TeamSpeak's Quit works while it is open.
//
// Keys (while no button has keyboard focus; a focused button takes Space / Enter as usual):
//   Enter = Send (the fix in an error, Close when too short), Esc = Cancel (Close; Keep in the question)

#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <QVector>

class QLabel;
class QPushButton;
class QWidget;

class VoicePanel : public QDialog
{
    Q_OBJECT

  public:
    enum class Mode { Starting, Recording, Saving, TooShort, Error };
    enum class Fix { None, TryAgain, Send, WindowsPrivacy, SoundSettings };

    struct View {
        Mode           mode = Mode::Starting;
        QString        target;            // "the channel “Lobby”"
        QString        device;            // the microphone's name (tooltip of the status)
        qint64         timeMs  = 0;       // the length so far
        qint64         limitMs = 300000;
        QVector<float> liveBins;          // 50 ms peaks (dBFS), oldest first (the last ones are shown)
        QStringList    hints;             // lines under the waveform, most important first
        bool           hintError = false; // the first hint is a problem (error colour)
        bool           timeWarning = false; // the last 30 seconds: the time in the error colour
        bool           confirmDiscard = false;
        bool           sendingShown = false; // Saving: "Sending…" (only after 300 ms)
        bool           sending = true;    // Saving: it goes once saved (else "Saving…" and Send stays)
        QString        errorTitle;
        QString        errorBody;
        Fix            fix = Fix::None;
        QString        fixText;
        bool           animate = true; // Windows animations: the dot pulses
    };

    explicit VoicePanel(QWidget* parent = nullptr);

    void        setView(const View& view);
    const View& view() const { return m_view; }

    // Centred over anchor (the chat input), 8 px above it, kept on its screen; again whenever the
    // window's height changes (its bottom edge stays put).
    void placeNear(QWidget* anchor);

    // Tests and the harness: the buttons as shown (left: Cancel / Close / Keep; send: Send / the fix /
    // Discard in the question).
    QPushButton* leftButton() const { return m_left; }
    QPushButton* sendButton() const { return m_send; }

  signals:
    void sendRequested();   // Send, Enter, the Send fix
    void cancelRequested(); // Cancel, Esc, the close button: the controller may ask first
    void keepRequested();      // "Keep" in the discard question
    void discardConfirmed();   // "Discard" in the discard question
    void fixRequested();
    void closeRequested(); // Close in the error / too-short states

  protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    class WaveStrip;
    class Dot;

    void applyTheme();
    void rebuildButtons();
    void announce(QWidget* widget);
    void keepPlaced();
    void fitHeight(bool invalidate);
    bool buttonHasFocus() const;

    View m_view;
    bool m_dark          = false;
    bool m_applyingTheme = false;

    Dot*         m_dot;
    QLabel*      m_status;
    QLabel*      m_target;
    QWidget*     m_mainRow;
    WaveStrip*   m_wave;
    QLabel*      m_time;
    QLabel*      m_limit;
    QLabel*      m_hint;  // the first hint (a problem in the error colour)
    QLabel*      m_hint2; // the others, secondary
    QWidget*     m_errorBox;
    QLabel*      m_errorIcon;
    QLabel*      m_errorTitle;
    QLabel*      m_errorBody;
    QLabel*      m_question;
    QPushButton* m_left; // Cancel / Close / Keep
    QPushButton* m_send; // Send / the fix / Discard (question)
    QPointer<QWidget> m_anchor;
    QString      m_lastStatus;
    QString      m_lastHint;
    QString      m_layoutKey;
    int          m_placedBottom = -1;
};
