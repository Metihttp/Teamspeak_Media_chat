// Unit tests for the 2.2 foundation modules: structured log text (logtext), the plugin's rotating log
// file (pluginlog), SHA-256 of files (hashing) and the per-connection FloodGovernor.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

#include "floodgovernor.h"
#include "hashing.h"
#include "logtext.h"
#include "medialink.h"
#include "pluginlog.h"
#include "testmain.h"

namespace {

QString marked(char kind, const QString& text)
{
    return ts3::kLogMarkStart + QLatin1Char(kind) + text + ts3::kLogMarkEnd;
}

QByteArray pattern(qint64 size)
{
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 8)) & 0xff);
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

} // namespace

class TestFoundation : public QObject
{
    Q_OBJECT

  private slots:
    // structured log
    void logSubstitution();
    void logMarksOnlyPrivateValues();
    void logArgumentsCantFakeMarkers();
    void logOneLine();
    void logUnclassifiedAndQuoted();
    void logRedaction();

    // plugin log file
    void pluginLogWritesAndTails();
    void pluginLogRotates();
    void pluginLogCapsLines();
    void pluginLogLineFormat();

    // hashing
    void sha256FileMatches_data();
    void sha256FileMatches();
    void sha256FileProgress();
    void sha256FileCancelAndErrors();
    void digestTexts();

    // FloodGovernor
    void postsOneAtATime();
    void postFloodPauseAndRetries();
    void commandsWaitForPosts();
    void commandBucket();
    void commandBackoff();
    void floodBurstScenario();
};

// ---- structured log ----------------------------------------------------------------------------

void TestFoundation::logSubstitution()
{
    // One pass: a "%2" inside a value is never expanded again.
    const ts3::LogText text = ts3::formatLog("Uploaded %1 (%2)", {ts3::file(QStringLiteral("a%2b.jpg")), ts3::pub(QStringLiteral("4 MB"))});
    QCOMPARE(text.plain, QStringLiteral("Uploaded a%2b.jpg (4 MB)"));
    QCOMPARE(text.marked, QStringLiteral("Uploaded ") + marked('f', QStringLiteral("a%2b.jpg")) + QStringLiteral(" (4 MB)"));

    // Missing arguments stay as written; "%" without a digit too; numbers through pub().
    QCOMPARE(ts3::formatLog("%1 of %3 at 100%", {ts3::pub(7)}).plain, QStringLiteral("7 of %3 at 100%"));
    QCOMPARE(ts3::formatLog("%1 %2 %3 %4", {ts3::pub(qint64(-5)), ts3::pub(quint64(18446744073709551615ull)), ts3::pub(2.5), ts3::pub(static_cast<unsigned short>(9))}).plain,
             QStringLiteral("-5 18446744073709551615 2.5 9"));
    QCOMPARE(ts3::formatLog("%9", {ts3::pub(1), ts3::pub(2), ts3::pub(3), ts3::pub(4), ts3::pub(5), ts3::pub(6), ts3::pub(7), ts3::pub(8), ts3::pub(9)}).plain, QStringLiteral("9"));
    QCOMPARE(ts3::formatLog(nullptr, {}).plain, QString());
    QCOMPARE(ts3::formatLog("non-ASCII format: “%1”", {ts3::name(QStringLiteral("Mehdi"))}).plain, QStringLiteral("non-ASCII format: “Mehdi”"));
}

void TestFoundation::logMarksOnlyPrivateValues()
{
    const ts3::LogText text = ts3::formatLog("%1 %2 %3 %4", {ts3::pub(QStringLiteral("p")), ts3::file(QStringLiteral("f")), ts3::local(QStringLiteral("C:\\x")), ts3::name(QStringLiteral("n"))});
    QCOMPARE(text.plain, QStringLiteral("p f C:\\x n"));
    QCOMPARE(text.marked, QStringLiteral("p ") + marked('f', QStringLiteral("f")) + QLatin1Char(' ') + marked('l', QStringLiteral("C:\\x")) + QLatin1Char(' ') + marked('n', QStringLiteral("n")));
    QCOMPARE(ts3::redactedLog(text.marked), QStringLiteral("p <file 1> <path 1> <name 1>"));
}

