#pragma once

// 2.2 voice: the "Voice message" window. It is on screen for as long as the microphone is open (the
// recording never runs without it): a red dot and "Recording", the destination, a live waveform of the
// last 3 seconds (the level meter), the time and the limit; then the review (play, a seekable waveform,
// the length and size) with Discard / Re-record / Send; an inline "Discard this voice message?"; the
// error states with a way to fix them.
//
// A view only: VoiceController decides what it shows (setView) and acts on its signals. Plain text
// everywhere, strings through i18n::t, style sheets with QString::fromLatin1. objectName
// "tsmediaVoiceRecorder", so plugin shutdown finds it.
//
// Keys (while no button has keyboard focus; a focused button takes Space / Enter as usual):
//   recording: Enter = Send, Space = Stop, Esc = Cancel
//   review:    Enter = Send, Space = play / pause, Esc = Discard; on the waveform Left / Right = 5 s,
//              Home / End
//   confirm:   Esc = Keep
//   error:     Enter = the fix, Esc = Close

#include <QByteArray>
#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <QVector>

class QLabel;
class QPushButton;
class QHBoxLayout;
class QWidget;

class VoicePanel : public QDialog
{
    Q_OBJECT

  public:
    enum class Mode { Starting, Recording, Saving, Review, TooShort, Error };
    enum class Fix { None, TryAgain, WindowsPrivacy, SoundSettings, PluginSettings };

    struct View {
        Mode           mode = Mode::Starting;
        QString        target;          // "the channel “Lobby”"
        QString        device;          // the microphone's name (tooltip of the status)
        qint64         timeMs  = 0;     // recording: length so far; review: playback position
        qint64         lengthMs = 0;    // review: the message's length
        qint64         limitMs = 300000;
        QVector<float> liveBins;        // recording: 50 ms peaks (dBFS), oldest first (the last ones are shown)
        QByteArray     levels;          // review: the waveform (64 levels 0..15)
        bool           playing  = false; // review
        bool           started  = false; // review: playback moved away from the start (played part shown)
        qint64         bytes    = 0;     // review: file size
        QStringList    hints;           // lines under the waveform, most important first
        bool           hintError = false; // the first hint is a problem (error colour)
        bool           timeWarning = false; // the last 30 seconds: the time in the error colour
        bool           canSend  = true;
        bool           confirmDiscard = false;
        bool           savingShown = false; // Saving: "Saving…" (only after 300 ms)
        QString        errorTitle;
        QString        errorBody;
        Fix            fix = Fix::None;
        QString        fixText;
        bool           animate = true; // Windows animations: the dot pulses, the spinner turns
    };

    explicit VoicePanel(QWidget* parent = nullptr);

    void        setView(const View& view);
    const View& view() const { return m_view; }

    // Centred over anchor (the chat input), 8 px above it, kept on its screen; again whenever the
    // window's height changes (its bottom edge stays put).
    void placeNear(QWidget* anchor);

  signals:
    void sendRequested();
    void stopRequested();
    void cancelRequested(); // Cancel, Discard, Esc, the close button: the controller may ask first
    void rerecordRequested();
    void keepRequested();      // "Keep" in the discard question
    void discardConfirmed();   // "Discard" in the discard question
    void playToggled();
    void seekRequested(double fraction);
    void seekByRequested(qint64 deltaMs); // keyboard on the waveform
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
    class PlayButton;
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
    PlayButton*  m_play;
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
    QPushButton* m_left;   // Cancel / Discard / Close / Keep
    QPushButton* m_middle; // Stop / Re-record
    QPushButton* m_right;  // Send / the fix / Discard (question)
    QPointer<QWidget> m_anchor;
    QString      m_lastStatus;
    QString      m_lastHint;
    QString      m_layoutKey;
    int          m_placedBottom = -1;
};
