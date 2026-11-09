// Unit tests for 2.4 link previews, the network core (src/linkpreview): which links get a preview and
// their keys (linkurl), the address ranges, URL and redirect rules (netpolicy), content checks and hostile
// oEmbed JSON (content), and the Fetcher end to end through a scripted transport: success, cache,
// redirects, refused addresses (before and after connecting), size caps, magic bytes, the pixel cap,
// cancel mid-download, shutdown during a stalled download, the queue and the overall timeout.
//
// Nothing here touches the internet: every Fetcher gets FakeTransport.

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QThread>

#include <atomic>

#include "core.h"
#include "fakets3.h"
#include "linkpreview/content.h"
#include "linkpreview/autoload.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/lphub.h" // linksCacheDir
#include "linkpreview/netpolicy.h"
#include "linkpreview/transport.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"

using namespace linkpreview;

namespace {

const QString kVideoId = QStringLiteral("dQw4w9WgXcQ");

net::Address ip(const char* text)
{
    return net::parseAddress(QString::fromLatin1(text));
}

// ---- a scripted network ---------------------------------------------------------------------------------

struct FakeResponse {
    int          status = 200;
    QByteArray   contentType;
    qint64       contentLength = -2; // -2: the body's size, -1: not sent
    QString      location;
    QByteArray   body;
    int          chunk           = 4096;
    int          stallAfterBytes = -1; // then wait for the cancel (or the deadline)
    bool         stallHead       = false;
    net::Address peer;                 // null: the host's first address
    bool         ignoreAllowPeer = false; // a transport that doesn't check (the Fetcher must still refuse)
    bool         unknownPeer     = false; // the response doesn't say where it came from
    // 2.4 auto-load: held after this many bytes while *hold is true (then it goes on); -1: never held
    int                                holdAfterBytes = -1;
    std::shared_ptr<std::atomic<bool>> hold;
};

class FakeTransport;

class FakeExchange : public http::Exchange
{
  public:
    FakeExchange(FakeTransport* owner, FakeResponse response, net::Address peer, const http::Cancel& cancel, quint64 deadline)
        : m_owner(owner)
        , m_response(std::move(response))
        , m_peer(peer)
        , m_cancel(cancel)
        , m_deadline(deadline)
    {
    }
    http::Error head(http::Head* out) override;
    http::Error read(char* buffer, int capacity, int* got) override;

  private:
    http::Error stall();

    FakeTransport*      m_owner;
    FakeResponse        m_response;
    net::Address        m_peer;
    const http::Cancel& m_cancel;
    quint64             m_deadline;
    int                 m_pos = 0;
};

class FakeTransport : public http::Transport
{
  public:
    QMutex                                 mutex;
    QHash<QString, QVector<net::Address>> dns;       // host -> addresses (missing: 93.184.216.34)
    QHash<QString, FakeResponse>          responses; // by the fully encoded URL (missing: 404)
    QStringList                            resolved;
    QStringList                            requested; // URLs whose request was "sent"
    QList<QByteArray>                      accepts;
    QStringList                            refusedPeers;
    QStringList                            proxied;   // URLs sent through "the Windows proxy"
    QStringList                            routed;    // URLs route() was asked about
    bool                                   viaProxy = false; // route(): every request through 127.0.0.1:10809
    std::atomic<int>                       reads{0};
    std::atomic<int>                       stalls{0};
    std::atomic<int>                       holds{0}; // exchanges waiting at their hold

    void respond(const QString& url, const FakeResponse& response)
    {
        QMutexLocker lock(&mutex);
        responses.insert(QUrl(url).toString(QUrl::FullyEncoded), response);
    }
    int requestCount()
    {
        QMutexLocker lock(&mutex);
        return requested.size();
    }
    QStringList requests()
    {
        QMutexLocker lock(&mutex);
        return requested;
    }

    http::Error resolve(const QString& host, QVector<net::Address>* out, const http::Cancel& cancel, quint64) override
    {
        if (cancel.isSet())
            return http::Error::Canceled;
        QMutexLocker lock(&mutex);
        resolved << host;
        *out = dns.value(host, {ip("93.184.216.34")});
        return http::Error::None;
    }

    http::Route route(const QUrl& url, const http::Cancel& cancel, quint64) override
    {
        Q_UNUSED(cancel);
        http::Route r;
        QMutexLocker lock(&mutex);
        routed << url.toString(QUrl::FullyEncoded);
        if (viaProxy) {
            r.viaProxy = true;
            r.proxy    = QStringLiteral("127.0.0.1:10809");
        }
        return r;
    }

    std::unique_ptr<http::Exchange> open(const http::HopOptions& options, const http::Cancel& cancel, http::Error* error) override
    {
        *error = http::Error::None;
        const QString key = options.url.toString(QUrl::FullyEncoded);
        FakeResponse  response;
        net::Address  peer;
        {
            QMutexLocker lock(&mutex);
            if (responses.contains(key)) {
                response = responses.value(key);
            } else {
                response.status = 404;
                response.body   = "missing";
            }
            peer = response.peer.isNull() ? dns.value(options.url.host(), {ip("93.184.216.34")}).value(0) : response.peer;
            if (options.route.viaProxy) {
                peer = ip("127.0.0.1"); // the connection goes to the proxy on this PC
                proxied << key;
            }
        }
        if (!response.ignoreAllowPeer && options.allowPeer && !options.allowPeer(peer)) {
            QMutexLocker lock(&mutex);
            refusedPeers << key;
            *error = http::Error::Blocked;
            return nullptr;
        }
        {
            QMutexLocker lock(&mutex);
            requested << key;
            accepts << options.accept;
        }
        return std::unique_ptr<http::Exchange>(new FakeExchange(this, response, peer, cancel, options.deadline));
    }
};

http::Error FakeExchange::stall()
{
    ++m_owner->stalls;
    const int wait = http::msUntil(m_deadline);
    if (m_cancel.wait(wait))
        return http::Error::Canceled;
    return http::Error::Timeout;
}

http::Error FakeExchange::head(http::Head* out)
{
    if (m_response.stallHead)
        return stall();
    out->status        = m_response.status;
    out->contentType   = m_response.contentType;
    out->contentLength = m_response.contentLength == -2 ? m_response.body.size() : m_response.contentLength;
    out->location      = m_response.location;
    out->peer          = m_response.unknownPeer ? net::Address() : m_peer;
    return http::Error::None;
}

http::Error FakeExchange::read(char* buffer, int capacity, int* got)
{
    *got = 0;
    if (m_cancel.isSet())
        return http::Error::Canceled;
    if (m_response.stallAfterBytes >= 0 && m_pos >= m_response.stallAfterBytes)
        return stall();
    if (m_response.holdAfterBytes >= 0 && m_pos >= m_response.holdAfterBytes && m_response.hold) {
        ++m_owner->holds;
        while (m_response.hold->load() && !m_cancel.isSet() && http::msUntil(m_deadline) > 0)
            m_cancel.wait(5);
        --m_owner->holds;
        m_response.holdAfterBytes = -1;
        if (m_cancel.isSet())
            return http::Error::Canceled;
    }
    int count = qMin(qMin(capacity, m_response.chunk), m_response.body.size() - m_pos);
    if (m_response.stallAfterBytes >= 0)
        count = qMin(count, m_response.stallAfterBytes - m_pos);
    if (count <= 0)
        return http::Error::None;
    memcpy(buffer, m_response.body.constData() + m_pos, static_cast<size_t>(count));
    m_pos += count;
    *got = count;
    ++m_owner->reads;
    return http::Error::None;
}

// ---- media made here ---------------------------------------------------------------------------------------

QByteArray encoded(const QImage& image, const char* format)
{
    QByteArray data;
    QBuffer    buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return data;
}

QByteArray pngBytes(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(30, 120, 200));
    return encoded(image, "PNG");
}

QByteArray jpegBytes(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(200, 40, 40));
    return encoded(image, "JPG");
}

// A 1x1 GIF89a.
QByteArray gifBytes()
{
    static const unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0xff, 0xff,
                                        0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
                                        0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b};
    return QByteArray(reinterpret_cast<const char*>(gif), sizeof(gif));
}

quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char byte : data) {
        crc ^= static_cast<quint8>(byte);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void appendBE(QByteArray* out, quint32 value)
{
    out->append(static_cast<char>(value >> 24));
    out->append(static_cast<char>(value >> 16));
    out->append(static_cast<char>(value >> 8));
    out->append(static_cast<char>(value));
}

// A PNG whose header claims width x height (a valid IHDR with its CRC), with no real pixels.
QByteArray pngHeaderOnly(quint32 width, quint32 height)
{
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    QByteArray ihdr("IHDR");
    appendBE(&ihdr, width);
    appendBE(&ihdr, height);
    ihdr.append(char(8)).append(char(2)).append(char(0)).append(char(0)).append(char(0));
    appendBE(&png, 13);
    png += ihdr;
    appendBE(&png, crc32(ihdr));
    for (const char* type : {"IDAT", "IEND"}) { // empty chunks: a reader learns the size before any pixel
        appendBE(&png, 0);
        png += type;
        appendBE(&png, crc32(QByteArray(type)));
    }
    return png;
}

QByteArray mp4Bytes(int size)
{
    QByteArray data("\x00\x00\x00\x18" "ftypmp42" "\x00\x00\x00\x00" "mp42isom", 24);
    data += QByteArray(size - data.size(), '\x01');
    return data;
}

Result resultOf(const QSignalSpy& spy, int index = 0)
{
    return spy.at(index).at(1).value<Result>();
}

QStringList filesIn(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files, QDir::Name);
}

bool anyPart(const QString& dir)
{
    for (const QString& name : filesIn(dir)) {
        if (name.endsWith(QLatin1String(".part")))
            return true;
    }
    return false;
}

Fetcher::Options testOptions()
{
    Fetcher::Options options;
    options.totalTimeoutMs = 10000;
    return options;
}

QString oEmbedFor(const QString& id)
{
    return net::oEmbedUrl(id).toString(QUrl::FullyEncoded);
}

} // namespace

