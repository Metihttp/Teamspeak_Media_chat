#pragma once

// 2.7.1: a chat's place across Qt's own move at its first Show. QTextEdit::showEvent moves the view once in a widget's
// life, the first time it is shown: it makes the widget's text cursor visible (ensureCursorVisible). TeamSpeak keeps
// that cursor at the chat's end (QTextEdit::append moves it along), so a chat filled while it was hidden (a new
// connection's channel chat with its history, a private chat opened by an incoming message, a second server tab's
// chats) shows its end. TS Media's layout can take that from it:
//   - the block that holds the cursor is one the layout hid (a collapsed run of events at the chat's end, an album's
//     later message): Qt finds no place for a hidden block and scrolls to the chat's top;
//   - the cursor's line is one of TS Media's pictures, taller than the view: Qt scrolls to that line's top, short of the
//     chat's end.
// ShowPlace watches each chat's Show with an event filter on the chat itself (after dark mode's application filter has
// put back a place it kept for the Show, before QTextEdit's own showEvent): it reads where the chat belongs (the load
// hold's place while one keeps it; the chat's end when it was at its end before Qt laid it out for its new size; else the
// reader's place as it is now), works out where Qt's move would take the view, and when that is somewhere else watches
// the chat's scroll bar for exactly that move: inside QTextEdit's showEvent it puts the place back at once, before
// anything paints. A Show that isn't the chat's first moves nothing; the watch goes on the next turn (an owned 0 ms timer).
// When that place is the load hold's or the end read at the hidden resize, a scale pass the Show started (the saved zoom
// set before the first paint) takes it again (Hooks::placed), so its settle 150 ms later doesn't put back the place it read
// before ShowPlace ran.
// CRASH RULE: its timer is a child; the filters come off in stop() (and the destructor); no functor singleShot, no
// metatype, no static data of this DLL left in Qt's state.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>

#include "chatscroll.h"

class QEvent;
class QTextBrowser;
class QTimer;

class ShowPlace : public QObject
{
  public:
    struct Hooks {
        // The place a load hold keeps for this chat, or nullptr (R-DEF-1: its place wins while it is active).
        std::function<const chatscroll::ReaderPlace*(QTextBrowser*)> heldPlace;
        // Dark mode keeps a place for this chat's Show (a toggle while it was hidden): it is put back at the Show, before
        // this filter runs, and is what the chat's place is then.
        std::function<bool(QTextBrowser*)> placeWaits;
        // The chat's place at its Show is the load hold's place or the end it had at its hidden resize (also right after the
        // put-back below): a scale pass that a font set at this Show started (ChatZoom's saved zoom for a chat hidden
        // through a zoom change) read its place before ShowPlace had put that place back, and takes it again
        // (ChatScale::retake, as after dark mode's place bracket). A reader's place is left to that pass (it read it before
        // the new font moved the text), also one the new font pushed down to the end.
        std::function<void(QTextBrowser*)> placed;
        // Optional (the live test's log): the place was put back after a Show moved the chat (the value at the Show, the
        // value the Show left, the value now).
        std::function<void(QTextBrowser*, int, int, int)> putBack;
    };

    explicit ShowPlace(Hooks hooks, QObject* parent = nullptr);
    ~ShowPlace() override; // stop()

    void watch(QTextBrowser* browser); // once per chat (a second call does nothing)
    void stop();                       // every filter off, the timer stopped, nothing queued (the plugin's unload)

    // For the tests and the live test's log: Shows armed for Qt's move (it would have taken the view elsewhere), and the
    // moves that came and were undone.
    int armed() const { return m_arms; }
    int fixed() const { return m_fixes; }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // observes only, returns false

  private:
    struct Armed {
        QPointer<QTextBrowser>  browser;
        QMetaObject::Connection watch; // the chat's scroll bar, until Qt's move or the next turn
    };
    void shown(QTextBrowser* browser);
    void disarm(QTextBrowser* browser);
    void disarmAll();
    void settle(); // next turn: what no Show used is forgotten

    Hooks                           m_hooks;
    QVector<QPointer<QTextBrowser>> m_watched;
    QHash<QObject*, bool>           m_endAtResize; // a hidden chat's end before Qt laid it out for its Show (this turn only)
    QVector<Armed>                  m_armed;
    QTimer*                         m_timer   = nullptr; // child, single shot, 0 ms
    bool                            m_stopped = false;
    int                             m_arms    = 0;
    int                             m_fixes   = 0;
};
