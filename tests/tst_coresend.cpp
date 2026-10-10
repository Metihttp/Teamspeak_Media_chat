// 2.2 sending, end to end inside Core (core.cpp against the fake TeamSpeak of fakets3.*): the folder and
// upload requests go through the connection's FloodGovernor and come back after a flood instead of
// failing; the messages of one send keep their order; a retry leaves the caption with the album that is
// still on its way; uploaded files held for their album can be retried after a lost connection; a
// failed album message warns once; Core says when a caption is in the chat.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

#include "core.h"
#include "fakets3.h"
#include "fileverify.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"

namespace {

constexpr int kTimeoutMs = 10000;

QByteArray pattern(int size, int seed)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 8) + seed) & 0xff);
    return data;
}

int linksIn(const fakets3::Message& message)
{
    return MediaLink::findInMessage(QString::fromUtf8(message.text)).size();
}

// Chat warnings (printed into the chat, not only logged) that contain text.
int chatWarnings(const QString& text)
{
    int count = 0;
    for (const QString& line : fakets3::logLines())
        count += line.startsWith(QLatin1String("[chat]")) && line.contains(text) ? 1 : 0;
    return count;
}

} // namespace

class TestCoreSend : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void foldersAreAskedOnce();
    void floodedRequestsGoAgain();
    void retryLeavesCaptionWithAlbum();
    void heldAlbumFilesRetryAfterDisconnect();
    void failedAlbumMessageWarnsOnce();
    void postsOfOneSendKeepTheirOrder();
    void captionSettles();
    void replyLeadGoesFirst(); // 2.2 reply

  private:
    QString    file(const QString& name, int seed = 1);
    ChatTarget channel() const;
    // What the fake server answers since the last call: folder requests Ok, uploads complete, chat
    // messages Ok (unless m_answerMessages is off). Transfers in m_skip are left alone.
    void serve();
    bool serveUntil(const std::function<bool()>& done, int timeoutMs = kTimeoutMs);
    int  jobsIn(UploadState state, bool waiting) const;
    int  firstJobIn(UploadState state) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    QSet<QString>                  m_answered;
    QSet<anyID>                    m_completed;
    QSet<anyID>                    m_skip;
    bool                           m_answerMessages = true;
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
};

void TestCoreSend::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
}

void TestCoreSend::cleanupTestCase()
{
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
    fileverify::resetCounters();
}

void TestCoreSend::init()
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
    m_answered.clear();
    m_completed.clear();
    m_skip.clear();
    m_answerMessages = true;
    m_core           = std::make_unique<Core>();
    m_core->start();
}

void TestCoreSend::cleanup()
{
    m_core.reset();
    m_dir.reset();
}

QString TestCoreSend::file(const QString& name, int seed)
{
    const QString path = m_dir->path() + QStringLiteral("/outside/") + name;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    out.write(pattern(20000 + seed * 7, seed));
    return path;
}

ChatTarget TestCoreSend::channel() const
{
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    return target;
}

void TestCoreSend::serve()
{
    for (const fakets3::Directory& d : fakets3::directories()) {
        if (m_answered.contains(d.returnCode))
            continue;
        m_answered.insert(d.returnCode);
        m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
    }
    for (const fakets3::Transfer& t : fakets3::uploads()) {
        if (t.halted || m_completed.contains(t.id) || m_skip.contains(t.id))
            continue;
        m_completed.insert(t.id);
        if (fakets3::completeUpload(t.id))
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    if (!m_answerMessages)
        return;
    for (const fakets3::Message& message : fakets3::messages()) {
        if (m_answered.contains(message.returnCode))
            continue;
        m_answered.insert(message.returnCode);
        m_core->onServerError(fakets3::kConnection, ERROR_ok, message.returnCode, QString(), false);
    }
}

bool TestCoreSend::serveUntil(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        serve();
        if (done())
            return true;
        QTest::qWait(20);
    }
    serve();
    return done();
}

int TestCoreSend::jobsIn(UploadState state, bool waiting) const
{
    int count = 0;
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        count += job && job->state == state && job->waiting == waiting ? 1 : 0;
    }
    return count;
}

