// 2.4 link previews, the sender's side and the announcements (sender-previews):
//  * LP: the announcement's wire form, every field rule, hostile input, the size limit;
//  * what the sender reads of its own message (links, the text identity every receiver computes);
//  * the PreviewStore: lookups, limits, the same sender only, persistence;
//  * receiving through linkpreview::Hub: only the message's own sender (spoofing ignored), never ServerQuery,
//    our channel only, private chats, waiting for the message, per-sender rate, the setting;
//  * the SenderPipeline end to end through the real Core against the fake TeamSpeak (tests/fakets3.*) with a
//    fake Fetcher: YouTube, images, videos, private chats, failures, reuse, TeaSpeak, flood costs, shutdown;
//  * 2.4 auto-load: LPNO (the sender's "no preview" notice) on the wire, sent and received; the records of our
//    own links (what our own chat shows at once); the pipeline's downloads in the chat's links folder.
// Nothing here touches the internet.

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>

#include "core.h"
#include "fakets3.h"
#include "fileverify.h"
#include "floodgovernor.h"
#include "linkpreview/announcement.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/lphub.h"
#include "linkpreview/lpmessage.h"
#include "linkpreview/pagemeta.h"
#include "linkpreview/previewmaker.h"
#include "linkpreview/textclean.h"
#include "linkpreview/previewstore.h"
#include "linkpreview/senderpipeline.h"
#include "linkpreview/transport.h"
#include "pluginlink.h"
#include "settings.h"
#include "testmain.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "ts3api.h"
#include "tschat.h"

using namespace linkpreview;

namespace {

constexpr int kTimeoutMs = 10000;

const QString kOwnUid   = QStringLiteral("OwnUidAAAAAAAAAAAAAAAAAAAAA=");
const QString kAliceUid = QStringLiteral("AliceUidBBBBBBBBBBBBBBBBBBB=");
const QString kBobUid   = QStringLiteral("BobUidCCCCCCCCCCCCCCCCCCCCC=");
const QString kCarolUid = QStringLiteral("CarolUidDDDDDDDDDDDDDDDDDDD=");
constexpr anyID  kAlice = 20, kBob = 21, kCarol = 22, kQuery = 30;
constexpr uint64 kOtherChannel = 9;

QByteArray sha(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QString shaOf(const QByteArray& bytes)
{
    return shaText(sha(bytes));
}

QImage picture(int w, int h, bool alpha = false)
{
    QImage image(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    image.fill(alpha ? Qt::transparent : Qt::white);
    QPainter p(&image);
    p.fillRect(QRect(w / 8, h / 8, w / 2, h / 2), QColor(30, 120, 220));
    p.fillRect(QRect(w / 3, h / 3, w / 2, h / 2), QColor(220, 60, 40, 200));
    return image;
}

bool savePicture(const QString& path, const QImage& image, const char* format)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path, format);
}

// A video's probe whose first frame is busy: its poster's JPEG is well over the 64 KB a tail check compares
// (2.4.0 on TeaSpeak: a 1080p sample's poster was 168 KB).
LocalMediaInfo busyVideoProbe(const QString& path)
{
    LocalMediaInfo info;
    info.kind       = MediaKind::Video;
    info.width      = 1920;
    info.height     = 1080;
    info.durationMs = 5759;
    if (QFileInfo(path).isFile()) {
        QImage           frame = picture(1920, 1080);
        QRandomGenerator noise(7);
        for (int y = 0; y < frame.height(); ++y)
            for (int x = 0; x < frame.width() / 3; ++x)
                frame.setPixel(x, y, qRgb(noise.bounded(256), noise.bounded(256), noise.bounded(256)));
        info.preview = frame;
    }
    return info;
}

QByteArray sampleMp4(int bytes)
{
    QByteArray mp4("\0\0\0\x20" "ftypisom" "\0\0\x02\0" "isomiso2avc1mp41", 32);
    for (int i = 0; mp4.size() < bytes; ++i)
        mp4 += QByteArray::number(i * 7919) + ' ';
    return mp4;
}

// What a preallocating server (TeaSpeak) has of an upload TeamSpeak reports complete: the whole size, the first
// half, zeros after.
QByteArray halfArrived(const QByteArray& bytes)
{
    QByteArray file(bytes.size(), '\0');
    file.replace(0, bytes.size() / 2, bytes.left(bytes.size() / 2));
    return file;
}

// A complete, valid announcement (stored form).
Announcement sample(Kind kind = Kind::Image, bool media = true)
{
    Announcement a;
    a.messageKey  = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000);
    a.urlKey      = QStringLiteral("00112233445566778899aabbccddeeff");
    a.kind        = kind;
    a.previewPath = QStringLiteral("/tsmedia/previews/3f9a1c2e.jpg");
    a.previewSha  = shaOf("preview");
    a.previewSize = 4321;
    if (media && kind != Kind::YouTube) {
        a.mediaPath = kind == Kind::Video ? QStringLiteral("/tsmedia/link_3f9a1c2e.mp4") : kind == Kind::Gif ? QStringLiteral("/tsmedia/link_3f9a1c2e.gif")
                                                                                                           : QStringLiteral("/tsmedia/link_3f9a1c2e.png");
        a.mediaSha  = shaOf("media");
        a.mediaSize = 123456;
    }
    a.width  = 1280;
    a.height = 720;
    if (kind == Kind::Video)
        a.durationMs = 61000;
    if (kind == Kind::YouTube) {
        a.title  = QStringLiteral("A song");
        a.author = QStringLiteral("A channel");
    }
    a.senderUid = kAliceUid;
    a.serverUid = QString::fromLatin1(fakets3::kServerUid);
    a.channelId = fakets3::kChannel;
    return a;
}

QByteArray sampleRef()
{
    return textreact::wireRef(QStringLiteral("0123456789abcdef"), 3);
}

proto::Message without(proto::Message m, const char* key)
{
    for (int i = 0; i < m.fields.size(); ++i) {
        if (m.fields.at(i).first == key) {
            m.fields.removeAt(i);
            break;
        }
    }
    return m;
}

proto::Message with(proto::Message m, const char* key, const QByteArray& value)
{
    m.set(key, value);
    return m;
}

// What the receiver gets: the wire form parsed again.
std::optional<Incoming> roundTrip(const proto::Message& m)
{
    const QByteArray wire = proto::serialize(m);
    if (wire.isEmpty())
        return std::nullopt;
    const std::optional<proto::Message> parsed = proto::parse(wire);
    return parsed ? readMessage(*parsed) : std::nullopt;
}

QByteArray b64(const QString& text)
{
    return toBase64Url(text.toUtf8()).toLatin1();
}

// 2.5: a complete, valid LPX (stored form): a card (with or without its picture) or a media page's media.
Announcement sampleX(Kind kind = Kind::Card, bool picture = true)
{
    Announcement a;
    a.extended   = true;
    a.messageKey = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000);
    a.urlKey     = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418");
    a.kind       = kind;
    if (picture || kind != Kind::Card) {
        a.channelId   = fakets3::kChannel;
        a.previewPath = kind == Kind::Card ? QStringLiteral("/tsmedia/previews/linkpreview_3f9a1c2e.jpg") : QStringLiteral("/tsmedia/previews/3f9a1c2e.jpg");
        a.previewSha  = shaOf("preview");
        a.previewSize = 4321;
        a.width       = 1200;
        a.height      = 630;
    }
    if (kind != Kind::Card) {
        a.mediaPath = kind == Kind::Gif ? QStringLiteral("/tsmedia/link_3f9a1c2e.gif") : kind == Kind::Image ? QStringLiteral("/tsmedia/link_3f9a1c2e.png")
                                                                                                             : QStringLiteral("/tsmedia/link_3f9a1c2e.mp4");
        a.mediaSha  = shaOf("media");
        a.mediaSize = 654321;
    }
    if (kind == Kind::Video || kind == Kind::Loop)
        a.durationMs = 4200;
    if (kind == Kind::Card) {
        a.card.site        = QStringLiteral("Example News");
        a.card.author      = QStringLiteral("Someone");
        a.card.title       = QStringLiteral("A page \u0645\u06cc\u200c\u062e\u0648\u0627\u0647\u0645");
        a.card.description = QStringLiteral("What the page is about, in a sentence or two.");
        a.card.layout      = picture ? CardLayout::Large : CardLayout::None;
        a.card.hasColor    = true;
        a.card.color       = qRgb(0x2e, 0x7d, 0x32);
        a.card.play        = picture;
        if (picture)
            a.durationMs = 95000;
    }
    a.senderUid = kAliceUid;
    a.serverUid = QString::fromLatin1(fakets3::kServerUid);
    return a;
}

std::optional<Incoming> roundTripX(const proto::Message& m)
{
    const QByteArray wire = proto::serialize(m);
    if (wire.isEmpty())
        return std::nullopt;
    const std::optional<proto::Message> parsed = proto::parse(wire);
    return parsed ? readLpx(*parsed) : std::nullopt;
}

std::optional<SkipX> roundTripSkipX(const proto::Message& m)
{
    const QByteArray wire = proto::serialize(m);
    if (wire.isEmpty())
        return std::nullopt;
    const std::optional<proto::Message> parsed = proto::parse(wire);
    return parsed ? readLpxSkip(*parsed) : std::nullopt;
}

// An incoming LPX as the hub stores it (the receiver resolved m and filled in who and where).
Announcement storedFrom(const Incoming& in)
{
    Announcement a = in.announcement;
    a.messageKey   = textreact::localKey(in.hash, 1760000000);
    a.senderUid    = kAliceUid;
    a.serverUid    = QString::fromLatin1(fakets3::kServerUid);
    return a;
}

// ---- fakes --------------------------------------------------------------------------------------------

class FakeFetch : public FetchPort
{
  public:
    struct Load {
        Target  target;
        qint64  maxBytes = 0; // 2.4: caps.media
        QString cacheDir;
        Caps    caps;         // 2.5
    };
    QVector<Load>  loads;
    QStringList    canceled;
    std::function<void(const Load&)> onLoad; // answers at once when set

    void load(const Target& target, const Caps& caps, const QString& cacheDir) override
    {
        loads.append({target, caps.media, cacheDir, caps});
        if (onLoad)
            onLoad(loads.last());
    }
    void cancel(const QString& key) override { canceled.append(key); }
};

// A website for the real Fetcher (transport.h): every network call counted; every answer is the same picture
// from a public address.
class CountingWebsite : public http::Transport
{
  public:
    QMutex      mutex;
    QStringList routed, resolved, requested;
    QByteArray  body;

    http::Error resolve(const QString& host, QVector<net::Address>* out, const http::Cancel& cancel, quint64 deadline) override
    {
        Q_UNUSED(cancel);
        Q_UNUSED(deadline);
        QMutexLocker lock(&mutex);
        resolved << host;
        *out = {net::parseAddress(QStringLiteral("93.184.216.34"))};
        return http::Error::None;
    }
    http::Route route(const QUrl& url, const http::Cancel& cancel, quint64 deadline) override
    {
        Q_UNUSED(cancel);
        Q_UNUSED(deadline);
        QMutexLocker lock(&mutex);
        routed << url.toString(QUrl::FullyEncoded);
        return {};
    }
    std::unique_ptr<http::Exchange> open(const http::HopOptions& options, const http::Cancel& cancel, http::Error* error) override
    {
        Q_UNUSED(cancel);
        *error = http::Error::None;
        QMutexLocker lock(&mutex);
        requested << options.url.toString(QUrl::FullyEncoded);
        return std::unique_ptr<http::Exchange>(new Answer(body));
    }
    int calls()
    {
        QMutexLocker lock(&mutex);
        return routed.size() + resolved.size() + requested.size();
    }

  private:
    class Answer : public http::Exchange
    {
      public:
        explicit Answer(QByteArray body)
            : m_body(std::move(body))
        {
        }
        http::Error head(http::Head* out) override
        {
            out->status        = 200;
            out->contentType   = "image/png";
            out->contentLength = m_body.size();
            out->peer          = net::parseAddress(QStringLiteral("93.184.216.34"));
            return http::Error::None;
        }
        http::Error read(char* buffer, int capacity, int* got) override
        {
            const int n = qMax(0, qMin(capacity, m_body.size() - m_pos));
            if (n > 0)
                memcpy(buffer, m_body.constData() + m_pos, static_cast<size_t>(n));
            m_pos += n;
            *got = n;
            return http::Error::None;
        }

      private:
        QByteArray m_body;
        int        m_pos = 0;
    };
};

// The pipeline's FetchPort over a real Fetcher (as linkpreview::Hub's own).
class FetcherPort : public FetchPort
{
  public:
    explicit FetcherPort(Fetcher* fetcher)
        : m_fetcher(fetcher)
    {
    }
    void load(const Target& target, const Caps& caps, const QString& cacheDir) override { m_fetcher->load(target, caps, cacheDir); }
    void cancel(const QString& key) override { m_fetcher->cancel(key); }

  private:
    Fetcher* m_fetcher;
};

// PluginLink's backend over the fake TeamSpeak and Core's flood governor; commands are recorded.
class LinkBackend : public PluginLink::Backend
{
  public:
    struct Sent {
        quint64       sch;
        QByteArray    payload;
        peers::Target target;
        QString       rc;
    };
    Core*         core = nullptr;
    QVector<Sent> sent;
    int           nextCode = 0;

    qint64         nowMs() const override { return core->floodClockMs(); }
    FloodGovernor* governor(quint64 sch) override { return ts3::isConnected(sch) ? &core->floodGovernor(sch) : nullptr; }
    void           floodStateChanged(quint64 sch) override { core->floodStateChanged(sch); }
    QString        newReturnCode() override { return QStringLiteral("lp-rc-%1").arg(++nextCode); }
    void    sendCommand(quint64 sch, const QByteArray& payload, const peers::Target& target, const QString& rc) override { sent.append({sch, payload, target, rc}); }
    bool    isConnected(quint64 sch) const override { return ts3::isConnected(sch); }
    bool    clientVisible(quint64 sch, quint16 client) const override
    {
        uint64 channel = 0;
        return client != 0 && ts3::funcs.getChannelOfClient(sch, client, &channel) == ERROR_ok && channel != 0;
    }
    quint16 ownClientId(quint64) const override { return fakets3::kOwnClient; }
};

} // namespace

class TestLinkPreviewSnd : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // LP
    void lpRoundTrip();
    void lpEveryKind();
    void lpTitleShortenedToFit();
    void lpHostileInput_data();
    void lpHostileInput();
    void lpUnknownFieldsIgnored();
    void lpMakeRefusesBadAnnouncements();
    void cleanTextAndBase64();

    // the sender's message
    void identityMatchesTheChat();
    void linksInMessages();

    // the store
    void storeLookupForgivesTheTime();
    void storeSameSenderOnlyAndPerMessageCap();
    void storeLeastRecentlyUsedAndAge();
    void storePersistence();
    void storeClearRemembersWhatHadAPreview();
    void linkForCore();

    // receiving
    void acceptsTheSendersOwnAnnouncement();
    void spoofedAnnouncementIgnored();
    void serverQueryAndOtherChannelsIgnored();
    void privateChats();
    void waitsForTheMessage();
    void perSenderRate();
    void settingOffAcceptsNothing();

    // the pipeline
    void youTubeEndToEnd();
    void imageWithItsFile();
    void transparentPictureKeepsAlpha();
    void videoPosterAndFile();
    void gifTooLargeGoesAsStill();
    void privateChatAnnouncesToThePartner();
    void onlyOwnChannelAndPrivateMessages();
    void failuresSendOneSkipNotice();
    void sameLinkAgainReusesTheFiles();
    void oneWebsiteRequestPerSend();
    void teaSpeakNeverAskedForFileInfo();
    void teaSpeakPosterIsOnTheServerBeforeItsVideoGoesUp();
    void askableServerSendsTheVideoRightAfterItsPoster();
    void stopsWhileThePosterIsChecked();
    void floodCosts();
    void floodCostsOneCommandPerJob();
    void settingOffStopsTheJob();
    void shutdownMidUpload();
    void shutdownWhileMakingJoinsTheWorker();

    // review (protocol)
    void serverPosterGoneFallsBackToTheClickCard();
    void serverPosterInterruptedKeepsTheSendersPreview();
    void inlinePreviewsOffDownloadsNothing();
    void forgedWebServerLinkIgnored();
    void movedAwayUploadsNothing();
    void announcementDelayedInTheQueueStillMatches();
    void pictureKindFromItsBytesStillShows();

    // 2.4 auto-load
    void lpnoRoundTrip();
    void lpnoHostileInput_data();
    void lpnoHostileInput();
    void noSkipNoticeWhenItCannotGo();
    void skipNoticeReceived();
    void ownLinkRecords();
    void ownLinkRecordStates();

    // Live pass C: a reload of the plugin in TeamSpeak's process
    void pluginRegistersNoMetaTypes();

    // 2.5: LPX and LPXNO
    void lpxRoundTrip();
    void lpxHostileInput_data();
    void lpxHostileInput();
    void lpxUnknownFieldsIgnored();
    void lpxShortenToFit();
    void lpxStoreFormat();
    void lpxnoReasons();
    void lpxnoHostileInput_data();
    void lpxnoHostileInput();
    void maxSendBytesPerType();
    void lpxCapPerConnection();
    void lpxSentThroughPluginLink();
    void lpxSeededFuzz();
    // 2.5: receiving LPX / LPXNO
    void lpxOnlyFromTheSender();
    void lpxOwnBucket();
    void lpxnoReceivedWithReasons();
    // 2.5: the sender's page links
    void senderCardWithPicture();
    void senderTextOnlyCard();
    void senderLoopGifCap();
    void senderCrossSiteNoCard();
    void senderWalledNoCard();
    void senderWebCardsOff();
    void senderFollowsWebCardsSwitch();
    void intakeNoticeOnePerMessage();
    void ownLinkRecordsForCards();
    void cardMakerSafeAndSmall();
    // Review fixes (2.5.0)
    void pageMediaSensitiveMade();
    void refusedUploadRemembered();

  private:
    void answerCard(const CardData& card, const QImage& image = QImage(), const QString& finalHost = QString());
    std::optional<Incoming> lastSentX() const;
    void    newHub(const Hub::Limits& limits = Hub::Limits());
    void    serve();
    bool    serveUntil(const std::function<bool()>& done, int timeoutMs = kTimeoutMs);
    // Our own message as TeamSpeak reports it back.
    void    sendOwn(const QString& bbcode, int mode = TextMessageTarget_CHANNEL, anyID to = 0);
    // The fake website's answer to the last load: a picture / video file in its cacheDir.
    void    answerYouTube(const QString& title = QStringLiteral("Never \u202egonna\u202c give you up"), const QString& author = QStringLiteral("Rick\nAstley"));
    void    answerFile(const QByteArray& bytes, const QString& name);
    // Our chat's index holds the message (channel chat, or the private chat with partnerUid).
    void    indexMessage(const QString& hash, qint64 timeSec, const QString& sender, textreact::ChatKind kind = textreact::ChatKind::Channel,
                         const QString& partnerUid = QString(), quintptr chat = 1);
    proto::Message announcementFrom(const QString& senderUid, const QString& hash, proto::Scope scope = proto::Scope::Channel, Kind kind = Kind::Image);
    std::optional<Incoming> lastSent() const;
    QList<proto::Message>   sentMessages() const; // every plugin command sent, in order
    QByteArrayList          sentTypes() const;
    int     chatLines() const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    std::unique_ptr<LinkBackend>   m_backend;
    std::unique_ptr<PluginLink>    m_link;
    std::unique_ptr<FakeFetch>     m_fetch;
    std::unique_ptr<Hub>           m_hub;
    textreact::Index               m_index;
    QSet<QString>                  m_answered;
    QSet<anyID>                    m_completed;
    QSet<anyID>                    m_served;
    bool                           m_refuseUploads = false;
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
};

void TestLinkPreviewSnd::initTestCase()
{
    m_savedFuncs    = ts3::funcs;
    m_savedPluginId = ts3::pluginId;
    m_savedSettings = Settings::instance();
    // QSignalSpy needs them (this test process only: the plugin itself never registers a metatype).
    qRegisterMetaType<linkpreview::Result>();
    qRegisterMetaType<linkpreview::Announcement>();
}

void TestLinkPreviewSnd::cleanupTestCase()
{
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
    fileverify::resetCounters();
}

void TestLinkPreviewSnd::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    fakets3::setOwnUid(kOwnUid);
    fakets3::addClient({kAlice, kAliceUid, fakets3::kChannel, QStringLiteral("Alice"), false});
    fakets3::addClient({kBob, kBobUid, fakets3::kChannel, QStringLiteral("Bob"), false});
    fakets3::addClient({kCarol, kCarolUid, kOtherChannel, QStringLiteral("Carol"), false});
    fakets3::addClient({kQuery, QStringLiteral("serveradmin"), fakets3::kChannel, QStringLiteral("serveradmin"), true});
    Settings& s           = Settings::instance();
    s                     = Settings(); // both link-preview settings on, uploads into /tsmedia
    s.addRequiredNotice   = false;
    s.videoAutoDownloadMB = 0;
    m_answered.clear();
    m_completed.clear();
    m_served.clear();
    m_refuseUploads = false;
    m_index.clear();
    m_core = std::make_unique<Core>();
    m_core->start();
    m_backend       = std::make_unique<LinkBackend>();
    m_backend->core = m_core.get();
    m_link          = std::make_unique<PluginLink>(*m_backend);
    m_fetch         = std::make_unique<FakeFetch>();
    newHub();
}

void TestLinkPreviewSnd::cleanup()
{
    m_hub.reset();
    m_link.reset();
    m_backend.reset();
    m_core.reset();
    m_fetch.reset();
    m_dir.reset();
}

void TestLinkPreviewSnd::newHub(const Hub::Limits& limits)
{
    m_hub.reset();
    m_hub = std::make_unique<Hub>(m_core.get(), m_link.get(), &m_index, m_fetch.get(), limits);
    m_hub->start();
}

void TestLinkPreviewSnd::serve()
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
            m_core->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, t.returnCode, QStringLiteral("insufficient client permissions"), true);
            continue;
        }
        if (fakets3::completeUpload(t.id))
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    // 2.3.2's content checks of what was sent (a TeaSpeak server: no file info question, only these)
    for (const fakets3::Transfer& t : fakets3::checkDownloads()) {
        if (t.halted || m_served.contains(t.id))
            continue;
        m_served.insert(t.id);
        if (!fakets3::deliverServerFile(t.id).isEmpty())
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    for (const fakets3::InfoRequest& r : fakets3::fileInfoRequests()) {
        if (m_answered.contains(r.returnCode))
            continue;
        m_answered.insert(r.returnCode);
        m_core->onFileInfo(fakets3::kConnection, r.channel, QFileInfo(r.path).fileName(), static_cast<quint64>(fakets3::serverFile(r.path).size()));
        m_core->onServerError(fakets3::kConnection, ERROR_ok, r.returnCode, QString(), false);
    }
}

bool TestLinkPreviewSnd::serveUntil(const std::function<bool()>& done, int timeoutMs)
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

void TestLinkPreviewSnd::sendOwn(const QString& bbcode, int mode, anyID to)
{
    m_hub->onTextMessage(fakets3::kConnection, mode, to, fakets3::kOwnClient, kOwnUid, bbcode);
}

void TestLinkPreviewSnd::answerYouTube(const QString& title, const QString& author)
{
    QVERIFY(!m_fetch->loads.isEmpty());
    const FakeFetch::Load load  = m_fetch->loads.last();
    const QString         thumb = load.cacheDir + QStringLiteral("/thumb.jpg");
    QVERIFY(savePicture(thumb, picture(480, 360), "JPG"));
    Result r;
    r.ok            = true;
    r.title         = title;
    r.author        = author;
    r.thumbnailFile = thumb;
    m_hub->pipeline()->fetched(load.target.key, r);
}

void TestLinkPreviewSnd::answerFile(const QByteArray& bytes, const QString& name)
{
    QVERIFY(!m_fetch->loads.isEmpty());
    const FakeFetch::Load load = m_fetch->loads.last();
    const QString         path = load.cacheDir + QLatin1Char('/') + name;
    QDir().mkpath(load.cacheDir);
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(bytes);
    out.close();
    Result r;
    r.ok        = true;
    r.localFile = path;
    r.size      = bytes.size();
    m_hub->pipeline()->fetched(load.target.key, r);
}

void TestLinkPreviewSnd::indexMessage(const QString& hash, qint64 timeSec, const QString& sender, textreact::ChatKind kind, const QString& partnerUid, quintptr chat)
{
    textreact::IndexedChat c;
    c.serverUid  = QString::fromLatin1(fakets3::kServerUid);
    c.kind       = kind;
    c.partnerUid = partnerUid;
    c.messages.append({hash, timeSec, sender});
    m_index.setChat(chat, c);
    m_hub->indexChanged();
}

