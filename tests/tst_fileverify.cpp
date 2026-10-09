// Unit tests for 2.2 sha (src/fileverify): the download check with a fake environment (hostile,
// mismatching, truncated, racing and canceled cases), the forged-hash guard, the sender's hashing step,
// the preview check and the texts. Core's own wiring is tested in tst_coresha.cpp.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QTemporaryDir>
#include <QThread>
#include <QWaitCondition>
#include <QtTest>

#include <atomic>

#include "fileverify.h"
#include "hashing.h"
#include "testmain.h"

namespace {

using fileverify::Outcome;
using fileverify::Result;

QByteArray digestOf(const QByteArray& data)
{
    return hashing::sha256(data);
}

QByteArray pattern(qint64 size, int seed = 0)
{
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 8) + seed) & 0xff);
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

// A hash function the test scripts: for each path a list of passes, one per call (the last repeats).
// A pass can be held until release() (or a cancel), and can report progress first. Thread-safe: the
// verifier calls it on its pool.
class FakeHasher
{
  public:
    struct Pass {
        QByteArray digest;          // empty: a read error
        QString    error;
        bool       hold     = false;
        double     progress = -1.0; // reported before returning, if >= 0
    };

    void script(const QString& path, const QList<Pass>& passes)
    {
        QMutexLocker lock(&m_mutex);
        m_scripts.insert(path, passes);
    }

    void release()
    {
        QMutexLocker lock(&m_mutex);
        m_released = true;
        m_changed.wakeAll();
    }

    int calls(const QString& path) const
    {
        QMutexLocker lock(&m_mutex);
        return m_calls.value(path);
    }

    int holding() const
    {
        QMutexLocker lock(&m_mutex);
        return m_holding;
    }

    fileverify::HashFile function()
    {
        return [this](const QString& path, const std::atomic<bool>* cancel, const hashing::Progress& progress, QString* error) { return run(path, cancel, progress, error); };
    }

  private:
    QByteArray run(const QString& path, const std::atomic<bool>* cancel, const hashing::Progress& progress, QString* error)
    {
        Pass pass;
        {
            QMutexLocker lock(&m_mutex);
            const int call = m_calls.value(path);
            m_calls[path]  = call + 1;
            const auto it  = m_scripts.constFind(path);
            if (it != m_scripts.constEnd() && !it->isEmpty())
                pass = it->value(qMin(call, it->size() - 1));
            if (pass.hold) {
                ++m_holding;
                while (!m_released && !(cancel && cancel->load()))
                    m_changed.wait(&m_mutex, 5);
                --m_holding;
            }
        }
        if (cancel && cancel->load()) {
            if (error)
                *error = QStringLiteral("canceled");
            return {};
        }
        if (pass.progress >= 0 && progress)
            progress(static_cast<qint64>(pass.progress * 1000.0), 1000);
        if (error)
            *error = pass.error;
        return pass.digest;
    }

    mutable QMutex                 m_mutex;
    QWaitCondition                 m_changed;
    QHash<QString, QList<Pass>>    m_scripts;
    QHash<QString, int>            m_calls;
    bool                           m_released = false;
    int                            m_holding  = 0;
};

fileverify::Environment quickEnvironment()
{
    fileverify::Environment env;
    env.recheckDelayMs = 40;
    env.writePollMs    = 5;
    env.maxWriteWaitMs = 2000;
    return env;
}

// Collects what a verifier reports.
struct Reports {
    QVector<Result>                    results;
    QVector<QPair<QString, double>>    progress;

    void attach(fileverify::Verifier* verifier)
    {
        QObject::connect(verifier, &fileverify::Verifier::finished, verifier, [this](const Result& r) { results.append(r); });
        QObject::connect(verifier, &fileverify::Verifier::progressChanged, verifier,
                         [this](const QString& key, double fraction, bool) { progress.append({key, fraction}); });
    }
};

} // namespace

class TestFileVerify : public QObject
{
    Q_OBJECT

  private slots:
    void init();

