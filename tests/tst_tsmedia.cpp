// Unit tests for the pure-logic parts of TS Media chat: the chat link format, the message composer,
// BlurHash, the formatting helpers, display names, error texts, the shared UI helpers (uiutil) and
// the settings checks (link for the note, upload folder).
// Built as target tsmedia_tests (see CMakeLists.txt).

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QUrl>
#include <QtTest>

#include "blurhash.h"
#include "medialink.h"
#include "settings.h"
#include "testregistry.h" // 2.2: test classes in other files
#include "uiutil.h"

// settings.cpp keeps its ini file in the plugin's data folder; these tests never load or save it.
namespace ts3 {
QString dataDir()
{
    return QDir::tempPath();
}
} // namespace ts3

namespace {

const QString kRefHash = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj"); // the example on blurha.sh

// A typical TS Media link without metadata (as parsed from chat).
const QString kBaseHref = QStringLiteral("ts3file://voice.example.org?port=9987&serverUID=uid&channel=3&path=%2Ftsmedia"
                                         "&filename=clip.mp4&isDir=0&size=100&fileDateTime=1700000000");

const QString kNote = QStringLiteral(" [COLOR=#72767d][I]— TS Media chat plugin required to view this in chat[/I][/COLOR]");

MediaLink sampleLink()
{
    MediaLink link;
    link.host        = QStringLiteral("voice.example.org");
    link.port        = 9987;
    link.serverUid   = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    link.channelId   = 12;
    link.path        = QStringLiteral("/tsmedia");
    link.fileName    = QStringLiteral("clip_41234.mp4");
    link.size        = 4812345;
    link.dateTime    = 1700000000;
    link.protocol    = MediaLink::kProtocol;
    link.width       = 1280;
    link.height      = 720;
    link.durationMs  = 10010;
    link.blurHash    = kRefHash;
    link.previewFile = QStringLiteral("/tsmedia/previews/clip_41234.jpg");
    return link;
}

template <typename T>
QString str(const T& value)
{
    return QVariant::fromValue(value).toString();
}

// Empty if both links carry the same data, else the first differing field.
QString difference(const MediaLink& a, const MediaLink& b)
{
#define TSM_COMPARE_FIELD(field)                                                               \
    if (!(a.field == b.field))                                                                 \
        return QStringLiteral(#field ": [%1] != [%2]").arg(str(a.field), str(b.field));
    TSM_COMPARE_FIELD(host)
    TSM_COMPARE_FIELD(port)
    TSM_COMPARE_FIELD(serverUid)
    TSM_COMPARE_FIELD(channelId)
    TSM_COMPARE_FIELD(path)
    TSM_COMPARE_FIELD(fileName)
    TSM_COMPARE_FIELD(size)
    TSM_COMPARE_FIELD(dateTime)
    TSM_COMPARE_FIELD(isDir)
    TSM_COMPARE_FIELD(protocol)
    TSM_COMPARE_FIELD(width)
    TSM_COMPARE_FIELD(height)
    TSM_COMPARE_FIELD(durationMs)
    TSM_COMPARE_FIELD(blurHash)
    TSM_COMPARE_FIELD(previewFile)
#undef TSM_COMPARE_FIELD
    return {};
}

int utf8Bytes(const QString& text)
{
    return text.toUtf8().size();
}

// The same deterministic test image as the Python port of the reference implementation used to
// produce the expected hashes below.
QImage synthImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, qRgb((x * 16) % 256, (y * 20) % 256, (x * y * 7) % 256));
    }
    return image;
}

bool closeTo(QRgb actual, int r, int g, int b, int tolerance)
{
    return qAbs(qRed(actual) - r) <= tolerance && qAbs(qGreen(actual) - g) <= tolerance && qAbs(qBlue(actual) - b) <= tolerance;
}

QString rgbText(QRgb c)
{
    return QStringLiteral("(%1, %2, %3)").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c));
}

} // namespace

class TestTsMedia : public QObject
{
    Q_OBJECT

  private slots:
    // MediaLink
    void linkRoundTrip_data();
    void linkRoundTrip();
    void toUrlOnlyWritesSetParams();
    void plainTeamSpeakLink();
    void plainLinkWithRawBase64Uid();
    void anyOrderUnknownParamsAndCase();
    void fileHrefAndEscapedAmpersands();
    void malformedPercentEncoding();
    void hostileParams_data();
    void hostileParams();
    void oversizedBlurHashDropped();
    void keyIgnoresMetadata();
    void keyMatchesV1();
    void previewLink();
    void findInComposedMessage();

    // composeChatMessage
    void composeNotice_data();
    void composeNotice();
    void composeWithoutNotice();
    void composeDownloadUrlSanitising_data();
    void composeDownloadUrlSanitising();
    void composeSizeRule();
    void composeSizeRuleWithUrl();
    void composeSizeRuleCountsBytes();
    void noticeColourContrast();

    // BlurHash
    void blurhashDecodeReference();
    void blurhashDecodePunch();
    void blurhashEncodeMatchesReference_data();
    void blurhashEncodeMatchesReference();
    void blurhashTwoColourRoundTrip();
    void blurhashTransparentIsBlack();
    void blurhashIsValid_data();
    void blurhashIsValid();
    void blurhashInvalidInput();

    // Helpers
    void formatDuration_data();
    void formatDuration();
    void formatSize_data();
    void formatSize();
    void formatProgressTexts();
    void formatTimeLeft_data();
    void formatTimeLeft();
    void kindForFileName_data();
    void kindForFileName();

    // Names and texts
    void displayFileName_data();
    void displayFileName();
    void displayNameFor_data();
    void displayNameFor();
    void errorTexts();

    // UI helpers
    void contrastRatio();
    void savedToText();

    // Settings
    void checkDownloadUrl_data();
    void checkDownloadUrl();
    void normalizeUploadDirectory_data();
    void normalizeUploadDirectory();
};

// ---- MediaLink ---------------------------------------------------------------------------------

void TestTsMedia::linkRoundTrip_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("serverUid");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("fileName");

    const QString uid = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    QTest::newRow("plain") << QStringLiteral("voice.example.org") << uid << QStringLiteral("/tsmedia") << QStringLiteral("photo.png");
    QTest::newRow("root dir") << QStringLiteral("127.0.0.1") << uid << QStringLiteral("/") << QStringLiteral("photo.png");
    QTest::newRow("spaces") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/my files") << QStringLiteral("my holiday photo 2024.jpg");
    QTest::newRow("greek") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("διακοπές 2024.jpg");
    QTest::newRow("cyrillic dir") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/медиа") << QStringLiteral("видео.mp4");
    QTest::newRow("emoji") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("😀 party time.gif");
    QTest::newRow("cjk") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("视频 剪辑.webm");
    QTest::newRow("brackets") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("[draft] report (v2).pdf");
    QTest::newRow("ampersand") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("tom & jerry&amp;.mp4");
    QTest::newRow("plus") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("c++ notes+more.txt");
    QTest::newRow("percent") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("100% done %20 %zz.png");
    QTest::newRow("query chars") << QStringLiteral("ts.example.com") << uid << QStringLiteral("/tsmedia") << QStringLiteral("what?#a=b;c,d.png");
    QTest::newRow("uid all base64 chars") << QStringLiteral("ts.example.com") << QStringLiteral("+/+/abcXYZ019==") << QStringLiteral("/tsmedia") << QStringLiteral("a.png");
}

