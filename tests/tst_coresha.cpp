// 2.2 sha, end to end inside Core: Core itself (core.cpp, ts3api.cpp, the real hashing and file system)
// runs against a fake TeamSpeak (fakets3.*). Covers what the design asks of the receiver (match ->
// cached and marked verified; mismatch -> deleted, both hashes logged, final once a second download
// agrees (2.3.2); the 2.0.5 write race re-checked once; made-up hashes refused without a download;
// previews checked against ph; cached files not hashed again) and of the sender (the posted sha is the
// uploaded bytes').
//
// 2.3.2, the upload race (uploadwait.h): the sender's message waits until the server has all of the
// file and its preview (and survives cancel and disconnect meanwhile); a receiver's download that came
// out short or didn't match waits for the upload and is downloaded again instead of failing.

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core.h"
#include "fakets3.h"
#include "fileverify.h"
#include "hashing.h"
#include "i18n.h"
#include "previewrenderer.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "uploadwait.h"

#include <windows.h> // 2.3.2: the tail check's local file is sparse

namespace {

constexpr int kCheckTimeoutMs = 8000; // a mismatch takes two passes and the 1 s pause between them

QByteArray pattern(qint64 size, int seed = 0)
{
    QByteArray data(static_cast<int>(size), Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 8) + seed) & 0xff);
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A TS Media link to remotePath on the fake server, sized like bytes, with their sha (or another one).
MediaLink linkTo(const QString& remotePath, const QByteArray& bytes, const QByteArray& sha)
{
    MediaLink link;
    link.host      = QString::fromLatin1(fakets3::kHost);
    link.port      = fakets3::kPort;
    link.serverUid = QString::fromLatin1(fakets3::kServerUid);
    link.channelId = fakets3::kChannel;
    const int slash = remotePath.lastIndexOf(QLatin1Char('/'));
    link.path      = slash > 0 ? remotePath.left(slash) : QStringLiteral("/");
    link.fileName  = remotePath.mid(slash + 1);
    link.size      = static_cast<quint64>(bytes.size());
    link.dateTime  = 1760000000;
    link.protocol  = MediaLink::kProtocol;
    link.sha256    = sha;
    // Through the text form, as a receiver gets it.
    return MediaLink::parse(link.toUrl());
}

QStringList filesUnder(const QString& dir)
{
    QStringList  files;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    return files;
}

// 2.3.2: what a preallocating server (TeaSpeak) has while an upload arrives: the whole size, the first
// `arrived` bytes, zeros after.
QByteArray zeroAfter(const QByteArray& bytes, int arrived)
{
    QByteArray file(bytes.size(), '\0');
    file.replace(0, arrived, bytes.left(arrived));
    return file;
}

int logCount(const QString& text)
{
    int count = 0;
    for (const QString& line : fakets3::logLines())
        count += line.contains(text) ? 1 : 0;
    return count;
}

bool isSparse(const QString& path)
{
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
}

} // namespace

class TestCoreSha : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // receiver
    void verifiedDownloadIsCached();
    void mismatchIsBlockedAfterTwoDownloads();
    void forgedLinkIsRefusedWithoutDownload();
    void forgedLinkFirstDoesNotPoisonGenuine();
    void writeRaceRecoversOnSecondPass();
    void unreadableFileIsRetryable();
    void connectionLossDuringCheck();
    void shutdownDuringCheckIsQuick();
    void cachedVerifiedFileIsNotHashedAgain();
    void previewChecksum_data();
    void previewChecksum();
    void linkWithoutShaIsNotChecked();
    void unknownSizeLinkIsChecked();
    void checkingShownOnlyWhenSlow();

    // sender
    void sentLinkCarriesShaOfStagedBytes();

    // 2.3.2: the upload race, sender
    void messageWaitsForTheWholeFile();
    void previewIsConfirmedToo();
    void previewNotAskedWhenTheFileIsThere();
    void cancelWhileTheServerCatchesUp();
    void disconnectWhileTheServerCatchesUp();
    void repostWaitsOnlyForItsOwnFile();
    void serverThatCantTellGetsTheMessageAnyway();
    // 2.3.2: the upload race, receiver
    void shortDownloadWaitsForTheUpload();
    void announcedShortDownloadIsHalted();
    void shaLessShortDownloadRecovers();
    void racingMismatchRecovers();
    void shortDownloadOfADeletedFile();
    void waitingEndsWithTheConnection();
    void plainLinkShortIsAnotherFile();
    void floodedQuestionIsAskedAgain();
    void dataSaverPausesAWaitingDownload();
    void clickWhileWaitingIsTheUsers();
    void cantTellShortEndsAndNotesTheSize();
    void cantTellMismatchIsNotNoted();
    void waitingIsDrawnWithoutPercent();

    // 2.3.2: preallocating servers (TeaSpeak), sender: the content check
    void tailCheckWaitsForTheEnd();
    void serverWithoutSeekIsCheckedWhole();
    void largeFileWithoutSeekGoesAfterTheSize();
    void refusedSizeIsCheckedByItsBytes();
    void busyFileIsStillArriving();
    void cancelDuringTheTailCheck();
    void disconnectDuringTheTailCheck();
    void shutdownDuringTheTailCheck();
    void serverVersionIsLoggedOnce();
    void tailCheckReachesTheLastData();
    void serverIgnoringTheOffsetIsCheckedWhole();
    void partialServerIsWaitedFor();
    // 2.3.2: preallocating servers, receiver: zero tails
    void zeroTailedDownloadWaitsAndRecovers();
    void zeroTailThatNeverFillsIsIncomplete();
    void shaLessZeroTailWaits();
    void mediaNeedsALongerZeroRun();
    void shaLessFileEndingInZerosIsKept();
    void shortZeroRunOfAMismatchWaits();
    // 2.3.3: TeaSpeak drops the connection when asked for a file's size
    void teaSpeakSenderIsNeverAskedForASize();
    void teaSpeakReceiverIsNeverAskedForASize();

  private:
    void newCore();
    anyID lastDownloadId() const;
    void  complete(anyID id);
    // The newest download: delivers bytes and reports it complete.
    void  deliverAndComplete(const QByteArray& bytes);
    // 2.3.2: sends bytes as name and reports the upload complete while the server has only the first
    // `arrived` of them (the rest still on its way). *upload: its transfer, *id: its job.
    void sendArriving(const QString& name, const QByteArray& bytes, int arrived, fakets3::Transfer* upload, int* id);
    // 2.3.2: answers file info question `index` as TeamSpeak would: the size event (naming the file as
    // `name`, or by the path asked for) and the return code's answer, in either order. size < 0: no
    // such file; permission: no permission to look.
    void answerInfo(int index, qint64 size, bool sizeFirst = true, const QString& name = {}, bool permission = false);
    // 2.3.2: content check `index` (fakets3::checkDownloads) once it is there: served as the server would
    // (its tail with TeamSpeak's resume, or the whole file) and reported complete; or refused.
    void serveCheck(int index, fakets3::Transfer* served = nullptr);
    void refuseCheck(int index);
    const MediaEntry* entry(const MediaLink& link) const { return m_core->entry(link.key()); }
    QString cacheDir() const { return m_dir->path() + QStringLiteral("/plugins/tsmedia/cache"); }
    QString serverCheckDir() const { return m_dir->path() + QStringLiteral("/plugins/tsmedia/servercheck"); }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    // The other test classes run in the same executable: what this one changes is put back.
    TS3Functions m_savedFuncs{};
    QString      m_savedPluginId;
    Settings     m_savedSettings;
};

void TestCoreSha::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
}

void TestCoreSha::cleanupTestCase()
{
    ts3::funcs            = m_savedFuncs;
    ts3::pluginId         = m_savedPluginId;
    Settings::instance()  = m_savedSettings;
    fileverify::resetCounters();
}

void TestCoreSha::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    Settings& s           = Settings::instance();
    s                     = Settings();
    s.uploadDirectory     = QStringLiteral("/");
    s.generatePreviews    = false;
    s.addRequiredNotice   = false;
    s.videoAutoDownloadMB = 0;
    fileverify::resetCounters();
    newCore();
}

void TestCoreSha::cleanup()
{
    m_core.reset();
    m_dir.reset();
}

void TestCoreSha::newCore()
{
    m_core.reset();
    m_core = std::make_unique<Core>();
    m_core->start();
}

anyID TestCoreSha::lastDownloadId() const
{
    const QList<fakets3::Transfer> downloads = fakets3::downloads();
    return downloads.isEmpty() ? 0 : downloads.last().id;
}

void TestCoreSha::complete(anyID id)
{
    m_core->onTransferStatus(id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
}

void TestCoreSha::deliverAndComplete(const QByteArray& bytes)
{
    const anyID id = lastDownloadId();
    QVERIFY(id != 0);
    QVERIFY(!fakets3::deliver(id, bytes).isEmpty());
    complete(id);
}

void TestCoreSha::sendArriving(const QString& name, const QByteArray& bytes, int arrived, fakets3::Transfer* upload, int* id)
{
    const QString source = m_dir->path() + QStringLiteral("/outside/") + name;
    QVERIFY(writeFile(source, bytes));
    const int  before = fakets3::uploads().size();
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    m_core->uploadFiles({source}, target);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), before + 1, 5000);
    *upload = fakets3::uploads().last();
    *id     = m_core->uploadIds().last();
    // TeamSpeak reports the upload complete while part of it is still on its way to the server.
    fakets3::putFile(upload->remotePath, bytes.left(arrived));
    m_core->onTransferStatus(upload->id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
}

void TestCoreSha::answerInfo(int index, qint64 size, bool sizeFirst, const QString& name, bool permission)
{
    const QList<fakets3::InfoRequest> requests = fakets3::fileInfoRequests();
    QVERIFY(index < requests.size());
    const fakets3::InfoRequest request = requests.at(index);
    if (permission) {
        m_core->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, request.returnCode, QStringLiteral("insufficient client permissions"),
                              true);
        return;
    }
    if (size < 0) {
        m_core->onServerError(fakets3::kConnection, ERROR_file_not_found, request.returnCode, QStringLiteral("file not found"), false);
        return;
    }
    const QString reported = name.isEmpty() ? request.path : name;
    if (sizeFirst)
        m_core->onFileInfo(fakets3::kConnection, request.channel, reported, static_cast<quint64>(size));
    m_core->onServerError(fakets3::kConnection, ERROR_ok, request.returnCode, QString(), false);
    if (!sizeFirst)
        m_core->onFileInfo(fakets3::kConnection, request.channel, reported, static_cast<quint64>(size));
}

