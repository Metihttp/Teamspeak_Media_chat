// Unit tests for 2.3.2's upload race (src/uploadwait): when the sender's message may go (the server
// reports the file's whole size, or it gives up waiting), and what a receiver's download that came out
// short or didn't match does next (ask again, download again, or fail and why). Pure logic with a made-up
// clock; Core's wiring is tested in tst_coresha.cpp.
//
// Preallocating servers (TeaSpeak): the sender's content check (the tail compared, the fallbacks, the
// retries and their limits) and the receiver's zero tails (where a file's data ends, when that is an
// upload still arriving, and that it never counts as a mismatch).

#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "testmain.h"
#include "uploadwait.h"

namespace {

using uploadwait::Answer;
using uploadwait::Bytes;
using uploadwait::Confirm;
using uploadwait::Content;
using uploadwait::Method;
using uploadwait::Next;
using uploadwait::Seen;

const QByteArray kDigestA(32, 'a');
const QByteArray kDigestB(32, 'b');
const QByteArray kDigestC(32, 'c');

QByteArray pattern(int size, int seed = 0)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = static_cast<char>(((i * 7 + seed) % 255) + 1); // never zero
    return data;
}

// A preallocated file: the first `arrived` bytes of data, zeros after.
QByteArray preallocated(const QByteArray& data, int arrived)
{
    QByteArray file(data.size(), '\0');
    file.replace(0, arrived, data.left(arrived));
    return file;
}

qint64 dataEndOf(const QByteArray& bytes)
{
    QByteArray copy = bytes;
    QBuffer    buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    return uploadwait::dataEnd(buffer);
}

} // namespace

class TestUploadWait : public QObject
{
    Q_OBJECT

  private slots:
    void sameRemoteFile_data();
    void sameRemoteFile();
    void canAskFileInfo_data(); // 2.3.3
    void canAskFileInfo();

    // sender
    void pollDelaysBackOff();
    void timeoutGrowsWithTheFile();
    void confirmedAtOnce();
    void pollsWhileArriving();
    void largerMissingRefused();
    void stallsWithoutGrowth();
    void noAnswersStall();
    void timesOutWhileStillGrowing();
    void previewKeepsTheDeadline();
    void senderRaceScenario();

    // receiver
    void refetchDelaysBackOff();
    void shortWaitsForTheServer();
    void shortWithWholeFileDownloadsSoon();
    void waitingGivesUpWithoutProgress();
    void missingAndChanged();
    void mismatchNeedsTheSameDigestTwice();
    void mismatchWithChangingDigests();
    void mismatchWhileArrivingDoesNotCount();
    void serverCantTell();
    void noAnswerAsksAgain();
    void cantTellFlatDownloadsEnd();
    void cantTellMismatchIsNeverConfirmed();
    void confirmedOnlyAfterTheWholeFile();
    void downloadsAreBounded();
    void longerDownloadsCountAsProgress();
    void receiverRaceScenario();

    // sender, preallocating servers: the content check
    void tailLengthsAndDelays();
    void firstMethodChoices();
    void compareTail_data();
    void compareTail();
    void contentConfirmedAtOnce();
    void contentRetriesWhileZeros();
    void contentFallsBackToTheWholeFile();
    void contentNotPossible();
    void contentTimesOut();
    void contentStallsWithoutChange();
    void noAnswerDoesNotCountAsChange();
    void teaSpeakSenderScenario();
    void tailWindows();
    void tailWindowReachesTheLastData();
    void otherBytesFallBackToTheWholeFile();
    void refusedGoesAfterTheSize();

    // receiver, preallocating servers: zero tails
    void trailingZerosAndThresholds();
    void dataEndScansBackwards();
    void dataEndOfAFile();
    void zeroTailBoundaries();
    void zeroTailWaitsAndDownloadsAgain();
    void zeroTailIsNeverAMismatch();
    void zeroTailGrowingKeepsGoing();
    void firstDataAfterAllZerosIsGrowth();
    void zeroTailWithAShortServerPolls();
    void zeroTailWhileTheServerCantTell();
    void mismatchAfterZeroTailsStillNeedsTwo();
    void teaSpeakReceiverScenario();
    void mismatchNeedsOnlyAShortZeroRun();
    void stuckZeroTailIsConfirmed();
    void changingZeroTailIsNotConfirmed();
    void shaLessZeroTailSettles();
};

void TestUploadWait::sameRemoteFile_data()
{
    QTest::addColumn<QString>("reported");
    QTest::addColumn<QString>("requested");
    QTest::addColumn<bool>("same");
    QTest::newRow("full path") << "/tsmedia/photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("without leading slash") << "tsmedia/photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("name only") << "photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("root file, name only") << "photo_1234abcd.png" << "/photo_1234abcd.png" << true;
    QTest::newRow("name with a slash") << "/photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("trailing slash") << "/tsmedia/photo_1234abcd.png/" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("double slash") << "/tsmedia//photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("backslashes") << "\\tsmedia\\photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << true;
    QTest::newRow("another file") << "/tsmedia/photo_99999999.png" << "/tsmedia/photo_1234abcd.png" << false;
    QTest::newRow("another folder") << "/other/photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << false;
    QTest::newRow("the preview of it") << "/tsmedia/previews/1234abcd.jpg" << "/tsmedia/photo_1234abcd.png" << false;
    QTest::newRow("case differs") << "/tsmedia/Photo_1234abcd.png" << "/tsmedia/photo_1234abcd.png" << false;
    QTest::newRow("empty") << "" << "/tsmedia/photo_1234abcd.png" << false;
}

void TestUploadWait::sameRemoteFile()
{
    QFETCH(QString, reported);
    QFETCH(QString, requested);
    QFETCH(bool, same);
    QCOMPARE(uploadwait::sameRemoteFile(reported, requested), same);
}

void TestUploadWait::canAskFileInfo_data()
{
    // 2.3.3: TeaSpeak's answer to a file info request drops the TeamSpeak client's connection.
    QTest::addColumn<QString>("version");
    QTest::addColumn<QString>("platform");
    QTest::addColumn<bool>("teaSpeak");
    QTest::addColumn<bool>("ask");
    QTest::newRow("server 3.13") << "3.13.7 [Build: 1655727713]" << "Linux" << false << true;
    QTest::newRow("server 3.0 on Windows") << "3.0.13.8 [Build: 1500452811]" << "Windows" << false << true;
    QTest::newRow("TeaSpeak 1.5") << "TeaSpeak 1.5.6-beta [Build: 1622043432]" << "Linux" << true << false;
    QTest::newRow("TeaSpeak 1.4") << "TeaSpeak 1.4.21-beta [Build: 1600246831]" << "Linux" << true << false;
    QTest::newRow("TeaSpeak by its platform") << "3.13.7 [Build: 1655727713]" << "TeaSpeak" << true << false;
    QTest::newRow("lower case") << "teaspeak 1.5.6" << "Linux" << true << false;
    QTest::newRow("another server") << "MyServer 2.0" << "Linux" << false << false;
    QTest::newRow("version not known yet") << "" << "Linux" << false << false;
    QTest::newRow("nothing known") << "" << "" << false << false;
}

void TestUploadWait::canAskFileInfo()
{
    QFETCH(QString, version);
    QFETCH(QString, platform);
    QFETCH(bool, teaSpeak);
    QFETCH(bool, ask);
    QCOMPARE(uploadwait::looksLikeTeaSpeak(version, platform), teaSpeak);
    QCOMPARE(uploadwait::canAskFileInfo(version, platform), ask);
}

// ---- sender ---------------------------------------------------------------------------------------

void TestUploadWait::pollDelaysBackOff()
{
    QCOMPARE(uploadwait::confirmPollDelayMs(-1), 300);
    const QList<int> expected{300, 300, 500, 800, 1200, 2000, 2000, 2000};
    for (int i = 0; i < expected.size(); ++i)
        QCOMPARE(uploadwait::confirmPollDelayMs(i), expected.at(i));
    QCOMPARE(uploadwait::confirmPollDelayMs(1000), uploadwait::kMaxPollMs);
}

void TestUploadWait::timeoutGrowsWithTheFile()
{
    QCOMPARE(uploadwait::confirmTimeoutMs(0), qint64(60000));
    QCOMPARE(uploadwait::confirmTimeoutMs(100 * 1024), qint64(61000));                 // + 1 s at 100 KB/s
    QCOMPARE(uploadwait::confirmTimeoutMs(10 * 1024 * 1024), qint64(60000 + 102400)); // + 102.4 s
    QCOMPARE(uploadwait::confirmTimeoutMs(50 * 1024), qint64(60500));
    QCOMPARE(uploadwait::confirmTimeoutMs(Q_UINT64_C(4) * 1024 * 1024 * 1024), uploadwait::kConfirmMaxMs); // never more than 15 min
    QCOMPARE(uploadwait::confirmTimeoutMs(~Q_UINT64_C(0)), uploadwait::kConfirmMaxMs);
}