void TestTsMedia::linkRoundTrip()
{
    QFETCH(QString, host);
    QFETCH(QString, serverUid);
    QFETCH(QString, path);
    QFETCH(QString, fileName);

    MediaLink link   = sampleLink();
    link.host        = host;
    link.serverUid   = serverUid;
    link.path        = path;
    link.fileName    = fileName;
    link.previewFile = (path == QLatin1String("/") ? QString() : path) + QStringLiteral("/previews/") + QFileInfo(fileName).completeBaseName() + QStringLiteral(".jpg");
    QVERIFY(link.isValid());

    const QString url = link.toUrl();
    QVERIFY2(!url.contains(QLatin1Char(' ')) && !url.contains(QLatin1Char('[')) && !url.contains(QLatin1Char(']')), qPrintable(url));
    QVERIFY2(!url.contains(QLatin1Char('#')) && url.count(QLatin1Char('?')) == 1, qPrintable(url));
    QCOMPARE(url.count(QLatin1Char('&')), 13); // ?port + 7 more TeamSpeak params + tsm, w, h, d, bh, pv

    const MediaLink parsed = MediaLink::parse(url);
    const QString   diff   = difference(parsed, link);
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
    QVERIFY(parsed.isTsMedia());
    QCOMPARE(parsed.key(), link.key());

    // The way it travels: BBCode in a chat message.
    const QList<MediaLink> found = MediaLink::findInMessage(QStringLiteral("look ") + link.toBBCode() + QStringLiteral(" nice"));
    QCOMPARE(found.size(), 1);
    const QString diff2 = difference(found.first(), link);
    QVERIFY2(diff2.isEmpty(), qPrintable(diff2));

    // The label is shown by clients without the plugin; brackets must not break the BBCode.
    const QString bb = link.toBBCode();
    QVERIFY(bb.endsWith(QStringLiteral("[/URL]")));
    QCOMPARE(bb.count(QLatin1Char('[')), 2);
    QCOMPARE(bb.count(QLatin1Char(']')), 2);
}

void TestTsMedia::toUrlOnlyWritesSetParams()
{
    MediaLink link = sampleLink();
    link.protocol  = 0;
    link.width = link.height = 0;
    link.durationMs          = 0;
    link.blurHash.clear();
    link.previewFile.clear();
    const QString plain = link.toUrl();
    for (const char* param : {"&tsm=", "&w=", "&h=", "&d=", "&bh=", "&pv="})
        QVERIFY2(!plain.contains(QLatin1String(param)), param);
    QCOMPARE(plain.count(QLatin1Char('&')), 7);

    link.protocol = 2;
    link.width    = 640; // without a height the size is meaningless
    const QString partial = link.toUrl();
    QVERIFY(partial.endsWith(QStringLiteral("&tsm=2")));
    QVERIFY(!partial.contains(QStringLiteral("&w=")));
}

void TestTsMedia::plainTeamSpeakLink()
{
    // As produced by dragging a file from TeamSpeak's own file browser into the chat.
    const QString href = QStringLiteral("ts3file://127.0.0.1?port=9987&serverUID=Wn5SbAbc%2B%2F9xQ0pRu7Zy3pCt%2BYs%3D&channel=1"
                                        "&path=%2Fphotos&filename=screenshot%201.png&isDir=0&size=48213&fileDateTime=1700000000");
    const MediaLink link = MediaLink::parse(href);
    QVERIFY(link.isValid());
    QVERIFY(!link.isTsMedia());
    QCOMPARE(link.protocol, 0);
    QCOMPARE(link.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(link.port, quint16(9987));
    QCOMPARE(link.serverUid, QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys="));
    QCOMPARE(link.channelId, quint64(1));
    QCOMPARE(link.path, QStringLiteral("/photos"));
    QCOMPARE(link.fileName, QStringLiteral("screenshot 1.png"));
    QCOMPARE(link.remoteFile(), QStringLiteral("/photos/screenshot 1.png"));
    QCOMPARE(link.size, quint64(48213));
    QCOMPARE(link.dateTime, qint64(1700000000));
    QCOMPARE(link.width, 0);
    QCOMPARE(link.height, 0);
    QCOMPARE(link.durationMs, qint64(0));
    QVERIFY(link.blurHash.isEmpty());
    QVERIFY(link.previewFile.isEmpty());
    QVERIFY(!link.previewLink().isValid());

    // Directories are not media.
    QVERIFY(!MediaLink::parse(QString(href).replace(QStringLiteral("isDir=0"), QStringLiteral("isDir=1"))).isValid());
    // Not a file link at all.
    QVERIFY(!MediaLink::parse(QStringLiteral("https://example.com/?serverUID=a&channel=1&filename=x.png")).isValid());
    QVERIFY(!MediaLink::parse(QStringLiteral("ts3file://host-without-query")).isValid());
}

void TestTsMedia::plainLinkWithRawBase64Uid()
{
    // '+' must stay '+' (it is not a space in TeamSpeak links), '/' and '=' survive unencoded too.
    const MediaLink link = MediaLink::parse(QStringLiteral("ts3file://h?port=1&serverUID=Wn5S+/9x=&channel=4&path=/&filename=a+b.png&isDir=0&size=1&fileDateTime=0"));
    QVERIFY(link.isValid());
    QCOMPARE(link.serverUid, QStringLiteral("Wn5S+/9x="));
    QCOMPARE(link.fileName, QStringLiteral("a+b.png"));
    QCOMPARE(link.path, QStringLiteral("/"));
}

void TestTsMedia::anyOrderUnknownParamsAndCase()
{
    const MediaLink link = MediaLink::parse(QStringLiteral("TS3FILE://Host.Example?bh=") + QString::fromLatin1(QUrl::toPercentEncoding(kRefHash))
                                            + QStringLiteral("&FILENAME=a.png&foo=bar&&size=10&tsm=2&channel=7&h=20&SERVERUID=x"
                                                             "&port=1&isDir=0&w=10&path=%2Fdir&future=1&pv=%2Fdir%2Fpreviews%2Fa.jpg&d="));
    QVERIFY(link.isValid());
    QCOMPARE(link.host, QStringLiteral("Host.Example"));
    QCOMPARE(link.serverUid, QStringLiteral("x"));
    QCOMPARE(link.channelId, quint64(7));
    QCOMPARE(link.remoteFile(), QStringLiteral("/dir/a.png"));
    QCOMPARE(link.size, quint64(10));
    QCOMPARE(link.protocol, 2);
    QCOMPARE(link.width, 10);
    QCOMPARE(link.height, 20);
    QCOMPARE(link.durationMs, qint64(0));
    QCOMPARE(link.blurHash, kRefHash);
    QCOMPARE(link.previewFile, QStringLiteral("/dir/previews/a.jpg"));
}

void TestTsMedia::fileHrefAndEscapedAmpersands()
{
    // QTextBrowser sometimes reports TeamSpeak's links as file:///ts3file/... hrefs.
    const MediaLink a = MediaLink::parse(QStringLiteral("file:///ts3file/voice.example.org?port=9987&amp;serverUID=uid&amp;channel=3"
                                                        "&amp;path=%2Ftsmedia&amp;filename=clip.mp4&amp;isDir=0&amp;size=100"
                                                        "&amp;fileDateTime=0&amp;tsm=2&amp;w=640&amp;h=360&amp;d=5000"));
    QVERIFY(a.isValid());
    QCOMPARE(a.host, QStringLiteral("voice.example.org"));
    QCOMPARE(a.remoteFile(), QStringLiteral("/tsmedia/clip.mp4"));
    QCOMPARE(a.protocol, 2);
    QCOMPARE(a.width, 640);
    QCOMPARE(a.height, 360);
    QCOMPARE(a.durationMs, qint64(5000));

    // A full v2 link in both escaped forms.
    const MediaLink link    = sampleLink();
    const QString   url     = link.toUrl();
    const QString   escaped = QString(url).replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    const QString   asFile  = QStringLiteral("file:///ts3file/") + escaped.mid(int(qstrlen("ts3file://")));
    for (const QString& href : {escaped, asFile}) {
        const QString diff = difference(MediaLink::parse(href), link);
        QVERIFY2(diff.isEmpty(), qPrintable(href + QStringLiteral(" -> ") + diff));
    }

    const QString bbcode = QStringLiteral("[url=") + escaped + QStringLiteral("]clip[/url]");
    QCOMPARE(MediaLink::findInMessage(bbcode).size(), 1);
}

void TestTsMedia::malformedPercentEncoding()
{
    // Hand-typed links: never crash, keep what can be understood.
    const MediaLink link = MediaLink::parse(QStringLiteral("ts3file://h?port=x&serverUID=u&channel=2&path=%2&filename=100%.png%&size=-1&fileDateTime=zz&isDir=0"));
    QVERIFY(link.isValid());
    QCOMPARE(link.port, quint16(0));
    QVERIFY(!link.fileName.isEmpty());
    QVERIFY(!link.fileName.contains(QLatin1Char('/')));
    QVERIFY(link.path.startsWith(QLatin1Char('/')));

    // A file name can never escape its directory.
    const MediaLink escape = MediaLink::parse(QStringLiteral("ts3file://h?serverUID=u&channel=2&path=%2F&filename=..%5C..%2Fevil.dll"));
    QCOMPARE(escape.fileName, QStringLiteral("evil.dll"));
}

void TestTsMedia::hostileParams_data()
{
    QTest::addColumn<QString>("params");
    QTest::addColumn<int>("protocol");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<qint64>("durationMs");
    QTest::addColumn<QString>("blurHash");
    QTest::addColumn<QString>("previewFile");

    const QString none;
    const QString pv = QStringLiteral("/tsmedia/previews/clip.jpg");
    // clang-format off
    QTest::newRow("valid")              << "&tsm=2&w=1920&h=1080&d=10000&bh=LEHV6nWB2yk8pyo0adR%2A.7kCMdnj&pv=%2Ftsmedia%2Fpreviews%2Fclip.jpg" << 2 << 1920 << 1080 << qint64(10000) << kRefHash << pv;
    QTest::newRow("huge w/h")           << "&tsm=2&w=99999999&h=88888888"                     << 2 << 16384 << 16384 << qint64(0) << none << none;
    QTest::newRow("w overflows int64")  << "&w=999999999999999999999999999&h=100"             << 0 << 16384 << 2048  << qint64(0) << none << none;
    QTest::newRow("extreme wide")       << "&w=10000&h=1"                                     << 0 << 10000 << 1250  << qint64(0) << none << none;
    QTest::newRow("extreme tall")       << "&w=3&h=4000"                                      << 0 << 500   << 4000  << qint64(0) << none << none;
    QTest::newRow("exactly 8:1")        << "&w=800&h=100"                                     << 0 << 800   << 100   << qint64(0) << none << none;
    QTest::newRow("negative w")         << "&w=-640&h=480"                                    << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("zero h")             << "&w=640&h=0"                                       << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("only w")             << "&w=640"                                           << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("garbage w")          << "&w=12px&h=10"                                     << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("float w")            << "&w=1.5e3&h=10"                                    << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("negative d")         << "&d=-5000"                                         << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("garbage d")          << "&d=1e9"                                           << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("huge d")             << "&d=99999999999999999999"                          << 0 << 0     << 0     << qint64(3600000000) << none << none;
    QTest::newRow("bogus bh")           << "&bh=hello%20world"                                << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("bh wrong length")    << "&bh=LEHV6nWB2yk8pyo0adR%2A.7kCMdn"                << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("bh bad char")        << "&bh=LEHV6nWB2yk8pyo0adR%2A.7kCMdn%21"             << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("bh script")          << "&bh=%3Cscript%3Ealert(1)%3C%2Fscript%3E"          << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv relative")        << "&pv=previews%2Fclip.jpg"                          << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv dotdot")          << "&pv=%2Ftsmedia%2F..%2F..%2Fsecret.jpg"            << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv dotdot raw")      << "&pv=/tsmedia/previews/../../x.jpg"                << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv dotdot backslash")<< "&pv=%5Ctsmedia%5C..%5Cx.jpg"                      << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv single dot")      << "&pv=%2Ftsmedia%2F.%2Fclip.jpg"                    << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv equals main")     << "&pv=%2Ftsmedia%2Fclip.mp4"                        << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv equals main 2")   << "&pv=%2F%2Ftsmedia%2F%2FCLIP.MP4"                  << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv directory")       << "&pv=%2Ftsmedia%2Fpreviews%2F"                     << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv root")            << "&pv=%2F"                                          << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv control char")    << "&pv=%2Ftsmedia%2Fa%0Ab.jpg"                       << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv too long")        << "&pv=%2F" + QString(2000, QLatin1Char('a'))        << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("pv backslashes")     << "&pv=%5Ctsmedia%5Cpreviews%5Cclip.jpg"             << 0 << 0     << 0     << qint64(0) << none << pv;
    QTest::newRow("pv double slashes")  << "&pv=%2F%2Ftsmedia%2Fpreviews%2F%2Fclip.jpg"       << 0 << 0     << 0     << qint64(0) << none << pv;
    QTest::newRow("tsm garbage")        << "&tsm=two"                                         << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("tsm negative")       << "&tsm=-2"                                          << 0 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("tsm future")         << "&tsm=7"                                           << 7 << 0     << 0     << qint64(0) << none << none;
    QTest::newRow("tsm v1")             << "&tsm=1"                                           << 1 << 0     << 0     << qint64(0) << none << none;
    // clang-format on
}