    // the download check (fake hash)
    void matchOnFirstPass();
    void mismatchIsCheckedTwice();
    void raceRecoversOnSecondPass();
    void secondPassWaitsForWriter();
    void writerThatNeverStops();
    void readErrors_data();
    void readErrors();
    void changingFileIsNoStableMismatch();
    void restartDropsOldResult();
    void cancelDropsResult();
    void shutdownStopsRunningPass();
    void progressIsReported();
    void invalidExpectedDigest();
    void severalKeysAtOnce();

    // the download check (real files)
    void realFileMatches();
    void realFileTruncatedOrPadded_data();
    void realFileTruncatedOrPadded();
    void realFileFixedBeforeSecondPass();
    void realFileMissing();

    // forged-hash guard
    void knownDigestsVerdicts();
    void knownDigestsIgnoresJunk();
    void knownDigestsForgetsOldest();

    // sender
    void finalizeStagedHashesStagedBytes();
    void finalizeStagedReadErrors();

    // preview, texts, counters
    void previewDigests();
    void mismatchDetailsText();
    void diagnostics();
};

void TestFileVerify::init()
{
    fileverify::resetCounters();
}

// ---- the download check (fake hash) ---------------------------------------------------------------

void TestFileVerify::matchOnFirstPass()
{
    const QByteArray good = digestOf("good");
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {{good}});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), good);
    QVERIFY(verifier.isChecking(QStringLiteral("k")));
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(r.key, QStringLiteral("k"));
    QCOMPARE(r.outcome, Outcome::Match);
    QCOMPARE(r.received, good);
    QVERIFY(!r.rechecked);
    QVERIFY(r.firstReceived.isEmpty());
    QVERIFY(!verifier.isChecking(QStringLiteral("k")));
    QCOMPARE(hasher.calls(QStringLiteral("a")), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::RecoveredOnRecheck), 0);
}

void TestFileVerify::mismatchIsCheckedTwice()
{
    const QByteArray expected = digestOf("sent");
    const QByteArray other    = digestOf("on the server");
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {{other}});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    QElapsedTimer timer;
    timer.start();
    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_COMPARE(reports.results.size(), 1);
    QVERIFY(timer.elapsed() >= env.recheckDelayMs); // not before the delay
    const Result r = reports.results.first();
    QCOMPARE(r.outcome, Outcome::Mismatch);
    QCOMPARE(r.expected, expected);
    QCOMPARE(r.received, other);
    QCOMPARE(r.firstReceived, other);
    QVERIFY(r.rechecked);
    QVERIFY(r.stableMismatch());
    QCOMPARE(hasher.calls(QStringLiteral("a")), 2); // exactly once more, never a third time
    QCOMPARE(fileverify::counted(fileverify::Counter::Mismatched), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 0);
}

void TestFileVerify::raceRecoversOnSecondPass()
{
    // TeamSpeak reported the transfer complete while the end was still zeros (2.0.5).
    const QByteArray expected = digestOf("complete");
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {{digestOf("zeros at the end")}, {expected}});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(r.outcome, Outcome::Match);
    QVERIFY(r.rechecked);
    QCOMPARE(r.firstReceived, digestOf("zeros at the end"));
    QVERIFY(!r.stableMismatch());
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::RecoveredOnRecheck), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::Mismatched), 0);
}

void TestFileVerify::secondPassWaitsForWriter()
{
    const QByteArray expected = digestOf("complete");
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {{digestOf("partial")}, {expected}});
    std::atomic<bool> writing{true};
    int               asked = 0;
    auto              env   = quickEnvironment();
    env.hashFile            = hasher.function();
    env.isBeingWritten      = [&](const QString& path) {
        ++asked;
        return path == QLatin1String("a") && writing.load();
    };
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_VERIFY(asked >= 3); // polling while the writer has the file
    QTest::qWait(env.recheckDelayMs * 3);
    QCOMPARE(hasher.calls(QStringLiteral("a")), 1); // no second pass while it is written
    QVERIFY(reports.results.isEmpty());
    QVERIFY(verifier.isSecondPass(QStringLiteral("k")));

    QElapsedTimer sinceWriterLeft;
    sinceWriterLeft.start();
    writing = false;
    QTRY_COMPARE(reports.results.size(), 1);
    QVERIFY(sinceWriterLeft.elapsed() >= env.recheckDelayMs - 5); // and a moment longer
    QCOMPARE(reports.results.first().outcome, Outcome::Match);
    QCOMPARE(hasher.calls(QStringLiteral("a")), 2);
}

