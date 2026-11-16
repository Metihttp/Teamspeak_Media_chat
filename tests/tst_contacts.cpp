// 2.7 F2: TeamSpeak's contacts (contacts.h) and the Qt5Sql delay-load guard (delayload.h). Synthesized settings.db
// files only (made here with QtSql, shaped like TeamSpeak's table, synthetic unique IDs): the parser (Friend, Blocked,
// Neutral, IDS with any separator, junk, Blocked wins, other fields never read), the read (a table, none, no file, a file
// that isn't a database, a folder with non-ASCII characters), the danger rules of ts_facts 3.5 (nothing open after a
// read, no journal of ours, the bytes unchanged, another connection can lock at once), a locked database (another
// connection holds BEGIN EXCLUSIVE: the read fails within the busy timeout, a good list is kept as Stale, retried after
// 2 s on the fake clock), the change watch, and the first read at activation (Checking, then changed()).

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <windows.h>

#include "contacts.h"
#include "delayload.h"
#include "testmain.h"

namespace {

// A synthetic unique ID: 20 bytes in base64 (28 characters, "=" last), like TeamSpeak's.
QString uid(const char* seed)
{
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray(seed), QCryptographicHash::Sha1).toBase64());
}

QString row(const QString& nick, int friendValue, const QString& ids)
{
    return QStringLiteral("Nickname=%1\nFriend=%2\nAutomute=false\nIgnorePublicMessages=false\nIgnorePrivateMessages=false\nIgnorePokes=false\n"
                          "IgnoreAvatar=false\nIgnoreAwayMessage=false\nNickShowType=0\nHaveVolumeModifier=false\nVolumeModifier=0\n"
                          "WhisperAllow=true\nPhoneticNickname=\nLastSeen=2026-11-15T12:00:00\nLastSeenServerName=Test\n"
                          "LastSeenServerAddress=127.0.0.1\nIDS=%3")
        .arg(nick)
        .arg(friendValue)
        .arg(ids);
}

// The test's own connection (never the reader's name).
class Db
{
  public:
    explicit Db(const QString& path, const QString& name = QStringLiteral("tst-contacts-writer"))
        : m_name(name)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        db.open();
    }
    ~Db()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(m_name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_name);
    }
    bool exec(const QString& sql)
    {
        QSqlQuery q(QSqlDatabase::database(m_name, false));
        const bool ok = q.exec(sql);
        q.finish();
        return ok;
    }
    bool put(int key, const QString& value)
    {
        QSqlQuery q(QSqlDatabase::database(m_name, false));
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO Contacts (timestamp, key, value) VALUES (1700000000, ?, ?)"));
        q.addBindValue(QString::number(key));
        q.addBindValue(value);
        const bool ok = q.exec();
        q.finish();
        return ok;
    }

  private:
    QString m_name;
};

void makeTable(const QString& path)
{
    Db db(path);
    QVERIFY(db.exec(QStringLiteral("CREATE TABLE Contacts (timestamp INTEGER UNSIGNED NOT NULL, key VARCHAR NOT NULL UNIQUE, value VARCHAR)")));
    QVERIFY(db.exec(QStringLiteral("CREATE TABLE Misc (timestamp INTEGER UNSIGNED NOT NULL, key VARCHAR NOT NULL UNIQUE, value VARCHAR)")));
}

QByteArray bytesOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void ensureQtSql()
{
    // TeamSpeak has Qt5Sql loaded long before a plugin; the test process does it here the same way.
    LoadLibraryW(L"Qt5Sql.dll");
}

} // namespace

class TestContacts : public QObject
{
    Q_OBJECT

    const QString friendA = uid("friend-a");
    const QString friendB = uid("friend-b");
    const QString blocked = uid("blocked-c");
    const QString neutral = uid("neutral-d");

  private slots:
    void initTestCase() { ensureQtSql(); }

