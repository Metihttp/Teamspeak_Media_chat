// 2.5 GIFs: sending from the tab and recording uploads, through the real Core against the fake TeamSpeak (design_gifs
// 10.3, and 10.1 T13, T21, T22): a GIF upload is recorded once its message is confirmed and goes up again under its own
// name; link items go through Core's post queue (flood retries, private partners by identity, the server chat); blocks;
// replies; the 800 ms spacing; a chat that changed since the picker opened; a refused upload remembered per channel.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

#include "core.h"
#include "fakets3.h"
#include "giflibrary.h"
#include "gifrules.h"
#include "linkfixtures.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"

namespace {

constexpr int kTimeoutMs = 10000;

QString a(const char* text)
{
    return QString::fromLatin1(text);
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const char kGiphy[] = "https://giphy.com/gifs/x-ABCdef123";

} // namespace

class TestGifSend : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void uploadRecordsAndResends();   // 1
    void uploadHook();                // T13
    void mediaPostedOnce();           // T22
    void linkSendsThroughPostQueue(); // 2
    void serverChatUploads();         // 3
    void blocks();                    // 4
    void replyLead();                 // 5
    void spacing();                   // 6
    void targetChangedSinceOpen();    // 7
    void postQueueOrder();            // 8
    void spoilerResend();             // 9
    void refusalMemory();             // T21

  private:
    ChatTarget channel() const;
    QString    gif(const QString& name, int frames = 3);
    void       serve();
    bool       serveUntil(const std::function<bool()>& done, int timeoutMs = kTimeoutMs);
    // A GIF uploaded through Core and recorded by the library (its item id).
    QString    uploadGif(const QString& name, bool spoiler = false);
    QString    saveLink(const char* url);
    gifs::Host host(ChatTarget* target, QString* lead = nullptr);
    bool       allDone() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    std::unique_ptr<GifLibrary>    m_lib;
    QSet<QString>                  m_answered;
    QSet<anyID>                    m_completed;
    bool                           m_answerMessages = true;
    bool                           m_refuseUploads  = false;
    int                            m_floodNext      = 0; // answer this many messages with 0x020c first
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
};

void TestGifSend::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
}

void TestGifSend::cleanupTestCase()
{
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
}

void TestGifSend::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    fakets3::setOwnUid(a("ownUid="));
    Settings& s           = Settings::instance();
    s                     = Settings();
    s.uploadDirectory     = a("/");
    s.generatePreviews    = false;
    s.addRequiredNotice   = false;
    s.videoAutoDownloadMB = 0;
    s.rememberGifs        = true;
    m_answered.clear();
    m_completed.clear();
    m_answerMessages = true;
    m_refuseUploads  = false;
    m_floodNext      = 0;
    m_core           = std::make_unique<Core>();
    m_core->start();
    m_lib = std::make_unique<GifLibrary>(m_core.get(), ts3::dataDir() + a("/gifs"), m_core->cacheDir() + a("/links"));
    m_lib->start();
}

void TestGifSend::cleanup()
{
    m_lib.reset();
    m_core.reset();
    m_dir.reset();
}

ChatTarget TestGifSend::channel() const
{
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    return target;
}

QString TestGifSend::gif(const QString& name, int frames)
{
    const QString path = m_dir->path() + a("/outside/") + name;
    return writeFile(path, linkfixtures::gif(frames)) ? path : QString();
}

void TestGifSend::serve()
{
    for (const fakets3::Directory& d : fakets3::directories()) {
        if (m_answered.contains(d.returnCode))
            continue;
        m_answered.insert(d.returnCode);
        m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
    }
    for (const fakets3::Transfer& t : fakets3::uploads()) {
        if (t.halted || m_completed.contains(t.id))
            continue;
        m_completed.insert(t.id);
        if (m_refuseUploads) {
            m_core->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, t.returnCode, a("insufficient client permissions"), true);
            continue;
        }
        if (fakets3::completeUpload(t.id))
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    if (!m_answerMessages)
        return;
    for (const fakets3::Message& message : fakets3::messages()) {
        if (m_answered.contains(message.returnCode))
            continue;
        m_answered.insert(message.returnCode);
        if (m_floodNext > 0) {
            --m_floodNext;
            m_core->onServerError(fakets3::kConnection, ERROR_client_is_flooding, message.returnCode, a("client is flooding"), false, a("retry in 50ms"));
            continue;
        }
        m_core->onServerError(fakets3::kConnection, ERROR_ok, message.returnCode, QString(), false);
    }
}

