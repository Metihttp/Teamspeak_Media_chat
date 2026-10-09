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
    void postBucketKeepsReserve();
    void chargesShareTheGeneralCounter();
    void postFloodHintAndRetries();
    void floodWithoutHint();
    void retryHintParsing_data();
    void retryHintParsing();
    void pluginBucket();
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
// The numbers are the S0 measurement (server 3.13 defaults, Guest): 150 points, 5 back per second,
// text 15, file info 8, mkdir 5, transfers 3, plugin commands 5 on their own counter.

void TestFoundation::postsOneAtATime()
{
    FloodGovernor g;
    QVERIFY(g.postReady(0));
    const quint64 first = g.postSent(0);
    QVERIFY(!g.postReady(10));                     // waits for its answer
    QCOMPARE(g.nextPostCheckMs(10), qint64(1000)); // or a second
    QVERIFY(!g.postAnswered(first, 200, false, 1));
    QVERIFY(g.postReady(200)); // answered: the next may go
    const quint64 second = g.postSent(300);
    QVERIFY(second != first);
    QVERIFY(!g.postReady(1299));
    QVERIFY(g.postReady(1300)); // no answer within a second: the next one may go (the first isn't "sent")
    // A late answer for an older post doesn't release a newer one.
    const quint64 third = g.postSent(1300);
    QVERIFY(!g.postAnswered(second, 1310, false, 1));
    QVERIFY(!g.postReady(1320));
    QVERIFY(!g.postAnswered(third, 1330, false, 1));
    QVERIFY(g.postReady(1330));
    QCOMPARE(g.counters().postsSent, 3);
}

void TestFoundation::postBucketKeepsReserve()
{
    // From rest: 8 posts back to back (8 x 15 = 120 points, 30 stay for the user's own messages), then
    // one every 3 s (15 points at 5 a second).
    FloodGovernor g;
    int           burst = 0;
    while (g.postReady(0)) {
        g.postAnswered(g.postSent(0), 0, false, 1);
        ++burst;
    }
    QCOMPARE(burst, 8);
    QCOMPARE(g.generalPoints(0), 120.0);
    QCOMPARE(g.nextPostCheckMs(0), qint64(3000));
    QVERIFY(!g.postReady(2999));
    QVERIFY(g.postReady(3000));
    qint64 now = 3000;
    for (int i = 0; i < 5; ++i) {
        QVERIFY(g.postReady(now));
        g.postAnswered(g.postSent(now), now + 20, false, 1);
        QCOMPARE(g.nextPostCheckMs(now + 20), now + 3000);
        now += 3000;
    }
    // Rested again after 24 s.
    QCOMPARE(g.generalPoints(now + 24000), 0.0);
}

void TestFoundation::chargesShareTheGeneralCounter()
{
    FloodGovernor g;
    // Transfers, folders and file info are never held back, but posts wait for the points they cost.
    for (int i = 0; i < 20; ++i)
        g.charge(FloodGovernor::Cost::Transfer, 0); // 60
    g.charge(FloodGovernor::Cost::Mkdir, 0);        // 65
    g.charge(FloodGovernor::Cost::FileInfo, 0);     // 73
    QCOMPARE(g.generalPoints(0), 73.0);
    int posts = 0;
    while (g.postReady(0)) {
        g.postAnswered(g.postSent(0), 0, false, 1);
        ++posts;
    }
    QCOMPARE(posts, 3); // 73 + 45 = 118; a fourth would leave less than the reserve
    QCOMPARE(g.generalPoints(1000), 113.0);
    // Plugin commands have their own counter.
    QVERIFY(g.commandReady(0));
}

void TestFoundation::postFloodHintAndRetries()
{
    FloodGovernor g;
    QCOMPARE(FloodGovernor::retryHintMs(QStringLiteral("retry in 5687ms")), 5687);
    int    attempts = 0;
    qint64 now      = 0;
    for (;;) {
        QVERIFY(g.postReady(now));
        const quint64 ticket = g.postSent(now);
        ++attempts;
        const bool retry = g.postAnswered(ticket, now + 50, true, attempts, 5687);
        // Paused for the server's hint + 250 ms; plugin commands too.
        QCOMPARE(g.counters().lastPauseMs, 5937);
        QVERIFY(!g.postReady(now + 51));
        QVERIFY(g.commandsPaused(now + 51));
        QCOMPARE(g.nextPostCheckMs(now + 60), now + 50 + 5937);
        if (!retry)
            break;
        now += 50 + 5937;
    }
    QCOMPARE(attempts, 4); // the first send and 3 retries
    QCOMPARE(g.counters().postFloods, 4);

    // After the pause exactly one post fits, then the steady rate.
    FloodGovernor h;
    h.postAnswered(h.postSent(0), 10, true, 1, 5000);
    const qint64 resume = 10 + 5250;
    QVERIFY(!h.postReady(resume - 1));
    QVERIFY(h.postReady(resume));
    h.postAnswered(h.postSent(resume), resume + 10, false, 1);
    QVERIFY(!h.postReady(resume + 10));
    QCOMPARE(h.nextPostCheckMs(resume + 10), resume + 3000);
}

