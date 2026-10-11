#pragma once

// 2.3 poll: the "New poll" window (the attach menu's Poll). A question (up to 200 characters), 2 to 10
// options (up to 100 each) that can be added, removed and reordered (drag the grip, or Alt+Up / Alt+Down
// in an option), "Allow multiple answers" (off by default), and a line that says the votes are anonymous.
// Enter in an option goes to the next one (or adds one after the last), Backspace in an empty option
// removes it, Ctrl+Enter posts, Esc cancels. Problems are shown under the fields when Post is pressed.
// Themed by its own style sheet from TeamSpeak's palette (light or dark skin). Named "tsmediaPollDialog":
// plugin shutdown closes it.

#include <QDialog>
#include <QList>
#include <QStringList>

#include <functional>

class QAbstractButton;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class PollOptionRow;
class PollSwitch;

class PollDialog : public QDialog
{
    Q_OBJECT

  public:
    struct Result {
        QString     question;
        QStringList options;
        bool        multiple = false;
    };

    // destination: where it goes ("the channel “Home”").
    explicit PollDialog(const QString& destination, QWidget* parent = nullptr);
    ~PollDialog() override;

    // An extra check before posting (the message's size): an error text, or empty when it can go.
    void   setValidator(std::function<QString(const Result&)> validator) { m_validator = std::move(validator); }
    Result result() const; // cleaned: empty options left out

    // ---- tests and the harness ------------------------------------------------------------------------
    QLineEdit*        questionField() const { return m_question; }
    QList<QLineEdit*> optionFields() const;
    QAbstractButton*  addButton() const;
    QAbstractButton*  multipleSwitch() const;
    QPushButton*      postButton() const { return m_post; }
    QString           errorText() const;
    void              addOption(const QString& text = QString(), bool focus = true);
    void              removeOption(int index);
    void              moveOption(int from, int to);
    bool              tryPost(); // what Post does: checks, then accepts

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override; // fits the height again with the sizes it is shown with

  private:
    friend class PollOptionRow;
    friend class PollGrip;
    void applyTheme();
    void relayoutOptions();
    void updateCounters();
    void fitHeight(); // adjustSize(), then as tall as the content (the screen permitting)
    void showError(const QString& text, QWidget* field, QLabel* where); // where: next to what it is about (nullptr: above the buttons)
    void clearErrors();
    int  indexOf(const QObject* field) const;
    void dragRow(PollOptionRow* row, int globalY);

    QLabel*                 m_title    = nullptr;
    QLabel*                 m_subtitle = nullptr;
    QLineEdit*              m_question = nullptr;
    QLabel*                 m_questionCount = nullptr;
    QWidget*                m_optionsHost   = nullptr;
    QVBoxLayout*            m_optionsLayout = nullptr;
    QList<PollOptionRow*>   m_rows;
    QPushButton*            m_add      = nullptr;
    QLabel*                 m_addHint  = nullptr;
    PollSwitch*             m_multiple = nullptr;
    QLabel*                 m_anonymous = nullptr;
    QLabel*                 m_lock     = nullptr;
    QLabel*                 m_error    = nullptr;
    QLabel*                 m_questionError = nullptr;
    QLabel*                 m_optionsError  = nullptr;
    QPushButton*            m_cancel   = nullptr;
    QPushButton*            m_post     = nullptr;
    std::function<QString(const Result&)> m_validator;
    bool                    m_applyingTheme = false;
    bool                    m_dark          = false;
};
