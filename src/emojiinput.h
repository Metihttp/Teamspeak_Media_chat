#pragma once

// 2.2 emoji: the emoji picker in text fields.
//  * TeamSpeak's chat input (ChatLineEdit, a QTextEdit): a smiley button at its right edge, in a
//    margin of the input's own (its viewport is narrowed, so text never runs under the button), and
//    Ctrl+E while the input has the focus (Windows' own Win + . panel keeps working). The picked emoji
//    is inserted at the cursor; Shift keeps the picker open for more.
//  * The send window's caption field: the same button inside the field (addPickerButton).
// The button is grey like the input's other icons and shows a random face in colour under the pointer,
// as in Discord. Everything is given back on destruction (margins, buttons, filters).

#include <QMargins>
#include <QObject>
#include <QPointer>
#include <QVector>

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
    void settingsChanged(); // Settings::emojiButton
    bool hasInput() const;
    void insertEmoji(int id);      // into the input that is shown (the last used one)
    void openPicker(QTextEdit* input);

    // A button inside field (trailing) that opens the picker and inserts at the cursor.
    static void addPickerButton(QLineEdit* field);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Input {
        QPointer<QTextEdit>       edit;
        QPointer<QAbstractButton> button;
        QMargins                  original; // the input's viewport margins before ours
        bool                      ours = false;
    };

    Input* inputFor(QObject* watched);
    void   layout(Input& input);
    void   detach(Input& input);

    QVector<Input>        m_inputs;
    QPointer<QTextEdit>   m_last; // the input the picker was last used for
    QPointer<EmojiPicker> m_picker;
};