void TestFoundation::logArgumentsCantFakeMarkers()
{
    // A value (or format) with stray markers can't open, close or fake a span.
    const QString evil = ts3::kLogMarkEnd + QStringLiteral("secret") + ts3::kLogMarkStart + QStringLiteral("f");
    const ts3::LogText text = ts3::formatLog("a %1 b", {ts3::file(evil)});
    QCOMPARE(text.plain, QStringLiteral("a secretf b"));
    QCOMPARE(text.marked, QStringLiteral("a ") + marked('f', QStringLiteral("secretf")) + QStringLiteral(" b"));
    QCOMPARE(ts3::formatLog("x\xee\x80\x80y %1", {ts3::pub(evil)}).plain, QStringLiteral("xy secretf"));
    QVERIFY(!ts3::redactedLog(text.marked).contains(QStringLiteral("secret")));
}

void TestFoundation::logOneLine()
{
    const ts3::LogText text = ts3::formatLog("a\r\nb\nc %1", {ts3::name(QStringLiteral("x\ny\u2028z"))});
    QCOMPARE(text.plain, QStringLiteral("a b c x y z"));
    QVERIFY(!text.marked.contains(QLatin1Char('\n')) && !text.marked.contains(QLatin1Char('\r')));
}

void TestFoundation::logUnclassifiedAndQuoted()
{
    const ts3::LogText legacy = ts3::unclassifiedLog(QStringLiteral("Uploaded /tsmedia/holiday_3f9a1c2e.jpg (2.4 MB)"));
    QCOMPARE(legacy.plain, QStringLiteral("Uploaded /tsmedia/holiday_3f9a1c2e.jpg (2.4 MB)"));
    QCOMPARE(ts3::redactedLog(legacy.marked), QStringLiteral("<text 1>"));
    QCOMPARE(ts3::unclassifiedLog(QString()).marked, QString());

    // Chat warnings put names in curly quotes.
    const ts3::LogText warning = ts3::quotedNamesLog(QStringLiteral("Couldn't send “holiday.jpg”: You don't have permission. Also “b”."));
    QCOMPARE(warning.plain, QStringLiteral("Couldn't send “holiday.jpg”: You don't have permission. Also “b”."));
    QCOMPARE(ts3::redactedLog(warning.marked), QStringLiteral("Couldn't send “<name 1>”: You don't have permission. Also “<name 2>”."));
    // No closing quote: the rest of the line counts as the name (fail closed).
    QCOMPARE(ts3::redactedLog(ts3::quotedNamesLog(QStringLiteral("Couldn't find “C:\\Users\\me\\a.jpg. Gone")).marked), QStringLiteral("Couldn't find “<name 1>"));
    QCOMPARE(ts3::redactedLog(ts3::quotedNamesLog(QStringLiteral("No names here.")).marked), QStringLiteral("No names here."));
}

void TestFoundation::logRedaction()
{
    // The same value keeps its number, per kind.
    const QString line = marked('f', QStringLiteral("a.jpg")) + QStringLiteral(" ") + marked('f', QStringLiteral("b.jpg")) + QStringLiteral(" ") + marked('f', QStringLiteral("a.jpg"))
                         + QStringLiteral(" ") + marked('l', QStringLiteral("a.jpg"));
    QCOMPARE(ts3::redactedLog(line), QStringLiteral("<file 1> <file 2> <file 1> <path 1>"));
    // An unbalanced marker hides the rest of the line.
    QCOMPARE(ts3::redactedLog(QStringLiteral("ok ") + ts3::kLogMarkStart + QStringLiteral("fsecret and more")), QStringLiteral("ok <hidden>"));
    QCOMPARE(ts3::redactedLog(QStringLiteral("ok ") + ts3::kLogMarkStart), QStringLiteral("ok <hidden>"));
    // A stray end marker is dropped.
    QCOMPARE(ts3::redactedLog(QStringLiteral("a") + ts3::kLogMarkEnd + QStringLiteral("b")), QStringLiteral("ab"));
}

// ---- plugin log file ---------------------------------------------------------------------------