class TestLinkPreviewNet : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        qRegisterMetaType<linkpreview::Result>(); // QSignalSpy on finished()
    }

    // ---- linkurl -----------------------------------------------------------------------------------------

    void classify_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("videoId");
        QTest::addColumn<QString>("host");
        const Kind    none = Kind::None, yt = Kind::YouTube, img = Kind::Image, gif = Kind::Gif, vid = Kind::Video;
        const QString id   = kVideoId;
        const auto    row  = [](const char* name, const QString& href, Kind kind, const QString& videoId, const QString& host) {
            QTest::newRow(name) << href << int(kind) << videoId << host;
        };

        // YouTube forms
        row("watch", "https://www.youtube.com/watch?v=" + id, yt, id, "www.youtube.com");
        row("watch bare host + start", "https://youtube.com/watch?v=" + id + "&t=42s", yt, id, "youtube.com");
        row("watch v not first", "https://www.youtube.com/watch?feature=share&v=" + id, yt, id, "www.youtube.com");
        row("mobile", "https://m.youtube.com/watch?v=" + id, yt, id, "m.youtube.com");
        row("music", "https://music.youtube.com/watch?v=" + id + "&list=RDAMVM", yt, id, "music.youtube.com");
        row("youtu.be", "https://youtu.be/" + id, yt, id, "youtu.be");
        row("youtu.be + t + slash", "https://youtu.be/" + id + "/?t=10", yt, id, "youtu.be");
        row("shorts", "https://www.youtube.com/shorts/" + id, yt, id, "www.youtube.com");
        row("shorts slash query", "https://youtube.com/shorts/" + id + "/?feature=share", yt, id, "youtube.com");
        row("upper-case scheme/host", "HTTPS://WWW.YOUTUBE.COM/watch?v=" + id, yt, id, "www.youtube.com");
        row("fragment ignored", "https://www.youtube.com/watch?v=" + id + "#t=10", yt, id, "www.youtube.com");
        row("port 443", "https://www.youtube.com:443/watch?v=" + id, yt, id, "www.youtube.com");
        row("trailing dot host", "https://www.youtube.com./watch?v=" + id, yt, id, "www.youtube.com");
        row("id with - and _", "https://youtu.be/a-b_c-d_e-f", yt, "a-b_c-d_e-f", "youtu.be");

        // Not YouTube videos
        row("short id", "https://www.youtube.com/watch?v=dQw4w9WgXc", none, "", "");
        row("long id", "https://www.youtube.com/watch?v=dQw4w9WgXcQQ", none, "", "");
        row("bad id char", "https://www.youtube.com/watch?v=dQw4w9WgX!Q", none, "", "");
        row("encoded id char", "https://www.youtube.com/watch?v=dQw4w9WgX%2FQ", none, "", "");
        row("channel", "https://www.youtube.com/@somechannel", none, "", "");
        row("playlist", "https://www.youtube.com/playlist?list=PL123", none, "", "");
        row("watch no v", "https://www.youtube.com/watch?list=PL123", none, "", "");
        row("shorts extra path", "https://www.youtube.com/shorts/" + id + "/x", none, "", "");
        row("youtube media path", "https://www.youtube.com/img/logo.png", none, "", "");
        row("look-alike suffix", "https://www.youtube.com.evil.com/watch?v=" + id, none, "", "");
        row("look-alike suffix media", "https://youtube.com.evil.com/a.png", img, "", "youtube.com.evil.com");
        row("youtube in path", "https://evil.com/www.youtube.com/watch?v=" + id, none, "", "");
        row("punycode look-alike", "https://xn--youtbe-kva.com/watch?v=" + id, none, "", "");
        row("unicode look-alike", QStringLiteral("https://youtu\u00fcbe.com/watch?v=") + id, none, "", "");
        // http YouTube links get a card too (TeamSpeak links "www.youtube.com/..." as http://); the fetch is
        // always the canonical https address.
        row("http youtube", "http://www.youtube.com/watch?v=" + id, yt, id, "www.youtube.com");
        row("http youtu.be", "http://youtu.be/" + id, yt, id, "youtu.be");
        row("http shorts", "HTTP://youtube.com/shorts/" + id, yt, id, "youtube.com");
        row("http youtube port 80", "http://www.youtube.com:80/watch?v=" + id, yt, id, "www.youtube.com");
        row("http youtube port 443", "http://www.youtube.com:443/watch?v=" + id, none, "", "");
        row("http youtube other port", "http://www.youtube.com:8080/watch?v=" + id, none, "", "");
        row("http look-alike", "http://www.youtube.com.evil.com/watch?v=" + id, none, "", "");
        row("http youtube channel", "http://www.youtube.com/@somechannel", none, "", "");
        row("http userinfo youtube", "http://user@www.youtube.com/watch?v=" + id, none, "", "");
        row("http youtube ip", "http://142.250.0.1/watch?v=" + id, none, "", "");
        row("userinfo youtube", "https://user:pass@www.youtube.com/watch?v=" + id, none, "", "");
        row("youtube as userinfo", "https://www.youtube.com@evil.com/a.png", none, "", "");
        row("empty userinfo", "https://@example.com/a.png", none, "", "");
        row("other port", "https://www.youtube.com:8443/watch?v=" + id, none, "", "");

        // Direct media
        row("png", "https://example.com/cat.png", img, "", "example.com");
        row("PNG upper", "https://example.com/CAT.PNG", img, "", "example.com");
        row("jpg query", "https://cdn.example.com/a/b/photo.jpg?w=800&h=600", img, "", "cdn.example.com");
        row("jpeg", "https://example.com/p.jpeg", img, "", "example.com");
        row("webp", "https://example.com/p.webp", img, "", "example.com");
        row("gif", "https://media.example.net/funny.gif", gif, "", "media.example.net");
        row("mp4", "https://example.com/v.mp4", vid, "", "example.com");
        row("webm", "https://example.com/v.webm", vid, "", "example.com");
        row("MOV", "https://example.com/v.MOV", vid, "", "example.com");
        row("m4v", "https://example.com/v.m4v", vid, "", "example.com");
        row("fragment", "https://example.com/cat.png#frag", img, "", "example.com");
        row("encoded dot", "https://example.com/cat%2Epng", img, "", "example.com");
        row("encoded space", "https://example.com/my%20cat.png", img, "", "example.com");
        row("dot segments", "https://example.com/a/../b/cat.png", img, "", "example.com");
        row("punycode host", "https://xn--e1afmkfd.xn--p1ai/pic.jpg", img, "", "xn--e1afmkfd.xn--p1ai");
        row("unicode host is ascii", QStringLiteral("https://b\u00fccher.example.com/a.png"), img, "", "xn--bcher-kva.example.com");
        row("underscore label", "https://my_cdn.example.com/a.png", img, "", "my_cdn.example.com");
        row("look-alike github", "https://github.com.example.com/a.png", img, "", "github.com.example.com");

        // Not media / not allowed
        row("svg", "https://example.com/a.svg", none, "", "");
        row("html", "https://example.com/page.html", none, "", "");
        row("no ext", "https://example.com/picture", none, "", "");
        row("ext in query only", "https://example.com/view?file=a.png", none, "", "");
        row("trailing slash", "https://example.com/a.png/", none, "", "");
        row("pngx", "https://example.com/a.pngx", none, "", "");
        row("only ext", "https://example.com/.png", none, "", "");
        row("root", "https://example.com/", none, "", "");
        row("http media", "http://example.com/a.png", none, "", "");
        row("http gif", "http://media.example.net/funny.gif", none, "", "");
        row("http video", "http://example.com/v.mp4", none, "", "");
        row("port media", "https://example.com:444/a.png", none, "", "");
        row("ipv4 literal", "https://93.184.216.34/a.png", none, "", "");
        row("loopback literal", "https://127.0.0.1/a.png", none, "", "");
        row("ipv6 literal", "https://[2606:2800:220:1::1]/a.png", none, "", "");
        row("ipv6 loopback", "https://[::1]/a.png", none, "", "");
        row("decimal ip", "https://2130706433/a.png", none, "", "");
        row("hex ip", "https://0x7f.1/a.png", none, "", "");
        row("short ip", "https://127.1/a.png", none, "", "");
        row("localhost", "https://localhost/a.png", none, "", "");
        row("sub.localhost", "https://app.localhost/a.png", none, "", "");
        row("mdns", "https://printer.local/a.png", none, "", "");
        row("lan", "https://nas.lan/a.png", none, "", "");
        row("internal", "https://wiki.corp.internal/a.png", none, "", "");
        row("home.arpa", "https://router.home.arpa/a.png", none, "", "");
        row("single label", "https://intranet/a.png", none, "", "");
        row("empty label", "https://a..example.com/a.png", none, "", "");
        row("no host", "https:///a.png", none, "", "");
        row("ts3file", "ts3file://example.com/a.png", none, "", "");
        row("client link", "client://5/abc~Name", none, "", "");
        row("channel link", "channelid://5", none, "", "");
        row("javascript", "javascript:alert(1)//a.png", none, "", "");
        row("data", "data:image/png;base64,AAAA", none, "", "");
        row("file", "file:///C:/a.png", none, "", "");
        row("ftp", "ftp://example.com/a.png", none, "", "");
        row("relative", "/a.png", none, "", "");
        row("scheme-less", "www.example.com/a.png", none, "", "");
        row("own repo", "https://github.com/Metihttp/Teamspeak_Media_chat", none, "", "");
        row("own repo image", "https://github.com/Metihttp/Teamspeak_Media_chat/raw/main/docs/shot.png", none, "", "");
        row("own update hint", "https://github.com/Metihttp/Teamspeak_Media_chat/releases/latest#tsmupd-1a2b-2.4.0", none, "", "");
        row("tsmupd fragment media", "https://example.com/a.png#tsmupd-1-2.4.0", none, "", "");
        row("encoded control", "https://example.com/a%0A.png", none, "", "");
        row("backslash trick", "https://example.com\\@evil.com/a.png", none, "", "");
    }

    void classify()
    {
        QFETCH(QString, href);
        QFETCH(int, kind);
        QFETCH(QString, videoId);
        QFETCH(QString, host);
        const Target target = linkpreview::classify(href);
        QCOMPARE(int(target.kind), kind);
        QCOMPARE(target.videoId, videoId);
        QCOMPARE(target.host, host);
        if (target.isValid()) {
            QVERIFY(isKey(target.key));
            QVERIFY(net::isAllowedUrl(target.url));
            QVERIFY(!target.url.hasFragment());
            QCOMPARE(target.url.scheme(), QStringLiteral("https"));
            if (target.kind == Kind::YouTube)
                QCOMPARE(target.url, youTubeWatchUrl(videoId));
        } else {
            QVERIFY(target.key.isEmpty());
        }
        // The QUrl overload agrees for anything QUrl parses the same way.
        if (href.startsWith(QLatin1String("http"), Qt::CaseInsensitive) && !href.contains(QLatin1Char('\\')) && !href.contains(QLatin1Char('@')))
            QCOMPARE(int(linkpreview::classify(QUrl(href)).kind), kind);
    }

    void classifyHugeAndHidden()
    {
        const QString head = QStringLiteral("https://example.com/");
        // The longest allowed address, and one character more.
        const QString fits = head + QString(kMaxUrlChars - head.size() - 4, QLatin1Char('a')) + QStringLiteral(".png");
        QCOMPARE(fits.size(), kMaxUrlChars);
        QCOMPARE(linkpreview::classify(fits).kind, Kind::Image);
        QCOMPARE(linkpreview::classify(QString(fits).insert(head.size(), QLatin1Char('b'))).kind, Kind::None);
        QCOMPARE(linkpreview::classify(head + QString(100000, QLatin1Char('a')) + QStringLiteral(".png")).kind, Kind::None);
        QCOMPARE(linkpreview::classify(QStringLiteral("https://example.com/a.png?") + QString(5000, QLatin1Char('q'))).kind, Kind::None);
        // A host over 253 characters.
        QString longHost;
        for (int i = 0; i < 5; ++i)
            longHost += QString(60, QLatin1Char('a' + i)) + QLatin1Char('.');
        QCOMPARE(linkpreview::classify(QStringLiteral("https://") + longHost + QStringLiteral("com/a.png")).kind, Kind::None);
        QCOMPARE(linkpreview::classify(QStringLiteral("https://") + QString(64, QLatin1Char('a')) + QStringLiteral(".com/a.png")).kind, Kind::None);
        // White space, controls, bidi marks and invisible characters in a href.
        for (const ushort u : {ushort(0x20), ushort(0x09), ushort(0x0A), ushort(0x00), ushort(0x7F), ushort(0x85), ushort(0x200B), ushort(0x200E),
                               ushort(0x202E), ushort(0x2066), ushort(0xFEFF), ushort(0x2028)}) {
            const QString href = QStringLiteral("https://example.com/c") + QChar(u) + QStringLiteral("at.png");
            QVERIFY2(linkpreview::classify(href).kind == Kind::None, qPrintable(QString::number(u, 16)));
        }
        QCOMPARE(linkpreview::classify(QStringLiteral(" https://example.com/a.png")).kind, Kind::None);
        QCOMPARE(linkpreview::classify(QString()).kind, Kind::None);
        QCOMPARE(linkpreview::classify(QUrl()).kind, Kind::None);
    }

    void keys()
    {
        const auto key = [](const char* href) { return linkpreview::classify(QString::fromLatin1(href)).key; };
        // Every form of one video shares a key.
        const QString yt = key("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
        QCOMPARE(yt.size(), kKeyChars);
        QCOMPARE(key("https://youtu.be/dQw4w9WgXcQ?t=5"), yt);
        QCOMPARE(key("https://m.youtube.com/watch?v=dQw4w9WgXcQ&list=x"), yt);
        QCOMPARE(key("https://youtube.com/shorts/dQw4w9WgXcQ"), yt);
        QVERIFY(key("https://youtu.be/dQw4w9WgXcA") != yt);
        // Media: one normalized address, one key.
        const QString png = key("https://example.com/~me/cat.png?a=1");
        QCOMPARE(key("https://EXAMPLE.com:443/%7eme/cat.png?a=1#x"), png);
        QCOMPARE(key("https://example.com./%7Eme/cat.png?a=1"), png);
        QVERIFY(key("https://example.com/~me/cat.png?a=2") != png);
        QVERIFY(key("https://example.com/~me/cat.png") != png);
        QVERIFY(key("https://example.com/~me/CAT.png?a=1") != png);
        QCOMPARE(key("https://example.com/a%2fb.png"), key("https://example.com/a%2Fb.png"));
        QVERIFY(isKey(png));
        QVERIFY(!isKey(png.toUpper()));
        QVERIFY(!isKey(png.left(31)));
        // The normalized address itself.
        QCOMPARE(linkpreview::classify(QStringLiteral("https://EXAMPLE.com:443/%7eme/cat.png?a=1#x")).url.toString(QUrl::FullyEncoded),
                 QStringLiteral("https://example.com/~me/cat.png?a=1"));
        QCOMPARE(youTubeWatchUrl(kVideoId).toString(), QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
        QVERIFY(!youTubeWatchUrl(QStringLiteral("short")).isValid());
    }

    void logUrls()
    {
        QCOMPARE(logUrl(QUrl(QStringLiteral("https://cdn.example.com/a/b/cat.gif?token=SECRET#frag"))), QStringLiteral("https://cdn.example.com/\u2026/cat.gif"));
        QCOMPARE(logUrl(QUrl(QStringLiteral("https://user:pw@example.com:8443/cat.png?x=1"))), QStringLiteral("https://example.com/cat.png"));
        QCOMPARE(logUrl(QUrl(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ"))), QStringLiteral("https://www.youtube.com/watch"));
        const QString longName = logUrl(QUrl(QStringLiteral("https://example.com/") + QString(80, QLatin1Char('n')) + QStringLiteral(".png")));
        QVERIFY(longName.size() < 70);
        QVERIFY(!logUrl(QUrl(QStringLiteral("https://example.com/a.png?secret=1"))).contains(QLatin1String("secret")));
    }

    // ---- netpolicy -------------------------------------------------------------------------------------

    void addressRanges_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("isPublic");
        const QList<QPair<const char*, bool>> rows = {
            {"8.8.8.8", true},           {"1.1.1.1", true},          {"93.184.216.34", true},      {"0.0.0.0", false},
            {"0.1.2.3", false},          {"10.0.0.1", false},        {"10.255.255.255", false},    {"100.63.255.255", true},
            {"100.64.0.1", false},       {"100.127.255.255", false}, {"100.128.0.0", true},        {"127.0.0.1", false},
            {"127.255.0.1", false},      {"169.254.169.254", false}, {"172.15.255.255", true},     {"172.16.0.1", false},
            {"172.31.255.255", false},   {"172.32.0.1", true},       {"192.0.0.8", false},         {"192.0.2.1", false},
            {"192.88.99.1", false},      {"192.167.255.255", true},  {"192.168.1.1", false},       {"198.17.255.255", true},
            {"198.18.0.1", false},       {"198.19.255.255", false},  {"198.20.0.0", true},         {"198.51.100.7", false},
            {"203.0.113.9", false},      {"224.0.0.1", false},       {"239.255.255.250", false},   {"240.0.0.1", false},
            {"255.255.255.255", false},  {"223.255.255.255", true},
            {"::", false},               {"::1", false},             {"fe80::1", false},           {"febf::1", false},
            {"fec0::1", false},          {"fc00::1", false},         {"fd12:3456::1", false},      {"ff02::1", false},
            {"::ffff:10.0.0.1", false},  {"::ffff:127.0.0.1", false}, {"::ffff:8.8.8.8", true},    {"::ffff:0808:0808", true},
            {"64:ff9b::808:808", true},  {"64:ff9b::a00:1", false},  {"64:ff9b::7f00:1", false},   {"64:ff9b:1::1", false},
            {"2002:0808:0808::1", true}, {"2002:0a00:0001::1", false}, {"2002:c0a8:0101::", false}, {"2001::1", false},
            {"2001:0:4136:e378::1", false}, {"2001:1ff::1", false},  {"2001:200::1", true},        {"2001:db8::1", false},
            {"3fff::1", false},          {"3fff:1000::1", true},     {"5f00::1", false},           {"100::1", false},
            {"::8.8.8.8", false},        {"2606:4700:4700::1111", true}, {"2a00:1450:4001::200e", true}, {"4000::1", false},
        };
        for (const auto& row : rows)
            QTest::newRow(row.first) << QString::fromLatin1(row.first) << row.second;
    }

    void addressRanges()
    {
        QFETCH(QString, address);
        QFETCH(bool, isPublic);
        const net::Address parsed = net::parseAddress(address);
        QVERIFY2(!parsed.isNull(), qPrintable(address));
        QCOMPARE(net::isPublicAddress(parsed), isPublic);
    }

    void parseAddresses()
    {
        QCOMPARE(net::parseAddress(QStringLiteral("1.2.3.4")).ipv4(), 0x01020304u);
        QCOMPARE(net::parseAddress(QStringLiteral(" 1.2.3.4:443 ")).ipv4(), 0x01020304u);
        QCOMPARE(net::parseAddress(QStringLiteral("[2606:2800::1]:443")), net::parseAddress(QStringLiteral("2606:2800:0:0:0:0:0:1")));
        QCOMPARE(net::parseAddress(QStringLiteral("fe80::1%12")), net::parseAddress(QStringLiteral("fe80::1")));
        QCOMPARE(net::parseAddress(QStringLiteral("::ffff:1.2.3.4")).family, net::Address::Family::IPv6);
        QCOMPARE(net::parseAddress(QStringLiteral("2606:2800::1")).toString(), QStringLiteral("2606:2800:0:0:0:0:0:1"));
        QCOMPARE(net::Address::fromIPv4(0x7F000001).toString(), QStringLiteral("127.0.0.1"));
        for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1.2.3.-4", "1..3.4", "0x7f.0.0.1", "2130706433", ":::",
                                "1::2::3", "12345::1", "g::1", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", ":1::2", "::1.2.3", "example.com",
                                "[::1", "1.2.3.4:443:1"}) {
            QVERIFY2(net::parseAddress(QString::fromLatin1(bad)).isNull(), bad);
        }
        QVERIFY(!net::isPublicAddress(net::Address()));
        QVERIFY(!net::parseAddress(QStringLiteral("1:2:3:4:5:6:7::")).isNull());
        QVERIFY(!net::parseAddress(QStringLiteral("::2:3:4:5:6:7:8")).isNull());
    }

    void urlRulesAndRedirects()
    {
        const QUrl here(QStringLiteral("https://cdn.example.com/a/b/cat.png?x=1"));
        QVERIFY(net::isAllowedUrl(here));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("http://cdn.example.com/a.png"))));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("https://u@cdn.example.com/a.png"))));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("https://cdn.example.com:8443/a.png"))));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("https://10.0.0.1/a.png"))));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("https://localhost/a.png"))));
        QVERIFY(!net::isAllowedUrl(QUrl(QStringLiteral("/relative.png"))));

        const auto next = [&](const char* location) { return net::redirectTarget(here, QString::fromLatin1(location)).toString(QUrl::FullyEncoded); };
        QCOMPARE(next("/other/dog.png"), QStringLiteral("https://cdn.example.com/other/dog.png"));
        QCOMPARE(next("dog.png?y=2"), QStringLiteral("https://cdn.example.com/a/b/dog.png?y=2"));
        QCOMPARE(next("//images.example.org/x.png"), QStringLiteral("https://images.example.org/x.png"));
        QCOMPARE(next("https://images.example.org/x.png#frag"), QStringLiteral("https://images.example.org/x.png"));
        QCOMPARE(next("  https://images.example.org/x  "), QStringLiteral("https://images.example.org/x"));
        for (const char* refused : {"", "http://images.example.org/x.png", "ftp://images.example.org/x.png", "https://127.0.0.1/x.png",
                                    "https://[::1]/x.png", "https://localhost/x.png", "https://user@images.example.org/x.png",
                                    "https://images.example.org:444/x.png", "javascript:alert(1)", "file:///C:/x.png", "//10.1.2.3/x",
                                    "https://intranet/x", "https://images.example.org/a\nb", "data:text/html,hi"}) {
            QVERIFY2(next(refused).isEmpty(), refused);
        }
        QVERIFY(next(qPrintable(QStringLiteral("/") + QString(kMaxUrlChars, QLatin1Char('a')))).isEmpty());
    }

    void youTubeAddresses()
    {
        QCOMPARE(oEmbedFor(kVideoId), QStringLiteral("https://www.youtube.com/oembed?url=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3DdQw4w9WgXcQ&format=json"));
        QVERIFY(net::isOEmbedHop(net::oEmbedUrl(kVideoId)));
        QVERIFY(!net::isOEmbedHop(QUrl(QStringLiteral("https://evil.com/oembed?url=x"))));
        QVERIFY(!net::isOEmbedHop(QUrl(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ"))));
        QVERIFY(!net::oEmbedUrl(QStringLiteral("bad")).isValid());

        const auto allowed = [](const char* url) { return net::isAllowedThumbnail(QUrl(QString::fromLatin1(url), QUrl::StrictMode), kVideoId); };
        QVERIFY(allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg"));
        QVERIFY(allowed("https://i.ytimg.com/vi_webp/dQw4w9WgXcQ/sddefault.webp"));
        QVERIFY(!allowed("http://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com.evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(!allowed("https://i9.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(!allowed("https://evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com/vi/AAAAAAAAAAA/hqdefault.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg?x=1"));
        QVERIFY(!allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ/../../x.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ%2Fhqdefault.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.png"));
        QVERIFY(!allowed("https://user@i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(!allowed("https://i.ytimg.com:8443/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QCOMPARE(net::defaultThumbnail(kVideoId).toString(), QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"));
        QVERIFY(net::isThumbnailHop(net::defaultThumbnail(kVideoId)));
        QVERIFY(!net::isThumbnailHop(QUrl(QStringLiteral("https://yt3.ggpht.com/x.jpg"))));
    }

    // ---- content ---------------------------------------------------------------------------------------

    void sniffAndTypes()
    {
        using content::Format;
        QCOMPARE(content::sniff(pngBytes(4, 4)), Format::Png);
        QCOMPARE(content::sniff(jpegBytes(4, 4)), Format::Jpeg);
        QCOMPARE(content::sniff(gifBytes()), Format::Gif);
        QCOMPARE(content::sniff(QByteArray("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)), Format::WebP);
        QCOMPARE(content::sniff(QByteArray("RIFF\x10\x00\x00\x00WAVEfmt ", 16)), Format::Unknown);
        QCOMPARE(content::sniff(mp4Bytes(64)), Format::Mp4);
        QCOMPARE(content::sniff(QByteArray("\x00\x00\x00\x14" "ftypqt  ", 12)), Format::Mov);
        QCOMPARE(content::sniff(QByteArray("\x00\x00\x00\x08" "wide", 8)), Format::Mov);
        QCOMPARE(content::sniff(QByteArray("\x1A\x45\xDF\xA3\x9F\x42\x86\x81", 8)), Format::WebM);
        QCOMPARE(content::sniff(QByteArray("<!DOCTYPE html><html>")), Format::Unknown);
        QCOMPARE(content::sniff(QByteArray("<svg xmlns=")), Format::Unknown);
        QCOMPARE(content::sniff(QByteArray("\x89PN", 3)), Format::Unknown);
        QCOMPARE(content::sniff(QByteArray()), Format::Unknown);

        QVERIFY(content::fits(Kind::Image, Format::Gif));
        QVERIFY(content::fits(Kind::Gif, Format::WebP));
        QVERIFY(!content::fits(Kind::Image, Format::Mp4));
        QVERIFY(!content::fits(Kind::Video, Format::Png));
        QVERIFY(content::fits(Kind::Video, Format::WebM));
        QVERIFY(!content::fits(Kind::Video, Format::Unknown));
        QCOMPARE(QByteArray(content::extensionOf(Format::Jpeg)), QByteArray("jpg"));
        QCOMPARE(QByteArray(content::mimeOf(Format::Mov)), QByteArray("video/quicktime"));

        QVERIFY(content::contentTypeAllowed(Kind::Image, "image/png"));
        QVERIFY(content::contentTypeAllowed(Kind::Image, "IMAGE/JPEG; charset=binary"));
        QVERIFY(content::contentTypeAllowed(Kind::Image, ""));
        QVERIFY(content::contentTypeAllowed(Kind::Gif, "application/octet-stream"));
        QVERIFY(!content::contentTypeAllowed(Kind::Image, "image/svg+xml"));
        QVERIFY(!content::contentTypeAllowed(Kind::Image, "text/html; charset=utf-8"));
        QVERIFY(!content::contentTypeAllowed(Kind::Image, "application/json"));
        QVERIFY(!content::contentTypeAllowed(Kind::Image, "video/mp4"));
        QVERIFY(content::contentTypeAllowed(Kind::Video, "video/mp4"));
        QVERIFY(content::contentTypeAllowed(Kind::Video, "application/mp4"));
        QVERIFY(!content::contentTypeAllowed(Kind::Video, "image/gif"));
        QVERIFY(!content::contentTypeAllowed(Kind::Video, "text/plain"));
        QVERIFY(content::isJsonType("application/json; charset=utf-8"));
        QVERIFY(!content::isJsonType("text/html"));
        QVERIFY(!content::isJsonType(""));
    }

    void cleanTitles()
    {
        QCOMPARE(content::cleanText(QStringLiteral("  Hello \t\n  World  "), 200), QStringLiteral("Hello World"));
        // Controls, bidi marks, zero-width space, BOM and non-characters go; ZWJ and ZWNJ stay.
        QCOMPARE(content::cleanText(QStringLiteral("a\u0085b"), 200), QStringLiteral("a b")); // NEL is a line break
        QCOMPARE(content::cleanText(QStringLiteral("a\u0000b\u0007c\u009Fd\u202Ee\u2066f\u200Bg\uFEFFh\u200Ei\u061Cj\uFFFEk"), 200), QStringLiteral("abcdefghijk"));
        QCOMPARE(content::cleanText(QStringLiteral("x\u200Cy\u200Dz"), 200), QStringLiteral("x\u200Cy\u200Dz"));
        QCOMPARE(content::cleanText(QStringLiteral("line\u2028break\u2029here"), 200), QStringLiteral("line break here"));
        // Lone surrogates out, pairs kept.
        QCOMPARE(content::cleanText(QString(QChar(0xD83D)) + QStringLiteral("a") + QChar(0xDE00), 200), QStringLiteral("a"));
        const QString smile = QString(QChar(0xD83D)) + QChar(0xDE00);
        QCOMPARE(content::cleanText(smile + smile, 200), smile + smile);
        // Cut: never half a pair, with an ellipsis.
        const QString cut = content::cleanText(QString(9, QLatin1Char('a')) + smile + smile, 11);
        QVERIFY(cut.size() <= 11);
        QVERIFY(cut.endsWith(QChar(0x2026)));
        QVERIFY(!cut.at(cut.size() - 2).isHighSurrogate());
        QCOMPARE(content::cleanText(QString(500, QLatin1Char('t')), content::kMaxTitleChars).size(), content::kMaxTitleChars);
        QCOMPARE(content::cleanText(QStringLiteral("\u202E\u200B  "), 200), QString());
    }

    void oEmbedHostile_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QString>("error");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("thumbnail");
        const QString std = QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg");
        const auto    row = [](const char* name, const QByteArray& json, bool ok, const QString& error, const QString& title, const QString& thumbnail) {
            QTest::newRow(name) << json << ok << error << title << thumbnail;
        };
        row("valid", QByteArray("{\"type\":\"video\",\"title\":\"Never Gonna\",\"author_name\":\"Rick\",\"thumbnail_url\":\"https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg\",\"html\":\"<iframe src=x>\"}"), true, "", "Never Gonna", "https://i.ytimg.com/vi/dQw4w9WgXcQ/maxresdefault.jpg");
        row("no type", QByteArray("{\"title\":\"T\"}"), true, "", "T", std);
        row("evil thumbnail", QByteArray("{\"title\":\"T\",\"thumbnail_url\":\"https://evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"}"), true, "", "T", std);
        row("thumbnail of another video", QByteArray("{\"title\":\"T\",\"thumbnail_url\":\"https://i.ytimg.com/vi/AAAAAAAAAAA/hqdefault.jpg\"}"), true, "", "T", std);
        row("http thumbnail", QByteArray("{\"title\":\"T\",\"thumbnail_url\":\"http://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"}"), true, "", "T", std);
        row("thumbnail number", QByteArray("{\"title\":\"T\",\"thumbnail_url\":5}"), true, "", "T", std);
        row("bidi title", QByteArray("{\"title\":\"a\\u202Eb\\u0000c\\n d\"}"), true, "", "abc d", std);
        row("not json", QByteArray("<html>nope</html>"), false, "badJson", "", "");
        row("array", QByteArray("[{\"title\":\"T\"}]"), false, "badJson", "", "");
        row("string", QByteArray("\"T\""), false, "badJson", "", "");
        row("trailing garbage", QByteArray("{\"title\":\"T\"} x"), false, "badJson", "", "");
        row("truncated", QByteArray("{\"title\":\"T"), false, "badJson", "", "");
        row("empty", QByteArray(), false, "badJson", "", "");
        row("title number", QByteArray("{\"title\":42}"), false, "noTitle", "", "");
        row("title object", QByteArray("{\"title\":{\"a\":1}}"), false, "noTitle", "", "");
        row("title only marks", QByteArray("{\"title\":\"\\u202E\\u200B \"}"), false, "noTitle", "", "");
        row("no title", QByteArray("{\"author_name\":\"x\"}"), false, "noTitle", "", "");
        row("rich type", QByteArray("{\"type\":\"rich\",\"title\":\"T\"}"), false, "wrongType", "", "");
        row("type number", QByteArray("{\"type\":1,\"title\":\"T\"}"), false, "wrongType", "", "");
        QByteArray deep;
        for (int i = 0; i < 2000; ++i)
            deep += "{\"a\":";
        row("deep nesting", deep, false, "badJson", "", "");
        row("too large", QByteArray("{\"title\":\"") + QByteArray(70000, 'x') + QByteArray("\"}"), false, "tooLarge", "", "");
    }

    void oEmbedHostile()
    {
        QFETCH(QByteArray, json);
        QFETCH(bool, ok);
        QFETCH(QString, error);
        QFETCH(QString, title);
        QFETCH(QString, thumbnail);
        content::OEmbed out;
        QString         why;
        QCOMPARE(content::parseOEmbed(json, kVideoId, &out, &why), ok);
        if (ok) {
            QCOMPARE(out.title, title);
            QCOMPARE(out.thumbnail.toString(), thumbnail);
            QVERIFY(net::isAllowedThumbnail(out.thumbnail, kVideoId));
        } else {
            QCOMPARE(why, error);
        }
    }

    void oEmbedLongNames()
    {
        content::OEmbed out;
        QByteArray      json = "{\"title\":\"" + QByteArray(5000, 'T') + "\",\"author_name\":\"" + QByteArray(5000, 'A') + "\"}";
        QVERIFY(content::parseOEmbed(json, kVideoId, &out, nullptr));
        QCOMPARE(out.title.size(), content::kMaxTitleChars);
        QCOMPARE(out.author.size(), content::kMaxAuthorChars);
    }

    // ---- the Fetcher -------------------------------------------------------------------------------------

    void fetchImageAndCache()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        const QByteArray png = pngBytes(64, 48);
        FakeResponse     response;
        response.contentType = "image/png";
        response.body        = png;
        response.chunk       = 100;
        fake->respond(QStringLiteral("https://cdn.example.com/cat.png?x=1"), response);

        Fetcher    fetcher(fake, testOptions());
        QStringList lines;
        fetcher.setLogger([&lines](const QString& line) { lines << line; });
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        QSignalSpy progress(&fetcher, &Fetcher::progress);
        const Target target = linkpreview::classify(QStringLiteral("https://cdn.example.com/cat.png?x=1#f"));
        fetcher.load(target, 1 << 20, dir.path());
        QCOMPARE(finished.count(), 0); // never from inside load()
        QVERIFY(fetcher.isLoading(target.key));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.at(0).at(0).toString(), target.key);
        const Result r = resultOf(finished);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.mime, QStringLiteral("image/png"));
        QCOMPARE(r.size, qint64(png.size()));
        QCOMPARE(r.width, 64);
        QCOMPARE(r.height, 48);
        QCOMPARE(r.localFile, dir.path() + QLatin1Char('/') + target.key + QStringLiteral(".png"));
        QFile file(r.localFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), png);
        QVERIFY(progress.count() >= 1);
        QCOMPARE(progress.last().at(1).toLongLong(), qint64(png.size()));
        QCOMPARE(progress.last().at(2).toLongLong(), qint64(png.size()));
        QVERIFY(!anyPart(dir.path()));
        QVERIFY(QFile::exists(dir.path() + QLatin1Char('/') + target.key + QStringLiteral(".json")));
        QVERIFY(!fetcher.isLoading(target.key));
        // What went over the "network": one request, the normalized address, an image Accept header.
        QCOMPARE(fake->requests(), QStringList{QStringLiteral("https://cdn.example.com/cat.png?x=1")});
        QVERIFY(fake->accepts.value(0).startsWith("image/"));
        QCOMPARE(fake->resolved, QStringList{QStringLiteral("cdn.example.com")});
        // The log line has no query string.
        QCOMPARE(lines.size(), 1);
        QVERIFY(lines.at(0).contains(QLatin1String("cdn.example.com")));
        QVERIFY(!lines.at(0).contains(QLatin1String("x=1")));

        // Again: from the cache, no network.
        Result cached;
        QVERIFY(Fetcher::cached(target, dir.path(), &cached));
        QCOMPARE(cached.localFile, r.localFile);
        QCOMPARE(cached.width, 64);
        fetcher.load(target, 1 << 20, dir.path());
        QVERIFY(finished.wait(10000));
        QVERIFY(resultOf(finished, 1).ok);
        QCOMPARE(fake->requestCount(), 1);
        // A smaller cap than the cached file: too large, still without network.
        fetcher.load(target, 10, dir.path());
        QVERIFY(finished.wait(10000));
        QCOMPARE(resultOf(finished, 2).error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(resultOf(finished, 2).size, qint64(png.size()));
        QCOMPARE(fake->requestCount(), 1);
        // A damaged cache record is not trusted.
        QFile media(r.localFile);
        QVERIFY(media.open(QIODevice::Append));
        media.write("x");
        media.close();
        QVERIFY(!Fetcher::cached(target, dir.path(), &cached));
        // Another link has no cache.
        QVERIFY(!Fetcher::cached(linkpreview::classify(QStringLiteral("https://cdn.example.com/dog.png")), dir.path(), &cached));
    }

    void fetchGifJpegVideo()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  gif;
        gif.contentType = "image/gif";
        gif.body        = gifBytes();
        fake->respond(QStringLiteral("https://example.com/a.gif"), gif);
        FakeResponse jpeg;
        jpeg.contentType = ""; // none sent: the bytes decide
        jpeg.body        = jpegBytes(40, 30);
        fake->respond(QStringLiteral("https://example.com/b.webp"), jpeg); // the name lies, the bytes are a JPEG
        FakeResponse video;
        video.contentType = "video/mp4";
        video.body        = mp4Bytes(3000);
        fake->respond(QStringLiteral("https://example.com/c.mp4"), video);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/a.gif")), 1 << 20, dir.path());
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/b.webp")), 1 << 20, dir.path());
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/c.mp4")), 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 20000);
        QHash<QString, Result> byKey;
        for (const QList<QVariant>& args : finished)
            byKey.insert(args.at(0).toString(), args.at(1).value<Result>());
        const Result g = byKey.value(linkpreview::classify(QStringLiteral("https://example.com/a.gif")).key);
        QVERIFY2(g.ok, qPrintable(g.error));
        QCOMPARE(g.mime, QStringLiteral("image/gif"));
        QVERIFY(g.localFile.endsWith(QLatin1String(".gif")));
        QCOMPARE(g.width, 1);
        const Result j = byKey.value(linkpreview::classify(QStringLiteral("https://example.com/b.webp")).key);
        QVERIFY2(j.ok, qPrintable(j.error));
        QCOMPARE(j.mime, QStringLiteral("image/jpeg"));
        QVERIFY(j.localFile.endsWith(QLatin1String(".jpg")));
        QCOMPARE(j.width, 40);
        const Result v = byKey.value(linkpreview::classify(QStringLiteral("https://example.com/c.mp4")).key);
        QVERIFY2(v.ok, qPrintable(v.error)); // not a playable video, but the file is there (no poster, no duration)
        QCOMPARE(v.mime, QStringLiteral("video/mp4"));
        QVERIFY(v.localFile.endsWith(QLatin1String(".mp4")));
        QCOMPARE(v.size, qint64(3000));
        QVERIFY(fake->accepts.contains("video/mp4,video/webm,video/quicktime,video/*;q=0.8"));
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchYouTube()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  json;
        json.contentType = "application/json; charset=utf-8";
        json.body        = "{\"type\":\"video\",\"title\":\"Never \\u202EGonna\",\"author_name\":\"Rick\\u0000 Astley\",\"thumbnail_url\":\"https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"}";
        fake->respond(oEmbedFor(kVideoId), json);
        FakeResponse thumb;
        thumb.contentType = "image/jpeg";
        thumb.body        = jpegBytes(480, 360);
        fake->respond(QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"), thumb);

        Fetcher      fetcher(fake, testOptions());
        QSignalSpy   finished(&fetcher, &Fetcher::finished);
        const Target target = linkpreview::classify(QStringLiteral("https://youtu.be/dQw4w9WgXcQ?t=30"));
        fetcher.load(target, 0, dir.path());
        QVERIFY(finished.wait(10000));
        const Result r = resultOf(finished);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.title, QStringLiteral("Never Gonna"));
        QCOMPARE(r.author, QStringLiteral("Rick Astley"));
        QVERIFY(r.localFile.isEmpty());
        QCOMPARE(r.thumbnailFile, dir.path() + QLatin1Char('/') + target.key + QStringLiteral("-thumb.jpg"));
        QCOMPARE(r.mime, QStringLiteral("image/jpeg"));
        QCOMPARE(r.width, 480);
        QCOMPARE(r.height, 360);
        QCOMPARE(fake->requests(), (QStringList{oEmbedFor(kVideoId), QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg")}));
        QCOMPARE(fake->accepts.value(0), QByteArray("application/json"));
        Result cached;
        QVERIFY(Fetcher::cached(target, dir.path(), &cached));
        QCOMPARE(cached.title, r.title);
        QCOMPARE(cached.thumbnailFile, r.thumbnailFile);
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchYouTubeHostile()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  json;
        json.contentType = "application/json";
        json.body        = "{\"title\":\"T\",\"thumbnail_url\":\"https://evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"}";
        fake->respond(oEmbedFor(kVideoId), json);
        FakeResponse evil;
        evil.contentType = "image/jpeg";
        evil.body        = jpegBytes(10, 10);
        fake->respond(QStringLiteral("https://evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg"), evil);
        FakeResponse thumb = evil;
        // The standard thumbnail redirects off i.ytimg.com: never followed.
        thumb.status   = 302;
        thumb.body     = QByteArray();
        thumb.location = QStringLiteral("https://evil.com/vi/dQw4w9WgXcQ/hqdefault.jpg");
        fake->respond(QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"), thumb);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        fetcher.load(linkpreview::classify(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ")), 0, dir.path());
        QVERIFY(finished.wait(10000));
        const Result r = resultOf(finished);
        QVERIFY(r.ok); // a title without a picture
        QCOMPARE(r.title, QStringLiteral("T"));
        QVERIFY(r.thumbnailFile.isEmpty());
        for (const QString& url : fake->requests())
            QVERIFY2(!url.contains(QLatin1String("evil.com")), qPrintable(url));
        QVERIFY(!fake->resolved.contains(QStringLiteral("evil.com")));
        // Not complete: not cached, so the next load tries again.
        Result cached;
        QVERIFY(!Fetcher::cached(linkpreview::classify(QStringLiteral("https://youtu.be/dQw4w9WgXcQ")), dir.path(), &cached));
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchYouTubeErrors()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        const QString idMissing = QStringLiteral("AAAAAAAAAAA");
        const QString idBadJson = QStringLiteral("BBBBBBBBBBB");
        const QString idNoEmbed = QStringLiteral("CCCCCCCCCCC");
        const QString idHtml    = QStringLiteral("DDDDDDDDDDD");
        FakeResponse  missing;
        missing.status = 404;
        fake->respond(oEmbedFor(idMissing), missing);
        FakeResponse bad;
        bad.contentType = "application/json";
        bad.body        = "{\"title\":";
        fake->respond(oEmbedFor(idBadJson), bad);
        FakeResponse closed;
        closed.status = 401;
        fake->respond(oEmbedFor(idNoEmbed), closed);
        FakeResponse thumb;
        thumb.contentType = "image/jpeg";
        thumb.body        = jpegBytes(120, 90);
        fake->respond(net::defaultThumbnail(idNoEmbed).toString(), thumb);
        FakeResponse html;
        html.contentType = "text/html";
        html.body        = "<html>consent page</html>";
        fake->respond(oEmbedFor(idHtml), html);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const QString& id) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QStringLiteral("https://youtu.be/") + id), 0, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        QCOMPARE(load(idMissing).error, QString::fromLatin1(errors::kUnavailable));
        QCOMPARE(load(idBadJson).error, QString::fromLatin1(errors::kBadResponse));
        QCOMPARE(load(idHtml).error, QString::fromLatin1(errors::kBadType));
        const Result noEmbed = load(idNoEmbed); // oEmbed closed for this video: the thumbnail alone
        QVERIFY2(noEmbed.ok, qPrintable(noEmbed.error));
        QVERIFY(noEmbed.title.isEmpty());
        QCOMPARE(noEmbed.width, 120);
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchRedirects()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        const QByteArray png = pngBytes(8, 8);
        FakeResponse     hop;
        hop.status   = 302;
        hop.location = QStringLiteral("/real/cat.png?sig=1");
        fake->respond(QStringLiteral("https://example.com/cat.png"), hop);
        FakeResponse fine;
        fine.contentType = "image/png";
        fine.body        = png;
        fake->respond(QStringLiteral("https://example.com/real/cat.png?sig=1"), fine);
        // A loop of six hops.
        for (int i = 0; i < 7; ++i) {
            FakeResponse loop;
            loop.status   = 301;
            loop.location = QStringLiteral("https://example.com/loop%1.png").arg(i + 1);
            fake->respond(QStringLiteral("https://example.com/loop%1.png").arg(i), loop);
        }
        FakeResponse five = fine;
        fake->respond(QStringLiteral("https://example.com/loop5.png"), five); // reached after exactly 5 redirects
        FakeResponse down;
        down.status   = 307;
        down.location = QStringLiteral("http://example.com/plain.png");
        fake->respond(QStringLiteral("https://example.com/down.png"), down);
        FakeResponse inside;
        inside.status   = 308;
        inside.location = QStringLiteral("https://192.168.1.1/admin.png");
        fake->respond(QStringLiteral("https://example.com/inside.png"), inside);
        FakeResponse viaName;
        viaName.status   = 302;
        viaName.location = QStringLiteral("https://router.example.net/x.png");
        fake->respond(QStringLiteral("https://example.com/vianame.png"), viaName);
        fake->dns.insert(QStringLiteral("router.example.net"), {ip("10.0.0.1")});
        FakeResponse noLocation;
        noLocation.status = 302;
        fake->respond(QStringLiteral("https://example.com/nolocation.png"), noLocation);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const char* href) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QString::fromLatin1(href)), 1 << 20, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        const Result followed = load("https://example.com/cat.png");
        QVERIFY2(followed.ok, qPrintable(followed.error));
        QCOMPARE(followed.size, qint64(png.size()));
        const Result fiveHops = load("https://example.com/loop0.png");
        QVERIFY2(fiveHops.ok, qPrintable(fiveHops.error));
        for (const QString& name : filesIn(dir.path()))
            QFile::remove(dir.path() + QLatin1Char('/') + name); // no cached answers below
        FakeResponse more = hop;
        more.location     = QStringLiteral("https://example.com/loop6.png");
        fake->respond(QStringLiteral("https://example.com/loop5.png"), more); // now a sixth redirect
        fake->respond(QStringLiteral("https://example.com/loop6.png"), fine);
        QCOMPARE(load("https://example.com/loop1.png").ok, true); // 1 -> 6: five redirects
        QCOMPARE(load("https://example.com/loop0.png").error, QString::fromLatin1(errors::kRedirect)); // 0 -> 6: six
        QCOMPARE(load("https://example.com/down.png").error, QString::fromLatin1(errors::kRedirect));
        QCOMPARE(load("https://example.com/inside.png").error, QString::fromLatin1(errors::kRedirect));
        QCOMPARE(load("https://example.com/vianame.png").error, QString::fromLatin1(errors::kBlocked));
        QCOMPARE(load("https://example.com/nolocation.png").error, QString::fromLatin1(errors::kRedirect));
        for (const QString& url : fake->requests()) {
            QVERIFY2(!url.startsWith(QLatin1String("http:")) && !url.contains(QLatin1String("192.168")) && !url.contains(QLatin1String("router")),
                     qPrintable(url));
        }
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchBlockedAddresses()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  fine;
        fine.contentType = "image/png";
        fine.body        = pngBytes(4, 4);
        fake->dns.insert(QStringLiteral("private.example.com"), {ip("192.168.0.10")});
        fake->dns.insert(QStringLiteral("mixed.example.com"), {ip("93.184.216.34"), ip("127.0.0.1")});
        fake->dns.insert(QStringLiteral("mapped.example.com"), {ip("::ffff:10.1.2.3")});
        fake->dns.insert(QStringLiteral("cgnat.example.com"), {ip("100.64.1.1")});
        fake->dns.insert(QStringLiteral("v6local.example.com"), {ip("fd00::5")});
        fake->dns.insert(QStringLiteral("nat64.example.com"), {ip("64:ff9b::5db8:d822")}); // 93.184.216.34: fine
        fake->dns.insert(QStringLiteral("nothing.example.com"), {});
        for (const char* host : {"private", "mixed", "mapped", "cgnat", "v6local", "nat64", "nothing", "rebind", "sneaky", "peerless"})
            fake->respond(QStringLiteral("https://%1.example.com/a.png").arg(QString::fromLatin1(host)), fine);
        // DNS rebinding: public when asked, private when connected. Once with a transport that checks
        // (refused before sending), once with one that doesn't (the Fetcher refuses the response).
        FakeResponse rebind = fine;
        rebind.peer         = ip("10.9.8.7");
        fake->respond(QStringLiteral("https://rebind.example.com/a.png"), rebind);
        FakeResponse sneaky   = rebind;
        sneaky.ignoreAllowPeer = true;
        fake->respond(QStringLiteral("https://sneaky.example.com/a.png"), sneaky);
        FakeResponse peerless   = fine;
        peerless.unknownPeer     = true;
        fake->respond(QStringLiteral("https://peerless.example.com/a.png"), peerless);
        fake->dns.insert(QStringLiteral("peerless.example.com"), {ip("93.184.216.34")});

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const char* host) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QStringLiteral("https://%1.example.com/a.png").arg(QString::fromLatin1(host))), 1 << 20, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        for (const char* host : {"private", "mixed", "mapped", "cgnat", "v6local", "rebind", "sneaky", "peerless"})
            QVERIFY2(load(host).error == QLatin1String(errors::kBlocked), host);
        QCOMPARE(load("nothing").error, QString::fromLatin1(errors::kOffline));
        QVERIFY(load("nat64").ok);
        // The transport "connected" to the rebinding host but sent nothing.
        QVERIFY(fake->refusedPeers.contains(QStringLiteral("https://rebind.example.com/a.png")));
        const QStringList sent = fake->requests();
        for (const char* host : {"private", "mixed", "mapped", "cgnat", "v6local", "rebind"})
            QVERIFY2(!sent.join(QLatin1Char(' ')).contains(QString::fromLatin1(host)), host);
        // The sneaky one got its request through but no byte of its body was used.
        QVERIFY(!QFile::exists(dir.path() + QLatin1Char('/') + linkpreview::classify(QStringLiteral("https://sneaky.example.com/a.png")).key + QStringLiteral(".png")));
        QVERIFY(!anyPart(dir.path()));
    }

    // The Windows proxy's lists (netpolicy.h): which proxy an https request takes, which hosts go direct.
    void windowsProxyLists()
    {
        const QString yt = QStringLiteral("www.youtube.com");
        QCOMPARE(net::proxyForHost(QStringLiteral("127.0.0.1:10809"), QString(), yt), QStringLiteral("127.0.0.1:10809"));
        QCOMPARE(net::proxyForHost(QStringLiteral("http=127.0.0.1:10809;https=127.0.0.1:10810"), QString(), yt), QStringLiteral("127.0.0.1:10810"));
        QCOMPARE(net::proxyForHost(QStringLiteral("https=a.example.net:1 b.example.net:2"), QString(), yt), QStringLiteral("a.example.net:1"));
        QCOMPARE(net::proxyForHost(QStringLiteral("a.example.net:1;b.example.net:2"), QString(), yt), QStringLiteral("a.example.net:1;b.example.net:2"));
        QCOMPARE(net::proxyForHost(QStringLiteral("http://proxy.example.com:8080/"), QString(), yt), QStringLiteral("proxy.example.com:8080"));
        // Nothing an https request can use: direct.
        for (const char* none : {"", "http=127.0.0.1:10809", "socks=127.0.0.1:10808", "ftp=a.example.net:21", "socks5://127.0.0.1:1080",
                                 "user:pass@a.example.net:1", "127.0.0.1:10809\x01"})
            QVERIFY2(net::proxyForHost(QString::fromLatin1(none), QString(), yt).isEmpty(), none);
        // What v2rayN and similar tools set as the bypass list: YouTube still goes through the proxy.
        const QString v2ray = QStringLiteral("localhost;127.*;10.*;172.16.*;172.17.*;192.168.*;*.local;<local>");
        QCOMPARE(net::proxyForHost(QStringLiteral("127.0.0.1:10809"), v2ray, yt), QStringLiteral("127.0.0.1:10809"));
        QCOMPARE(net::proxyForHost(QStringLiteral("127.0.0.1:10809"), v2ray, QStringLiteral("i.ytimg.com")), QStringLiteral("127.0.0.1:10809"));
        QVERIFY(net::bypassesProxy(v2ray, QStringLiteral("printer.local")));
        QVERIFY(net::bypassesProxy(v2ray, QStringLiteral("intranet")));
        QVERIFY(!net::bypassesProxy(v2ray, QStringLiteral("cdn.example.com")));
        QVERIFY(net::bypassesProxy(QStringLiteral("*.YouTube.com"), yt));
        QVERIFY(net::bypassesProxy(QStringLiteral("https://*.example.com:443"), QStringLiteral("cdn.example.com")));
        QVERIFY(net::bypassesProxy(QStringLiteral("cdn.example.com"), QStringLiteral("cdn.example.com")));
        QVERIFY(!net::bypassesProxy(QStringLiteral("example.com"), QStringLiteral("cdn.example.com")));
        QVERIFY(!net::bypassesProxy(QStringLiteral("*.example.com"), QStringLiteral("example.net")));
        QVERIFY(net::proxyForHost(QStringLiteral("127.0.0.1:10809"), QStringLiteral("*.youtube.com"), yt).isEmpty());
        // An auto-config script (possibly found by WPAD on the local network) learns the host only.
        QCOMPARE(net::proxyQueryUrl(QUrl(QStringLiteral("https://CDN.example.com/a/b/cat.png?token=SECRET#frag"))).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://cdn.example.com/"));
        QCOMPARE(net::proxyQueryUrl(net::oEmbedUrl(kVideoId)).toString(QUrl::FullyEncoded), QStringLiteral("https://www.youtube.com/"));
        QVERIFY(!net::proxyQueryUrl(QUrl(QStringLiteral("https://10.0.0.1/a.png"))).isValid());
        QVERIFY(!net::proxyQueryUrl(QUrl(QStringLiteral("http://cdn.example.com/a.png"))).isValid());
    }

    // Through the user's Windows proxy: no local DNS check and no connected-address check (the proxy
    // resolves the name; filtered local DNS answers don't matter), every other rule still applies.
    void fetchThroughProxy()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  fine;
        fine.contentType = "image/png";
        fine.body        = pngBytes(4, 4);
        // Filtered local DNS (a private address for a public site), as behind a filtering DNS.
        fake->dns.insert(QStringLiteral("filtered.example.com"), {ip("10.20.30.40")});
        fake->dns.insert(QStringLiteral("www.youtube.com"), {ip("10.20.30.40")});
        fake->dns.insert(QStringLiteral("i.ytimg.com"), {ip("10.20.30.41")});
        fake->respond(QStringLiteral("https://filtered.example.com/a.png"), fine);
        FakeResponse down;
        down.status   = 302;
        down.location = QStringLiteral("http://example.com/plain.png");
        fake->respond(QStringLiteral("https://filtered.example.com/down.png"), down);
        FakeResponse inside;
        inside.status   = 302;
        inside.location = QStringLiteral("https://192.168.1.1/admin.png");
        fake->respond(QStringLiteral("https://filtered.example.com/inside.png"), inside);
        FakeResponse page;
        page.contentType = "text/html";
        page.body        = "<html><body>hello</body></html>";
        fake->respond(QStringLiteral("https://filtered.example.com/page.png"), page);
        FakeResponse big = fine;
        big.contentLength = 5LL * 1024 * 1024;
        fake->respond(QStringLiteral("https://filtered.example.com/big.png"), big);
        FakeResponse json;
        json.contentType = "application/json";
        json.body = QStringLiteral("{\"title\":\"A video\",\"author_name\":\"Someone\",\"thumbnail_url\":\"https://i.ytimg.com/vi/%1/hqdefault.jpg\"}")
                        .arg(kVideoId)
                        .toUtf8();
        fake->respond(oEmbedFor(kVideoId), json);
        FakeResponse thumb;
        thumb.contentType = "image/jpeg";
        thumb.body        = jpegBytes(480, 360);
        fake->respond(QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(kVideoId), thumb);

        Fetcher     fetcher(fake, testOptions());
        QStringList lines;
        fetcher.setLogger([&lines](const QString& line) { lines << line; });
        QSignalSpy  finished(&fetcher, &Fetcher::finished);
        const auto  load = [&](const QString& href, qint64 cap = 1 << 20) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(href), cap, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        // Without a proxy: the filtered answer is refused before anything connects.
        QCOMPARE(load(QStringLiteral("https://filtered.example.com/a.png")).error, QString::fromLatin1(errors::kBlocked));
        QVERIFY(fake->requests().isEmpty());
        QVERIFY(!lines.last().contains(QLatin1String("proxy")));

        // With one: through it (the peer is the proxy on this PC), no local DNS lookup.
        fake->viaProxy        = true;
        const int resolvedNow = fake->resolved.size();
        const Result viaProxy = load(QStringLiteral("https://filtered.example.com/a.png"));
        QVERIFY2(viaProxy.ok, qPrintable(viaProxy.error));
        QVERIFY(viaProxy.viaProxy);
        QCOMPARE(fake->proxied, QStringList{QStringLiteral("https://filtered.example.com/a.png")});
        QVERIFY(lines.last().contains(QLatin1String("via the Windows proxy")));
        // YouTube through the proxy: oEmbed, then the thumbnail.
        const Result video = load(QStringLiteral("http://www.youtube.com/watch?v=") + kVideoId);
        QVERIFY2(video.ok, qPrintable(video.error));
        QCOMPARE(video.title, QStringLiteral("A video"));
        QVERIFY(QFile::exists(video.thumbnailFile));
        QCOMPARE(fake->resolved.size(), resolvedNow);
        // Every other rule still holds: redirects, content checks, caps.
        QCOMPARE(load(QStringLiteral("https://filtered.example.com/down.png")).error, QString::fromLatin1(errors::kRedirect));
        QCOMPARE(load(QStringLiteral("https://filtered.example.com/inside.png")).error, QString::fromLatin1(errors::kRedirect));
        QCOMPARE(load(QStringLiteral("https://filtered.example.com/page.png")).error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(load(QStringLiteral("https://filtered.example.com/big.png")).error, QString::fromLatin1(errors::kTooLarge));
        for (const QString& url : fake->requests())
            QVERIFY2(!url.startsWith(QLatin1String("http:")) && !url.contains(QLatin1String("192.168")), qPrintable(url));
        // Addresses that are never fetched stay refused (classify), proxy or not.
        QVERIFY(!linkpreview::classify(QStringLiteral("https://10.20.30.40/a.png")).isValid());
        QVERIFY(!linkpreview::classify(QStringLiteral("https://router.lan/a.png")).isValid());
        QVERIFY(!anyPart(dir.path()));
    }

    void fetchContentChecks()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  html;
        html.contentType = "text/html; charset=utf-8";
        html.body        = QByteArray(100000, 'h');
        fake->respond(QStringLiteral("https://example.com/typed.png"), html);
        FakeResponse lying;
        lying.contentType = "image/png";
        lying.body        = "<!DOCTYPE html><html><body>" + QByteArray(100000, 'x');
        lying.chunk       = 1000;
        fake->respond(QStringLiteral("https://example.com/lying.png"), lying);
        FakeResponse svg;
        svg.contentType = "image/svg+xml";
        svg.body        = "<svg xmlns='http://www.w3.org/2000/svg'/>";
        fake->respond(QStringLiteral("https://example.com/vector.png"), svg);
        FakeResponse videoAsImage;
        videoAsImage.contentType = "application/octet-stream";
        videoAsImage.body        = mp4Bytes(500);
        fake->respond(QStringLiteral("https://example.com/notpicture.jpg"), videoAsImage);
        FakeResponse tiny;
        tiny.contentType = "image/png";
        tiny.body        = "\x89PN";
        fake->respond(QStringLiteral("https://example.com/tiny.png"), tiny);
        FakeResponse broken;
        broken.contentType = "image/png";
        broken.body        = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(200, '\x07'); // right magic, no picture
        fake->respond(QStringLiteral("https://example.com/broken.png"), broken);
        FakeResponse cut;
        cut.contentType   = "image/png";
        cut.body          = pngBytes(16, 16);
        cut.contentLength = cut.body.size() + 100; // the connection ends early
        fake->respond(QStringLiteral("https://example.com/cut.png"), cut);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const char* href) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QString::fromLatin1(href)), 1 << 20, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        int reads = fake->reads;
        QCOMPARE(load("https://example.com/typed.png").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(int(fake->reads), reads); // refused on its header: the body is never read
        reads = fake->reads;
        QCOMPARE(load("https://example.com/lying.png").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(int(fake->reads), reads + 1); // stopped after the first chunk
        QCOMPARE(load("https://example.com/vector.png").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(load("https://example.com/notpicture.jpg").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(load("https://example.com/tiny.png").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(load("https://example.com/broken.png").error, QString::fromLatin1(errors::kBadType));
        QCOMPARE(load("https://example.com/cut.png").error, QString::fromLatin1(errors::kBadResponse));
        QCOMPARE(filesIn(dir.path()), QStringList()); // nothing kept from any of them
    }

    void fetchCaps()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  announced;
        announced.contentType = "video/mp4";
        announced.body        = mp4Bytes(300000);
        fake->respond(QStringLiteral("https://example.com/big.mp4"), announced);
        FakeResponse unannounced  = announced;
        unannounced.contentLength = -1;
        unannounced.chunk         = 10000;
        fake->respond(QStringLiteral("https://example.com/stream.mp4"), unannounced);
        FakeResponse understated  = unannounced;
        understated.contentLength = 1000;
        fake->respond(QStringLiteral("https://example.com/liar.mp4"), understated);
        FakeResponse huge;
        huge.contentType = "image/png";
        huge.body        = pngHeaderOnly(10000, 9000); // 90 megapixels
        fake->respond(QStringLiteral("https://example.com/huge.png"), huge);
        FakeResponse justFits;
        justFits.contentType = "video/mp4";
        justFits.body        = mp4Bytes(2048);
        fake->respond(QStringLiteral("https://example.com/fits.mp4"), justFits);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const char* href, qint64 maxBytes) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QString::fromLatin1(href)), maxBytes, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        int          reads = fake->reads;
        const Result big   = load("https://example.com/big.mp4", 100000);
        QCOMPARE(big.error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(big.size, qint64(300000)); // "Too large (300 KB) - Open in browser"
        QCOMPARE(int(fake->reads), reads);  // decided on the header alone
        const Result stream = load("https://example.com/stream.mp4", 100000);
        QCOMPARE(stream.error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(stream.size, qint64(-1));
        QVERIFY(int(fake->reads) - reads <= 11); // stopped right after the cap
        const Result liar = load("https://example.com/liar.mp4", 100000);
        QCOMPARE(liar.error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(liar.size, qint64(-1));
        QCOMPARE(load("https://example.com/huge.png", 1 << 20).error, QString::fromLatin1(errors::kTooManyPixels));
        QVERIFY(load("https://example.com/fits.mp4", 2048).ok); // exactly the cap
        QCOMPARE(load("https://example.com/fits.mp4", 0).ok, true); // 0: the default cap (cached now)
        QVERIFY(!anyPart(dir.path()));
        QVERIFY(!QFile::exists(dir.path() + QLatin1Char('/') + linkpreview::classify(QStringLiteral("https://example.com/huge.png")).key + QStringLiteral(".png")));
    }

    // 2.4 auto-load: an automatic load asks for at most the automatic limit (autoload::autoCap). A file announced
    // larger stops before its body (the card says its size), one that doesn't say right after the limit; nothing of
    // it is left.
    void autoCapStopsBeforeTheBody()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  announced;
        announced.contentType = "video/mp4";
        announced.body        = mp4Bytes(3 * 1024 * 1024);
        announced.chunk       = 64 * 1024;
        fake->respond(QStringLiteral("https://example.com/match.mp4"), announced);
        FakeResponse unannounced  = announced;
        unannounced.contentLength = -1;
        fake->respond(QStringLiteral("https://example.com/live.mp4"), unannounced);

        Settings s;
        s.videoAutoDownloadMB = 1;
        const qint64 cap      = linkpreview::autoload::autoCap(Kind::Video, s, QString());
        QCOMPARE(cap, qint64(1024 * 1024));
        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        const auto load = [&](const QString& href) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(href), cap, dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        };
        const int    reads = fake->reads;
        const Result big   = load(QStringLiteral("https://example.com/match.mp4"));
        QCOMPARE(big.error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(big.size, qint64(3 * 1024 * 1024)); // "Video · 3.0 MB" + Load preview
        QCOMPARE(int(fake->reads), reads);            // not one byte of the body
        const Result live = load(QStringLiteral("https://example.com/live.mp4"));
        QCOMPARE(live.error, QString::fromLatin1(errors::kTooLarge));
        QCOMPARE(live.size, qint64(-1));
        QVERIFY(int(fake->reads) - reads <= cap / announced.chunk + 1); // stopped right after the limit
        QCOMPARE(filesIn(dir.path()), QStringList());
        // A click then loads it with the click cap.
        const Result clicked = [&] {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/match.mp4")), linkpreview::autoload::clickCap(Kind::Video), dir.path());
            if (!finished.wait(10000) && finished.count() == before)
                return Result();
            return resultOf(finished, finished.count() - 1);
        }();
        QVERIFY2(clicked.ok, qPrintable(clicked.error));
    }

    // 2.4 auto-load: the sender pipeline's Fetcher and the chat's load into the same folder (cache/links); the same
    // link may run in both at once. Their part files are never one file, both finish, and one complete copy stays.
    void twoFetchersOneFolder()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  slow;
        slow.contentType    = "video/mp4";
        slow.body           = mp4Bytes(200000);
        slow.chunk          = 20000;
        slow.holdAfterBytes = 60000;
        slow.hold           = std::make_shared<std::atomic<bool>>(true);
        fake->respond(QStringLiteral("https://example.com/both.mp4"), slow);
        const Target target = linkpreview::classify(QStringLiteral("https://example.com/both.mp4"));

        Fetcher    sender(fake, testOptions()); // the pipeline's
        Fetcher    chat(fake, testOptions());   // the chat's
        QSignalSpy sent(&sender, &Fetcher::finished);
        QSignalSpy shown(&chat, &Fetcher::finished);
        sender.load(target, 25 << 20, dir.path());
        chat.load(target, 50 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(int(fake->holds), 2, 10000); // both mid-body
        QStringList parts;
        for (const QString& name : filesIn(dir.path())) {
            if (name.endsWith(QLatin1String(".part")))
                parts << name;
        }
        QCOMPARE(parts.size(), 2); // two part files, not one shared by both
        QVERIFY(parts.at(0) != parts.at(1));
        for (const QString& part : qAsConst(parts))
            QVERIFY(part.startsWith(target.key + QLatin1Char('.')));
        slow.hold->store(false);
        QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(shown.count(), 1, 10000);
        QVERIFY2(resultOf(sent).ok, qPrintable(resultOf(sent).error));
        QVERIFY2(resultOf(shown).ok, qPrintable(resultOf(shown).error));
        QCOMPARE(fake->requestCount(), 2);
        QTRY_COMPARE(sender.runningThreads() + chat.runningThreads(), 0);
        QCOMPARE(filesIn(dir.path()), QStringList({target.key + QStringLiteral(".json"), target.key + QStringLiteral(".mp4")}));
        QFile file(dir.path() + QLatin1Char('/') + target.key + QStringLiteral(".mp4"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), slow.body); // whole, not two downloads mixed into one file
        file.close();
        Result cached;
        QVERIFY(Fetcher::cached(target, dir.path(), &cached));
        QVERIFY(cached.ok);
        QCOMPARE(cached.size, qint64(slow.body.size()));
        // A third load (the other side again) is answered from the folder: no request.
        chat.load(target, 50 << 20, dir.path());
        QTRY_COMPARE(shown.count(), 2);
        QVERIFY(resultOf(shown, 1).ok);
        QCOMPARE(fake->requestCount(), 2);
    }

    // 2.4 auto-load, race R9: the chat's load of a link is canceled because the sender pipeline's download of it came
    // first, while the chat's job is past putting its file in place (probing a video takes a while). That file is the
    // one our chat just adopted and the pipeline's maker copies: a cancel keeps a complete download.
    void cancelKeepsAPlacedFile()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  video;
        video.contentType = "video/mp4";
        video.body        = mp4Bytes(120000);
        fake->respond(QStringLiteral("https://example.com/race.mp4"), video);
        const Target target = linkpreview::classify(QStringLiteral("https://example.com/race.mp4"));

        // Held right after the file is in place, until the test has canceled the load.
        auto             placed  = std::make_shared<std::atomic<bool>>(false);
        auto             go      = std::make_shared<std::atomic<bool>>(false);
        Fetcher::Options options = testOptions();
        options.placed           = [placed, go](const QString&) {
            placed->store(true);
            QElapsedTimer clock;
            clock.start();
            while (!go->load() && clock.elapsed() < 10000)
                QThread::msleep(5);
        };
        Fetcher    chat(fake, options);
        QSignalSpy finished(&chat, &Fetcher::finished);
        chat.load(target, 50 << 20, dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(placed->load(), 10000);
        const QString file = dir.path() + QLatin1Char('/') + target.key + QStringLiteral(".mp4");
        QVERIFY(QFileInfo::exists(file));
        chat.cancel(target.key);
        go->store(true);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(resultOf(finished).error, QString::fromLatin1(errors::kCanceled));
        QTRY_COMPARE(chat.runningThreads(), 0);
        QVERIFY(QFileInfo::exists(file)); // the complete download stays (the cache cap keeps the folder in bounds)
        QFile kept(file);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), video.body);
        kept.close();
        QVERIFY(!anyPart(dir.path()));

        // Bytes that turn out bad are still removed (only a file this job put there).
        FakeResponse bad;
        bad.contentType = "image/png";
        bad.body        = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(4000, 'x'); // a PNG signature, no picture
        fake->respond(QStringLiteral("https://example.com/broken.png"), bad);
        go->store(true);
        Fetcher    plain(fake, testOptions());
        QSignalSpy plainDone(&plain, &Fetcher::finished);
        const Target broken = linkpreview::classify(QStringLiteral("https://example.com/broken.png"));
        plain.load(broken, 1 << 20, dir.path());
        QTRY_COMPARE(plainDone.count(), 1);
        QVERIFY(!resultOf(plainDone).ok);
        QTRY_COMPARE(plain.runningThreads(), 0);
        QVERIFY(!QFileInfo::exists(dir.path() + QLatin1Char('/') + broken.key + QStringLiteral(".png")));
    }

    // 2.4 auto-load (C4): a link loaded before (by a click, an automatic load or our own sender pipeline, in an
    // earlier session) shows from cache/links with no network at all, as ChatLinkPreviews::tryCache does it:
    // Fetcher::cached, then the click path's adoption (Core::adoptLocalFile with linkstate::webLink).
    void cachedLinkAdoptsWithoutNetwork()
    {
        const TS3Functions savedFuncs = ts3::funcs;
        const Settings     saved      = Settings::instance();
        QTemporaryDir      config;
        QVERIFY(config.isValid());
        fakets3::install(config.path());
        Settings::instance() = Settings();
        {
            auto core = std::make_unique<Core>();
            core->start();
            const QString links = linksCacheDir(core.get());
            QCOMPARE(links, core->cacheDir() + QStringLiteral("/links"));
            QVERIFY(QDir().mkpath(links));
            const QByteArray png   = pngBytes(64, 48);
            const Target     image = linkpreview::classify(QStringLiteral("https://cdn.example.com/cat.png"));
            const Target     video = linkpreview::classify(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"));
            {
                // The earlier session: the website answers once.
                auto         site = std::make_shared<FakeTransport>();
                FakeResponse picture;
                picture.contentType = "image/png";
                picture.body        = png;
                site->respond(QStringLiteral("https://cdn.example.com/cat.png"), picture);
                FakeResponse json;
                json.contentType = "application/json";
                json.body        = "{\"type\":\"video\",\"title\":\"Never Gonna\",\"author_name\":\"Rick Astley\",\"thumbnail_url\":\"https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"}";
                site->respond(oEmbedFor(kVideoId), json);
                FakeResponse thumb;
                thumb.contentType = "image/jpeg";
                thumb.body        = jpegBytes(480, 360);
                site->respond(QStringLiteral("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"), thumb);
                Fetcher    earlier(site, testOptions());
                QSignalSpy finished(&earlier, &Fetcher::finished);
                earlier.load(image, linkpreview::autoload::clickCap(Kind::Image), links);
                earlier.load(video, linkpreview::autoload::clickCap(Kind::YouTube), links);
                QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
                QVERIFY(resultOf(finished, 0).ok && resultOf(finished, 1).ok);
                QCOMPARE(site->requestCount(), 3);
            }

            // This session: a transport nobody may use.
            auto    offline = std::make_shared<FakeTransport>();
            Fetcher now(offline, testOptions());
            Result  cached;
            QVERIFY(Fetcher::cached(image, links, &cached));
            QVERIFY(cached.ok);
            QCOMPARE(cached.size, qint64(png.size()));
            const MediaLink link = linkstate::webLink(image, cached.mime, QFileInfo(cached.localFile).size(), cached.width, cached.height, 0);
            const QString   key  = core->adoptLocalFile(link, cached.localFile);
            QVERIFY(!key.isEmpty());
            QVERIFY(core->isLocalFile(key));
            QCOMPARE(core->entry(key)->state, MediaState::Ready);
            QCOMPARE(core->entry(key)->localPath, cached.localFile);
            Result tube;
            QVERIFY(Fetcher::cached(video, links, &tube));
            QCOMPARE(tube.title, QStringLiteral("Never Gonna"));
            QCOMPARE(tube.author, QStringLiteral("Rick Astley"));
            QVERIFY(QFileInfo(tube.thumbnailFile).isFile()); // the YouTube card from the local thumbnail
            // A load of either (a click with the switch off) is answered from the folder too.
            QSignalSpy finished(&now, &Fetcher::finished);
            now.load(image, linkpreview::autoload::clickCap(Kind::Image), links);
            now.load(video, linkpreview::autoload::clickCap(Kind::YouTube), links);
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
            QVERIFY(resultOf(finished, 0).ok && resultOf(finished, 1).ok);
            QCOMPARE(offline->requestCount(), 0);
            QVERIFY(offline->resolved.isEmpty());
            QVERIFY(fakets3::downloads().isEmpty()); // nor the TeamSpeak server
            // Clear cache: nothing to show from any more.
            core->clearCache();
            QVERIFY(!Fetcher::cached(image, links, &cached));
            QVERIFY(!Fetcher::cached(video, links, &tube));
        }
        ts3::funcs           = savedFuncs;
        Settings::instance() = saved;
    }

    void fetchStatusCodes()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        const QList<QPair<int, const char*>> codes = {{404, errors::kNotFound}, {410, errors::kNotFound},      {401, errors::kForbidden},
                                                      {403, errors::kForbidden}, {429, errors::kRateLimited}, {500, errors::kServerError},
                                                      {503, errors::kServerError}, {204, errors::kBadResponse}, {206, errors::kBadResponse},
                                                      {304, errors::kBadResponse}, {100, errors::kBadResponse}};
        for (const auto& code : codes) {
            FakeResponse response;
            response.status      = code.first;
            response.contentType = "image/png";
            response.body        = pngBytes(2, 2);
            fake->respond(QStringLiteral("https://example.com/s%1.png").arg(code.first), response);
        }
        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        for (const auto& code : codes) {
            const int before = finished.count();
            fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/s%1.png").arg(code.first)), 1 << 20, dir.path());
            QVERIFY(finished.wait(10000) || finished.count() > before);
            QCOMPARE(resultOf(finished, finished.count() - 1).error, QString::fromLatin1(code.second));
        }
        QCOMPARE(filesIn(dir.path()), QStringList());
    }

    void notPreviewableAndDisabled()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        Fetcher       fetcher(fake, testOptions());
        QSignalSpy    finished(&fetcher, &Fetcher::finished);
        Target        bogus;
        bogus.kind = Kind::Image;
        bogus.key  = QString(kKeyChars, QLatin1Char('a'));
        bogus.url  = QUrl(QStringLiteral("https://127.0.0.1/a.png"));
        fetcher.load(bogus, 0, dir.path());
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/a.txt")), 0, dir.path());
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/a.png")), 0, QString());
        QCOMPARE(finished.count(), 0);
        QTRY_COMPARE(finished.count(), 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(resultOf(finished, i).error, QString::fromLatin1(errors::kNotPreviewable));
        fetcher.setEnabled(false);
        QVERIFY(!fetcher.isEnabled());
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/a.png")), 0, dir.path());
        QTRY_COMPARE(finished.count(), 4);
        QCOMPARE(resultOf(finished, 3).error, QString::fromLatin1(errors::kDisabled));
        QCOMPARE(fake->requestCount(), 0);
        QVERIFY(fake->resolved.isEmpty());
    }

    void cancelMidDownload()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  slow;
        slow.contentType     = "video/mp4";
        slow.body            = mp4Bytes(400000);
        slow.chunk           = 50000;
        slow.stallAfterBytes = 100000; // then nothing more until canceled
        fake->respond(QStringLiteral("https://example.com/slow.mp4"), slow);

        Fetcher      fetcher(fake, testOptions());
        QSignalSpy   finished(&fetcher, &Fetcher::finished);
        QSignalSpy   progress(&fetcher, &Fetcher::progress);
        const Target target = linkpreview::classify(QStringLiteral("https://example.com/slow.mp4"));
        fetcher.load(target, 1 << 20, dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(fake->stalls > 0, 10000);
        QTRY_VERIFY(progress.count() > 0);
        QVERIFY(progress.last().at(1).toLongLong() > 0);
        QCOMPARE(progress.last().at(2).toLongLong(), qint64(400000));
        QVERIFY(anyPart(dir.path())); // the partial download

        QElapsedTimer clock;
        clock.start();
        fetcher.cancel(target.key);
        QVERIFY(!fetcher.isLoading(target.key));
        QCOMPARE(finished.count(), 0);
        QVERIFY(finished.wait(5000));
        QVERIFY(clock.elapsed() < 2000);
        QCOMPARE(resultOf(finished).error, QString::fromLatin1(errors::kCanceled));
        QTRY_COMPARE(fetcher.runningThreads(), 0); // the worker wound down at once
        QTRY_VERIFY(!anyPart(dir.path()));
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1); // only the cancel's answer, not the worker's

        // Loading it again works (a new fetch).
        FakeResponse quick = slow;
        quick.stallAfterBytes = -1;
        fake->respond(QStringLiteral("https://example.com/slow.mp4"), quick);
        fetcher.load(target, 1 << 20, dir.path());
        QVERIFY(finished.wait(10000));
        QVERIFY2(resultOf(finished, 1).ok, qPrintable(resultOf(finished, 1).error));
        QCOMPARE(fake->requestCount(), 2);
        fetcher.cancel(target.key); // not loading: nothing happens
        QTest::qWait(50);
        QCOMPARE(finished.count(), 2);
    }

    void shutdownDuringStall()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  stuckHead;
        stuckHead.stallHead = true;
        fake->respond(QStringLiteral("https://example.com/stuck.png"), stuckHead);
        FakeResponse stuckBody;
        stuckBody.contentType     = "image/png";
        stuckBody.body            = pngBytes(32, 32);
        stuckBody.stallAfterBytes = 20;
        fake->respond(QStringLiteral("https://example.com/half.png"), stuckBody);
        FakeResponse queued;
        queued.contentType = "image/png";
        queued.body        = pngBytes(4, 4);
        for (int i = 0; i < 4; ++i)
            fake->respond(QStringLiteral("https://example.com/q%1.png").arg(i), queued);

        Fetcher::Options options = testOptions();
        options.maxConcurrent    = 2;
        auto*      fetcher       = new Fetcher(fake, options);
        QSignalSpy finished(fetcher, &Fetcher::finished);
        QSignalSpy progress(fetcher, &Fetcher::progress);
        fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/stuck.png")), 1 << 20, dir.path());
        fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/half.png")), 1 << 20, dir.path());
        for (int i = 0; i < 4; ++i)
            fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/q%1.png").arg(i)), 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(int(fake->stalls), 2, 10000);
        QCOMPARE(fetcher->runningThreads(), 2);
        QVERIFY(fetcher->isLoading(linkpreview::classify(QStringLiteral("https://example.com/q3.png")).key)); // queued

        QElapsedTimer clock;
        clock.start();
        fetcher->shutdown();
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(fetcher->runningThreads(), 0);
        QVERIFY(!fetcher->isLoading(linkpreview::classify(QStringLiteral("https://example.com/q3.png")).key));
        const int progressBefore = progress.count();
        QTest::qWait(300);
        QCOMPARE(finished.count(), 0); // nothing after shutdown()
        QCOMPARE(progress.count(), progressBefore);
        QCOMPARE(fake->requestCount(), 2); // the queued ones never went out
        QVERIFY(!anyPart(dir.path()));

        // Disabled until turned on again; then it works.
        fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/q0.png")), 1 << 20, dir.path());
        QVERIFY(finished.wait(5000));
        QCOMPARE(resultOf(finished).error, QString::fromLatin1(errors::kDisabled));
        fetcher->setEnabled(true);
        fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/q0.png")), 1 << 20, dir.path());
        QVERIFY(finished.wait(10000));
        QVERIFY(resultOf(finished, 1).ok);

        // Destroying it while a load stalls is just as quick.
        fetcher->load(linkpreview::classify(QStringLiteral("https://example.com/stuck.png")), 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(int(fake->stalls), 3, 10000);
        clock.restart();
        delete fetcher;
        QVERIFY(clock.elapsed() < 1500);
        QTest::qWait(100); // nothing queued for the deleted object may run
    }

    void dedupeAndQueue()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  slow;
        slow.contentType     = "image/png";
        slow.body            = pngBytes(8, 8);
        slow.stallAfterBytes = 10;
        for (int i = 0; i < 5; ++i)
            fake->respond(QStringLiteral("https://example.com/p%1.png").arg(i), slow);

        Fetcher::Options options = testOptions();
        options.maxConcurrent    = 3;
        options.maxQueued        = 1;
        Fetcher    fetcher(fake, options);
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        QVector<Target> targets;
        for (int i = 0; i < 5; ++i)
            targets << linkpreview::classify(QStringLiteral("https://example.com/p%1.png").arg(i));
        fetcher.load(targets[0], 1 << 20, dir.path());
        fetcher.load(targets[0], 1 << 20, dir.path()); // already loading: not fetched twice
        for (int i = 1; i < 5; ++i)
            fetcher.load(targets[i], 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(int(fake->stalls), 3, 10000);
        QCOMPARE(fetcher.runningThreads(), 3);
        QVERIFY(fetcher.isLoading(targets[3].key)); // queued
        QTRY_COMPARE(finished.count(), 1);           // the fifth: the queue is full
        QCOMPARE(finished.at(0).at(0).toString(), targets[4].key);
        QCOMPARE(resultOf(finished).error, QString::fromLatin1(errors::kBusy));

        fetcher.cancel(targets[3].key); // a queued one
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(resultOf(finished, 1).error, QString::fromLatin1(errors::kCanceled));
        QCOMPARE(fake->requestCount(), 3);

        FakeResponse quick = slow;
        quick.stallAfterBytes = -1;
        fake->respond(QStringLiteral("https://example.com/p4.png"), quick);
        fetcher.load(targets[4], 1 << 20, dir.path()); // queued again (room now)
        fetcher.cancel(targets[0].key);                // a running one: frees a thread for the queue
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 10000);
        QHash<QString, QString> outcome;
        for (const QList<QVariant>& args : finished)
            outcome.insert(args.at(0).toString(), args.at(1).value<Result>().error);
        QCOMPARE(outcome.value(targets[0].key), QString::fromLatin1(errors::kCanceled));
        QCOMPARE(outcome.value(targets[4].key), QString());
        QCOMPARE(fake->requests().count(QUrl(QStringLiteral("https://example.com/p0.png")).toString(QUrl::FullyEncoded)), 1);
        fetcher.shutdown();
    }

    // Exactly one fetch per load(), every network call accounted for: per hop one route, one name lookup (none
    // through the proxy) and one request; YouTube is its oEmbed data, then its thumbnail. Nothing is retried or
    // fetched later, and a cached link, a load already running or a disabled Fetcher costs no network at all.
    void exactlyOneFetchPerLoad()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  png;
        png.contentType = "image/png";
        png.body        = pngBytes(8, 8);
        for (const char* name : {"one", "two", "three", "four"})
            fake->respond(QStringLiteral("https://example.com/%1.png").arg(QString::fromLatin1(name)), png);
        FakeResponse hop;
        hop.status   = 302;
        hop.location = QStringLiteral("/two.png");
        fake->respond(QStringLiteral("https://example.com/hop.png"), hop);
        FakeResponse slow    = png;
        slow.stallAfterBytes = 10;
        fake->respond(QStringLiteral("https://example.com/slow.png"), slow);
        FakeResponse json;
        json.contentType = "application/json";
        json.body        = QStringLiteral("{\"title\":\"T\",\"thumbnail_url\":\"https://i.ytimg.com/vi/%1/hqdefault.jpg\"}").arg(kVideoId).toUtf8();
        fake->respond(oEmbedFor(kVideoId), json);
        FakeResponse thumb;
        thumb.contentType = "image/jpeg";
        thumb.body        = jpegBytes(480, 360);
        const QString thumbUrl = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(kVideoId);
        fake->respond(thumbUrl, thumb);

        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        struct Calls {
            int routes = 0, lookups = 0, requests = 0;
        };
        const auto calls = [&fake] {
            QMutexLocker lock(&fake->mutex);
            return Calls{fake->routed.size(), fake->resolved.size(), fake->requested.size() + fake->refusedPeers.size()};
        };
        // One load; then everything it cost (and that nothing more follows while the event loop runs).
        const auto costOf = [&](const QString& href, Result* out = nullptr) {
            const Calls before = calls();
            const int   count  = finished.count();
            fetcher.load(linkpreview::classify(href), 1 << 20, dir.path());
            if (!finished.wait(10000) && finished.count() == count)
                return Calls{-1, -1, -1};
            QTest::qWait(150);
            if (out)
                *out = resultOf(finished, finished.count() - 1);
            const Calls after = calls();
            return Calls{after.routes - before.routes, after.lookups - before.lookups, after.requests - before.requests};
        };
        const auto check = [](const Calls& c, int routes, int lookups, int requests) {
            return c.routes == routes && c.lookups == lookups && c.requests == requests;
        };
        Result r;
        QVERIFY(check(costOf(QStringLiteral("https://example.com/one.png"), &r), 1, 1, 1));
        QVERIFY(r.ok);
        QCOMPARE(fake->requests().last(), QStringLiteral("https://example.com/one.png"));
        QVERIFY(check(costOf(QStringLiteral("https://example.com/one.png"), &r), 0, 0, 0)); // cached
        QVERIFY(r.ok);
        QVERIFY(check(costOf(QStringLiteral("https://example.com/hop.png"), &r), 2, 2, 2)); // one redirect
        QVERIFY(r.ok);
        QVERIFY(check(costOf(QStringLiteral("https://example.com/gone.png"), &r), 1, 1, 1)); // 404: no retry
        QCOMPARE(r.error, QString::fromLatin1(errors::kNotFound));
        const int before = fake->requestCount();
        QVERIFY(check(costOf(QStringLiteral("http://www.youtube.com/watch?v=") + kVideoId, &r), 2, 2, 2));
        QVERIFY(r.ok);
        QCOMPARE(fake->requests().mid(before), QStringList({oEmbedFor(kVideoId), thumbUrl}));
        QVERIFY(check(costOf(QStringLiteral("https://youtu.be/") + kVideoId, &r), 0, 0, 0)); // the same video, cached
        QVERIFY(r.ok);

        // The same link asked for twice while it loads: one fetch, one answer.
        Calls start = calls();
        const Target three = linkpreview::classify(QStringLiteral("https://example.com/three.png"));
        int answers = finished.count();
        fetcher.load(three, 1 << 20, dir.path());
        fetcher.load(three, 1 << 20, dir.path());
        QVERIFY(finished.wait(10000));
        QTest::qWait(150);
        QCOMPARE(finished.count(), answers + 1);
        QVERIFY(check(Calls{calls().routes - start.routes, calls().lookups - start.lookups, calls().requests - start.requests}, 1, 1, 1));

        // Canceled while it downloads: nothing is fetched again.
        start   = calls();
        answers = finished.count();
        const Target slowTarget = linkpreview::classify(QStringLiteral("https://example.com/slow.png"));
        fetcher.load(slowTarget, 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(int(fake->stalls), 1, 10000);
        fetcher.cancel(slowTarget.key);
        QTRY_COMPARE(finished.count(), answers + 1);
        QTest::qWait(150);
        QVERIFY(check(Calls{calls().routes - start.routes, calls().lookups - start.lookups, calls().requests - start.requests}, 1, 1, 1));

        // Through the Windows proxy: no local name lookup.
        fake->viaProxy = true;
        QVERIFY(check(costOf(QStringLiteral("https://example.com/four.png"), &r), 1, 0, 1));
        QVERIFY(r.ok);

        // Switched off: nothing at all.
        fetcher.setEnabled(false);
        start = calls();
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/two.png")), 1 << 20, dir.path());
        QTRY_COMPARE(resultOf(finished, finished.count() - 1).error, QString::fromLatin1(errors::kDisabled));
        QTest::qWait(150);
        QVERIFY(check(Calls{calls().routes - start.routes, calls().lookups - start.lookups, calls().requests - start.requests}, 0, 0, 0));
    }

    void totalTimeout()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  stuck;
        stuck.contentType     = "image/png";
        stuck.body            = pngBytes(8, 8);
        stuck.stallAfterBytes = 10;
        fake->respond(QStringLiteral("https://example.com/stuck.png"), stuck);
        Fetcher::Options options = testOptions();
        options.totalTimeoutMs   = 400;
        Fetcher       fetcher(fake, options);
        QSignalSpy    finished(&fetcher, &Fetcher::finished);
        QElapsedTimer clock;
        clock.start();
        fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/stuck.png")), 1 << 20, dir.path());
        QVERIFY(finished.wait(5000));
        QCOMPARE(resultOf(finished).error, QString::fromLatin1(errors::kTimeout));
        QVERIFY(clock.elapsed() >= 350);
        QVERIFY(clock.elapsed() < 3000);
        QTRY_VERIFY(!anyPart(dir.path()));
    }

    void pruneCache()
    {
        QTemporaryDir dir;
        auto          fake = std::make_shared<FakeTransport>();
        FakeResponse  png;
        png.contentType = "image/png";
        png.body        = pngBytes(200, 200);
        for (int i = 0; i < 3; ++i)
            fake->respond(QStringLiteral("https://example.com/c%1.png").arg(i), png);
        Fetcher    fetcher(fake, testOptions());
        QSignalSpy finished(&fetcher, &Fetcher::finished);
        for (int i = 0; i < 3; ++i) {
            fetcher.load(linkpreview::classify(QStringLiteral("https://example.com/c%1.png").arg(i)), 1 << 20, dir.path());
            QVERIFY(finished.wait(10000));
            QTest::qWait(30); // distinct times
        }
        QFile other(dir.path() + QStringLiteral("/notes.txt"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("not ours");
        other.close();
        const auto bytesOf = [&dir](const QString& key) {
            qint64 bytes = 0;
            for (const QFileInfo& file : QDir(dir.path()).entryInfoList(QDir::Files)) {
                if (file.fileName().startsWith(key))
                    bytes += file.size();
            }
            return bytes;
        };
        const qint64 newest = bytesOf(linkpreview::classify(QStringLiteral("https://example.com/c1.png")).key)
                              + bytesOf(linkpreview::classify(QStringLiteral("https://example.com/c2.png")).key);
        Fetcher::pruneCache(dir.path(), newest); // room for the two newer ones
        Result cached;
        QVERIFY(!Fetcher::cached(linkpreview::classify(QStringLiteral("https://example.com/c0.png")), dir.path(), &cached)); // the oldest went
        QVERIFY(Fetcher::cached(linkpreview::classify(QStringLiteral("https://example.com/c2.png")), dir.path(), &cached));
        QVERIFY(QFile::exists(other.fileName()));
        Fetcher::pruneCache(dir.path(), 0);
        QCOMPARE(filesIn(dir.path()), QStringList{QStringLiteral("notes.txt")});
    }
};

TSMEDIA_REGISTER_TEST(TestLinkPreviewNet)

#include "tst_linkpreview_net.moc"