void TestTsMedia::hostileParams()
{
    QFETCH(QString, params);
    QFETCH(int, protocol);
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(qint64, durationMs);
    QFETCH(QString, blurHash);
    QFETCH(QString, previewFile);

    const MediaLink base = MediaLink::parse(kBaseHref);
    const MediaLink link = MediaLink::parse(kBaseHref + params);
    QVERIFY(link.isValid()); // bad metadata is dropped, never fatal
    QCOMPARE(link.key(), base.key());
    QCOMPARE(link.remoteFile(), QStringLiteral("/tsmedia/clip.mp4"));
    QCOMPARE(link.protocol, protocol);
    QCOMPARE(link.isTsMedia(), protocol >= 2);
    QCOMPARE(link.width, width);
    QCOMPARE(link.height, height);
    QCOMPARE(link.durationMs, durationMs);
    QCOMPARE(link.blurHash, blurHash);
    QCOMPARE(link.previewFile, previewFile);

    if (link.width > 0) {
        QVERIFY(link.width <= 16384 && link.height <= 16384);
        QVERIFY(link.width <= link.height * 8 && link.height <= link.width * 8);
    }
}

void TestTsMedia::oversizedBlurHashDropped()
{
    // Valid BlurHash, but 9x9 components = 166 characters > 120.
    const QString big = blurhash::encode(synthImage(16, 12), 9, 9);
    QCOMPARE(big.length(), 166);
    QVERIFY(blurhash::isValid(big));
    const MediaLink link = MediaLink::parse(kBaseHref + QStringLiteral("&bh=") + QString::fromLatin1(QUrl::toPercentEncoding(big)));
    QVERIFY(link.isValid());
    QVERIFY(link.blurHash.isEmpty());

    // 4x3 (what the plugin sends) and the 120 character boundary are fine.
    const QString medium = blurhash::encode(synthImage(16, 12), 8, 7); // 4 + 2 * 56 = 116
    QCOMPARE(medium.length(), 116);
    QCOMPARE(MediaLink::parse(kBaseHref + QStringLiteral("&bh=") + QString::fromLatin1(QUrl::toPercentEncoding(medium))).blurHash, medium);
}

void TestTsMedia::keyIgnoresMetadata()
{
    const MediaLink full  = sampleLink();
    MediaLink       plain = full;
    plain.protocol        = 0;
    plain.width = plain.height = 0;
    plain.durationMs           = 0;
    plain.blurHash.clear();
    plain.previewFile.clear();
    QCOMPARE(full.key(), plain.key());
    QCOMPARE(MediaLink::parse(full.toUrl()).key(), MediaLink::parse(plain.toUrl()).key());

    // The date and host are not part of the identity either (v1 behaviour), the size is.
    MediaLink other = full;
    other.dateTime += 60;
    other.host = QStringLiteral("other.example.org");
    QCOMPARE(other.key(), full.key());
    other.size += 1;
    QVERIFY(other.key() != full.key());
}