    void parseRows()
    {
        QCOMPARE(friendA.size(), 28);
        QVERIFY(friendA.endsWith(QLatin1Char('=')));
        const QString nickLikeUid = uid("looks-like-a-uid");
        const contacts::Lists l = contacts::parse({
            row(QStringLiteral("Alice"), 0, friendA),
            row(nickLikeUid, 1, blocked),                                            // the nickname is never read
            row(QStringLiteral("Dana"), 2, neutral),                                 // Neutral: nothing
            row(QStringLiteral("Two"), 0, friendB + QStringLiteral(",") + uid("x1") + QStringLiteral("_") + uid("x2") + QStringLiteral(";") + uid("x3")
                                              + QStringLiteral(" ") + uid("x4")), // any separator
            row(QStringLiteral("Junk"), 0, QStringLiteral("not-a-uid ===  AAAA=")),  // junk IDS: nobody
            row(QStringLiteral("Long"), 0, QStringLiteral("QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFB=")), // not 20 bytes: nobody
            QStringLiteral("Nickname=NoIds\nFriend=0"),                               // no IDS
            QStringLiteral("Nickname=NoMark\nIDS=") + uid("no-mark"),                // no Friend line: not ours
            row(QStringLiteral("Both"), 0, blocked),                                 // Blocked in another row wins
        });
        QVERIFY(l.friends.contains(friendA));
        QVERIFY(l.friends.contains(friendB));
        for (const char* seed : {"x1", "x2", "x3", "x4"})
            QVERIFY2(l.friends.contains(uid(seed)), seed);
        QCOMPARE(l.friends.size(), 6);
        QCOMPARE(l.blocked, QSet<QString>{blocked});
        QVERIFY(!l.friends.contains(blocked));
        QVERIFY(!l.friends.contains(nickLikeUid) && !l.blocked.contains(nickLikeUid));
        QVERIFY(!l.friends.contains(neutral) && !l.blocked.contains(neutral));
        QVERIFY(!l.friends.contains(uid("no-mark")));
        QVERIFY(contacts::parse({}).friends.isEmpty());
    }

    void stateWords()
    {
        QCOMPARE(contacts::stateWord(contacts::State::Idle), QStringLiteral("not read"));
        QCOMPARE(contacts::stateWord(contacts::State::Checking), QStringLiteral("checking"));
        QCOMPARE(contacts::stateWord(contacts::State::Ok), QStringLiteral("read"));
        QCOMPARE(contacts::stateWord(contacts::State::Stale), QStringLiteral("read, last re-read failed"));
        QCOMPARE(contacts::stateWord(contacts::State::Unreadable), QStringLiteral("unreadable"));
        QCOMPARE(contacts::stateWord(contacts::State::Unavailable), QStringLiteral("unavailable"));
        QCOMPARE(ContactList::connectionName(), QStringLiteral("tsmedia-contacts"));
    }

    // H4: the guard binds this process's Qt5Sql (the test is linked with the same delay-load); a module that isn't in the
    // process is refused without an exception, and never loaded.
    void delayLoadGuard()
    {
        QVERIFY(GetModuleHandleW(L"Qt5Sql.dll") != nullptr);
        QVERIFY(delayload::bindQtSql());
        QVERIFY(delayload::bindQtSql()); // the kept answer
        QVERIFY(!delayload::bindLoaded(L"tsmedia-not-loaded-7f3a.dll", "tsmedia-not-loaded-7f3a.dll"));
        QVERIFY(GetModuleHandleW(L"tsmedia-not-loaded-7f3a.dll") == nullptr);
        QVERIFY(!delayload::bindLoaded(nullptr, "Qt5Sql.dll"));
        // A module that is loaded but has no delay-load imports here: no exception either.
        QVERIFY(!delayload::bindLoaded(L"kernel32.dll", "tsmedia-not-loaded-7f3a.dll"));
    }

