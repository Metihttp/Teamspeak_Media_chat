#pragma once

// 2.2.1 hold to record: the slim strip right above TeamSpeak's chat input while the microphone button is
// held, and for a moment after a hold that sent nothing (a tap, a cancel, too short) or that 5:00 sent.
//
//   Recording  a pulsing red dot (steady with Windows animations off), the time and "/ 5:00", a live
//              waveform of the last seconds and "Release to send · Move away to cancel". From 4:30 the
//              time is red; a problem (not connected, no sound, the limit) replaces the hint, in the
//              error colour.
//   Cancel     the pointer is in the cancel zone: the strip turns red, a bin and "Release to cancel"
//              (never colour alone).
//   Saving     released: "Sending…" (or "Saving…" when it is going to be kept in the recorder window).
//   Notice     one line with an icon: "Hold to record, release to send", "Canceled", "Too short", ...
//
// A view only: VoiceController decides what it shows (setView) and where (placeAbove). It never takes the
// mouse or the focus (the button keeps the press, the input the keys). A child of the input's window,
// raised over the bottom of the chat, as wide as the input, its bottom edge on the input's top edge:
// TeamSpeak's layouts are never touched, and it hides with its input (another server tab). Never a
// window of its own (no Qt::WA_QuitOnClose either: it can't keep TeamSpeak running). Drawn with QPainter
// (render() for the harness and the tests); colours from the input's palette (light and dark), texts
// through i18n::t. objectName "tsmediaHoldStrip", so plugin shutdown's sweep finds it among TeamSpeak's
// widgets.

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

class HoldStrip : public QWidget
{
    Q_OBJECT

  public:
    enum class Mode { Recording, Cancel, Saving, Notice };
    enum class Icon { Info, Canceled, Sent };

    struct View {
        Mode           mode    = Mode::Recording;
        qint64         timeMs  = 0;
        qint64         limitMs = 300000;
        QVector<float> liveBins;            // 50 ms peaks (dBFS), oldest first (the last ones are shown)
        bool           timeWarning = false; // the last 30 seconds: the time in the error colour
        QString        problem;             // Recording: shown instead of the hint, in the error colour
        QString        text;                // Saving and Notice: the line
        Icon           icon    = Icon::Info; // Notice
        bool           animate = true;       // Windows animations: the dot pulses, the spinner turns
    };

    // The input's theme, and the moment of the pulse (render() only; the widget keeps its own clock).
    struct Look {
        bool   dark = false;
        QColor base;       // the input's background
        QFont  font;       // the input's font
        qreal  phase = -1; // 0..1 of the dot's pulse and the spinner's turn; < 0: still
    };

    explicit HoldStrip(QWidget* parent = nullptr);
    ~HoldStrip() override; // TeamSpeak's input loses our filter

    void        setView(const View& view);
    const View& view() const { return m_view; }

    // As wide as input, right above it, in input's window (made its child if it isn't yet), raised; the
    // theme follows the input. Again whenever the controller refreshes (TeamSpeak's window resized).
    void     placeAbove(QWidget* input);
    QWidget* input() const { return m_input.data(); }

    // What render() shows on the right (Recording, Cancel, Saving) or as the line (Notice): the tests.
    static QString line(const View& view);
    static int     heightFor(const QFont& inputFont);
    static void    render(QPainter& p, const QRectF& rect, const View& view, const Look& look);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // the input hidden: so is the strip
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void updateClock();
    Look look() const;

    View              m_view;
    QPointer<QWidget> m_input;
    QTimer*           m_frame = nullptr; // a child: the pulse while shown and animated
    QElapsedTimer     m_clock;
    QString           m_announced;
};