int TestCoreSend::firstJobIn(UploadState state) const
{
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        if (job && job->state == state)
            return id;
    }
    return 0;
}

// ---- pacing ---------------------------------------------------------------------------------------

void TestCoreSend::foldersAreAskedOnce()
{
    // Every folder request costs flood points (S0): the upload folder is asked for while it isn't
    // known, then never again on this connection.
    Settings::instance().uploadDirectory = QStringLiteral("/tsmedia");
    m_core->uploadFiles({file(QStringLiteral("a.zip"), 1), file(QStringLiteral("b.zip"), 2), file(QStringLiteral("c.zip"), 3)}, channel());
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 3; }));
    const int asked = fakets3::directories().size();
    QVERIFY(asked >= 1);
    for (const fakets3::Directory& d : fakets3::directories())
        QCOMPARE(d.path, QStringLiteral("/tsmedia"));
    for (const fakets3::Transfer& t : fakets3::uploads())
        QVERIFY2(t.remotePath.startsWith(QLatin1String("/tsmedia/")), qPrintable(t.remotePath));

    m_core->uploadFiles({file(QStringLiteral("d.zip"), 4), file(QStringLiteral("e.zip"), 5)}, channel());
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 5; }));
    QCOMPARE(fakets3::directories().size(), asked);

    // A new connection may have another server behind it: asked again.
    m_core->onConnectionLost(fakets3::kConnection);
    m_core->uploadFiles({file(QStringLiteral("f.zip"), 6)}, channel());
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 6; }));
    QCOMPARE(fakets3::directories().size(), asked + 1);
}

void TestCoreSend::floodedRequestsGoAgain()
{
    // 0x020c on the folder request or the upload start: both go again after the server's pause. The
    // file neither fails nor lands in the channel's root instead of its folder.
    Settings::instance().uploadDirectory = QStringLiteral("/tsmedia");
    m_core->uploadFiles({file(QStringLiteral("a.zip"))}, channel());
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::directories().size(), 1, kTimeoutMs);
    const int id = m_core->uploadIds().last();
    m_core->onServerError(fakets3::kConnection, ERROR_client_is_flooding, fakets3::directories().first().returnCode, QStringLiteral("client is flooding"), false,
                          QStringLiteral("retry in 200ms"));
    QCOMPARE(fakets3::directories().size(), 1); // not at once: the pause first
    QVERIFY(m_core->upload(id) && m_core->upload(id)->state == UploadState::Preparing);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::directories().size(), 2, kTimeoutMs);
    QCOMPARE(fakets3::directories().at(1).path, QStringLiteral("/tsmedia"));
    for (const fakets3::Directory& d : fakets3::directories())
        m_answered.insert(d.returnCode);
    m_core->onServerError(fakets3::kConnection, ERROR_ok, fakets3::directories().at(1).returnCode, QString(), false);

    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 1, kTimeoutMs);
    const fakets3::Transfer first = fakets3::uploads().first();
    QVERIFY(first.remotePath.startsWith(QLatin1String("/tsmedia/")));
    m_skip.insert(first.id);
    m_core->onServerError(fakets3::kConnection, ERROR_client_is_flooding, first.returnCode, QStringLiteral("client is flooding"), false, QStringLiteral("retry in 200ms"));
    QVERIFY(m_core->upload(id) && m_core->upload(id)->state == UploadState::Preparing); // waits, doesn't fail
    QCOMPARE(fakets3::uploads().size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, kTimeoutMs);
    QCOMPARE(fakets3::uploads().at(1).remotePath, first.remotePath);

    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    QVERIFY(serveUntil([&] { return m_core->upload(id) && m_core->upload(id)->state == UploadState::Done; }));
    QCOMPARE(chatWarnings(QStringLiteral("Couldn")), 0);
}