void TestCoreSha::serveCheck(int index, fakets3::Transfer* served)
{
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::checkDownloads().size() > index, 5000);
    const fakets3::Transfer check = fakets3::checkDownloads().at(index);
    QVERIFY(!fakets3::deliverServerFile(check.id).isEmpty());
    complete(check.id);
    if (served)
        *served = check;
}

void TestCoreSha::refuseCheck(int index)
{
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::checkDownloads().size() > index, 5000);
    const fakets3::Transfer check = fakets3::checkDownloads().at(index);
    m_core->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, check.returnCode, QStringLiteral("insufficient client permissions"), true);
}

// ---- receiver -------------------------------------------------------------------------------------

void TestCoreSha::verifiedDownloadIsCached()
{
    const QString    remote = QStringLiteral("/tsmedia/photo_1234abcd.png");
    const QByteArray bytes  = pattern(300000);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    QCOMPARE(link.sha256, hashing::sha256(bytes));

    m_core->ensure(link); // a picture under the auto-download limit: fetched at once
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    deliverAndComplete(bytes);

    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    const MediaEntry* e = entry(link);
    QVERIFY(e->check.verified);
    QVERIFY(!e->check.running);
    QCOMPARE(readFile(e->localPath), bytes);
    // The cache name carries the whole key (a made-up hash can't be tuned to share it).
    QVERIFY2(QFileInfo(e->localPath).fileName().startsWith(link.key() + QLatin1Char('_')), qPrintable(e->localPath));
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 1);
    QVERIFY(filesUnder(cacheDir() + QStringLiteral("/.partial")).isEmpty());
}

void TestCoreSha::mismatchIsBlockedAfterTwoDownloads()
{
    fakets3::enableFileInfo(true);
    const QString    remote   = QStringLiteral("/tsmedia/photo_1234abcd.png");
    const QByteArray sent     = pattern(200000, 1);
    const QByteArray replaced = pattern(200000, 2); // same size: the size check can't tell
    fakets3::putFile(remote, replaced);
    const MediaLink link = linkTo(remote, sent, hashing::sha256(sent));

    m_core->ensure(link);
    deliverAndComplete(replaced);
    // 2.3.2: not final at once (the sender's upload may still have been arriving at the server): it
    // waits, never shown, the server is asked, and it is downloaded once more after it reported the
    // whole file.
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, kCheckTimeoutMs);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    QVERIFY(filesUnder(cacheDir()).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, replaced.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, kCheckTimeoutMs);
    QVERIFY(!entry(link)->waitingForUpload);
    deliverAndComplete(replaced);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, kCheckTimeoutMs);
    answerInfo(1, replaced.size());

    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    const MediaEntry* e = entry(link);
    QCOMPARE(e->error, MediaError::Mismatch);
    QCOMPARE(e->errorText, downloadErrorText(MediaError::Mismatch));
    QCOMPARE(e->check.received, hashing::sha256(replaced));
    QVERIFY(!e->check.verified);
    QVERIFY(!e->waitingForUpload);
    QVERIFY(!QFileInfo::exists(e->localPath));
    QVERIFY(filesUnder(cacheDir()).isEmpty()); // deleted: never shown, cached or saved
    QVERIFY(fakets3::logContains(QStringLiteral("SHA-256 mismatch for /tsmedia/photo_1234abcd.png: expected ") + hashing::toHex(hashing::sha256(sent))
                                 + QStringLiteral(", got ") + hashing::toHex(hashing::sha256(replaced))));
    QVERIFY(fakets3::logContains(QStringLiteral("doesn't match its SHA-256 the second time either")));
    QCOMPARE(fileverify::counted(fileverify::Counter::Mismatched), 2);
    QCOMPARE(fakets3::downloads().size(), 2);

    // The same wrong bytes twice: noted, so another made-up hash for this file is refused at once.
    const MediaLink forged = linkTo(remote, sent, hashing::sha256("made up"));
    m_core->ensure(forged);
    QCOMPARE(entry(forged)->state, MediaState::Failed);
    QCOMPARE(entry(forged)->error, MediaError::Mismatch);
    QCOMPARE(fakets3::downloads().size(), 2);

    // 2.3.2: a click still tries again (whatever made it not match may be over): downloaded at once,
    // and the server's file is the right one by now.
    fakets3::putFile(remote, sent);
    m_core->retry(link.key());
    QCOMPARE(fakets3::downloads().size(), 3);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), sent);
}

void TestCoreSha::forgedLinkIsRefusedWithoutDownload()
{
    const QString    remote = QStringLiteral("/tsmedia/clip_00c0ffee.png");
    const QByteArray bytes  = pattern(150000, 3);
    fakets3::putFile(remote, bytes);
    const MediaLink genuine = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(genuine);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(genuine)->state, MediaState::Ready, kCheckTimeoutMs);

    // Someone posts the same file with made-up hashes (to make everyone fetch it again and again).
    for (int i = 0; i < 3; ++i) {
        const MediaLink forged = linkTo(remote, bytes, hashing::sha256(QByteArray::number(i)));
        QVERIFY(forged.key() != genuine.key());
        m_core->ensure(forged);
        QCOMPARE(entry(forged)->state, MediaState::Failed);
        QCOMPARE(entry(forged)->error, MediaError::Mismatch);
        QCOMPARE(entry(forged)->check.received, hashing::sha256(bytes)); // the real one, for "Copy details"
    }
    QCOMPARE(fakets3::downloads().size(), 1); // only the genuine one was ever fetched
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 3);
    QCOMPARE(entry(genuine)->state, MediaState::Ready);
    QVERIFY(entry(genuine)->check.verified);

    // 2.3.2: a click on one of them is the user asking: it is downloaded after all (and checked).
    const MediaLink clicked = linkTo(remote, bytes, hashing::sha256(QByteArray::number(0)));
    m_core->download(clicked.key(), true);
    QCOMPARE(fakets3::downloads().size(), 2);
    QCOMPARE(entry(clicked)->state, MediaState::Downloading);
    QCOMPARE(entry(genuine)->state, MediaState::Ready); // the genuine copy stays as it is
    // The guard isn't weakened by it: another made-up hash is still refused without a download.
    const MediaLink another = linkTo(remote, bytes, hashing::sha256("another"));
    m_core->ensure(another);
    QCOMPARE(entry(another)->state, MediaState::Failed);
    QCOMPARE(entry(another)->error, MediaError::Mismatch);
    QCOMPARE(fakets3::downloads().size(), 2);
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 4);
}

void TestCoreSha::forgedLinkFirstDoesNotPoisonGenuine()
{
    const QString    remote = QStringLiteral("/tsmedia/song_0badf00d.png");
    const QByteArray bytes  = pattern(120000, 4);
    fakets3::putFile(remote, bytes);

    // The forged link arrives first: downloaded, doesn't match, deleted; 2.3.2: downloaded once more
    // (the upload might still have been arriving), the same bytes again: a real mismatch.
    const MediaLink forged = linkTo(remote, bytes, hashing::sha256("not the file"));
    m_core->ensure(forged);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, kCheckTimeoutMs);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(forged)->state, MediaState::Failed, kCheckTimeoutMs);
    QCOMPARE(entry(forged)->error, MediaError::Mismatch);

    // The genuine link has a key of its own: downloaded, checked, shown.
    const MediaLink genuine = linkTo(remote, bytes, hashing::sha256(bytes));
    QVERIFY(genuine.key() != forged.key());
    m_core->ensure(genuine);
    QCOMPARE(fakets3::downloads().size(), 3);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(genuine)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(genuine)->check.verified);
    QCOMPARE(readFile(entry(genuine)->localPath), bytes);
    QCOMPARE(entry(forged)->state, MediaState::Failed);

    // Another made-up hash for the same file: the real digest is known now, no download.
    const MediaLink again = linkTo(remote, bytes, hashing::sha256("also not"));
    m_core->ensure(again);
    QCOMPARE(entry(again)->error, MediaError::Mismatch);
    QCOMPARE(fakets3::downloads().size(), 3);
}

void TestCoreSha::writeRaceRecoversOnSecondPass()
{
    // TeamSpeak reports the transfer complete while the end of the file is still zeros and it opens it
    // again to finish writing (2.0.5): the first pass doesn't match, the second waits for the writer.
    const QString    remote = QStringLiteral("/tsmedia/video_feedbeef.mp4");
    const QByteArray bytes  = pattern(3 * 1024 * 1024 + 4321, 5);
    QByteArray       torn   = bytes;
    torn.replace(torn.size() - 65536, 65536, QByteArray(65536, '\0'));
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));

    m_core->ensure(link);
    QCOMPARE(fakets3::downloads().size(), 0); // videos wait for a click (videoAutoDownloadMB 0)
    m_core->download(link.key(), false);
    const anyID   id   = lastDownloadId();
    const QString path = fakets3::deliver(id, torn);
    QVERIFY(!path.isEmpty());
    complete(id);

    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->check.again, kCheckTimeoutMs); // the first pass didn't match
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    {
        QFile writer(path); // "TeamSpeak" has it open for writing again
        QVERIFY(writer.open(QIODevice::ReadWrite));
        QVERIFY(writer.seek(0));
        QCOMPARE(writer.write(bytes), static_cast<qint64>(bytes.size()));
        writer.flush();
        QTest::qWait(1500); // longer than the pause before the second pass: it must wait for the writer
        QCOMPARE(entry(link)->state, MediaState::Downloading);
    }
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);
    QCOMPARE(fileverify::counted(fileverify::Counter::RecoveredOnRecheck), 1);
    QVERIFY(fakets3::logContains(QStringLiteral("matched its SHA-256 only on the second check")));
}