proto::Message TestLinkPreviewSnd::announcementFrom(const QString& senderUid, const QString& hash, proto::Scope scope, Kind kind)
{
    Announcement a = sample(kind);
    a.senderUid    = senderUid;
    return makeMessage(a, scope, textreact::wireRef(hash, 2));
}

std::optional<Incoming> TestLinkPreviewSnd::lastSent() const
{
    if (m_backend->sent.isEmpty())
        return std::nullopt;
    const std::optional<proto::Message> m = proto::parse(m_backend->sent.last().payload);
    return m ? readMessage(*m) : std::nullopt;
}

void TestLinkPreviewSnd::answerCard(const CardData& card, const QImage& image, const QString& finalHost)
{
    QVERIFY(!m_fetch->loads.isEmpty());
    const FakeFetch::Load load = m_fetch->loads.last();
    Result                r;
    r.ok        = true;
    r.kind      = Kind::Card;
    r.card      = card;
    r.finalHost = finalHost;
    r.requests  = image.isNull() ? 1 : 2;
    if (!image.isNull()) {
        const QString path = load.cacheDir + QLatin1Char('/') + load.target.key + QStringLiteral("-thumb.jpg");
        QVERIFY(savePicture(path, image, "JPG"));
        r.card.pictureFile   = path;
        r.card.pictureWidth  = image.width();
        r.card.pictureHeight = image.height();
        r.mime               = QStringLiteral("image/jpeg");
    }
    m_hub->pipeline()->fetched(load.target.key, r);
}

std::optional<Incoming> TestLinkPreviewSnd::lastSentX() const
{
    for (int i = m_backend->sent.size() - 1; i >= 0; --i) {
        const std::optional<proto::Message> m = proto::parse(m_backend->sent.at(i).payload);
        if (m && m->type == "LPX")
            return readLpx(*m);
    }
    return std::nullopt;
}

QList<proto::Message> TestLinkPreviewSnd::sentMessages() const
{
    QList<proto::Message> out;
    for (const LinkBackend::Sent& s : m_backend->sent) {
        const std::optional<proto::Message> m = proto::parse(s.payload);
        if (m)
            out << *m;
    }
    return out;
}

QByteArrayList TestLinkPreviewSnd::sentTypes() const
{
    QByteArrayList out;
    for (const proto::Message& m : sentMessages())
        out << m.type;
    return out;
}

int TestLinkPreviewSnd::chatLines() const
{
    int n = 0;
    for (const QString& line : fakets3::logLines())
        n += line.startsWith(QLatin1String("[chat]")) ? 1 : 0;
    return n;
}

// ---- LP ---------------------------------------------------------------------------------------------

void TestLinkPreviewSnd::lpRoundTrip()
{
    const Announcement   a = sample(Kind::Image);
    const proto::Message m = makeMessage(a, proto::Scope::Channel, sampleRef());
    QCOMPARE(m.type, QByteArray("LP"));
    const QByteArray wire = proto::serialize(m);
    QVERIFY(!wire.isEmpty());
    QVERIFY(wire.size() <= proto::kMaxSendBytes);
    const std::optional<Incoming> in = roundTrip(m);
    QVERIFY(in);
    QCOMPARE(in->scope, proto::Scope::Channel);
    QCOMPARE(in->hash, QStringLiteral("0123456789abcdef"));
    QCOMPARE(in->ageSec, qint64(3));
    const Announcement& b = in->announcement;
    QVERIFY(b.messageKey.isEmpty()); // the receiver resolves it
    QVERIFY(b.senderUid.isEmpty());  // never from the payload
    QCOMPARE(b.urlKey, a.urlKey);
    QCOMPARE(b.kind, a.kind);
    QCOMPARE(b.channelId, a.channelId);
    QCOMPARE(b.previewPath, a.previewPath);
    QCOMPARE(b.previewSha, a.previewSha);
    QCOMPARE(b.previewSize, a.previewSize);
    QCOMPARE(b.mediaPath, a.mediaPath);
    QCOMPARE(b.mediaSha, a.mediaSha);
    QCOMPARE(b.mediaSize, a.mediaSize);
    QCOMPARE(b.width, a.width);
    QCOMPARE(b.height, a.height);
    QVERIFY(!b.privateChat);

    const std::optional<Incoming> p = roundTrip(makeMessage(a, proto::Scope::Private, sampleRef()));
    QVERIFY(p);
    QCOMPARE(p->scope, proto::Scope::Private);
    QVERIFY(p->announcement.privateChat);
}

void TestLinkPreviewSnd::lpEveryKind()
{
    for (const Kind kind : {Kind::YouTube, Kind::Image, Kind::Gif, Kind::Video}) {
        for (const bool media : {false, true}) {
            const Announcement            a  = sample(kind, media);
            const std::optional<Incoming> in = roundTrip(makeMessage(a, proto::Scope::Channel, sampleRef()));
            QVERIFY2(in, kindCode(kind) ? QByteArray(1, kindCode(kind)).constData() : "?");
            QCOMPARE(in->announcement.kind, kind);
            QCOMPARE(in->announcement.mediaPath, a.mediaPath);
            QCOMPARE(in->announcement.durationMs, a.durationMs);
            QCOMPARE(in->announcement.title, a.title);
            QCOMPARE(in->announcement.author, a.author);
        }
    }
    // A non-ASCII remote path and title travel as base64url of their UTF-8.
    Announcement a = sample(Kind::YouTube);
    a.previewPath  = QStringLiteral("/m\u00e9dia/previews/\u0444\u043e\u0442\u043e.jpg");
    a.title        = QStringLiteral("\u266b Caf\u00e9 \U0001F600");
    const std::optional<Incoming> in = roundTrip(makeMessage(a, proto::Scope::Channel, sampleRef()));
    QVERIFY(in);
    QCOMPARE(in->announcement.previewPath, a.previewPath);
    QCOMPARE(in->announcement.title, a.title);
}

void TestLinkPreviewSnd::lpTitleShortenedToFit()
{
    Announcement a = sample(Kind::YouTube);
    a.previewPath  = QStringLiteral("/") + QString(60, QLatin1Char('d')) + QStringLiteral("/previews/") + QString(60, QLatin1Char('n')) + QStringLiteral(".jpg");
    a.title        = QString(400, QChar(0x4e2d)); // 3 bytes each: cut to kMaxTitleBytes, then to fit
    a.author       = QString(200, QLatin1Char('a'));
    const proto::Message m = makeMessage(a, proto::Scope::Channel, sampleRef());
    QVERIFY(!m.type.isEmpty());
    QVERIFY(proto::serialize(m).size() <= proto::kMaxSendBytes);
    const std::optional<Incoming> in = roundTrip(m);
    QVERIFY(in);
    QVERIFY(!in->announcement.title.isEmpty());
    QVERIFY(in->announcement.title.toUtf8().size() <= kMaxTitleBytes);
    QVERIFY(in->announcement.author.toUtf8().size() <= kMaxAuthorBytes);
    QCOMPARE(in->announcement.previewPath, a.previewPath);

    // Paths too long for any message: no LP at all.
    a.previewPath = QStringLiteral("/") + QString(300, QLatin1Char('x')) + QStringLiteral(".jpg");
    QVERIFY(makeMessage(a, proto::Scope::Channel, sampleRef()).type.isEmpty());
}

void TestLinkPreviewSnd::lpHostileInput_data()
{
    QTest::addColumn<int>("which");
    // Each row breaks one rule of a valid image announcement with its file (see the switch below).
    const char* const names[] = {"scope s", "scope missing", "m malformed", "m too old", "u upper case", "u short", "u long", "k unknown", "k two", "c zero",
                                 "c leading zero", "c sign", "c too long", "p missing", "p not base64", "p not canonical", "p padded", "p relative",
                                 "p dot dot", "p backslash", "p control", "p not a picture", "p trailing slash", "p bidi", "h short", "h not base64",
                                 "ps zero", "ps huge", "ps leading zero", "f without fh", "fh without fs", "f wrong kind", "f same as p", "fs too large",
                                 "media for YouTube", "w without ht", "w zero", "ht huge", "d for an image", "t for an image", "t not UTF-8", "t too long",
                                 "u missing", "h missing", "ps missing", "not LP"};
    for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i)
        QTest::newRow(names[i]) << i;
}

void TestLinkPreviewSnd::lpHostileInput()
{
    QFETCH(int, which);
    const proto::Message base = makeMessage(sample(Kind::Image), proto::Scope::Channel, sampleRef());
    QVERIFY(readMessage(base));
    proto::Message m = base;
    switch (which) {
    case 0: m = with(m, "s", "s"); break;
    case 1: m = without(m, "s"); break;
    case 2: m = with(m, "m", "0123456789abcdeg-3"); break;
    case 3: m = with(m, "m", "0123456789abcdef-99999999"); break;
    case 4: m = with(m, "u", "00112233445566778899AABBCCDDEEFF"); break;
    case 5: m = with(m, "u", "0011"); break;
    case 6: m = with(m, "u", QByteArray(65, 'a')); break;
    case 7: m = with(m, "k", "x"); break;
    case 8: m = with(m, "k", "ii"); break;
    case 9: m = with(m, "c", "0"); break;
    case 10: m = with(m, "c", "05"); break;
    case 11: m = with(m, "c", "-5"); break;
    case 12: m = with(m, "c", "123456789012345678901"); break;
    case 13: m = without(m, "p"); break;
    case 14: m = with(m, "p", "a.b"); break;
    case 15: { // the last character's unused low bits set: another spelling of the same bytes
        static const QByteArray alphabet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
        QByteArray p = b64(QStringLiteral("/a/b.jpg")); // 8 bytes: the last character carries 2 unused bits
        p[p.size() - 1] = alphabet.at(alphabet.indexOf(p.at(p.size() - 1)) | 1);
        m = with(m, "p", p);
        break;
    }
    case 16: m = with(m, "p", b64(QStringLiteral("/a/b.jpg")) + "-"); break;
    case 17: m = with(m, "p", b64(QStringLiteral("tsmedia/a.jpg"))); break;
    case 18: m = with(m, "p", b64(QStringLiteral("/tsmedia/../x.jpg"))); break;
    case 19: m = with(m, "p", b64(QStringLiteral("/tsmedia\\x.jpg"))); break;
    case 20: m = with(m, "p", b64(QStringLiteral("/tsmedia/a\nb.jpg"))); break;
    case 21: m = with(m, "p", b64(QStringLiteral("/tsmedia/previews/a.exe"))); break;
    case 22: m = with(m, "p", b64(QStringLiteral("/tsmedia/previews/"))); break;
    case 23: m = with(m, "p", b64(QStringLiteral("/tsmedia/\u202egpj.exe.jpg"))); break;
    case 24: m = with(m, "h", "abc"); break;
    case 25: m = with(m, "h", QByteArray(43, '*')); break;
    case 26: m = with(m, "ps", "0"); break;
    case 27: m = with(m, "ps", QByteArray::number(kMaxPreviewBytes + 1)); break;
    case 28: m = with(m, "ps", "0123"); break;
    case 29: m = without(m, "fh"); break;
    case 30: m = without(m, "fs"); break;
    case 31: m = with(m, "f", b64(QStringLiteral("/tsmedia/link.mp4"))); break;
    case 32: m = with(m, "f", m.value("p")); break;
    case 33: m = with(m, "fs", QByteArray::number(kMaxMediaBytes + 1)); break;
    case 34: m = with(m, "k", "y"); break; // an image's f= on a YouTube announcement
    case 35: m = without(m, "ht"); break;
    case 36: m = with(m, "w", "0"); break;
    case 37: m = with(m, "ht", QByteArray::number(kMaxSide + 1)); break;
    case 38: m = with(m, "d", "1000"); break;
    case 39: m = with(m, "t", b64(QStringLiteral("title"))); break;
    case 40: {
        m = with(makeMessage(sample(Kind::YouTube), proto::Scope::Channel, sampleRef()), "t", toBase64Url(QByteArray("\xff\xfe\x80")).toLatin1());
        break;
    }
    case 41: m = with(makeMessage(sample(Kind::YouTube), proto::Scope::Channel, sampleRef()), "t", b64(QString(kMaxTitleBytes + 1, QLatin1Char('t')))); break;
    case 42: m = without(m, "u"); break;
    case 43: m = without(m, "h"); break;
    case 44: m = without(m, "ps"); break;
    case 45: m.type = "TR"; break;
    default: QFAIL("no such row");
    }
    QVERIFY(!readMessage(m));
    // And through the wire, when the grammar lets it through at all.
    const QByteArray wire = proto::serialize(m);
    if (!wire.isEmpty()) {
        const std::optional<proto::Message> parsed = proto::parse(wire);
        QVERIFY(!parsed || !readMessage(*parsed));
    }
}

void TestLinkPreviewSnd::lpUnknownFieldsIgnored()
{
    proto::Message m = makeMessage(sample(Kind::Gif), proto::Scope::Channel, sampleRef());
    m.set("zz", "newer");
    m.set("x", "1");
    const std::optional<Incoming> in = roundTrip(m);
    QVERIFY(in);
    QCOMPARE(in->announcement.kind, Kind::Gif);
    // tsm1's own rules still hold: a key twice, a space in a value, a longer command are dropped before LP.
    QVERIFY(!proto::parse(proto::serialize(m) + " m=x"));
    QVERIFY(!proto::parse(QByteArray("tsm1 LP s=c ") + QByteArray(proto::kMaxRecvBytes, 'a')));
}

void TestLinkPreviewSnd::lpMakeRefusesBadAnnouncements()
{
    Announcement a = sample(Kind::Image);
    a.previewPath  = QStringLiteral("/tsmedia/previews/x.gif"); // a preview is a JPEG or PNG
    QVERIFY(makeMessage(a, proto::Scope::Channel, sampleRef()).type.isEmpty());
    a = sample(Kind::YouTube);
    a.mediaPath = QStringLiteral("/tsmedia/x.png");
    a.mediaSha  = shaOf("x");
    a.mediaSize = 1;
    QVERIFY(makeMessage(a, proto::Scope::Channel, sampleRef()).type.isEmpty());
    QVERIFY(makeMessage(sample(), proto::Scope::Channel, QByteArray("nonsense")).type.isEmpty());
    a = sample(Kind::Image);
    a.channelId = 0;
    QVERIFY(makeMessage(a, proto::Scope::Channel, sampleRef()).type.isEmpty());
    QVERIFY(isValid(sample()));
    a = sample();
    a.messageKey = QStringLiteral("nope");
    QVERIFY(!isValid(a));
}

void TestLinkPreviewSnd::cleanTextAndBase64()
{
    QCOMPARE(cleanText(QStringLiteral("  A\u202eB\u200b C\n\tD\u0007 "), 100), QStringLiteral("AB C D"));
    QCOMPARE(cleanText(QStringLiteral("\u00e9\u00e9\u00e9"), 4), QStringLiteral("\u00e9\u00e9")); // whole characters
    QCOMPARE(cleanText(QStringLiteral("\U0001F600\U0001F600"), 5), QStringLiteral("\U0001F600"));
    QVERIFY(fromBase64Url("aGk", 10));
    QVERIFY(!fromBase64Url("aGk=", 10));
    QVERIFY(!fromBase64Url("aGl", 10)); // not the canonical spelling of "hi"
    QVERIFY(!fromBase64Url("aGk", 1));
    QVERIFY(!fromBase64Url("", 10));
    QCOMPARE(shaBytes(shaOf("x")), sha("x"));
    QVERIFY(shaBytes(QStringLiteral("x")).isEmpty());
    QVERIFY(isUrlKey(QStringLiteral("0123abcd")));
    QVERIFY(!isUrlKey(QStringLiteral("0123ABCD")));
    QVERIFY(isMediaPath(Kind::Video, QStringLiteral("/a/b.MP4")));
    QVERIFY(!isMediaPath(Kind::Video, QStringLiteral("/a/b.gif")));
}

// ---- the sender's message ----------------------------------------------------------------------------

void TestLinkPreviewSnd::identityMatchesTheChat()
{
    // What the sender computes from its BBCode is what every receiver computes from its chat document.
    struct Case {
        QString bbcode;
        QString bodyHtml;
    };
    const tschat::Skin skin = tschat::darkSkin();
    const QString      yt   = QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    const QString      img  = QStringLiteral("https://example.com/cat.png?x=1&y=2");
    const QVector<Case> cases = {
        {QStringLiteral("[URL]") + yt + QStringLiteral("[/URL]"), tschat::linkHtml(skin, yt)},
        {QStringLiteral("look at this :) [URL]") + img + QStringLiteral("[/URL] nice!"),
         QStringLiteral("look at this :) ") + tschat::linkHtml(skin, img) + QStringLiteral(" nice!")},
        {QStringLiteral("[b]bold[/b] [URL=") + yt + QStringLiteral("]my video[/URL] \\[not a tag\\]"),
         QStringLiteral("<b>bold</b> ") + tschat::linkHtml(skin, yt, QStringLiteral("my video")) + QStringLiteral(" [not a tag]")},
        {QStringLiteral("Caf\u00e9 \U0001F600 [color=red]red[/color] [URL]") + img + QStringLiteral("[/URL]"),
         QStringLiteral("Caf\u00e9 \U0001F600 <span style=\"color:red\">red</span> ") + tschat::linkHtml(skin, img)},
    };
    for (const Case& c : cases) {
        QTextDocument doc;
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:17"), 7, kOwnUid, QStringLiteral("Me"), c.bodyHtml, true));
        const textreactdoc::Identity id = textreactdoc::identify(doc.lastBlock());
        QVERIFY2(id.eligible(), qPrintable(c.bbcode));
        QCOMPARE(message::identityHash(kOwnUid, c.bbcode), id.hash);
    }
    // A reply: its quote line is not part of the message's identity, nor are links in it.
    const QString reply = QStringLiteral("[i]\u21aa [URL=client://0/") + kAliceUid + QStringLiteral("~Alice]Alice[/URL] \u00b7 21\u223614: \u201chi\u201d[/i]\n[URL]")
                          + yt + QStringLiteral("[/URL]");
    QCOMPARE(message::identityHash(kOwnUid, reply), message::identityHash(kOwnUid, QStringLiteral("[URL]") + yt + QStringLiteral("[/URL]")));
    QCOMPARE(message::visibleText(QStringLiteral("a [URL=https://x.example/]b[/URL] \\\\[c]")), QStringLiteral("a b [c]"));
}

void TestLinkPreviewSnd::linksInMessages()
{
    const QList<QUrl> links = message::webLinks(QStringLiteral(
        "[URL]https://a.example/1.png[/URL] [URL=https://b.example/2.gif]label[/URL] see https://c.example/3.mp4, and http://d.example/4.jpg. "
        "\\[URL]https://e.example/escaped.png\\[/URL] [URL=client://1/x~y]nick[/URL] ts3file://x"));
    QStringList hosts;
    for (const QUrl& url : links)
        hosts << url.host() + url.path();
    // The escaped tag is plain text, but its address is still a bare one TeamSpeak links.
    QCOMPARE(hosts, QStringList({QStringLiteral("a.example/1.png"), QStringLiteral("b.example/2.gif"), QStringLiteral("c.example/3.mp4"),
                                 QStringLiteral("d.example/4.jpg"), QStringLiteral("e.example/escaped.png")}));
    QCOMPARE(message::webLinks(QStringLiteral("https://a.example/1.png https://a.example/2.png https://a.example/3.png"), 2).size(), 2);
    QVERIFY(message::isMediaMessage(QStringLiteral("[URL=ts3file://127.0.0.1?x]a.png[/URL] https://y.example/a.png")));
    // What gets a preview: a YouTube video (http too) or an https media file.
    QVERIFY(!classify(QUrl(QStringLiteral("http://d.example.com/4.jpg"))).isLegacy()); // 2.5: a media-file page link (LPX), never LP
    QCOMPARE(classify(QUrl(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"))).kind, Kind::YouTube);
    QCOMPARE(classify(QUrl(QStringLiteral("http://youtu.be/dQw4w9WgXcQ"))).kind, Kind::YouTube);
    // An address typed without a scheme: TeamSpeak's [URL] holds it as written, the chat links it as http://.
    const QList<QUrl> typed = message::webLinks(QStringLiteral("look [URL]www.youtube.com/watch?v=dQw4w9WgXcQ[/URL] and www.example.com/a.png"));
    QCOMPARE(typed.size(), 1);
    QCOMPARE(typed.first().scheme(), QStringLiteral("http"));
    QCOMPARE(classify(typed.first()).kind, Kind::YouTube);
    QCOMPARE(classify(typed.first()).key, classify(QUrl(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"))).key);
    QVERIFY(!classify(QUrl(QStringLiteral("https://example.com/page"))).isLegacy()); // 2.5: a Web link (LPX), never LP
    QCOMPARE(classify(QUrl(QStringLiteral("https://example.com/a.GIF?x=1"))).kind, Kind::Gif);
}

// ---- the store ----------------------------------------------------------------------------------------

void TestLinkPreviewSnd::storeLookupForgivesTheTime()
{
    PreviewStore   store;
    QSignalSpy     announced(&store, &PreviewStore::announced);
    const Announcement a = sample();
    QCOMPARE(store.add(a, 1000), PreviewStore::Add::Added);
    QCOMPARE(announced.size(), 1);
    QVERIFY(store.find(a.messageKey, a.urlKey));
    // The same message read with a header a second or two off (the sender's own copy, another clock).
    const QString near = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000002);
    QVERIFY(store.find(near, a.urlKey));
    QCOMPARE(store.find(near, a.urlKey)->previewPath, a.previewPath);
    QVERIFY(!store.find(textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000020), a.urlKey));
    QVERIFY(!store.find(a.messageKey, QStringLiteral("ffffffffffffffffffffffffffffffff")));
    QVERIFY(!store.find(QStringLiteral("garbage"), a.urlKey));
    QCOMPARE(store.forMessage(near).size(), 1);
}

void TestLinkPreviewSnd::storeSameSenderOnlyAndPerMessageCap()
{
    PreviewStore store;
    Announcement a = sample();
    QCOMPARE(store.add(a, 1000), PreviewStore::Add::Added);
    QCOMPARE(store.add(a, 1001), PreviewStore::Add::Unchanged);
    Announcement other = a;
    other.senderUid    = kBobUid; // someone else for the same link of the same message
    other.previewSha   = shaOf("other");
    QCOMPARE(store.add(other, 1002), PreviewStore::Add::Rejected);
    QCOMPARE(store.find(a.messageKey, a.urlKey)->previewSha, a.previewSha);
    Announcement again = a;
    again.previewSha   = shaOf("newer");
    QCOMPARE(store.add(again, 1003), PreviewStore::Add::Replaced);
    QCOMPARE(store.find(a.messageKey, a.urlKey)->previewSha, again.previewSha);
    // At most four links per message.
    for (int i = 0; i < 6; ++i) {
        Announcement more = a;
        more.urlKey       = QStringLiteral("aaaaaaaa%1").arg(i);
        store.add(more, 1004);
    }
    QCOMPARE(store.forMessage(a.messageKey).size(), 4);
    Announcement bad = a;
    bad.previewPath  = QStringLiteral("relative.jpg");
    QCOMPARE(store.add(bad, 1005), PreviewStore::Add::Rejected);
}

void TestLinkPreviewSnd::storeLeastRecentlyUsedAndAge()
{
    PreviewStore::Limits limits;
    limits.maxEntries = 3;
    limits.maxAgeSec  = 1000;
    PreviewStore store(limits);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto entry = [](int i) {
        Announcement a = sample();
        a.messageKey   = textreact::localKey(QStringLiteral("00000000000000%1").arg(i, 2, 10, QLatin1Char('0')), 1760000000);
        return a;
    };
    for (int i = 0; i < 3; ++i)
        QCOMPARE(store.add(entry(i), now - 100 + i), PreviewStore::Add::Added);
    QVERIFY(store.find(entry(0).messageKey, entry(0).urlKey)); // used now: the most recent
    QCOMPARE(store.add(entry(3), now - 50), PreviewStore::Add::Added);
    QCOMPARE(store.count(), 3);
    QVERIFY(store.find(entry(0).messageKey, entry(0).urlKey));
    QVERIFY(!store.find(entry(1).messageKey, entry(1).urlKey)); // the least recently used went
    QVERIFY(store.find(entry(3).messageKey, entry(3).urlKey));
    // Entries unused for longer than maxAgeSec are not saved and not loaded.
    const QByteArray json = store.toJson(now + 5000);
    PreviewStore     later(limits);
    QVERIFY(later.fromJson(json, now + 5000));
    QCOMPARE(later.count(), 0);
}

void TestLinkPreviewSnd::storePersistence()
{
    const QString path = m_dir->path() + QStringLiteral("/lp.json");
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    {
        PreviewStore store;
        for (const Kind kind : {Kind::YouTube, Kind::Image, Kind::Gif, Kind::Video}) {
            Announcement a = sample(kind);
            a.urlKey       = QStringLiteral("0011223344556677889900000000000") + QString::number(static_cast<int>(kind));
            a.privateChat  = kind == Kind::Gif;
            a.channelId    = 18446744073709551000ull; // 64-bit channel ids survive JSON
            QCOMPARE(store.add(a, now), PreviewStore::Add::Added);
        }
        QVERIFY(store.save(path, now));
    }
    PreviewStore loaded;
    QVERIFY(loaded.load(path, now));
    QCOMPARE(loaded.count(), 4);
    const Announcement* v = loaded.find(sample().messageKey, QStringLiteral("00112233445566778899000000000004"));
    QVERIFY(v);
    QCOMPARE(v->kind, Kind::Video);
    QCOMPARE(v->durationMs, qint64(61000));
    QCOMPARE(v->channelId, 18446744073709551000ull);
    QCOMPARE(v->senderUid, kAliceUid);
    const Announcement* y = loaded.find(sample().messageKey, QStringLiteral("00112233445566778899000000000001"));
    QVERIFY(y);
    QCOMPARE(y->title, QStringLiteral("A song"));
    QVERIFY(loaded.find(sample().messageKey, QStringLiteral("00112233445566778899000000000003"))->privateChat);

    // A tampered entry is skipped; the rest loads.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray json = file.readAll();
    file.close();
    json.replace("/tsmedia/previews/3f9a1c2e.jpg", "/tsmedia/../../evil.jpg");
    PreviewStore tampered;
    QVERIFY(tampered.fromJson(json, now));
    QCOMPARE(tampered.count(), 0);
    // Not ours: moved aside, the store starts empty.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{ not json");
    file.close();
    PreviewStore broken;
    QVERIFY(!broken.load(path, now));
    QCOMPARE(broken.count(), 0);
    QVERIFY(QFileInfo::exists(path + QStringLiteral(".bad")));
    // Too large to be ours: not even read.
    PreviewStore::Limits small;
    small.maxFileBytes = 10;
    PreviewStore tiny(small);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArray(100, ' '));
    file.close();
    QVERIFY(!tiny.load(path, now));
}

// 2.4 auto-load: Clear cache takes every announcement (and link-previews.json), but which links of which messages had a
// sender's preview stays known in memory for the session, so the chat never loads those from their websites by itself.
void TestLinkPreviewSnd::storeClearRemembersWhatHadAPreview()
{
    PreviewStore::Limits limits;
    limits.maxEntries = 3;
    PreviewStore       store(limits);
    const Announcement a = sample();
    QCOMPARE(store.add(a, 1000), PreviewStore::Add::Added);
    QVERIFY(!store.clearedEarlier(a.messageKey, a.urlKey)); // still there: nothing cleared
    store.clear();
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.find(a.messageKey, a.urlKey));
    QVERIFY(store.clearedEarlier(a.messageKey, a.urlKey));
    QCOMPARE(store.clearedCount(), 1);
    // find()'s matching: a header a second or two off is the same message; another link, time or message isn't.
    QVERIFY(store.clearedEarlier(textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000002), a.urlKey));
    QVERIFY(!store.clearedEarlier(textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000020), a.urlKey));
    QVERIFY(!store.clearedEarlier(a.messageKey, QStringLiteral("ffffffffffffffffffffffffffffffff")));
    QVERIFY(!store.clearedEarlier(QStringLiteral("garbage"), a.urlKey));
    QVERIFY(!store.clearedEarlier(a.messageKey, QString()));
    // Nothing of it is written: a saved store has none of it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/link-previews.json");
    QVERIFY(store.save(path, 2000));
    PreviewStore again(limits);
    QVERIFY(again.load(path, 2000));
    QVERIFY(!again.clearedEarlier(a.messageKey, a.urlKey));
    // Bounded: at most maxEntries, a later clear's first.
    for (int i = 0; i < 3; ++i) {
        Announcement more = a;
        more.messageKey   = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760001000 + i * 100);
        QCOMPARE(store.add(more, 3000), PreviewStore::Add::Added);
    }
    store.clear();
    QVERIFY(store.clearedCount() <= limits.maxEntries);
    QVERIFY(store.clearedEarlier(textreact::localKey(QStringLiteral("0123456789abcdef"), 1760001200), a.urlKey));
}