void TestFileVerify::writerThatNeverStops()
{
    const QByteArray expected = digestOf("sent");
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {{digestOf("other")}});
    auto env           = quickEnvironment();
    env.hashFile       = hasher.function();
    env.maxWriteWaitMs = 60;
    env.isBeingWritten = [](const QString&) { return true; };
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_COMPARE(reports.results.size(), 1); // the second pass runs anyway
    QCOMPARE(reports.results.first().outcome, Outcome::Mismatch);
    QCOMPARE(hasher.calls(QStringLiteral("a")), 2);
}

void TestFileVerify::readErrors_data()
{
    QTest::addColumn<bool>("firstReads");
    QTest::addColumn<bool>("secondReads");
    QTest::addColumn<int>("outcome");
    QTest::addColumn<QString>("error");
    // A first pass that can't read (the file still growing: "changed while it was read") gets a second.
    QTest::newRow("unreadable, then the right bytes") << false << true << static_cast<int>(Outcome::Match) << QString();
    QTest::newRow("unreadable twice") << false << false << static_cast<int>(Outcome::ReadError) << QStringLiteral("second error");
    QTest::newRow("wrong bytes, then unreadable") << true << false << static_cast<int>(Outcome::ReadError) << QStringLiteral("second error");
}

void TestFileVerify::readErrors()
{
    QFETCH(bool, firstReads);
    QFETCH(bool, secondReads);
    QFETCH(int, outcome);
    QFETCH(QString, error);

    const QByteArray expected = digestOf("sent");
    FakeHasher::Pass first{firstReads ? digestOf("other") : QByteArray(), QStringLiteral("first error")};
    FakeHasher::Pass second{secondReads ? expected : QByteArray(), QStringLiteral("second error")};
    FakeHasher       hasher;
    hasher.script(QStringLiteral("a"), {first, second});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(static_cast<int>(r.outcome), outcome);
    QCOMPARE(r.error, error);
    QVERIFY(r.rechecked);
    QVERIFY(!r.stableMismatch());
    if (r.outcome == Outcome::ReadError) {
        QVERIFY(r.received.isEmpty());
        QCOMPARE(fileverify::counted(fileverify::Counter::ReadErrors), 1);
    }
}

void TestFileVerify::changingFileIsNoStableMismatch()
{
    // Two different wrong digests: the file changed between the passes. Still a mismatch, but Core
    // must not remember either digest as the server file's.
    FakeHasher hasher;
    hasher.script(QStringLiteral("a"), {{digestOf("one")}, {digestOf("two")}});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), digestOf("sent"));
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(r.outcome, Outcome::Mismatch);
    QCOMPARE(r.received, digestOf("two"));
    QCOMPARE(r.firstReceived, digestOf("one"));
    QVERIFY(!r.stableMismatch());
}

void TestFileVerify::restartDropsOldResult()
{
    const QByteArray expected = digestOf("sent");
    FakeHasher       hasher;
    FakeHasher::Pass held{expected};
    held.hold = true;
    hasher.script(QStringLiteral("old"), {held});
    hasher.script(QStringLiteral("new"), {{expected}});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("old"), expected);
    QTRY_COMPARE(hasher.holding(), 1);
    verifier.start(QStringLiteral("k"), QStringLiteral("new"), expected); // the same key again
    QTRY_COMPARE(reports.results.size(), 1);
    hasher.release();
    QTest::qWait(50);
    QCOMPARE(reports.results.size(), 1); // the old pass never reports
    QCOMPARE(reports.results.first().outcome, Outcome::Match);
    QCOMPARE(hasher.holding(), 0);
}

