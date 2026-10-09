// 2.2 sha, end to end inside Core: Core itself (core.cpp, ts3api.cpp, the real hashing and file system)
// runs against a fake TeamSpeak (fakets3.*). Covers what the design asks of the receiver (match ->
// cached and marked verified; mismatch -> deleted, final, both hashes logged; the 2.0.5 write race
// re-checked once; made-up hashes refused without a download; previews checked against ph; cached
// files not hashed again) and of the sender (the posted sha is the uploaded bytes').

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "core.h"
#include "fakets3.h"
#include "fileverify.h"
#include "hashing.h"
#include "settings.h"
#include "testmain.h"

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

} // namespace

class TestCoreSha : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();

    // receiver
    void verifiedDownloadIsCached();
    void mismatchIsBlockedAndFinal();
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

  private:
    void newCore();
    anyID lastDownloadId() const;
    void  complete(anyID id);
    // The newest download: delivers bytes and reports it complete.
    void  deliverAndComplete(const QByteArray& bytes);
    const MediaEntry* entry(const MediaLink& link) const { return m_core->entry(link.key()); }
    QString cacheDir() const { return m_dir->path() + QStringLiteral("/plugins/tsmedia/cache"); }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
};

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

void TestCoreSha::mismatchIsBlockedAndFinal()
{
    const QString    remote   = QStringLiteral("/tsmedia/photo_1234abcd.png");
    const QByteArray sent     = pattern(200000, 1);
    const QByteArray replaced = pattern(200000, 2); // same size: the size check can't tell
    fakets3::putFile(remote, replaced);
    const MediaLink link = linkTo(remote, sent, hashing::sha256(sent));

    m_core->ensure(link);
    deliverAndComplete(replaced);
    QTRY_COMPARE_WITH_TIMEOUT(entry(link)->state, MediaState::Failed, kCheckTimeoutMs);
    const MediaEntry* e = entry(link);
    QCOMPARE(e->error, MediaError::Mismatch);
    QCOMPARE(e->errorText, downloadErrorText(MediaError::Mismatch));
    QCOMPARE(e->check.received, hashing::sha256(replaced));
    QVERIFY(!e->check.verified);
    QVERIFY(!QFileInfo::exists(e->localPath));
    QVERIFY(filesUnder(cacheDir()).isEmpty()); // deleted: never shown, cached or saved
    QVERIFY(fakets3::logContains(QStringLiteral("SHA-256 mismatch for /tsmedia/photo_1234abcd.png: expected ") + hashing::toHex(hashing::sha256(sent))
                                 + QStringLiteral(", got ") + hashing::toHex(hashing::sha256(replaced))));
    QCOMPARE(fileverify::counted(fileverify::Counter::Mismatched), 1);

    // Final: neither a retry nor a click downloads it again.
    m_core->retry(link.key());
    m_core->download(link.key(), true);
    QTest::qWait(50);
    QCOMPARE(fakets3::downloads().size(), 1);
    QCOMPARE(entry(link)->state, MediaState::Failed);
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
        m_core->download(forged.key(), true);
    }
    QCOMPARE(fakets3::downloads().size(), 1); // only the genuine one was ever fetched
    QCOMPARE(fileverify::counted(fileverify::Counter::ForgedBlocked), 3);
    QCOMPARE(entry(genuine)->state, MediaState::Ready);
    QVERIFY(entry(genuine)->check.verified);
}

void TestCoreSha::forgedLinkFirstDoesNotPoisonGenuine()
{
    const QString    remote = QStringLiteral("/tsmedia/song_0badf00d.png");
    const QByteArray bytes  = pattern(120000, 4);
    fakets3::putFile(remote, bytes);

    // The forged link arrives first: downloaded once, doesn't match, deleted.
    const MediaLink forged = linkTo(remote, bytes, hashing::sha256("not the file"));
    m_core->ensure(forged);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(forged)->state, MediaState::Failed, kCheckTimeoutMs);
    QCOMPARE(entry(forged)->error, MediaError::Mismatch);

    // The genuine link has a key of its own: downloaded, checked, shown.
    const MediaLink genuine = linkTo(remote, bytes, hashing::sha256(bytes));
    QVERIFY(genuine.key() != forged.key());
    m_core->ensure(genuine);
    QCOMPARE(fakets3::downloads().size(), 2);
    deliverAndComplete(bytes);
    QTRY_COMPARE_WITH_TIMEOUT(entry(genuine)->state, MediaState::Ready, kCheckTimeoutMs);
    QVERIFY(entry(genuine)->check.verified);
    QCOMPARE(readFile(entry(genuine)->localPath), bytes);
    QCOMPARE(entry(forged)->state, MediaState::Failed);

    // Another made-up hash for the same file: the real digest is known now, no download.
    const MediaLink again = linkTo(remote, bytes, hashing::sha256("also not"));
    m_core->ensure(again);
    QCOMPARE(entry(again)->error, MediaError::Mismatch);
    QCOMPARE(fakets3::downloads().size(), 2);
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
    QCOMPARE(entry(link)->error, MediaError::Mismatch); // not "Not connected" (which would resume it)
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

TSMEDIA_TEST_MAIN(TestCoreSha)

#include "tst_coresha.moc"