void TestLinkPreviewSnd::linkForCore()
{
    const Announcement withFile = sample(Kind::Video);
    const MediaLink    a        = linkFor(withFile);
    QVERIFY(a.isValid());
    QCOMPARE(a.remoteFile(), withFile.mediaPath);
    QCOMPARE(a.previewFile, withFile.previewPath);
    QCOMPARE(a.sha256, shaBytes(withFile.mediaSha));
    QCOMPARE(a.previewSha, shaBytes(withFile.previewSha).left(16));
    QCOMPARE(a.size, quint64(withFile.mediaSize));
    QCOMPARE(a.durationMs, qint64(61000));
    const Announcement previewOnly = sample(Kind::YouTube);
    const MediaLink    b           = linkFor(previewOnly);
    QCOMPARE(b.remoteFile(), previewOnly.previewPath);
    QCOMPARE(b.sha256, shaBytes(previewOnly.previewSha));
    QCOMPARE(b.size, quint64(previewOnly.previewSize));
    QVERIFY(b.previewFile.isEmpty());
    QVERIFY(a.key() != b.key());
}

// ---- receiving ----------------------------------------------------------------------------------------

void TestLinkPreviewSnd::acceptsTheSendersOwnAnnouncement()
{
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("look https://example.com/cat.png"));
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    indexMessage(hash, now - 2, kAliceUid);
    QSignalSpy announced(m_hub->store(), &PreviewStore::announced);
    bool       knownToCore = false; // the chat can look the files up in Core when it hears of them
    connect(m_hub->store(), &PreviewStore::announced, this, [&](const Announcement& x) { knownToCore = m_core->entry(linkFor(x).key()) != nullptr; });
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->counters().accepted, 1);
    QCOMPARE(announced.size(), 1);
    QVERIFY(knownToCore);
    const Announcement a = qvariant_cast<Announcement>(announced.first().first());
    QCOMPARE(a.senderUid, kAliceUid);
    QCOMPARE(a.messageKey, textreact::localKey(hash, now - 2));
    QCOMPARE(a.serverUid, QString::fromLatin1(fakets3::kServerUid));
    QVERIFY(m_hub->store()->find(a.messageKey, a.urlKey));
    // The preview is downloaded from the TeamSpeak server (Core, checked against its sha).
    QTRY_VERIFY_WITH_TIMEOUT(!fakets3::downloads().isEmpty(), 3000);
    QCOMPARE(fakets3::downloads().first().remotePath, a.previewPath);
    // The same again: nothing new.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(announced.size(), 1);
    // Saved a few seconds later, and at shutdown.
    m_hub->prepareShutdown();
    QVERIFY(QFileInfo::exists(m_hub->storePath()));
    newHub();
    QVERIFY(m_hub->store()->find(a.messageKey, a.urlKey));
}

void TestLinkPreviewSnd::spoofedAnnouncementIgnored()
{
    Hub::Limits limits;
    limits.waitTtlMs = 300;
    newHub(limits);
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("look https://example.com/cat.png"));
    indexMessage(hash, QDateTime::currentSecsSinceEpoch() - 2, kAliceUid);
    // Bob announces a preview for Alice's message (the payload says nothing of who sent it: the server does).
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), announcementFrom(kBobUid, hash));
    QCOMPARE(m_hub->counters().accepted, 0);
    QCOMPARE(m_hub->store()->count(), 0);
    QCOMPARE(m_hub->waitingCount(), 1); // it might be for a message of Bob's that isn't here yet ...
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->waitingCount(), 0, 3000);
    QCOMPARE(m_hub->counters().unmatched, 1); // ... it wasn't
    QCOMPARE(m_hub->store()->count(), 0);
    QVERIFY(fakets3::downloads().isEmpty());
    // Alice's own announcement for it still counts afterwards.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->counters().accepted, 1);
    QCOMPARE(m_hub->store()->find(textreact::localKey(hash, QDateTime::currentSecsSinceEpoch() - 2), sample().urlKey)->senderUid, kAliceUid);
}

void TestLinkPreviewSnd::serverQueryAndOtherChannelsIgnored()
{
    const QString hash = textreact::contentHash(kCarolUid, QStringLiteral("hi"));
    const QString qh   = textreact::contentHash(QStringLiteral("serveradmin"), QStringLiteral("hi"));
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    indexMessage(hash, now - 2, kCarolUid, textreact::ChatKind::Channel, QString(), 1);
    indexMessage(qh, now - 2, QStringLiteral("serveradmin"), textreact::ChatKind::Channel, QString(), 2);
    // ServerQuery
    m_hub->received(fakets3::kConnection, kQuery, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"), announcementFrom(QStringLiteral("serveradmin"), qh));
    // Carol is in another channel
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), announcementFrom(kCarolUid, hash));
    QCOMPARE(m_hub->counters().rejected, 2);
    QCOMPARE(m_hub->store()->count(), 0);
    // Alice is here, but says the files are in another channel.
    const QString ah = textreact::contentHash(kAliceUid, QStringLiteral("hi"));
    indexMessage(ah, now - 2, kAliceUid, textreact::ChatKind::Channel, QString(), 3);
    Announcement elsewhere = sample();
    elsewhere.channelId    = kOtherChannel;
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), makeMessage(elsewhere, proto::Scope::Channel, textreact::wireRef(ah, 2)));
    QCOMPARE(m_hub->counters().rejected, 3);
    // Malformed
    proto::Message bad = announcementFrom(kAliceUid, ah);
    bad.set("k", "z");
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), bad);
    QCOMPARE(m_hub->counters().rejected, 4);
    QCOMPARE(m_hub->store()->count(), 0);
}

void TestLinkPreviewSnd::privateChats()
{
    Hub::Limits limits;
    limits.waitTtlMs = 200;
    newHub(limits);
    const qint64  now     = QDateTime::currentSecsSinceEpoch();
    const QString inPm    = textreact::contentHash(kCarolUid, QStringLiteral("pm https://example.com/a.png"));
    const QString inChat  = textreact::contentHash(kCarolUid, QStringLiteral("channel https://example.com/a.png"));
    indexMessage(inPm, now - 1, kCarolUid, textreact::ChatKind::Private, kCarolUid, 1);
    indexMessage(inChat, now - 1, kCarolUid, textreact::ChatKind::Channel, QString(), 2);
    // Carol (in another channel) announces the preview of her private message to us: files in her channel.
    Announcement a = sample();
    a.channelId    = kOtherChannel;
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), makeMessage(a, proto::Scope::Private, textreact::wireRef(inPm, 1)));
    QCOMPARE(m_hub->counters().accepted, 1);
    const Announcement* stored = m_hub->store()->find(textreact::localKey(inPm, now - 1), a.urlKey);
    QVERIFY(stored);
    QVERIFY(stored->privateChat);
    QCOMPARE(stored->channelId, kOtherChannel);
    // A private announcement for a message of hers in the channel chat: not ours to take.
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), makeMessage(a, proto::Scope::Private, textreact::wireRef(inChat, 1)));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->waitingCount(), 0, 3000);
    QCOMPARE(m_hub->counters().accepted, 1);
    // Bob (in our channel) announces in the channel for a message he sent privately: no.
    const QString bobPm = textreact::contentHash(kBobUid, QStringLiteral("secret"));
    indexMessage(bobPm, now - 1, kBobUid, textreact::ChatKind::Private, kBobUid, 3);
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), announcementFrom(kBobUid, bobPm, proto::Scope::Channel));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->waitingCount(), 0, 3000);
    QCOMPARE(m_hub->counters().accepted, 1);
}

void TestLinkPreviewSnd::waitsForTheMessage()
{
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("soon"));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->waitingCount(), 1);
    QCOMPARE(m_hub->store()->count(), 0);
    // The chat shows the message a moment later.
    indexMessage(hash, QDateTime::currentSecsSinceEpoch() - 2, kAliceUid);
    QCOMPARE(m_hub->waitingCount(), 0);
    QCOMPARE(m_hub->counters().accepted, 1);
}

void TestLinkPreviewSnd::perSenderRate()
{
    Hub::Limits limits;
    limits.perSenderBurst = 3;
    newHub(limits);
    for (int i = 0; i < 5; ++i)
        m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, textreact::contentHash(kAliceUid, QString::number(i))));
    QCOMPARE(m_hub->counters().rateLimited, 2);
    QCOMPARE(m_hub->waitingCount(), 3);
    // Bob has his own allowance.
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), announcementFrom(kBobUid, textreact::contentHash(kBobUid, QStringLiteral("x"))));
    QCOMPARE(m_hub->counters().rateLimited, 2);
}

void TestLinkPreviewSnd::settingOffAcceptsNothing()
{
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("x"));
    indexMessage(hash, QDateTime::currentSecsSinceEpoch() - 2, kAliceUid);
    Settings::instance().showLinkPreviews = false;
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->store()->count(), 0);
    QCOMPARE(m_hub->waitingCount(), 0);
    QTest::qWait(50);
    QVERIFY(fakets3::downloads().isEmpty());
    // Nor are files asked for later (a history reload finding an announcement).
    requestFiles(m_core.get(), sample());
    QTest::qWait(50);
    QVERIFY(fakets3::downloads().isEmpty());
    // Nor while the chat shows no previews at all ("Show images, videos and file cards in the chat" off): nothing
    // is downloaded for a card nobody sees.
    Settings::instance().showLinkPreviews = true;
    Settings::instance().inlinePreviews   = false;
    requestFiles(m_core.get(), sample(Kind::YouTube)); // only a preview: it would be downloaded explicitly
    QTest::qWait(50);
    QVERIFY(fakets3::downloads().isEmpty());
    Settings::instance().inlinePreviews = true; // shown again: the chat asks, and it comes
    requestFiles(m_core.get(), sample(Kind::YouTube));
    QTRY_VERIFY_WITH_TIMEOUT(!fakets3::downloads().isEmpty(), 2000);
    Settings::instance().showLinkPreviews = false;
    // Nor does sending fetch anything.
    sendOwn(QStringLiteral("[URL]https://www.youtube.com/watch?v=dQw4w9WgXcQ[/URL]"));
    QVERIFY(m_fetch->loads.isEmpty());
}

// ---- 2.4 auto-load ------------------------------------------------------------------------------------

void TestLinkPreviewSnd::lpnoRoundTrip()
{
    const QString        urlKey = sample().urlKey;
    const proto::Message m      = makeSkip(proto::Scope::Channel, sampleRef(), urlKey);
    QCOMPARE(m.type, QByteArray("LPNO"));
    const QByteArray wire = proto::serialize(m);
    QVERIFY(!wire.isEmpty());
    QVERIFY(wire.size() <= proto::kMaxSendBytes);
    const std::optional<proto::Message> parsed = proto::parse(wire);
    QVERIFY(parsed);
    const std::optional<Skip> skip = readSkip(*parsed);
    QVERIFY(skip);
    QCOMPARE(skip->scope, proto::Scope::Channel);
    QCOMPARE(skip->hash, QStringLiteral("0123456789abcdef"));
    QCOMPARE(skip->ageSec, qint64(3));
    QCOMPARE(skip->urlKey, urlKey);
    QVERIFY(!readMessage(*parsed)); // not an LP
    const std::optional<Skip> p = readSkip(makeSkip(proto::Scope::Private, sampleRef(), urlKey));
    QVERIFY(p);
    QCOMPARE(p->scope, proto::Scope::Private);
    // Unknown fields are ignored (a later version may add some).
    proto::Message newer = m;
    newer.set("zz", "1");
    QVERIFY(readSkip(newer));
    // Bad input makes no message.
    QVERIFY(makeSkip(proto::Scope::Channel, QByteArray("nope"), urlKey).type.isEmpty());
    QVERIFY(makeSkip(proto::Scope::Channel, sampleRef(), QStringLiteral("XYZ")).type.isEmpty());
    // An LP is never taken for one.
    QVERIFY(!readSkip(makeMessage(sample(), proto::Scope::Channel, sampleRef())));
}

void TestLinkPreviewSnd::lpnoHostileInput_data()
{
    QTest::addColumn<int>("which");
    const char* const names[] = {"s other", "s missing", "m malformed", "m too old", "m missing", "u upper case", "u short", "u long", "u missing",
                                 "u not hex", "not LPNO"};
    for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i)
        QTest::newRow(names[i]) << i;
}

void TestLinkPreviewSnd::lpnoHostileInput()
{
    QFETCH(int, which);
    const proto::Message base = makeSkip(proto::Scope::Channel, sampleRef(), sample().urlKey);
    QVERIFY(readSkip(base));
    proto::Message m = base;
    switch (which) {
    case 0: m = with(m, "s", "x"); break;
    case 1: m = without(m, "s"); break;
    case 2: m = with(m, "m", "0123456789abcdeg-3"); break;
    case 3: m = with(m, "m", "0123456789abcdef-99999999"); break;
    case 4: m = without(m, "m"); break;
    case 5: m = with(m, "u", "00112233445566778899AABBCCDDEEFF"); break;
    case 6: m = with(m, "u", "0011"); break;
    case 7: m = with(m, "u", QByteArray(65, 'a')); break;
    case 8: m = without(m, "u"); break;
    case 9: m = with(m, "u", "0011223344556677889900zz"); break;
    case 10: m.type = "LP"; break;
    default: QFAIL("no such row");
    }
    QVERIFY(!readSkip(m));
    const QByteArray wire = proto::serialize(m);
    if (!wire.isEmpty()) {
        const std::optional<proto::Message> parsed = proto::parse(wire);
        QVERIFY(!parsed || !readSkip(*parsed));
    }
    // Non-ASCII and duplicate keys never get past tsm1's own grammar.
    QVERIFY(!proto::parse(proto::serialize(base) + QByteArray(" u=") + QByteArray(sample().urlKey.toLatin1())));
    QVERIFY(!proto::parse(QByteArray("tsm1 LPNO s=c m=0123456789abcdef-3 u=\xc3\xa9")));
}

// A notice goes only where our LP would have gone, only for a job that was accepted, and never once we stopped.
void TestLinkPreviewSnd::noSkipNoticeWhenItCannotGo()
{
    Result refused;
    refused.error = QStringLiteral("HTTP 403");
    // Make switched off mid-job: no notice (the caps re-announce tells the channel).
    sendOwn(QStringLiteral("[URL]https://example.com/a.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    Settings::instance().makeLinkPreviews = false;
    m_hub->applySettings();
    QCOMPARE(m_hub->pipeline()->jobCount(), 0);
    Settings::instance().makeLinkPreviews = true;
    m_hub->applySettings();
    // Moved to another channel while the website answered: no notice there (nor here).
    sendOwn(QStringLiteral("[URL]https://example.com/b.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    fakets3::setOwnChannel(kOtherChannel);
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, refused);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    fakets3::setOwnChannel(fakets3::kChannel);
    // The private partner left.
    sendOwn(QStringLiteral("[URL]https://example.com/c.png[/URL]"), TextMessageTarget_CLIENT, kAlice);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    fakets3::removeClient(kAlice);
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, refused);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    QTest::qWait(50);
    serve();
    QVERIFY(m_backend->sent.isEmpty());
    QCOMPARE(m_hub->pipeline()->counters().skipNotices, 0);

    // Links refused at intake (more than maxJobsPerMinute): no LPNO for each (it would exceed the budget); 2.5: ONE
    // intake notice (LPXNO) for a message's refused links, at most one per 10 s. The accepted ones still go; at unload
    // nothing goes.
    for (int i = 0; i < 4; ++i)
        sendOwn(QStringLiteral("https://a.example.com/%1a.png https://a.example.com/%1b.png").arg(i));
    const int refusedAtIntake = m_hub->pipeline()->counters().skipped;
    QVERIFY(refusedAtIntake >= 2);
    QVERIFY(m_hub->pipeline()->jobCount() > 0);
    m_hub->prepareShutdown(); // every job ends ("stopped"): nothing goes
    QTest::qWait(50);
    serve();
    QCOMPARE(sentTypes(), QByteArrayList({"LPXNO"}));
    QCOMPARE(m_hub->pipeline()->counters().intakeNotices, 1);
    QCOMPARE(m_hub->pipeline()->counters().skipNotices, 0);
    m_backend->sent.clear();

    // A server that doesn't pass on plugin commands: nothing (blocked at intake, or while the job ran).
    newHub();
    const int loadsBefore = m_fetch->loads.size();
    sendOwn(QStringLiteral("[URL]https://example.com/d.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), loadsBefore + 1, 3000);
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, refused);
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 1; }));
    QCOMPARE(sentTypes(), QByteArrayList({"LPNO"})); // the one that could go
    sendOwn(QStringLiteral("[URL]https://example.com/e.png[/URL]"));
    const int loads = m_fetch->loads.size();
    QTRY_VERIFY_WITH_TIMEOUT(m_fetch->loads.size() == loads && m_hub->pipeline()->jobCount() == 1, 3000);
    m_link->onServerError(fakets3::kConnection, ERROR_permissions_client_insufficient, m_backend->sent.last().rc, QStringLiteral("insufficient client permissions"), true);
    QVERIFY(m_link->blocked(fakets3::kConnection));
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, refused);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    sendOwn(QStringLiteral("[URL]https://example.com/f.png[/URL]")); // blocked at intake
    QTest::qWait(50);
    serve();
    QCOMPARE(m_backend->sent.size(), 1);
    const SenderPipeline::OwnLink* f = m_hub->pipeline()->latestOwnLink(classify(QStringLiteral("https://example.com/f.png")).key);
    QVERIFY(f);
    QCOMPARE(f->state, SenderPipeline::OwnLink::State::Skipped);
    QCOMPARE(f->hash, message::identityHash(kOwnUid, QStringLiteral("[URL]https://example.com/f.png[/URL]")));
}