    // L1: a folder with non-ASCII characters; the table, read, with nothing left open, no journal, the bytes unchanged.
    void readsAndLeavesNothingOpen()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString folder = tmp.filePath(QString::fromUtf8("Kontakte \xC3\xBC\xC3\x9F \xE8\x81\x94\xE7\xB3\xBB"));
        QVERIFY(QDir().mkpath(folder));
        const QString path = folder + QStringLiteral("/settings.db");
        makeTable(path);
        {
            Db db(path);
            QVERIFY(db.put(0, row(QStringLiteral("Alice"), 0, friendA)));
            QVERIFY(db.put(1, row(QStringLiteral("Carl"), 1, blocked)));
            QVERIFY(db.put(2, row(QStringLiteral("Dana"), 2, neutral)));
        }
        const QByteArray before = bytesOf(path);
        ContactList      list(path);
        QSignalSpy       changed(&list, &ContactList::changed);
        list.readNow();
        QCOMPARE(list.state(), contacts::State::Ok);
        QCOMPARE(list.markOf(friendA), contacts::Mark::Friend);
        QCOMPARE(list.markOf(blocked), contacts::Mark::Blocked);
        QCOMPARE(list.markOf(neutral), contacts::Mark::None);
        QCOMPARE(list.markOf(QString()), contacts::Mark::None);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!QSqlDatabase::connectionNames().contains(ContactList::connectionName()));
        QVERIFY(!QFile::exists(path + QStringLiteral("-journal")));
        QCOMPARE(bytesOf(path), before);
        QVERIFY2(list.lastReadMs() < 1000, qPrintable(QString::number(list.lastReadMs())));
        {
            Db locker(path, QStringLiteral("tst-contacts-locker"));
            QVERIFY(locker.exec(QStringLiteral("BEGIN EXCLUSIVE"))); // nothing of ours holds a lock
            QVERIFY(locker.exec(QStringLiteral("ROLLBACK")));
        }
        list.readNow(); // the same lists: no news
        QCOMPARE(changed.count(), 1);
    }

    void emptyMissingAndBroken()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            // No Contacts table (TeamSpeak drops it with the last contact): nobody, read.
            const QString path = tmp.filePath(QStringLiteral("notable.db"));
            {
                Db db(path);
                QVERIFY(db.exec(QStringLiteral("CREATE TABLE Misc (timestamp INTEGER, key VARCHAR, value VARCHAR)")));
            }
            ContactList list(path);
            list.readNow();
            QCOMPARE(list.state(), contacts::State::Ok);
            QCOMPARE(list.markOf(friendA), contacts::Mark::None);
        }
        {
            // No settings.db at all: no contacts, read (and no file was made).
            const QString path = tmp.filePath(QStringLiteral("missing.db"));
            ContactList   list(path);
            list.readNow();
            QCOMPARE(list.state(), contacts::State::Ok);
            QVERIFY(!QFile::exists(path));
        }
        {
            // A file that isn't a database: unreadable (Only from friends then holds everyone, without it 2.6's loads).
            const QString path = tmp.filePath(QStringLiteral("broken.db"));
            QFile         f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(4096, 'x'));
            f.close();
            ContactList list(path);
            list.readNow();
            QCOMPARE(list.state(), contacts::State::Unreadable);
            QVERIFY(!QSqlDatabase::connectionNames().contains(ContactList::connectionName()));
            QVERIFY(!contacts::allows(list.state(), list.markOf(friendA), true, true));
            QVERIFY(contacts::allows(list.state(), list.markOf(friendA), false, true));
        }
        {
            ContactList list{QString()}; // TeamSpeak's folder unknown
            list.readNow();
            QCOMPARE(list.state(), contacts::State::Unreadable);
        }
    }

    // H5 / ts_facts 3.5: TeamSpeak in the middle of a write (another connection holds BEGIN EXCLUSIVE): the read gives up
    // after the 100 ms busy timeout; the good list stays (Stale); retried after 2 s (fake clock) and read again.
    void lockedKeepsTheGoodList()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("settings.db"));
        makeTable(path);
        {
            Db db(path);
            QVERIFY(db.put(0, row(QStringLiteral("Alice"), 0, friendA)));
            QVERIFY(db.put(1, row(QStringLiteral("Carl"), 1, blocked)));
        }
        qint64      now = 1000;
        ContactList list(path);
        list.setClock([&now] { return now; });
        list.setActive(true);
        QCOMPARE(list.state(), contacts::State::Checking);
        QTRY_COMPARE(list.state(), contacts::State::Ok); // the first read, a turn later
        QCOMPARE(list.markOf(friendA), contacts::Mark::Friend);
        {
            Db locker(path, QStringLiteral("tst-contacts-locker"));
            QVERIFY(locker.exec(QStringLiteral("BEGIN EXCLUSIVE")));
            QElapsedTimer took;
            took.start();
            list.readNow();
            QVERIFY2(took.elapsed() < 1500, qPrintable(QString::number(took.elapsed())));
            QCOMPARE(list.state(), contacts::State::Stale);
            QCOMPARE(list.markOf(friendA), contacts::Mark::Friend); // kept
            QCOMPARE(list.markOf(blocked), contacts::Mark::Blocked);
            QCOMPARE(list.nextRetryAt(), now + 2000);
            QVERIFY(!QSqlDatabase::connectionNames().contains(ContactList::connectionName()));
            const int reads = list.reads();
            now += 1500;
            list.checkNow(); // not yet
            QCOMPARE(list.reads(), reads);
            QVERIFY(locker.exec(QStringLiteral("ROLLBACK")));
        }
        now += 600;
        const int reads = list.reads();
        list.checkNow(); // 2.1 s after the failure: read again
        QCOMPARE(list.reads(), reads + 1);
        QCOMPARE(list.state(), contacts::State::Ok);
        list.shutdown();
    }

    // Never read successfully: Unreadable, tried again after 2, 5, 10, 30, then every 60 s.
    void retrySchedule()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("settings.db"));
        makeTable(path);
        qint64      now = 0;
        ContactList list(path);
        list.setClock([&now] { return now; });
        Db locker(path, QStringLiteral("tst-contacts-locker"));
        QVERIFY(locker.exec(QStringLiteral("BEGIN EXCLUSIVE")));
        list.setActive(true);
        QTRY_COMPARE(list.state(), contacts::State::Unreadable);
        for (const int gap : {2000, 5000, 10000, 30000, 60000, 60000}) {
            QCOMPARE(list.nextRetryAt(), now + gap);
            now += gap - 1;
            const int reads = list.reads();
            list.checkNow();
            QCOMPARE(list.reads(), reads);
            now += 1;
            list.checkNow();
            QCOMPARE(list.reads(), reads + 1);
        }
        QVERIFY(locker.exec(QStringLiteral("ROLLBACK")));
        list.shutdown();
    }

    // The watch: a changed file (TeamSpeak saved its settings) is read again and says what changed; an unchanged one isn't.
    void readsAgainWhenTheFileChanges()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("settings.db"));
        makeTable(path);
        {
            Db db(path);
            QVERIFY(db.put(0, row(QStringLiteral("Alice"), 0, friendA)));
        }
        ContactList list(path);
        QSignalSpy  changed(&list, &ContactList::changed);
        list.setActive(true);
        QTRY_COMPARE(list.state(), contacts::State::Ok);
        QCOMPARE(changed.count(), 1); // the first read ended
        const int reads = list.reads();
        list.checkNow();
        QCOMPARE(list.reads(), reads); // nothing changed: no read
        QThread::msleep(30);
        {
            Db db(path);
            QVERIFY(db.put(1, row(QStringLiteral("Bob"), 0, friendB))); // Add as Friend
        }
        list.checkNow();
        QCOMPARE(list.reads(), reads + 1);
        QCOMPARE(list.markOf(friendB), contacts::Mark::Friend);
        QCOMPARE(changed.count(), 2);
        QThread::msleep(30);
        {
            Db db(path);
            QVERIFY(db.exec(QStringLiteral("DELETE FROM Contacts WHERE key='0'"))); // Remove from Contacts
        }
        list.checkNow();
        QCOMPARE(list.markOf(friendA), contacts::Mark::None);
        QCOMPARE(changed.count(), 3);
        // Off: no watch, no read.
        list.setActive(false);
        QVERIFY(!list.active());
        const int off = list.reads();
        list.checkNow();
        QCOMPARE(list.reads(), off);
        list.shutdown();
        list.setActive(true); // after shutdown nothing starts again
        QVERIFY(!list.active());
    }
};

TSMEDIA_REGISTER_TEST(TestContacts)

#include "tst_contacts.moc"