void TestFoundation::pluginLogWritesAndTails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logs = dir.path() + QStringLiteral("/logs");
    plog::write(2, QStringLiteral("before start")); // dropped
    plog::start(logs);
    QCOMPARE(plog::currentFilePath(), QDir::cleanPath(logs) + QStringLiteral("/tsmedia.log"));
    QVERIFY(plog::tail(5).isEmpty());
    plog::write(4, QStringLiteral("first"));
    plog::write(2, ts3::formatLog("second %1", {ts3::file(QStringLiteral("x.jpg"))}).marked);
    const QStringList lines = plog::tail(5);
    QCOMPARE(lines.size(), 2);
    QVERIFY2(lines.at(0).endsWith(QStringLiteral(" INFO  first")), qPrintable(lines.at(0)));
    QVERIFY(lines.at(1).endsWith(QStringLiteral(" WARN  second ") + marked('f', QStringLiteral("x.jpg"))));
    QCOMPARE(plog::tail(1), QStringList{lines.at(1)});
    QVERIFY(QFile::exists(logs + QStringLiteral("/tsmedia.log")));

    // After shutdown nothing is written (and the file is closed: it can be deleted).
    plog::shutdown();
    plog::write(4, QStringLiteral("after"));
    QVERIFY(plog::currentFilePath().isEmpty());
    QVERIFY(QFile::remove(logs + QStringLiteral("/tsmedia.log")));
    plog::start(logs);
    plog::write(4, QStringLiteral("again"));
    QCOMPARE(plog::tail(5).size(), 1);
    plog::shutdown();
}

void TestFoundation::pluginLogRotates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    plog::start(dir.path());
    const QString line = QString(500, QLatin1Char('x'));
    for (int i = 0; i < 1500; ++i) // about 800 KB of lines
        plog::write(4, QString::number(i) + QLatin1Char(' ') + line);
    const QFileInfo current(dir.path() + QStringLiteral("/tsmedia.log"));
    const QFileInfo backup(dir.path() + QStringLiteral("/tsmedia.1.log"));
    QVERIFY(current.exists() && backup.exists());
    QVERIFY(current.size() <= plog::kMaxFileBytes);
    QVERIFY(backup.size() <= plog::kMaxFileBytes);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2); // one backup only
    // The tail reaches into the backup when the current file is short, newest last, without gaps; the
    // oldest lines are gone.
    const QStringList all = plog::tail(100000);
    QVERIFY2(all.size() >= 400 && all.size() <= 2 * plog::kMaxFileBytes / 500, qPrintable(QString::number(all.size())));
    QVERIFY(all.last().contains(QStringLiteral(" 1499 ")));
    QVERIFY(all.first().contains(QStringLiteral(" %1 ").arg(1500 - all.size())));
    const QStringList few = plog::tail(3);
    QCOMPARE(few.size(), 3);
    QVERIFY(few.at(0).contains(QStringLiteral(" 1497 ")) && few.at(2).contains(QStringLiteral(" 1499 ")));
    plog::shutdown();
}

void TestFoundation::pluginLogCapsLines()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    plog::start(dir.path());
    // A huge name inside a span: cut, the span closed again, never half a surrogate pair (the cut
    // falls on the first half of the emoji: "Big " + the 2 marker characters + 1992 = 1998).
    plog::write(2, ts3::formatLog("Big %1 end", {ts3::file(QString(1992, QLatin1Char('x')) + QString::fromUtf8("😀😀") + QString(100, QLatin1Char('a')))}).marked);
    const QStringList lines = plog::tail(1);
    QCOMPARE(lines.size(), 1);
    const QString text = lines.first().mid(lines.first().indexOf(QStringLiteral("Big")));
    QVERIFY(text.size() <= plog::kMaxLineChars + 1);
    QVERIFY(text.endsWith(QString(ts3::kLogMarkEnd) + QChar(0x2026)));
    QCOMPARE(ts3::redactedLog(text), QStringLiteral("Big <file 1>") + QChar(0x2026));
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate())
            QVERIFY(i + 1 < text.size() && text.at(i + 1).isLowSurrogate());
    }
    QVERIFY(!text.contains(QString::fromUtf8("😀").at(0))); // the emoji went as a whole
    plog::shutdown();
}