void TestCoreSha::unreadableFileIsRetryable()
{
    const QString    remote = QStringLiteral("/tsmedia/photo_abcdef01.png");
    const QByteArray bytes  = pattern(100000, 6);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    const anyID   id   = lastDownloadId();
    const QString path = fakets3::deliver(id, pattern(100000, 7));
    complete(id);
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->check.again, kCheckTimeoutMs);
    QVERIFY(QFile::remove(path)); // gone before the second pass (antivirus, a cleanup tool)

    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    QCOMPARE(entry(link)->error, MediaError::Other);
    QVERIFY(entry(link)->errorText.contains(QStringLiteral("Couldn't check the downloaded file")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ReadErrors), 1);

    m_core->retry(link.key()); // this one can be tried again
    QCOMPARE(fakets3::downloads().size(), 2);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
}

void TestCoreSha::connectionLossDuringCheck()
{
    // The file is complete once it is checked: losing the connection meanwhile changes nothing.
    const QString    remote = QStringLiteral("/tsmedia/photo_abcdef02.png");
    const QByteArray bytes  = pattern(100000, 8);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256("other"));
    m_core->ensure(link);
    deliverAndComplete(bytes);
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->check.again, kCheckTimeoutMs);
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    // 2.3.2: a mismatch isn't final before the server was asked whether the upload was still arriving,
    // and it can't be asked now: "Not connected", downloaded again once connected (resumed).
    QCOMPARE(entry(link)->error, MediaError::NotConnected);
    QVERIFY(!entry(link)->waitingForUpload);
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::Mismatched), 1);
}

void TestCoreSha::shutdownDuringCheckIsQuick()
{
    // ~Core while a large file is hashed: returns within about one chunk, nothing crashes later.
    const QString    remote = QStringLiteral("/tsmedia/movie_12345678.mp4");
    const QByteArray bytes  = pattern(96 * 1024 * 1024, 9);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    m_core->download(link.key(), false);
    deliverAndComplete(bytes);
    QVERIFY(entry(link)->check.running);
    QTest::qWait(20);

    QElapsedTimer timer;
    timer.start();
    m_core.reset();
    QVERIFY2(timer.elapsed() < 300, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
    QTest::qWait(100); // queued results for the deleted Core are dropped
}

void TestCoreSha::cachedVerifiedFileIsNotHashedAgain()
{
    const QString    remote = QStringLiteral("/tsmedia/photo_abcdef03.png");
    const QByteArray bytes  = pattern(250000, 10);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 1);

    // TeamSpeak restarts: the link is seen again and its file is in the cache under its sha key.
    newCore();
    m_core->ensure(link);
    QCOMPARE(entry(link)->state, MediaState::Ready);
    QVERIFY(entry(link)->check.verified);
    QTest::qWait(50);
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 1); // not hashed again
}

void TestCoreSha::previewChecksum_data()
{
    QTest::addColumn<bool>("matches");
    QTest::newRow("preview matches ph") << true;
    QTest::newRow("preview was replaced") << false;
}

void TestCoreSha::previewChecksum()
{
    QFETCH(bool, matches);
    const QString    remote  = QStringLiteral("/tsmedia/clip_0c0ffee0.mp4");
    const QString    pvPath  = QStringLiteral("/tsmedia/previews/0c0ffee0.jpg");
    const QByteArray bytes   = pattern(500000, 11);
    const QByteArray preview = pattern(20000, 12);
    fakets3::putFile(remote, bytes);
    fakets3::putFile(pvPath, matches ? preview : pattern(20000, 13));

    MediaLink link   = linkTo(remote, bytes, hashing::sha256(bytes));
    link.width       = 1280;
    link.height      = 720;
    link.durationMs  = 5000;
    link.previewFile = pvPath;
    link.previewSha  = fileverify::previewDigest(preview);
    link             = MediaLink::parse(link.toUrl());
    QCOMPARE(link.previewSha, fileverify::previewDigest(preview));

    m_core->ensure(link); // a video: only its poster is fetched
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(fakets3::downloads().first().remotePath, pvPath);
    const anyID id = lastDownloadId();
    QVERIFY(!fakets3::deliverServerFile(id).isEmpty());
    complete(id);

    const MediaState want = matches ? MediaState::Ready : MediaState::Failed;
    QTRY_COMPARE(entry(link)->previewState, want);
    QCOMPARE(QFileInfo::exists(entry(link)->previewPath), matches);
    QCOMPARE(fileverify::counted(fileverify::Counter::PreviewMismatched), matches ? 0 : 1);

    // Seen again after a restart: a cached preview is checked against ph before it is used.
    if (matches) {
        newCore();
        m_core->ensure(link);
        QCOMPARE(entry(link)->previewState, MediaState::Ready);
        MediaLink other  = link;
        other.previewSha = fileverify::previewDigest("another preview");
        other.size += 1; // another link (key) sharing that preview file
        m_core->ensure(other);
        QVERIFY(entry(other)->previewState != MediaState::Ready);
        QCOMPARE(fakets3::downloads().size(), 2); // its own preview is fetched (and checked) instead
    }
}

void TestCoreSha::linkWithoutShaIsNotChecked()
{
    // From 2.0/2.1 senders: no sha, no check, no "verified", cached under 2.1's names.
    const QString    remote = QStringLiteral("/tsmedia/photo_abcdef04.png");
    const QByteArray bytes  = pattern(90000, 14);
    fakets3::putFile(remote, bytes);
    const MediaLink link = linkTo(remote, bytes, QByteArray());
    QVERIFY(link.sha256.isEmpty());
    m_core->ensure(link);
    deliverAndComplete(bytes);
    QTRY_COMPARE(entry(link)->state, MediaState::Ready);
    QVERIFY(!entry(link)->check.verified);
    QVERIFY(QFileInfo(entry(link)->localPath).fileName().startsWith(link.key().left(8) + QLatin1Char('_')));
    QCOMPARE(fileverify::counted(fileverify::Counter::Verified), 0);
}

void TestCoreSha::unknownSizeLinkIsChecked()
{
    // A hostile link without a size (no size check possible): the sha still decides.
    const QString    remote = QStringLiteral("/tsmedia/photo_abcdef05.png");
    const QByteArray bytes  = pattern(80000, 15);
    fakets3::putFile(remote, bytes);
    MediaLink link = linkTo(remote, bytes, hashing::sha256("something else"));
    link.size      = 0;
    link           = MediaLink::parse(link.toUrl());
    QVERIFY(!link.sha256.isEmpty());
    m_core->ensure(link);
    QCOMPARE(fakets3::downloads().size(), 1);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    QCOMPARE(entry(link)->error, MediaError::Mismatch);
    QVERIFY(filesUnder(cacheDir()).isEmpty());
}

void TestCoreSha::checkingShownOnlyWhenSlow()
{
    // A quick check never says "Checking file…" (no flash); one that takes a moment does.
    const QString    quick = QStringLiteral("/tsmedia/photo_abcdef06.png");
    const QByteArray bytes = pattern(60000, 16);
    fakets3::putFile(quick, bytes);
    const MediaLink fast = linkTo(quick, bytes, hashing::sha256(bytes));
    bool            shown    = false;
    const auto      watching = connect(m_core.get(), &Core::entryChanged, this, [&](const QString& key) {
        if (const MediaEntry* e = m_core->entry(key))
            shown = shown || e->check.shown;
    });
    const auto stopWatching = qScopeGuard([watching] { QObject::disconnect(watching); });
    m_core->ensure(fast);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(fast)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(!shown);

    const QString slowPath = QStringLiteral("/tsmedia/photo_abcdef07.png");
    fakets3::putFile(slowPath, bytes);
    const MediaLink slow = linkTo(slowPath, bytes, hashing::sha256("wrong")); // two passes, a pause between
    m_core->ensure(slow);
    deliverAndComplete(bytes);
    QTRY_VERIFY_WITH_TIMEOUT(shown, kCheckTimeoutMs);
    QVERIFY(entry(slow)->check.running);
    QCOMPARE(entry(slow)->state, MediaState::Downloading);
    QCOMPARE(entry(slow)->progress, 1.0);
    // 2.3.2: a mismatch is downloaded once more before it counts; the same bytes again end it.
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 3, kCheckTimeoutMs);
    QVERIFY(!entry(slow)->check.shown && !entry(slow)->check.running);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(slow)->state, MediaState::Failed, kCheckTimeoutMs);
    QVERIFY(!entry(slow)->check.shown && !entry(slow)->check.running);
}

// ---- sender ---------------------------------------------------------------------------------------

void TestCoreSha::sentLinkCarriesShaOfStagedBytes()
{
    const QString    source = m_dir->path() + QStringLiteral("/outside/report.zip");
    const QByteArray bytes  = pattern(700000, 17);
    QVERIFY(writeFile(source, bytes));

    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    m_core->uploadFiles({source}, target);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 1, 5000);
    // The original changes after it was staged: what is sent (and hashed) is the staged copy.
    QVERIFY(writeFile(source, pattern(700000, 18)));
    const fakets3::Transfer upload = fakets3::uploads().first();
    QVERIFY(fakets3::completeUpload(upload.id));
    m_core->onTransferStatus(upload.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);

    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 5000);
    const fakets3::Message message = fakets3::messages().first();
    const QList<MediaLink> links   = MediaLink::findInMessage(QString::fromUtf8(message.text));
    QCOMPARE(links.size(), 1);
    const MediaLink sent = links.first();
    QCOMPARE(sent.remoteFile(), upload.remotePath);
    QCOMPARE(sent.sha256, hashing::sha256(bytes));
    QCOMPARE(sent.sha256, hashing::sha256(fakets3::serverFile(upload.remotePath))); // what the server has
    m_core->onServerError(fakets3::kConnection, ERROR_ok, message.returnCode, QString(), false);
    QCOMPARE(fileverify::counted(fileverify::Counter::SentWithSha), 1);

    // The sender's own copy is in the cache, marked verified; its echo downloads nothing.
    const MediaEntry* own = m_core->entry(sent.key());
    QVERIFY(own);
    QVERIFY(own->isOwnUpload);
    QCOMPARE(own->state, MediaState::Ready);
    QVERIFY(own->check.verified);
    m_core->onTextMessage(fakets3::kConnection, QString::fromUtf8(message.text));
    QCOMPARE(fakets3::downloads().size(), 0);

    // A made-up hash for the file we sent ourselves is refused without a download.
    MediaLink forged = sent;
    forged.sha256    = hashing::sha256("forged");
    m_core->ensure(MediaLink::parse(forged.toUrl()));
    QCOMPARE(m_core->entry(MediaLink::parse(forged.toUrl()).key())->error, MediaError::Mismatch);
    QCOMPARE(fakets3::downloads().size(), 0);
}

