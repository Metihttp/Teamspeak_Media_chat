#pragma once

// 2.7 F2: TeamSpeak's contacts (Tools > Contacts, the client menu's Add as Friend / Add as Blocked) for automatic link
// previews: with "Only from friends" a link loads from its website by itself only for a sender marked Friend; a sender
// marked Blocked never gets an automatic load (design_features 2, design 6.2).
//
// The plugin SDK has no contacts API. TeamSpeak keeps them in <config>/settings.db, table Contacts (key, value): value is
// "\n"-separated Name=Value lines; Friend=0 is Friend, 1 Blocked, anything else Neutral; IDS holds the contact's unique
// identifier(s) (ts_facts 3.2). The table is missing while there are no contacts.
//
// The read (ts_facts 3.5: a read that lingers makes TeamSpeak lose settings): QtSql, its own connection
// ("tsmedia-contacts"), QSQLITE_OPEN_READONLY and a 100 ms busy timeout; open, two autocommit SELECTs, every query
// finished, closed and removed again; no BEGIN, no PRAGMA, no write, nothing open afterwards. On the GUI thread (about
// 1 ms). Only the Friend mark and the unique IDs are read and kept, in memory; nothing of it is ever logged, shown or
// written anywhere.
//
// When: the first read when automatic loads are switched on (setActive, an owned 0 ms timer, never inside markOf); then
// only when the file's time or size changed (a stat every 2 s, only while active). A failed read is tried again after 2,
// 5, 10, 30, then every 60 s; a failed re-read after a good one keeps the last good lists (Stale). Qt5Sql is bound
// through delayload::bindQtSql() before the first QtSql call: false = Unavailable for the session.
// CRASH RULE: owned timers (children), stopped in shutdown(); connection name and options built at run time.

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace contacts {

enum class Mark { None, Friend, Blocked, Neutral };

struct Lists {
    QSet<QString> friends;
    QSet<QString> blocked;
    bool operator==(const Lists& o) const { return friends == o.friends && blocked == o.blocked; }
    bool operator!=(const Lists& o) const { return !(*this == o); }
};

// The Contacts table's values: only the Friend= and IDS= lines are looked at; in IDS every [A-Za-z0-9+/]{27}= is a
// unique identifier (any separator). A UID that is Blocked in one row and Friend in another counts as Blocked. Pure.
Lists parse(const QStringList& values);

enum class State {
    Idle,        // not read yet (automatic loads off)
    Checking,    // the first read is due: automatic candidates wait for it
    Ok,          // read
    Stale,       // read once; the last re-read failed (the lists of the good read are kept)
    Unreadable,  // never read successfully (tried again later)
    Unavailable, // Qt5Sql can't be used in this TeamSpeak (the whole session)
};
// The diagnostic info's words: "not read", "checking", "read", "read, last re-read failed", "unreadable", "unavailable".
QString stateWord(State state);

// What the gate of automatic loads decides for a sender (design 6.2, ChatLinkPreviews::contactAllows): an unknown sender
// (uid empty) only without Only friends; while Checking nobody (they wait for the read); never read (Unreadable,
// Unavailable) or not active (Idle): as 2.6 without Only friends, nobody with it; read (Ok, Stale): never a Blocked
// one, and only a Friend with Only friends.
bool allows(State state, Mark mark, bool onlyFriends, bool uidKnown);

// Why a held link is held, for its card (linkcard::AutoHold's values): 0 none, 1 not a friend, 2 blocked, 3 the list
// couldn't be read (Only friends on), 4 checking.
enum class Hold { None, NotFriend, Blocked, Unreadable, Checking };
Hold holdOf(State state, Mark mark, bool onlyFriends, bool uidKnown);

} // namespace contacts

class ContactList : public QObject
{
    Q_OBJECT

  public:
    // dbPath: TeamSpeak's settings.db (empty: unknown, never readable).
    explicit ContactList(const QString& dbPath, QObject* parent = nullptr);
    ~ContactList() override;

    contacts::Mark  markOf(const QString& uid) const; // from memory only (None until a read succeeded)
    contacts::State state() const { return m_state; }
    // Automatic loads on: the first read is due (Checking) unless a read succeeded already; the stat watch runs. Off: the
    // watch stops (the lists stay).
    void            setActive(bool on);
    bool            active() const { return m_active; }
    // Unload: every timer stopped; nothing reads any more.
    void            shutdown();

    // ---- tests ----
    void   setClock(std::function<qint64()> clock) { m_clock = std::move(clock); } // ms (the retry schedule)
    void   checkNow();      // the 2 s watch's tick now: a changed file or a due retry reads
    void   readNow();       // one read now
    int    reads() const { return m_reads; }
    qint64 lastReadMs() const { return m_lastReadMs; } // how long the last read took
    qint64 nextRetryAt() const { return m_retryAt; }
    static QString connectionName(); // "tsmedia-contacts" (built at run time)

  signals:
    // The lists changed, or the first read ended (well or not): the automatic decisions are made again.
    void changed();

  private:
    struct Stat {
        bool      exists = false;
        qint64    size   = -1;
        QDateTime modified;
        bool operator==(const Stat& o) const { return exists == o.exists && size == o.size && modified == o.modified; }
    };
    Stat statNow() const;
    bool readOnce(contacts::Lists* out); // false: the database couldn't be read
    void afterRead(bool ok, const contacts::Lists& lists, const Stat& stat);
    qint64 now() const;

    QString                  m_path;
    contacts::Lists          m_lists;
    contacts::State          m_state  = contacts::State::Idle;
    bool                     m_active = false;
    bool                     m_down   = false;
    bool                     m_everOk = false;
    Stat                     m_readStat; // the file as the last good read found it
    int                      m_failures = 0;
    qint64                   m_retryAt  = 0;
    int                      m_reads    = 0;
    qint64                   m_lastReadMs = 0;
    QTimer*                  m_readSoon = nullptr; // child, single shot, 0 ms: the first read after setActive(true)
    QTimer*                  m_watch    = nullptr; // child, 2 s: the stat watch and the retries, only while active
    std::function<qint64()>  m_clock;
};