// LPNO is taken exactly like LP: from the message's own sender only, never ServerQuery, s=c only from our channel,
// LP's per-sender limit; it waits for its message; an LP already stored wins. Kept in memory, bounded.
void TestLinkPreviewSnd::skipNoticeReceived()
{
    Hub::Limits limits;
    limits.waitTtlMs = 300;
    newHub(limits);
    const qint64  now    = QDateTime::currentSecsSinceEpoch();
    const QString urlKey = sample().urlKey;
    const QString hash   = textreact::contentHash(kAliceUid, QStringLiteral("look https://example.com/cat.png"));
    indexMessage(hash, now - 2, kAliceUid);
    const QString key = textreact::localKey(hash, now - 2);
    QSignalSpy    skipped(m_hub.get(), &Hub::skipped);
    const auto    notice = [](const QString& h, proto::Scope scope = proto::Scope::Channel, const QString& u = QString()) {
        return makeSkip(scope, textreact::wireRef(h, 2), u.isEmpty() ? sample().urlKey : u);
    };
    // Bob for Alice's message: not his message (it waits for one of his, then gives up).
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), notice(hash));
    QCOMPARE(m_hub->waitingCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->waitingCount(), 0, 3000);
    QCOMPARE(m_hub->counters().unmatched, 1);
    // ServerQuery; Carol from another channel with s=c; malformed.
    const QString qh = textreact::contentHash(QStringLiteral("serveradmin"), QStringLiteral("hi"));
    m_hub->received(fakets3::kConnection, kQuery, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"), notice(qh));
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), notice(textreact::contentHash(kCarolUid, QStringLiteral("hi"))));
    proto::Message bad = notice(hash);
    bad.set("u", "XYZ");
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), bad);
    QCOMPARE(m_hub->counters().rejected, 3);
    QCOMPARE(skipped.count(), 0);
    QVERIFY(!m_hub->isSkipped(key, urlKey));
    // Alice's own: accepted, the chat hears of it.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash));
    QCOMPARE(m_hub->counters().skipsAccepted, 1);
    QCOMPARE(skipped.count(), 1);
    QCOMPARE(skipped.first().first().toString(), urlKey);
    QVERIFY(m_hub->isSkipped(key, urlKey));
    QVERIFY(m_hub->isSkipped(textreact::localKey(hash, now - 2 + textreact::kToleranceSec), urlKey));
    QVERIFY(!m_hub->isSkipped(textreact::localKey(hash, now - 2 + textreact::kToleranceSec + 1), urlKey));
    QVERIFY(!m_hub->isSkipped(key, QStringLiteral("00112233445566778899aabbccddee00")));
    QVERIFY(!m_hub->isSkipped(textreact::localKey(textreact::contentHash(kAliceUid, QStringLiteral("other")), now - 2), urlKey));
    QCOMPARE(m_hub->store()->count(), 0); // nothing stored, nothing downloaded
    QTest::qWait(50);
    QVERIFY(fakets3::downloads().isEmpty());
    // The same again: kept once.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash));
    QCOMPARE(m_hub->skipCount(), 1);
    // A private one from Carol (another channel) for her private message: fine (no channel rule for s=p).
    const QString pm = textreact::contentHash(kCarolUid, QStringLiteral("pm https://example.com/a.png"));
    indexMessage(pm, now - 1, kCarolUid, textreact::ChatKind::Private, kCarolUid, 2);
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), notice(pm, proto::Scope::Private));
    QVERIFY(m_hub->isSkipped(textreact::localKey(pm, now - 1), urlKey));
    // It waits for its message like an LP.
    const QString later = textreact::contentHash(kAliceUid, QStringLiteral("soon"));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(later));
    QCOMPARE(m_hub->waitingCount(), 1);
    indexMessage(later, now - 1, kAliceUid, textreact::ChatKind::Channel, QString(), 3);
    QCOMPARE(m_hub->waitingCount(), 0);
    QVERIFY(m_hub->isSkipped(textreact::localKey(later, now - 1), urlKey));
    // Its LP is here already: the LP wins, the notice is ignored.
    const QString shown = textreact::contentHash(kAliceUid, QStringLiteral("shown"));
    indexMessage(shown, now - 1, kAliceUid, textreact::ChatKind::Channel, QString(), 4);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, shown));
    QCOMPARE(m_hub->counters().accepted, 1);
    const int accepted = m_hub->counters().skipsAccepted;
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(shown));
    QCOMPARE(m_hub->counters().skipsAccepted, accepted);
    QVERIFY(!m_hub->isSkipped(textreact::localKey(shown, now - 1), urlKey));
    // Show link previews off: nothing is taken.
    Settings::instance().showLinkPreviews = false;
    m_hub->applySettings();
    QCOMPARE(m_hub->skipCount(), 0); // forgotten
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash));
    QVERIFY(!m_hub->isSkipped(key, urlKey));
    Settings::instance().showLinkPreviews = true;

    // LP and LPNO share one limit per sender.
    Hub::Limits three;
    three.perSenderBurst = 3;
    newHub(three);
    indexMessage(hash, now - 2, kAliceUid);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash, proto::Scope::Channel, QStringLiteral("00112233445566778899aabbccddee01")));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash, proto::Scope::Channel, QStringLiteral("00112233445566778899aabbccddee02")));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hash, proto::Scope::Channel, QStringLiteral("00112233445566778899aabbccddee03")));
    QCOMPARE(m_hub->counters().rateLimited, 1);
    QCOMPARE(m_hub->counters().skipsAccepted, 2);

    // At most kMaxSkips are kept (the oldest go).
    Hub::Limits many;
    many.perSenderBurst = 1000;
    newHub(many);
    textreact::IndexedChat chat;
    chat.serverUid = QString::fromLatin1(fakets3::kServerUid);
    chat.kind      = textreact::ChatKind::Channel;
    QStringList hashes;
    for (int i = 0; i < Hub::kMaxSkips + 20; ++i) {
        hashes << textreact::contentHash(kAliceUid, QStringLiteral("link %1").arg(i));
        chat.messages.append({hashes.last(), now - 2, kAliceUid});
    }
    m_index.setChat(9, chat);
    for (const QString& h : qAsConst(hashes))
        m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(h));
    QCOMPARE(m_hub->counters().skipsAccepted, Hub::kMaxSkips + 20);
    QCOMPARE(m_hub->skipCount(), Hub::kMaxSkips);
    QVERIFY(!m_hub->isSkipped(textreact::localKey(hashes.first(), now - 2), urlKey));
    QVERIFY(m_hub->isSkipped(textreact::localKey(hashes.last(), now - 2), urlKey));
    // Clear cache and unload forget them.
    m_core->clearCache();
    QCOMPARE(m_hub->skipCount(), 0);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), notice(hashes.last()));
    QCOMPARE(m_hub->skipCount(), 1);
    m_hub->prepareShutdown();
    QCOMPARE(m_hub->skipCount(), 0);
}

// Our own links: what became of each (our own chat shows "Making the preview…", then the website's files the moment
// the fetch is done, before anything goes up).
void TestLinkPreviewSnd::ownLinkRecords()
{
    using State = SenderPipeline::OwnLink::State;
    SenderPipeline* pipeline = m_hub->pipeline();
    QSignalSpy      changed(pipeline, &SenderPipeline::ownLinkChanged);
    const QString   text = QStringLiteral("look [URL]https://example.com/photo.jpg[/URL]");
    const Target    t    = classify(QStringLiteral("https://example.com/photo.jpg"));
    const qint64    now  = QDateTime::currentSecsSinceEpoch();
    sendOwn(text);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QVERIFY(changed.count() >= 2); // Queued, then Fetching
    for (const QList<QVariant>& args : qAsConst(changed))
        QCOMPARE(args.first().toString(), t.key);
    const SenderPipeline::OwnLink* r = pipeline->latestOwnLink(t.key);
    QVERIFY(r);
    QCOMPARE(r->state, State::Fetching);
    QVERIFY(r->jobId > 0);
    QCOMPARE(r->hash, message::identityHash(kOwnUid, text));
    QVERIFY(qAbs(r->receivedSec - now) <= 2);
    QVERIFY(pipeline->fetching(t.key));
    // The website's files go to the chat's links folder.
    const FakeFetch::Load load = m_fetch->loads.first();
    QCOMPARE(load.cacheDir, linksCacheDir(m_core.get()));
    QCOMPARE(pipeline->cacheDir(), linksCacheDir(m_core.get()));
    QByteArray jpeg;
    {
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(picture(800, 600).save(&buffer, "JPG"));
    }
    const int seen = changed.count();
    answerFile(jpeg, QStringLiteral("photo.jpg"));
    // Fetched at once: before anything is made or uploaded.
    r = pipeline->latestOwnLink(t.key);
    QCOMPARE(r->state, State::Fetched);
    QVERIFY(r->result.ok);
    QCOMPARE(r->result.localFile, load.cacheDir + QStringLiteral("/photo.jpg"));
    QVERIFY(!pipeline->fetching(t.key));
    QCOMPARE(changed.count(), seen + 1);
    QVERIFY(fakets3::uploads().isEmpty());
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    QTRY_COMPARE_WITH_TIMEOUT(pipeline->jobCount(), 0, 3000);
    // After the LP the record still says Fetched (our chat keeps the website's files: no swap).
    QCOMPARE(pipeline->latestOwnLink(t.key)->state, State::Fetched);
    // Copied, not moved: the file is still there for our chat; the work folder is empty.
    QVERIFY(QFileInfo::exists(load.cacheDir + QStringLiteral("/photo.jpg")));
    QTRY_VERIFY_WITH_TIMEOUT(QDir(ts3::dataDir() + QStringLiteral("/linkpreview/send")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(), 3000);
    // Found by the message's identity and time (the chat's key), within textreact::kToleranceSec.
    const QString hash = message::identityHash(kOwnUid, text);
    QVERIFY(pipeline->ownLink(hash, r->receivedSec, t.key));
    QVERIFY(pipeline->ownLink(hash, r->receivedSec + textreact::kToleranceSec, t.key));
    QVERIFY(!pipeline->ownLink(hash, r->receivedSec + textreact::kToleranceSec + 1, t.key));
    QVERIFY(!pipeline->ownLink(hash, r->receivedSec, QStringLiteral("00112233445566778899aabbccddeeff")));
    QVERIFY(!pipeline->ownLink(message::identityHash(kOwnUid, QStringLiteral("other")), r->receivedSec, t.key));
    // Bounded: the newest kMaxOwnLinks (links refused at intake get records too); 15 minutes at most.
    QCOMPARE(SenderPipeline::kOwnLinkTtlMs, 15LL * 60 * 1000);
    for (int i = 0; i < SenderPipeline::kMaxOwnLinks + 8; ++i)
        sendOwn(QStringLiteral("https://b.example.com/%1.png").arg(i));
    QCOMPARE(pipeline->ownLinkCount(), SenderPipeline::kMaxOwnLinks);
    QVERIFY(!pipeline->latestOwnLink(t.key));
    QVERIFY(pipeline->latestOwnLink(classify(QStringLiteral("https://b.example.com/%1.png").arg(SenderPipeline::kMaxOwnLinks + 7)).key));
    // Shutdown clears them without a word.
    changed.clear();
    m_hub->prepareShutdown();
    QCOMPARE(pipeline->ownLinkCount(), 0);
    QTest::qWait(50);
    QCOMPARE(changed.count(), 0);
}

void TestLinkPreviewSnd::ownLinkRecordStates()
{
    using State = SenderPipeline::OwnLink::State;
    SenderPipeline* pipeline = m_hub->pipeline();
    const auto      stateOf  = [pipeline](const QString& url) {
        const SenderPipeline::OwnLink* r = pipeline->latestOwnLink(classify(url).key);
        return r ? static_cast<int>(r->state) : -1;
    };
    // The website refuses: Failed, with the error.
    sendOwn(QStringLiteral("[URL]https://example.com/a.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    Result refused;
    refused.error = QString::fromLatin1(errors::kForbidden);
    pipeline->fetched(m_fetch->loads.last().target.key, refused);
    QCOMPARE(stateOf(QStringLiteral("https://example.com/a.png")), int(State::Failed));
    QCOMPARE(pipeline->latestOwnLink(classify(QStringLiteral("https://example.com/a.png")).key)->result.error, QString::fromLatin1(errors::kForbidden));
    // Too large: Failed, with the size (our chat then knows it without asking).
    sendOwn(QStringLiteral("[URL]https://example.com/huge.mp4[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    Result large;
    large.error = QString::fromLatin1(errors::kTooLarge);
    large.size  = 38LL * 1024 * 1024;
    pipeline->fetched(m_fetch->loads.last().target.key, large);
    const SenderPipeline::OwnLink* huge = pipeline->latestOwnLink(classify(QStringLiteral("https://example.com/huge.mp4")).key);
    QCOMPARE(huge->state, State::Failed);
    QCOMPARE(huge->result.size, 38LL * 1024 * 1024);
    // The same link again soon: its files are on the server already (Reused: the store has our LP at once).
    const QString yt = QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]");
    sendOwn(yt);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return sentTypes().contains("LP"); }));
    QTRY_COMPARE_WITH_TIMEOUT(pipeline->jobCount(), 0, 3000);
    sendOwn(QStringLiteral("again ") + yt);
    QTRY_COMPARE_WITH_TIMEOUT(stateOf(QStringLiteral("https://youtu.be/dQw4w9WgXcQ")), int(State::Reused), 3000);
    QCOMPARE(m_fetch->loads.size(), 3);
    // Rate-limited at intake: Skipped (no job).
    for (int i = 0; i < 4; ++i)
        sendOwn(QStringLiteral("https://c.example.com/%1a.png https://c.example.com/%1b.png").arg(i));
    const SenderPipeline::OwnLink* last = pipeline->latestOwnLink(classify(QStringLiteral("https://c.example.com/3b.png")).key);
    QVERIFY(last);
    QCOMPARE(last->state, State::Skipped);
    QCOMPARE(last->jobId, 0);
    m_hub->prepareShutdown();

    // The website takes too long: Failed (timeout); a link queued behind it past its time: dropped (Skipped). Both
    // tell the channel (LPNO): they were accepted, and we are still here.
    SenderPipeline::Limits limits;
    limits.fetchTimeoutMs = 200;
    limits.queuedTtlMs    = 100;
    auto own = std::make_unique<SenderPipeline>(m_core.get(), m_link.get(), nullptr, &m_index, m_fetch.get(), limits);
    const int before = m_fetch->loads.size();
    own->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, fakets3::kOwnClient, kOwnUid,
                       QStringLiteral("[URL]https://example.com/slow.png[/URL] [URL]https://example.com/next.png[/URL]"));
    QCOMPARE(m_fetch->loads.size(), before + 1);
    const Target slow = classify(QStringLiteral("https://example.com/slow.png"));
    const Target next = classify(QStringLiteral("https://example.com/next.png"));
    QCOMPARE(own->latestOwnLink(next.key)->state, State::Queued);
    QTRY_COMPARE_WITH_TIMEOUT(own->jobCount(), 0, 5000);
    QCOMPARE(own->latestOwnLink(slow.key)->state, State::Failed);
    QCOMPARE(own->latestOwnLink(slow.key)->result.error, QString::fromLatin1(errors::kTimeout));
    QCOMPARE(own->latestOwnLink(next.key)->state, State::Skipped);
    QCOMPARE(m_fetch->loads.size(), before + 1); // the second never went to the website
    QVERIFY(m_fetch->canceled.contains(slow.key));
    const auto noticesFor = [this, &slow, &next] {
        QStringList keys;
        for (const proto::Message& m : sentMessages()) {
            const std::optional<Skip> skip = readSkip(m);
            if (skip && (skip->urlKey == slow.key || skip->urlKey == next.key))
                keys << skip->urlKey;
        }
        return keys;
    };
    QVERIFY(serveUntil([&noticesFor] { return noticesFor().size() == 2; }));
    QCOMPARE(noticesFor(), QStringList({slow.key, next.key}));
    QCOMPARE(own->counters().skipNotices, 2);
    own.reset();
}

// ---- the pipeline -------------------------------------------------------------------------------------