bool TestGifSend::serveUntil(const std::function<bool()>& done, int timeoutMs)
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

bool TestGifSend::allDone() const
{
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        if (job && job->state != UploadState::Done && job->state != UploadState::Failed && job->state != UploadState::Canceled)
            return false;
    }
    return true;
}

QString TestGifSend::uploadGif(const QString& name, bool spoiler)
{
    SendRequest request;
    request.target = channel();
    SendItem item;
    item.path     = gif(name);
    item.spoiler  = spoiler;
    request.items = {item};
    const int before = fakets3::messages().size();
    if (m_core->send(request) <= 0)
        return {};
    if (!serveUntil([&] { return fakets3::messages().size() > before && allDone(); }))
        return {};
    m_lib->waitForWorkers();
    return m_lib->count() > 0 ? m_lib->items().first().id : QString();
}

QString TestGifSend::saveLink(const char* url)
{
    QString message;
    if (!m_lib->saveLink(a(url), linkpreview::classify(a(url)), QString(), false, gifs::Kind::Loop, &message))
        return {};
    const gifs::Item* item = m_lib->findLink(linkpreview::classify(a(url)).key, QString());
    return item ? item->id : QString();
}

gifs::Host TestGifSend::host(ChatTarget* target, QString* lead)
{
    gifs::HostEnv env;
    env.resolve = [target](ChatTarget* out) {
        *out = *target;
        return true;
    };
    env.describe  = [](const ChatTarget&) { return a("the channel"); };
    env.replyLead = [lead](const ChatTarget&) { return lead ? *lead : QString(); };
    env.replySent = [lead](const ChatTarget&) {
        if (lead)
            lead->clear();
    };
    return m_lib->makeHost(env);
}

// ---- 1, T13, T22: uploads --------------------------------------------------------------------------------------

void TestGifSend::uploadRecordsAndResends()
{
    const QString id = uploadGif(a("party.gif"));
    QVERIFY(!id.isEmpty());
    const gifs::Item item = *m_lib->find(id);
    QCOMPARE(item.source, gifs::Source::File);
    QCOMPARE(item.how, gifs::How::Sent);
    QCOMPARE(item.name, a("party.gif"));
    QCOMPARE(item.sha.size(), 32);
    QCOMPARE(readFile(m_lib->copyPath(item)), linkfixtures::gif(3));
    const QList<MediaLink> sent = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().first().text));
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().sha256, item.sha);

    // Sent again from the tab: a new upload under the item's own name, the chat message a TS Media link, no second copy.
    ChatTarget       target = channel();
    const gifs::Host h      = host(&target);
    QString          error;
    QVERIFY2(h.send(id, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 2; }));
    QVERIFY(serveUntil([this] { return allDone(); }));
    m_lib->waitForWorkers();
    const fakets3::Transfer upload = fakets3::uploads().last();
    QVERIFY2(QRegularExpression(a("^/party_[0-9a-f]{8}\\.gif$")).match(upload.remotePath).hasMatch(), qPrintable(upload.remotePath));
    QCOMPARE(fakets3::serverFile(upload.remotePath), linkfixtures::gif(3));
    QCOMPARE(MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().last().text)).size(), 1);
    QCOMPARE(m_lib->count(), 1);
    QCOMPARE(QDir(ts3::dataDir() + a("/gifs/files")).entryList(QDir::Files).size(), 1);

    // remoteName goes inside remoteNameFor: a pasted picture sent alongside keeps its own naming.
    SendRequest both;
    both.target = channel();
    SendItem named;
    named.path       = m_lib->copyPath(item);
    named.remoteName = a("party.gif");
    SendItem pasted;
    pasted.path       = m_dir->path() + a("/outside/paste.png");
    pasted.pasted     = true;
    pasted.remoteName = a("ignored.png");
    QVERIFY(writeFile(pasted.path, linkfixtures::png(4, 4)));
    both.items = {named, pasted};
    QVERIFY(m_core->send(both) > 0);
    QVERIFY(serveUntil([this] { return allDone() && fakets3::uploads().size() == 4; }));
    QStringList names;
    for (const fakets3::Transfer& t : fakets3::uploads())
        names << t.remotePath;
    const QRegularExpression party(a("^/party_[0-9a-f]{8}\\.gif$"));
    QVERIFY(party.match(names.at(2)).hasMatch() || party.match(names.at(3)).hasMatch());
    QVERIFY(names.at(2).startsWith(a("/new_photo_")) || names.at(3).startsWith(a("/new_photo_")));
}