// ---- 2.3.2: the upload race, sender ---------------------------------------------------------------

void TestCoreSha::messageWaitsForTheWholeFile()
{
    // The field report: TeamSpeak says "complete" while half of the file is still on its way, and the
    // message went out at once. Now it waits until the server reports the whole size.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(2097152, 30);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("race.zip"), bytes, 1048576, &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000); // asked right away
    QCOMPARE(fakets3::fileInfoRequests().first().path, upload.remotePath);
    QCOMPARE(fakets3::fileInfoRequests().first().channel, static_cast<uint64>(fakets3::kChannel));
    const UploadJob* job = m_core->upload(id);
    QVERIFY(job);
    QCOMPARE(job->state, UploadState::Posting);
    QVERIFY(job->waiting);   // "Posting to chat…", and it can still be canceled
    QVERIFY(!job->uploaded); // not before the server has all of it
    QVERIFY(m_core->canCancelUpload(id));
    QCOMPARE(job->message, i18n::t("Posting to chat…"));

    answerInfo(0, 1048576); // half: asked again in a moment
    QTest::qWait(150);
    QVERIFY(fakets3::messages().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    QVERIFY(fakets3::messages().isEmpty());
    // The rest has arrived. This time the size comes after the return code's answer, naming the file alone.
    fakets3::putFile(upload.remotePath, bytes);
    answerInfo(1, bytes.size(), false, QFileInfo(upload.remotePath).fileName());
    // 2.3.2: its end is read back from the server (a preallocating server reports the whole size early).
    fakets3::Transfer check;
    serveCheck(0, &check);
    if (QTest::currentTestFailed())
        return;
    QCOMPARE(check.remotePath, upload.remotePath);
    QVERIFY(check.resume);
    QVERIFY(!check.overwrite);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(m_core->upload(id)->uploaded);
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + upload.remotePath + QStringLiteral(" (content checked: tail) after ")));
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000); // the check's download is gone
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsWaited), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
    const QList<MediaLink> links = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().sha256, hashing::sha256(bytes));
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().first().returnCode, QString(), false);
    QCOMPARE(m_core->upload(id)->state, UploadState::Done);
    QTest::qWait(400);
    QCOMPARE(fakets3::fileInfoRequests().size(), 2); // nothing asked after that
}

void TestCoreSha::previewIsConfirmedToo()
{
    // The message names the preview too: the server must have all of both.
    fakets3::enableFileInfo(true);
    Settings::instance().generatePreviews = true;
    QImage banner(2600, 300, QImage::Format_RGB32); // wider than 2560: it gets a preview
    banner.fill(QColor(30, 120, 200));
    const QString source = m_dir->path() + QStringLiteral("/outside/banner.png");
    QDir().mkpath(QFileInfo(source).absolutePath());
    QVERIFY(banner.save(source));
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    m_core->uploadFiles({source}, target);

    QTRY_COMPARE_WITH_TIMEOUT(fakets3::directories().size(), 1, 10000); // the previews folder
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::directories().first().returnCode, QString(), false);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 1, 5000);
    const fakets3::Transfer preview = fakets3::uploads().first();
    QVERIFY(preview.remotePath.startsWith(QLatin1String("/previews/")));
    QVERIFY(fakets3::completeUpload(preview.id));
    m_core->onTransferStatus(preview.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, 5000);
    const fakets3::Transfer main = fakets3::uploads().at(1);
    QVERIFY(fakets3::completeUpload(main.id));
    m_core->onTransferStatus(main.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);

    // The file wasn't all there at once (the line to the server was busy): its preview is asked about
    // too once the file is, before the message goes.
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    QCOMPARE(fakets3::fileInfoRequests().at(0).path, main.remotePath);
    const int mainSize = fakets3::serverFile(main.remotePath).size();
    answerInfo(0, mainSize / 2);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    QCOMPARE(fakets3::fileInfoRequests().at(1).path, main.remotePath);
    answerInfo(1, mainSize);
    serveCheck(0); // 2.3.2: the file's bytes
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 3, 3000);
    QCOMPARE(fakets3::fileInfoRequests().at(2).path, preview.remotePath);
    QVERIFY(fakets3::messages().isEmpty());
    answerInfo(2, fakets3::serverFile(preview.remotePath).size());
    fakets3::Transfer previewCheck;
    serveCheck(1, &previewCheck); // ... and the preview's
    if (QTest::currentTestFailed())
        return;
    QCOMPARE(previewCheck.remotePath, preview.remotePath);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    const QList<MediaLink> links = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().previewFile, preview.remotePath);
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsWaited), 1);
}

void TestCoreSha::previewNotAskedWhenTheFileIsThere()
{
    // The server has all of the file at the first question: its preview went out before it, so it is
    // there too. One question (8 flood points) per file, not two.
    fakets3::enableFileInfo(true);
    Settings::instance().generatePreviews = true;
    QImage banner(2600, 300, QImage::Format_RGB32);
    banner.fill(QColor(200, 120, 30));
    const QString source = m_dir->path() + QStringLiteral("/outside/wide.png");
    QDir().mkpath(QFileInfo(source).absolutePath());
    QVERIFY(banner.save(source));
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    m_core->uploadFiles({source}, target);

    QTRY_COMPARE_WITH_TIMEOUT(fakets3::directories().size(), 1, 10000);
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::directories().first().returnCode, QString(), false);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 1, 5000);
    const fakets3::Transfer preview = fakets3::uploads().first();
    QVERIFY(fakets3::completeUpload(preview.id));
    m_core->onTransferStatus(preview.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, 5000);
    const fakets3::Transfer main = fakets3::uploads().at(1);
    QVERIFY(fakets3::completeUpload(main.id));
    m_core->onTransferStatus(main.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);

    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    QCOMPARE(fakets3::fileInfoRequests().at(0).path, main.remotePath);
    answerInfo(0, fakets3::serverFile(main.remotePath).size());
    serveCheck(0); // 2.3.2: its bytes, all there at once too
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    const QList<MediaLink> links = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().previewFile, preview.remotePath);
    QCOMPARE(fakets3::fileInfoRequests().size(), 1);
    QCOMPARE(fakets3::checkDownloads().size(), 1); // nor the preview's bytes
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsWaited), 0); // all there at once
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
}

void TestCoreSha::cancelWhileTheServerCatchesUp()
{
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(300000, 31);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("cancel.zip"), bytes, 1000, &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    QVERIFY(m_core->canCancelUpload(id));
    m_core->cancelUpload(id);
    QCOMPARE(m_core->upload(id)->state, UploadState::Canceled);
    QVERIFY(!fakets3::hasServerFile(upload.remotePath)); // no message will ever name it
    answerInfo(0, bytes.size());                        // a late answer changes nothing
    QTest::qWait(600);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(fakets3::fileInfoRequests().size(), 1);
    QCOMPARE(m_core->upload(id)->state, UploadState::Canceled);
}

void TestCoreSha::disconnectWhileTheServerCatchesUp()
{
    // TeamSpeak reported the upload complete, so the file is on the server (or its last bytes are): the
    // message fails like any other that couldn't go, and Retry posts it without a second upload (no
    // orphan in the file browser), after asking the server again.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(300000, 32);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("cut.zip"), bytes, 1000, &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    const UploadJob* job = m_core->upload(id);
    QVERIFY(job);
    QCOMPARE(job->state, UploadState::Failed);
    QVERIFY(job->uploaded);
    QCOMPARE(job->message, postErrorText(MediaError::NotConnected));
    QVERIFY(m_core->canRetryUpload(id));
    QVERIFY(fakets3::hasServerFile(upload.remotePath)); // kept: Retry posts it
    answerInfo(0, bytes.size()); // too late
    QTest::qWait(600);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(m_core->upload(id)->state, UploadState::Failed);
    QCOMPARE(fakets3::fileInfoRequests().size(), 1);

    fakets3::setConnected(true);
    QCOMPARE(m_core->retryUpload(id), id);
    // The server is asked first; the message waits for its answer.
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    QCOMPARE(fakets3::fileInfoRequests().at(1).path, upload.remotePath);
    QTest::qWait(200);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(m_core->upload(id)->state, UploadState::Posting);
    QVERIFY(!m_core->canCancelUpload(id));
    fakets3::putFile(upload.remotePath, bytes); // its last bytes arrived after all
    answerInfo(1, bytes.size());
    // 2.3.2: its bytes are checked too, against the end kept since the upload (the staging folder went
    // with the failed message).
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
    QCOMPARE(fakets3::uploads().size(), 1); // no second upload
    const QList<MediaLink> links = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().remoteFile(), upload.remotePath);
    QCOMPARE(links.first().sha256, hashing::sha256(bytes));
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().first().returnCode, QString(), false);
    QCOMPARE(m_core->upload(id)->state, UploadState::Done);
}

void TestCoreSha::repostWaitsOnlyForItsOwnFile()
{
    // While a reposted message waits for the server's answer, messages of other sends go.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(200000, 34);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("first.zip"), bytes, 500, &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    QCOMPARE(m_core->upload(id)->state, UploadState::Failed);
    fakets3::setConnected(true);
    QCOMPARE(m_core->retryUpload(id), id);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);

    fakets3::Transfer other;
    int               otherId = 0;
    sendArriving(QStringLiteral("second.zip"), bytes, bytes.size(), &other, &otherId);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 3, 3000);
    answerInfo(2, bytes.size());
    serveCheck(0); // 2.3.2
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QCOMPARE(MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text)).first().remoteFile(), other.remotePath);
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().first().returnCode, QString(), false);

    // The server never gets the rest of the first one and can't say so: the message goes anyway. (The
    // newest question about it: a slow machine may have let one time out and asked again.)
    int pending = -1;
    for (int i = 0; i < fakets3::fileInfoRequests().size(); ++i) {
        if (fakets3::fileInfoRequests().at(i).path == upload.remotePath)
            pending = i;
    }
    QVERIFY(pending >= 1);
    answerInfo(pending, -1);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 2, 3000);
    QCOMPARE(MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().at(1).text)).first().remoteFile(), upload.remotePath);
}