void TestTsMedia::keyMatchesV1()
{
    // Values computed with v1's formula (sha1(uid \n channel \n remoteFile \n size), 20 hex digits).
    MediaLink a;
    a.serverUid = QStringLiteral("uid");
    a.channelId = 3;
    a.path      = QStringLiteral("/tsmedia/");
    a.fileName  = QStringLiteral("clip.mp4");
    a.size      = 100;
    QCOMPARE(a.key(), QStringLiteral("6aa7f62c6e2880efae24"));
    QCOMPARE(MediaLink::parse(kBaseHref).key(), QStringLiteral("6aa7f62c6e2880efae24"));

    MediaLink b;
    b.serverUid = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    b.channelId = 12;
    b.path      = QStringLiteral("/tsmedia");
    b.fileName  = QStringLiteral("φωτογραφία μου.jpg");
    b.size      = 2048;
    b.blurHash  = kRefHash;
    QCOMPARE(b.key(), QStringLiteral("fa6ca2c6c38ebcdcbd36"));
}

void TestTsMedia::previewLink()
{
    const MediaLink link    = sampleLink();
    const MediaLink preview = link.previewLink();
    QVERIFY(preview.isValid());
    QCOMPARE(preview.host, link.host);
    QCOMPARE(preview.port, link.port);
    QCOMPARE(preview.serverUid, link.serverUid);
    QCOMPARE(preview.channelId, link.channelId);
    QCOMPARE(preview.path, QStringLiteral("/tsmedia/previews"));
    QCOMPARE(preview.fileName, QStringLiteral("clip_41234.jpg"));
    QCOMPARE(preview.remoteFile(), link.previewFile);
    QCOMPARE(preview.size, quint64(0));
    QCOMPARE(preview.protocol, 0);
    QVERIFY(preview.previewFile.isEmpty());
    QVERIFY(preview.blurHash.isEmpty());
    QVERIFY(preview.key() != link.key());
    QCOMPARE(preview.key(), MediaLink::parse(link.toUrl()).previewLink().key());

    MediaLink rooted   = link;
    rooted.previewFile = QStringLiteral("/p.jpg");
    QCOMPARE(rooted.previewLink().path, QStringLiteral("/"));
    QCOMPARE(rooted.previewLink().fileName, QStringLiteral("p.jpg"));

    MediaLink none = link;
    none.previewFile.clear();
    QVERIFY(!none.previewLink().isValid());

    MediaLink self   = link;
    self.previewFile = link.remoteFile();
    QVERIFY(!self.previewLink().isValid());

    MediaLink escape   = link;
    escape.previewFile = QStringLiteral("/tsmedia/../x.jpg");
    QVERIFY(!escape.previewLink().isValid());
}

void TestTsMedia::findInComposedMessage()
{
    const MediaLink link = sampleLink();
    for (const QString& url : {QString(), QStringLiteral("https://example.com/tsmedia")}) {
        const QString          message = composeChatMessage(link, true, url);
        const QList<MediaLink> found   = MediaLink::findInMessage(message);
        QCOMPARE(found.size(), 1); // the download link in the note is not a ts3file link
        const QString diff = difference(found.first(), link);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    // Several links in one message, mixed with ordinary URLs.
    MediaLink second = link;
    second.fileName  = QStringLiteral("other.png");
    const QString message = link.toBBCode() + QStringLiteral(" and [URL=https://example.com]site[/URL] and ") + second.toBBCode();
    const auto    found   = MediaLink::findInMessage(message);
    QCOMPARE(found.size(), 2);
    QCOMPARE(found.at(1).fileName, QStringLiteral("other.png"));
}

// ---- composeChatMessage ------------------------------------------------------------------------

void TestTsMedia::composeNotice_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("note");

    const QString url = QStringLiteral("https://example.com/tsmedia");
    QTest::newRow("no url") << QString() << QStringLiteral("TS Media chat plugin required to view this in chat");
    QTest::newRow("url") << url << QStringLiteral("[URL=https://example.com/tsmedia]TS Media chat[/URL] plugin required to view this in chat");
    QTest::newRow("blank url") << QStringLiteral("   ") << QStringLiteral("TS Media chat plugin required to view this in chat");
}

void TestTsMedia::composeNotice()
{
    QFETCH(QString, url);
    QFETCH(QString, note);

    const MediaLink link    = sampleLink();
    const QString   message = composeChatMessage(link, true, url);
    QCOMPARE(message, link.toBBCode() + QStringLiteral(" [COLOR=#72767d][I]— ") + note + QStringLiteral("[/I][/COLOR]"));
    QVERIFY(utf8Bytes(message) < 1000);
}

void TestTsMedia::composeWithoutNotice()
{
    const MediaLink link = sampleLink();
    QCOMPARE(composeChatMessage(link, false, QString()), link.toBBCode());
    QCOMPARE(composeChatMessage(link, false, QStringLiteral("https://example.com")), link.toBBCode());
}

void TestTsMedia::composeDownloadUrlSanitising_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("linked"); // empty = the note without a link

    QTest::newRow("https") << QStringLiteral("https://github.com/user/tsmedia/releases") << QStringLiteral("https://github.com/user/tsmedia/releases");
    QTest::newRow("http") << QStringLiteral("http://example.com/x?a=1&b=2") << QStringLiteral("http://example.com/x?a=1&b=2");
    QTest::newRow("no scheme") << QStringLiteral("example.com/get") << QStringLiteral("https://example.com/get");
    QTest::newRow("whitespace") << QStringLiteral("  https://example.com/a b  ") << QStringLiteral("https://example.com/a%20b");
    QTest::newRow("brackets") << QStringLiteral("https://example.com/a[1]") << QStringLiteral("https://example.com/a%5B1%5D");
    QTest::newRow("bbcode injection") << QStringLiteral("https://example.com/]x[/URL][URL=https://evil.example")
                                      << QStringLiteral("https://example.com/%5Dx%5B/URL%5D%5BURL=https://evil.example");
    QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)") << QString();
    QTest::newRow("file") << QStringLiteral("file:///C:/Windows/notepad.exe") << QString();
    QTest::newRow("ts3file") << QStringLiteral("ts3file://h?serverUID=a") << QString();
}

void TestTsMedia::composeDownloadUrlSanitising()
{
    QFETCH(QString, input);
    QFETCH(QString, linked);

    const MediaLink link    = sampleLink();
    const QString   message = composeChatMessage(link, true, input);
    QVERIFY(message.startsWith(link.toBBCode()));
    const QString note = message.mid(link.toBBCode().length());
    if (linked.isEmpty()) {
        QCOMPARE(note, kNote);
    } else {
        QCOMPARE(note, QStringLiteral(" [COLOR=#72767d][I]— [URL=") + linked + QStringLiteral("]TS Media chat[/URL] plugin required to view this in chat[/I][/COLOR]"));
    }
    // Whatever the user typed, the message has exactly the expected BBCode tags.
    QCOMPARE(message.count(QStringLiteral("[URL="), Qt::CaseInsensitive), linked.isEmpty() ? 1 : 2);
    QCOMPARE(MediaLink::findInMessage(message).size(), 1);
}