void TestFileVerify::cancelDropsResult()
{
    FakeHasher       hasher;
    FakeHasher::Pass held{digestOf("x")};
    held.hold = true;
    hasher.script(QStringLiteral("a"), {held});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), digestOf("x"));
    QTRY_COMPARE(hasher.holding(), 1);
    verifier.cancel(QStringLiteral("k"));
    QVERIFY(!verifier.isChecking(QStringLiteral("k")));
    QTRY_COMPARE(hasher.holding(), 0); // the cancel flag stops the pass
    QTest::qWait(30);
    QVERIFY(reports.results.isEmpty());

    // A cancel while waiting for the second pass: that pass never starts.
    FakeHasher other;
    other.script(QStringLiteral("b"), {{digestOf("wrong")}});
    auto env2           = quickEnvironment();
    env2.hashFile       = other.function();
    env2.recheckDelayMs = 200;
    fileverify::Verifier second(env2);
    Reports              reports2;
    reports2.attach(&second);
    second.start(QStringLiteral("k"), QStringLiteral("b"), digestOf("x"));
    QTRY_VERIFY(second.isSecondPass(QStringLiteral("k")));
    second.cancel(QStringLiteral("k"));
    QTest::qWait(300);
    QCOMPARE(other.calls(QStringLiteral("b")), 1);
    QVERIFY(reports2.results.isEmpty());
}

void TestFileVerify::shutdownStopsRunningPass()
{
    FakeHasher       hasher;
    FakeHasher::Pass held{digestOf("x")};
    held.hold = true;
    hasher.script(QStringLiteral("a"), {held});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    auto*   verifier = new fileverify::Verifier(env);
    Reports reports;
    reports.attach(verifier);

    verifier->start(QStringLiteral("k1"), QStringLiteral("a"), digestOf("x"));
    verifier->start(QStringLiteral("k2"), QStringLiteral("a"), digestOf("x"));
    QTRY_COMPARE(hasher.holding(), 2);
    QElapsedTimer timer;
    timer.start();
    delete verifier; // shutdown(): cancels and joins
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(hasher.holding(), 0);
    QTest::qWait(20);
    QVERIFY(reports.results.isEmpty());
}

void TestFileVerify::progressIsReported()
{
    const QByteArray expected = digestOf("sent");
    FakeHasher       hasher;
    FakeHasher::Pass pass{expected};
    pass.progress = 0.5;
    hasher.script(QStringLiteral("a"), {pass});
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), expected);
    QTRY_COMPARE(reports.results.size(), 1);
    QCOMPARE(reports.progress.size(), 1);
    QCOMPARE(reports.progress.first().first, QStringLiteral("k"));
    QCOMPARE(reports.progress.first().second, 0.5);
}

void TestFileVerify::invalidExpectedDigest()
{
    FakeHasher hasher;
    auto       env = quickEnvironment();
    env.hashFile   = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    verifier.start(QStringLiteral("k"), QStringLiteral("a"), QByteArray(31, 'x'));
    QVERIFY(reports.results.isEmpty()); // reported from the event loop, never inside start()
    QTRY_COMPARE(reports.results.size(), 1);
    QCOMPARE(reports.results.first().outcome, Outcome::ReadError);
    QCOMPARE(hasher.calls(QStringLiteral("a")), 0); // nothing to compare with: not even read
}

void TestFileVerify::severalKeysAtOnce()
{
    FakeHasher hasher;
    QHash<QString, QByteArray> expected;
    for (int i = 0; i < 6; ++i) {
        const QString path = QStringLiteral("f%1").arg(i);
        expected.insert(path, digestOf(path.toLatin1()));
        hasher.script(path, {{i % 2 ? digestOf("wrong") : expected.value(path)}});
    }
    auto env     = quickEnvironment();
    env.hashFile = hasher.function();
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);

    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        verifier.start(QStringLiteral("key ") + it.key(), it.key(), it.value());
    QCOMPARE(verifier.running(), 6);
    QTRY_COMPARE(reports.results.size(), 6);
    int matched = 0;
    for (const Result& r : qAsConst(reports.results)) {
        const int index = r.key.right(1).toInt();
        QCOMPARE(r.outcome, index % 2 ? Outcome::Mismatch : Outcome::Match);
        matched += r.outcome == Outcome::Match;
    }
    QCOMPARE(matched, 3);
    QCOMPARE(verifier.running(), 0);
}