void TestFoundation::floodWithoutHint()
{
    FloodGovernor g;
    QCOMPARE(FloodGovernor::retryHintMs(QString()), -1);
    g.postAnswered(g.postSent(0), 0, true, 1, -1);
    QCOMPARE(g.counters().lastPauseMs, g.limits().textFloodPauseMs);
    QVERIFY(g.postsPaused(5999) && !g.postsPaused(6000));

    FloodGovernor c;
    c.commandSent(0);
    c.commandFlooded(0); // no hint: the shorter pause measured for plugin commands
    QCOMPARE(c.counters().lastPauseMs, c.limits().commandFloodPauseMs);
    QVERIFY(c.postsPaused(1999) && !c.commandReady(1999));
    QVERIFY(c.commandReady(2000));

    FloodGovernor f;
    f.charge(FloodGovernor::Cost::Mkdir, 0);
    f.generalFlooded(0, 1500); // a folder request hit the limit: posts wait as well
    QCOMPARE(f.counters().otherFloods, 1);
    QVERIFY(!f.postReady(1749));
    QVERIFY(f.postReady(1750));
}

void TestFoundation::retryHintParsing_data()
{
    QTest::addColumn<QString>("extra");
    QTest::addColumn<int>("ms");
    QTest::newRow("measured") << QStringLiteral("retry in 5687ms") << 5687;
    QTest::newRow("space") << QStringLiteral("please retry in 1233 ms") << 1233;
    QTest::newRow("case") << QStringLiteral("Retry In 20MS") << 20;
    QTest::newRow("capped") << QStringLiteral("retry in 999999999ms") << 120000;
    QTest::newRow("none") << QStringLiteral("client is flooding") << -1;
    QTest::newRow("seconds") << QStringLiteral("retry in 5 s") << -1;
    QTest::newRow("negative") << QStringLiteral("retry in -5ms") << -1;
}

void TestFoundation::retryHintParsing()
{
    QFETCH(QString, extra);
    QFETCH(int, ms);
    QCOMPARE(FloodGovernor::retryHintMs(extra), ms);
}

void TestFoundation::pluginBucket()
{
    // From rest 24 commands (5 points each, 30 kept), then one a second. Posts don't hold them back:
    // the server counts plugin commands separately.
    FloodGovernor g;
    g.setPendingPosts(3);
    g.postSent(0);
    int sent = 0;
    while (g.commandReady(0)) {
        g.commandSent(0);
        ++sent;
    }
    QCOMPARE(sent, 24);
    QCOMPARE(g.nextCommandCheckMs(0), qint64(1000));
    QVERIFY(!g.commandReady(999));
    QVERIFY(g.commandReady(1000));
    g.commandSent(1000);
    QVERIFY(!g.commandReady(1000));
    // Back to a full burst after a rest.
    sent = 0;
    while (g.commandReady(100000)) {
        g.commandSent(100000);
        ++sent;
    }
    QCOMPARE(sent, 24);
    QCOMPARE(g.counters().commandsSent, 49);

    // A flooded command pauses everything for the hint; then one command fits.
    FloodGovernor h;
    h.commandSent(0);
    h.commandFlooded(10, 1233);
    QCOMPARE(h.counters().commandFloods, 1);
    QVERIFY(!h.postReady(1492) && !h.commandReady(1492));
    QVERIFY(h.postReady(1493) && h.commandReady(1493));
    h.commandSent(1493);
    QVERIFY(!h.commandReady(1493));
    QCOMPARE(h.nextCommandCheckMs(1493), qint64(2493));
}

void TestFoundation::floodBurstScenario()
{
    // A 10-item album (5 messages) next to a stream of 40 plugin commands on one connection, with a
    // server that answers posts after 80 ms and floods the third post once ("retry in 5687ms"). Posts
    // keep their order and go one at a time; neither counter ever goes past its limit.
    FloodGovernor   g;
    QList<int>      queue = {1, 2, 3, 4, 5};
    QVector<int>    delivered;
    QHash<int, int> attempts;
    int             commands     = 40;
    bool            floodedOnce  = false;
    quint64         inFlight     = 0;
    int             inFlightPost = 0;
    qint64          answerAt     = -1;
    qint64          flood        = -1;
    QVector<qint64> postTimes;
    for (qint64 now = 0; now < 120000; now += 10) {
        if (inFlight && now >= answerAt) {
            const bool flooded = inFlightPost == 3 && !floodedOnce;
            floodedOnce        = floodedOnce || flooded;
            if (flooded)
                flood = now;
            if (g.postAnswered(inFlight, now, flooded, attempts.value(inFlightPost), flooded ? 5687 : -1))
                queue.prepend(inFlightPost);
            else
                QVERIFY(!flooded);
            if (!flooded)
                delivered.append(inFlightPost);
            inFlight = 0;
        }
        g.setPendingPosts(queue.size());
        if (!queue.isEmpty() && g.postReady(now)) {
            inFlightPost = queue.takeFirst();
            ++attempts[inFlightPost];
            inFlight = g.postSent(now);
            answerAt = now + 80;
            postTimes.append(now);
        }
        if (commands > 0 && g.commandReady(now)) {
            g.commandSent(now);
            --commands;
        }
        QVERIFY(g.generalPoints(now) <= g.limits().generalCapacity - g.limits().generalReserve + 0.001);
        QVERIFY(g.pluginPoints(now) <= g.limits().pluginCapacity - g.limits().pluginReserve + 0.001);
    }
    // (Element by element: QVector/QList operator== goes through MSVC's deprecated checked_array_iterator.)
    QCOMPARE(delivered.size(), 5);
    for (int i = 0; i < delivered.size(); ++i)
        QCOMPARE(delivered.at(i), i + 1);
    QCOMPARE(commands, 0);
    // Nothing went during the pause the flood asked for.
    QVERIFY(flood > 0);
    for (qint64 t : qAsConst(postTimes))
        QVERIFY(t <= flood || t >= flood + 5687 + 250);
}

TSMEDIA_REGISTER_TEST(TestFoundation)

#include "tst_foundation.moc"