void TestTsMedia::composeSizeRule()
{
    bool sawFull = false, sawNoHash = false, sawNoNote = false, sawLeaner = false;
    for (int n = 1; n <= 1000; ++n) {
        MediaLink link   = sampleLink();
        link.fileName    = QString(n, QLatin1Char('a')) + QStringLiteral(".mp4");
        link.previewFile = QStringLiteral("/tsmedia/previews/p.jpg");
        MediaLink noHash = link;
        noHash.blurHash.clear();
        MediaLink noPreview = noHash;
        noPreview.previewFile.clear();
        MediaLink bare  = noPreview;
        bare.width      = 0;
        bare.height     = 0;
        bare.durationMs = 0;

        const QString message = composeChatMessage(link, true, QString());
        if (utf8Bytes(link.toBBCode() + kNote) < 1000) {
            QCOMPARE(message, link.toBBCode() + kNote);
            sawFull = true;
        } else if (utf8Bytes(noHash.toBBCode() + kNote) < 1000) {
            QCOMPARE(message, noHash.toBBCode() + kNote); // BlurHash goes first
            sawNoHash = true;
        } else if (utf8Bytes(noHash.toBBCode()) < 1000) {
            QCOMPARE(message, noHash.toBBCode()); // then the note
            sawNoNote = true;
        } else if (utf8Bytes(bare.toBBCode()) < 1000) {
            QVERIFY(message == noPreview.toBBCode() || message == bare.toBBCode());
            sawLeaner = true;
        } else {
            QCOMPARE(message, bare.toBBCode()); // nothing left to drop
            continue;
        }
        QVERIFY2(utf8Bytes(message) < 1000, qPrintable(QString::number(n)));
        QCOMPARE(MediaLink::findInMessage(message).size(), 1);
        QCOMPARE(MediaLink::findInMessage(message).first().key(), link.key());
    }
    QVERIFY(sawFull && sawNoHash && sawNoNote && sawLeaner);
}

void TestTsMedia::composeSizeRuleWithUrl()
{
    const QString url     = QStringLiteral("https://example.com/download/tsmedia");
    const QString noteUrl = QStringLiteral(" [COLOR=#72767d][I]— [URL=") + url + QStringLiteral("]TS Media chat[/URL] plugin required to view this in chat[/I][/COLOR]");
    bool          sawUrl = false, sawNoHash = false, sawPlainNote = false, sawNoNote = false;
    for (int n = 1; n <= 1000; ++n) {
        MediaLink link = sampleLink();
        link.fileName  = QString(n, QLatin1Char('b')) + QStringLiteral(".png");
        MediaLink noHash = link;
        noHash.blurHash.clear();

        const QString message = composeChatMessage(link, true, url);
        if (utf8Bytes(link.toBBCode() + noteUrl) < 1000) {
            QCOMPARE(message, link.toBBCode() + noteUrl);
            sawUrl = true;
        } else if (utf8Bytes(noHash.toBBCode() + noteUrl) < 1000) {
            QCOMPARE(message, noHash.toBBCode() + noteUrl);
            sawNoHash = true;
        } else if (utf8Bytes(noHash.toBBCode() + kNote) < 1000) {
            QCOMPARE(message, noHash.toBBCode() + kNote); // keep the note, without its link
            sawPlainNote = true;
        } else if (utf8Bytes(noHash.toBBCode()) < 1000) {
            QCOMPARE(message, noHash.toBBCode());
            sawNoNote = true;
        } else {
            break;
        }
        QVERIFY(utf8Bytes(message) < 1000);
    }
    QVERIFY(sawUrl && sawNoHash && sawPlainNote && sawNoNote);
}

void TestTsMedia::composeSizeRuleCountsBytes()
{
    // Greek text is 2 bytes per letter in UTF-8 (and 6 when percent-encoded in the URL).
    bool sawCharsFitButBytesDont = false;
    for (int n = 1; n <= 400; ++n) {
        MediaLink link = sampleLink();
        link.fileName  = QString(n, QChar(0x03c6)) + QStringLiteral(".jpg"); // φ
        MediaLink bare = link;
        bare.blurHash.clear();
        bare.previewFile.clear();
        bare.width = bare.height = 0;
        bare.durationMs          = 0;
        if (utf8Bytes(bare.toBBCode()) >= 1000)
            break;

        const QString message = composeChatMessage(link, true, QStringLiteral("https://example.com"));
        QVERIFY2(utf8Bytes(message) < 1000, qPrintable(QString::number(n)));
        QVERIFY(message.startsWith(QStringLiteral("[URL=ts3file://")));
        const QString full = link.toBBCode();
        if (full.length() < 1000 && utf8Bytes(full) >= 1000) {
            sawCharsFitButBytesDont = true;
            QVERIFY(message != full);
        }
    }
    QVERIFY(sawCharsFitButBytesDont);
}

void TestTsMedia::noticeColourContrast()
{
    // The note's colour is fixed by the sender: it must pass on TeamSpeak's default white chat and
    // stay readable on a dark one.
    const QString message = composeChatMessage(sampleLink(), true, QString());
    const int     at      = message.indexOf(QStringLiteral("[COLOR="));
    QVERIFY(at > 0);
    const QColor note(message.mid(at + 7, 7));
    QVERIFY(note.isValid());
    QVERIFY2(ui::contrastRatio(note, Qt::white) >= 4.5, qPrintable(QString::number(ui::contrastRatio(note, Qt::white))));
    QVERIFY2(ui::contrastRatio(note, QColor(0x2b, 0x2d, 0x31)) >= 3.0, qPrintable(QString::number(ui::contrastRatio(note, QColor(0x2b, 0x2d, 0x31)))));
}

// ---- BlurHash ----------------------------------------------------------------------------------

void TestTsMedia::blurhashDecodeReference()
{
    const QImage image = blurhash::decode(kRefHash, QSize(32, 32));
    QCOMPARE(image.size(), QSize(32, 32));
    QCOMPARE(image.format(), QImage::Format_RGB32);

    // Expected pixels from a direct port of the reference decoder (rounding may differ by 1).
    struct Px {
        int x, y, r, g, b;
    };
    const Px expected[] = {{0, 0, 135, 164, 177},  {31, 0, 137, 166, 181}, {0, 31, 136, 144, 147},
                           {31, 31, 133, 142, 147}, {16, 16, 158, 125, 108}, {7, 23, 143, 138, 137}};
    for (const Px& p : expected) {
        const QRgb c = image.pixel(p.x, p.y);
        QVERIFY2(closeTo(c, p.r, p.g, p.b, 1), qPrintable(QStringLiteral("(%1,%2) = %3").arg(p.x).arg(p.y).arg(rgbText(c))));
    }

    // The average colour is the DC component (#97 96 95, a warm grey) and the image is not flat.
    double sum[3] = {0, 0, 0};
    int    minR = 255, maxR = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const QRgb c = image.pixel(x, y);
            sum[0] += qRed(c);
            sum[1] += qGreen(c);
            sum[2] += qBlue(c);
            minR = qMin(minR, qRed(c));
            maxR = qMax(maxR, qRed(c));
        }
    }
    const int dc[3] = {151, 150, 149};
    for (int i = 0; i < 3; ++i)
        QVERIFY2(qAbs(sum[i] / 1024.0 - dc[i]) < 6.0, qPrintable(QString::number(sum[i] / 1024.0)));
    QVERIFY(maxR - minR > 20);

    // Any aspect ratio / size.
    QCOMPARE(blurhash::decode(kRefHash, QSize(7, 61)).size(), QSize(7, 61));
    QCOMPARE(blurhash::decode(kRefHash, QSize(1, 1)).size(), QSize(1, 1));
}

void TestTsMedia::blurhashDecodePunch()
{
    const QImage image = blurhash::decode(kRefHash, QSize(20, 10), 2.0);
    QVERIFY2(closeTo(image.pixel(0, 0), 116, 176, 201, 1), qPrintable(rgbText(image.pixel(0, 0))));
    QVERIFY2(closeTo(image.pixel(10, 5), 165, 92, 0, 1), qPrintable(rgbText(image.pixel(10, 5))));

    // Non-positive punch behaves like 1.
    QCOMPARE(blurhash::decode(kRefHash, QSize(8, 8), 0.0), blurhash::decode(kRefHash, QSize(8, 8), 1.0));
    QCOMPARE(blurhash::decode(kRefHash, QSize(8, 8), -3.0), blurhash::decode(kRefHash, QSize(8, 8), 1.0));
}