// ---- the download check (real files) ------------------------------------------------------------

void TestFileVerify::realFileMatches()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("video.mp4"));
    const QByteArray data = pattern(3 * hashing::kChunkBytes + 17);
    QVERIFY(writeFile(path, data));
    fileverify::Verifier verifier(quickEnvironment()); // hashing::sha256File
    Reports              reports;
    reports.attach(&verifier);
    verifier.start(QStringLiteral("k"), path, digestOf(data));
    QTRY_COMPARE(reports.results.size(), 1);
    QCOMPARE(reports.results.first().outcome, Outcome::Match);
}

void TestFileVerify::realFileTruncatedOrPadded_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("truncated by one byte") << -1;
    QTest::newRow("one byte more") << 1;
    QTest::newRow("one byte flipped") << 0;
    QTest::newRow("end zeroed (2.0.5 race)") << 2;
}

void TestFileVerify::realFileTruncatedOrPadded()
{
    QFETCH(int, change);
    QTemporaryDir    dir;
    const QString    path = dir.filePath(QStringLiteral("photo.jpg"));
    const QByteArray sent = pattern(hashing::kChunkBytes + 4096);
    QByteArray       got  = sent;
    if (change == -1)
        got.chop(1);
    else if (change == 1)
        got.append('x');
    else if (change == 0)
        got[1000] = static_cast<char>(got.at(1000) ^ 0x01);
    else
        got.replace(got.size() - 4096, 4096, QByteArray(4096, '\0'));
    QVERIFY(writeFile(path, got));
    fileverify::Verifier verifier(quickEnvironment());
    Reports              reports;
    reports.attach(&verifier);
    verifier.start(QStringLiteral("k"), path, digestOf(sent));
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(r.outcome, Outcome::Mismatch);
    QCOMPARE(r.received, digestOf(got));
    QVERIFY(r.stableMismatch());
}

void TestFileVerify::realFileFixedBeforeSecondPass()
{
    // The race end to end with a real file: the first pass reads zeros at the end; "TeamSpeak" still
    // has it open, finishes writing, closes it; the second pass reads the complete file.
    QTemporaryDir    dir;
    const QString    path = dir.filePath(QStringLiteral("clip.mp4"));
    const QByteArray sent = pattern(2 * hashing::kChunkBytes + 65536);
    QByteArray       torn = sent;
    torn.replace(torn.size() - 65536, 65536, QByteArray(65536, '\0'));
    QVERIFY(writeFile(path, torn));

    int  polls = 0;
    auto env   = quickEnvironment();
    env.isBeingWritten = [&](const QString& p) {
        if (p != path)
            return false;
        ++polls;
        if (polls == 3)
            writeFile(path, sent); // the writer catches up ...
        return polls <= 3;          // ... and closes the file
    };
    fileverify::Verifier verifier(env);
    Reports              reports;
    reports.attach(&verifier);
    verifier.start(QStringLiteral("k"), path, digestOf(sent));
    QTRY_COMPARE(reports.results.size(), 1);
    const Result r = reports.results.first();
    QCOMPARE(r.outcome, Outcome::Match);
    QVERIFY(r.rechecked);
    QCOMPARE(r.firstReceived, digestOf(torn));
    QVERIFY(polls > 3);
}

void TestFileVerify::realFileMissing()
{
    QTemporaryDir        dir;
    fileverify::Verifier verifier(quickEnvironment());
    Reports              reports;
    reports.attach(&verifier);
    verifier.start(QStringLiteral("k"), dir.filePath(QStringLiteral("gone.png")), digestOf("x"));
    QTRY_COMPARE(reports.results.size(), 1);
    QCOMPARE(reports.results.first().outcome, Outcome::ReadError);
    QVERIFY(!reports.results.first().error.isEmpty());
}

// ---- forged-hash guard -----------------------------------------------------------------------------