void TestCoreSha::serverThatCantTellGetsTheMessageAnyway()
{
    // No such file (or no permission to look): the message goes at once, receivers cope (below).
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(100000, 33);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("missing.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, -1);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("The server doesn't list ") + upload.remotePath));
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().first().returnCode, QString(), false);

    fakets3::Transfer second;
    int               secondId = 0;
    sendArriving(QStringLiteral("refused.zip"), bytes, bytes.size(), &second, &secondId);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    answerInfo(1, 0, true, {}, true);
    QVERIFY(fakets3::logContains(QStringLiteral("Couldn't ask the server whether it has all of ") + second.remotePath));
    // 2.3.2: its bytes would tell, but downloading them isn't allowed either. Refused for lack of
    // permission: the whole file would be too, so it isn't asked for.
    refuseCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 2, 3000);
    QCOMPARE(fakets3::checkDownloads().size(), 1);
    QVERIFY(fakets3::logContains(QStringLiteral("Couldn't check whether the server has all of ") + second.remotePath + QStringLiteral(" (no permission to download it)")));
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 2);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().at(1).returnCode, QString(), false);

    // The next file in this channel isn't checked by its bytes at all (each refusal costs flood points) ...
    fakets3::Transfer third;
    int               thirdId = 0;
    sendArriving(QStringLiteral("again.zip"), bytes, bytes.size(), &third, &thirdId);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 3, 3000);
    answerInfo(2, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 3, 3000);
    QCOMPARE(fakets3::checkDownloads().size(), 1);
    QVERIFY(fakets3::logContains(QStringLiteral("Content check of ") + third.remotePath + QStringLiteral(" not possible (no permission to download it)")));
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::messages().at(2).returnCode, QString(), false);

    // ... until the connection ends (permissions may differ on the next one).
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    fakets3::setConnected(true);
    fakets3::Transfer fourth;
    int               fourthId = 0;
    sendArriving(QStringLiteral("later.zip"), bytes, bytes.size(), &fourth, &fourthId);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 4, 3000);
    answerInfo(3, bytes.size());
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 4, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + fourth.remotePath + QStringLiteral(" (content checked: tail) after ")));
}

// ---- 2.3.2: the upload race, receiver -------------------------------------------------------------

void TestCoreSha::shortDownloadWaitsForTheUpload()
{
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_5e5e5e5e.png");
    const QByteArray bytes  = pattern(400000, 40);
    fakets3::putFile(remote, bytes.left(150000)); // the sender's upload is still arriving
    const MediaLink link   = linkTo(remote, bytes, hashing::sha256(bytes));
    bool            failed = false;
    const auto      watching = connect(m_core.get(), &Core::entryChanged, this, [&](const QString& key) {
        if (const MediaEntry* e = m_core->entry(key))
            failed = failed || e->state == MediaState::Failed;
    });
    const auto stopWatching = qScopeGuard([watching] { QObject::disconnect(watching); });

    m_core->ensure(link);
    QCOMPARE(fakets3::downloads().size(), 1);
    deliverAndComplete(bytes.left(150000));
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QCOMPARE(entry(link)->state, MediaState::Downloading); // waiting, not an error
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    QCOMPARE(fakets3::fileInfoRequests().first().path, remote);
    answerInfo(0, 200000); // more has arrived, not all of it
    QTest::qWait(500);
    QCOMPARE(fakets3::downloads().size(), 1); // not downloaded again for nothing
    QVERIFY(entry(link)->waitingForUpload);

    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 5000); // asked again after a pause
    answerInfo(1, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 3000);
    QVERIFY(!entry(link)->waitingForUpload);
    QVERIFY(fakets3::downloads().at(1).localDir != fakets3::downloads().at(0).localDir); // a folder of its own
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);
    QVERIFY(!failed);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRefetched), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRecovered), 1);
    QVERIFY(fakets3::logContains(QStringLiteral("arrived complete after waiting")));
    QVERIFY(filesUnder(cacheDir() + QStringLiteral("/.partial")).isEmpty());
}

void TestCoreSha::racingMismatchRecovers()
{
    // The 2.2 report itself: the download came out complete in size but wrong (the server didn't have
    // the end yet). It waits for the upload instead of "File doesn't match what was sent".
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/clip_7a7a7a7a.png");
    const QByteArray bytes  = pattern(300000, 41);
    QByteArray       torn   = bytes;
    torn.replace(200000, 100000, QByteArray(100000, '\x5a'));
    fakets3::putFile(remote, torn);
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(torn);
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, kCheckTimeoutMs);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, 200000); // the server is still getting it
    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 5000);
    answerInfo(1, bytes.size());
    // Had the racing digest been noted for the forged-hash guard, this download would be refused.
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 3000);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 0);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRecovered), 1);
}

void TestCoreSha::shortDownloadOfADeletedFile()
{
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_6d6d6d6d.png");
    const QByteArray bytes  = pattern(200000, 42);
    fakets3::putFile(remote, bytes.left(50000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(50000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, -1); // gone from the server meanwhile
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, 3000);
    QCOMPARE(entry(link)->error, MediaError::NotFound);
    QCOMPARE(entry(link)->errorText, downloadErrorText(MediaError::NotFound));
    QVERIFY(!entry(link)->waitingForUpload);
    QCOMPARE(fakets3::downloads().size(), 1);
}

void TestCoreSha::waitingEndsWithTheConnection()
{
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_7c7c7c7c.png");
    const QByteArray bytes  = pattern(200000, 43);
    fakets3::putFile(remote, bytes.left(50000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(50000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    QCOMPARE(entry(link)->state, MediaState::Failed);
    QCOMPARE(entry(link)->error, MediaError::NotConnected); // resumed once connected again
    QVERIFY(!entry(link)->waitingForUpload);
    answerInfo(0, bytes.size()); // too late: nothing
    QTest::qWait(300);
    QCOMPARE(entry(link)->state, MediaState::Failed);
    QCOMPARE(fakets3::downloads().size(), 1);
}

void TestCoreSha::plainLinkShortIsAnotherFile()
{
    // A plain TeamSpeak link (copied from the file browser, not posted after an upload) can't race an
    // upload: a short file is another file, as before 2.3.2.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/shared/notes.png");
    const QByteArray bytes  = pattern(120000, 44);
    fakets3::putFile(remote, bytes.left(60000));
    MediaLink link = linkTo(remote, bytes, QByteArray());
    link.protocol  = 0;
    link           = MediaLink::parse(link.toUrl());
    QVERIFY(!link.isTsMedia());
    m_core->ensure(link);
    QCOMPARE(fakets3::downloads().size(), 1);
    deliverAndComplete(bytes.left(60000));
    QCOMPARE(entry(link)->state, MediaState::Failed);
    QCOMPARE(entry(link)->error, MediaError::NotFound);
    QTest::qWait(100);
    QVERIFY(fakets3::fileInfoRequests().isEmpty());
}

void TestCoreSha::announcedShortDownloadIsHalted()
{
    // The server announces fewer bytes than the link names while the download runs: it can only bring
    // what is there so far. Halted (the progress timer sees it), and fetched again once the server has
    // all of it.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/clip_1b1b1b1b.png");
    const QByteArray bytes  = pattern(300000, 45);
    fakets3::putFile(remote, bytes.left(100000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    QCOMPARE(fakets3::downloads().size(), 1);
    const anyID first = lastDownloadId();
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QVERIFY(fakets3::transfer(first).halted);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    QVERIFY(fakets3::logContains(QStringLiteral("so far; waiting for the sender's upload")));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, 100000);
    QTest::qWait(300);
    QCOMPARE(fakets3::downloads().size(), 1); // its slot is free, nothing fetched for nothing

    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 5000);
    answerInfo(1, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 3000);
    QVERIFY(fakets3::downloads().at(1).localDir != fakets3::downloads().at(0).localDir);
    // The halted transfer's late "canceled" changes nothing.
    m_core->onTransferStatus(first, ERROR_file_transfer_canceled, QString(), fakets3::kConnection);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(cacheDir() + QStringLiteral("/.partial")).isEmpty(), 3000);
}

void TestCoreSha::shaLessShortDownloadRecovers()
{
    // A TS Media link without a checksum (the sender couldn't hash it) races the upload just the same.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/notes_2c2c2c2c.png");
    const QByteArray bytes  = pattern(160000, 46);
    fakets3::putFile(remote, bytes.left(80000));
    const MediaLink link = linkTo(remote, bytes, QByteArray());
    QVERIFY(link.isTsMedia());
    QVERIFY(link.sha256.isEmpty());
    m_core->ensure(link);
    deliverAndComplete(bytes.left(80000));
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("Downloaded only ")));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    fakets3::putFile(remote, bytes);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, 3000);
    QVERIFY(!entry(link)->check.verified); // nothing to check it against
    QCOMPARE(readFile(entry(link)->localPath), bytes);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRecovered), 1);
}

void TestCoreSha::floodedQuestionIsAskedAgain()
{
    // The server's flood protection holds the question back: that says nothing about the file. Asked
    // again after the pause, never a download while the server is known to be short.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_8e8e8e8e.png");
    const QByteArray bytes  = pattern(200000, 50);
    fakets3::putFile(remote, bytes.left(50000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(50000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, 50000); // still short
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 5000);
    m_core->onServerError(fakets3::kConnection, ERROR_client_is_flooding, fakets3::fileInfoRequests().at(1).returnCode, QStringLiteral("client is flooding"),
                          false, QStringLiteral("retry in 500ms"));
    QTest::qWait(500);
    QCOMPARE(fakets3::downloads().size(), 1);
    QVERIFY(entry(link)->waitingForUpload);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 3, 5000);
    QCOMPARE(fakets3::downloads().size(), 1);
    fakets3::putFile(remote, bytes);
    answerInfo(2, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 3000);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
}

void TestCoreSha::dataSaverPausesAWaitingDownload()
{
    // An automatic download waiting for the sender's upload is still an automatic one: data saver
    // turned on meanwhile holds it (it waits for a click), and the server's answer brings nothing.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_3d3d3d3d.png");
    const QByteArray bytes  = pattern(240000, 47);
    fakets3::putFile(remote, bytes.left(60000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(60000));
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);

    Settings::instance().dataSaver = true;
    m_core->onDataSaverChanged();
    QCOMPARE(entry(link)->state, MediaState::Idle);
    QVERIFY(entry(link)->heldByDataSaver);
    QVERIFY(!entry(link)->waitingForUpload);
    fakets3::putFile(remote, bytes);
    answerInfo(0, bytes.size()); // too late: nothing waits for it
    QTest::qWait(2500);
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(fakets3::fileInfoRequests().size(), 1);

    // A click fetches it as usual.
    m_core->download(link.key(), true);
    QCOMPARE(fakets3::downloads().size(), 2);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
}