void TestUploadWait::confirmedAtOnce()
{
    uploadwait::ConfirmState state = uploadwait::startConfirm(2097152, 1000);
    QCOMPARE(state.deadline, 1000 + uploadwait::confirmTimeoutMs(2097152));
    const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, 2097152, Answer::Size, 2097152, 1040);
    QCOMPARE(step.what, Confirm::Confirmed);
    QCOMPARE(uploadwait::confirmName(step.what), QStringLiteral("confirmed"));
}

void TestUploadWait::pollsWhileArriving()
{
    // The server has half of it (what a 3.13 server reports while the upload still arrives).
    uploadwait::ConfirmState state = uploadwait::startConfirm(2097152, 0);
    uploadwait::ConfirmStep  step  = uploadwait::confirmStep(state, 2097152, Answer::Size, 1048576, 50);
    QCOMPARE(step.what, Confirm::Poll);
    QCOMPARE(step.delayMs, 300);
    QCOMPARE(state.lastSize, qint64(1048576));
    QCOMPARE(state.grewAt, qint64(0)); // the first size is no growth

    step = uploadwait::confirmStep(state, 2097152, Answer::Size, 1572864, 400);
    QCOMPARE(step.what, Confirm::Poll);
    QCOMPARE(step.delayMs, 300);
    QCOMPARE(state.grewAt, qint64(400));
    step = uploadwait::confirmStep(state, 2097152, Answer::Size, 1572864, 750);
    QCOMPARE(step.what, Confirm::Poll);
    QCOMPARE(step.delayMs, 500);
    QCOMPARE(state.grewAt, qint64(400)); // no growth
    step = uploadwait::confirmStep(state, 2097152, Answer::Size, 2097152, 1300);
    QCOMPARE(step.what, Confirm::Confirmed);
}

void TestUploadWait::largerMissingRefused()
{
    uploadwait::ConfirmState state = uploadwait::startConfirm(1000, 0);
    QCOMPARE(uploadwait::confirmStep(state, 1000, Answer::Size, 1001, 10).what, Confirm::Larger);
    QCOMPARE(uploadwait::confirmStep(state, 1000, Answer::Missing, 0, 10).what, Confirm::Missing);
    QCOMPARE(uploadwait::confirmStep(state, 1000, Answer::Refused, 0, 10).what, Confirm::Refused);
    // A size of 0 is a size (the upload has just begun arriving), not an error.
    QCOMPARE(uploadwait::confirmStep(state, 1000, Answer::Size, 0, 10).what, Confirm::Poll);
}

void TestUploadWait::stallsWithoutGrowth()
{
    uploadwait::ConfirmState state = uploadwait::startConfirm(100 * 1024 * 1024, 0); // a long deadline
    qint64                   now   = 0;
    quint64                  size  = 0;
    // Grows for 10 s, then stops.
    while (now < 10000) {
        size += 1024 * 1024;
        const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, 100 * 1024 * 1024, Answer::Size, size, now);
        QCOMPARE(step.what, Confirm::Poll);
        now += step.delayMs;
    }
    const qint64 grewAt = state.grewAt;
    for (;;) {
        const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, 100 * 1024 * 1024, Answer::Size, size, now);
        if (step.what != Confirm::Poll) {
            QCOMPARE(step.what, Confirm::Stalled);
            break;
        }
        QVERIFY(step.delayMs <= uploadwait::kMaxPollMs);
        now += step.delayMs;
        QVERIFY(now < 200000);
    }
    QVERIFY(now - grewAt >= uploadwait::kConfirmStallMs);
    QVERIFY(now - grewAt < uploadwait::kConfirmStallMs + uploadwait::kMaxPollMs);
}

void TestUploadWait::noAnswersStall()
{
    // A server that never answers (or keeps flooding us): the message goes after a minute.
    uploadwait::ConfirmState state = uploadwait::startConfirm(1000, 0);
    qint64                   now   = 0;
    int                      asked = 0;
    for (;;) {
        const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, 1000, Answer::None, 0, now);
        if (step.what != Confirm::Poll) {
            QVERIFY(step.what == Confirm::Stalled || step.what == Confirm::TimedOut);
            break;
        }
        ++asked;
        now += step.delayMs + 5000; // each question waited for its answer's timeout
    }
    QVERIFY(now >= uploadwait::kConfirmStallMs);
    QVERIFY(asked >= 5);
}

void TestUploadWait::timesOutWhileStillGrowing()
{
    // A slow trickle that keeps growing: the time limit ends it (60 s + the file at 100 KB/s).
    const quint64            total = 1024 * 1024;
    uploadwait::ConfirmState state = uploadwait::startConfirm(total, 0);
    qint64                   now   = 0;
    quint64                  size  = 0;
    for (;;) {
        size                                = qMin(total - 1, size + 1024);
        const uploadwait::ConfirmStep step  = uploadwait::confirmStep(state, total, Answer::Size, size, now);
        if (step.what != Confirm::Poll) {
            QCOMPARE(step.what, Confirm::TimedOut);
            break;
        }
        now += step.delayMs;
    }
    QVERIFY(now >= uploadwait::confirmTimeoutMs(total));
    QVERIFY(now < uploadwait::confirmTimeoutMs(total) + uploadwait::kMaxPollMs);
}

void TestUploadWait::previewKeepsTheDeadline()
{
    uploadwait::ConfirmState state = uploadwait::startConfirm(5000, 0);
    QCOMPARE(uploadwait::confirmStep(state, 5000, Answer::Size, 100, 10).what, Confirm::Poll);
    QCOMPARE(uploadwait::confirmStep(state, 5000, Answer::Size, 5000, 320).what, Confirm::Confirmed);
    const qint64 deadline = state.deadline;
    uploadwait::nextConfirmFile(state, 330);
    QCOMPARE(state.deadline, deadline);
    QCOMPARE(state.lastSize, qint64(-1));
    QCOMPARE(state.asked, 0);
    QCOMPARE(state.grewAt, qint64(330));
    const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, 2000, Answer::Size, 1000, 340);
    QCOMPARE(step.what, Confirm::Poll);
    QCOMPARE(step.delayMs, 300); // the preview's own questions start over
    QCOMPARE(uploadwait::confirmStep(state, 2000, Answer::Size, 2000, 650).what, Confirm::Confirmed);
}

void TestUploadWait::senderRaceScenario()
{
    // The field report: 2 MB, TeamSpeak says "complete" with 1.2 MB still on its way at 400 KB/s. The
    // message waits ~3 s instead of going out at once; nothing waits longer than needed by much.
    const quint64            total   = 2097152;
    const qint64             inFlight = 1200 * 1024;
    const double             rate     = 400.0 * 1024 / 1000; // bytes per ms
    uploadwait::ConfirmState state    = uploadwait::startConfirm(total, 0);
    qint64                   now      = 0;
    int                      asked    = 0;
    for (;;) {
        const qint64  arrived = qMin<qint64>(inFlight, static_cast<qint64>(rate * static_cast<double>(now)));
        const quint64 server  = total - static_cast<quint64>(inFlight - arrived);
        ++asked;
        const uploadwait::ConfirmStep step = uploadwait::confirmStep(state, total, Answer::Size, server, now);
        if (step.what == Confirm::Confirmed)
            break;
        QCOMPARE(step.what, Confirm::Poll);
        now += step.delayMs + 40; // the round trip
    }
    const qint64 needed = static_cast<qint64>(static_cast<double>(inFlight) / rate);
    QVERIFY2(now >= needed, qPrintable(QString::number(now)));
    QVERIFY2(now <= needed + uploadwait::kMaxPollMs + 40, qPrintable(QString::number(now)));
    QVERIFY(asked <= 10); // each question costs flood points
}

// ---- receiver -------------------------------------------------------------------------------------

void TestUploadWait::refetchDelaysBackOff()
{
    const QList<int> expected{2000, 4000, 8000, 15000, 30000, 30000, 30000};
    for (int i = 0; i < expected.size(); ++i)
        QCOMPARE(uploadwait::refetchDelayMs(i), expected.at(i));
    QCOMPARE(uploadwait::refetchDelayMs(-3), 2000);
}