void TestTsMedia::blurhashEncodeMatchesReference_data()
{
    QTest::addColumn<int>("cx");
    QTest::addColumn<int>("cy");
    QTest::addColumn<QString>("expected");

    // From a direct port of the reference encoder over synthImage(16, 12).
    QTest::newRow("4x3") << 4 << 3 << QStringLiteral("LsGuUZ2,wsouqeR%jse?f_fjfTfl");
    QTest::newRow("1x1") << 1 << 1 << QStringLiteral("00GuUZ");
    QTest::newRow("3x5") << 3 << 5 << QStringLiteral("csGuUZ2,wsqeR%jsf_fjfTt3Sgjvemf9fT");
    QTest::newRow("9x9") << 9 << 9
                         << QStringLiteral("|sGuUZ2,wsouSIt5SJxYN^qeR%jse?a~jJb0jIa{f_fjfTflfSfjfOfhfOt3Sgjvfja_j;a{j]b0emf9fTf4fOf9fTf9fNtOShjsfga}j^a_j;a_eUf9fOf8fTf5fNf9fTtOShjrfka~j;a~j]a_eWf8fOfAfNf6fTf4fT");
}

void TestTsMedia::blurhashEncodeMatchesReference()
{
    QFETCH(int, cx);
    QFETCH(int, cy);
    QFETCH(QString, expected);

    const QImage image = synthImage(16, 12);
    QCOMPARE(blurhash::encode(image, cx, cy), expected);
    QVERIFY(blurhash::isValid(expected));
    // Pixel format does not matter.
    QCOMPARE(blurhash::encode(image.convertToFormat(QImage::Format_ARGB32), cx, cy), expected);
    QCOMPARE(blurhash::encode(image.convertToFormat(QImage::Format_RGB888), cx, cy), expected);
}

void TestTsMedia::blurhashTwoColourRoundTrip()
{
    const QRgb left  = qRgb(220, 40, 40);
    const QRgb right = qRgb(40, 70, 220);
    QImage     image(64, 32, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, x < 32 ? left : right);
    }

    const QString hash = blurhash::encode(image, 4, 3);
    QCOMPARE(hash.length(), 28);
    QVERIFY(blurhash::isValid(hash));

    const QImage decoded = blurhash::decode(hash, image.size());
    QCOMPARE(decoded.size(), image.size());
    for (int y : {4, 16, 27}) {
        const QRgb l = decoded.pixel(6, y);
        const QRgb r = decoded.pixel(57, y);
        QVERIFY2(closeTo(l, qRed(left), qGreen(left), qBlue(left), 45), qPrintable(rgbText(l)));
        QVERIFY2(closeTo(r, qRed(right), qGreen(right), qBlue(right), 45), qPrintable(rgbText(r)));
        QVERIFY(qRed(l) > qBlue(l) + 100);
        QVERIFY(qBlue(r) > qRed(r) + 100);
    }
}

void TestTsMedia::blurhashTransparentIsBlack()
{
    QImage transparent(8, 8, QImage::Format_ARGB32);
    transparent.fill(qRgba(255, 0, 0, 0));
    QImage black(8, 8, QImage::Format_RGB32);
    black.fill(Qt::black);
    QCOMPARE(blurhash::encode(transparent), blurhash::encode(black));
}

void TestTsMedia::blurhashIsValid_data()
{
    QTest::addColumn<QString>("hash");
    QTest::addColumn<bool>("valid");

    QTest::newRow("reference") << kRefHash << true;
    QTest::newRow("1x1") << QStringLiteral("00GuUZ") << true;
    QTest::newRow("3x5") << QStringLiteral("csGuUZ2,wsqeR%jsf_fjfTt3Sgjvemf9fT") << true;
    QTest::newRow("empty") << QString() << false;
    QTest::newRow("one char") << QStringLiteral("L") << false;
    QTest::newRow("five chars") << QStringLiteral("00GuU") << false;
    QTest::newRow("too short") << kRefHash.left(27) << false;
    QTest::newRow("too long") << kRefHash + QLatin1Char('j') << false;
    QTest::newRow("bad char !") << kRefHash.left(27) + QLatin1Char('!') << false;
    QTest::newRow("bad char &") << kRefHash.left(27) + QLatin1Char('&') << false;
    QTest::newRow("space") << kRefHash.left(26) + QStringLiteral(" j") << false;
    QTest::newRow("non-ascii") << kRefHash.left(27) + QChar(0x00e9) << false;
    QTest::newRow("fullwidth digit") << kRefHash.left(27) + QChar(0xff11) << false;
    QTest::newRow("size flag > 80") << QStringLiteral("~0GuUZ") << false;
    QTest::newRow("wrong components") << QStringLiteral("10GuUZ") << false; // 2x1 needs 8 chars
}

void TestTsMedia::blurhashIsValid()
{
    QFETCH(QString, hash);
    QFETCH(bool, valid);
    QCOMPARE(blurhash::isValid(hash), valid);
    QCOMPARE(blurhash::decode(hash, QSize(4, 4)).isNull(), !valid);
}

void TestTsMedia::blurhashInvalidInput()
{
    const QImage image = synthImage(16, 12);
    QVERIFY(blurhash::encode(QImage()).isEmpty());
    QVERIFY(blurhash::encode(image, 0, 3).isEmpty());
    QVERIFY(blurhash::encode(image, 10, 3).isEmpty());
    QVERIFY(blurhash::encode(image, 4, 0).isEmpty());
    QVERIFY(blurhash::encode(image, 4, 10).isEmpty());
    QVERIFY(!blurhash::encode(image, 9, 9).isEmpty());

    QVERIFY(blurhash::decode(kRefHash, QSize()).isNull());
    QVERIFY(blurhash::decode(kRefHash, QSize(0, 10)).isNull());
    QVERIFY(blurhash::decode(kRefHash, QSize(10, -1)).isNull());

    // The largest values four / two base83 digits can hold must not overflow a channel.
    const QImage extreme = blurhash::decode(QStringLiteral("L") + QString(27, QLatin1Char('~')), QSize(8, 8));
    QCOMPARE(extreme.size(), QSize(8, 8));
}

// ---- helpers -----------------------------------------------------------------------------------

void TestTsMedia::formatDuration_data()
{
    QTest::addColumn<qint64>("ms");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << qint64(0) << QStringLiteral("0:00");
    QTest::newRow("sub-second") << qint64(999) << QStringLiteral("0:00");
    QTest::newRow("7 s") << qint64(7000) << QStringLiteral("0:07");
    QTest::newRow("7.9 s") << qint64(7999) << QStringLiteral("0:07");
    QTest::newRow("10 s") << qint64(10010) << QStringLiteral("0:10");
    QTest::newRow("59 s") << qint64(59999) << QStringLiteral("0:59");
    QTest::newRow("1 min") << qint64(60000) << QStringLiteral("1:00");
    QTest::newRow("12:34") << qint64(754000) << QStringLiteral("12:34");
    QTest::newRow("59:59") << qint64(3599000) << QStringLiteral("59:59");
    QTest::newRow("1 h") << qint64(3600000) << QStringLiteral("1:00:00");
    QTest::newRow("1:02:03") << qint64(3723000) << QStringLiteral("1:02:03");
    QTest::newRow("25 h") << qint64(90000000) << QStringLiteral("25:00:00");
    QTest::newRow("negative") << qint64(-5000) << QStringLiteral("0:00");
}

void TestTsMedia::formatDuration()
{
    QFETCH(qint64, ms);
    QFETCH(QString, text);
    QCOMPARE(::formatDuration(ms), text);
}