void TestFileVerify::knownDigestsVerdicts()
{
    using Verdict = fileverify::KnownDigests::Verdict;
    fileverify::KnownDigests known;
    const QString            file = QStringLiteral("uid\n5\n/tsmedia/a_1234abcd.png");
    const QByteArray         real = digestOf("real");
    const QByteArray         fake = digestOf("fake");

    QCOMPARE(known.check(file, 100, real), Verdict::Unknown);
    known.note(file, 100, real);
    QCOMPARE(known.check(file, 100, real), Verdict::Agrees);
    QCOMPARE(known.check(file, 100, fake), Verdict::Contradicts);
    QCOMPARE(known.check(file, 101, fake), Verdict::Unknown); // another size: another file (sizes are checked elsewhere)
    QCOMPARE(known.check(QStringLiteral("uid\n6\n/tsmedia/a_1234abcd.png"), 100, fake), Verdict::Unknown); // another channel
    QCOMPARE(known.check(file, 100, QByteArray()), Verdict::Unknown); // a link without a sha: nothing to compare
    QCOMPARE(known.known(file, 100), real);

    known.note(file, 100, fake); // a newer note (the file was replaced and checked again) wins
    QCOMPARE(known.check(file, 100, fake), Verdict::Agrees);
    QCOMPARE(known.count(), 1);
    known.clear();
    QCOMPARE(known.check(file, 100, fake), Verdict::Unknown);
}

void TestFileVerify::knownDigestsIgnoresJunk()
{
    fileverify::KnownDigests known;
    const QString            file = QStringLiteral("uid\n5\n/x.png");
    known.note(file, 100, QByteArray(31, 'x'));
    known.note(file, 0, digestOf("a")); // size unknown
    known.note(QString(), 100, digestOf("a"));
    QCOMPARE(known.count(), 0);
    QCOMPARE(known.check(file, 100, QByteArray(33, 'y')), fileverify::KnownDigests::Verdict::Unknown);
    QVERIFY(known.known(file, 0).isEmpty());
}

void TestFileVerify::knownDigestsForgetsOldest()
{
    using Verdict = fileverify::KnownDigests::Verdict;
    fileverify::KnownDigests known;
    const auto               file = [](int i) { return QStringLiteral("uid\n5\n/f%1.png").arg(i); };
    for (int i = 0; i < fileverify::KnownDigests::kMaxEntries; ++i)
        known.note(file(i), 10, digestOf(QByteArray::number(i)));
    known.note(file(0), 10, digestOf(QByteArray::number(0))); // used again: now the newest
    known.note(file(-1), 10, digestOf("new"));                // one too many: the oldest goes
    QCOMPARE(known.count(), fileverify::KnownDigests::kMaxEntries);
    QCOMPARE(known.check(file(0), 10, digestOf("0")), Verdict::Agrees);
    QCOMPARE(known.check(file(1), 10, digestOf("1")), Verdict::Unknown);
    QCOMPARE(known.check(file(2), 10, digestOf("2")), Verdict::Agrees);
    QCOMPARE(known.check(file(-1), 10, digestOf("new")), Verdict::Agrees);
}

// ---- sender ----------------------------------------------------------------------------------------

void TestFileVerify::finalizeStagedHashesStagedBytes()
{
    QTemporaryDir    dir;
    const QString    staged = dir.filePath(QStringLiteral("holiday_3f9a1c2e.jpg"));
    const QByteArray data   = pattern(hashing::kChunkBytes * 2 + 3);
    const QByteArray jpeg   = pattern(5000, 7);
    QVERIFY(writeFile(staged, data));

    const fileverify::StagedDigest digest = fileverify::finalizeStaged(staged, jpeg, nullptr);
    QCOMPARE(digest.sha256, digestOf(data));
    QCOMPARE(digest.previewSha, digestOf(jpeg).left(fileverify::kPreviewShaBytes));
    QVERIFY(digest.error.isEmpty());

    const fileverify::StagedDigest noPreview = fileverify::finalizeStaged(staged, QByteArray(), nullptr);
    QCOMPARE(noPreview.sha256, digestOf(data));
    QVERIFY(noPreview.previewSha.isEmpty());
}