void TestUploadWait::shortWaitsForTheServer()
{
    // The download brought half; the server has 60 % now: wait, ask again, download once it has it all.
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Short, 1000000, 500000, {}, Answer::Size, 600000, 0);
    QCOMPARE(step.next, Next::Poll);
    QCOMPARE(step.delayMs, 2000);
    QCOMPARE(state.downloads, 0);
    step = uploadwait::nextStep(state, Seen::Poll, 1000000, 0, {}, Answer::Size, 800000, 2000);
    QCOMPARE(step.next, Next::Poll);
    QCOMPARE(step.delayMs, 4000);
    QCOMPARE(state.grewAt, qint64(2000));
    step = uploadwait::nextStep(state, Seen::Poll, 1000000, 0, {}, Answer::Size, 1000000, 6000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 0); // it is all there: at once
    QCOMPARE(state.downloads, 1);
}

void TestUploadWait::shortWithWholeFileDownloadsSoon()
{
    // Short, but by the time the server is asked it has all of it.
    uploadwait::RefetchState      state;
    const uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::Short, 1000000, 400000, {}, Answer::Size, 1000000, 0);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, uploadwait::kSoonMs);
    QVERIFY(!step.confirmed);
}

void TestUploadWait::waitingGivesUpWithoutProgress()
{
    // The sender's upload stopped short (they left): no growth for three minutes, and it fails.
    uploadwait::RefetchState state;
    qint64                   now  = 0;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Short, 1000000, 300000, {}, Answer::Size, 300000, now);
    int                      polls = 0;
    while (step.next == Next::Poll) {
        ++polls;
        now += step.delayMs;
        step = uploadwait::nextStep(state, Seen::Poll, 1000000, 0, {}, Answer::Size, 300000, now);
    }
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(now >= uploadwait::kNoProgressMs);
    QVERIFY(now < uploadwait::kNoProgressMs + 30000);
    QVERIFY(polls <= 10);
    QCOMPARE(state.downloads, 0); // never downloaded again for nothing

    // Growth keeps it waiting past three minutes.
    uploadwait::RefetchState growing;
    now                = 0;
    quint64 serverSize = 100000;
    step               = uploadwait::nextStep(growing, Seen::Short, 10000000, 100000, {}, Answer::Size, serverSize, now);
    while (step.next == Next::Poll && now < 10 * 60 * 1000) {
        now += step.delayMs;
        serverSize += 20000;
        step = uploadwait::nextStep(growing, Seen::Poll, 10000000, 0, {}, Answer::Size, serverSize, now);
    }
    QCOMPARE(step.next, Next::Poll);
    QVERIFY(now >= 10 * 60 * 1000);
}

void TestUploadWait::missingAndChanged()
{
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::Short, 1000, 500, {}, Answer::Missing, 0, 0).next, Next::FailMissing);
    uploadwait::RefetchState other;
    QCOMPARE(uploadwait::nextStep(other, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 2000, 0).next, Next::FailChanged);
    uploadwait::RefetchState polling;
    QCOMPARE(uploadwait::nextStep(polling, Seen::Short, 1000, 500, {}, Answer::Size, 600, 0).next, Next::Poll);
    QCOMPARE(uploadwait::nextStep(polling, Seen::Poll, 1000, 0, {}, Answer::Missing, 0, 2000).next, Next::FailMissing); // deleted meanwhile
}

void TestUploadWait::mismatchNeedsTheSameDigestTwice()
{
    // A made-up hash (or a replaced file): the server has the whole file, two downloads agree.
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 0);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, uploadwait::kSoonMs);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 3000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(step.confirmed);
    QCOMPARE(state.downloads, 1);
}

void TestUploadWait::mismatchWithChangingDigests()
{
    // The first download raced the upload, the second didn't match either but got other bytes: one more.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 0).next, Next::Download);
    uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, Answer::Size, 1000, 3000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, Answer::Size, 1000, 6000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(step.confirmed);
}

void TestUploadWait::mismatchWhileArrivingDoesNotCount()
{
    // The server still has less than the link names: the wrong bytes say nothing about the file yet.
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 700, 0);
    QCOMPARE(step.next, Next::Poll);
    QVERIFY(state.lastDigest.isEmpty());
    step = uploadwait::nextStep(state, Seen::Poll, 1000, 0, {}, Answer::Size, 1000, 2000);
    QCOMPARE(step.next, Next::Download);
    // The first complete download after that doesn't match either: still one more before it counts.
    step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 3000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 6000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(step.confirmed);
}

void TestUploadWait::serverCantTell()
{
    // No permission to look at files: the next download has to tell.
    uploadwait::RefetchState      shortOne;
    const uploadwait::RefetchStep first = uploadwait::nextStep(shortOne, Seen::Short, 1000, 400, {}, Answer::Refused, 0, 0);
    QCOMPARE(first.next, Next::Download);
    QCOMPARE(first.delayMs, 2000);
    const uploadwait::RefetchStep second = uploadwait::nextStep(shortOne, Seen::Short, 1000, 600, {}, Answer::Refused, 0, 3000);
    QCOMPARE(second.next, Next::Download); // it brought more: the upload is still arriving
    QCOMPARE(second.delayMs, 4000);
    QVERIFY(!shortOne.afterFull);

    uploadwait::RefetchState polling;
    QCOMPARE(uploadwait::nextStep(polling, Seen::Short, 1000, 400, {}, Answer::Size, 500, 0).next, Next::Poll);
    const uploadwait::RefetchStep poll = uploadwait::nextStep(polling, Seen::Poll, 1000, 0, {}, Answer::Refused, 0, 2000);
    QCOMPARE(poll.next, Next::Download);
    QCOMPARE(poll.delayMs, 0);
}

void TestUploadWait::noAnswerAsksAgain()
{
    // Flooded or timed out: the same question again, never a download while the server was short.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::Short, 1000, 400, {}, Answer::Size, 500, 0).next, Next::Poll);
    for (int i = 1; i < uploadwait::kMaxNoAnswers; ++i) {
        const uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::Poll, 1000, 0, {}, Answer::None, 0, i * 3000);
        QCOMPARE(step.next, Next::Ask);
        QCOMPARE(step.delayMs, uploadwait::kSoonMs);
        QCOMPARE(state.downloads, 0);
    }
    // An answer in between starts the count again.
    QCOMPARE(uploadwait::nextStep(state, Seen::Poll, 1000, 0, {}, Answer::Size, 600, 10000).next, Next::Poll);
    QCOMPARE(state.noAnswers, 0);
    for (int i = 1; i < uploadwait::kMaxNoAnswers; ++i)
        QCOMPARE(uploadwait::nextStep(state, Seen::Poll, 1000, 0, {}, Answer::None, 0, 10000 + i * 3000).next, Next::Ask);
    // Never an answer: the server can't tell, the next download has to.
    const uploadwait::RefetchStep last = uploadwait::nextStep(state, Seen::Poll, 1000, 0, {}, Answer::None, 0, 20000);
    QCOMPARE(last.next, Next::Download);
    QCOMPARE(state.downloads, 1);

    // What a download showed waits for the real answer: asking again doesn't count it twice.
    uploadwait::RefetchState shortOne;
    QCOMPARE(uploadwait::nextStep(shortOne, Seen::Short, 1000, 400, {}, Answer::None, 0, 0).next, Next::Ask);
    QCOMPARE(shortOne.longest, quint64(0));
    const uploadwait::RefetchStep answered = uploadwait::nextStep(shortOne, Seen::Short, 1000, 400, {}, Answer::Size, 1000, 2000);
    QCOMPARE(answered.next, Next::Download);
    QCOMPARE(shortOne.longest, quint64(400));
    QCOMPARE(shortOne.flat, 0);
}

