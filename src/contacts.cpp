#include "contacts.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

#include "delayload.h"
#include "i18n.h"

namespace {

// TeamSpeak's own list is a few hundred contacts at most; rows beyond this many are not read (they count as no contact),
// so a broken or hostile file can't make the read long.
constexpr int kMaxRows    = 5000;
constexpr int kWatchMs    = 2000;
constexpr int kRetryMs[] = {2000, 5000, 10000, 30000, 60000};

} // namespace

namespace contacts {

Lists parse(const QStringList& values)
{
    // A unique identifier: 20 bytes in standard base64 (28 characters, the last "="), not part of a longer base64 run.
    const QRegularExpression uidPattern(QString::fromLatin1("(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{27}="));
    const QString            friendKey = QString::fromLatin1("Friend=");
    const QString            idsKey    = QString::fromLatin1("IDS=");
    Lists                    out;
    for (const QString& value : values) {
        int         friendValue = -1; // no Friend line: not a mark of ours
        QStringList uids;
        for (const QString& line : value.split(QLatin1Char('\n'))) {
            if (line.startsWith(friendKey)) {
                bool      ok = false;
                const int v  = line.mid(friendKey.size()).trimmed().toInt(&ok);
                friendValue  = ok ? v : 2;
            } else if (line.startsWith(idsKey)) {
                QRegularExpressionMatchIterator it = uidPattern.globalMatch(line, idsKey.size());
                while (it.hasNext())
                    uids << it.next().captured(0);
            }
            // Every other line (the nickname, the last server, ...) is never looked at.
        }
        if (friendValue == 0) {
            for (const QString& uid : qAsConst(uids))
                out.friends.insert(uid);
        } else if (friendValue == 1) {
            for (const QString& uid : qAsConst(uids))
                out.blocked.insert(uid);
        }
    }
    out.friends.subtract(out.blocked); // Blocked in one row wins over Friend in another
    return out;
}

QString stateWord(State state)
{
    switch (state) {
    case State::Idle:
        return i18n::t("not read");
    case State::Checking:
        return i18n::t("checking");
    case State::Ok:
        return i18n::t("read");
    case State::Stale:
        return i18n::t("read, last re-read failed");
    case State::Unreadable:
        return i18n::t("unreadable");
    case State::Unavailable:
        return i18n::t("unavailable");
    }
    return i18n::t("not read");
}

bool allows(State state, Mark mark, bool onlyFriends, bool uidKnown)
{
    if (!uidKnown)
        return !onlyFriends; // a header that can't be read, a ServerQuery login: never a friend
    switch (state) {
    case State::Checking:
        return false; // waits for the first read (a moment)
    case State::Idle:
    case State::Unreadable:
    case State::Unavailable:
        return !onlyFriends; // Blocked can't be known: as 2.6 without Only friends, nobody with it
    case State::Ok:
    case State::Stale:
        return mark != Mark::Blocked && (!onlyFriends || mark == Mark::Friend);
    }
    return !onlyFriends;
}

Hold holdOf(State state, Mark mark, bool onlyFriends, bool uidKnown)
{
    if (allows(state, mark, onlyFriends, uidKnown))
        return Hold::None;
    if (!uidKnown)
        return Hold::NotFriend;
    switch (state) {
    case State::Checking:
        return Hold::Checking;
    case State::Idle:
    case State::Unreadable:
    case State::Unavailable:
        return Hold::Unreadable;
    case State::Ok:
    case State::Stale:
        break;
    }
    return mark == Mark::Blocked ? Hold::Blocked : Hold::NotFriend;
}

} // namespace contacts

ContactList::ContactList(const QString& dbPath, QObject* parent)
    : QObject(parent)
    , m_path(dbPath)
{
    m_readSoon = new QTimer(this);
    m_readSoon->setSingleShot(true);
    m_readSoon->setInterval(0);
    connect(m_readSoon, &QTimer::timeout, this, [this] {
        if (m_everOk)
            checkNow(); // active again: a change made meanwhile
        else
            readNow();
    });
    m_watch = new QTimer(this);
    m_watch->setInterval(kWatchMs);
    connect(m_watch, &QTimer::timeout, this, &ContactList::checkNow);
}

ContactList::~ContactList()
{
    shutdown();
}

QString ContactList::connectionName()
{
    return QString::fromLatin1("tsmedia-contacts");
}

qint64 ContactList::now() const
{
    if (m_clock)
        return m_clock();
    static QElapsedTimer clock; // a monotonic clock for the retry schedule
    if (!clock.isValid())
        clock.start();
    return clock.elapsed();
}

contacts::Mark ContactList::markOf(const QString& uid) const
{
    if (uid.isEmpty())
        return contacts::Mark::None;
    if (m_lists.blocked.contains(uid))
        return contacts::Mark::Blocked;
    if (m_lists.friends.contains(uid))
        return contacts::Mark::Friend;
    return contacts::Mark::None;
}