void TestLinkPreviewSnd::youTubeEndToEnd()
{
    const QString text = QStringLiteral("watch this [URL]https://youtu.be/dQw4w9WgXcQ[/URL]");
    sendOwn(text);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const FakeFetch::Load load = m_fetch->loads.first();
    QCOMPARE(load.target.kind, Kind::YouTube);
    QCOMPARE(load.target.videoId, QStringLiteral("dQw4w9WgXcQ"));
    QCOMPARE(load.maxBytes, SenderPipeline::Limits().thumbnailBytes);
    QVERIFY(load.cacheDir.startsWith(ts3::dataDir()));
    answerYouTube();
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    QCOMPARE(m_backend->sent.size(), 1);
    QVERIFY(m_backend->sent.first().target.channel);
    const std::optional<Incoming> in = lastSent();
    QVERIFY(in);
    const Announcement& a = in->announcement;
    QCOMPARE(in->scope, proto::Scope::Channel);
    QCOMPARE(in->hash, message::identityHash(kOwnUid, text));
    QVERIFY(in->ageSec >= 0 && in->ageSec < 30);
    QCOMPARE(a.kind, Kind::YouTube);
    QCOMPARE(a.urlKey, load.target.key);
    QCOMPARE(a.channelId, fakets3::kChannel);
    QVERIFY(a.previewPath.startsWith(QLatin1String("/tsmedia/previews/")));
    QVERIFY(a.mediaPath.isEmpty());
    QCOMPARE(a.title, QStringLiteral("Never gonna give you up")); // bidi marks out
    QCOMPARE(a.author, QStringLiteral("Rick Astley"));
    // hqdefault's 4:3 with black bars: only the 16:9 picture goes up.
    QCOMPARE(a.width, 480);
    QCOMPARE(a.height, 270);
    // The preview on the server is the very file announced.
    const QByteArray onServer = fakets3::serverFile(a.previewPath);
    QVERIFY(!onServer.isEmpty());
    QCOMPARE(QImage::fromData(onServer).size(), QSize(480, 270));
    QCOMPARE(a.previewSha, shaOf(onServer));
    QCOMPARE(a.previewSize, qint64(onServer.size()));
    QCOMPARE(maker::sniff(onServer), maker::Format::Jpeg);
    // No chat message, no chat line, no toast job left waiting; the LP spent the plugin bucket.
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(chatLines(), 0);
    QCOMPARE(m_core->floodGovernor(fakets3::kConnection).counters().commandsSent, 1);
    QCOMPARE(m_hub->pipeline()->counters().announced, 1);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    // Our own chat shows it at once: the store has it under our message, Core's cache has the file.
    const Announcement* own = m_hub->store()->find(textreact::localKey(in->hash, QDateTime::currentSecsSinceEpoch()), a.urlKey);
    QVERIFY(own);
    QCOMPARE(own->senderUid, kOwnUid);
    const MediaEntry* entry = m_core->entry(linkFor(*own).key());
    QVERIFY(entry);
    QCOMPARE(entry->state, MediaState::Ready);
    // The work files are gone.
    QTRY_VERIFY_WITH_TIMEOUT(QDir(ts3::dataDir() + QStringLiteral("/linkpreview/send")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(), 3000);
}

void TestLinkPreviewSnd::imageWithItsFile()
{
    sendOwn(QStringLiteral("[URL]https://example.com/photo.jpg?size=large[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QCOMPARE(m_fetch->loads.first().target.kind, Kind::Image);
    QByteArray jpeg;
    {
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(picture(2400, 1600).save(&buffer, "JPG"));
    }
    answerFile(jpeg, QStringLiteral("download.bin"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const std::optional<Incoming> in = lastSent();
    QVERIFY(in);
    const Announcement& a = in->announcement;
    QCOMPARE(a.kind, Kind::Image);
    QCOMPARE(a.width, 2400);
    QCOMPARE(a.height, 1600);
    // The file itself (it is within the automatic download limit) and a smaller preview, both checked.
    QVERIFY(a.mediaPath.startsWith(QLatin1String("/tsmedia/link_")));
    QVERIFY(a.mediaPath.endsWith(QLatin1String(".jpg")));
    QCOMPARE(fakets3::serverFile(a.mediaPath), jpeg);
    QCOMPARE(a.mediaSha, shaOf(jpeg));
    QCOMPARE(a.mediaSize, qint64(jpeg.size()));
    QVERIFY(a.previewPath.startsWith(QLatin1String("/tsmedia/previews/")));
    const QByteArray preview = fakets3::serverFile(a.previewPath);
    QCOMPARE(a.previewSha, shaOf(preview));
    QImage shown;
    QVERIFY(shown.loadFromData(preview));
    QCOMPARE(qMax(shown.width(), shown.height()), 1280);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(chatLines(), 0);
}

void TestLinkPreviewSnd::transparentPictureKeepsAlpha()
{
    // Above the automatic download limit: only the preview goes up, as a PNG with its transparency.
    Settings::instance().autoDownloadMaxMB = 1;
    sendOwn(QStringLiteral("https://example.com/logo.png"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImage           big = picture(1100, 1100, true);
        QRandomGenerator noise(42); // noise, so the PNG is larger than 1 MB
        for (int y = 0; y < big.height(); ++y)
            for (int x = 0; x < big.width() / 2; ++x)
                big.setPixel(x, y, qRgba(noise.bounded(256), noise.bounded(256), noise.bounded(256), 64 + noise.bounded(128)));
        QVERIFY(big.save(&buffer, "PNG"));
    }
    QVERIFY(png.size() > 1024 * 1024);
    answerFile(png, QStringLiteral("logo.png"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const Announcement a = lastSent()->announcement;
    QVERIFY(a.mediaPath.isEmpty());
    QVERIFY(a.previewPath.endsWith(QLatin1String(".png")));
    QImage shown;
    QVERIFY(shown.loadFromData(fakets3::serverFile(a.previewPath)));
    QVERIFY(shown.hasAlphaChannel());
}

void TestLinkPreviewSnd::videoPosterAndFile()
{
    m_hub->pipeline()->setVideoProbe([](const QString& path) {
        LocalMediaInfo info;
        info.kind       = MediaKind::Video;
        info.width      = 1920;
        info.height     = 1080;
        info.durationMs = 12345;
        info.preview    = QFileInfo(path).isFile() ? picture(960, 540) : QImage();
        return info;
    });
    sendOwn(QStringLiteral("clip: [URL]https://cdn.example.com/v/clip.mp4[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QCOMPARE(m_fetch->loads.first().maxBytes, SenderPipeline::Limits().videoBytes);
    QByteArray mp4("\0\0\0\x20" "ftypisom" "\0\0\x02\0" "isomiso2avc1mp41", 32);
    mp4 += QByteArray(50000, 'v');
    answerFile(mp4, QStringLiteral("clip"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const Announcement a = lastSent()->announcement;
    QCOMPARE(a.kind, Kind::Video);
    QCOMPARE(a.durationMs, qint64(12345));
    QCOMPARE(a.width, 1920);
    QVERIFY(a.mediaPath.endsWith(QLatin1String(".mp4")));
    QCOMPARE(fakets3::serverFile(a.mediaPath), mp4);
    QCOMPARE(a.mediaSha, shaOf(mp4));
    QCOMPARE(maker::sniff(fakets3::serverFile(a.previewPath)), maker::Format::Jpeg);

    // A video above the cap: only its poster goes up ("Open in browser" for the rest).
    m_backend->sent.clear();
    Settings::instance().uploadMaxMB = 1;
    sendOwn(QStringLiteral("[URL]https://cdn.example.com/v/big.mov[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    QByteArray mov("\0\0\0\x14" "ftypqt  " "\0\0\0\0" "qt  ", 20);
    mov += QByteArray(2 * 1024 * 1024, 'm');
    answerFile(mov, QStringLiteral("big"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const Announcement big = lastSent()->announcement;
    QCOMPARE(big.kind, Kind::Video);
    QVERIFY(big.mediaPath.isEmpty());
    QVERIFY(big.previewPath.startsWith(QLatin1String("/tsmedia/previews/")));
    QCOMPARE(big.durationMs, qint64(12345));
}

void TestLinkPreviewSnd::gifTooLargeGoesAsStill()
{
    Settings::instance().autoDownloadMaxMB = 1;
    sendOwn(QStringLiteral("[URL]https://example.com/fun.gif[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    if (!QImageReader::supportedImageFormats().contains("gif"))
        QSKIP("no GIF reader in this Qt");
    // A 1x1 GIF89a (Qt has no GIF writer), then bytes after its end: still a GIF, now too large.
    QByteArray gif("GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00"
                   "\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b",
                   43);
    QCOMPARE(maker::sniff(gif), maker::Format::Gif);
    gif += QByteArray(2 * 1024 * 1024, '\0');
    answerFile(gif, QStringLiteral("fun.gif"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const Announcement a = lastSent()->announcement;
    QCOMPARE(a.kind, Kind::Gif);
    QVERIFY(a.mediaPath.isEmpty());
    QCOMPARE(maker::sniff(fakets3::serverFile(a.previewPath)), maker::Format::Jpeg);
}

void TestLinkPreviewSnd::privateChatAnnouncesToThePartner()
{
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"), TextMessageTarget_CLIENT, kAlice);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const LinkBackend::Sent sent = m_backend->sent.first();
    QVERIFY(!sent.target.channel);
    QCOMPARE(sent.target.clients, QVector<peers::ClientId>({kAlice}));
    QCOMPARE(lastSent()->scope, proto::Scope::Private);
    QVERIFY(fakets3::messages().isEmpty());

    // The partner reconnected with another client id: found again by identity.
    m_backend->sent.clear();
    fakets3::removeClient(kAlice);
    fakets3::addClient({41, kAliceUid, fakets3::kChannel, QStringLiteral("Alice"), false});
    sendOwn(QStringLiteral("[URL]https://youtu.be/aaaaaaaaaaa[/URL]"), TextMessageTarget_CLIENT, 41);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    fakets3::removeClient(41);
    fakets3::addClient({42, kAliceUid, fakets3::kChannel, QStringLiteral("Alice"), false});
    answerYouTube();
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    QCOMPARE(m_backend->sent.first().target.clients, QVector<peers::ClientId>({42}));
}

void TestLinkPreviewSnd::onlyOwnChannelAndPrivateMessages()
{
    const QString yt = QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]");
    // Someone else's message.
    m_hub->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, kAlice, kAliceUid, yt);
    // Our id with another identity (a spoofed callback can't make us fetch).
    m_hub->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, fakets3::kOwnClient, kAliceUid, yt);
    // The server chat: no LP there.
    sendOwn(yt, TextMessageTarget_SERVER);
    // TS Media's own media message.
    sendOwn(QStringLiteral("[URL=ts3file://127.0.0.1?port=9987&path=/&filename=a.png]a.png[/URL] ") + yt);
    QTest::qWait(50);
    QVERIFY(m_fetch->loads.isEmpty());
    // 2.5: a web page and an http link to a file get their previews too (LPX: a card, the file over https).
    sendOwn(QStringLiteral("https://example.com/page http://example.com/a.png"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QCOMPARE(m_hub->pipeline()->jobCount(), 2);
    QCOMPARE(m_fetch->loads.first().target.cls, LinkClass::Web);
    newHub();
    // Too many links in one message: at most three, each once (the files first).
    sendOwn(QStringLiteral("https://a.example.com/1.png https://a.example.com/1.png https://a.example.com/2.png https://a.example.com/3.png https://a.example.com/4.png"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    QCOMPARE(m_hub->pipeline()->jobCount(), 3); // one at a time, the others wait
    m_hub->pipeline()->shutdown();
    QCOMPARE(m_hub->pipeline()->jobCount(), 0);
}

void TestLinkPreviewSnd::failuresSendOneSkipNotice()
{
    // The website refuses.
    const QString first = QStringLiteral("[URL]https://example.com/a.png[/URL]");
    sendOwn(first);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    Result refused;
    refused.error = QStringLiteral("HTTP 403 from https://example.com/a.png?token=secret"); // the log never gets the address
    m_hub->pipeline()->fetched(m_fetch->loads.first().target.key, refused);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    // Not a picture after all.
    sendOwn(QStringLiteral("[URL]https://example.com/b.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    answerFile(QByteArray("<html>not a picture</html>"), QStringLiteral("b.png"));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    // No upload permission on this server.
    m_refuseUploads = true;
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return m_hub->pipeline()->jobCount() == 0; }));
    QCOMPARE(m_hub->pipeline()->counters().failed, 3);
    // Core's job for it is a background one: no toast row, no Retry, gone soon.
    QVERIFY(!m_core->uploadIds().isEmpty());
    for (const int id : m_core->uploadIds()) {
        QVERIFY(m_core->upload(id)->background);
        QCOMPARE(m_core->upload(id)->state, UploadState::Failed);
        QVERIFY(!m_core->canRetryUpload(id));
    }
    // 2.4 auto-load: nothing announced, but each failure tells the chat once (LPNO, background, to the channel), so
    // the people waiting for our preview load the link themselves.
    QVERIFY(serveUntil([this] { return m_backend->sent.size() >= 3; }));
    serve();
    QCOMPARE(sentTypes(), QByteArrayList({"LPNO", "LPNO", "LPNO"}));
    const QList<proto::Message> notices = sentMessages();
    const QStringList texts{first, QStringLiteral("[URL]https://example.com/b.png[/URL]"), QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]")};
    for (int i = 0; i < 3; ++i) {
        QVERIFY(m_backend->sent.at(i).target.channel);
        const std::optional<Skip> skip = readSkip(notices.at(i));
        QVERIFY(skip);
        QCOMPARE(skip->scope, proto::Scope::Channel);
        QCOMPARE(skip->hash, message::identityHash(kOwnUid, texts.at(i)));
        QCOMPARE(skip->urlKey, m_fetch->loads.at(i).target.key);
        QVERIFY(skip->ageSec >= 0 && skip->ageSec < 30);
        QVERIFY(!readMessage(notices.at(i))); // never an LP
    }
    QCOMPARE(m_hub->pipeline()->counters().skipNotices, 3);
    QCOMPARE(m_hub->pipeline()->counters().announced, 0);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(chatLines(), 0); // and nothing said in the chat: the log has it
    QVERIFY(fakets3::logContains(QStringLiteral("No link preview for")));
    QVERIFY(!fakets3::logContains(QStringLiteral("https://"))); // never the address
    QVERIFY(!fakets3::logContains(QStringLiteral("token=secret")));
    QVERIFY(fakets3::logContains(QStringLiteral("HTTP 403")));
    QCOMPARE(m_hub->store()->count(), 0);
}

void TestLinkPreviewSnd::sameLinkAgainReusesTheFiles()
{
    const QString yt = QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]");
    sendOwn(yt);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 1; }));
    const int uploads = fakets3::uploads().size();
    sendOwn(QStringLiteral("again ") + yt);
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 2; }));
    QCOMPARE(m_fetch->loads.size(), 1); // no second fetch ...
    QCOMPARE(fakets3::uploads().size(), uploads); // ... and no second upload
    const std::optional<proto::Message> first  = proto::parse(m_backend->sent.at(0).payload);
    const std::optional<Incoming>       second = lastSent();
    QVERIFY(first && second);
    QCOMPARE(second->announcement.previewPath, readMessage(*first)->announcement.previewPath);
    QCOMPARE(second->hash, message::identityHash(kOwnUid, QStringLiteral("again ") + yt)); // for the new message
    QCOMPARE(m_hub->pipeline()->counters().reused, 1);
}

// The real Fetcher over a counted website: a link you send costs exactly one request to its website (one route,
// one name lookup, one GET); sending it again reuses the files on the server; other people's messages, an
// announcement received and the settings off cost no network call at all.
void TestLinkPreviewSnd::oneWebsiteRequestPerSend()
{
    auto site = std::make_shared<CountingWebsite>();
    {
        QBuffer buffer(&site->body);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(picture(640, 480).save(&buffer, "PNG"));
    }
    Fetcher     fetcher(site, Fetcher::Options());
    FetcherPort port(&fetcher);
    struct HubGoesFirst { // the hub's pipeline uses the port and the Fetcher: it goes before them, also on a failure
        std::unique_ptr<Hub>& hub;
        ~HubGoesFirst() { hub.reset(); }
    } hubGoesFirst{m_hub};
    m_hub.reset();
    m_hub = std::make_unique<Hub>(m_core.get(), m_link.get(), &m_index, &port, Hub::Limits());
    m_hub->start();
    connect(&fetcher, &Fetcher::finished, m_hub->pipeline(), &SenderPipeline::fetched);

    const QString link = QStringLiteral("[URL]https://example.com/cat.png[/URL]");
    sendOwn(QStringLiteral("look ") + link);
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 1; }));
    QCOMPARE(site->requested, QStringList{QStringLiteral("https://example.com/cat.png")});
    QCOMPARE(site->calls(), 3);

    // 2.4 auto-load: the download is in the chat's links folder (kept: our own chat shows it, and a later load of the
    // link anywhere here costs no request). The chat's own Fetcher answers from it.
    const Target cat = classify(QStringLiteral("https://example.com/cat.png"));
    Result       cached;
    QVERIFY(Fetcher::cached(cat, linksCacheDir(m_core.get()), &cached));
    QVERIFY(QFileInfo::exists(cached.localFile));
    {
        Fetcher    chat(site, Fetcher::Options());
        QSignalSpy shown(&chat, &Fetcher::finished);
        chat.load(cat, 25 << 20, linksCacheDir(m_core.get()));
        QTRY_COMPARE_WITH_TIMEOUT(shown.count(), 1, 3000);
        QVERIFY(shown.first().at(1).value<Result>().ok);
        QCOMPARE(site->calls(), 3);
    }
    // The record our chat finds the message by is the identity every chat computes from the document.
    const SenderPipeline::OwnLink* record = m_hub->pipeline()->latestOwnLink(cat.key);
    QVERIFY(record);
    QCOMPARE(record->state, SenderPipeline::OwnLink::State::Fetched);
    {
        const tschat::Skin skin = tschat::darkSkin();
        QTextDocument      doc;
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:17"), 7, kOwnUid, QStringLiteral("Me"),
                                                 QStringLiteral("look ") + tschat::linkHtml(skin, QStringLiteral("https://example.com/cat.png")), true));
        const textreactdoc::Identity id = textreactdoc::identify(doc.lastBlock());
        QVERIFY(id.eligible());
        QCOMPARE(record->hash, id.hash);
    }

    // The same link again in this channel: announced with the files already on the server.
    sendOwn(QStringLiteral("again ") + link);
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 2; }));
    QCOMPARE(m_hub->pipeline()->counters().reused, 1);

    // Someone else's message with links, and a preview announced by its sender: nothing from any website.
    m_hub->onTextMessage(fakets3::kConnection, TextMessageTarget_CHANNEL, 0, kAlice, kAliceUid, QStringLiteral("[URL]https://example.com/dog.png[/URL]"));
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("https://example.com/dog.png"));
    indexMessage(hash, QDateTime::currentSecsSinceEpoch() - 2, kAliceUid);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->counters().accepted, 1);

    // Either setting off: your links aren't fetched.
    Settings::instance().makeLinkPreviews = false;
    m_hub->applySettings();
    sendOwn(QStringLiteral("[URL]https://example.com/bird.png[/URL]"));
    Settings::instance().makeLinkPreviews = true;
    Settings::instance().showLinkPreviews = false;
    m_hub->applySettings();
    sendOwn(QStringLiteral("[URL]https://example.com/fish.png[/URL] [URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QVERIFY(!serveUntil([this] { return m_backend->sent.size() > 2 || m_hub->pipeline()->busy(); }, 500));
    QCOMPARE(site->calls(), 3);
    QCOMPARE(site->requested.size(), 1);
}

void TestLinkPreviewSnd::teaSpeakNeverAskedForFileInfo()
{
    fakets3::enableFileInfo(true);
    fakets3::setServerVersion(QStringLiteral("1.5.6 [Build: 1] TeaSpeak"), QStringLiteral("Linux"));
    m_core->noteServer(fakets3::kConnection);
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    QVERIFY(fakets3::fileInfoRequests().isEmpty()); // it would drop the connection there
    QVERIFY(!fakets3::checkDownloads().isEmpty());  // the bytes were checked instead (2.3.2)
}

void TestLinkPreviewSnd::teaSpeakPosterIsOnTheServerBeforeItsVideoGoesUp()
{
    // Seen with 2.4.0 on TeaSpeak (no file info there: only the 2.3.2 tail check): a link's video went up with
    // its 168 KB poster. The poster's upload was reported complete, the 2.8 MB video's started right away, and
    // the poster's end never reached the server: after the video, the poster's check found the same short copy
    // for 60 s and the preview was announced after 67 s, naming a poster the server didn't have whole. TeaSpeak
    // has an upload's bytes a while after TeamSpeak reports it complete; this fake server loses the end of one
    // still arriving when the next upload starts, as the poster did. The poster is confirmed on the server
    // before its video goes up, and what the server has is what the LP's h= and ps= describe.
    fakets3::enableFileInfo(true);
    fakets3::setServerVersion(QStringLiteral("1.5.6 [Build: 1] TeaSpeak"), QStringLiteral("Linux"));
    m_core->noteServer(fakets3::kConnection);
    m_hub->pipeline()->setVideoProbe(busyVideoProbe);
    sendOwn(QStringLiteral("[URL]https://samplelib.example.com/sample-5s.mp4[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const QByteArray mp4 = sampleMp4(400000);
    answerFile(mp4, QStringLiteral("sample"));

    QHash<QString, QByteArray> arriving; // reported complete, not all on the server yet: what it will have
    QStringList                cut;      // ... and never will: another upload started first
    QStringList                order;    // "upload <path>" and "check <path>", as Core asked for them
    const auto                 serveTeaSpeak = [&] {
        for (const fakets3::Directory& d : fakets3::directories()) {
            if (!m_answered.contains(d.returnCode)) {
                m_answered.insert(d.returnCode);
                m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
            }
        }
        for (const fakets3::Transfer& t : fakets3::uploads()) {
            if (t.halted || m_completed.contains(t.id))
                continue;
            m_completed.insert(t.id);
            order << QStringLiteral("upload ") + t.remotePath;
            for (auto it = arriving.begin(); it != arriving.end(); it = arriving.erase(it))
                cut << it.key();
            if (!fakets3::completeUpload(t.id))
                continue;
            const QByteArray all = fakets3::serverFile(t.remotePath);
            fakets3::putFile(t.remotePath, halfArrived(all));
            arriving.insert(t.remotePath, all);
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
        }
        for (const fakets3::Transfer& t : fakets3::checkDownloads()) {
            if (t.halted || m_served.contains(t.id))
                continue;
            m_served.insert(t.id);
            order << QStringLiteral("check ") + t.remotePath;
            if (fakets3::deliverServerFile(t.id).isEmpty())
                continue;
            if (arriving.contains(t.remotePath))
                fakets3::putFile(t.remotePath, arriving.take(t.remotePath)); // all there after its first check
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
        }
    };
    QElapsedTimer clock;
    clock.start();
    while (m_backend->sent.isEmpty() && clock.elapsed() < 15000) {
        serveTeaSpeak();
        QTest::qWait(20);
    }
    QVERIFY(fakets3::fileInfoRequests().isEmpty()); // TeaSpeak is never asked (it would drop the connection)
    QVERIFY2(cut.isEmpty(), qPrintable(QStringLiteral("an upload started while these were still arriving: ") + cut.join(QStringLiteral(", "))));
    const std::optional<Incoming> in = lastSent();
    QVERIFY(in);
    const Announcement& a = in->announcement;
    QCOMPARE(a.kind, Kind::Video);
    QVERIFY(a.previewPath.startsWith(QLatin1String("/tsmedia/previews/")));
    // The poster went up, was checked (it was still arriving at first) and only then did the video go up.
    const QStringList expected{QStringLiteral("upload ") + a.previewPath, QStringLiteral("check ") + a.previewPath, QStringLiteral("check ") + a.previewPath,
                               QStringLiteral("upload ") + a.mediaPath,   QStringLiteral("check ") + a.mediaPath,   QStringLiteral("check ") + a.mediaPath};
    QCOMPARE(order, expected);
    // What the server has is what the LP describes, and what each check compared.
    QCOMPARE(fakets3::serverFile(a.mediaPath), mp4);
    QCOMPARE(a.mediaSha, shaOf(mp4));
    QCOMPARE(a.mediaSize, qint64(mp4.size()));
    const QByteArray poster = fakets3::serverFile(a.previewPath);
    QVERIFY(poster.size() > 64 * 1024);
    QCOMPARE(maker::sniff(poster), maker::Format::Jpeg);
    QCOMPARE(a.previewSize, qint64(poster.size()));
    QCOMPARE(a.previewSha, shaOf(poster));
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + a.previewPath + QStringLiteral(" (content checked: tail)")));
    QVERIFY(fakets3::logContains(QStringLiteral("Server has all of ") + a.mediaPath + QStringLiteral(" (content checked: tail)")));
    QVERIFY(!fakets3::logContains(QStringLiteral("still doesn't end like the uploaded file")));
    // 2.4: a check that found other bytes says what came back.
    QVERIFY(fakets3::logContains(QStringLiteral("The check of ") + a.previewPath + QStringLiteral(" brought ")));
    // Our own chat shows it from Core's cache: the poster there is the announced one.
    Announcement stored = a; // as the receiving side stores it: the server is its connection's
    stored.serverUid    = QString::fromLatin1(fakets3::kServerUid);
    const MediaEntry* video = m_core->entry(linkFor(stored).key());
    QVERIFY(video);
    QCOMPARE(video->state, MediaState::Ready);
    QCOMPARE(video->previewState, MediaState::Ready);
    QFile cachedPoster(video->previewPath);
    QVERIFY(cachedPoster.open(QIODevice::ReadOnly));
    QCOMPARE(cachedPoster.readAll(), poster);
    QTRY_VERIFY_WITH_TIMEOUT(!m_hub->pipeline()->busy(), 3000);
    QCOMPARE(m_hub->pipeline()->counters().announced, 1);
}

void TestLinkPreviewSnd::askableServerSendsTheVideoRightAfterItsPoster()
{
    // A server that can be asked for a file's size (TeamSpeak's own) has an upload's bytes when it says so: the
    // video goes up right after its poster as before, and nothing is asked about the poster first.
    fakets3::enableFileInfo(true);
    m_core->noteServer(fakets3::kConnection);
    m_hub->pipeline()->setVideoProbe(busyVideoProbe);
    sendOwn(QStringLiteral("[URL]https://samplelib.example.com/sample-5s.mp4[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const QByteArray mp4 = sampleMp4(200000);
    answerFile(mp4, QStringLiteral("sample"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const Announcement a = lastSent()->announcement;
    QCOMPARE(fakets3::uploads().size(), 2);
    QCOMPARE(fakets3::uploads().at(0).remotePath, a.previewPath);
    QCOMPARE(fakets3::uploads().at(1).remotePath, a.mediaPath);
    for (const fakets3::InfoRequest& r : fakets3::fileInfoRequests())
        QCOMPARE(r.path, a.mediaPath); // the video was all there at once: its poster isn't asked about
    for (const fakets3::Transfer& t : fakets3::checkDownloads())
        QCOMPARE(t.remotePath, a.mediaPath);
    QCOMPARE(fakets3::serverFile(a.mediaPath), mp4);
    QCOMPARE(a.previewSha, shaOf(fakets3::serverFile(a.previewPath)));
}

void TestLinkPreviewSnd::stopsWhileThePosterIsChecked()
{
    // The poster's check (TeaSpeak) runs while the job still prepares: switched off, unloaded or disconnected
    // then, its download stops, the video never goes up, nothing is announced, and (but for the lost
    // connection) the poster leaves the file browser.
    for (int round = 0; round < 3; ++round) {
        cleanup();
        init();
        fakets3::enableFileInfo(true);
        fakets3::setServerVersion(QStringLiteral("1.5.6 [Build: 1] TeaSpeak"), QStringLiteral("Linux"));
        m_core->noteServer(fakets3::kConnection);
        m_hub->pipeline()->setVideoProbe(busyVideoProbe);
        sendOwn(QStringLiteral("[URL]https://samplelib.example.com/sample-5s.mp4[/URL]"));
        QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
        answerFile(sampleMp4(200000), QStringLiteral("sample"));
        // Folders answered and the poster's upload completed; its check's download is left running.
        QElapsedTimer clock;
        clock.start();
        while (fakets3::checkDownloads().isEmpty() && clock.elapsed() < kTimeoutMs) {
            for (const fakets3::Directory& d : fakets3::directories()) {
                if (!m_answered.contains(d.returnCode)) {
                    m_answered.insert(d.returnCode);
                    m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
                }
            }
            for (const fakets3::Transfer& t : fakets3::uploads()) {
                if (!m_completed.contains(t.id) && fakets3::completeUpload(t.id)) {
                    m_completed.insert(t.id);
                    m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
                }
            }
            QTest::qWait(20);
        }
        QCOMPARE(fakets3::uploads().size(), 1); // the poster only
        QCOMPARE(fakets3::checkDownloads().size(), 1);
        const fakets3::Transfer posterUpload = fakets3::uploads().first();
        const fakets3::Transfer check        = fakets3::checkDownloads().first();
        QCOMPARE(check.remotePath, posterUpload.remotePath);
        QVERIFY(fakets3::hasServerFile(posterUpload.remotePath));
        if (round == 0) {
            Settings::instance().makeLinkPreviews = false;
            m_hub->applySettings();
        } else if (round == 1) {
            m_hub->prepareShutdown();
        } else {
            fakets3::setConnected(false);
            m_core->onConnectionLost(fakets3::kConnection);
            QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000); // Core's answer comes on a later turn
        }
        QCOMPARE(m_hub->pipeline()->jobCount(), 0);
        QVERIFY(fakets3::transfer(check.id).halted); // the check's download stopped
        if (round < 2)
            QVERIFY(!fakets3::hasServerFile(posterUpload.remotePath)); // no LP will name it
        QTest::qWait(300);
        QCOMPARE(fakets3::uploads().size(), 1); // the video never went up
        QVERIFY(m_backend->sent.isEmpty());
        QCOMPARE(m_hub->pipeline()->counters().announced, 0);
    }
}

void TestLinkPreviewSnd::floodCosts()
{
    // Uploads wait for the general bucket like any send (the user's typing keeps its reserve).
    FloodGovernor& governor = m_core->floodGovernor(fakets3::kConnection);
    const qint64   now      = m_core->floodClockMs();
    while (governor.generalReady(FloodGovernor::Cost::Transfer, now))
        governor.charge(FloodGovernor::Cost::Mkdir, now);
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    QTest::qWait(300);
    serve();
    QVERIFY(fakets3::uploads().isEmpty());
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }, 20000)); // once the bucket allows it
    QCOMPARE(fakets3::uploads().size(), 1);
    QCOMPARE(governor.counters().commandsSent, 1); // one plugin command per preview

    // At most maxJobsPerMinute jobs per connection: a burst of links is cut.
    for (int i = 0; i < 4; ++i)
        sendOwn(QStringLiteral("https://a.example.com/%1a.png https://a.example.com/%1b.png").arg(i));
    QCOMPARE(m_hub->pipeline()->counters().started + m_hub->pipeline()->jobCount() - 1, SenderPipeline::Limits().maxJobsPerMinute);
    QVERIFY(m_hub->pipeline()->counters().skipped >= 3);
}

// 2.4 auto-load: each accepted job ends with exactly one plugin command, its LP or its LPNO: N jobs, N commands.
void TestLinkPreviewSnd::floodCostsOneCommandPerJob()
{
    FloodGovernor& governor = m_core->floodGovernor(fakets3::kConnection);
    const int      before   = governor.counters().commandsSent;
    // A success, a website failure, a file that isn't one, a success again (another link), an upload refused.
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 1; }));
    sendOwn(QStringLiteral("[URL]https://example.com/a.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    Result refused;
    refused.error = QStringLiteral("HTTP 404");
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, refused);
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 2; }));
    sendOwn(QStringLiteral("[URL]https://example.com/b.png[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    answerFile(QByteArray("<html>no</html>"), QStringLiteral("b.png"));
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 3; }));
    sendOwn(QStringLiteral("[URL]https://youtu.be/aaaaaaaaaaa[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 4, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 4; }));
    m_refuseUploads = true;
    sendOwn(QStringLiteral("[URL]https://youtu.be/bbbbbbbbbbb[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 5, 3000);
    answerYouTube();
    QVERIFY(serveUntil([this] { return m_backend->sent.size() == 5 && m_hub->pipeline()->jobCount() == 0; }));
    QTest::qWait(100);
    serve();
    QCOMPARE(sentTypes(), QByteArrayList({"LP", "LPNO", "LPNO", "LP", "LPNO"}));
    QCOMPARE(governor.counters().commandsSent - before, 5);
    const SenderPipeline::Counters c = m_hub->pipeline()->counters();
    QCOMPARE(c.announced + c.skipNotices, 5);
}

void TestLinkPreviewSnd::settingOffStopsTheJob()
{
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    Settings::instance().makeLinkPreviews = false;
    m_hub->applySettings();
    QCOMPARE(m_fetch->canceled, QStringList({m_fetch->loads.first().target.key}));
    QCOMPARE(m_hub->pipeline()->jobCount(), 0);
    answerYouTube(); // a late answer changes nothing
    QTest::qWait(100);
    serve();
    QVERIFY(fakets3::uploads().isEmpty());
    sendOwn(QStringLiteral("[URL]https://youtu.be/aaaaaaaaaaa[/URL]"));
    QCOMPARE(m_fetch->loads.size(), 1);
    // On again: new messages make previews again.
    Settings::instance().makeLinkPreviews = true;
    m_hub->applySettings();
    sendOwn(QStringLiteral("[URL]https://youtu.be/aaaaaaaaaaa[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
}

void TestLinkPreviewSnd::shutdownMidUpload()
{
    sendOwn(QStringLiteral("[URL]https://youtu.be/dQw4w9WgXcQ[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerYouTube();
    // Folders answered, the upload started, never completed.
    QElapsedTimer clock;
    clock.start();
    while (fakets3::uploads().isEmpty() && clock.elapsed() < kTimeoutMs) {
        for (const fakets3::Directory& d : fakets3::directories()) {
            if (!m_answered.contains(d.returnCode)) {
                m_answered.insert(d.returnCode);
                m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
            }
        }
        QTest::qWait(20);
    }
    QCOMPARE(fakets3::uploads().size(), 1);
    QCOMPARE(m_hub->pipeline()->jobCount(), 1);
    m_hub->prepareShutdown();
    QCOMPARE(m_hub->pipeline()->jobCount(), 0);
    QVERIFY(fakets3::transfer(fakets3::uploads().first().id).halted); // the transfer stopped
    QTest::qWait(100);
    QVERIFY(m_backend->sent.isEmpty());
    QCOMPARE(m_hub->store()->count(), 0);
    // Nothing starts any more.
    sendOwn(QStringLiteral("[URL]https://youtu.be/aaaaaaaaaaa[/URL]"));
    QCOMPARE(m_fetch->loads.size(), 1);
    m_hub.reset(); // and it goes cleanly
}

void TestLinkPreviewSnd::shutdownWhileMakingJoinsTheWorker()
{
    std::atomic<bool> inProbe{false};
    std::atomic<bool> probeDone{false};
    m_hub->pipeline()->setVideoProbe([&](const QString&) {
        inProbe = true;
        QThread::msleep(400);
        probeDone = true;
        LocalMediaInfo info;
        info.preview = picture(64, 36);
        return info;
    });
    sendOwn(QStringLiteral("[URL]https://cdn.example.com/a.webm[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerFile(QByteArray("\x1a\x45\xdf\xa3", 4) + QByteArray(1000, 'w'), QStringLiteral("a.webm"));
    QTRY_VERIFY_WITH_TIMEOUT(inProbe.load(), 3000);
    m_hub.reset(); // joins the worker before anything it uses goes
    QVERIFY(probeDone.load());
    QTest::qWait(50);
    QVERIFY(fakets3::uploads().isEmpty());
    QVERIFY(m_backend->sent.isEmpty());
}

// ---- review (protocol) --------------------------------------------------------------------------------

// A sender's video went up with its poster; videos aren't downloaded automatically (videoAutoDownloadMB 0), so
// the receiver loads only the poster. When it is gone from the server (an admin cleaned the folder; or it never
// matched its hash), nothing of the sender's preview can be shown: the slot goes back to the click card.
void TestLinkPreviewSnd::serverPosterGoneFallsBackToTheClickCard()
{
    const Announcement a = sample(Kind::Video, true);
    requestFiles(m_core.get(), a);
    const MediaLink main = linkFor(a);
    QTRY_VERIFY_WITH_TIMEOUT(!fakets3::downloads().isEmpty(), 3000);
    QCOMPARE(fakets3::downloads().size(), 1); // the poster only
    const fakets3::Transfer poster = fakets3::downloads().first();
    QCOMPARE(poster.remotePath, a.previewPath);
    m_core->onServerError(fakets3::kConnection, ERROR_file_not_found, poster.returnCode, QStringLiteral("file not found"), false);
    const MediaEntry* e = m_core->entry(main.key());
    QVERIFY(e);
    QCOMPARE(e->state, MediaState::Idle); // the video itself waits for a click
    QCOMPARE(e->previewState, MediaState::Failed);

    // What the chat reads of it (ChatLinkPreviews::serverOf).
    linkstate::ServerState s;
    s.announced = true;
    s.hasMedia  = true;
    s.mainKey   = main.key();
    linkstate::readEntry(&s, e);
    QVERIFY(linkstate::serverFailed(s));
    const linkstate::Decision d = linkstate::decide(Kind::Video, false, linkstate::LocalState(), s);
    QVERIFY(!d.media);
    QVERIFY(!d.fromServer);
    QCOMPARE(d.look, linkcard::Look::Click);
    // A poster that loads, or the video being downloaded, keeps the sender's preview.
    for (const MediaState state : {MediaState::Queued, MediaState::Downloading, MediaState::Ready}) {
        linkstate::ServerState t = s;
        t.state                  = state;
        QVERIFY(!linkstate::serverFailed(t));
        QVERIFY(linkstate::decide(Kind::Video, false, linkstate::LocalState(), t).media);
    }
    linkstate::ServerState fine = s;
    fine.previewFailed          = false;
    QVERIFY(linkstate::decide(Kind::Video, false, linkstate::LocalState(), fine).fromServer);

    // Merged with the ux review: only a poster that failed for good (gone, not ours to download, not the announced
    // one) gives the slot back. A failure that may pass (the connection, a quota, anything else) keeps the
    // sender's preview while Core loads the poster again; the slot never invites a website request meanwhile.
    QCOMPARE(e->previewError, MediaError::NotFound);
    for (const MediaError forGood : {MediaError::NotFound, MediaError::Permission, MediaError::Password, MediaError::Mismatch}) {
        MediaEntry gone   = *e;
        gone.previewError = forGood;
        linkstate::ServerState t;
        t.announced = true;
        t.hasMedia  = true;
        t.mainKey   = main.key();
        linkstate::readEntry(&t, &gone);
        QVERIFY(t.previewFailed);
        QVERIFY(linkstate::serverFailed(t));
    }
    for (const MediaError passing : {MediaError::NotConnected, MediaError::Quota, MediaError::Other}) {
        MediaEntry hiccup   = *e;
        hiccup.previewError = passing;
        linkstate::ServerState t;
        t.announced = true;
        t.hasMedia  = true;
        t.mainKey   = main.key();
        linkstate::readEntry(&t, &hiccup);
        QVERIFY(!t.previewFailed);
        QVERIFY(!linkstate::serverFailed(t));
        QVERIFY(linkstate::decide(Kind::Video, false, linkstate::LocalState(), t).fromServer);
    }
}

// A poster download cut by a lost connection is a hiccup (Core loads it again after the reconnect): the entry says
// so, and the chat keeps the sender's preview.
void TestLinkPreviewSnd::serverPosterInterruptedKeepsTheSendersPreview()
{
    const Announcement a = sample(Kind::Video, true);
    requestFiles(m_core.get(), a);
    const MediaLink main = linkFor(a);
    QTRY_VERIFY_WITH_TIMEOUT(!fakets3::downloads().isEmpty(), 3000);
    const fakets3::Transfer poster = fakets3::downloads().first();
    QCOMPARE(poster.remotePath, a.previewPath);
    m_core->onServerError(fakets3::kConnection, ERROR_connection_lost, poster.returnCode, QStringLiteral("connection lost"), false);
    const MediaEntry* e = m_core->entry(main.key());
    QVERIFY(e);
    QCOMPARE(e->state, MediaState::Idle);
    QCOMPARE(e->previewState, MediaState::Failed);
    QCOMPARE(e->previewError, MediaError::NotConnected);
    linkstate::ServerState s;
    s.announced = true;
    s.hasMedia  = true;
    s.mainKey   = main.key();
    linkstate::readEntry(&s, e);
    QVERIFY(!linkstate::serverFailed(s));
    QVERIFY(linkstate::decide(Kind::Video, false, linkstate::LocalState(), s).fromServer);
}

// "Show images in chat" off: no media is downloaded automatically, link previews' files neither (the chat shows
// none then). The announcement is still kept: the chat asks for its files once pictures are shown again.
void TestLinkPreviewSnd::inlinePreviewsOffDownloadsNothing()
{
    Settings::instance().inlinePreviews = false;
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("look https://youtu.be/dQw4w9WgXcQ"));
    indexMessage(hash, QDateTime::currentSecsSinceEpoch() - 2, kAliceUid);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash, proto::Scope::Channel, Kind::YouTube));
    QCOMPARE(m_hub->counters().accepted, 1);
    QCOMPARE(m_hub->store()->count(), 1);
    requestFiles(m_core.get(), sample(Kind::Image, false)); // a preview-only picture, as the chat would ask
    QTest::qWait(100);
    QVERIFY(fakets3::downloads().isEmpty());
    // On again: the chat's next look at the card asks, and the preview comes from the TeamSpeak server.
    Settings::instance().inlinePreviews = true;
    requestFiles(m_core.get(), sample(Kind::YouTube, false));
    QTRY_COMPARE_WITH_TIMEOUT(fakets3::downloads().size(), 1, 3000);
    QCOMPARE(fakets3::downloads().first().remotePath, sample(Kind::YouTube, false).previewPath);
}

// A website's file a viewer loads is a made-up link of a reserved server (linkstate::webLink, "tsmedia-web").
// Anyone can post a ts3file link naming that server with the same folder, name and size: Core must never take
// it, or it holds the key first and the viewer's click fails ("a real link's key is never taken over").
void TestLinkPreviewSnd::forgedWebServerLinkIgnored()
{
    const Target  t    = classify(QStringLiteral("https://example.com/cat.png"));
    const QString dir  = m_core->cacheDir() + QStringLiteral("/links");
    const QString file = dir + QStringLiteral("/cat.png");
    QVERIFY(savePicture(file, picture(64, 48), "PNG"));
    const MediaLink web = linkstate::webLink(t, QStringLiteral("image/png"), QFileInfo(file).size(), 64, 48, 0);
    // Bob's message with that link (TeamSpeak's BBCode of a file link), seen before the viewer clicks.
    const QString forged = QStringLiteral("[URL=") + web.toUrl() + QStringLiteral("]cat.png[/URL]");
    const QList<MediaLink> found = MediaLink::findInMessage(forged);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().key(), web.key()); // the very key of the viewer's file
    m_core->onTextMessage(fakets3::kConnection, forged, kBobUid);
    m_core->ensure(found.first()); // the chat's scan
    QVERIFY(!m_core->entry(web.key()));
    QTest::qWait(50);
    QVERIFY(fakets3::downloads().isEmpty());
    // The viewer's click still shows the website's file, and the forged link never becomes a download.
    const QString key = m_core->adoptLocalFile(web, file);
    QCOMPARE(key, web.key());
    QVERIFY(m_core->isLocalFile(key));
    m_core->onTextMessage(fakets3::kConnection, forged, kBobUid);
    QCOMPARE(m_core->entry(key)->localPath, file);
    QVERIFY(fakets3::downloads().isEmpty());
}

// A channel message's preview goes up only into that channel, where its LP goes. Moved to another channel while
// the website answered: Core would upload into the channel you are in now (its file browser gets a link's files
// from a chat it never saw), and the LP isn't sent anyway. Nothing goes up.
void TestLinkPreviewSnd::movedAwayUploadsNothing()
{
    sendOwn(QStringLiteral("[URL]https://example.com/photo.jpg[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    fakets3::setOwnChannel(kOtherChannel);
    QByteArray jpeg;
    {
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(picture(800, 600).save(&buffer, "JPG"));
    }
    answerFile(jpeg, QStringLiteral("photo.jpg"));
    QVERIFY(serveUntil([this] { return m_hub->pipeline()->jobCount() == 0; }));
    for (const int id : m_core->uploadIds())
        QVERIFY2(m_core->upload(id)->channelId != kOtherChannel, "a link preview's file went into another channel");
    QVERIFY(m_core->uploadIds().isEmpty());
    QVERIFY(fakets3::directories().isEmpty());
    QVERIFY(m_backend->sent.isEmpty());
    QCOMPARE(m_hub->pipeline()->counters().failed, 1);
    // What was made for it is gone with the job (not left for the next session's clearing).
    QTRY_VERIFY_WITH_TIMEOUT(QDir(ts3::dataDir() + QStringLiteral("/linkpreview/send")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(), 3000);
    // Back in the channel: the next message's link goes up there as usual.
    fakets3::setOwnChannel(fakets3::kChannel);
    sendOwn(QStringLiteral("again [URL]https://example.com/photo.jpg[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    answerFile(jpeg, QStringLiteral("photo.jpg"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
}

// The LP's age is written when the sender makes it; the transport may hold a background command a while (behind
// presence and reactions, or a flood pause of several seconds). It arrives "younger" than its message by that
// delay: still the sender's own message with that very text, so it is matched within a wider window.
void TestLinkPreviewSnd::announcementDelayedInTheQueueStillMatches()
{
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("look https://example.com/cat.png"));
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    indexMessage(hash, now - 22, kAliceUid); // received 22 s ago; the LP says 2 s (it waited 20 s to go out)
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, hash));
    QCOMPARE(m_hub->counters().accepted, 1);
    QVERIFY(m_hub->store()->find(textreact::localKey(hash, now - 22), sample().urlKey));
    // Two of her messages with that text: the nearest one takes it.
    const QString again = textreact::contentHash(kAliceUid, QStringLiteral("again https://example.com/cat.png"));
    textreact::IndexedChat c;
    c.serverUid = QString::fromLatin1(fakets3::kServerUid);
    c.kind      = textreact::ChatKind::Channel;
    c.messages.append({again, now - 40, kAliceUid});
    c.messages.append({again, now - 5, kAliceUid});
    m_index.setChat(7, c);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, again));
    QCOMPARE(m_hub->counters().accepted, 2);
    QVERIFY(m_hub->store()->find(textreact::localKey(again, now - 5), sample().urlKey));
    QVERIFY(!m_hub->store()->find(textreact::localKey(again, now - 40), sample().urlKey));
    // Never someone else's message, however near.
    const QString bobs = textreact::contentHash(kBobUid, QStringLiteral("bob https://example.com/cat.png"));
    indexMessage(bobs, now - 20, kBobUid, textreact::ChatKind::Channel, QString(), 8);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, bobs));
    QCOMPARE(m_hub->counters().accepted, 2);
}

// The sender names the kind by the bytes it got (previewmaker): a ".jpg" that is an animated GIF goes up as a GIF
// (k=g), a ".gif" that is a still picture as a picture (k=i). The chat classifies the link by its address: the
// pictures are one family there (decide() shows both as the media), so the sender's preview is still shown.
void TestLinkPreviewSnd::pictureKindFromItsBytesStillShows()
{
    const QString url = QStringLiteral("https://example.com/funny.jpg");
    sendOwn(QStringLiteral("[URL]") + url + QStringLiteral("[/URL]"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    if (!QImageReader::supportedImageFormats().contains("gif"))
        QSKIP("no GIF reader in this Qt");
    // A 1x1 GIF89a (Qt has no GIF writer).
    const QByteArray gif("GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00"
                         "\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b",
                         43);
    answerFile(gif, QStringLiteral("funny.jpg"));
    QVERIFY(serveUntil([this] { return !m_backend->sent.isEmpty(); }));
    const std::optional<Incoming> in = lastSent();
    QVERIFY(in);
    const Target link = classify(QUrl(url));
    QCOMPARE(link.kind, Kind::Image);
    QCOMPARE(in->announcement.kind, Kind::Gif); // what the bytes are
    QCOMPARE(in->announcement.urlKey, link.key);
    // What the chat accepts for that link (ChatLinkPreviews::serverOf), and what it shows.
    QVERIFY(linkstate::kindFits(in->announcement.kind, link.kind));
    QVERIFY(linkstate::kindFits(Kind::Image, Kind::Gif));
    linkstate::ServerState s;
    s.announced = true;
    s.hasMedia  = true;
    s.mainKey   = linkFor(in->announcement).key();
    QVERIFY(linkstate::decide(link.kind, false, linkstate::LocalState(), s).media);
    // Never across families: a video, a YouTube card and a picture stay apart.
    for (const Kind a : {Kind::YouTube, Kind::Video}) {
        for (const Kind b : {Kind::Image, Kind::Gif}) {
            QVERIFY(!linkstate::kindFits(a, b));
            QVERIFY(!linkstate::kindFits(b, a));
        }
    }
    QVERIFY(!linkstate::kindFits(Kind::YouTube, Kind::Video));
    QVERIFY(!linkstate::kindFits(Kind::None, Kind::None));
    for (const Kind k : {Kind::YouTube, Kind::Image, Kind::Gif, Kind::Video})
        QVERIFY(linkstate::kindFits(k, k));
}

// Live pass C: TeamSpeak crashed in QMetaType::create after Options > Addons > Reload. qRegisterMetaType puts this
// DLL's copy and delete functions in Qt's process-wide registry, which outlives an unload; a type of the same name
// registered again finds the old entry, so a reload at another address calls freed code on the first queued
// linkpreview::Result. The plugin's sources never register a metatype, and the link preview signals that carry
// our structs are never connected queued (they are all emitted on the GUI thread).
void TestLinkPreviewSnd::pluginRegistersNoMetaTypes()
{
    static const QRegularExpression registering(QStringLiteral("\\bqRegisterMetaType(StreamOperators)?\\s*[<(]"));
    static const QRegularExpression queuedStruct(
        QStringLiteral("connect\\s*\\([^;]*&(linkpreview::)?(Fetcher::finished|PreviewStore::announced)\\b[^;]*Qt::(Queued|BlockingQueued|Auto)Connection"));
    QDirIterator it(QStringLiteral(TSMEDIA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
    int          files = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        ++files;
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        const QRegularExpressionMatch reg = registering.match(text);
        QVERIFY2(!reg.hasMatch(), qPrintable(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), reg.captured(0))));
        const QRegularExpressionMatch queued = queuedStruct.match(text);
        QVERIFY2(!queued.hasMatch(), qPrintable(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), queued.captured(0).left(160))));
    }
    QVERIFY(files > 50);
    // The chat still gets the fetcher's results a turn later, through a functor of its own.
    QFile chat(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/chatlinkpreviews.cpp"));
    QVERIFY(chat.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(chat.readAll());
    QVERIFY(text.contains(QLatin1String("&linkpreview::Fetcher::finished")));
    QVERIFY(text.contains(QLatin1String("onFetched(key, result); }, Qt::QueuedConnection)")));
}

// ---- 2.5: LPX and LPXNO -------------------------------------------------------------------------------------

void TestLinkPreviewSnd::lpxRoundTrip()
{
    for (const Kind kind : {Kind::Image, Kind::Gif, Kind::Video, Kind::Loop, Kind::Card}) {
        for (const bool picture : {true, false}) {
            if (!picture && kind != Kind::Card)
                continue;
            const Announcement   a = sampleX(kind, picture);
            QVERIFY(isValidX(a));
            QVERIFY(!isValid(a)); // never an LP
            const proto::Message m = makeLpx(a, proto::Scope::Channel, sampleRef(), kLpxShortCap);
            QCOMPARE(m.type, QByteArray("LPX"));
            QVERIFY(proto::serialize(m).size() <= kLpxShortCap);
            const std::optional<Incoming> in = roundTripX(m);
            QVERIFY2(in, QByteArray(1, kindCodeX(kind)).constData());
            const Announcement& b = in->announcement;
            QVERIFY(b.extended);
            QVERIFY(b.messageKey.isEmpty() && b.senderUid.isEmpty()); // never from the payload
            QCOMPARE(b.kind, a.kind);
            QCOMPARE(b.urlKey, a.urlKey);
            QCOMPARE(b.channelId, a.channelId);
            QCOMPARE(b.previewPath, a.previewPath);
            QCOMPARE(b.previewSha, a.previewSha);
            QCOMPARE(b.previewSize, a.previewSize);
            QCOMPARE(b.mediaPath, a.mediaPath);
            QCOMPARE(b.mediaSha, a.mediaSha);
            QCOMPARE(b.mediaSize, a.mediaSize);
            QCOMPARE(b.width, a.width);
            QCOMPARE(b.height, a.height);
            QCOMPARE(b.durationMs, a.durationMs);
            QCOMPARE(b.sensitive, a.sensitive);
            QCOMPARE(b.card.title, a.card.title); // the Persian joiner stays between its letters
            QCOMPARE(b.card.author, a.card.author);
            QCOMPARE(b.card.site, a.card.site);
            QCOMPARE(b.card.description, a.card.description);
            QCOMPARE(b.card.layout, a.card.layout);
            QCOMPARE(b.card.hasColor, a.card.hasColor);
            if (b.card.hasColor)
                QCOMPARE(b.card.color & 0xffffffu, a.card.color & 0xffffffu);
            QCOMPARE(b.card.play, a.card.play);
            if (kind == Kind::Card)
                QCOMPARE(b.card.pictureWidth, a.width);
            QVERIFY(isValidX(storedFrom(*in)));
        }
    }
    // Sensitive on media: sn=1, a spoiler cover on the server's link.
    Announcement gif = sampleX(Kind::Gif);
    gif.sensitive    = true;
    const std::optional<Incoming> in = roundTripX(makeLpx(gif, proto::Scope::Private, sampleRef(), kLpxShortCap));
    QVERIFY(in && in->announcement.sensitive && in->scope == proto::Scope::Private);
    QVERIFY(linkFor(storedFrom(*in)).spoiler);
    // A loop's media is a looping link for Core; its key is the file's like any other.
    const Announcement loop = storedFrom(*roundTripX(makeLpx(sampleX(Kind::Loop), proto::Scope::Channel, sampleRef(), kLpxShortCap)));
    QVERIFY(linkFor(loop).loop);
    Announcement asVideo = loop;
    asVideo.kind         = Kind::Video;
    QCOMPARE(linkFor(asVideo).key(), linkFor(loop).key());
}

void TestLinkPreviewSnd::lpxHostileInput_data()
{
    QTest::addColumn<QByteArray>("kind");
    QTest::addColumn<QByteArrayList>("drop");
    QTest::addColumn<QByteArrayList>("set"); // "key=value"
    QTest::addColumn<bool>("accepted");
    const auto row = [](const char* name, const char* kind, QByteArrayList drop, QByteArrayList set, bool ok) {
        QTest::newRow(name) << QByteArray(kind) << drop << set << ok;
    };
    const QByteArray title = b64(QStringLiteral("Title"));
    row("card as made", "c", {}, {}, true);
    row("media as made", "i", {}, {}, true);
    row("unknown k", "c", {}, {"k=z"}, false);
    row("k twice letters", "c", {}, {"k=cc"}, false);
    row("k=y (an LP kind)", "i", {}, {"k=y"}, false);
    row("no s", "c", {"s"}, {}, false);
    row("server chat s", "c", {}, {"s=s"}, false);
    row("bad m", "c", {}, {"m=xyz"}, false);
    row("bad u", "c", {}, {"u=XYZ"}, false);
    row("comma list in u", "c", {}, {"u=00112233445566778899aabbccddeeff,8eb4311921f7b1964785a11a4cc1f418"}, false);
    row("t on media", "i", {}, {"t=" + title}, false);
    row("a on media", "g", {}, {"a=" + title}, false);
    row("n on media", "v", {}, {"n=" + title}, false);
    row("x on media", "a", {}, {"x=" + title}, false);
    row("cl on media", "i", {}, {"cl=2e7d32"}, false);
    row("ly on media", "i", {}, {"ly=l"}, false);
    row("pg on media", "i", {}, {"pg=1"}, false);
    row("p without h", "c", {"h"}, {}, false);
    row("p without ps", "c", {"ps"}, {}, false);
    row("p without c", "c", {"c"}, {}, false);
    row("media without preview", "i", {"c", "p", "h", "ps"}, {}, false);
    row("f on a card", "c", {}, {"f=" + b64(QStringLiteral("/tsmedia/x.mp4")), "fh=" + shaOf("m").toLatin1(), "fs=10"}, false);
    row("f without fh", "i", {"fh"}, {}, false);
    row("f without fs", "i", {"fs"}, {}, false);
    row("gif as png path", "g", {}, {"f=" + b64(QStringLiteral("/tsmedia/link.png"))}, false);
    row("gif as webp path", "g", {}, {"f=" + b64(QStringLiteral("/tsmedia/link.webp"))}, true);
    row("loop as mov", "a", {}, {"f=" + b64(QStringLiteral("/tsmedia/link.mov"))}, false);
    row("video as mov", "v", {}, {"f=" + b64(QStringLiteral("/tsmedia/link.mov"))}, true);
    row("image as gif", "i", {}, {"f=" + b64(QStringLiteral("/tsmedia/link.gif"))}, false);
    row("preview svg", "c", {}, {"p=" + b64(QStringLiteral("/tsmedia/previews/x.svg"))}, false);
    row("preview dotdot", "c", {}, {"p=" + b64(QStringLiteral("/tsmedia/../x.jpg"))}, false);
    row("preview relative", "c", {}, {"p=" + b64(QStringLiteral("tsmedia/x.jpg"))}, false);
    row("preview backslash", "c", {}, {"p=" + b64(QStringLiteral("/tsmedia\\x.jpg"))}, false);
    row("ps 0", "c", {}, {"ps=0"}, false);
    row("ps over 2 MiB", "c", {}, {"ps=2097153"}, false);
    row("fs over 100 MiB", "v", {}, {"fs=104857601"}, false);
    row("sha short", "c", {}, {"h=abc"}, false);
    row("cl red", "c", {}, {"cl=red"}, false);
    row("cl 5 digits", "c", {}, {"cl=12345"}, false);
    row("cl not hex", "c", {}, {"cl=12345g"}, false);
    row("cl white ignored", "c", {}, {"cl=ffffff"}, true);
    row("cl brand ignored", "c", {}, {"cl=5865f2"}, true);
    row("t over 256 bytes", "c", {}, {"t=" + toBase64Url(QByteArray(257, 'a')).toLatin1()}, false);
    row("x over 384 bytes", "c", {}, {"x=" + toBase64Url(QByteArray(385, 'a')).toLatin1()}, false);
    row("n over 64 bytes", "c", {}, {"n=" + toBase64Url(QByteArray(65, 'a')).toLatin1()}, false);
    row("a over 96 bytes", "c", {}, {"a=" + toBase64Url(QByteArray(97, 'a')).toLatin1()}, false);
    row("invalid utf8", "c", {}, {"t=" + toBase64Url(QByteArray("\xff\xfe" "abc", 5)).toLatin1()}, false);
    row("t only invisible", "c", {}, {"t=" + b64(QString(QChar(0x200b)) + QChar(0x202e) + QChar(0x2066))}, false);
    row("no t", "c", {"t"}, {}, false);
    row("t not base64", "c", {}, {"t=a.b"}, false);
    row("d on image", "i", {}, {"d=1000"}, false);
    row("d on card", "c", {}, {"d=1000"}, true);
    row("d over a day", "v", {}, {"d=86400001"}, false);
    row("w without ht", "c", {"ht"}, {}, false);
    row("w 0", "c", {}, {"w=0"}, false);
    row("w over 16384", "c", {}, {"w=16385"}, false);
    row("w on text-only card", "c", {"c", "p", "h", "ps", "ly"}, {}, false);
    row("ly without p", "c", {"c", "p", "h", "ps", "w", "ht"}, {"ly=t"}, false);
    row("ly other", "c", {}, {"ly=x"}, false);
    row("pg 2", "c", {}, {"pg=2"}, false);
    row("sn 0", "c", {}, {"sn=0"}, false);
    row("sn on media", "g", {}, {"sn=1"}, true);
    row("card without x and p", "c", {"c", "p", "h", "ps", "w", "ht", "ly", "x"}, {}, false);
    row("card without p", "c", {"c", "p", "h", "ps", "w", "ht", "ly"}, {}, true);
    row("card without x", "c", {"x"}, {}, true);
    row("x only invisible, no p", "c", {"c", "p", "h", "ps", "w", "ht", "ly"}, {"x=" + b64(QStringLiteral("​ ⁠"))}, false);
    row("leading zero channel", "c", {}, {"c=07"}, false);
}

void TestLinkPreviewSnd::lpxHostileInput()
{
    QFETCH(QByteArray, kind);
    QFETCH(QByteArrayList, drop);
    QFETCH(QByteArrayList, set);
    QFETCH(bool, accepted);
    const Kind     k = kindOfX(kind.at(0));
    proto::Message m = makeLpx(sampleX(k), proto::Scope::Channel, sampleRef(), proto::kMaxLpxSendBytes);
    QVERIFY(!m.type.isEmpty());
    for (const QByteArray& key : drop)
        m = without(m, key.constData());
    for (const QByteArray& kv : set) {
        const int eq = kv.indexOf('=');
        m            = with(m, kv.left(eq).constData(), kv.mid(eq + 1));
    }
    const std::optional<Incoming> in = roundTripX(m);
    QCOMPARE(bool(in), accepted);
    if (in) {
        QVERIFY(isValidX(storedFrom(*in)));
        QVERIFY(!in->announcement.card.hasColor || pagemeta::isAcceptableThemeColour(in->announcement.card.color));
    }
}

void TestLinkPreviewSnd::lpxUnknownFieldsIgnored()
{
    proto::Message m = makeLpx(sampleX(Kind::Card), proto::Scope::Channel, sampleRef(), kLpxShortCap);
    m.set("zz", "whatever");
    m.set("vv", "2");
    const std::optional<Incoming> in = roundTripX(m);
    QVERIFY(in);
    QCOMPARE(in->announcement.card.title, sampleX(Kind::Card).card.title);
    // An LP reader never takes it, an LPX reader never takes an LP.
    QVERIFY(!readMessage(*proto::parse(proto::serialize(m))));
    QVERIFY(!readLpx(*proto::parse(proto::serialize(makeMessage(sample(Kind::Image), proto::Scope::Channel, sampleRef())))));
}

void TestLinkPreviewSnd::lpxShortenToFit()
{
    // A web card at the most: a 48-byte site name, a 256-byte title, a 384-byte description, a 96-byte author.
    Announcement a      = sampleX(Kind::Card);
    a.card.site         = QString(48, QLatin1Char('s'));
    a.card.title        = QString(256, QLatin1Char('t'));
    a.card.description  = QString::fromUtf8("\xd9\x85\xdb\x8c").repeated(96); // 384 bytes of Persian
    a.card.author       = QString(96, QLatin1Char('a'));
    const proto::Message wide = makeLpx(a, proto::Scope::Channel, sampleRef(), proto::kMaxLpxSendBytes);
    QVERIFY(!wide.type.isEmpty());
    QVERIFY(proto::serialize(wide).size() <= proto::kMaxLpxSendBytes);
    const std::optional<Incoming> all = roundTripX(wide);
    QVERIFY(all);
    QCOMPARE(all->announcement.card.description.toUtf8().size(), 384); // nothing had to go at 1400
    QCOMPARE(all->announcement.card.author, a.card.author);

    // At 900: the description goes first (whole characters), the author and site stay while they can.
    const proto::Message narrow = makeLpx(a, proto::Scope::Channel, sampleRef(), kLpxShortCap);
    QVERIFY(!narrow.type.isEmpty());
    QVERIFY(proto::serialize(narrow).size() <= kLpxShortCap);
    const std::optional<Incoming> cut = roundTripX(narrow);
    QVERIFY(cut);
    const CardData& c = cut->announcement.card;
    QVERIFY(c.description.toUtf8().size() < 384);
    QVERIFY(a.card.description.startsWith(c.description));
    QCOMPARE(c.title, a.card.title);
    QVERIFY(!cut->announcement.previewPath.isEmpty()); // the picture always stays

    // A long upload folder: the title is shortened too, then the author and the site name go.
    Announcement deep = a;
    deep.previewPath  = QStringLiteral("/") + QString(200, QLatin1Char('f')) + QStringLiteral("/linkpreview_3f9a1c2e.jpg");
    const std::optional<Incoming> deeper = roundTripX(makeLpx(deep, proto::Scope::Channel, sampleRef(), kLpxShortCap));
    QVERIFY(deeper);
    QVERIFY(deeper->announcement.card.description.toUtf8().size() <= 64 || deeper->announcement.card.description.isEmpty());
    QVERIFY(!deeper->announcement.card.title.isEmpty());

    // A text-only card that can't fit even without the rest: no LPX (the job then sends LPXNO).
    Announcement text = sampleX(Kind::Card, false);
    text.card.title   = QString(256, QLatin1Char('t'));
    QVERIFY(makeLpx(text, proto::Scope::Channel, sampleRef(), 200).type.isEmpty());
    QVERIFY(!makeLpx(text, proto::Scope::Channel, sampleRef(), kLpxShortCap).type.isEmpty());
}

void TestLinkPreviewSnd::lpxStoreFormat()
{
    PreviewStore store;
    const qint64 now = 1760000100;
    Announcement card = sampleX(Kind::Card);
    Announcement text = sampleX(Kind::Card, false);
    text.messageKey   = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000001);
    Announcement loop = sampleX(Kind::Loop);
    loop.messageKey   = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000002);
    loop.sensitive    = true;
    Announcement lp   = sample(Kind::Image);
    lp.messageKey     = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000003);
    for (const Announcement& a : {card, text, loop, lp})
        QCOMPARE(store.add(a, now), PreviewStore::Add::Added);
    QCOMPARE(store.add(card, now), PreviewStore::Add::Unchanged);
    Announcement changed = card;
    changed.card.title   = QStringLiteral("Another title");
    QCOMPARE(store.check(changed), PreviewStore::Add::Replaced); // the card's text counts as its files
    const QByteArray json = store.toJson(now);
    QVERIFY(json.contains("\"k\":\"xc\""));
    QVERIFY(json.contains("\"k\":\"xa\""));
    QVERIFY(json.contains("\"title2\""));
    PreviewStore again;
    QVERIFY(again.fromJson(json, now));
    QCOMPARE(again.count(), 4);
    const Announcement* back = again.find(card.messageKey, card.urlKey);
    QVERIFY(back && back->extended && back->kind == Kind::Card);
    QCOMPARE(back->card.title, card.card.title);
    QCOMPARE(back->card.description, card.card.description);
    QCOMPARE(back->card.layout, card.card.layout);
    QCOMPARE(back->card.hasColor, true);
    QCOMPARE(back->card.play, true);
    QCOMPARE(back->card.pictureWidth, card.width);
    const Announcement* noPicture = again.find(text.messageKey, text.urlKey);
    QVERIFY(noPicture && noPicture->previewPath.isEmpty() && noPicture->channelId == 0);
    const Announcement* looped = again.find(loop.messageKey, loop.urlKey);
    QVERIFY(looped && looped->kind == Kind::Loop && looped->sensitive);
    const Announcement* old = again.find(lp.messageKey, lp.urlKey);
    QVERIFY(old && !old->extended && old->kind == Kind::Image);
    // A hostile entry: a stored text that isn't clean, a colour that a card may not wear.
    QByteArray bad = json;
    bad.replace("Example News", "Example \xe2\x80\xae News");
    PreviewStore hostile;
    QVERIFY(hostile.fromJson(bad, now));
    QCOMPARE(hostile.count(), 2); // both cards had that site name: skipped one by one
}

void TestLinkPreviewSnd::lpxnoReasons()
{
    const QString k1 = QStringLiteral("00112233445566778899aabbccddeeff"), k2 = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418"),
                  k3 = QStringLiteral("2019fab9d770c5f007bced59a5abfa1e");
    const std::optional<SkipX> plain = roundTripSkipX(makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1}));
    QVERIFY(plain && plain->urlKeys == QStringList{k1} && plain->reason == SkipReason::Unknown);
    const std::optional<SkipX> nothing = roundTripSkipX(makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1}, SkipReason::Nothing));
    QVERIFY(nothing && nothing->reason == SkipReason::Nothing);
    const std::optional<SkipX> cross = roundTripSkipX(makeLpxSkip(proto::Scope::Private, sampleRef(), {k2}, SkipReason::CrossSite));
    QVERIFY(cross && cross->reason == SkipReason::CrossSite && cross->scope == proto::Scope::Private);
    const std::optional<SkipX> intake = roundTripSkipX(makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1, k2, k3}));
    QVERIFY(intake && intake->urlKeys.size() == 3 && intake->reason == SkipReason::Unknown);
    // A reason only with one key; an unknown reason reads as none (a newer sender's: receivers may load it).
    QVERIFY(makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1, k2}, SkipReason::Nothing).type.isEmpty());
    proto::Message newer = makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1});
    newer.set("r", "q");
    QVERIFY(roundTripSkipX(newer) && roundTripSkipX(newer)->reason == SkipReason::Unknown);
    proto::Message many = makeLpxSkip(proto::Scope::Channel, sampleRef(), {k1, k2});
    many.set("r", "n");
    QVERIFY(roundTripSkipX(many) && roundTripSkipX(many)->reason == SkipReason::Unknown);
    // Always short.
    QVERIFY(proto::serialize(makeLpxSkip(proto::Scope::Channel, sampleRef(), {QString(64, QLatin1Char('a')), QString(64, QLatin1Char('b')), QString(64, QLatin1Char('c'))}))
                .size()
            <= proto::kMaxSendBytes);
}