void TestUploadWait::cantTellFlatDownloadsEnd()
{
    // The server can't tell and the downloads stop growing (the upload was cut off): two downloads that
    // bring nothing new end it instead of eight full ones.
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Short, 2000000, 1000000, {}, Answer::Refused, 0, 0);
    QCOMPARE(step.next, Next::Download);
    step = uploadwait::nextStep(state, Seen::Short, 2000000, 1000000, {}, Answer::Refused, 0, 3000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(state.flat, 1);
    step = uploadwait::nextStep(state, Seen::Short, 2000000, 1000000, {}, Answer::Refused, 0, 8000);
    QCOMPARE(step.next, Next::FailIncomplete);
    QCOMPARE(state.downloads, uploadwait::kMaxFlatDownloads);
    QCOMPARE(state.longest, quint64(1000000)); // the size it stopped at (Core notes it)

    // Any growth starts the count again.
    uploadwait::RefetchState growing;
    QCOMPARE(uploadwait::nextStep(growing, Seen::Short, 2000000, 1000000, {}, Answer::Refused, 0, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(growing, Seen::Short, 2000000, 1000000, {}, Answer::Refused, 0, 3000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(growing, Seen::Short, 2000000, 1500000, {}, Answer::Refused, 0, 8000).next, Next::Download);
    QCOMPARE(growing.flat, 0);
}

void TestUploadWait::cantTellMismatchIsNeverConfirmed()
{
    // The server can't tell whether it has the whole file: the same wrong bytes twice only show that
    // nothing changes (a stalled upload, or a made-up hash). It ends, but is never noted.
    for (const Answer answer : {Answer::Refused, Answer::None}) {
        uploadwait::RefetchState state;
        qint64                   now  = 0;
        uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, answer, 0, now);
        while (step.next == Next::Ask) { // no answer: asked again (a few times) before it counts as can't tell
            now += step.delayMs;
            step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, answer, 0, now);
        }
        QCOMPARE(step.next, Next::Download);
        QCOMPARE(step.delayMs, 2000);
        QVERIFY(!state.afterFull);
        now += step.delayMs;
        step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, answer, 0, now);
        while (step.next == Next::Ask) {
            now += step.delayMs;
            step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, answer, 0, now);
        }
        QCOMPARE(step.next, Next::Download); // other bytes: something is still changing
        QCOMPARE(step.delayMs, 4000);        // after a longer pause
        now += step.delayMs;
        step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, answer, 0, now);
        while (step.next == Next::Ask) {
            now += step.delayMs;
            step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestB, answer, 0, now);
        }
        QCOMPARE(step.next, Next::FailMismatch);
        QVERIFY(!step.confirmed);
    }
}