void TestFoundation::pluginLogLineFormat()
{
    // 2026-11-02 13:33:40.123 UTC, shown at +03:30.
    const qint64 ms = 1793626420123LL;
    QCOMPARE(plog::formatLine(ms, 3 * 3600 + 1800, 2, QStringLiteral("x")), QStringLiteral("2026-11-02T17:03:40.123+03:30 WARN  x"));
    QCOMPARE(plog::formatLine(ms, 0, 1, QStringLiteral("y")), QStringLiteral("2026-11-02T13:33:40.123Z ERROR y"));
    QCOMPARE(plog::formatLine(ms, 0, 0, QString()).mid(25), QStringLiteral("CRIT  "));
    QCOMPARE(plog::formatLine(ms, 0, 3, QString()).mid(25), QStringLiteral("DEBUG "));
    QCOMPARE(plog::formatLine(ms, 0, 5, QStringLiteral("a\nb")).mid(25), QStringLiteral("DEVEL a b"));
}

// ---- hashing -----------------------------------------------------------------------------------

void TestFoundation::sha256FileMatches_data()
{
    QTest::addColumn<qint64>("size");
    QTest::newRow("empty") << qint64(0);
    QTest::newRow("1 byte") << qint64(1);
    QTest::newRow("1 MiB") << qint64(hashing::kChunkBytes);
    QTest::newRow("1 MiB + 1") << qint64(hashing::kChunkBytes + 1);
    QTest::newRow("5 MiB") << qint64(5 * hashing::kChunkBytes);
}

void TestFoundation::sha256FileMatches()
{
    QFETCH(qint64, size);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString    path = dir.path() + QStringLiteral("/data.bin");
    const QByteArray data = pattern(size);
    QVERIFY(writeFile(path, data));
    QString          error = QStringLiteral("unset");
    const QByteArray hash  = hashing::sha256File(path, nullptr, {}, &error);
    QCOMPARE(hash, QCryptographicHash::hash(data, QCryptographicHash::Sha256));
    QCOMPARE(hash.size(), 32);
    QVERIFY(error.isEmpty());
    QCOMPARE(hashing::sha256(data), hash);
}

void TestFoundation::sha256FileProgress()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/big.bin");
    const qint64  size = 2 * hashing::kProgressStepBytes + 12345;
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(size)); // zeros, written sparsely
    }
    QVector<qint64> reports;
    qint64          reportedTotal = -1;
    const QByteArray hash = hashing::sha256File(path, nullptr, [&](qint64 done, qint64 total) {
        reports.append(done);
        reportedTotal = total;
    });
    QCOMPARE(hash.size(), 32);
    QCOMPARE(reportedTotal, size);
    QCOMPARE(reports.size(), 3); // at 64 MiB, at 128 MiB, at the end
    QCOMPARE(reports.last(), size);
    QVERIFY(std::is_sorted(reports.cbegin(), reports.cend()));
}