void TestCoreSend::retryLeavesCaptionWithAlbum()
{
    // An album with a caption; one photo fails while the others still upload and is retried: the
    // caption stays with the album (which still posts), the retried photo goes without it.
    SendRequest request;
    request.target  = channel();
    request.caption = QStringLiteral("trip");
    request.album   = true;
    for (int i = 1; i <= 3; ++i) {
        SendItem item;
        item.path = file(QStringLiteral("p%1.jpg").arg(i), i);
        request.items.append(item);
    }
    QVERIFY(m_core->send(request) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, kTimeoutMs); // two at a time
    const fakets3::Transfer second = fakets3::uploads().at(1);
    m_skip.insert(second.id);
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, second.returnCode, QStringLiteral("refused"), false);
    const int failed = firstJobIn(UploadState::Failed);
    QVERIFY(failed != 0);
    QVERIFY(m_core->canRetryUpload(failed));
    QVERIFY(m_core->retryUpload(failed) != 0);

    QVERIFY(serveUntil([] { return fakets3::messages().size() == 2; }));
    QTest::qWait(200);
    serve();
    QCOMPARE(fakets3::messages().size(), 2);
    int withCaption = 0;
    for (const fakets3::Message& message : fakets3::messages()) {
        const bool caption = QString::fromUtf8(message.text).startsWith(QLatin1String("trip"));
        withCaption += caption ? 1 : 0;
        QCOMPARE(linksIn(message), caption ? 2 : 1); // the album of the two others; the retried photo alone
    }
    QCOMPARE(withCaption, 1);
}

void TestCoreSend::heldAlbumFilesRetryAfterDisconnect()
{
    // Two photos of an album are uploaded and wait for the third when the connection drops: they fail
    // with one chat line, and Retry posts them (they are on the server already).
    SendRequest request;
    request.target = channel();
    request.album  = true;
    for (int i = 1; i <= 3; ++i) {
        SendItem item;
        item.path = file(QStringLiteral("h%1.jpg").arg(i), i);
        request.items.append(item);
    }
    QVERIFY(m_core->send(request) != 0);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, kTimeoutMs);
    for (const fakets3::Transfer& t : fakets3::uploads()) {
        QVERIFY(fakets3::completeUpload(t.id));
        m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    QTRY_COMPARE_WITH_TIMEOUT(jobsIn(UploadState::Posting, true), 2, kTimeoutMs); // "Waiting for the rest of the album…"
    const int before = chatWarnings(QStringLiteral("Couldn"));
    m_core->onConnectionLost(fakets3::kConnection);
    QCOMPARE(jobsIn(UploadState::Failed, false), 3);
    QCOMPARE(chatWarnings(QStringLiteral("Couldn")) - before, 1);
    QVERIFY(fakets3::messages().isEmpty());

    QVector<int> held;
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        if (job && job->uploaded)
            held.append(id);
    }
    QCOMPARE(held.size(), 2);
    for (const int id : held)
        QVERIFY(m_core->canRetryUpload(id));
    QVERIFY(m_core->retryUpload(held.first()) != 0);
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    QCOMPARE(linksIn(fakets3::messages().first()), 2); // one message announces both
    QVERIFY(serveUntil([&] { return m_core->upload(held.at(0))->state == UploadState::Done && m_core->upload(held.at(1))->state == UploadState::Done; }));
}

void TestCoreSend::failedAlbumMessageWarnsOnce()
{
    SendRequest request;
    request.target = channel();
    request.album  = true;
    for (int i = 1; i <= 3; ++i) {
        SendItem item;
        item.path = file(QStringLiteral("w%1.jpg").arg(i), i);
        request.items.append(item);
    }
    QVERIFY(m_core->send(request) != 0);
    m_answerMessages = false;
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    QCOMPARE(linksIn(fakets3::messages().first()), 3);
    const int before = chatWarnings(QStringLiteral("Couldn"));
    m_core->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, fakets3::messages().first().returnCode, QStringLiteral("insufficient"),
                          true);
    QCOMPARE(chatWarnings(QStringLiteral("Couldn")) - before, 1);
    QCOMPARE(chatWarnings(QStringLiteral("3 files")), 1);
    QCOMPARE(jobsIn(UploadState::Failed, false), 3);
    for (const int id : m_core->uploadIds())
        QVERIFY(m_core->canRetryUpload(id));
}