void TestUploadWait::confirmedOnlyAfterTheWholeFile()
{
    // The first download went while the server couldn't tell; the second gives the same bytes and now
    // the server reports the whole file. That download didn't go after a whole-file answer: one more.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Refused, 0, 0).next, Next::Download);
    uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 3000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(state.afterFull);
    step = uploadwait::nextStep(state, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 9000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(step.confirmed);

    // A download after a whole-file answer, but the server can't tell afterwards: not confirmed.
    uploadwait::RefetchState other;
    QCOMPARE(uploadwait::nextStep(other, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Size, 1000, 0).next, Next::Download);
    QVERIFY(other.afterFull);
    step = uploadwait::nextStep(other, Seen::Mismatch, 1000, 1000, kDigestA, Answer::Refused, 0, 3000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(!step.confirmed);
}

void TestUploadWait::downloadsAreBounded()
{
    // A server that keeps handing out short files although it says it has them all: two downloads that
    // bring nothing new end it.
    uploadwait::RefetchState state;
    qint64                   now = 0;
    int                      downloads = 0;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Short, 1000, 500, {}, Answer::Size, 1000, now);
    while (step.next == Next::Download) {
        ++downloads;
        now += step.delayMs + 100;
        step = uploadwait::nextStep(state, Seen::Short, 1000, 500, {}, Answer::Size, 1000, now);
    }
    QCOMPARE(step.next, Next::FailIncomplete);
    QCOMPARE(downloads, uploadwait::kMaxFlatDownloads);

    // ... and when each brings a little more: kMaxDownloads at most.
    uploadwait::RefetchState creeping;
    now        = 0;
    downloads  = 0;
    quint64 got = 500;
    step        = uploadwait::nextStep(creeping, Seen::Short, 1000, got, {}, Answer::Size, 1000, now);
    while (step.next == Next::Download) {
        ++downloads;
        now += step.delayMs + 100;
        got += 10;
        step = uploadwait::nextStep(creeping, Seen::Short, 1000, got, {}, Answer::Size, 1000, now);
    }
    QCOMPARE(step.next, Next::FailIncomplete);
    QCOMPARE(downloads, uploadwait::kMaxDownloads);

    // Ever new wrong digests: a mismatch in the end, but not one to note (never the same twice).
    uploadwait::RefetchState changing;
    downloads = 0;
    char seed = 'd';
    step      = uploadwait::nextStep(changing, Seen::Mismatch, 1000, 1000, QByteArray(32, seed), Answer::Size, 1000, 0);
    while (step.next == Next::Download) {
        ++downloads;
        ++seed;
        step = uploadwait::nextStep(changing, Seen::Mismatch, 1000, 1000, QByteArray(32, seed), Answer::Size, 1000, downloads * 3000);
    }
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(!step.confirmed);
    QCOMPARE(downloads, uploadwait::kMaxDownloads);
}

void TestUploadWait::longerDownloadsCountAsProgress()
{
    // Without the server's size, longer downloads are the progress: past three minutes while they grow.
    uploadwait::RefetchState state;
    qint64                   now        = 0;
    quint64                  downloaded = 100;
    uploadwait::RefetchStep  step       = uploadwait::nextStep(state, Seen::Short, 1000000, downloaded, {}, Answer::Refused, 0, now);
    QCOMPARE(step.next, Next::Download);
    const qint64 first = state.grewAt;
    now += 170000;
    downloaded += 1000;
    step = uploadwait::nextStep(state, Seen::Short, 1000000, downloaded, {}, Answer::Refused, 0, now);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(state.grewAt > first);
    now += 170000; // the same length again, three minutes after the last growth
    step = uploadwait::nextStep(state, Seen::Short, 1000000, downloaded, {}, Answer::Refused, 0, now + 20000);
    QCOMPARE(step.next, Next::FailIncomplete);
}

void TestUploadWait::receiverRaceScenario()
{
    // The field report from the receiver's side: the message comes at once, the first download stalls
    // short (the server had 1 MB of 2 MB). The server gets the rest within 3 s: one more download.
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::Short, 2097152, 1048576, {}, Answer::Size, 1500000, 0);
    QCOMPARE(step.next, Next::Poll);
    qint64 now = step.delayMs;
    step       = uploadwait::nextStep(state, Seen::Poll, 2097152, 0, {}, Answer::Size, 2097152, now);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(state.downloads, 1);
    // ... and the same for a download that came out complete but wrong (the 2.2 mismatch).
    uploadwait::RefetchState mismatch;
    step = uploadwait::nextStep(mismatch, Seen::Mismatch, 2097152, 2097152, kDigestC, Answer::Size, 1800000, 0);
    QCOMPARE(step.next, Next::Poll);
    step = uploadwait::nextStep(mismatch, Seen::Poll, 2097152, 0, {}, Answer::Size, 2097152, 2000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(mismatch.downloads, 1);
    QVERIFY(mismatch.lastDigest.isEmpty()); // the racing digest is never the one that counts
}

// ---- sender, preallocating servers: the content check -----------------------------------------------

void TestUploadWait::tailLengthsAndDelays()
{
    QCOMPARE(uploadwait::tailLength(0), quint64(0));
    QCOMPARE(uploadwait::tailLength(1), quint64(1));
    QCOMPARE(uploadwait::tailLength(65535), quint64(65535));
    QCOMPARE(uploadwait::tailLength(65536), quint64(65536));
    QCOMPARE(uploadwait::tailLength(65537), quint64(65536));
    QCOMPARE(uploadwait::tailLength(Q_UINT64_C(10) * 1024 * 1024), quint64(uploadwait::kTailBytes));

    const QList<int> expected{500, 1000, 2000, 3000, 5000, 8000, 8000, 8000};
    for (int i = 0; i < expected.size(); ++i)
        QCOMPARE(uploadwait::contentRetryDelayMs(i), expected.at(i));
    QCOMPARE(uploadwait::contentRetryDelayMs(-2), 500);

    // A tail is small: the fixed limit. A whole file: plus its download at 100 KB/s (16 MB at most).
    QCOMPARE(uploadwait::contentTransferTimeoutMs(Method::Tail, Q_UINT64_C(1) << 30), uploadwait::kContentTransferTimeoutMs);
    QCOMPARE(uploadwait::contentTransferTimeoutMs(Method::Full, 1024 * 1024), uploadwait::kContentTransferTimeoutMs + 10240);
    QCOMPARE(uploadwait::contentTransferTimeoutMs(Method::Full, uploadwait::kMaxFullCheckBytes), uploadwait::kContentTransferTimeoutMs + 163840);
    QCOMPARE(uploadwait::contentTransferTimeoutMs(Method::Full, Q_UINT64_C(1) << 30), uploadwait::kContentTransferTimeoutMs + 163840);
}

void TestUploadWait::firstMethodChoices()
{
    // The uploaded tail is known: its tail, whatever the size.
    QCOMPARE(uploadwait::firstMethod(true, false, Q_UINT64_C(4) << 30), Method::Tail);
    QCOMPARE(uploadwait::firstMethod(true, true, 100), Method::Tail);
    // Not known (the staged file and the cache copy are gone): the whole file, up to 16 MB, with a checksum.
    QCOMPARE(uploadwait::firstMethod(false, true, uploadwait::kMaxFullCheckBytes), Method::Full);
    QCOMPARE(uploadwait::firstMethod(false, true, uploadwait::kMaxFullCheckBytes + 1), Method::SizeOnly);
    QCOMPARE(uploadwait::firstMethod(false, false, 100), Method::SizeOnly);
    QCOMPARE(uploadwait::methodName(Method::Tail), QStringLiteral("tail"));
    QCOMPARE(uploadwait::methodName(Method::Full), QStringLiteral("full"));
    QCOMPARE(uploadwait::methodName(Method::SizeOnly), QStringLiteral("size only"));
}

void TestUploadWait::compareTail_data()
{
    QTest::addColumn<QByteArray>("expected");
    QTest::addColumn<quint64>("fileSize");
    QTest::addColumn<qint64>("downloaded");
    QTest::addColumn<QByteArray>("got");
    QTest::addColumn<int>("bytes");

    const QByteArray tail = pattern(65536, 3);
    const quint64    size = 1024 * 1024;
    QByteArray       half = tail;
    half.replace(32768, 32768, QByteArray(32768, '\0'));
    QByteArray oneByte = tail;
    oneByte[65535]     = static_cast<char>(oneByte.at(65535) ^ 0x01);
    QTest::newRow("the uploaded bytes") << tail << size << qint64(size) << tail << int(Bytes::Same);
    QTest::newRow("zeros (still arriving)") << tail << size << qint64(size) << QByteArray(65536, '\0') << int(Bytes::Different);
    QTest::newRow("half arrived") << tail << size << qint64(size) << half << int(Bytes::Different);
    // Other bytes that aren't zeros: not the file's end (a preallocating server's missing bytes are zeros).
    QTest::newRow("one byte differs") << tail << size << qint64(size) << oneByte << int(Bytes::NotTheEnd);
    QTest::newRow("the file's beginning (offset ignored)") << tail << size << qint64(size) << pattern(65536, 0) << int(Bytes::NotTheEnd);
    QTest::newRow("whole file appended (no seek)") << tail << size << qint64(2 * size - 65536) << tail << int(Bytes::WrongLength);
    QTest::newRow("nothing came") << tail << size << qint64(size - 65536) << QByteArray() << int(Bytes::WrongLength);
    // A server that sends what it has so far (not preallocating, the size unknown): still arriving.
    QTest::newRow("some came, not all") << tail << size << qint64(size - 1000) << pattern(65536, 5) << int(Bytes::Different);
    QTest::newRow("no file") << tail << size << qint64(-1) << QByteArray() << int(Bytes::WrongLength);
    const QByteArray small = pattern(1000, 4);
    QTest::newRow("a small file whole") << small << quint64(1000) << qint64(1000) << small << int(Bytes::Same);

    // A file whose own end is zeros (its window: 64 KB of data and its 10000 zeros).
    const QByteArray zeroEnded = pattern(65536, 6) + QByteArray(10000, '\0');
    QTest::newRow("own zeros, all there") << zeroEnded << size << qint64(size) << zeroEnded << int(Bytes::Same);
    QTest::newRow("own zeros, data missing") << zeroEnded << size << qint64(size) << preallocated(zeroEnded, 60000) << int(Bytes::Different);
    QTest::newRow("own zeros, only them missing") << zeroEnded << size << qint64(size) << preallocated(zeroEnded, 65536) << int(Bytes::Same);
    QTest::newRow("own zeros, nothing there") << zeroEnded << size << qint64(size) << QByteArray(zeroEnded.size(), '\0') << int(Bytes::Different);
}

void TestUploadWait::compareTail()
{
    QFETCH(QByteArray, expected);
    QFETCH(quint64, fileSize);
    QFETCH(qint64, downloaded);
    QFETCH(QByteArray, got);
    QFETCH(int, bytes);
    QCOMPARE(int(uploadwait::compareTail(expected, fileSize, downloaded, got)), bytes);
}

void TestUploadWait::contentConfirmedAtOnce()
{
    // The official server (and a TeaSpeak server that has it all): one check, confirmed.
    uploadwait::ContentState      state = uploadwait::startContent(Method::Tail, 1000);
    const uploadwait::ContentStep step  = uploadwait::contentStep(state, Bytes::Same, kDigestA, true, 2097152, 61000, 1060);
    QCOMPARE(step.what, Content::Confirmed);
    QCOMPARE(state.retries, 0);
    QCOMPARE(uploadwait::contentName(Content::Confirmed), QStringLiteral("confirmed"));
}

void TestUploadWait::contentRetriesWhileZeros()
{
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    uploadwait::ContentStep  step  = uploadwait::contentStep(state, Bytes::Different, kDigestA, true, 1000000, 60000, 100);
    QCOMPARE(step.what, Content::Retry);
    QCOMPARE(step.delayMs, 500);
    QCOMPARE(state.seen, kDigestA);
    QCOMPARE(state.changedAt, qint64(0)); // the first sight is no change
    step = uploadwait::contentStep(state, Bytes::Different, kDigestB, true, 1000000, 60000, 700);
    QCOMPARE(step.what, Content::Retry);
    QCOMPARE(step.delayMs, 1000);
    QCOMPARE(state.changedAt, qint64(700)); // more of it arrived
    step = uploadwait::contentStep(state, Bytes::Different, kDigestB, true, 1000000, 60000, 1800);
    QCOMPARE(step.what, Content::Retry);
    QCOMPARE(step.delayMs, 2000);
    QCOMPARE(state.changedAt, qint64(700));
    QCOMPARE(uploadwait::contentStep(state, Bytes::Same, kDigestC, true, 1000000, 60000, 3900).what, Content::Confirmed);
    QCOMPARE(uploadwait::contentName(Content::Retry), QStringLiteral("still arriving"));
}

void TestUploadWait::contentFallsBackToTheWholeFile()
{
    // A server that can't send the end alone: the whole file (up to 16 MB, it has a checksum).
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    uploadwait::ContentStep  step  = uploadwait::contentStep(state, Bytes::WrongLength, {}, true, 1000000, 60000, 100);
    QCOMPARE(step.what, Content::Full);
    QCOMPARE(step.delayMs, 0);
    QCOMPARE(state.method, Method::Full);
    // A whole file of another length is other bytes (still arriving), not another fallback.
    step = uploadwait::contentStep(state, Bytes::WrongLength, QByteArray::number(600000), true, 1000000, 60000, 300);
    QCOMPARE(step.what, Content::Retry);
    QCOMPARE(state.method, Method::Full);
    step = uploadwait::contentStep(state, Bytes::Different, kDigestA, true, 1000000, 60000, 900);
    QCOMPARE(step.what, Content::Retry);
    QCOMPARE(uploadwait::contentStep(state, Bytes::Same, kDigestB, true, 1000000, 60000, 2000).what, Content::Confirmed);

    // The tail's download failed (a permission, an error): the same fallback.
    uploadwait::ContentState failed = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(failed, Bytes::Failed, {}, true, 5000, 60000, 100).what, Content::Full);
    QCOMPARE(failed.method, Method::Full);
    // ... and when the whole file's download fails too: not possible.
    QCOMPARE(uploadwait::contentStep(failed, Bytes::Failed, {}, true, 5000, 60000, 200).what, Content::SizeOnly);
    QCOMPARE(failed.method, Method::SizeOnly);
}

void TestUploadWait::contentNotPossible()
{
    // No checksum to compare a whole file with, or too large to download whole: after the size check.
    uploadwait::ContentState noHash = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(noHash, Bytes::WrongLength, {}, false, 1000, 60000, 100).what, Content::SizeOnly);
    QCOMPARE(noHash.method, Method::SizeOnly);
    uploadwait::ContentState large = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(large, Bytes::Failed, {}, true, uploadwait::kMaxFullCheckBytes + 1, 60000, 100).what, Content::SizeOnly);
    QCOMPARE(uploadwait::contentName(Content::SizeOnly), QStringLiteral("not possible"));
}

