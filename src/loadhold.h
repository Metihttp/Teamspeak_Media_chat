#pragma once

// 2.7 (R-DEF-1): keeps one chat's reader where they were while the plugin styles a chat it has just attached (the load
// after Options > Addons > Reload or after turning TS Media on, a new chat tab): the end stays the end, a reader above
// keeps their message or line (chatscroll::ReaderPlace). After any edit or range change it puts the place back once
// per event-loop turn (an owned 0 ms timer: never inside Qt's layout or another module's pass). It lets go at the
// reader's first scroll, click, key, wheel or touch, once the chat is settled (busy() false and nothing changed for
// quietMs), or after capMs. The modules' own place keeping is unchanged; the hold only overrides it during the load.
// CRASH RULE: its timers are children, its connections take this as the context, its filters come off in finish();
// no functor singleShot, no metatype, no static string data of this DLL left in Qt's state.

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <functional>

#include "chatscroll.h"

class QEvent;
class QTextBrowser;
class QTextDocument;
class QTimer;

class LoadHold : public QObject
{
  public:
    enum class End { None, User, Settled, Cap, Swapped };

    LoadHold(QTextBrowser* browser, const chatscroll::ReaderPlace& place, std::function<bool()> busy, QObject* parent,
             int quietMs = 1500, int capMs = 30000);
    ~LoadHold() override; // finish(None) if still active

    bool active() const { return !m_finished; }
    End  ended() const { return m_end; }
    // What the unload keeps if the hold is still active then (the place as the load is keeping it).
    const chatscroll::ReaderPlace& place() const { return m_place; }
    int    corrections() const { return m_corrections; }
    qint64 ageMs() const { return m_age.elapsed(); }
    // Idempotent: timers stopped, connections and filters off. why: what ended it (None: the hold went with its owner).
    void finish(End why);

    // Called once when the hold ends (ChatIntegration's count and its test log line); optional.
    std::function<void(End)> onFinished;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // observes only, returns false

  private:
    void changed(); // an edit or a new range: the place back on the next turn; the chat is not quiet yet
    void apply();
    void quiet();

    QPointer<QTextBrowser>  m_browser;
    QPointer<QTextDocument> m_doc;
    QPointer<QObject>       m_viewport;
    chatscroll::ReaderPlace m_place;
    std::function<bool()>   m_busy;
    QTimer*                 m_apply = nullptr; // children, single shot: 0 ms, quietMs, capMs
    QTimer*                 m_quiet = nullptr;
    QTimer*                 m_cap   = nullptr;
    QMetaObject::Connection m_range, m_contents, m_action;
    bool                    m_applying    = false;
    bool                    m_finished    = false;
    End                     m_end         = End::None;
    int                     m_corrections = 0;
    QElapsedTimer           m_age;
};