void TestGifSend::uploadHook()
{
    // A spoiler is hidden on purpose: not recorded.
    QVERIFY(uploadGif(a("secret.gif"), true).isEmpty());
    QCOMPARE(m_lib->count(), 0);
    // A still GIF (one frame) is no animation: not recorded.
    SendRequest request;
    request.target = channel();
    SendItem still;
    still.path    = gif(a("still.gif"), 1);
    request.items = {still};
    QVERIFY(m_core->send(request) > 0);
    QVERIFY(serveUntil([this] { return allDone() && fakets3::messages().size() == 2; }));
    m_lib->waitForWorkers();
    QCOMPARE(m_lib->count(), 0);
    // Remember off: nothing.
    Settings::instance().rememberGifs = false;
    QVERIFY(uploadGif(a("off.gif")).isEmpty());
    Settings::instance().rememberGifs = true;
    // Recorded once; the same file again only touches it.
    const QString id = uploadGif(a("party.gif"));
    QVERIFY(!id.isEmpty());
    QVERIFY(!uploadGif(a("party2.gif")).isEmpty());
    QCOMPARE(m_lib->count(), 1);
    // A picture is never recorded.
    request.items.first().path = m_dir->path() + a("/outside/p.png");
    QVERIFY(writeFile(request.items.first().path, linkfixtures::png(4, 4)));
    QVERIFY(m_core->send(request) > 0);
    QVERIFY(serveUntil([this] { return allDone(); }));
    m_lib->waitForWorkers();
    QCOMPARE(m_lib->count(), 1);
}

void TestGifSend::mediaPostedOnce()
{
    QList<int>  posted;
    QList<bool> linkInSlot;
    connect(m_core.get(), &Core::mediaPosted, this, [&](int id) {
        posted << id;
        linkInSlot << (m_core->postedLink(id) != nullptr);
    });
    m_floodNext = 1; // the first answer is 0x020c: the post goes again
    SendRequest request;
    request.target = channel();
    SendItem item;
    item.path     = gif(a("flood.gif"));
    request.items = {item};
    QVERIFY(m_core->send(request) > 0);
    QVERIFY(serveUntil([this] { return allDone() && fakets3::messages().size() == 2; }));
    QCOMPARE(posted.size(), 1);
    QCOMPARE(linkInSlot, QList<bool>{true});
    QVERIFY(!m_core->postedLink(posted.first())); // Done: no link any more
    // A background send (link previews) never.
    SendRequest background;
    background.target     = channel();
    background.background = true;
    SendItem quiet;
    quiet.path       = gif(a("quiet.gif"));
    background.items = {quiet};
    QVERIFY(m_core->send(background) > 0);
    QVERIFY(serveUntil([this] { return allDone(); }));
    QCOMPARE(posted.size(), 1);
}

// ---- 2-9: sending from the tab ---------------------------------------------------------------------------------