void TestUploadWait::contentTimesOut()
{
    // Still other (but changing) bytes at the size check's deadline: posted anyway.
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    qint64                   now   = 0;
    int                      seed  = 0;
    for (;;) {
        const uploadwait::ContentStep step = uploadwait::contentStep(state, Bytes::Different, QByteArray(32, static_cast<char>('a' + (seed++ % 20))), true, 1000000, 10000, now);
        if (step.what != Content::Retry) {
            QCOMPARE(step.what, Content::TimedOut);
            break;
        }
        now += step.delayMs + 60;
    }
    QVERIFY(now >= 10000);
    QVERIFY(now < 10000 + 8000 + 60);
    QCOMPARE(uploadwait::contentName(Content::TimedOut), QStringLiteral("timed out"));
}

void TestUploadWait::contentStallsWithoutChange()
{
    // The same zeros for a minute (the upload stopped arriving): posted anyway, long before the deadline.
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    qint64                   now   = 0;
    for (;;) {
        const uploadwait::ContentStep step = uploadwait::contentStep(state, Bytes::Different, kDigestA, true, 100000000, 900000, now);
        if (step.what != Content::Retry) {
            QCOMPARE(step.what, Content::Stalled);
            break;
        }
        QVERIFY(step.delayMs <= 8000);
        now += step.delayMs;
    }
    QVERIFY(now >= uploadwait::kConfirmStallMs);
    QVERIFY(now < uploadwait::kConfirmStallMs + 8000);

    // A change starts the minute again.
    uploadwait::ContentState changing = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(changing, Bytes::Different, kDigestA, true, 100000000, 900000, 0).what, Content::Retry);
    QCOMPARE(uploadwait::contentStep(changing, Bytes::Different, kDigestB, true, 100000000, 900000, 50000).what, Content::Retry);
    QCOMPARE(uploadwait::contentStep(changing, Bytes::Different, kDigestB, true, 100000000, 900000, 100000).what, Content::Retry);
    QCOMPARE(uploadwait::contentStep(changing, Bytes::Different, kDigestB, true, 100000000, 900000, 110000).what, Content::Stalled);
    QCOMPARE(uploadwait::contentName(Content::Stalled), QStringLiteral("stopped changing"));
}

void TestUploadWait::noAnswerDoesNotCountAsChange()
{
    // Core passes no fingerprint when a check couldn't run (flood protection, the connection): it never
    // counts as progress, so a server that never lets a check through still ends within the limits.
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    qint64                   now   = 0;
    int                      tries = 0;
    for (;;) {
        const uploadwait::ContentStep step = uploadwait::contentStep(state, Bytes::Different, {}, true, 1000, 900000, now);
        if (step.what != Content::Retry)
            break;
        ++tries;
        now += step.delayMs;
    }
    QVERIFY(now >= uploadwait::kConfirmStallMs);
    QVERIFY(tries >= 5);
    QVERIFY(state.seen.isEmpty());
}

void TestUploadWait::teaSpeakSenderScenario()
{
    // The field report on a TeaSpeak server: 2 MB, TeamSpeak says "complete" with 1.2 MB still on its way
    // at 400 KB/s. The server reports the whole size at once (preallocated), so the size check passes
    // at the first question; the tail check sees zeros until the end has arrived. The message waits
    // about as long as the upload needs, not much longer, with few checks (each costs flood points).
    const quint64            total    = 2097152;
    const qint64             inFlight = 1200 * 1024;
    const double             rate     = 400.0 * 1024 / 1000; // bytes per ms
    const auto               arrived  = [&](qint64 at) { return qMin<qint64>(inFlight, static_cast<qint64>(rate * static_cast<double>(at))); };
    uploadwait::ConfirmState size     = uploadwait::startConfirm(total, 0);
    QCOMPARE(uploadwait::confirmStep(size, total, Answer::Size, total, 40).what, Confirm::Confirmed); // what round 1 trusted

    uploadwait::ContentState state  = uploadwait::startContent(Method::Tail, 40);
    qint64                   now    = 40;
    int                      checks = 0;
    for (;;) {
        ++checks;
        now += 60; // the tail's download
        // The tail: the last 64 KB of what the server has, zeros where the bytes haven't arrived.
        const qint64     end  = static_cast<qint64>(total) - inFlight + arrived(now);
        const qint64     from = static_cast<qint64>(total) - uploadwait::kTailBytes;
        const QByteArray seen = QByteArray::number(qMax<qint64>(0, end - from));
        const Bytes      got  = end >= static_cast<qint64>(total) ? Bytes::Same : Bytes::Different;
        const uploadwait::ContentStep step = uploadwait::contentStep(state, got, seen, true, total, size.deadline, now);
        if (step.what == Content::Confirmed)
            break;
        QCOMPARE(step.what, Content::Retry);
        now += step.delayMs;
    }
    const qint64 needed = static_cast<qint64>(static_cast<double>(inFlight) / rate);
    QVERIFY2(now >= needed, qPrintable(QString::number(now)));
    QVERIFY2(now <= needed + 3000 + 200, qPrintable(QString::number(now)));
    QVERIFY2(checks <= 5, qPrintable(QString::number(checks)));
}

void TestUploadWait::tailWindows()
{
    // A file that doesn't end in zeros: its last 64 KB (all of a smaller one).
    const QByteArray data = pattern(200000, 11);
    QCOMPARE(uploadwait::tailWindow(data.right(65536), 200000), data.right(65536));
    QCOMPARE(uploadwait::tailWindow(data.right(1000), 1000), data.right(1000));
    // Its own zeros at the end: the last 64 KB of data and the zeros after them.
    const QByteArray zeroEnded = data + QByteArray(100000, '\0');
    QCOMPARE(uploadwait::tailWindow(zeroEnded.right(uploadwait::kMaxTailWindowBytes), 300000), zeroEnded.right(100000 + 65536));
    // Less data than 64 KB before the zeros: what there is.
    const QByteArray little = pattern(500, 12) + QByteArray(3000, '\0');
    QCOMPARE(uploadwait::tailWindow(little, 3500), little);
    // All zeros as far as read (the whole file, or more zeros than a window may hold): none.
    QVERIFY(uploadwait::tailWindow(QByteArray(4096, '\0'), 4096).isEmpty());
    QVERIFY(uploadwait::tailWindow(QByteArray(uploadwait::kMaxTailWindowBytes, '\0'), Q_UINT64_C(50) * 1024 * 1024).isEmpty());
    // Nothing read, or more than the file.
    QVERIFY(uploadwait::tailWindow(QByteArray(), 100).isEmpty());
    QVERIFY(uploadwait::tailWindow(data.right(2000), 1000).isEmpty());
}

void TestUploadWait::tailWindowReachesTheLastData()
{
    // A WAV ending in silence on a preallocating server: its last 64 KB are zeros, like the server's
    // copy before anything arrived. Compared there, the check would pass at once; the window reaches back
    // to the last non-zero byte, so it passes only once that has arrived.
    const QByteArray sent   = pattern(400000, 13) + QByteArray(80000, '\0');
    const quint64    size   = static_cast<quint64>(sent.size());
    const QByteArray naive  = sent.right(65536);
    const QByteArray window = uploadwait::tailWindow(sent.right(uploadwait::kMaxTailWindowBytes), size);
    QCOMPARE(window.size(), 80000 + 65536);
    QCOMPARE(uploadwait::trailingZeros(naive), naive.size());
    for (const int arrived : {0, 100000, 350000, 399999}) {
        const QByteArray server = preallocated(sent, arrived);
        QCOMPARE(uploadwait::compareTail(naive, size, static_cast<qint64>(size), server.right(naive.size())), Bytes::Same); // what went wrong
        QCOMPARE(uploadwait::compareTail(window, size, static_cast<qint64>(size), server.right(window.size())), Bytes::Different);
    }
    for (const int arrived : {400000, 440000, 480000})
        QCOMPARE(uploadwait::compareTail(window, size, static_cast<qint64>(size), preallocated(sent, arrived).right(window.size())), Bytes::Same);
}

void TestUploadWait::otherBytesFallBackToTheWholeFile()
{
    // A tail of the right length with other bytes (the server sent the file's beginning): never "still
    // arriving" (that would wait a minute for nothing), the whole file is checked instead.
    uploadwait::ContentState state = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(state, Bytes::NotTheEnd, kDigestA, true, 1000000, 60000, 100).what, Content::Full);
    QCOMPARE(state.method, Method::Full);
    QCOMPARE(uploadwait::contentStep(state, Bytes::Same, kDigestB, true, 1000000, 60000, 300).what, Content::Confirmed);
    // Without a checksum (or too large): after the size check.
    uploadwait::ContentState noHash = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(noHash, Bytes::NotTheEnd, kDigestA, false, 1000000, 60000, 100).what, Content::SizeOnly);
}