void TestFileVerify::finalizeStagedReadErrors()
{
    QTemporaryDir dir;
    // Missing: no sha (the link goes without), but the preview's is still there.
    const fileverify::StagedDigest missing = fileverify::finalizeStaged(dir.filePath(QStringLiteral("gone.bin")), QByteArray("jpeg"), nullptr);
    QVERIFY(missing.sha256.isEmpty());
    QVERIFY(!missing.error.isEmpty());
    QCOMPARE(missing.previewSha.size(), fileverify::kPreviewShaBytes);

    const QString staged = dir.filePath(QStringLiteral("a.bin"));
    QVERIFY(writeFile(staged, pattern(1000)));
    std::atomic<bool>              canceled{true};
    const fileverify::StagedDigest stopped = fileverify::finalizeStaged(staged, QByteArray(), &canceled);
    QVERIFY(stopped.sha256.isEmpty());
    QCOMPARE(stopped.error, QStringLiteral("canceled"));

    // A hash function that returns something that isn't a SHA-256 is never trusted.
    const fileverify::StagedDigest odd = fileverify::finalizeStaged(staged, QByteArray(), nullptr, [](const QString&, const std::atomic<bool>*, const hashing::Progress&, QString*) {
        return QByteArray(20, 'x');
    });
    QVERIFY(odd.sha256.isEmpty());
    QVERIFY(!odd.error.isEmpty());
}

// ---- preview, texts, counters ------------------------------------------------------------------------

void TestFileVerify::previewDigests()
{
    const QByteArray jpeg = pattern(3000, 3);
    const QByteArray ph   = fileverify::previewDigest(jpeg);
    QCOMPARE(ph.size(), fileverify::kPreviewShaBytes);
    QVERIFY(fileverify::previewMatches(jpeg, ph));
    QByteArray changed = jpeg;
    changed[10]        = static_cast<char>(changed.at(10) ^ 0x80);
    QVERIFY(!fileverify::previewMatches(changed, ph));
    QVERIFY(!fileverify::previewMatches(jpeg, ph.left(15)));
    QVERIFY(!fileverify::previewMatches(jpeg, digestOf(jpeg))); // the full sha isn't a ph
    QVERIFY(!fileverify::previewMatches(jpeg, QByteArray()));
    QVERIFY(!fileverify::previewMatches(QByteArray(), ph));
}

void TestFileVerify::mismatchDetailsText()
{
    const QString text = fileverify::mismatchDetails(digestOf(""), digestOf("abc"), QStringLiteral("/tsmedia/sunset_3f9a1c2e.jpg"));
    QCOMPARE(text.split(QLatin1Char('\n')),
             (QStringList{QStringLiteral("Expected SHA-256: E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"),
                          QStringLiteral("Received SHA-256: BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"),
                          QStringLiteral("File: /tsmedia/sunset_3f9a1c2e.jpg")}));
    QVERIFY(fileverify::mismatchDetails(digestOf(""), QByteArray(), QStringLiteral("/x")).contains(QStringLiteral("Received SHA-256: (unknown)")));
}

void TestFileVerify::diagnostics()
{
    fileverify::count(fileverify::Counter::Verified);
    fileverify::count(fileverify::Counter::Verified);
    fileverify::count(fileverify::Counter::Mismatched);
    fileverify::count(fileverify::Counter::ForgedBlocked);
    fileverify::count(fileverify::Counter::SentWithSha);
    const QStringList lines = fileverify::diagnosticLines();
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines.at(0).contains(QStringLiteral("2 matched")));
    QVERIFY(lines.at(0).contains(QStringLiteral("1 didn't match")));
    QVERIFY(lines.at(1).contains(QStringLiteral(": 1;")));
    QVERIFY(lines.at(2).contains(QStringLiteral("1 with a checksum, 0 without")));
    QVERIFY(lines.at(3).contains(fileverify::hashBackend()));
    QVERIFY(!fileverify::diagnosticsTitle().isEmpty());
    fileverify::resetCounters();
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 0);
}

TSMEDIA_REGISTER_TEST(TestFileVerify)

#include "tst_fileverify.moc"