void TestGifSend::linkSendsThroughPostQueue()
{
    const QString id = saveLink(kGiphy);
    QVERIFY(!id.isEmpty());
    ChatTarget target = channel();
    gifs::Host h      = host(&target);
    QString    error;
    m_floodNext = 1;
    QVERIFY2(h.send(id, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 2; })); // flooded once, sent again
    QVERIFY(serveUntil([this] { return m_core->queuedTextPosts(fakets3::kConnection) == 0; }));
    for (const fakets3::Message& m : fakets3::messages()) {
        QCOMPARE(m.text, QByteArray("[URL]https://giphy.com/gifs/x-ABCdef123[/URL]"));
        QCOMPARE(m.mode, int(TextMessageTarget_CHANNEL));
    }
    // Private: to the partner's current id, found by identity.
    fakets3::addClient({20, a("partner="), fakets3::kChannel, a("Alex"), false});
    target.mode      = TextMessageTarget_CLIENT;
    target.clientId  = 19; // a stale id: Core finds the uid's current one
    target.clientUid = a("partner=");
    QTest::qWait(850);
    QVERIFY2(h.send(id, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 3; }));
    QCOMPARE(fakets3::messages().last().mode, int(TextMessageTarget_CLIENT));
    QCOMPARE(fakets3::messages().last().client, anyID(20));
    // A partner who left: nothing is sent.
    fakets3::removeClient(20);
    QTest::qWait(850);
    QVERIFY(!h.send(id, h.state().chat, &error));
    QCOMPARE(error, noRecipientText());
    // The server chat.
    target      = channel();
    target.mode = TextMessageTarget_SERVER;
    QTest::qWait(850);
    QVERIFY2(h.send(id, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 4; }));
    QCOMPARE(fakets3::messages().last().mode, int(TextMessageTarget_SERVER));
}

void TestGifSend::serverChatUploads()
{
    const QString id = uploadGif(a("party.gif"));
    QVERIFY(!id.isEmpty());
    ChatTarget target           = channel();
    target.mode                 = TextMessageTarget_SERVER;
    gifs::Host            h     = host(&target);
    const gifs::SendState state = h.state();
    QVERIFY(state.serverChat);
    QVERIFY(state.canFiles);
    QString error;
    QVERIFY2(h.send(id, state.chat, &error), qPrintable(error));
    QVERIFY(serveUntil([this] { return allDone() && fakets3::messages().size() == 2; }));
    QCOMPARE(fakets3::messages().last().mode, int(TextMessageTarget_SERVER));
    QCOMPARE(fakets3::uploads().size(), 2); // into your channel's files
}

void TestGifSend::blocks()
{
    const QString fileId = uploadGif(a("party.gif"));
    const QString linkId = saveLink(kGiphy);
    QVERIFY(!fileId.isEmpty() && !linkId.isEmpty());
    ChatTarget target = channel();
    gifs::Host h      = host(&target);
    // Not connected: both refused with Core's text.
    fakets3::setConnected(false);
    gifs::SendState state = h.state();
    QVERIFY(!state.canText);
    QCOMPARE(state.blocked, notConnectedText());
    QCOMPARE(m_lib->blockOf(*m_lib->find(linkId), state), notConnectedText());
    QCOMPARE(m_lib->blockOf(*m_lib->find(fileId), state), notConnectedText());
    fakets3::setConnected(true);
    // A password channel: links send, files don't.
    fakets3::setChannelPassword(true);
    state = h.state();
    QVERIFY(state.canText);
    QVERIFY(!state.canFiles);
    QCOMPARE(m_lib->blockOf(*m_lib->find(fileId), state), uploadErrorText(MediaError::Password));
    QVERIFY(m_lib->blockOf(*m_lib->find(linkId), state).isEmpty());
    const int uploads = fakets3::uploads().size();
    QString   error;
    QVERIFY(!h.send(fileId, state.chat, &error));
    QCOMPARE(error, uploadErrorText(MediaError::Password));
    QCOMPARE(fakets3::uploads().size(), uploads);
    QVERIFY2(h.send(linkId, state.chat, &error), qPrintable(error));
    fakets3::setChannelPassword(false);
    // Over the upload limit: Core's wording.
    Settings::instance().uploadMaxMB = 1;
    state                            = h.state();
    const gifs::Item item            = *m_lib->find(fileId);
    gifs::Item       large           = item;
    large.size                       = 2 << 20;
    QVERIFY(m_lib->blockOf(item, state).isEmpty());
    QCOMPARE(m_lib->blockOf(large, state), uploadLimitTextFor(a(fakets3::kServerUid)));
    QVERIFY(state.uploadLimitText.contains(a("upload limit")));
    Settings::instance().uploadMaxMB = 50;
    // A missing copy.
    QVERIFY(QFile::remove(m_lib->copyPath(item)));
    state = h.state();
    QCOMPARE(m_lib->blockOf(item, state), gifs::missingCopyText());
    QTest::qWait(850);
    QVERIFY(!h.send(fileId, state.chat, &error));
    QCOMPARE(error, gifs::missingCopyText());
}