void ContactList::setActive(bool on)
{
    if (m_down)
        return;
    if (on == m_active)
        return;
    m_active = on;
    if (m_state == contacts::State::Unavailable)
        return; // nothing to read in this session
    if (on) {
        if (!m_everOk)
            m_state = contacts::State::Checking; // candidates wait for the first read (a moment)
        m_readSoon->start();
        m_watch->start();
        return;
    }
    m_readSoon->stop();
    m_watch->stop();
    if (m_state == contacts::State::Checking)
        m_state = m_failures > 0 ? contacts::State::Unreadable : contacts::State::Idle;
}

void ContactList::shutdown()
{
    m_down   = true;
    m_active = false;
    if (m_readSoon)
        m_readSoon->stop();
    if (m_watch)
        m_watch->stop();
}

ContactList::Stat ContactList::statNow() const
{
    Stat            st;
    const QFileInfo info(m_path); // a new one each time: never a cached stat
    st.exists = !m_path.isEmpty() && info.exists();
    if (st.exists) {
        st.size     = info.size();
        st.modified = info.lastModified();
    }
    return st;
}

void ContactList::checkNow()
{
    if (m_down || !m_active)
        return;
    switch (m_state) {
    case contacts::State::Idle:
    case contacts::State::Checking:
        readNow();
        return;
    case contacts::State::Unavailable:
        return;
    case contacts::State::Unreadable:
    case contacts::State::Stale:
        if (now() >= m_retryAt)
            readNow();
        return;
    case contacts::State::Ok:
        if (!(statNow() == m_readStat))
            readNow(); // TeamSpeak saved its settings (a contact may have changed)
        return;
    }
}

void ContactList::readNow()
{
    if (m_down)
        return;
    m_readSoon->stop();
    ++m_reads;
    QElapsedTimer took;
    took.start();
    if (!delayload::bindQtSql()) {
        // This TeamSpeak has no Qt5Sql (or not the one we were built for): no QtSql call in this session.
        const bool first = m_state == contacts::State::Checking;
        m_state          = contacts::State::Unavailable;
        m_watch->stop();
        m_lastReadMs = took.elapsed();
        if (first)
            emit changed();
        return;
    }
    const Stat      st = statNow();
    contacts::Lists lists;
    bool            ok = false;
    if (m_path.isEmpty())
        ok = false; // TeamSpeak's folder is unknown
    else if (!st.exists)
        ok = true; // no settings.db yet: no contacts
    else
        ok = readOnce(&lists);
    m_lastReadMs = took.elapsed();
    afterRead(ok, lists, st);
}

bool ContactList::readOnce(contacts::Lists* out)
{
    const QString name = connectionName();
    bool          ok   = false;
    QStringList   values;
    // Every QtSql call names the connection (no default argument: those read QSqlDatabase's default connection name, a
    // data export a delay-loaded Qt5Sql can't give; the link would fail with LNK1194).
    if (QSqlDatabase::contains(name))
        QSqlDatabase::removeDatabase(name); // left over from a read that crashed
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QString::fromLatin1("QSQLITE"), name);
        db.setDatabaseName(m_path);
        // Read-only (never a write, never a journal of ours); a writer TeamSpeak is in the middle of makes the read fail
        // after 100 ms instead of waiting (TeamSpeak's own saves are never held up longer than one SELECT).
        db.setConnectOptions(QString::fromLatin1("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100"));
        if (db.isValid() && db.open()) {
            {
                QSqlQuery q(db);
                q.setForwardOnly(true);
                // The table is missing while there are no contacts (TeamSpeak drops it with the last one).
                if (q.exec(QString::fromLatin1("SELECT name FROM sqlite_master WHERE type='table' AND name='Contacts'"))) {
                    const bool table  = q.next();
                    const bool listed = q.lastError().type() == QSqlError::NoError;
                    q.finish();
                    if (listed && !table) {
                        ok = true;
                    } else if (listed && q.exec(QString::fromLatin1("SELECT value FROM Contacts"))) {
                        while (values.size() < kMaxRows && q.next())
                            values << q.value(0).toString();
                        ok = q.lastError().type() == QSqlError::NoError;
                        q.finish();
                    }
                }
            } // the query goes before the connection closes
            db.close();
        }
    } // and the handle before the connection is removed
    QSqlDatabase::removeDatabase(name);
    if (ok)
        *out = contacts::parse(values);
    return ok;
}

void ContactList::afterRead(bool ok, const contacts::Lists& lists, const Stat& stat)
{
    const contacts::State before = m_state;
    if (ok) {
        const bool differ = !m_everOk || lists != m_lists;
        m_lists           = lists;
        m_everOk          = true;
        m_failures        = 0;
        m_retryAt         = 0;
        m_readStat        = stat;
        m_state           = contacts::State::Ok;
        if (differ || before != contacts::State::Ok)
            emit changed();
        return;
    }
    // Failed: a good list stays (Stale); tried again after 2, 5, 10, 30, then every 60 s (by the 2 s watch).
    m_retryAt = now() + kRetryMs[qMin(m_failures, static_cast<int>(sizeof(kRetryMs) / sizeof(kRetryMs[0])) - 1)];
    ++m_failures;
    m_state = m_everOk ? contacts::State::Stale : contacts::State::Unreadable;
    if (m_active && !m_watch->isActive())
        m_watch->start();
    if (before == contacts::State::Checking)
        emit changed(); // the first read ended: the waiting candidates are decided again (a Stale list decides as before)
}