void TestLinkPreviewSnd::lpxnoHostileInput_data()
{
    QTest::addColumn<QByteArray>("u");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("one") << QByteArray("00112233445566778899aabbccddeeff") << true;
    QTest::newRow("three") << QByteArray("0011223344556677,8899aabbccddeeff,0123456789abcdef") << true;
    QTest::newRow("four") << QByteArray("0011223344556677,8899aabbccddeeff,0123456789abcdef,fedcba9876543210") << false;
    QTest::newRow("five") << QByteArray("00112233,44556677,8899aabb,ccddeeff,01234567") << false;
    QTest::newRow("duplicate") << QByteArray("0011223344556677,0011223344556677") << false;
    QTest::newRow("empty part") << QByteArray("0011223344556677,") << false;
    QTest::newRow("upper case") << QByteArray("00112233445566AA") << false;
    QTest::newRow("short") << QByteArray("0011223") << false;
    QTest::newRow("empty") << QByteArray() << false;
}

void TestLinkPreviewSnd::lpxnoHostileInput()
{
    QFETCH(QByteArray, u);
    QFETCH(bool, accepted);
    proto::Message m = makeLpxSkip(proto::Scope::Channel, sampleRef(), {QStringLiteral("00112233445566778899aabbccddeeff")});
    m.set("u", u);
    QCOMPARE(bool(roundTripSkipX(m)), accepted);
}

