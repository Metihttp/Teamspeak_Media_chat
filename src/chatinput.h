#pragma once

// 2.2.1: TeamSpeak's chat input (ChatLineEdit, a QTextEdit without rich text) has no real placeholder.
// While it is empty and has no focus it holds "Enter Chat Message..." (in the client's language) as grey
// text in its own document, and takes that out again when it gets the focus. Text put into the input
// meanwhile went out with it (an emoji picked into an empty input sent "<emoji>Enter Chat Message...").
// Everything of the plugin that writes into the input, or reads what was typed there while it may have
// no focus, goes through ChatInputs:
//  * insert(): TeamSpeak's window to the front and the input focused first, so TeamSpeak takes its
//    placeholder out itself; then the text at the cursor. When the focus comes later (Windows activates a
//    window asynchronously) the text goes in once the input has it: short tries on a timer of this
//    object, a bounded number, then without the focus. Focused with the placeholder still in after
//    TeamSpeak's moments: the input gets a click's focus-in too (a TeamSpeak that takes it out only on
//    its user's own focus-in). keepFocus (the emoji picker stays open for more):
//    the input gets the focus only for the insertion and the keyboard goes back to the picker; text that
//    can't go in then waits for the input's next focus-in.
//  * The placeholder is learned per input, in any language: the text a focus-in takes out of an input
//    that was empty when it lost the focus, or a focus-out puts into an empty one (only that text: what
//    comes in a moment later, a drop, is the user's), and its colour when that isn't the input's text
//    colour. While exactly that is all an input holds and TeamSpeak put it there (seen happening, or in
//    that colour, or for an input not yet seen changing focus: while it has no focus), the input counts
//    as empty: insert() takes it out first should it still be there (outside the undo history: Ctrl+Z
//    never brings it back as text), typedText() reads nothing. Text the user typed is never taken out,
//    not even the same words.

#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <vector>

class QTextEdit;
class QTimer;

class ChatInputs : public QObject
{
    Q_OBJECT

  public:
    explicit ChatInputs(QObject* parent = nullptr);
    ~ChatInputs() override; // the inputs' filters and connections removed, waiting text dropped

    void attach(QTextEdit* input); // TeamSpeak's chat input line (again: nothing)
    void detach(QTextEdit* input);

    // text at input's cursor (see above).
    void insert(QTextEdit* input, const QString& text, bool keepFocus = false);
    // What was typed into input: its text, or nothing while it holds TeamSpeak's placeholder.
    QString typedText(const QTextEdit* input) const;
    bool    showsPlaceholder(const QTextEdit* input) const;
    // The placeholder learned for input (or for another input: one client, one language); empty until seen.
    QString placeholder(const QTextEdit* input) const;
    bool    hasPending(const QTextEdit* input) const; // text waiting for the input's focus

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // The focus change TeamSpeak is handling. Shown: it has put its placeholder in on that focus-out (later
    // changes in the turn are its own only while the text stays the same: it giving the text its colour).
    enum class Turn { None, In, Out, Shown };
    struct Watch {
        QPointer<QTextEdit>     edit;
        QMetaObject::Connection changes;
        QString                 placeholder; // learned for this input
        QColor                  color;       // its colour when not the input's text colour (else invalid)
        bool                    known = false; // a focus change was seen: shows follows TeamSpeak's own
        bool                    shows = false; // TeamSpeak's placeholder is what the input holds
        // Empty when it last lost the focus (or not seen losing it): only then can what a focus-in takes
        // out be TeamSpeak's placeholder (not text the user deletes right after it, e.g. from the menu).
        bool                    emptyOut = true;
        Turn                    turn     = Turn::None;
        QString                 before;      // what the input held when the focus change came
        QColor                  beforeColor;
        QElapsedTimer           turnClock;   // since the focus change (monotonic)
        QString                 pending;     // text waiting for the focus
        bool                    waiting   = false; // ... on the timer
        bool                    activated = false; // the window was asked to come to the front for it
        bool                    refocused = false; // the click's focus-in was given for it (refocus())
        bool                    settling  = false; // see giveFocus()
        QString                 settle;
        int                     tries      = 0;
        int                     focusWaits = 0;
    };

    Watch*       watchFor(const QObject* input) const;
    void         prune();
    void         changed(Watch& w);
    void         learn(Watch& w, const QString& text, const QColor& color);
    QString      placeholderOf(const Watch& w) const;
    QColor       colorOf(const Watch& w) const;
    bool         shown(const Watch& w) const;     // TeamSpeak's placeholder is in the input now
    bool         shownAs(const Watch& w, bool unfocused) const;
    bool         decidable(const Watch& w) const; // the text can go in without the focus
    bool         unsettled(const Watch& w) const;
    void         giveFocus(Watch& w);
    void         refocus(Watch& w);
    void         attempt(Watch& w);
    void         put(Watch& w);
    void         tick();

    std::vector<std::unique_ptr<Watch>> m_watches;
    QString                             m_placeholder; // the one learned last (any input)
    QColor                              m_color;
    QTimer*                             m_timer     = nullptr;
    bool                                m_inserting = false;
};