void TestUploadWait::refusedGoesAfterTheSize()
{
    // No permission to download: the whole file would be refused too, so no second request.
    uploadwait::ContentState tail = uploadwait::startContent(Method::Tail, 0);
    QCOMPARE(uploadwait::contentStep(tail, Bytes::Refused, {}, true, 1000, 60000, 100).what, Content::SizeOnly);
    QCOMPARE(tail.method, Method::SizeOnly);
    uploadwait::ContentState full = uploadwait::startContent(Method::Full, 0);
    QCOMPARE(uploadwait::contentStep(full, Bytes::Refused, {}, true, 1000, 60000, 100).what, Content::SizeOnly);
}

// ---- receiver, preallocating servers: zero tails ------------------------------------------------------

void TestUploadWait::trailingZerosAndThresholds()
{
    QCOMPARE(uploadwait::trailingZeros(QByteArray()), 0);
    QCOMPARE(uploadwait::trailingZeros(QByteArray("abc")), 0);
    QCOMPARE(uploadwait::trailingZeros(QByteArray("abc\0\0", 5)), 2);
    QCOMPARE(uploadwait::trailingZeros(QByteArray(4, '\0')), 4);
    QCOMPARE(uploadwait::trailingZeros(QByteArray("\0\0a", 3)), 0);
    QCOMPARE(uploadwait::zeroRunThreshold(false), qint64(4096));
    QCOMPARE(uploadwait::zeroRunThreshold(true), qint64(65536));
}

void TestUploadWait::dataEndScansBackwards()
{
    QCOMPARE(dataEndOf(QByteArray()), qint64(0));
    QCOMPARE(dataEndOf(QByteArray(10000, '\0')), qint64(0));
    QCOMPARE(dataEndOf(pattern(10000)), qint64(10000));
    // The incident: valid data up to 150000 (or 225000), zeros after, across several 1 MB chunks.
    QCOMPARE(dataEndOf(preallocated(pattern(3 * 1024 * 1024 + 17), 150000)), qint64(150000));
    QCOMPARE(dataEndOf(preallocated(pattern(400000), 225000)), qint64(225000));
    // Ending right at a chunk's edge.
    QCOMPARE(dataEndOf(preallocated(pattern(2 * 1024 * 1024 + 10), 1024 * 1024)), qint64(1024 * 1024));
    QCOMPARE(dataEndOf(preallocated(pattern(2 * 1024 * 1024 + 10), 1024 * 1024 + 1)), qint64(1024 * 1024 + 1));
    // One byte of data, then zeros.
    QCOMPARE(dataEndOf(preallocated(pattern(3 * 1024 * 1024), 1)), qint64(1));
    // Zeros in the middle don't count: the data ends where the last non-zero byte is.
    QByteArray holes = pattern(300000);
    holes.replace(100000, 50000, QByteArray(50000, '\0'));
    QCOMPARE(dataEndOf(holes), qint64(300000));

    // Canceled, or a device that can't be read: unknown.
    QByteArray        bytes = preallocated(pattern(3 * 1024 * 1024), 1000);
    QBuffer           buffer(&bytes);
    std::atomic<bool> cancel{true};
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QCOMPARE(uploadwait::dataEnd(buffer, &cancel), qint64(-1));
    QBuffer closed;
    QCOMPARE(uploadwait::dataEnd(closed), qint64(-1));
}

void TestUploadWait::dataEndOfAFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/clip_1234abcd.mp4");
    QFile         file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(preallocated(pattern(400000), 225000)) == 400000);
    file.close();
    QCOMPARE(uploadwait::dataEnd(path), qint64(225000));
    QCOMPARE(uploadwait::dataEnd(dir.path() + QStringLiteral("/missing.mp4")), qint64(-1));
}

void TestUploadWait::zeroTailBoundaries()
{
    const qint64 size = 400000;
    QVERIFY(uploadwait::isZeroTail(size, size - 4096, false));
    QVERIFY(!uploadwait::isZeroTail(size, size - 4095, false)); // a picture never ends in 4 KB of zeros; less is data
    QVERIFY(!uploadwait::isZeroTail(size, size - 65535, true)); // videos and audio files may end in shorter runs
    QVERIFY(uploadwait::isZeroTail(size, size - 65536, true));
    QVERIFY(uploadwait::isZeroTail(size, 0, false));             // nothing has arrived yet
    QVERIFY(!uploadwait::isZeroTail(3000, 0, false));            // too small for the run
    QVERIFY(!uploadwait::isZeroTail(size, -1, false));           // not known
    QVERIFY(!uploadwait::isZeroTail(size, size + 1, false));
    QVERIFY(!uploadwait::isZeroTail(0, 0, false));
    QVERIFY(!uploadwait::isZeroTail(-1, -1, true));
}

void TestUploadWait::zeroTailWaitsAndDownloadsAgain()
{
    // A TeaSpeak server reports the whole size although the end hasn't arrived: its size says nothing,
    // the download is fetched again after a pause (2, 4, 8 s ...).
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 2000);
    QVERIFY(!step.confirmed);
    QCOMPARE(state.downloads, 1);
    QCOMPARE(state.longest, quint64(150000));
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 300000, {}, Answer::Size, 400000, 3000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 4000);
    QCOMPARE(state.grewAt, qint64(3000)); // the data before the zeros grew: progress
    QCOMPARE(state.flat, 0);
    QVERIFY(state.lastDigest.isEmpty());
}

void TestUploadWait::zeroTailIsNeverAMismatch()
{
    // The same zero tail again and again (the sender's upload stopped): downloads that bring nothing new
    // end it once nothing grew for kZeroTailStallMs, as an incomplete upload, retryable, never as a
    // confirmed mismatch. Two flat downloads alone don't (the bytes may stand still for a while).
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0).next, Next::Download);
    uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 3000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(state.flat, 1);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 8000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 8000);
    QCOMPARE(state.flat, 2);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 17000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 15000);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 33000);
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(!step.confirmed);
    QCOMPARE(state.downloads, 4);
    QVERIFY(state.lastDigest.isEmpty());

    // Growth in between starts the time over.
    uploadwait::RefetchState resumed;
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 3000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 8000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 250000, {}, Answer::Size, 400000, 17000).next, Next::Download);
    QCOMPARE(resumed.grewAt, qint64(17000));
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 250000, {}, Answer::Size, 400000, 33000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(resumed, Seen::ZeroTail, 400000, 250000, {}, Answer::Size, 400000, 64000).next, Next::FailIncomplete);

    // Three minutes without growth end it too (spaced out further than the flat rule would).
    uploadwait::RefetchState slow;
    QCOMPARE(uploadwait::nextStep(slow, Seen::ZeroTail, 400000, 100000, {}, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(slow, Seen::ZeroTail, 400000, 200000, {}, Answer::Size, 400000, 1000).next, Next::Download);
    step = uploadwait::nextStep(slow, Seen::ZeroTail, 400000, 200000, {}, Answer::Size, 400000, 1000 + uploadwait::kNoProgressMs);
    QCOMPARE(step.next, Next::FailIncomplete);
}

void TestUploadWait::firstDataAfterAllZerosIsGrowth()
{
    // The first downloads had no data at all (all zeros, nothing had arrived): the first data after them
    // is growth, so the 25 s without growth count from there.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 0, kDigestA, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 0, kDigestA, Answer::Size, 400000, 3000).next, Next::Download);
    QCOMPARE(state.flat, 1);
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 100000, kDigestB, Answer::Size, 400000, 8000).next, Next::Download);
    QCOMPARE(state.grewAt, qint64(8000));
    QCOMPARE(state.flat, 0);
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 100000, kDigestB, Answer::Size, 400000, 17000).next, Next::Download);
    // Two flat downloads, 22 s after the growth: still waiting.
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 100000, kDigestB, Answer::Size, 400000, 30000).next, Next::Download);
    QCOMPARE(state.flat, 2);
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 100000, kDigestB, Answer::Size, 400000, 61000).next, Next::FailIncomplete);
}