void TestLinkPreviewSnd::maxSendBytesPerType()
{
    QCOMPARE(proto::maxSendBytes("LPX"), 1400);
    for (const char* type : {"LP", "LPNO", "LPXNO", "HELLO", "HI", "R", "SYNC", "TR", "POLL"})
        QCOMPARE(proto::maxSendBytes(type), proto::kMaxSendBytes);
    proto::Message m;
    m.type = "LPX";
    m.set("x", QByteArray(512, 'a'));
    m.set("y", QByteArray(512, 'b'));
    QVERIFY(!proto::serialize(m).isEmpty()); // over 900, within 1400
    m.type = "LP";
    QVERIFY(proto::serialize(m).isEmpty());
    m.type = "LPX";
    m.set("z", QByteArray(400, 'c'));
    QVERIFY(proto::serialize(m).isEmpty()); // over 1400
}

void TestLinkPreviewSnd::lpxCapPerConnection()
{
    QCOMPARE(lpxSendCap(true, true), 1400);
    QCOMPARE(lpxSendCap(true, false), 900);
    QCOMPARE(lpxSendCap(false, true), 900);
    QCOMPARE(lpxSendCap(true), kLpxLongCommandsChecked ? 1400 : 900); // 2.5.0: 900 until live check L1
    // The fake server is TeamSpeak's own (3.13): still 900 while the constant says unchecked.
    QVERIFY(m_core->serverIsTeamSpeakOwn(fakets3::kConnection));
    QCOMPARE(lpxSendCap(m_core->serverIsTeamSpeakOwn(fakets3::kConnection)), 900);
    // TeaSpeak, and a connection nobody knows: 900.
    fakets3::setServerVersion(QStringLiteral("TeaSpeak 1.4.22"), QStringLiteral("Linux"));
    QVERIFY(!m_core->serverIsTeamSpeakOwn(fakets3::kConnection));
    QVERIFY(!m_core->serverIsTeamSpeakOwn(fakets3::kConnection + 77));
    QCOMPARE(lpxSendCap(m_core->serverIsTeamSpeakOwn(fakets3::kConnection), true), 900);
    // A shortened LPX still parses and keeps its picture.
    Announcement a     = sampleX(Kind::Card);
    a.card.description = QString(384, QLatin1Char('d'));
    const std::optional<Incoming> in = roundTripX(makeLpx(a, proto::Scope::Channel, sampleRef(), lpxSendCap(false)));
    QVERIFY(in && !in->announcement.previewPath.isEmpty());
}

void TestLinkPreviewSnd::lpxSentThroughPluginLink()
{
    // A 1300-byte LPX through the real transport: sent byte for byte, one command's flood cost.
    Announcement a     = sampleX(Kind::Card);
    a.card.title       = QString(256, QLatin1Char('t'));
    a.card.description = QString(384, QLatin1Char('d'));
    a.card.author      = QString(96, QLatin1Char('a'));
    a.card.site        = QString(60, QLatin1Char('s'));
    a.previewPath      = QStringLiteral("/") + QString(60, QLatin1Char('f')) + QStringLiteral("/linkpreview_3f9a1c2e.jpg");
    const proto::Message m    = makeLpx(a, proto::Scope::Channel, sampleRef(), proto::kMaxLpxSendBytes);
    const QByteArray     wire = proto::serialize(m);
    QVERIFY2(wire.size() > 1200 && wire.size() <= 1400, QByteArray::number(wire.size()).constData());
    FloodGovernor&       governor = m_core->floodGovernor(fakets3::kConnection);
    const int            before   = m_backend->sent.size();
    const double         p0       = governor.pluginPoints(m_core->floodClockMs());
    m_link->sendWith(fakets3::kConnection, m, peers::Target::toChannel(), PluginLink::Priority::Background, nullptr);
    QCOMPARE(m_backend->sent.size(), before + 1);
    QCOMPARE(m_backend->sent.last().payload, wire);
    const double longCost = governor.pluginPoints(m_core->floodClockMs()) - p0;
    // A short one costs the same.
    const double p1 = governor.pluginPoints(m_core->floodClockMs());
    m_link->sendWith(fakets3::kConnection, makeLpxSkip(proto::Scope::Channel, sampleRef(), {a.urlKey}), peers::Target::toChannel(),
                     PluginLink::Priority::Background, nullptr);
    const double shortCost = governor.pluginPoints(m_core->floodClockMs()) - p1;
    QVERIFY2(qAbs(longCost - shortCost) < 0.05, qPrintable(QStringLiteral("%1 vs %2").arg(longCost).arg(shortCost)));
    // Over 1400 bytes: refused before the queue.
    proto::Message tooLong = m;
    tooLong.set("zz", QByteArray(qMin(proto::kMaxValueChars, 1401 - wire.size() + 5), 'z'));
    QVERIFY(proto::serialize(tooLong).isEmpty());
    bool failed = false;
    m_link->sendWith(fakets3::kConnection, tooLong, peers::Target::toChannel(), PluginLink::Priority::Background,
                     [&](peers::SendResult r) { failed = r == peers::SendResult::Failed; });
    QVERIFY(failed);
    QCOMPARE(m_backend->sent.size(), before + 2);
}

void TestLinkPreviewSnd::lpxSeededFuzz()
{
    QRandomGenerator rng(25);
    const QVector<QByteArray> seeds{
        proto::serialize(makeLpx(sampleX(Kind::Card), proto::Scope::Channel, sampleRef(), proto::kMaxLpxSendBytes)),
        proto::serialize(makeLpx(sampleX(Kind::Card, false), proto::Scope::Private, sampleRef(), kLpxShortCap)),
        proto::serialize(makeLpx(sampleX(Kind::Loop), proto::Scope::Channel, sampleRef(), kLpxShortCap)),
        proto::serialize(makeLpx(sampleX(Kind::Gif), proto::Scope::Channel, sampleRef(), kLpxShortCap)),
        proto::serialize(makeLpxSkip(proto::Scope::Channel, sampleRef(), {QStringLiteral("0011223344556677"), QStringLiteral("8899aabbccddeeff")})),
        proto::serialize(makeLpxSkip(proto::Scope::Channel, sampleRef(), {QStringLiteral("0011223344556677")}, SkipReason::CrossSite)),
    };
    for (const QByteArray& s : seeds)
        QVERIFY(!s.isEmpty());
    const QByteArray alphabet("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._,:-= ");
    int              accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        QByteArray wire = seeds.at(i % seeds.size());
        const int  ops  = 1 + rng.bounded(4);
        for (int op = 0; op < ops && !wire.isEmpty(); ++op) {
            const int at = rng.bounded(wire.size());
            switch (rng.bounded(5)) {
            case 0: // a byte changed
                wire[at] = alphabet.at(rng.bounded(alphabet.size()));
                break;
            case 1: // a byte gone
                wire.remove(at, 1);
                break;
            case 2: // a piece repeated
                wire.insert(at, wire.mid(rng.bounded(wire.size()), 1 + rng.bounded(24)));
                break;
            case 3: // cut
                wire.truncate(at + 1);
                break;
            default: // spliced with another seed
                wire = wire.left(at) + seeds.at(rng.bounded(seeds.size())).mid(at);
                break;
            }
        }
        const std::optional<proto::Message> parsed = proto::parse(wire);
        if (!parsed)
            continue;
        if (const std::optional<Incoming> in = readLpx(*parsed)) {
            ++accepted;
            const Announcement stored = storedFrom(*in);
            QVERIFY2(isValidX(stored), wire.constData());
            const CardData& c = stored.card;
            QVERIFY(c.title.toUtf8().size() <= kCardTitleBytes && c.description.toUtf8().size() <= kCardDescriptionBytes);
            QVERIFY(c.author.toUtf8().size() <= kCardAuthorBytes && c.site.toUtf8().size() <= kCardSiteBytes);
            QCOMPARE(textclean::cleanToBytes(c.title, kCardTitleBytes), c.title);
            QCOMPARE(textclean::cleanToBytes(c.description, kCardDescriptionBytes), c.description);
        }
        if (const std::optional<SkipX> skip = readLpxSkip(*parsed)) {
            QVERIFY(!skip->urlKeys.isEmpty() && skip->urlKeys.size() <= kMaxSkipKeys);
            QVERIFY(skip->reason == SkipReason::Unknown || skip->urlKeys.size() == 1);
        }
    }
    QVERIFY(accepted > 0); // some mutations keep it valid (an unknown field, a changed key)

    // The store's x entries, mutated the same way: never a crash, every entry it keeps is valid.
    PreviewStore store;
    for (const Kind kind : {Kind::Card, Kind::Loop, Kind::Image}) {
        Announcement a = sampleX(kind);
        a.messageKey   = textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000 + static_cast<int>(kind));
        QCOMPARE(store.add(a, 1760000100), PreviewStore::Add::Added);
    }
    const QByteArray json = store.toJson(1760000100);
    for (int i = 0; i < 20000; ++i) {
        QByteArray doc = json;
        const int  at  = rng.bounded(doc.size());
        switch (rng.bounded(3)) {
        case 0:
            doc[at] = alphabet.at(rng.bounded(alphabet.size()));
            break;
        case 1:
            doc.remove(at, 1 + rng.bounded(8));
            break;
        default:
            doc.insert(at, doc.mid(rng.bounded(doc.size()), 1 + rng.bounded(16)));
            break;
        }
        PreviewStore mutated;
        if (!mutated.fromJson(doc, 1760000100))
            continue;
        for (const Kind kind : {Kind::Card, Kind::Loop, Kind::Image}) {
            const Announcement* a = mutated.find(textreact::localKey(QStringLiteral("0123456789abcdef"), 1760000000 + static_cast<int>(kind)),
                                                 sampleX(kind).urlKey);
            if (a)
                QVERIFY(isValidAny(*a));
        }
    }
}

// ---- 2.5: receiving LPX / LPXNO ------------------------------------------------------------------------------

void TestLinkPreviewSnd::lpxOnlyFromTheSender()
{
    Hub::Limits limits;
    limits.waitTtlMs = 300;
    newHub(limits);
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("look https://www.example.com/news/a-story"));
    indexMessage(hash, now - 2, kAliceUid);
    const auto lpx = [](const QString& h, proto::Scope scope = proto::Scope::Channel, Kind kind = Kind::Card) {
        return makeLpx(sampleX(kind), scope, textreact::wireRef(h, 2), kLpxShortCap);
    };
    // Bob for Alice's message: waits for one of his, then gives up.
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), lpx(hash));
    QCOMPARE(m_hub->waitingCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->waitingCount(), 0, 3000);
    QCOMPARE(m_hub->store()->count(), 0);
    // ServerQuery; Carol from another channel with s=c.
    m_hub->received(fakets3::kConnection, kQuery, QStringLiteral("serveradmin"), QStringLiteral("serveradmin"), lpx(hash));
    m_hub->received(fakets3::kConnection, kCarol, kCarolUid, QStringLiteral("Carol"), lpx(textreact::contentHash(kCarolUid, QStringLiteral("x"))));
    QCOMPARE(m_hub->counters().rejected, 2);
    // Files in another channel (s=c names the channel holding them): refused.
    Announcement elsewhere = sampleX(Kind::Card);
    elsewhere.channelId    = kOtherChannel;
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), makeLpx(elsewhere, proto::Scope::Channel, textreact::wireRef(hash, 2), 900));
    QCOMPARE(m_hub->counters().rejected, 3);
    // Alice's own: stored as an LPX; its files are NOT asked for here (the chat asks when the card is on screen).
    QSignalSpy announced(m_hub->store(), &PreviewStore::announced);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), lpx(hash));
    QCOMPARE(m_hub->counters().lpxAccepted, 1);
    QCOMPARE(announced.count(), 1);
    const Announcement* a = m_hub->store()->find(textreact::localKey(hash, now - 2), sampleX().urlKey);
    QVERIFY(a && a->extended && a->kind == Kind::Card && a->senderUid == kAliceUid);
    QCOMPARE(a->card.title, sampleX().card.title);
    QTest::qWait(100);
    QVERIFY(fakets3::downloads().isEmpty());
    // A text-only card names no files: accepted from our channel.
    const QString hash2 = textreact::contentHash(kAliceUid, QStringLiteral("two https://www.example.org/b"));
    indexMessage(hash2, now - 1, kAliceUid, textreact::ChatKind::Channel, QString(), 2);
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"),
                    makeLpx(sampleX(Kind::Card, false), proto::Scope::Channel, textreact::wireRef(hash2, 1), kLpxShortCap));
    QCOMPARE(m_hub->counters().lpxAccepted, 2);
    // An LP for the same key isn't an LPX (and the other way round): a 2.4.0 announcement still works.
    QCOMPARE(m_hub->counters().accepted, 0);
}

void TestLinkPreviewSnd::lpxOwnBucket()
{
    Hub::Limits limits;
    limits.perSenderBurst  = 2;
    limits.xPerSenderBurst = 3;
    newHub(limits);
    for (int i = 0; i < 5; ++i)
        m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"),
                        makeLpx(sampleX(), proto::Scope::Channel, textreact::wireRef(textreact::contentHash(kAliceUid, QString::number(i)), 2), kLpxShortCap));
    QCOMPARE(m_hub->counters().rateLimited, 2);
    QCOMPARE(m_hub->counters().lpxReceived, 5);
    // LP's own allowance is untouched.
    for (int i = 0; i < 3; ++i)
        m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), announcementFrom(kAliceUid, textreact::contentHash(kAliceUid, QStringLiteral("lp%1").arg(i))));
    QCOMPARE(m_hub->counters().rateLimited, 3);
    QCOMPARE(m_hub->waitingCount(), 3 + 2);
    // The default: 20 a minute for LPX / LPXNO, LP's stays 10.
    QCOMPARE(Hub::Limits().xPerSenderBurst, 20);
    QCOMPARE(Hub::Limits().perSenderBurst, 10);
}

void TestLinkPreviewSnd::lpxnoReceivedWithReasons()
{
    const qint64  now  = QDateTime::currentSecsSinceEpoch();
    const QString hash = textreact::contentHash(kAliceUid, QStringLiteral("three links"));
    indexMessage(hash, now - 2, kAliceUid);
    const QString key = textreact::localKey(hash, now - 2);
    const QString k1 = QStringLiteral("00112233445566778899aabbccddeeff"), k2 = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418"),
                  k3 = QStringLiteral("2019fab9d770c5f007bced59a5abfa1e");
    QSignalSpy skipped(m_hub.get(), &Hub::skipped);
    // The intake notice: three keys, no reason.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), makeLpxSkip(proto::Scope::Channel, textreact::wireRef(hash, 2), {k1, k2, k3}));
    QCOMPARE(skipped.count(), 3);
    for (const QString& k : {k1, k2, k3}) {
        QVERIFY(m_hub->isSkipped(key, k));
        QCOMPARE(m_hub->skipReason(key, k), SkipReason::Unknown);
    }
    QCOMPARE(m_hub->counters().lpxSkipsAccepted, 3);
    // r=n and r=x for one link each; the newest notice of a link wins.
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), makeLpxSkip(proto::Scope::Channel, textreact::wireRef(hash, 2), {k1}, SkipReason::Nothing));
    m_hub->received(fakets3::kConnection, kAlice, kAliceUid, QStringLiteral("Alice"), makeLpxSkip(proto::Scope::Channel, textreact::wireRef(hash, 2), {k2}, SkipReason::CrossSite));
    QCOMPARE(m_hub->skipReason(key, k1), SkipReason::Nothing);
    QCOMPARE(m_hub->skipReason(key, k2), SkipReason::CrossSite);
    QCOMPARE(m_hub->skipReason(key, k3), SkipReason::Unknown);
    // Bob can't speak for Alice's message.
    m_hub->received(fakets3::kConnection, kBob, kBobUid, QStringLiteral("Bob"), makeLpxSkip(proto::Scope::Channel, textreact::wireRef(hash, 2), {k3}, SkipReason::Nothing));
    QCOMPARE(m_hub->skipReason(key, k3), SkipReason::Unknown);
    QCOMPARE(m_hub->store()->count(), 0);
}