void TestTsMedia::formatSize_data()
{
    QTest::addColumn<quint64>("bytes");
    QTest::addColumn<QString>("text");

    QTest::newRow("0") << quint64(0) << QStringLiteral("0 B");
    QTest::newRow("1023") << quint64(1023) << QStringLiteral("1023 B");
    QTest::newRow("1 KB") << quint64(1024) << QStringLiteral("1.0 KB");
    QTest::newRow("1.5 KB") << quint64(1536) << QStringLiteral("1.5 KB");
    QTest::newRow("24.1 KB") << quint64(24678) << QStringLiteral("24.1 KB");
    QTest::newRow("99.9 KB") << quint64(102348) << QStringLiteral("99.9 KB");
    QTest::newRow("100 KB rounded") << quint64(102349) << QStringLiteral("100 KB");
    QTest::newRow("999 KB") << quint64(999 * 1024) << QStringLiteral("999 KB");
    QTest::newRow("1023.99 KB") << quint64(1048575) << QStringLiteral("1.0 MB");
    QTest::newRow("5 MB") << quint64(5 * 1024 * 1024) << QStringLiteral("5.0 MB");
    QTest::newRow("700 MB") << quint64(734003200) << QStringLiteral("700 MB");
    QTest::newRow("just under 1 GB") << quint64(1073741823) << QStringLiteral("1.0 GB");
    QTest::newRow("1.5 GB") << quint64(1536ull * 1024 * 1024) << QStringLiteral("1.5 GB");
    QTest::newRow("250 GB") << quint64(250ull * 1024 * 1024 * 1024) << QStringLiteral("250 GB");
    QTest::newRow("2 TB") << quint64(2ull * 1024 * 1024 * 1024 * 1024) << QStringLiteral("2.0 TB");
    QTest::newRow("5000 TB") << quint64(5000ull * 1024 * 1024 * 1024 * 1024) << QStringLiteral("5000 TB");
}

void TestTsMedia::formatSize()
{
    QFETCH(quint64, bytes);
    QFETCH(QString, text);
    QCOMPARE(::formatSize(bytes), text);
}

void TestTsMedia::formatProgressTexts()
{
    QCOMPARE(formatProgress(4718592, 10485760), QStringLiteral("4.5 MB of 10.0 MB"));
    QCOMPARE(formatProgress(0, 734003200), QStringLiteral("0 B of 700 MB"));
    QCOMPARE(formatProgress(2000, 1000), QStringLiteral("1000 B of 1000 B")); // done never exceeds total
    QCOMPARE(formatSpeed(1258291.0), QStringLiteral("1.2 MB/s"));
    QCOMPARE(formatSpeed(-5.0), QStringLiteral("0 B/s"));
}

void TestTsMedia::formatTimeLeft_data()
{
    QTest::addColumn<qint64>("ms");
    QTest::addColumn<QString>("text");

    QTest::newRow("nothing") << qint64(0) << QStringLiteral("1 s left");
    QTest::newRow("0.2 s") << qint64(200) << QStringLiteral("1 s left");
    QTest::newRow("8 s") << qint64(8000) << QStringLiteral("8 s left");
    QTest::newRow("59.5 s") << qint64(59500) << QStringLiteral("1 min left");
    QTest::newRow("89 s") << qint64(89000) << QStringLiteral("1 min left");
    QTest::newRow("90 s") << qint64(90000) << QStringLiteral("2 min left");
    QTest::newRow("59 min") << qint64(59 * 60000) << QStringLiteral("59 min left");
    QTest::newRow("1 h") << qint64(3600000) << QStringLiteral("1 h left");
    QTest::newRow("1 h 5 min") << qint64(65 * 60000) << QStringLiteral("1 h 5 min left");
}

void TestTsMedia::formatTimeLeft()
{
    QFETCH(qint64, ms);
    QFETCH(QString, text);
    QCOMPARE(::formatTimeLeft(ms), text);
}

void TestTsMedia::kindForFileName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("kind");

    QTest::newRow("png") << QStringLiteral("a.png") << int(MediaKind::Image);
    QTest::newRow("JPG upper") << QStringLiteral("A.JPG") << int(MediaKind::Image);
    QTest::newRow("webp") << QStringLiteral("a.webp") << int(MediaKind::Image);
    QTest::newRow("gif") << QStringLiteral("funny.gif") << int(MediaKind::AnimatedImage);
    QTest::newRow("GIF upper") << QStringLiteral("FUNNY.GIF") << int(MediaKind::AnimatedImage);
    QTest::newRow("mp4") << QStringLiteral("demo_clip.mp4") << int(MediaKind::Video);
    QTest::newRow("webm") << QStringLiteral("web_clip.webm") << int(MediaKind::Video);
    QTest::newRow("mp3") << QStringLiteral("song.mp3") << int(MediaKind::Audio);
    QTest::newRow("zip") << QStringLiteral("project_files.zip") << int(MediaKind::Archive);
    QTest::newRow("pdf") << QStringLiteral("doc.pdf") << int(MediaKind::Document);
    QTest::newRow("exe") << QStringLiteral("setup.exe") << int(MediaKind::Other);
    QTest::newRow("no suffix") << QStringLiteral("README") << int(MediaKind::Other);
}

void TestTsMedia::kindForFileName()
{
    QFETCH(QString, name);
    QFETCH(int, kind);
    QCOMPARE(int(::kindForFileName(name)), kind);
    QCOMPARE(isPreviewableImage(::kindForFileName(name)), kind == int(MediaKind::Image) || kind == int(MediaKind::AnimatedImage));
}

// ---- names and texts ---------------------------------------------------------------------------

void TestTsMedia::displayFileName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("shown");

    QTest::newRow("plain") << QStringLiteral("holiday.jpg") << QStringLiteral("holiday.jpg");
    QTest::newRow("RLO faked extension") << QStringLiteral("photo") + QChar(0x202E) + QStringLiteral("gpj.exe") << QStringLiteral("photogpj.exe");
    QTest::newRow("isolates and marks") << QChar(0x2067) + QStringLiteral("a") + QChar(0x2069) + QChar(0x200F) + QStringLiteral("b.png") << QStringLiteral("ab.png");
    QTest::newRow("control and line separator") << QStringLiteral("a\tb") + QChar(0x2028) + QStringLiteral("c.txt") << QStringLiteral("abc.txt");
    QTest::newRow("ZWJ kept") << QStringLiteral("a") + QChar(0x200D) + QStringLiteral("b.png") << QStringLiteral("a") + QChar(0x200D) + QStringLiteral("b.png");
    QTest::newRow("right-to-left script kept") << QString(QChar(0x05D0)) + QStringLiteral(".png") << QString(QChar(0x05D0)) + QStringLiteral(".png");
}

void TestTsMedia::displayFileName()
{
    QFETCH(QString, name);
    QFETCH(QString, shown);
    QCOMPARE(::displayFileName(name), shown);
}

void TestTsMedia::displayNameFor_data()
{
    QTest::addColumn<int>("protocol");
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("shown");

    QTest::newRow("upload") << 2 << QStringLiteral("holiday_3f9a1c2e.jpg") << QStringLiteral("holiday.jpg");
    QTest::newRow("plain TeamSpeak link") << 0 << QStringLiteral("holiday_3f9a1c2e.jpg") << QStringLiteral("holiday_3f9a1c2e.jpg");
    QTest::newRow("pasted png") << 2 << QStringLiteral("new_photo_1a2b3c4d.png") << QStringLiteral("Pasted image.png");
    QTest::newRow("pasted jpg") << 2 << QStringLiteral("new_photo_1a2b3c4d.jpg") << QStringLiteral("Pasted image.jpg");
    QTest::newRow("own new_photo name") << 2 << QStringLiteral("new_photo_1a2b3c4d_3f9a1c2e.png") << QStringLiteral("new_photo_1a2b3c4d.png");
    QTest::newRow("not 8 digits") << 2 << QStringLiteral("clip_41234.mp4") << QStringLiteral("clip_41234.mp4");
    QTest::newRow("upper-case hex is not ours") << 2 << QStringLiteral("x_3F9A1C2E.png") << QStringLiteral("x_3F9A1C2E.png");
    QTest::newRow("only the last part") << 2 << QStringLiteral("report_deadbeef_3f9a1c2e.pdf") << QStringLiteral("report_deadbeef.pdf");
    QTest::newRow("double extension") << 2 << QStringLiteral("backup.tar_3f9a1c2e.gz") << QStringLiteral("backup.tar.gz");
    QTest::newRow("no extension") << 2 << QStringLiteral("notes_3f9a1c2e") << QStringLiteral("notes");
    QTest::newRow("nothing else left") << 2 << QStringLiteral("_3f9a1c2e.png") << QStringLiteral("_3f9a1c2e.png");
    QTest::newRow("leading dot") << 2 << QStringLiteral(".env_3f9a1c2e") << QStringLiteral(".env");
    QTest::newRow("bidi stripped too") << 2 << QStringLiteral("a") + QChar(0x202E) + QStringLiteral("b_3f9a1c2e.png") << QStringLiteral("ab.png");
}