void TestFoundation::sha256FileCancelAndErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/data.bin");
    QVERIFY(writeFile(path, pattern(3 * hashing::kChunkBytes)));

    std::atomic<bool> cancel{true};
    QString           error;
    QVERIFY(hashing::sha256File(path, &cancel, {}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("canceled"));

    // Canceled while it runs (from the progress callback, as another thread would).
    std::atomic<bool> later{false};
    int               calls = 0;
    QVERIFY(writeFile(path, pattern(hashing::kProgressStepBytes + 10)));
    QVERIFY(hashing::sha256File(path, &later, [&](qint64, qint64) {
                ++calls;
                later = true;
            }).isEmpty());
    QCOMPARE(calls, 1);

    error.clear();
    QVERIFY(hashing::sha256File(dir.path() + QStringLiteral("/missing.bin"), nullptr, {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(hashing::sha256File(dir.path(), nullptr, {}, &error).isEmpty()); // a folder
}

void TestFoundation::digestTexts()
{
    const QByteArray empty = hashing::sha256(QByteArray());
    QCOMPARE(hashing::toHex(empty), QStringLiteral("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
    QCOMPARE(hashing::toHex(hashing::sha256("abc")), QStringLiteral("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    // What a link carries for it (MediaLink encodes the raw bytes).
    MediaLink link;
    link.serverUid   = QStringLiteral("uid");
    link.channelId   = 1;
    link.fileName    = QStringLiteral("a_3f9a1c2e.png");
    link.protocol    = MediaLink::kProtocol;
    link.sha256      = empty;
    link.previewFile = QStringLiteral("/previews/3f9a1c2e.jpg");
    link.previewSha  = empty.left(16);
    QVERIFY(link.toUrl().endsWith(QStringLiteral("&sha=47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU&ph=47DEQpj8HBSa-_TImW-5JA")));
}

// ---- FloodGovernor -----------------------------------------------------------------------------

void TestFoundation::postsOneAtATime()
{
    FloodGovernor g;
    QVERIFY(g.postReady(0));
    const quint64 first = g.postSent(0);
    QVERIFY(!g.postReady(10));                 // waits for its answer
    QCOMPARE(g.nextPostCheckMs(10), qint64(1000)); // or a second
    QVERIFY(!g.postAnswered(first, 200, false, 1));
    QVERIFY(g.postReady(200)); // answered: the next may go
    const quint64 second = g.postSent(300);
    QVERIFY(second != first);
    QVERIFY(!g.postReady(1299));
    QVERIFY(g.postReady(1300)); // no answer within a second: counts as delivered
    // A late answer for an older post doesn't release a newer one.
    const quint64 third = g.postSent(1300);
    QVERIFY(!g.postAnswered(second, 1310, false, 1));
    QVERIFY(!g.postReady(1320));
    QVERIFY(!g.postAnswered(third, 1330, false, 1));
    QVERIFY(g.postReady(1330));
    QCOMPARE(g.counters().postsSent, 3);

    FloodGovernor::Limits spaced;
    spaced.postMinSpacingMs = 250;
    FloodGovernor s(spaced);
    const quint64 t = s.postSent(0);
    s.postAnswered(t, 10, false, 1);
    QVERIFY(!s.postReady(100));
    QCOMPARE(s.nextPostCheckMs(100), qint64(250));
    QVERIFY(s.postReady(250));
}

void TestFoundation::postFloodPauseAndRetries()
{
    FloodGovernor g;
    int           attempts = 0;
    qint64        now      = 0;
    for (;;) {
        QVERIFY(g.postReady(now));
        const quint64 ticket = g.postSent(now);
        ++attempts;
        const bool retry = g.postAnswered(ticket, now + 50, true, attempts);
        if (!retry)
            break;
        // Paused for 2 s after a 0x020c.
        QVERIFY(!g.postReady(now + 51));
        QVERIFY(!g.postReady(now + 2049));
        QCOMPARE(g.nextPostCheckMs(now + 60), now + 2050);
        now += 2050;
    }
    QCOMPARE(attempts, 4); // the first send and 3 retries
    QCOMPARE(g.counters().postFloods, 4);
    QVERIFY(g.commandsPaused(now + 100)); // commands pause too
}

void TestFoundation::commandsWaitForPosts()
{
    FloodGovernor g;
    QVERIFY(g.commandReady(0));
    g.setPendingPosts(2);
    QVERIFY(!g.commandReady(0)); // posts first
    QCOMPARE(g.nextCommandCheckMs(0), qint64(-1));
    g.setPendingPosts(0);
    const quint64 ticket = g.postSent(0); // in flight
    QVERIFY(!g.commandReady(10));
    QCOMPARE(g.nextCommandCheckMs(10), qint64(1000));
    g.postAnswered(ticket, 20, false, 1);
    QVERIFY(g.commandReady(20));

    // A flooded command pauses posts too.
    g.commandSent(30);
    g.commandFlooded(40);
    QVERIFY(!g.postReady(41));
    QVERIFY(g.postReady(2040));
    QVERIFY(!g.commandReady(2040));
    QCOMPARE(g.counters().commandFloods, 1);
}

void TestFoundation::commandBucket()
{
    FloodGovernor g;
    // A burst of 6, then one every 1.5 s.
    int sent = 0;
    while (g.commandReady(0)) {
        g.commandSent(0);
        ++sent;
    }
    QCOMPARE(sent, 6);
    QCOMPARE(g.nextCommandCheckMs(0), qint64(1500));
    QVERIFY(!g.commandReady(1499));
    QVERIFY(g.commandReady(1500));
    g.commandSent(1500);
    QVERIFY(!g.commandReady(1500));
    // Refills up to the burst size, not beyond.
    sent = 0;
    while (g.commandReady(100000)) {
        g.commandSent(100000);
        ++sent;
    }
    QCOMPARE(sent, 6);
    // Posts spend tokens but never wait for them: an album posted back to back empties the bucket.
    FloodGovernor h;
    for (int i = 0; i < 10; ++i) {
        QVERIFY(h.postReady(i * 20));
        h.postAnswered(h.postSent(i * 20), i * 20 + 10, false, 1);
    }
    QVERIFY(!h.commandReady(190));
    QVERIFY(h.commandReady(180 + 1500));
}

void TestFoundation::commandBackoff()
{
    FloodGovernor g;
    g.commandFlooded(0);
    QCOMPARE(g.counters().currentBackoffMs, 15000);
    QVERIFY(!g.commandReady(14999));
    QVERIFY(g.commandsPaused(14999) && !g.commandsPaused(15000));
    g.commandFlooded(20000); // again soon: doubles
    QCOMPARE(g.counters().currentBackoffMs, 30000);
    g.commandFlooded(51000);
    QCOMPARE(g.counters().currentBackoffMs, 60000);
    g.commandFlooded(110000);
    QCOMPARE(g.counters().currentBackoffMs, 60000); // capped
    QVERIFY(!g.commandReady(169999) && g.commandsPaused(169999));
    // After a calm minute, it starts small again.
    g.answeredOk(110000 + 60000);
    QCOMPARE(g.counters().currentBackoffMs, 0);
    g.commandFlooded(400000);
    QCOMPARE(g.counters().currentBackoffMs, 15000);
    QCOMPARE(g.counters().lastFloodMs, qint64(400000));
}

void TestFoundation::floodBurstScenario()
{
    // A 10-item album (5 messages), 20 reactions and a HELLO storm on one connection, with a server
    // that answers posts after 80 ms and floods the third post once. Posts keep their order and go
    // one at a time; no plugin command goes while posts wait; commands never exceed the bucket.
    FloodGovernor     g;
    QList<int>        queue = {1, 2, 3, 4, 5};
    QVector<int>      delivered;
    QHash<int, int>   attempts;
    int               commands     = 25;
    int               commandsSent = 0;
    bool              floodedOnce  = false;
    quint64           inFlight     = 0;
    int               inFlightPost = 0;
    qint64            answerAt     = -1;
    QVector<qint64>   commandTimes;
    for (qint64 now = 0; now < 120000; now += 10) {
        if (inFlight && now >= answerAt) {
            const bool flood = inFlightPost == 3 && !floodedOnce;
            floodedOnce      = floodedOnce || flood;
            if (g.postAnswered(inFlight, now, flood, attempts.value(inFlightPost)))
                queue.prepend(inFlightPost);
            else
                QVERIFY(!flood);
            if (!flood)
                delivered.append(inFlightPost);
            inFlight = 0;
        }
        g.setPendingPosts(queue.size());
        if (!queue.isEmpty() && g.postReady(now)) {
            inFlightPost = queue.takeFirst();
            ++attempts[inFlightPost];
            inFlight = g.postSent(now);
            answerAt = now + 80;
            g.setPendingPosts(queue.size());
        }
        if (commands > 0 && g.commandReady(now)) {
            QVERIFY(queue.isEmpty()); // posts first
            g.commandSent(now);
            commandTimes.append(now);
            --commands;
            ++commandsSent;
        }
    }
    // (Element by element: QVector/QList operator== goes through MSVC's deprecated checked_array_iterator.)
    QCOMPARE(delivered.size(), 5);
    for (int i = 0; i < delivered.size(); ++i)
        QCOMPARE(delivered.at(i), i + 1);
    QCOMPARE(commandsSent, 25);
    // Never more than the burst within any window shorter than the refill of one token.
    for (int i = 6; i < commandTimes.size(); ++i)
        QVERIFY(commandTimes.at(i) - commandTimes.at(i - 6) >= 1500 - 10);
    // The flood paused commands for 15 s.
    QVERIFY(commandTimes.first() >= 15000);
}

TSMEDIA_REGISTER_TEST(TestFoundation)

#include "tst_foundation.moc"
