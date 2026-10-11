#pragma once

// 2.2 emoji: the emoji picker in text fields.
//  * TeamSpeak's chat input (ChatLineEdit, a QTextEdit): TeamSpeak's own emoticon button next to the
//    input shows our HD face and opens our picker (the takeover). The button keeps its widget, place,
//    size, layout slot and the skin's styling; only its painting and its left clicks are ours, and
//    nothing of TeamSpeak's is disconnected or changed (its tool tip and accessible texts are swapped
//    and given back). Ctrl+E opens the picker while the input has the focus (Windows' own Win + . panel
//    keeps working). The picked emoji is inserted at the cursor; Shift keeps the picker open for more.
//    2.2.1: whichever button opened the picker, the emoji goes in through ChatInputs (chatinput.h),
//    which gives the input the focus first so TeamSpeak takes its "Enter Chat Message..." placeholder
//    out, instead of the emoji going out with it.
//  * Fallback (no such button, a hidden one, or one of a class we can't draw): a smiley button inside
//    the input, in a margin of the input's own (its viewport is narrowed, so text never runs under the
//    button), as in 2.2.0. That margin is the input's slot mechanism (reserveSlots() and slotRect()):
//    room for our buttons at the input's right edge, slot 0 the rightmost.
//  * 2.2.1 mic: slot 0 always holds the microphone button (micbutton.h), placed by layout() on every
//    path, also with Settings::emojiButton off; the fallback smiley, when shown, is in slot 1 left of it.
//  * The send window's caption field: the same picker from a button inside the field (addPickerButton).
// The face is grey like TeamSpeak's icons (tinted with the tone of TeamSpeak's own icon) and shows a
// random face in colour under the pointer, as in Discord. Turning Settings::emojiButton off gives
// TeamSpeak's button back and removes the fallback (the microphone keeps its slot); on destruction
// everything is given back (margins, both buttons, filters, texts).

#include <QColor>
#include <QHash>
#include <QMargins>
#include <QMetaObject>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVector>

class ChatInputs;
class EmojiPicker;
class QAbstractButton;
class QLineEdit;
class QTextEdit;

class EmojiInput : public QObject
{
    Q_OBJECT

  public:
    explicit EmojiInput(QObject* parent = nullptr);
    ~EmojiInput() override;

    void attach(QTextEdit* input);
    // ChatIntegration's discover pass: TeamSpeak's emoticon button is looked for again until it is found
    // (it may be created after the input).
    void rediscover();
    void settingsChanged(); // Settings::emojiButton
    bool hasInput() const;
    void insertEmoji(int id);      // into the input that is shown (the last used one)
    void openPicker(QTextEdit* input);
    // The attached inputs' placeholders, and putting text into them (2.2.1).
    ChatInputs* inputs() const { return m_text; }

    // A button inside field (trailing) that opens the picker and inserts at the cursor.
    static void addPickerButton(QLineEdit* field);

    // ---- also for tests and the live-test driver ----------------------------------------------------
    // TeamSpeak's emoticon button for input by the discovery rules (nullptr: none), whatever its class.
    static QAbstractButton* findTeamSpeakButton(QTextEdit* input);
    QAbstractButton*        takenButton(QTextEdit* input) const;    // TeamSpeak's button we draw on, or nullptr
    QAbstractButton*        fallbackButton(QTextEdit* input) const; // our in-input button while it is shown, or nullptr
    bool                    pickerOpen() const;
    QString                 describe() const; // one line per input: what was found and what is shown

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // TeamSpeak's button while we draw on it.
    struct Taken {
        QPointer<QAbstractButton> button;
        QString                   toolTip; // TeamSpeak's, given back
        QString                   accessibleName;
        QString                   accessibleDescription;
        QMetaObject::Connection   pressed;
        QMetaObject::Connection   clicked;
        QMetaObject::Connection   destroyed;
        bool                      held   = false; // our left press (or Space / Enter) is held
        bool                      inside = true;  // ... and the pointer is on the button
        int                       face   = -1;    // the colour face shown under the pointer
        QColor                    tone;           // the mean colour of TeamSpeak's own icon
        qint64                    iconKey = 0;    // its icon's cache key when the tone was sampled
    };
    struct Input {
        QPointer<QTextEdit>       edit;
        QPointer<QAbstractButton> button;   // the fallback inside the input
        QMargins                  original; // the input's viewport margins before ours
        bool                      ours      = false;
        int                       slotCount = 0; // our slots at the input's right edge (reserveSlots)
        Taken                     taken;
    };

    Input* inputFor(QObject* watched);
    Input* inputForButton(QObject* watched);
    void   layout(Input& input);
    void   detach(Input& input);
    bool   takeOver(Input& input, QAbstractButton* button);
    void   release(Input& input); // gives TeamSpeak's button back as it was
    // The input's right-edge slots: room for our buttons inside the input, kMargin wide each, in a viewport
    // margin of the input's own (text never runs under them). Slot 0 is the rightmost, slot 1 left of it.
    // 2.2.1: slot 0 is the microphone's (always), slot 1 the fallback's while it is shown.
    void   reserveSlots(Input& input, int count);         // 0 (only detach()): the input gets its own margins back
    QRect  slotRect(const Input& input, int index) const; // input coordinates, as high as the viewport
    void   showFallback(Input& input);
    void   hideFallback(Input& input);
    bool   filterTaken(Input& input, QEvent* event);
    void   paintTaken(Input& input);
    void   sampleTone(Taken& taken);
    void   activate(Input& input); // a click (or Space / Enter) on TeamSpeak's button
    void   safetyNet(const QPointer<QTextEdit>& edit); // TeamSpeak's own handler ran after all
    QPixmap facePixmap(int face, int logicalSize, int drawSize, qreal dpr, const QColor& tone, bool dark, int mode);

    QVector<Input>          m_inputs;
    QPointer<QTextEdit>     m_last; // the input the picker was last used for
    QPointer<EmojiPicker>   m_picker;
    QHash<QString, QPixmap> m_pixmaps;        // faces by size, ratio, tone and look
    ChatInputs*             m_text = nullptr; // a child
};