void TestCoreSha::clickWhileWaitingIsTheUsers()
{
    // A click on a preview that waits for the sender's upload: the download that follows is the
    // user's, so data saver (and the automatic size limits) no longer hold it.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_9f9f9f9f.png");
    const QByteArray bytes  = pattern(220000, 51);
    fakets3::putFile(remote, bytes.left(70000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(70000));
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    m_core->download(link.key(), true);
    QVERIFY(entry(link)->waitingForUpload); // still waiting: the server decides when

    Settings::instance().dataSaver = true;
    m_core->onDataSaverChanged();
    QVERIFY(entry(link)->waitingForUpload);
    QCOMPARE(entry(link)->state, MediaState::Downloading);
    fakets3::putFile(remote, bytes);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000);
    QVERIFY(!entry(link)->heldByDataSaver);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
}

void TestCoreSha::cantTellShortEndsAndNotesTheSize()
{
    // The server can't be asked (this fake has no requestFileInfo) and the sender's upload was cut off:
    // two downloads that bring nothing new end it (not eight), retryable, and the size it stopped at
    // counts as the file's real one from then on.
    const QString    remote = QStringLiteral("/tsmedia/photo_4e4e4e4e.png");
    const QByteArray bytes  = pattern(300000, 48);
    fakets3::putFile(remote, bytes.left(120000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    deliverAndComplete(bytes.left(120000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000);
    deliverAndComplete(bytes.left(120000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 3, 8000);
    deliverAndComplete(bytes.left(120000));
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, 3000);
    QCOMPARE(entry(link)->error, MediaError::Other);
    QVERIFY(isRetryableDownload(*entry(link)));
    QVERIFY(!entry(link)->waitingForUpload);
    QTest::qWait(300);
    QCOMPARE(fakets3::downloads().size(), 3);

    // Another link naming this file with yet another size isn't fetched automatically.
    const MediaLink other = linkTo(remote, pattern(310000, 48), hashing::sha256(pattern(310000, 48)));
    m_core->ensure(other);
    QCOMPARE(entry(other)->state, MediaState::Idle);
    QCOMPARE(fakets3::downloads().size(), 3);
}

void TestCoreSha::cantTellMismatchIsNotNoted()
{
    // The server can't tell whether it has the whole file and the sender's upload is stuck: the same
    // torn bytes twice end it as a mismatch, but that digest is never taken for the file's real one.
    const QString    remote = QStringLiteral("/tsmedia/photo_5f5f5f5f.png");
    const QByteArray sent   = pattern(200000, 49);
    QByteArray       torn   = sent;
    torn.replace(150000, 50000, QByteArray(50000, '\x5a')); // not zeros: those are an upload still arriving (zeroTail*)
    fakets3::putFile(remote, torn);
    const MediaLink link = linkTo(remote, sent, hashing::sha256(sent));
    m_core->ensure(link);
    deliverAndComplete(torn);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, kCheckTimeoutMs);
    deliverAndComplete(torn);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    QCOMPARE(entry(link)->error, MediaError::Mismatch);
    QVERIFY(fakets3::logContains(QStringLiteral("still doesn't match its SHA-256 after")));
    QVERIFY(isRetryableDownload(*entry(link)));

    // Nothing noted: another link naming this file and size isn't refused at once.
    const MediaLink other = linkTo(remote, sent, hashing::sha256("another"));
    m_core->ensure(other);
    QCOMPARE(entry(other)->state, MediaState::Downloading);
    QCOMPARE(fakets3::downloads().size(), 3);
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 0);
}

void TestCoreSha::waitingIsDrawnWithoutPercent()
{
    // Pictures, album tiles, videos and cards that wait for the sender's upload show the waiting glyph
    // and "Waiting for the sender's upload…", never a 0% that looks stuck.
    MediaEntry e;
    e.link             = linkTo(QStringLiteral("/tsmedia/photo_6a6a6a6a.png"), pattern(5000, 52), hashing::sha256(pattern(5000, 52)));
    e.link.width       = 400;
    e.link.height      = 300;
    e.kind             = MediaKind::Image;
    e.state            = MediaState::Downloading;
    e.waitingForUpload = true;
    QVERIFY(isWaitingForUpload(e));
    QCOMPARE(shownDownloadProgress(e), -1.0);
    QCOMPARE(previewStatusText(e, false), waitingForUploadText());
    QVERIFY(isPreviewActionable(e));

    PreviewStyle style;
    style.animate = false;
    QImage pixels(400, 300, QImage::Format_RGB32);
    pixels.fill(QColor(90, 140, 60));
    MediaStill still;
    still.image  = pixels;
    still.source = MediaStill::Preview;
    QSize      size;
    const auto centre = [&size](const QImage& image) {
        const qreal  dpr = image.devicePixelRatio();
        const QPoint mid(qRound(size.width() * dpr / 2.0), qRound(size.height() * dpr / 2.0));
        return image.copy(QRect(mid - QPoint(14, 14), QSize(28, 28)));
    };
    const QImage waiting = centre(renderPreview(e, still, style, &size));
    MediaEntry   queued  = e;
    queued.state            = MediaState::Queued;
    queued.waitingForUpload = false;
    const QImage asQueued = centre(renderPreview(queued, still, style, &size));
    MediaEntry   zero     = e;
    zero.waitingForUpload = false;
    zero.progress         = 0.0;
    const QImage atZero   = centre(renderPreview(zero, still, style, &size));
    QCOMPARE(waiting, asQueued); // the waiting glyph in the disc
    QVERIFY(waiting != atZero);  // not the "0%" ring
}

// ---- 2.3.2: preallocating servers (TeaSpeak), sender ------------------------------------------------

void TestCoreSha::tailCheckWaitsForTheEnd()
{
    // A TeaSpeak server reports the whole size at once (it preallocates the file), but its copy ends in
    // zeros until the upload has arrived: the message waits for the end, not for the size.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(300000, 60);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("tea.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    fakets3::putFile(upload.remotePath, zeroAfter(bytes, 150000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size()); // what round 1 trusted
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    const fakets3::Transfer first = fakets3::checkDownloads().first();
    QCOMPARE(first.remotePath, upload.remotePath);
    QVERIFY(first.resume);
    QVERIFY(!first.overwrite);
    // TeamSpeak resumes where the local file ends: the file's size less the 64 KB to compare.
    const QString stub = first.localDir + QLatin1Char('/') + QFileInfo(upload.remotePath).fileName();
    QCOMPARE(QFileInfo(stub).size(), qint64(bytes.size() - 65536));
    QVERIFY(QFileInfo(stub).absoluteFilePath().startsWith(QDir(serverCheckDir()).absolutePath()));
    QVERIFY(isSparse(stub)); // the gap before the tail is never written as zeros on disk
    QVERIFY(!fakets3::deliverServerFile(first.id).isEmpty());
    QCOMPARE(QFileInfo(stub).size(), qint64(bytes.size()));
    complete(first.id);
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")), 3000);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(m_core->upload(id)->state, UploadState::Posting);
    QVERIFY(m_core->canCancelUpload(id));
    QVERIFY(!m_core->upload(id)->uploaded);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentWaited), 1);

    // The rest arrives; the next check (half a second later) finds it. The size isn't asked again.
    fakets3::putFile(upload.remotePath, bytes);
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + upload.remotePath + QStringLiteral(" (content checked: tail) after ")));
    QCOMPARE(fakets3::fileInfoRequests().size(), 1);
    QCOMPARE(fakets3::checkDownloads().size(), 2);
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsWaited), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);
    const QList<MediaLink> links = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.first().sha256, hashing::sha256(bytes));
}

void TestCoreSha::serverWithoutSeekIsCheckedWhole()
{
    // A server that can't send the end of a file alone (the resume brings all of it): the tail check
    // comes back too long, so the whole file is downloaded and its SHA-256 compared.
    fakets3::enableFileInfo(true);
    fakets3::setResumeSupported(false);
    const QByteArray  bytes = pattern(500000, 61);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("noseek.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 2, 3000);
    const fakets3::Transfer whole = fakets3::checkDownloads().at(1);
    QVERIFY(whole.overwrite);
    QVERIFY(!whole.resume);
    QVERIFY(fakets3::logContains(QStringLiteral("Couldn't read the end of ") + upload.remotePath));
    QVERIFY(fakets3::messages().isEmpty());
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 5000); // hashed off the GUI thread
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + upload.remotePath + QStringLiteral(" (content checked: full) after ")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByFull), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);
}

void TestCoreSha::largeFileWithoutSeekGoesAfterTheSize()
{
    // ... and a file larger than 16 MB isn't downloaded whole: it goes after the size check, and the log
    // says the content couldn't be checked.
    fakets3::enableFileInfo(true);
    fakets3::setResumeSupported(false);
    const QByteArray  bytes = pattern(17 * 1024 * 1024, 62);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("large.bin"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 5000);
    answerInfo(0, bytes.size());
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 5000);
    QCOMPARE(fakets3::checkDownloads().size(), 1);
    QVERIFY(fakets3::logContains(QStringLiteral("Content check of ") + upload.remotePath + QStringLiteral(" not possible (too large to download whole)")));
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + upload.remotePath + QStringLiteral(" (size only) after ")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentSizeOnly), 1);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);
}

void TestCoreSha::refusedSizeIsCheckedByItsBytes()
{
    // No permission to look at the server's files, but to download them: the bytes tell.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(120000, 63);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("perm.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, 0, true, {}, true);
    QVERIFY(fakets3::logContains(QStringLiteral("checking its bytes instead")));
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("(content checked: tail)")));
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
}