void TestGifSend::replyLead()
{
    const QString linkId     = saveLink(kGiphy);
    const QString fileId     = uploadGif(a("party.gif"));
    ChatTarget    target     = channel();
    QString       lead       = QString::fromUtf8("[color=#888888]\xe2\x86\xaa Alex: hello[/color]");
    gifs::Host    h          = host(&target, &lead);
    const QString expectLead = lead;
    QString       error;
    QVERIFY2(h.send(linkId, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 2; }));
    QCOMPARE(QString::fromUtf8(fakets3::messages().last().text), expectLead + a("\n[URL]https://giphy.com/gifs/x-ABCdef123[/URL]"));
    QVERIFY(lead.isEmpty()); // the reply ended
    lead = expectLead;
    QTest::qWait(850);
    QVERIFY2(h.send(fileId, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([this] { return allDone() && fakets3::messages().size() == 3; }));
    QVERIFY(QString::fromUtf8(fakets3::messages().last().text).startsWith(expectLead));
}

void TestGifSend::spacing()
{
    const QString id     = saveLink(kGiphy);
    ChatTarget    target = channel();
    gifs::Host    first  = host(&target);
    gifs::Host    second = host(&target); // another picker: the spacing is the library's
    QString       error;
    QVERIFY(first.send(id, first.state().chat, &error));
    QVERIFY(!first.send(id, first.state().chat, &error));
    QCOMPARE(error, gifs::waitText());
    QVERIFY(!second.send(id, second.state().chat, &error));
    QCOMPARE(error, gifs::waitText());
    QVERIFY(serveUntil([] { return !fakets3::messages().isEmpty(); }));
    QCOMPARE(fakets3::messages().size(), 1);
    // Two link posts unanswered: a third waits.
    m_answerMessages = false;
    QTest::qWait(850);
    QVERIFY(second.send(id, second.state().chat, &error));
    QTest::qWait(850);
    QVERIFY(second.send(id, second.state().chat, &error));
    QVERIFY(m_core->queuedTextPosts(fakets3::kConnection) >= 2);
    QTest::qWait(850);
    QVERIFY(!second.send(id, second.state().chat, &error));
    QCOMPARE(error, gifs::waitText());
}

void TestGifSend::targetChangedSinceOpen()
{
    const QString id = saveLink(kGiphy);
    fakets3::addClient({20, a("partner="), fakets3::kChannel, a("Alex"), false});
    struct Change {
        const char*                      name;
        std::function<void(ChatTarget*)> apply;
    };
    const Change changes[] = {
        {"another server tab", [](ChatTarget* t) { t->sch = 2; }},
        {"another chat tab", [](ChatTarget* t) { t->mode = TextMessageTarget_SERVER; }},
        {"another person with the same nick", [](ChatTarget* t) { t->clientUid = a("impostor="); }},
        {"moved to another channel", [](ChatTarget*) { fakets3::setOwnChannel(6); }},
    };
    for (const Change& change : changes) {
        fakets3::setOwnChannel(fakets3::kChannel);
        ChatTarget target = channel();
        target.mode       = TextMessageTarget_CLIENT;
        target.clientId   = 20;
        target.clientUid  = a("partner=");
        gifs::Host                  h      = host(&target);
        const gifs::ChatFingerprint opened = h.state().chat;
        QVERIFY(opened.valid);
        change.apply(&target);
        const int before = fakets3::messages().size();
        QString   error;
        QVERIFY2(!h.send(id, opened, &error), change.name);
        QCOMPARE(error, gifs::changedText());
        serve();
        QCOMPARE(fakets3::messages().size(), before);
    }
    fakets3::setOwnChannel(fakets3::kChannel);
}