void TestUploadWait::zeroTailGrowingKeepsGoing()
{
    // Each download brings more: downloaded again until kMaxDownloads (each one costs a whole file).
    uploadwait::RefetchState state;
    qint64                   now       = 0;
    quint64                  end       = 50000;
    int                      downloads = 0;
    uploadwait::RefetchStep  step      = uploadwait::nextStep(state, Seen::ZeroTail, 10000000, end, {}, Answer::Size, 10000000, now);
    while (step.next == Next::Download) {
        ++downloads;
        now += step.delayMs + 500;
        end += 100000;
        step = uploadwait::nextStep(state, Seen::ZeroTail, 10000000, end, {}, Answer::Size, 10000000, now);
    }
    QCOMPARE(step.next, Next::FailIncomplete);
    QCOMPARE(downloads, uploadwait::kMaxDownloads);
}

void TestUploadWait::zeroTailWithAShortServerPolls()
{
    // A server that reports what it has so far: waited for with questions, not downloads.
    uploadwait::RefetchState state;
    const uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 200000, 0);
    QCOMPARE(step.next, Next::Poll);
    QCOMPARE(step.delayMs, 2000);
    QCOMPARE(state.downloads, 0);
}

void TestUploadWait::zeroTailWhileTheServerCantTell()
{
    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Refused, 0, 0);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 2000);
    QVERIFY(!state.afterFull);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Refused, 0, 3000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(step.delayMs, 4000);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Refused, 0, 8000);
    QCOMPARE(step.next, Next::Download);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Refused, 0, 8000 + uploadwait::kZeroTailStallMs);
    QCOMPARE(step.next, Next::FailIncomplete);
}

void TestUploadWait::mismatchAfterZeroTailsStillNeedsTwo()
{
    // Zero tails first, then complete downloads that don't match (no zeros: a replaced file or a made-up
    // hash): a mismatch still counts only when two of them agree.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0).next, Next::Download);
    uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::Mismatch, 400000, 400000, kDigestA, Answer::Size, 400000, 3000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::Mismatch, 400000, 400000, kDigestA, Answer::Size, 400000, 8000);
    QCOMPARE(step.next, Next::FailMismatch);
    QVERIFY(step.confirmed);
}

void TestUploadWait::teaSpeakReceiverScenario()
{
    // The incident on the TeaSpeak server: receivers got the full size, valid up to 150000, then 225000,
    // zeros after. Now each such download waits and downloads again; the third one is complete.
    const QByteArray sent  = pattern(400000, 9);
    const QByteArray first = preallocated(sent, 150000);
    const QByteArray later = preallocated(sent, 225000);
    QVERIFY(uploadwait::isZeroTail(first.size(), dataEndOf(first), false));
    QVERIFY(uploadwait::isZeroTail(later.size(), dataEndOf(later), false));
    QVERIFY(!uploadwait::isZeroTail(sent.size(), dataEndOf(sent), false));

    uploadwait::RefetchState state;
    uploadwait::RefetchStep  step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, static_cast<quint64>(dataEndOf(first)), {}, Answer::Size, 400000, 0);
    QCOMPARE(step.next, Next::Download);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, static_cast<quint64>(dataEndOf(later)), {}, Answer::Size, 400000, step.delayMs + 200);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(state.downloads, 2);
    QCOMPARE(state.longest, quint64(225000));
}

void TestUploadWait::mismatchNeedsOnlyAShortZeroRun()
{
    // A download that didn't match its SHA-256 isn't the uploaded file anyway: 512 zeros at its end make
    // it an upload still arriving, whatever the kind (a 40 KB voice message, the last 3 KB of a JPEG).
    QCOMPARE(uploadwait::zeroRunThreshold(false, true), qint64(512));
    QCOMPARE(uploadwait::zeroRunThreshold(true, true), qint64(512));
    QVERIFY(uploadwait::isZeroTail(40000, 40000 - 512, true, true));
    QVERIFY(!uploadwait::isZeroTail(40000, 40000 - 511, true, true));
    QVERIFY(!uploadwait::isZeroTail(40000, 40000 - 3000, true));        // without a mismatch: 64 KB for audio
    QVERIFY(uploadwait::isZeroTail(400000, 400000 - 3000, false, true));
    QVERIFY(!uploadwait::isZeroTail(400000, 400000 - 3000, false));     // ... 4 KB for the rest
}

void TestUploadWait::stuckZeroTailIsConfirmed()
{
    // The same zero-tailed bytes three times while the server reports the whole file: the server's copy
    // stays like that. It fails as incomplete, but confirmed: Core notes that digest, so links with
    // ever new made-up hashes for this file aren't all downloaded again.
    uploadwait::RefetchState state;
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(state.lastDigest, kDigestA);
    uploadwait::RefetchStep step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Size, 400000, 3000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Size, 400000, 8000);
    QCOMPARE(step.next, Next::Download);
    QVERIFY(!step.confirmed);
    step = uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Size, 400000, 8000 + uploadwait::kZeroTailStallMs);
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(step.confirmed);

    // A question without an answer in between asks again: the digest isn't counted twice for it.
    uploadwait::RefetchState asked;
    QCOMPARE(uploadwait::nextStep(asked, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(asked, Seen::ZeroTail, 400000, 200000, kDigestB, Answer::None, 0, 3000).next, Next::Ask);
    step = uploadwait::nextStep(asked, Seen::ZeroTail, 400000, 200000, kDigestB, Answer::Size, 400000, 5000);
    QCOMPARE(step.next, Next::Download);
    QCOMPARE(asked.lastDigest, kDigestB);
}

void TestUploadWait::changingZeroTailIsNotConfirmed()
{
    // While the server can't tell, nothing is confirmed: it may still be arriving.
    uploadwait::RefetchState cantTell;
    QCOMPARE(uploadwait::nextStep(cantTell, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Refused, 0, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(cantTell, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Refused, 0, 3000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(cantTell, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Refused, 0, 8000).next, Next::Download);
    uploadwait::RefetchStep step = uploadwait::nextStep(cantTell, Seen::ZeroTail, 400000, 150000, kDigestA, Answer::Refused, 0, 8000 + uploadwait::kZeroTailStallMs);
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(!step.confirmed);

    // Growing to the download limit: other bytes every time, never confirmed.
    uploadwait::RefetchState growing;
    qint64                   now  = 0;
    quint64                  end  = 50000;
    int                      seed = 0;
    step = uploadwait::nextStep(growing, Seen::ZeroTail, 10000000, end, QByteArray(32, 'a'), Answer::Size, 10000000, now);
    while (step.next == Next::Download) {
        now += step.delayMs + 500;
        end += 100000;
        step = uploadwait::nextStep(growing, Seen::ZeroTail, 10000000, end, QByteArray(32, static_cast<char>('b' + seed++)), Answer::Size, 10000000, now);
    }
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(!step.confirmed);

    // Without a digest (a link without a sha): nothing to note.
    uploadwait::RefetchState noSha;
    QCOMPARE(uploadwait::nextStep(noSha, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(noSha, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 3000).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(noSha, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 8000).next, Next::Download);
    step = uploadwait::nextStep(noSha, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 8000 + uploadwait::kZeroTailStallMs);
    QCOMPARE(step.next, Next::FailIncomplete);
    QVERIFY(!step.confirmed);
}

void TestUploadWait::shaLessZeroTailSettles()
{
    // A link without a sha whose file really ends in zeros (a WAV's silence): the first download waits,
    // the next one that brings no more data counts once, the one after that settles it (kept as it is).
    uploadwait::RefetchState state;
    QVERIFY(!uploadwait::zeroTailSettled(state, 150000, 0)); // the first download: waited for
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 0).next, Next::Download);
    QVERIFY(!uploadwait::zeroTailSettled(state, 150000, 3000)); // flat once
    QCOMPARE(uploadwait::nextStep(state, Seen::ZeroTail, 400000, 150000, {}, Answer::Size, 400000, 3000).next, Next::Download);
    QCOMPARE(state.flat, 1);
    QVERIFY(uploadwait::zeroTailSettled(state, 150000, 8000)); // flat twice: settled
    QVERIFY(!uploadwait::zeroTailSettled(state, 150001, 8000)); // more data: still arriving

    // Growing, then three minutes without growth: settled too.
    uploadwait::RefetchState slow;
    QCOMPARE(uploadwait::nextStep(slow, Seen::ZeroTail, 400000, 100000, {}, Answer::Size, 400000, 0).next, Next::Download);
    QCOMPARE(uploadwait::nextStep(slow, Seen::ZeroTail, 400000, 200000, {}, Answer::Size, 400000, 1000).next, Next::Download);
    QVERIFY(!uploadwait::zeroTailSettled(slow, 200000, 2000));
    QVERIFY(uploadwait::zeroTailSettled(slow, 200000, 1000 + uploadwait::kNoProgressMs));
}

TSMEDIA_REGISTER_TEST(TestUploadWait)

#include "tst_uploadwait.moc"