void TestCoreSha::busyFileIsStillArriving()
{
    // A server that won't hand out a file while its upload is still being written ("file is currently
    // not available, try again later"): that is other bytes for now, checked again the same way.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(200000, 67);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("busy.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    m_core->onServerError(fakets3::kConnection, ERROR_file_already_in_use, fakets3::checkDownloads().first().returnCode, QStringLiteral("file already in use"),
                          false);
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")), 3000);
    QVERIFY(fakets3::messages().isEmpty());
    fakets3::Transfer again;
    serveCheck(1, &again);
    if (QTest::currentTestFailed())
        return;
    QVERIFY(again.resume); // the same way: its tail
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
}

void TestCoreSha::cancelDuringTheTailCheck()
{
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(400000, 64);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("cancelcheck.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    const fakets3::Transfer check = fakets3::checkDownloads().first();
    QVERIFY(m_core->canCancelUpload(id));
    m_core->cancelUpload(id);
    QCOMPARE(m_core->upload(id)->state, UploadState::Canceled);
    QVERIFY(fakets3::transfer(check.id).halted); // the check's download stops
    QVERIFY(!fakets3::hasServerFile(upload.remotePath));
    QVERIFY(fakets3::deletedWhileChecking().isEmpty()); // ... before the file is deleted
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);
    // Its late answers change nothing.
    m_core->onTransferStatus(check.id, ERROR_file_transfer_canceled, QString(), fakets3::kConnection);
    complete(check.id);
    QTest::qWait(700);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(fakets3::checkDownloads().size(), 1);
    QCOMPARE(m_core->upload(id)->state, UploadState::Canceled);
}

void TestCoreSha::disconnectDuringTheTailCheck()
{
    // The connection drops while the end is read back: the message fails like any other, the check's
    // download stops; Retry asks again (size and bytes) and posts it without a second upload.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(400000, 65);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("cutcheck.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    const fakets3::Transfer check = fakets3::checkDownloads().first();
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    QCOMPARE(m_core->upload(id)->state, UploadState::Failed);
    QVERIFY(m_core->upload(id)->uploaded);
    QVERIFY(fakets3::transfer(check.id).halted);
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(serverCheckDir()).isEmpty(), 3000);

    fakets3::setConnected(true);
    QCOMPARE(m_core->retryUpload(id), id);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    answerInfo(1, bytes.size());
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QCOMPARE(fakets3::uploads().size(), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentByTail), 1);
}

void TestCoreSha::shutdownDuringTheTailCheck()
{
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(400000, 66);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("quit.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    const fakets3::Transfer check = fakets3::checkDownloads().first();
    QVERIFY(QFileInfo::exists(serverCheckDir()));
    m_core.reset(); // plugin shutdown
    QVERIFY(fakets3::transfer(check.id).halted);
    QVERIFY(!QFileInfo::exists(serverCheckDir()));
    QVERIFY(fakets3::messages().isEmpty());

    // Leftovers of a session that ended abruptly go at the next start.
    QVERIFY(writeFile(serverCheckDir() + QStringLiteral("/7_1/left.zip"), QByteArray("x")));
    newCore();
    QVERIFY(!QFileInfo::exists(serverCheckDir()));
}

void TestCoreSha::serverVersionIsLoggedOnce()
{
    // For the log and the diagnostic info only: which server this is, once per connection.
    m_core->noteServer(fakets3::kConnection);
    m_core->noteServer(fakets3::kConnection);
    QCOMPARE(logCount(QStringLiteral("Server: 3.13.7 [Build: 1655727713] on Linux")), 1);
    QVERIFY(!fakets3::logContains(QStringLiteral("TeaSpeak")));
    QCOMPARE(m_core->serverCheckDiagnostics(), QStringList{QStringLiteral("Servers: 3.13.7 [Build: 1655727713] on Linux")});

    // Another connection on the same tab: noted again (a TeaSpeak server this time).
    fakets3::setConnected(false);
    m_core->onConnectionLost(fakets3::kConnection);
    QVERIFY(m_core->serverCheckDiagnostics().isEmpty());
    fakets3::setConnected(true);
    fakets3::setServerVersion(QStringLiteral("TeaSpeak 1.4.22-beta2 [Build: 4711]"), QStringLiteral("Linux"));
    m_core->noteServer(fakets3::kConnection);
    QVERIFY(fakets3::logContains(QStringLiteral("Server: TeaSpeak 1.4.22-beta2 [Build: 4711] on Linux (looks like TeaSpeak")));
    QCOMPARE(m_core->serverCheckDiagnostics(), QStringList{QStringLiteral("Servers: TeaSpeak 1.4.22-beta2 [Build: 4711] on Linux (TeaSpeak)")});
    QVERIFY(fileverify::diagnosticLines().join(QLatin1Char('\n')).contains(QStringLiteral("Server copies of sent files checked")));
}

void TestCoreSha::tailCheckReachesTheLastData()
{
    // A file whose own end is zeros (a WAV ending in silence, a BMP with black rows on top): on a
    // preallocating server its last 64 KB look right before anything arrived. The check reaches back to
    // its last non-zero byte, so the message still waits for the data.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(200000, 75) + QByteArray(100000, '\0');
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("padded.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    fakets3::putFile(upload.remotePath, zeroAfter(bytes, 150000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 1, 3000);
    const fakets3::Transfer first  = fakets3::checkDownloads().first();
    const QString           stub   = first.localDir + QLatin1Char('/') + QFileInfo(upload.remotePath).fileName();
    const QByteArray        window = uploadwait::tailWindow(bytes.right(uploadwait::kMaxTailWindowBytes), static_cast<quint64>(bytes.size()));
    QVERIFY(window.size() >= 100000 + 65536);
    QCOMPARE(QFileInfo(stub).size(), qint64(bytes.size() - window.size()));
    QVERIFY(!fakets3::deliverServerFile(first.id).isEmpty());
    complete(first.id);
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")), 3000);
    QVERIFY(fakets3::messages().isEmpty());

    fakets3::putFile(upload.remotePath, bytes);
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + upload.remotePath + QStringLiteral(" (content checked: tail) after ")));
}

void TestCoreSha::serverIgnoringTheOffsetIsCheckedWhole()
{
    // A server that sends a resumed download from the file's start (TeamSpeak stops at the length it
    // asked for): the tail comes back with the right length but other bytes, not zeros. That is no
    // upload still arriving (which would wait a minute for nothing): the whole file is checked at once.
    fakets3::enableFileInfo(true);
    fakets3::setResumeFromStart(true);
    const QByteArray  bytes = pattern(500000, 76);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("fromstart.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::checkDownloads().size(), 2, 3000);
    const fakets3::Transfer whole = fakets3::checkDownloads().at(1);
    QVERIFY(whole.overwrite);
    QVERIFY(!whole.resume);
    QVERIFY(fakets3::logContains(QStringLiteral("Couldn't read the end of ") + upload.remotePath + QStringLiteral(" back from the server (it came back with bytes from elsewhere)")));
    QVERIFY(!fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")));
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 5000);
    QVERIFY(fakets3::logContains(QStringLiteral("(content checked: full)")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ContentWaited), 0);
}

void TestCoreSha::partialServerIsWaitedFor()
{
    // The server can't tell the file's size and sends what it has so far (it doesn't preallocate): the
    // tail comes back shorter than asked for. That is still arriving, not a server that can't seek.
    fakets3::enableFileInfo(true);
    const QByteArray  bytes = pattern(300000, 77);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("partial.zip"), bytes, bytes.size() - 20000, &upload, &id);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, 0, true, {}, true);
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")), 3000);
    QVERIFY(fakets3::messages().isEmpty());
    fakets3::Transfer again;
    fakets3::putFile(upload.remotePath, bytes);
    serveCheck(1, &again);
    if (QTest::currentTestFailed())
        return;
    QVERIFY(again.resume); // the same way: its tail
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("(content checked: tail)")));
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
}

// ---- 2.3.2: preallocating servers (TeaSpeak), receiver ----------------------------------------------

void TestCoreSha::zeroTailedDownloadWaitsAndRecovers()
{
    // The incident: the download has the full size, valid up to 150000, zeros after (a TeaSpeak server
    // still receiving the upload). It waits and downloads again instead of "File doesn't match what was
    // sent", and the server's size (always the whole file there) is no reason to stop waiting.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_0e0e0e0e.png");
    const QByteArray bytes  = pattern(400000, 70);
    fakets3::putFile(remote, zeroAfter(bytes, 150000));
    const MediaLink link   = linkTo(remote, bytes, hashing::sha256(bytes));
    bool            failed = false;
    const auto      watching = connect(m_core.get(), &Core::entryChanged, this, [&](const QString& key) {
        if (const MediaEntry* e = m_core->entry(key))
            failed = failed || e->state == MediaState::Failed;
    });
    const auto stopWatching = qScopeGuard([watching] { QObject::disconnect(watching); });

    m_core->ensure(link);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, kCheckTimeoutMs);
    QVERIFY(fakets3::logContains(remote + QStringLiteral(" doesn't match its SHA-256 yet: it ends in ")));
    QVERIFY(!fakets3::logContains(QStringLiteral("SHA-256 mismatch for ") + remote));
    QCOMPARE(fileverify::counted(fileverify::Counter::ZeroTails), 1);
    QVERIFY(filesUnder(cacheDir()).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size()); // preallocated: the whole size

    fakets3::putFile(remote, zeroAfter(bytes, 225000));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000); // after a pause
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(fileverify::counted(fileverify::Counter::ZeroTails), 2, kCheckTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 2, 3000);
    QVERIFY(entry(link)->waitingForUpload);
    answerInfo(1, bytes.size());

    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 3, kCheckTimeoutMs); // a longer pause
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);
    QVERIFY(!failed);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRecovered), 1);
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 0);
}