void TestGifSend::postQueueOrder()
{
    const QString linkId = saveLink(kGiphy);
    // An upload whose message waits for its answer, then a GIF link: both go, in queue order, never flooded.
    m_answerMessages = false;
    SendRequest request;
    request.target = channel();
    SendItem item;
    item.path     = gif(a("first.gif"));
    request.items = {item};
    QVERIFY(m_core->send(request) > 0);
    QVERIFY(serveUntil([] { return fakets3::messages().size() == 1; }));
    ChatTarget target = channel();
    gifs::Host h      = host(&target);
    QString    error;
    QVERIFY2(h.send(linkId, h.state().chat, &error), qPrintable(error));
    m_answerMessages = true;
    QVERIFY(serveUntil([this] { return fakets3::messages().size() == 2 && allDone() && m_core->queuedTextPosts(fakets3::kConnection) == 0; }));
    QCOMPARE(MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().at(0).text)).size(), 1);
    QCOMPARE(fakets3::messages().at(1).text, QByteArray("[URL]https://giphy.com/gifs/x-ABCdef123[/URL]"));
    QVERIFY(!fakets3::logContains(a("flood protection held back")));
}

void TestGifSend::spoilerResend()
{
    // A revealed spoiler saved with Save to GIFs is kept covered and goes out as a spoiler again.
    const QString path = m_core->cacheDir() + a("/links/sp.gif");
    QVERIFY(writeFile(path, linkfixtures::gif(3)));
    const MediaLink link = linkstate::webLink(linkpreview::classify(a("https://example.com/sp.gif")), a("image/gif"), linkfixtures::gif(3).size(), 4, 4, 0, false, true);
    const QString   key  = m_core->adoptLocalFile(link, path);
    QVERIFY(!key.isEmpty());
    QSignalSpy saved(m_lib.get(), &GifLibrary::saveFinished);
    QString    message;
    QVERIFY(m_lib->saveFile(key, &message));
    m_lib->waitForWorkers();
    QCOMPARE(saved.count(), 1);
    const gifs::Item item = m_lib->items().first();
    QVERIFY(item.spoiler);
    ChatTarget target = channel();
    gifs::Host h      = host(&target);
    QString    error;
    QVERIFY2(h.send(item.id, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([this] { return allDone() && fakets3::messages().size() == 1; }));
    const QList<MediaLink> sent = MediaLink::findInMessage(QString::fromUtf8(fakets3::messages().last().text));
    QCOMPARE(sent.size(), 1);
    QVERIFY(sent.first().spoiler);
    QVERIFY(fakets3::messages().last().text.contains("sp=1"));
}

void TestGifSend::refusalMemory()
{
    const QString fileId = uploadGif(a("party.gif"));
    const QString linkId = saveLink(kGiphy);
    ChatTarget    target = channel();
    gifs::Host    h      = host(&target);
    QVERIFY(h.state().canFiles);
    m_refuseUploads = true;
    QString error;
    QVERIFY2(h.send(fileId, h.state().chat, &error), qPrintable(error));
    QVERIFY(serveUntil([this] { return allDone(); }));
    const gifs::SendState state = h.state();
    QVERIFY(state.canText);
    QVERIFY(!state.canFiles);
    QVERIFY(!state.filesBlocked.isEmpty());
    QCOMPARE(m_lib->blockOf(*m_lib->find(fileId), state), state.filesBlocked);
    QVERIFY(m_lib->blockOf(*m_lib->find(linkId), state).isEmpty()); // links still go
    // Another channel isn't affected.
    fakets3::setOwnChannel(6);
    QVERIFY(h.state().canFiles);
    fakets3::setOwnChannel(fakets3::kChannel);
    m_refuseUploads = false;
}

TSMEDIA_REGISTER_TEST(TestGifSend)

#include "tst_gifsend.moc"
