#pragma once

// 2.7 F1: formatting in TeamSpeak's chat input (textformat.h does the text). Owned by ChatIntegration (created in start,
// deleted first in its destructor); attach() for every chat input it finds.
//  * The input's right-click menu (by mouse, the Menu key or Shift+F10): with text selected, Bold, Italic, Underline
//    and Spoiler (ticked when applied: a click removes it) as one block after "Select All", a separator on each side;
//    TeamSpeak's own Format submenu gets Spoiler after Underline. The menu is caught while it is built (its Polish,
//    Show as a fallback) by an application-wide filter armed only for one context-menu event and disarmed when caught,
//    after two 0 ms hops of an owned timer (a menu TeamSpeak opens one turn late is still caught), and at shutdown.
//    Our actions are ours (TeamSpeak deletes its menu right after it closes); the separators are the menu's. Key texts
//    after a tab only, never setShortcut (TeamSpeak has QShortcuts for Alt+Shift+B/I/U/C on the input).
//  * Alt+Shift+S: Spoiler on the selection (an empty pair with the cursor inside without one), any keyboard layout.
//    Ctrl+B/I/U stay TeamSpeak's (Bookmarks, Identities, Collected URLs); Alt+Shift+B/I/U stay TeamSpeak's own Format.
//  * One edit per action (one Ctrl+Z), the text inside selected, the focus kept. Nothing while the input shows
//    TeamSpeak's placeholder.

#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>

#include "textformat.h"

class QAction;
class QMenu;
class QTextEdit;
class QTimer;

class InputFormat : public QObject
{
    Q_OBJECT

  public:
    explicit InputFormat(QObject* parent = nullptr);
    ~InputFormat() override; // shutdown()

    void attach(QTextEdit* edit); // a chat input (again: nothing)
    void detach(QTextEdit* edit);
    void shutdown(); // disarmed, the inputs' filters off

    // 2.2.1: true while the input holds TeamSpeak's "Enter Chat Message..." (ChatInputs): nothing is done then.
    std::function<bool(const QTextEdit*)> showsPlaceholder;

    // ---- also for tests and the live-test driver ------------------------------------------------------------
    bool     armed() const { return m_armed; }
    QAction* action(textformat::Format f) const; // the top block's
    QAction* formatSpoilerAction() const { return m_formatSpoiler; }
    // f on edit's selection as the menu and the key do it. False: nothing done (the placeholder, no input).
    bool     run(QTextEdit* edit, textformat::Format f);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    class Catcher;
    friend class Catcher;

    void arm(QTextEdit* edit);
    void disarm();
    void hop();
    void inject(QMenu* menu);
    void triggered(textformat::Format f);

    QList<QPointer<QTextEdit>> m_edits;
    Catcher*                   m_catcher = nullptr;
    bool                       m_armed   = false;
    QTimer*                    m_disarm  = nullptr; // owned, 0 ms, two hops
    int                        m_hops    = 0;
    QPointer<QTextEdit>        m_armedFor;  // the input whose context-menu event armed the catcher
    QPointer<QTextEdit>        m_menuEdit;  // the input of the menu caught last
    int                        m_menuStart = 0; // its selection when the menu was caught
    int                        m_menuEnd   = 0;
    QAction*                   m_top[4]    = {nullptr, nullptr, nullptr, nullptr};
    QAction*                   m_formatSpoiler = nullptr;
};