void TestTsMedia::displayNameFor()
{
    QFETCH(int, protocol);
    QFETCH(QString, fileName);
    QFETCH(QString, shown);

    MediaLink link = sampleLink();
    link.protocol  = protocol;
    link.fileName  = fileName;
    QCOMPARE(::displayNameFor(link), shown);
    QCOMPARE(link.fileName, fileName); // the link itself is untouched
}

void TestTsMedia::errorTexts()
{
    const MediaError all[] = {MediaError::None, MediaError::Permission, MediaError::Password, MediaError::NotFound,
                              MediaError::NotConnected, MediaError::Quota, MediaError::Other};
    for (const MediaError error : all) {
        const QString title = downloadErrorTitle(error);
        QVERIFY(!title.isEmpty());
        QVERIFY2(!title.endsWith(QLatin1Char('.')), qPrintable(title)); // titles are fragments
        // Explanations are full sentences.
        for (const QString& text : {downloadErrorText(error), uploadErrorText(error), postErrorText(error)}) {
            QVERIFY2(text.endsWith(QLatin1Char('.')), qPrintable(text));
            QVERIFY2(text.at(0).isUpper(), qPrintable(text));
        }
    }
    // The server's own message is quoted only where nothing more specific is known, and never as a title.
    const QString raw = QStringLiteral("file transfer interrupted.");
    QCOMPARE(downloadErrorTitle(MediaError::Other), QStringLiteral("Download failed"));
    QCOMPARE(downloadErrorText(MediaError::Other, raw), QStringLiteral("Something went wrong while downloading this file (file transfer interrupted). Try again."));
    QCOMPARE(uploadErrorText(MediaError::Other, raw), QStringLiteral("Upload failed (file transfer interrupted). Try again."));
    QVERIFY(!downloadErrorText(MediaError::Permission, raw).contains(QStringLiteral("interrupted")));
    // Upload and download wording differ for the same cause.
    QVERIFY(uploadErrorText(MediaError::Permission).contains(QStringLiteral("upload")));
    QVERIFY(downloadErrorText(MediaError::Permission).contains(QStringLiteral("download")));
    QVERIFY(postErrorText(MediaError::Permission).startsWith(QStringLiteral("Uploaded, but")));
}

// ---- UI helpers --------------------------------------------------------------------------------

void TestTsMedia::contrastRatio()
{
    QVERIFY(qAbs(ui::contrastRatio(Qt::black, Qt::white) - 21.0) < 1e-9);
    QVERIFY(qAbs(ui::contrastRatio(Qt::white, Qt::black) - 21.0) < 1e-9);
    QVERIFY(qAbs(ui::contrastRatio(QColor(0x80, 0x80, 0x80), QColor(0x80, 0x80, 0x80)) - 1.0) < 1e-9);
    // Well-known values: #767676 on white is the lightest grey that passes 4.5:1, #777777 fails.
    QVERIFY(ui::contrastRatio(QColor(0x76, 0x76, 0x76), Qt::white) >= 4.5);
    QVERIFY(ui::contrastRatio(QColor(0x77, 0x77, 0x77), Qt::white) < 4.5);
    // The old note colour, for the record: 3.13:1.
    QVERIFY(qAbs(ui::contrastRatio(QColor(0x8e, 0x92, 0x97), Qt::white) - 3.13) < 0.01);

    const QColor half = ui::flatten(QColor(0, 0, 0, 128), Qt::white);
    QVERIFY(qAbs(half.red() - 127) <= 1 && half.red() == half.green() && half.green() == half.blue());
    QCOMPARE(ui::flatten(QColor(10, 20, 30), Qt::white), QColor(10, 20, 30));
}

void TestTsMedia::savedToText()
{
    QCOMPARE(ui::savedToText(QStringLiteral("C:/Users/me/Pictures/holiday.jpg")), QStringLiteral("Saved to Pictures"));
    QCOMPARE(ui::savedToText(QStringLiteral("D:/clip.mp4")), QStringLiteral("Saved to D:\\"));
}

// ---- Settings ----------------------------------------------------------------------------------

void TestTsMedia::checkDownloadUrl_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<int>("problem");
    QTest::addColumn<QString>("normalized");

    const int none     = static_cast<int>(Settings::DownloadUrlProblem::None);
    const int notWeb   = static_cast<int>(Settings::DownloadUrlProblem::NotWebAddress);
    const int scheme   = static_cast<int>(Settings::DownloadUrlProblem::Scheme);
    const int brackets = static_cast<int>(Settings::DownloadUrlProblem::Brackets);
    const int tooLong  = static_cast<int>(Settings::DownloadUrlProblem::TooLong);
    QTest::newRow("empty: the default link") << QString() << none << QString();
    QTest::newRow("spaces only") << QStringLiteral("   ") << none << QString();
    QTest::newRow("https assumed") << QStringLiteral("example.com/x") << none << QStringLiteral("https://example.com/x");
    QTest::newRow("http kept") << QStringLiteral(" http://example.com/x ") << none << QStringLiteral("http://example.com/x");
    QTest::newRow("ftp") << QStringLiteral("ftp://a.b/x") << scheme << QString();
    QTest::newRow("no host") << QStringLiteral("https://") << notWeb << QString();
    QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)") << notWeb << QString();
    QTest::newRow("bracket in path") << QStringLiteral("a.com/[x]") << brackets << QString();
    QTest::newRow("IPv6 literal") << QStringLiteral("https://[::1]/") << brackets << QString();
    QTest::newRow("too long") << QStringLiteral("https://a.com/") + QString(600, QLatin1Char('a')) << tooLong << QString();
}

void TestTsMedia::checkDownloadUrl()
{
    QFETCH(QString, input);
    QFETCH(int, problem);
    QFETCH(QString, normalized);

    QString out = QStringLiteral("left over");
    QCOMPARE(static_cast<int>(Settings::checkDownloadUrl(input, &out)), problem);
    QCOMPARE(out, normalized); // cleared unless the link is usable
}

void TestTsMedia::normalizeUploadDirectory_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("folder");

    QTest::newRow("empty: the default folder") << QString() << QStringLiteral("/tsmedia");
    QTest::newRow("spaces only") << QStringLiteral("  ") << QStringLiteral("/tsmedia");
    QTest::newRow("backslashes, spaces, trailing slash") << QStringLiteral(" a\\b/ ") << QStringLiteral("/a/b");
    QTest::newRow("top level") << QStringLiteral("/") << QStringLiteral("/");
    QTest::newRow("trailing slashes") << QStringLiteral("x//") << QStringLiteral("/x");
    QTest::newRow("already normal") << QStringLiteral("/tsmedia/clips") << QStringLiteral("/tsmedia/clips");
}

void TestTsMedia::normalizeUploadDirectory()
{
    QFETCH(QString, input);
    QFETCH(QString, folder);

    QCOMPARE(Settings::normalizeUploadDirectory(input), folder);
}

// 2.2: test classes in other files (tests/testregistry.h) run after this one; QTEST_GUILESS_MAIN otherwise.
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    TestTsMedia test;
    int failed = QTest::qExec(&test, argc, argv); // first: the operands of + are evaluated in any order
    failed += tsmedia_tests::runRegistered(argc, argv);
    return failed;
}

#include "tst_tsmedia.moc"