void TestCoreSend::postsOfOneSendKeepTheirOrder()
{
    // Two files in one send: the second message waits for the first one's answer, also past the second
    // after which the governor would let an unrelated post go. A flood answer that comes late puts the
    // first one back in front, so the chat gets them in order.
    m_answerMessages = false;
    m_core->uploadFiles({file(QStringLiteral("first.zip"), 1), file(QStringLiteral("second.zip"), 2)}, channel());
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    QTest::qWait(1300);
    serve();
    QCOMPARE(fakets3::messages().size(), 1);
    const fakets3::Message first = fakets3::messages().first();
    QVERIFY(QString::fromUtf8(first.text).contains(QLatin1String("first_")));
    m_core->onServerError(fakets3::kConnection, ERROR_client_is_flooding, first.returnCode, QStringLiteral("client is flooding"), false, QStringLiteral("retry in 100ms"));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 2; }));
    QCOMPARE(fakets3::messages().at(1).text, first.text); // the same one again
    m_answerMessages = true;
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 3; }));
    QVERIFY(QString::fromUtf8(fakets3::messages().at(2).text).contains(QLatin1String("second_")));
}

void TestCoreSend::captionSettles()
{
    QSignalSpy settled(m_core.get(), &Core::captionSettled);
    SendRequest request;
    request.target  = channel();
    request.caption = QStringLiteral("hello");
    SendItem item;
    item.path = file(QStringLiteral("posted.zip"), 1);
    request.items.append(item);
    const int batch = m_core->send(request);
    QVERIFY(batch != 0);
    QVERIFY(serveUntil([&settled] { return settled.count() == 1; }));
    QCOMPARE(settled.at(0).at(0).toInt(), batch);
    QCOMPARE(settled.at(0).at(1).toBool(), true);
    QVERIFY(QString::fromUtf8(fakets3::messages().first().text).startsWith(QLatin1String("hello")));

    // Its only file fails: nothing is said until the job is gone (Retry could still post the caption).
    request.caption = QStringLiteral("lost");
    request.items.first().path = file(QStringLiteral("refused.zip"), 2);
    const int lost = m_core->send(request);
    QVERIFY(lost != 0);
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::uploads().size(), 2, kTimeoutMs);
    const fakets3::Transfer refused = fakets3::uploads().at(1);
    m_skip.insert(refused.id);
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, refused.returnCode, QStringLiteral("refused"), false);
    const int failed = firstJobIn(UploadState::Failed);
    QVERIFY(failed != 0);
    QCOMPARE(settled.count(), 1);
    m_core->dismissUpload(failed);
    QCOMPARE(settled.count(), 2);
    QCOMPARE(settled.at(1).at(0).toInt(), lost);
    QCOMPARE(settled.at(1).at(1).toBool(), false);
}

// 2.2 reply: files sent as a reply carry the quote line first, in the same message as the caption and
// the first link (an album: its one message); the quote line is posted once.
void TestCoreSend::replyLeadGoesFirst()
{
    const QString lead = QStringLiteral("[i]↪ Alice: “hi”[/i]");
    SendRequest   request;
    request.target    = channel();
    request.caption   = QStringLiteral("look");
    request.replyLead = lead;
    SendItem item;
    item.path = file(QStringLiteral("reply.zip"), 1);
    request.items.append(item);
    QVERIFY(m_core->send(request) != 0);
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    QVERIFY(QString::fromUtf8(fakets3::messages().first().text).startsWith(lead + QStringLiteral("\nlook\n[URL=ts3file://")));

    SendRequest album;
    album.target    = channel();
    album.album     = true;
    album.replyLead = lead;
    for (int i = 1; i <= 3; ++i) {
        SendItem photo;
        photo.path = file(QStringLiteral("r%1.jpg").arg(i), i + 1);
        album.items.append(photo);
    }
    album.items.append(item); // and a file after the album: no second quote line
    album.items.last().path = file(QStringLiteral("after.zip"), 9);
    QVERIFY(m_core->send(album) != 0);
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 3; }));
    const QString albumMessage = QString::fromUtf8(fakets3::messages().at(1).text);
    QVERIFY(albumMessage.startsWith(lead + QStringLiteral("\n[URL=ts3file://")));
    QCOMPARE(linksIn(fakets3::messages().at(1)), 3);
    QVERIFY(!QString::fromUtf8(fakets3::messages().at(2).text).contains(QChar(0x21AA)));
}

TSMEDIA_REGISTER_TEST(TestCoreSend)

#include "tst_coresend.moc"