void TestCoreSha::zeroTailThatNeverFillsIsIncomplete()
{
    // The upload never fills the file (the sender left): downloads that bring no more data end it as
    // "never arrived completely" once nothing grew for kZeroTailStallMs (the fifth download, about 30 s
    // in), retryable. The server's copy stayed the same while the server reported the whole file: that
    // digest is the server file's now (forged-hash guard).
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_0f0f0f0f.png");
    const QByteArray bytes  = pattern(200000, 71);
    fakets3::putFile(remote, zeroAfter(bytes, 120000));
    // A link naming the same file with another hash, held back by the data saver before anything is known.
    Settings::instance().dataSaver = true;
    const MediaLink third = linkTo(remote, bytes, hashing::sha256("third"));
    m_core->ensure(third);
    QCOMPARE(entry(third)->state, MediaState::Idle);
    Settings::instance().dataSaver = false;

    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    for (int download = 1; download <= 5; ++download) {
        QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), download, 20000);
        QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
        complete(lastDownloadId());
        QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), download, kCheckTimeoutMs);
        answerInfo(download - 1, bytes.size());
    }
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, 3000);
    QCOMPARE(entry(link)->error, MediaError::Other);
    QVERIFY(entry(link)->errorText.contains(QStringLiteral("never arrived completely")));
    QVERIFY(isRetryableDownload(*entry(link)));
    QVERIFY(fakets3::logContains(QStringLiteral("still ends in zeros after")));
    QVERIFY(fakets3::logContains(QStringLiteral("stayed the same; later links to it with another SHA-256")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ZeroTails), 5);
    QTest::qWait(300);
    QCOMPARE(fakets3::downloads().size(), 5);

    // Links naming this file with ever new made-up hashes aren't downloaded again and again: refused at
    // once (a file padded with zeros on purpose would otherwise cost five downloads per link).
    const MediaLink other = linkTo(remote, bytes, hashing::sha256("another"));
    m_core->ensure(other);
    QCOMPARE(entry(other)->state, MediaState::Failed);
    QCOMPARE(entry(other)->error, MediaError::Mismatch);
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 1);
    QCOMPARE(fakets3::downloads().size(), 5);

    // Clear cache keeps it as it was: loaded again automatically it would only be refused, as "File
    // doesn't match what was sent".
    m_core->clearCache();
    QCOMPARE(entry(link)->state, MediaState::Failed);
    QCOMPARE(entry(link)->error, MediaError::Other);
    QVERIFY(entry(link)->errorText.contains(QStringLiteral("never arrived completely")));
    QTest::qWait(300);
    QCOMPARE(fakets3::downloads().size(), 5);

    // A click on the first one still downloads it (the upload may have arrived meanwhile).
    fakets3::putFile(remote, bytes);
    m_core->retry(link.key());
    QCOMPARE(fakets3::downloads().size(), 6);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);

    // The link held back before the file's digest was noted, which the guard contradicts now: a click
    // gets its one attempt, as from Failed.
    QCOMPARE(entry(third)->state, MediaState::Idle);
    m_core->download(third.key(), false);
    QCOMPARE(entry(third)->state, MediaState::Downloading);
    QCOMPARE(fakets3::downloads().size(), 7);
}

void TestCoreSha::shaLessZeroTailWaits()
{
    // A TS Media link without a checksum: its file ending in zeros is looked at closer (off the GUI
    // thread) and waited for the same way.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/notes_1d1d1d1d.png");
    const QByteArray bytes  = pattern(160000, 72);
    fakets3::putFile(remote, zeroAfter(bytes, 90000));
    const MediaLink link = linkTo(remote, bytes, QByteArray());
    QVERIFY(link.isTsMedia());
    m_core->ensure(link);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("the sender's upload may still be arriving")));
    QCOMPARE(fileverify::counted(fileverify::Counter::ZeroTails), 1);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    fakets3::putFile(remote, bytes);
    answerInfo(0, bytes.size());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, 3000);
    QVERIFY(!entry(link)->check.verified);
    QCOMPARE(readFile(entry(link)->localPath), bytes);
}

void TestCoreSha::mediaNeedsALongerZeroRun()
{
    // Without a checksum, a video may end in a shorter run of zeros of its own (64 KB are needed), and a
    // plain TeamSpeak link is never looked at: both are cached as before.
    fakets3::enableFileInfo(true);
    const QString remote = QStringLiteral("/tsmedia/clip_2e2e2e2e.mp4");
    QByteArray    video  = pattern(300000, 73);
    video.replace(300000 - 8192, 8192, QByteArray(8192, '\0'));
    fakets3::putFile(remote, video);
    const MediaLink videoLink = linkTo(remote, video, QByteArray());
    m_core->ensure(videoLink);
    m_core->download(videoLink.key(), false);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(videoLink)->state, MediaState::Ready, 3000);
    QCOMPARE(readFile(entry(videoLink)->localPath), video);

    const QString plainRemote = QStringLiteral("/shared/scan.png");
    const QByteArray plain    = zeroAfter(pattern(120000, 74), 60000);
    fakets3::putFile(plainRemote, plain);
    MediaLink plainLink = linkTo(plainRemote, plain, QByteArray());
    plainLink.protocol  = 0;
    plainLink           = MediaLink::parse(plainLink.toUrl());
    m_core->ensure(plainLink);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(plainLink)->state, MediaState::Ready, 3000);
    QVERIFY(fakets3::fileInfoRequests().isEmpty());
    QCOMPARE(fileverify::counted(fileverify::Counter::ZeroTails), 0);
}

void TestCoreSha::shaLessFileEndingInZerosIsKept()
{
    // A link without a sha (a 2.1 sender) to a file that really ends in zeros (a tar's padding): it is
    // waited for like an upload still arriving, but once two more downloads bring no more data, nothing
    // contradicts it: kept as it is (before, it failed as "never arrived completely" for good).
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/backup_3a3a3a3a.tar");
    const QByteArray file   = pattern(150000, 78) + QByteArray(10240, '\0');
    fakets3::putFile(remote, file);
    const MediaLink link = linkTo(remote, file, QByteArray());
    m_core->ensure(link);
    m_core->download(link.key(), false);
    for (int download = 1; download <= 2; ++download) {
        QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), download, 2 * kCheckTimeoutMs);
        QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
        complete(lastDownloadId());
        QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), download, kCheckTimeoutMs);
        QVERIFY(entry(link)->waitingForUpload);
        answerInfo(download - 1, file.size());
    }
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 3, 2 * kCheckTimeoutMs);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QCOMPARE(readFile(entry(link)->localPath), file);
    QVERIFY(!entry(link)->check.verified);
    QVERIFY(fakets3::logContains(QStringLiteral("nothing contradicts it (no checksum), so they are its own end")));
    QCOMPARE(fakets3::fileInfoRequests().size(), 2);
    QCOMPARE(fileverify::counted(fileverify::Counter::DownloadsRecovered), 0);
}

void TestCoreSha::shortZeroRunOfAMismatchWaits()
{
    // A download that didn't match its SHA-256 and ends in only 3 KB of zeros (the last bytes of the
    // upload still on their way to a preallocating server): an upload still arriving too, never a
    // mismatch that the server's whole size would seem to confirm.
    fakets3::enableFileInfo(true);
    const QString    remote = QStringLiteral("/tsmedia/photo_4b4b4b4b.png");
    const QByteArray bytes  = pattern(300000, 79);
    fakets3::putFile(remote, zeroAfter(bytes, bytes.size() - 3000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, kCheckTimeoutMs);
    QVERIFY(fakets3::logContains(remote + QStringLiteral(" doesn't match its SHA-256 yet: it ends in ")));
    QVERIFY(!fakets3::logContains(QStringLiteral("SHA-256 mismatch for ") + remote));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::fileInfoRequests().size(), 1, 3000);
    answerInfo(0, bytes.size());
    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, 5000);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QCOMPARE(fileverify::counted(fileverify::Counter::ZeroTails), 1);
}

// ---- 2.3.3: TeaSpeak drops the connection when asked for a file's size ---------------------------

void TestCoreSha::teaSpeakSenderIsNeverAskedForASize()
{
    // TeaSpeak answers requestFileInfo with a notification TeamSpeak can't parse, and the client drops
    // the connection (seen live on TeaSpeak 1.5.6): the message waits for the file's bytes alone.
    fakets3::enableFileInfo(true);
    fakets3::setServerVersion(QStringLiteral("TeaSpeak 1.5.6-beta [Build: 1622043432]"), QStringLiteral("Linux"));
    const QByteArray  bytes = pattern(300000, 80);
    fakets3::Transfer upload;
    int               id = 0;
    sendArriving(QStringLiteral("teaspeak.zip"), bytes, bytes.size(), &upload, &id);
    if (QTest::currentTestFailed())
        return;
    fakets3::putFile(upload.remotePath, zeroAfter(bytes, 150000)); // preallocated, still arriving
    serveCheck(0);
    if (QTest::currentTestFailed())
        return;
    QTRY_VERIFY_WITH_TIMEOUT(fakets3::logContains(QStringLiteral("doesn't end like the uploaded file yet")), 3000);
    QVERIFY(fakets3::messages().isEmpty());
    fakets3::putFile(upload.remotePath, bytes);
    serveCheck(1);
    if (QTest::currentTestFailed())
        return;
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::messages().size(), 1, 3000);
    QVERIFY(fakets3::logContains(QStringLiteral("(content checked: tail)")));
    QVERIFY(fakets3::logContains(QStringLiteral("file sizes aren't asked")));
    QVERIFY(fakets3::fileInfoRequests().isEmpty());
    QCOMPARE(fileverify::counted(fileverify::Counter::UploadsUnconfirmed), 0);
}

void TestCoreSha::teaSpeakReceiverIsNeverAskedForASize()
{
    // A file still arriving at a TeaSpeak server: the receiver waits and downloads it again without
    // asking the server for its size.
    fakets3::enableFileInfo(true);
    fakets3::setServerVersion(QStringLiteral("TeaSpeak 1.4.21-beta [Build: 1600246831]"), QStringLiteral("Linux"));
    const QString    remote = QStringLiteral("/tsmedia/photo_5c5c5c5c.png");
    const QByteArray bytes  = pattern(400000, 81);
    fakets3::putFile(remote, zeroAfter(bytes, 150000));
    const MediaLink link = linkTo(remote, bytes, hashing::sha256(bytes));
    m_core->ensure(link);
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_VERIFY_WITH_TIMEOUT(entry(link)->waitingForUpload, kCheckTimeoutMs);
    fakets3::putFile(remote, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 2, kCheckTimeoutMs); // after a pause
    QVERIFY(!fakets3::deliverServerFile(lastDownloadId()).isEmpty());
    complete(lastDownloadId());
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(link)->check.verified);
    QVERIFY(fakets3::logContains(QStringLiteral("The server can't tell how much of ") + remote));
    QVERIFY(fakets3::fileInfoRequests().isEmpty());
}

TSMEDIA_REGISTER_TEST(TestCoreSha)

#include "tst_coresha.moc"