// ---- 2.5: the sender's page links ----------------------------------------------------------------------------

void TestLinkPreviewSnd::senderCardWithPicture()
{
    const QString text = QStringLiteral("look [URL]https://www.example.com/news/a-story[/URL]");
    sendOwn(text);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const FakeFetch::Load load = m_fetch->loads.first();
    QCOMPARE(load.target.cls, LinkClass::Web);
    QCOMPARE(load.caps.page, qint64(512 * 1024));      // the sender ignores data saver
    QCOMPARE(load.caps.picture, qint64(2 * 1024 * 1024));
    QCOMPARE(load.caps.media, qint64(25 * 1024 * 1024)); // a page that turns out to be media
    QVERIFY(!load.caps.userAsked);
    CardData card;
    card.site        = QStringLiteral("Example News");
    card.title       = QStringLiteral("A story about something");
    card.description = QStringLiteral("What happened, in a sentence.");
    card.layout      = CardLayout::Large;
    card.hasColor    = true;
    card.color       = qRgb(0x2e, 0x7d, 0x32);
    answerCard(card, picture(1600, 900));
    QVERIFY(serveUntil([this] { return lastSentX().has_value(); }));
    const std::optional<Incoming> in = lastSentX();
    const Announcement&           a  = in->announcement;
    QCOMPARE(a.kind, Kind::Card);
    QCOMPARE(a.urlKey, load.target.key);
    QCOMPARE(a.card.title, card.title);
    QCOMPARE(a.card.site, card.site);
    QCOMPARE(a.card.description, card.description);
    QCOMPARE(a.card.layout, CardLayout::Large);
    QVERIFY(a.card.hasColor);
    QCOMPARE(a.width, 1600);
    QCOMPARE(a.height, 900);
    QVERIFY(a.mediaPath.isEmpty()); // a card never uploads more than its picture
    QVERIFY(a.previewPath.startsWith(QLatin1String("/tsmedia/previews/")));
    const QByteArray preview = fakets3::serverFile(a.previewPath);
    QCOMPARE(a.previewSha, shaOf(preview));
    QImage shown;
    QVERIFY(shown.loadFromData(preview));
    QCOMPARE(qMax(shown.width(), shown.height()), 800); // a large card's picture fits 800 x 800
    QVERIFY(preview.size() <= kMaxPreviewBytes);
    // One LPX within this connection's cap, never an LP; our own copy is stored.
    QCOMPARE(sentTypes(), QByteArrayList({"LPX"}));
    QVERIFY(m_backend->sent.last().payload.size() <= kLpxShortCap);
    QCOMPARE(m_hub->pipeline()->counters().lpxSent, 1);
    QCOMPARE(m_hub->pipeline()->counters().lpxCapUsed, kLpxShortCap);
    QCOMPARE(m_hub->store()->count(), 1);
    QVERIFY(fakets3::messages().isEmpty());
    QCOMPARE(chatLines(), 0);
}

void TestLinkPreviewSnd::senderTextOnlyCard()
{
    sendOwn(QStringLiteral("https://www.example.org/docs/a-page"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    CardData card;
    card.title       = QStringLiteral("A page");
    card.description = QStringLiteral("The page's own short text.");
    answerCard(card);
    QVERIFY(serveUntil([this] { return lastSentX().has_value(); }));
    const Announcement a = lastSentX()->announcement;
    QCOMPARE(a.kind, Kind::Card);
    QVERIFY(a.previewPath.isEmpty() && a.channelId == 0);
    QCOMPARE(a.card.description, card.description);
    QVERIFY(fakets3::uploads().isEmpty()); // nothing went up
    // A card without its text or its picture is no card: LPXNO (without a reason: receivers may try).
    sendOwn(QStringLiteral("https://www.example.org/docs/b-page"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    CardData bare;
    bare.title = QStringLiteral("Only a title");
    answerCard(bare);
    QTRY_COMPARE_WITH_TIMEOUT(sentTypes().size(), 2, 3000);
    QCOMPARE(sentTypes().last(), QByteArray("LPXNO"));
    QCOMPARE(readLpxSkip(sentMessages().last())->reason, SkipReason::Unknown);
}

void TestLinkPreviewSnd::senderLoopGifCap()
{
    m_hub->pipeline()->setVideoProbe(busyVideoProbe);
    sendOwn(QStringLiteral("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QCOMPARE(m_fetch->loads.first().target.cls, LinkClass::MediaPage);
    QCOMPARE(m_fetch->loads.first().caps.media, qint64(20 * 1024 * 1024));
    const auto answerLoop = [this](int bytes) {
        const FakeFetch::Load load = m_fetch->loads.last();
        const QString         path = load.cacheDir + QLatin1Char('/') + load.target.key + QStringLiteral(".mp4");
        QDir().mkpath(load.cacheDir);
        QFile out(path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(sampleMp4(bytes));
        out.close();
        Result r;
        r.ok         = true;
        r.kind       = Kind::Loop;
        r.localFile  = path;
        r.size       = bytes;
        r.mime       = QStringLiteral("video/mp4");
        r.durationMs = 4200;
        m_hub->pipeline()->fetched(load.target.key, r);
    };
    answerLoop(3 * 1024 * 1024);
    QVERIFY(serveUntil([this] { return lastSentX().has_value(); }, 20000));
    Announcement a = lastSentX()->announcement;
    QCOMPARE(a.kind, Kind::Loop); // k=a: a silent loop, shown like a GIF
    QVERIFY(a.mediaPath.endsWith(QLatin1String(".mp4")));
    QVERIFY(a.mediaSize >= 3 * 1024 * 1024 && a.mediaSize < 3 * 1024 * 1024 + 64); // the whole file went up
    QVERIFY(!a.previewPath.isEmpty());
    // Over the GIF cap (8 MB): only its poster goes up.
    sendOwn(QStringLiteral("https://giphy.com/gifs/movie-80s-ferris-buellers-day-off-bkIgPwdV3Afzq"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    const int before = m_backend->sent.size();
    answerLoop(9 * 1024 * 1024);
    QVERIFY(serveUntil([this, before] { return m_backend->sent.size() > before; }, 20000));
    a = lastSentX()->announcement;
    QCOMPARE(a.kind, Kind::Loop);
    QVERIFY(a.mediaPath.isEmpty());
    QVERIFY(!a.previewPath.isEmpty());
}

void TestLinkPreviewSnd::senderCrossSiteNoCard()
{
    sendOwn(QStringLiteral("https://bit.example.com/abc123"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    CardData card;
    card.title       = QStringLiteral("Somewhere else");
    card.description = QStringLiteral("A page on another website.");
    answerCard(card, picture(800, 400), QStringLiteral("other.example.net"));
    QTRY_COMPARE_WITH_TIMEOUT(m_backend->sent.size(), 1, 3000);
    QCOMPARE(sentTypes(), QByteArrayList({"LPXNO"}));
    const std::optional<SkipX> skip = readLpxSkip(sentMessages().first());
    QVERIFY(skip && skip->reason == SkipReason::CrossSite && skip->urlKeys.size() == 1);
    QCOMPARE(m_hub->pipeline()->counters().lpxSkipsCrossSite, 1);
    QVERIFY(fakets3::uploads().isEmpty());
    // Our own chat still shows it (with the host it went to).
    const SenderPipeline::OwnLink* own = m_hub->pipeline()->latestOwnLink(m_fetch->loads.first().target.key);
    QVERIFY(own && own->state == SenderPipeline::OwnLink::State::Fetched);
    QCOMPARE(own->result.finalHost, QStringLiteral("other.example.net"));
}

void TestLinkPreviewSnd::senderWalledNoCard()
{
    const QStringList errorsInOrder{QString::fromLatin1(errors::kWalled), QString::fromLatin1(errors::kNothing), QString::fromLatin1(errors::kExpired),
                                    QString::fromLatin1(errors::kNotFound), QString::fromLatin1(errors::kRateLimited)};
    for (int i = 0; i < errorsInOrder.size(); ++i) {
        sendOwn(QStringLiteral("https://www.example.com/page-%1").arg(i));
        QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), i + 1, 3000);
        Result r;
        r.error = errorsInOrder.at(i);
        m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, r);
        QTRY_COMPARE_WITH_TIMEOUT(m_backend->sent.size(), i + 1, 3000);
    }
    const QList<proto::Message> sent = sentMessages();
    QCOMPARE(sent.size(), 5);
    // Review fix: a wall (a consent page, a bot check) depends on the sender (their region, their address), not on the
    // page: a plain LPXNO, so receivers may still load it themselves (their own walled memo bounds it). Only "the page
    // has no card" and "the link is expired" are facts about the content (r=n).
    const QVector<SkipReason> want{SkipReason::Unknown, SkipReason::Nothing, SkipReason::Nothing, SkipReason::Unknown, SkipReason::Unknown};
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(sent.at(i).type, QByteArray("LPXNO"));
        QCOMPARE(readLpxSkip(sent.at(i))->reason, want.at(i));
    }
    QCOMPARE(m_hub->pipeline()->counters().lpxSkipsNothing, 2);
    QCOMPARE(m_hub->pipeline()->counters().lpxSkips, 5);
    QCOMPARE(m_hub->pipeline()->counters().skipNotices, 0); // LPNO only for the 2.4 classes
}

void TestLinkPreviewSnd::senderWebCardsOff()
{
    Settings::instance().showWebCards = false;
    sendOwn(QStringLiteral("look https://www.example.com/news/a-story"));
    QTest::qWait(50);
    QVERIFY(m_fetch->loads.isEmpty());
    QVERIFY(m_backend->sent.isEmpty()); // no job, no notice: our "lw" is off, nobody waits
    // A media site and a well-known site still get theirs.
    sendOwn(QStringLiteral("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32 https://www.example.com/x"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    QCOMPARE(m_fetch->loads.first().target.provider, QStringLiteral("giphy"));
    sendOwn(QStringLiteral("https://vimeo.com/76979871"));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 2, 3000);
    // 2.5 (design2 2.10): a Nextcloud share always gets its job ("lx": it may be a picture, a GIF or a video), with a
    // video's cap for a video.
    sendOwn(QStringLiteral("https://cloud.example.org/s/Ab3dE6gH9jK2mN5"));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 3, 3000);
    const Target share = classify(QStringLiteral("https://cloud.example.org/s/Ab3dE6gH9jK2mN5"));
    QCOMPARE(share.provider, QStringLiteral("nextcloud"));
    const Caps caps = m_hub->pipeline()->fetchCapsFor(share);
    QCOMPARE(caps.media, m_hub->pipeline()->fetchCapsFor(classify(QStringLiteral("https://example.com/a.png"))).media);
    QCOMPARE(caps.video, m_hub->pipeline()->fetchCapsFor(classify(QStringLiteral("https://example.com/clip.mp4"))).media);
    QVERIFY(caps.video > caps.media);
    QCOMPARE(m_hub->pipeline()->fetchCapsFor(classify(QStringLiteral("https://giphy.com/gifs/x-8FhXc8w45aN32"))).video, qint64(-1));
}

void TestLinkPreviewSnd::senderFollowsWebCardsSwitch()
{
    // Review fix: Show cards for other web pages changed while our jobs run. A share link's running fetch holds the
    // switch as it started (Caps::webCards): it starts again under the new value.
    const QString share = QStringLiteral("https://cloud.example.org/s/Ab3dE6gH9jK2mN5");
    sendOwn(share);
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const QString key = m_fetch->loads.first().target.key;
    QVERIFY(m_fetch->loads.first().caps.webCards);
    Settings::instance().showWebCards = false;
    m_hub->applySettings();
    QCOMPARE(m_fetch->canceled, QStringList({key}));
    QCOMPARE(m_fetch->loads.size(), 2);
    QCOMPARE(m_fetch->loads.last().target.key, key);
    QVERIFY(!m_fetch->loads.last().caps.webCards); // that other website's page is never loaded now
    // The canceled fetch's answer doesn't end the job; applied again without a change: nothing more.
    Result canceled;
    canceled.error = QString::fromLatin1(errors::kCanceled);
    m_hub->pipeline()->fetched(key, canceled);
    m_hub->applySettings();
    QCOMPARE(m_hub->pipeline()->jobCount(), 1);
    QCOMPARE(m_fetch->loads.size(), 2);
    QVERIFY(m_backend->sent.isEmpty());
    // The new fetch's answer ends it as always: another website while off, LPXNO r=x.
    Result away;
    away.error     = QString::fromLatin1(errors::kWebOff);
    away.finalHost = QStringLiteral("go.example.net");
    m_hub->pipeline()->fetched(key, away);
    QTRY_COMPARE_WITH_TIMEOUT(m_backend->sent.size(), 1, 3000);
    QCOMPARE(readLpxSkip(sentMessages().last())->reason, SkipReason::CrossSite);
    // On again: a share fetch started with it off loads again with it on (it would end with kWebOff otherwise).
    sendOwn(QStringLiteral("https://cloud.example.org/s/Zy9xW8vU7tS6rQ5"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    QVERIFY(!m_fetch->loads.last().caps.webCards);
    Settings::instance().showWebCards = true;
    m_hub->applySettings();
    QCOMPARE(m_fetch->loads.size(), 4);
    QVERIFY(m_fetch->loads.last().caps.webCards);
    QCOMPARE(m_fetch->canceled.size(), 2);
    Result nothing;
    nothing.error = QString::fromLatin1(errors::kNothing);
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, nothing);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    // Other links' fetches don't hold that switch: never started again.
    sendOwn(QStringLiteral("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 5, 3000);
    Settings::instance().showWebCards = false;
    m_hub->applySettings();
    QCOMPARE(m_fetch->loads.size(), 5);
    QCOMPARE(m_fetch->canceled.size(), 2);
    m_hub->pipeline()->fetched(m_fetch->loads.last().target.key, nothing);
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    // Off: web links' jobs that haven't loaded their page (running and queued) stop, with an LPXNO each, and the
    // queued one is never loaded.
    Settings::instance().showWebCards = true;
    m_hub->applySettings();
    sendOwn(QStringLiteral("https://www.example.com/news/one https://www.example.com/news/two"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 6, 3000);
    QCOMPARE(m_hub->pipeline()->jobCount(), 2);
    const QString running = m_fetch->loads.last().target.key;
    const int     before  = m_backend->sent.size();
    Settings::instance().showWebCards = false;
    m_hub->applySettings();
    QCOMPARE(m_hub->pipeline()->jobCount(), 0);
    QVERIFY(m_fetch->canceled.contains(running));
    QTRY_COMPARE_WITH_TIMEOUT(m_backend->sent.size(), before + 2, 3000);
    for (const proto::Message& m : sentMessages().mid(before))
        QCOMPARE(m.type, QByteArray("LPXNO"));
    QTest::qWait(50);
    QCOMPARE(m_fetch->loads.size(), 6);
}

void TestLinkPreviewSnd::intakeNoticeOnePerMessage()
{
    // Six links fill the minute's allowance (each its own job; they wait for the fake website).
    for (int i = 0; i < 6; ++i)
        sendOwn(QStringLiteral("https://www.example.com/story-%1").arg(i));
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 6, 3000);
    QVERIFY(m_backend->sent.isEmpty());
    // The next message's two page links are refused at intake: ONE LPXNO names both. Review fix: its file link has an
    // allowance of its own (2.4.0 receivers never read LPXNO): it gets its job.
    const QString text = QStringLiteral("https://example.com/a.png https://www.example.com/more https://vimeo.com/76979871");
    sendOwn(text);
    QTRY_COMPARE_WITH_TIMEOUT(m_backend->sent.size(), 1, 3000);
    const std::optional<SkipX> notice = readLpxSkip(sentMessages().first());
    QVERIFY(notice);
    QCOMPARE(notice->urlKeys.size(), 2);
    QVERIFY(!notice->urlKeys.contains(classify(QStringLiteral("https://example.com/a.png")).key));
    QCOMPARE(m_hub->pipeline()->jobCount(), 7);
    QCOMPARE(notice->reason, SkipReason::Unknown);
    QCOMPARE(notice->hash, message::identityHash(kOwnUid, text));
    QCOMPARE(m_hub->pipeline()->counters().intakeNotices, 1);
    // Another refused message within 10 s: no second notice (receivers' own model covers it).
    sendOwn(QStringLiteral("https://www.example.com/again"));
    QTest::qWait(50);
    QCOMPARE(m_backend->sent.size(), 1);
    QCOMPARE(m_hub->pipeline()->counters().intakeNotices, 1);
    // Flood costs: every accepted job sends at most one command (LP/LPNO or LPX/LPXNO), plus at most one notice per
    // message.
    for (int i = 0; i < 7; ++i) {
        Result r;
        r.error = QString::fromLatin1(errors::kNotFound);
        QTRY_VERIFY_WITH_TIMEOUT(m_fetch->loads.size() > i, 3000);
        m_hub->pipeline()->fetched(m_fetch->loads.at(i).target.key, r);
    }
    QTRY_COMPARE_WITH_TIMEOUT(m_hub->pipeline()->jobCount(), 0, 3000);
    QCOMPARE(m_backend->sent.size(), 7 + 1);
}

void TestLinkPreviewSnd::ownLinkRecordsForCards()
{
    sendOwn(QStringLiteral("https://www.example.com/news/a-story"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    const QString key = m_fetch->loads.first().target.key;
    const SenderPipeline::OwnLink* rec = m_hub->pipeline()->latestOwnLink(key);
    QVERIFY(rec && rec->state == SenderPipeline::OwnLink::State::Fetching);
    QCOMPARE(rec->target.cls, LinkClass::Web);
    CardData card;
    card.title       = QStringLiteral("A story");
    card.description = QStringLiteral("Its text.");
    answerCard(card, picture(1200, 630));
    rec = m_hub->pipeline()->latestOwnLink(key);
    QVERIFY(rec && rec->state == SenderPipeline::OwnLink::State::Fetched);
    QCOMPARE(rec->result.kind, Kind::Card);
    QCOMPARE(rec->result.card.title, card.title); // our own chat adopts the card at once (before the upload)
    QVERIFY(QFileInfo::exists(rec->result.card.pictureFile));
}

void TestLinkPreviewSnd::refusedUploadRemembered()
{
    // Review fix: a channel that refuses our preview uploads (no permission there) is remembered for a few minutes: the
    // next page links of that channel make no upload request that would fail again. A card with its text goes without
    // its picture at once; a card that is only a picture ends with a plain LPXNO.
    m_refuseUploads = true;
    CardData card;
    card.title       = QStringLiteral("A story");
    card.description = QStringLiteral("What happened.");
    card.layout      = CardLayout::Large;
    sendOwn(QStringLiteral("https://www.example.com/news/one"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 1, 3000);
    answerCard(card, picture(800, 450));
    QVERIFY(serveUntil([this] { return lastSentX().has_value(); }, 20000));
    QCOMPARE(lastSentX()->announcement.card.layout, CardLayout::None); // its text, without the picture
    const int uploadsAfterFirst = fakets3::uploads().size();
    QVERIFY(uploadsAfterFirst >= 1);
    // The next card of this channel: no upload request at all, its text at once.
    sendOwn(QStringLiteral("https://www.example.com/news/two"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 2, 3000);
    answerCard(card, picture(800, 450));
    QVERIFY(serveUntil([this] { return sentTypes().size() == 2; }, 20000));
    QCOMPARE(sentTypes(), QByteArrayList({"LPX", "LPX"}));
    QCOMPARE(lastSentX()->announcement.card.layout, CardLayout::None);
    QCOMPARE(fakets3::uploads().size(), uploadsAfterFirst);
    // A card that is only a picture: a plain LPXNO, no upload.
    CardData only;
    only.title  = QStringLiteral("Only a picture");
    only.layout = CardLayout::Large;
    sendOwn(QStringLiteral("https://www.example.com/news/three"));
    QTRY_COMPARE_WITH_TIMEOUT(m_fetch->loads.size(), 3, 3000);
    answerCard(only, picture(800, 450));
    QVERIFY(serveUntil([this] { return sentTypes().size() == 3; }, 20000));
    QCOMPARE(sentTypes().last(), QByteArray("LPXNO"));
    QCOMPARE(readLpxSkip(sentMessages().last())->reason, SkipReason::Unknown);
    QCOMPARE(fakets3::uploads().size(), uploadsAfterFirst);
    QCOMPARE(m_hub->pipeline()->counters().uploadsSkipped, 2);
}

void TestLinkPreviewSnd::pageMediaSensitiveMade()
{
    // D24: media from a page that declares adult content goes out marked (the LPX's sn=1), like the page's card would.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.path() + QStringLiteral("/x.png");
    QVERIFY(picture(64, 48).save(file, "PNG"));
    QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/out")));
    maker::Input in;
    in.target = classify(QStringLiteral("https://someblog.tumblr.com/post/223456789"));
    QCOMPARE(in.target.cls, LinkClass::MediaPage);
    in.fetched.ok             = true;
    in.fetched.kind           = Kind::Image;
    in.fetched.localFile      = file;
    in.fetched.card.sensitive = true;
    in.workDir                = dir.path() + QStringLiteral("/out");
    in.mayMove                = false; // copied: the second make below reads it again
    in.caps.imageMediaBytes   = 1024 * 1024;
    maker::Made made          = maker::make(in);
    QVERIFY2(made.ok, qPrintable(made.error));
    QCOMPARE(made.kind, Kind::Image);
    QVERIFY(made.sensitive);
    QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/out2")));
    in.workDir                = dir.path() + QStringLiteral("/out2");
    in.fetched.card.sensitive = false;
    made                      = maker::make(in);
    QVERIFY(made.ok);
    QVERIFY(!made.sensitive);
}

void TestLinkPreviewSnd::cardMakerSafeAndSmall()
{
    // A card's picture is decoded by its bytes only: an SVG named .jpg gives no picture (a text card stays a card).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString svg = dir.path() + QStringLiteral("/x-thumb.jpg");
    QFile         f(svg);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("<svg xmlns='http://www.w3.org/2000/svg'><image href='//127.0.0.1/share/x.png'/></svg>");
    f.close();
    maker::Input in;
    in.target = classify(QStringLiteral("https://www.example.com/a"));
    QCOMPARE(in.target.cls, LinkClass::Web);
    in.fetched.ok               = true;
    in.fetched.kind             = Kind::Card;
    in.fetched.card.title       = QStringLiteral("Title");
    in.fetched.card.description = QStringLiteral("Text");
    in.fetched.card.pictureFile = svg;
    in.fetched.card.layout      = CardLayout::Large;
    in.workDir                  = dir.path();
    maker::Made made            = maker::make(in);
    QVERIFY(made.ok);
    QVERIFY(made.previewFile.isEmpty());
    QCOMPARE(made.card.layout, CardLayout::None);
    // Without text it would be nothing.
    in.fetched.card.description.clear();
    QVERIFY(!maker::make(in).ok);
    // A thumbnail card's picture fits 256 x 256; a transparent one stays PNG.
    QImage alpha = picture(1000, 500, true);
    bool   png   = false;
    const QByteArray thumb = maker::encodeCardPicture(alpha, CardLayout::Thumb, &png);
    QVERIFY(png);
    QImage back;
    QVERIFY(back.loadFromData(thumb));
    QCOMPARE(back.width(), 256);
    QVERIFY(back.hasAlphaChannel());
    // A noisy large picture stays within 2 MB.
    QImage           noisy(2000, 1500, QImage::Format_RGB32);
    QRandomGenerator noise(5);
    for (int y = 0; y < noisy.height(); y += 1)
        for (int x = 0; x < noisy.width(); x += 1)
            noisy.setPixel(x, y, qRgb(noise.bounded(256), noise.bounded(256), noise.bounded(256)));
    const QByteArray large = maker::encodeCardPicture(noisy, CardLayout::Large, &png);
    QVERIFY(!large.isEmpty() && large.size() <= kMaxPreviewBytes);
    QVERIFY(!png);
}

TSMEDIA_REGISTER_TEST(TestLinkPreviewSnd)
#include "tst_linkpreview_snd.moc"
