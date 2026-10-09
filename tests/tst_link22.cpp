// Unit tests for the 2.2 link schema (sha, ph, sp, al/ai/an, vm, wf), key() with sha, the 2.2 composer
// (captions, packing, the single cascade), captions, labels and byte-bounded remote names. A frozen copy
// of 2.1's link code checks that 2.2 stays compatible in both directions.

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QtTest>

#include <limits>
#include <optional>

#include "blurhash.h"
#include "i18n.h"
#include "medialink.h"
#include "testmain.h"

// ================================================================================================
// TS Media chat 2.1.0's link code, copied unchanged (src/medialink.cpp at fe234e9), only renamed.
// Never edit it: it is what 2.1 receivers run.
// ================================================================================================
namespace v21 {

struct Link {
    QString host;
    quint16 port = 0;
    QString serverUid;
    quint64 channelId = 0;
    QString path      = QStringLiteral("/");
    QString fileName;
    quint64 size       = 0;
    qint64  dateTime   = 0;
    bool    isDir      = false;
    int     protocol   = 0;
    int     width      = 0;
    int     height     = 0;
    qint64  durationMs = 0;
    QString blurHash;
    QString previewFile;

    bool    isValid() const { return !serverUid.isEmpty() && channelId != 0 && !fileName.isEmpty() && !isDir; }
    QString remoteFile() const;
    QString key() const;
    QString toUrl() const;
    QString toBBCode() const;
};

constexpr qint64 kMaxDimension     = 16384;
constexpr qint64 kMaxAspect        = 8;
constexpr qint64 kMaxDurationMs    = 1000ll * 3600 * 1000;
constexpr int    kMaxBlurHashChars = 120;
constexpr int    kMaxPathChars     = 1024;
constexpr int    kMaxMessageBytes  = 1000;

QString encode(const QString& value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString normalizeDir(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    while (path.length() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

std::optional<qint64> parseNumber(const QString& text)
{
    const QString trimmed = text.trimmed();
    bool          ok      = false;
    const qint64  value   = trimmed.toLongLong(&ok);
    if (ok)
        return value;
    static const QRegularExpression digits(QStringLiteral("^[+-]?\\d+$"));
    if (!digits.match(trimmed).hasMatch())
        return std::nullopt;
    return trimmed.startsWith(QLatin1Char('-')) ? std::numeric_limits<qint64>::min() : std::numeric_limits<qint64>::max();
}

QString sanitizeRemoteFile(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!path.startsWith(QLatin1Char('/')) || path.length() > kMaxPathChars)
        return {};
    for (const QChar c : qAsConst(path)) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return {};
    }
    static const QRegularExpression onlyDots(QStringLiteral("^\\.+$"));
    const QStringList               parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    for (const QString& part : parts) {
        if (onlyDots.match(part).hasMatch())
            return {};
    }
    if (path.endsWith(QLatin1Char('/')))
        return {};
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

void applySize(Link& link, const std::optional<qint64>& w, const std::optional<qint64>& h)
{
    if (!w || !h || *w <= 0 || *h <= 0)
        return;
    qint64 width  = qMin(*w, kMaxDimension);
    qint64 height = qMin(*h, kMaxDimension);
    if (width > height * kMaxAspect)
        height = (width + kMaxAspect - 1) / kMaxAspect;
    else if (height > width * kMaxAspect)
        width = (height + kMaxAspect - 1) / kMaxAspect;
    link.width  = static_cast<int>(width);
    link.height = static_cast<int>(height);
}

QString bbcodeSafeUrl(const QString& input)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return {};
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl    url(text, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return {};
    QString result = url.toString(QUrl::FullyEncoded);
    result.replace(QLatin1Char('['), QLatin1String("%5B"));
    result.replace(QLatin1Char(']'), QLatin1String("%5D"));
    result.replace(QLatin1Char(' '), QLatin1String("%20"));
    return result;
}

QString requiredNotice(const QString& url)
{
    const QString note = url.isEmpty() ? i18n::t("TS Media chat plugin required to view this in chat")
                                       : i18n::t("[URL=%1]TS Media chat[/URL] plugin required to view this in chat").arg(url);
    return QStringLiteral(" [COLOR=#72767d][I]— ") + note + QStringLiteral("[/I][/COLOR]");
}

QString Link::remoteFile() const
{
    const QString dir = normalizeDir(path);
    return dir == QLatin1String("/") ? dir + fileName : dir + QLatin1Char('/') + fileName;
}

QString Link::key() const
{
    const QString raw = serverUid + QLatin1Char('\n') + QString::number(channelId) + QLatin1Char('\n') + remoteFile() + QLatin1Char('\n') + QString::number(size);
    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}

QString Link::toUrl() const
{
    QString url = QStringLiteral("ts3file://") + encode(host);
    url += QStringLiteral("?port=") + QString::number(port);
    url += QStringLiteral("&serverUID=") + encode(serverUid);
    url += QStringLiteral("&channel=") + QString::number(channelId);
    url += QStringLiteral("&path=") + encode(normalizeDir(path));
    url += QStringLiteral("&filename=") + encode(fileName);
    url += QStringLiteral("&isDir=") + QString::number(isDir ? 1 : 0);
    url += QStringLiteral("&size=") + QString::number(size);
    url += QStringLiteral("&fileDateTime=") + QString::number(dateTime);
    if (protocol > 0)
        url += QStringLiteral("&tsm=") + QString::number(protocol);
    if (width > 0 && height > 0) {
        url += QStringLiteral("&w=") + QString::number(width);
        url += QStringLiteral("&h=") + QString::number(height);
    }
    if (durationMs > 0)
        url += QStringLiteral("&d=") + QString::number(durationMs);
    if (!blurHash.isEmpty())
        url += QStringLiteral("&bh=") + encode(blurHash);
    if (!previewFile.isEmpty())
        url += QStringLiteral("&pv=") + encode(previewFile);
    return url;
}

QString Link::toBBCode() const
{
    QString label = fileName;
    label.replace(QLatin1Char('['), QLatin1Char('('));
    label.replace(QLatin1Char(']'), QLatin1Char(')'));
    return QStringLiteral("[URL=") + toUrl() + QStringLiteral("]") + label + QStringLiteral("[/URL]");
}

Link parse(const QString& href)
{
    Link    link;
    QString text = href;
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    const int scheme = text.indexOf(QLatin1String("ts3file"), 0, Qt::CaseInsensitive);
    if (scheme < 0)
        return link;
    const int query = text.indexOf(QLatin1Char('?'), scheme);
    if (query < 0)
        return link;

    QString host = text.mid(scheme, query - scheme);
    host.remove(QRegularExpression(QStringLiteral("^ts3file:?/*"), QRegularExpression::CaseInsensitiveOption));
    link.host = QUrl::fromPercentEncoding(host.toUtf8());

    const QUrlQuery q(text.mid(query + 1));
    const auto      items = q.queryItems(QUrl::FullyDecoded);
    auto            value = [&items](const char* name) {
        for (const auto& item : items) {
            if (item.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                return item.second;
        }
        return QString();
    };
    auto has = [&items](const char* name) {
        for (const auto& item : items) {
            if (item.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    link.port      = static_cast<quint16>(value("port").toUInt());
    link.serverUid = value("serverUID");
    link.channelId = value("channel").toULongLong();
    link.path      = normalizeDir(value("path").isEmpty() ? QStringLiteral("/") : value("path"));
    link.fileName  = value("filename");
    link.size      = value("size").toULongLong();
    link.dateTime  = value("fileDateTime").toLongLong();
    link.isDir     = value("isDir").toInt() != 0;
    link.fileName  = QFileInfo(link.fileName.replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();

    if (has("tsm")) {
        if (const auto tsm = parseNumber(value("tsm")); tsm && *tsm > 0)
            link.protocol = static_cast<int>(qMin<qint64>(*tsm, std::numeric_limits<int>::max()));
    }
    if (has("w") || has("h"))
        applySize(link, parseNumber(value("w")), parseNumber(value("h")));
    if (has("d")) {
        if (const auto d = parseNumber(value("d")); d && *d > 0)
            link.durationMs = qMin(*d, kMaxDurationMs);
    }
    if (has("bh")) {
        const QString bh = value("bh");
        if (bh.length() <= kMaxBlurHashChars && blurhash::isValid(bh))
            link.blurHash = bh;
    }
    if (has("pv")) {
        const QString pv = sanitizeRemoteFile(value("pv"));
        if (!pv.isEmpty() && pv.compare(link.remoteFile(), Qt::CaseInsensitive) != 0)
            link.previewFile = pv;
    }
    return link;
}

QList<Link> findInMessage(const QString& message)
{
    static const QRegularExpression re(QStringLiteral(R"(\[url=([^\]]*ts3file[^\]]*)\])"), QRegularExpression::CaseInsensitiveOption);
    QList<Link>                     result;
    auto                            it = re.globalMatch(message);
    while (it.hasNext()) {
        const Link link = parse(it.next().captured(1));
        if (link.isValid())
            result.append(link);
    }
    return result;
}

QString composeChatMessage(const Link& link, bool includeNotice, const QString& downloadUrl)
{
    const QString url    = includeNotice ? bbcodeSafeUrl(downloadUrl) : QString();
    Link          noHash = link;
    noHash.blurHash.clear();
    Link noPreview = noHash;
    noPreview.previewFile.clear();
    Link bare       = noPreview;
    bare.width      = 0;
    bare.height     = 0;
    bare.durationMs = 0;

    QStringList candidates;
    if (includeNotice) {
        candidates << link.toBBCode() + requiredNotice(url) << noHash.toBBCode() + requiredNotice(url);
        if (!url.isEmpty())
            candidates << noHash.toBBCode() + requiredNotice(QString());
    } else {
        candidates << link.toBBCode();
    }
    candidates << noHash.toBBCode() << noPreview.toBBCode() << bare.toBBCode();
    for (const QString& message : qAsConst(candidates)) {
        if (message.toUtf8().size() < kMaxMessageBytes)
            return message;
    }
    return candidates.last();
}

} // namespace v21

namespace {

const QString kRefHash = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj");
const QString kShaEmpty = QStringLiteral("47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU"); // SHA-256("")
const QString kShaAbc   = QStringLiteral("ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0"); // SHA-256("abc")
const QString kPhEmpty  = QStringLiteral("47DEQpj8HBSa-_TImW-5JA");                      // its first 16 bytes
const QString kWfRamp   = QStringLiteral("ASNFZ4mrze8BI0VniavN7wEjRWeJq83vASNFZ4mrze8"); // levels 0,1,..,15,0,1,..

QByteArray digest(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray rampLevels()
{
    QByteArray levels;
    for (int i = 0; i < MediaLink::kWaveformLevels; ++i)
        levels.append(static_cast<char>(i % 16));
    return levels;
}

int utf8Bytes(const QString& text)
{
    return text.toUtf8().size();
}

// A typical 2.2 upload as Core builds it.
MediaLink photo(const QString& fileName = QStringLiteral("sunset_3f9a1c2e.jpg"))
{
    MediaLink link;
    link.host        = QStringLiteral("ts.example.com");
    link.port        = 9987;
    link.serverUid   = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    link.channelId   = 42;
    link.path        = QStringLiteral("/tsmedia");
    link.fileName    = fileName;
    link.size        = 2458123;
    link.dateTime    = 1760000000;
    link.protocol    = MediaLink::kProtocol;
    link.width       = 4032;
    link.height      = 3024;
    link.blurHash    = kRefHash;
    link.previewFile = QStringLiteral("/tsmedia/previews/3f9a1c2e.jpg");
    link.sha256      = digest("");
    link.previewSha  = digest("").left(16);
    return link;
}

MediaLink voiceMessage()
{
    MediaLink link   = photo(QStringLiteral("voice_message_5b0c77e1.m4a"));
    link.width       = 0;
    link.height      = 0;
    link.blurHash.clear();
    link.previewFile.clear();
    link.previewSha.clear();
    link.size       = 201733;
    link.durationMs = 12480;
    link.voice      = true;
    link.waveform   = rampLevels();
    return link;
}

v21::Link toV21(const MediaLink& l)
{
    v21::Link o;
    o.host        = l.host;
    o.port        = l.port;
    o.serverUid   = l.serverUid;
    o.channelId   = l.channelId;
    o.path        = l.path;
    o.fileName    = l.fileName;
    o.size        = l.size;
    o.dateTime    = l.dateTime;
    o.isDir       = l.isDir;
    o.protocol    = l.protocol;
    o.width       = l.width;
    o.height      = l.height;
    o.durationMs  = l.durationMs;
    o.blurHash    = l.blurHash;
    o.previewFile = l.previewFile;
    return o;
}

MediaLink without22(MediaLink link)
{
    link.sha256.clear();
    link.previewSha.clear();
    link.spoiler    = false;
    link.albumId    = 0;
    link.albumIndex = 0;
    link.albumCount = 0;
    link.voice      = false;
    link.waveform.clear();
    return link;
}

// Empty if the 2.1 fields of both are the same, else the first difference.
QString difference21(const v21::Link& a, const MediaLink& b)
{
#define TSM_COMPARE(field)                                                                    \
    if (!(a.field == b.field))                                                                \
        return QStringLiteral(#field ": [%1] != [%2]").arg(QVariant::fromValue(a.field).toString(), QVariant::fromValue(b.field).toString());
    TSM_COMPARE(host)
    TSM_COMPARE(port)
    TSM_COMPARE(serverUid)
    TSM_COMPARE(channelId)
    TSM_COMPARE(path)
    TSM_COMPARE(fileName)
    TSM_COMPARE(size)
    TSM_COMPARE(dateTime)
    TSM_COMPARE(isDir)
    TSM_COMPARE(protocol)
    TSM_COMPARE(width)
    TSM_COMPARE(height)
    TSM_COMPARE(durationMs)
    TSM_COMPARE(blurHash)
    TSM_COMPARE(previewFile)
#undef TSM_COMPARE
    return {};
}

QString albumText(const MediaLink& link)
{
    return link.albumId == 0 ? QString() : QStringLiteral("%1/%2/%3").arg(link.albumId, 8, 16, QLatin1Char('0')).arg(link.albumIndex).arg(link.albumCount);
}

QString b64(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// The (first) value of a query parameter of a URL, as written.
QString paramValue(const QString& url, const QString& name)
{
    for (const auto& item : QUrlQuery(url.mid(url.indexOf(QLatin1Char('?')) + 1)).queryItems()) {
        if (item.first == name)
            return item.second;
    }
    return {};
}

// A message as TeamSpeak reads it: every [noparse] closed by a [/noparse] after it.
bool noparseClosed(const QString& message)
{
    for (int from = 0;;) {
        const int open = message.indexOf(QStringLiteral("[noparse]"), from, Qt::CaseInsensitive);
        if (open < 0)
            return true;
        const int close = message.indexOf(QStringLiteral("[/noparse]"), open + 9, Qt::CaseInsensitive);
        if (close < 0)
            return false;
        from = close + 10;
    }
}

// The query parameter names of a URL, in order.
QStringList paramNames(const QString& url)
{
    QStringList names;
    for (const auto& item : QUrlQuery(url.mid(url.indexOf(QLatin1Char('?')) + 1)).queryItems())
        names << item.first;
    return names;
}

} // namespace

class TestLink22 : public QObject
{
    Q_OBJECT

  private slots:
    void v21LinksAreByteIdentical_data();
    void v21LinksAreByteIdentical();
    void v21ParserReadsLinks22();
    void roundTrip22_data();
    void roundTrip22();
    void paramOrder();
    void hostileParams22_data();
    void hostileParams22();
    void waveformPacking();
    void keyWithSha();
    void dropInvalidMetadata();
    void findInMessageSkipsNoparse();

    void composeEmptyCaptionMatches21();
    void composeCaptionFirst();
    void composeCaptionSplit();
    void composeCaptionNeverCut();
    void composeCascadeOrder_data();
    void composeCascadeOrder();
    void composeNeverDrops();
    void composeAlbumPacking_data();
    void composeAlbumPacking();
    void composeSizeRule22();
    void composeHostileCaptions();

    void sanitizeCaptionTexts_data();
    void sanitizeCaptionTexts();
    void captionToBBCodeTexts_data();
    void captionToBBCodeTexts();
    void linkLabels_data();
    void linkLabels();
    void boundRemoteBaseNames_data();
    void boundRemoteBaseNames();
    void remoteSuffixes();
};

// ---- 2.1 compatibility -------------------------------------------------------------------------

void TestLink22::v21LinksAreByteIdentical_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<bool>("tsMedia");
    QTest::newRow("photo") << QStringLiteral("sunset_3f9a1c2e.jpg") << true;
    QTest::newRow("plain TeamSpeak link") << QStringLiteral("report final.pdf") << false;
    QTest::newRow("brackets and percent") << QStringLiteral("[draft] 100% (v2)_3f9a1c2e.png") << true;
    QTest::newRow("persian") << QStringLiteral("عکس تعطیلات_3f9a1c2e.jpg") << true;
    QTest::newRow("long") << QString(700, QLatin1Char('x')) + QStringLiteral(".mp4") << true;
}

void TestLink22::v21LinksAreByteIdentical()
{
    QFETCH(QString, fileName);
    QFETCH(bool, tsMedia);
    MediaLink link = without22(photo(fileName));
    if (!tsMedia) {
        link.protocol = 0;
        link.width = link.height = 0;
        link.blurHash.clear();
        link.previewFile.clear();
    }
    const v21::Link old = toV21(link);
    QCOMPARE(link.toUrl(), old.toUrl());
    QCOMPARE(link.toBBCode(), old.toBBCode());
    QCOMPARE(link.key(), old.key());
    for (const QString& url : {QString(), QStringLiteral("https://example.com/get")}) {
        for (bool notice : {true, false}) {
            QCOMPARE(composeChatMessage(link, notice, url), v21::composeChatMessage(old, notice, url));
            ComposeOptions options;
            options.includeNotice  = notice;
            options.downloadUrl    = url;
            options.friendlyLabels = false;
            QCOMPARE(composeChatMessages({link}, options), QStringList{v21::composeChatMessage(old, notice, url)});
        }
    }
    // Hand-made 2.2 fields that a receiver would drop are never written either.
    MediaLink junk  = link;
    junk.sha256     = QByteArray(31, 'x');
    junk.previewSha = QByteArray(17, 'x');
    junk.albumId    = 7;
    junk.albumCount = 1;
    junk.waveform   = rampLevels(); // without the voice flag
    QCOMPARE(junk.toUrl(), old.toUrl());
}

void TestLink22::v21ParserReadsLinks22()
{
    // Everything 2.2 adds is ignored by 2.1, which still reads every field it knows.
    QList<MediaLink> links;
    MediaLink        a = photo();
    a.spoiler          = true;
    a.albumId          = 0x7c1e09ab;
    a.albumIndex       = 1;
    a.albumCount       = 3;
    MediaLink b        = a;
    b.fileName         = QStringLiteral("clip_0badf00d.mp4");
    b.albumIndex       = 2;
    b.durationMs       = 61000;
    MediaLink c        = voiceMessage();
    links << a << b << c;
    for (const MediaLink& link : qAsConst(links)) {
        const v21::Link old = v21::parse(link.toUrl());
        QVERIFY(old.isValid());
        const QString diff = difference21(old, without22(link));
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
        QCOMPARE(old.key(), without22(link).key()); // 2.1's key (2.1 has no sha)
    }

    // Packed messages with a caption: 2.1 finds every link, in order.
    ComposeOptions options;
    options.caption = QStringLiteral("Trip photos [day 2] https://example.com/album");
    for (int max : {kMaxMessageBytes, 8000}) {
        options.maxBytes             = max;
        QList<v21::Link> found;
        for (const QString& message : composeChatMessages(links, options))
            found += v21::findInMessage(message);
        QCOMPARE(found.size(), 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(found.at(i).remoteFile(), links.at(i).remoteFile());
    }
}

// ---- schema ------------------------------------------------------------------------------------

void TestLink22::roundTrip22_data()
{
    QTest::addColumn<int>("variant");
    QTest::addColumn<int>("ampersands");
    // One per parameter after the first ("?port"): 12 for photo()'s 2.1 part, then one per 2.2 parameter.
    QTest::newRow("photo + sha + ph") << 0 << 14;
    QTest::newRow("spoiler album photo") << 1 << 18;
    QTest::newRow("spoiler GIF") << 2 << 15;
    QTest::newRow("voice message") << 3 << 12; // no w/h/bh/pv/ph, but d, sha, vm, wf
    QTest::newRow("album video without sha") << 4 << 17;
}

void TestLink22::roundTrip22()
{
    QFETCH(int, variant);
    QFETCH(int, ampersands);
    MediaLink link = photo();
    switch (variant) {
    case 1:
        link.spoiler    = true;
        link.albumId    = 0xffffffffu;
        link.albumIndex = 10;
        link.albumCount = 10;
        break;
    case 2:
        link.fileName = QStringLiteral("party_3f9a1c2e.gif");
        link.spoiler  = true;
        break;
    case 3:
        link = voiceMessage();
        break;
    case 4:
        link.fileName   = QStringLiteral("clip_3f9a1c2e.mp4");
        link.durationMs = 42000;
        link.sha256.clear();
        link.albumId    = 1;
        link.albumIndex = 2;
        link.albumCount = 2;
        break;
    default:
        break;
    }
    const QString url = link.toUrl();
    QCOMPARE(url.count(QLatin1Char('&')), ampersands);
    // 2.2 values are never percent-encoded.
    const QString tail = url.mid(toV21(without22(link)).toUrl().size());
    QVERIFY2(!tail.contains(QLatin1Char('%')), qPrintable(tail));

    // The part up to pv is exactly 2.1's link.
    QVERIFY(url.startsWith(toV21(without22(link)).toUrl()));

    const MediaLink parsed = MediaLink::parse(url);
    QVERIFY(parsed.isValid());
    QCOMPARE(difference21(toV21(parsed), link), QString());
    QCOMPARE(parsed.sha256, link.sha256);
    QCOMPARE(parsed.previewSha, link.previewSha);
    QCOMPARE(parsed.spoiler, link.spoiler);
    QCOMPARE(albumText(parsed), albumText(link));
    QCOMPARE(parsed.voice, link.voice);
    QCOMPARE(parsed.waveform, link.waveform);
    QCOMPARE(parsed.key(), link.key());
    QCOMPARE(parsed.toUrl(), url);

    const QList<MediaLink> found = MediaLink::findInMessage(QStringLiteral("look ") + link.toBBCode(linkLabel(link)) + QStringLiteral(" nice"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().toUrl(), url);
}

void TestLink22::paramOrder()
{
    MediaLink link  = photo();
    link.spoiler    = true;
    link.albumId    = 0x7c1e09ab;
    link.albumIndex = 3;
    link.albumCount = 5;
    QCOMPARE(paramNames(link.toUrl()), (QStringList{"port", "serverUID", "channel", "path", "filename", "isDir", "size", "fileDateTime", "tsm", "w", "h", "bh", "pv", "sha", "ph", "sp", "al", "ai", "an"}));
    QVERIFY(link.toUrl().endsWith(QStringLiteral("&sha=") + kShaEmpty + QStringLiteral("&ph=") + kPhEmpty + QStringLiteral("&sp=1&al=7c1e09ab&ai=3&an=5")));

    const MediaLink voice = voiceMessage();
    QCOMPARE(paramNames(voice.toUrl()), (QStringList{"port", "serverUID", "channel", "path", "filename", "isDir", "size", "fileDateTime", "tsm", "d", "sha", "vm", "wf"}));
    QVERIFY(voice.toUrl().endsWith(QStringLiteral("&vm=1&wf=") + kWfRamp));
}

void TestLink22::hostileParams22_data()
{
    QTest::addColumn<QString>("base"); // image, audio, plain (no tsm), nopv
    QTest::addColumn<QString>("params");
    QTest::addColumn<QString>("sha");
    QTest::addColumn<QString>("ph");
    QTest::addColumn<bool>("sp");
    QTest::addColumn<QString>("album");
    QTest::addColumn<bool>("vm");
    QTest::addColumn<QString>("wf");

    const QString none;
    const QString img = QStringLiteral("image");
    const QString aud = QStringLiteral("audio");
    const QString album = QStringLiteral("7c1e09ab/3/5");
    // clang-format off
    // sha
    QTest::newRow("sha valid")             << img << "&sha=" + kShaEmpty                          << kShaEmpty << none << false << none << false << none;
    QTest::newRow("sha upper-case name")   << img << "&SHA=" + kShaEmpty                          << kShaEmpty << none << false << none << false << none;
    QTest::newRow("sha 42 chars")          << img << "&sha=" + kShaEmpty.left(42)                 << none << none << false << none << false << none;
    QTest::newRow("sha 44 chars")          << img << "&sha=" + kShaEmpty + "A"                    << none << none << false << none << false << none;
    QTest::newRow("sha padded")            << img << "&sha=" + kShaEmpty + "%3D"                  << none << none << false << none << false << none;
    QTest::newRow("sha plus")              << img << "&sha=47DEQpj8HBSa+_TImW-5JCeuQeRkm5NMpJWZG3hSuFU" << none << none << false << none << false << none;
    QTest::newRow("sha encoded plus")      << img << "&sha=47DEQpj8HBSa%2B_TImW-5JCeuQeRkm5NMpJWZG3hSuFU" << none << none << false << none << false << none;
    QTest::newRow("sha slash")             << img << "&sha=47DEQpj8HBSa%2F_TImW-5JCeuQeRkm5NMpJWZG3hSuFU" << none << none << false << none << false << none;
    QTest::newRow("sha standard base64")   << img << "&sha=47DEQpj8HBSa%2B%2FTImW%2B5JCeuQeRkm5NMpJWZG3hSuFU%3D" << none << none << false << none << false << none;
    QTest::newRow("sha hex")               << img << "&sha=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << none << none << false << none << false << none;
    QTest::newRow("sha not canonical")     << img << "&sha=47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFV" << none << none << false << none << false << none;
    QTest::newRow("sha space")             << img << "&sha=47DEQpj8HBSa%20TImW-5JCeuQeRkm5NMpJWZG3hSuFU" << none << none << false << none << false << none;
    QTest::newRow("sha empty")             << img << "&sha="                                      << none << none << false << none << false << none;
    QTest::newRow("sha first wins")        << img << "&sha=" + kShaAbc + "&sha=" + kShaEmpty      << kShaAbc << none << false << none << false << none;
    QTest::newRow("sha first wins, bad")   << img << "&sha=x&sha=" + kShaEmpty                    << none << none << false << none << false << none;
    QTest::newRow("sha on plain link")     << "plain" << "&sha=" + kShaEmpty                      << none << none << false << none << false << none;
    // ph
    QTest::newRow("ph valid")              << img << "&ph=" + kPhEmpty                            << none << kPhEmpty << false << none << false << none;
    QTest::newRow("ph 21 chars")           << img << "&ph=" + kPhEmpty.left(21)                   << none << none << false << none << false << none;
    QTest::newRow("ph 23 chars")           << img << "&ph=" + kPhEmpty + "A"                      << none << none << false << none << false << none;
    QTest::newRow("ph not canonical")      << img << "&ph=47DEQpj8HBSa-_TImW-5JB"                 << none << none << false << none << false << none;
    QTest::newRow("ph a full sha")         << img << "&ph=" + kShaEmpty                           << none << none << false << none << false << none;
    QTest::newRow("ph without pv")         << "nopv" << "&ph=" + kPhEmpty                         << none << none << false << none << false << none;
    QTest::newRow("ph with a bad pv")      << "nopv" << "&pv=%2Ftsmedia%2F..%2Fx.jpg&ph=" + kPhEmpty << none << none << false << none << false << none;
    // sp
    QTest::newRow("sp image")              << img << "&sp=1"                                      << none << none << true  << none << false << none;
    QTest::newRow("sp 0")                  << img << "&sp=0"                                      << none << none << false << none << false << none;
    QTest::newRow("sp 2")                  << img << "&sp=2"                                      << none << none << false << none << false << none;
    QTest::newRow("sp yes")                << img << "&sp=yes"                                    << none << none << false << none << false << none;
    QTest::newRow("sp true")               << img << "&sp=true"                                   << none << none << false << none << false << none;
    QTest::newRow("sp empty")              << img << "&sp="                                       << none << none << false << none << false << none;
    QTest::newRow("sp no value")           << img << "&sp"                                        << none << none << false << none << false << none;
    QTest::newRow("sp 01")                 << img << "&sp=01"                                     << none << none << false << none << false << none;
    QTest::newRow("sp space")              << img << "&sp=%201"                                   << none << none << false << none << false << none;
    QTest::newRow("sp first wins")         << img << "&sp=0&sp=1"                                 << none << none << false << none << false << none;
    QTest::newRow("sp on audio")           << aud << "&sp=1"                                      << none << none << false << none << false << none;
    QTest::newRow("sp on plain link")      << "plain" << "&sp=1"                                  << none << none << false << none << false << none;
    // album
    QTest::newRow("album valid")           << img << "&al=7c1e09ab&ai=3&an=5"                     << none << none << false << album << false << none;
    QTest::newRow("album any order")       << img << "&an=5&ai=3&al=7c1e09ab"                     << none << none << false << album << false << none;
    QTest::newRow("album 10 of 10")        << img << "&al=7c1e09ab&ai=10&an=10"                   << none << none << false << "7c1e09ab/10/10" << false << none;
    QTest::newRow("album on audio")        << aud << "&al=7c1e09ab&ai=3&an=5"                     << none << none << false << album << false << none;
    QTest::newRow("al upper case")         << img << "&al=7C1E09AB&ai=3&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("al 7 chars")            << img << "&al=7c1e09a&ai=3&an=5"                      << none << none << false << none << false << none;
    QTest::newRow("al 9 chars")            << img << "&al=7c1e09abc&ai=3&an=5"                    << none << none << false << none << false << none;
    QTest::newRow("al zero")               << img << "&al=00000000&ai=3&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("al not hex")            << img << "&al=7c1e09ag&ai=3&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("al signed")             << img << "&al=-7c1e09a&ai=3&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("ai 0")                  << img << "&al=7c1e09ab&ai=0&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("ai past an")            << img << "&al=7c1e09ab&ai=6&an=5"                     << none << none << false << none << false << none;
    QTest::newRow("ai negative")           << img << "&al=7c1e09ab&ai=-1&an=5"                    << none << none << false << none << false << none;
    QTest::newRow("ai huge")               << img << "&al=7c1e09ab&ai=99999999999&an=5"           << none << none << false << none << false << none;
    QTest::newRow("ai leading zero")       << img << "&al=7c1e09ab&ai=03&an=5"                    << none << none << false << none << false << none;
    QTest::newRow("ai plus")               << img << "&al=7c1e09ab&ai=%2B3&an=5"                  << none << none << false << none << false << none;
    QTest::newRow("an 1")                  << img << "&al=7c1e09ab&ai=1&an=1"                     << none << none << false << none << false << none;
    QTest::newRow("an 11")                 << img << "&al=7c1e09ab&ai=3&an=11"                    << none << none << false << none << false << none;
    QTest::newRow("an 0")                  << img << "&al=7c1e09ab&ai=3&an=0"                     << none << none << false << none << false << none;
    QTest::newRow("an missing")            << img << "&al=7c1e09ab&ai=3"                          << none << none << false << none << false << none;
    QTest::newRow("al missing")            << img << "&ai=3&an=5"                                 << none << none << false << none << false << none;
    QTest::newRow("album first wins")      << img << "&al=7c1e09ab&ai=3&an=5&al=00000001&ai=1&an=2" << none << none << false << album << false << none;
    QTest::newRow("album on plain link")   << "plain" << "&al=7c1e09ab&ai=3&an=5"                 << none << none << false << none << false << none;
    // vm, wf
    QTest::newRow("vm audio")              << aud << "&vm=1"                                      << none << none << false << none << true  << none;
    QTest::newRow("vm + wf")               << aud << "&vm=1&wf=" + kWfRamp                        << none << none << false << none << true  << kWfRamp;
    QTest::newRow("wf before vm")          << aud << "&wf=" + kWfRamp + "&vm=1"                   << none << none << false << none << true  << kWfRamp;
    QTest::newRow("vm on image")           << img << "&vm=1&wf=" + kWfRamp                        << none << none << false << none << false << none;
    QTest::newRow("vm yes")                << aud << "&vm=yes&wf=" + kWfRamp                      << none << none << false << none << false << none;
    QTest::newRow("wf without vm")         << aud << "&wf=" + kWfRamp                             << none << none << false << none << false << none;
    QTest::newRow("wf 42 chars")           << aud << "&vm=1&wf=" + kWfRamp.left(42)               << none << none << false << none << true  << none;
    QTest::newRow("wf 44 chars")           << aud << "&vm=1&wf=" + kWfRamp + "A"                  << none << none << false << none << true  << none;
    QTest::newRow("wf 48 one-char samples") << aud << "&vm=1&wf=ARdnw27--961ulaNGVgqy48--85zriWILYjt059--73xpeSD" << none << none << false << none << true << none;
    QTest::newRow("wf encoded plus")       << aud << "&vm=1&wf=%2B" + kWfRamp.mid(1)              << none << none << false << none << true  << none;
    QTest::newRow("wf not canonical")      << aud << "&vm=1&wf=" + kWfRamp.left(42) + "9"         << none << none << false << none << true  << none;
    QTest::newRow("vm on plain link")      << "plain" << "&vm=1&wf=" + kWfRamp                    << none << none << false << none << false << none;
    // everything at once, plus noise
    QTest::newRow("all valid, noise")      << img << "&foo=bar&sha=" + kShaEmpty + "&ph=" + kPhEmpty + "&sp=1&al=7c1e09ab&ai=3&an=5&vm=1&future=x" << kShaEmpty << kPhEmpty << true << album << false << none;
    // clang-format on
}

void TestLink22::hostileParams22()
{
    QFETCH(QString, base);
    QFETCH(QString, params);
    QFETCH(QString, sha);
    QFETCH(QString, ph);
    QFETCH(bool, sp);
    QFETCH(QString, album);
    QFETCH(bool, vm);
    QFETCH(QString, wf);

    QString href = QStringLiteral("ts3file://h?port=9987&serverUID=uid&channel=3&path=%2Ftsmedia&isDir=0&size=100&fileDateTime=1700000000");
    if (base == QLatin1String("audio"))
        href += QStringLiteral("&filename=voice_message_5b0c77e1.m4a&tsm=2&d=12480");
    else if (base == QLatin1String("plain"))
        href += QStringLiteral("&filename=photo_3f9a1c2e.jpg&pv=%2Ftsmedia%2Fpreviews%2F3f9a1c2e.jpg");
    else if (base == QLatin1String("nopv"))
        href += QStringLiteral("&filename=photo_3f9a1c2e.jpg&tsm=2");
    else
        href += QStringLiteral("&filename=photo_3f9a1c2e.jpg&tsm=2&pv=%2Ftsmedia%2Fpreviews%2F3f9a1c2e.jpg");

    const MediaLink before = MediaLink::parse(href);
    const MediaLink link   = MediaLink::parse(href + params);
    QVERIFY(link.isValid()); // bad metadata is dropped, never fatal
    QCOMPARE(difference21(toV21(link), before), QString());
    QCOMPARE(b64(link.sha256), sha);
    QCOMPARE(b64(link.previewSha), ph);
    QCOMPARE(link.spoiler, sp);
    QCOMPARE(albumText(link), album);
    QCOMPARE(link.voice, vm);
    QCOMPARE(paramValue(link.toUrl(), QStringLiteral("wf")), wf);
    QCOMPARE(link.waveform.size(), wf.isEmpty() ? 0 : MediaLink::kWaveformLevels);
    // The identity only changes with a valid sha.
    if (sha.isEmpty())
        QCOMPARE(link.key(), before.key());
    else
        QVERIFY(link.key() != before.key());
    // What was accepted is written back the same way.
    QCOMPARE(MediaLink::parse(link.toUrl()).toUrl(), link.toUrl());
}

void TestLink22::waveformPacking()
{
    MediaLink link = voiceMessage();
    QByteArray levels;
    for (int i = 0; i < MediaLink::kWaveformLevels; ++i)
        levels.append(static_cast<char>(QRandomGenerator::global()->bounded(16)));
    link.waveform          = levels;
    const MediaLink parsed = MediaLink::parse(link.toUrl());
    QCOMPARE(parsed.waveform, levels);

    for (const QByteArray& flat : {QByteArray(64, '\0'), QByteArray(64, '\x0f')}) {
        link.waveform = flat;
        QCOMPARE(MediaLink::parse(link.toUrl()).waveform, flat);
    }
    QVERIFY(MediaLink::parse(link.toUrl()).toUrl().contains(QStringLiteral("&wf=__________________________________________8")));

    // Out-of-range levels or the wrong count are not sent.
    link.waveform = QByteArray(64, '\x10');
    QVERIFY(!link.toUrl().contains(QStringLiteral("&wf=")));
    link.waveform = QByteArray(48, '\x01');
    QVERIFY(!link.toUrl().contains(QStringLiteral("&wf=")));
}

void TestLink22::keyWithSha()
{
    const QString base = QStringLiteral("ts3file://voice.example.org?port=9987&serverUID=uid&channel=3&path=%2Ftsmedia&filename=clip.mp4&isDir=0&size=100&fileDateTime=1700000000&tsm=2");
    // Without a sha: v1's key (also what keyMatchesV1 checks).
    QCOMPARE(MediaLink::parse(base).key(), QStringLiteral("6aa7f62c6e2880efae24"));
    // With one: sha1(v1 raw + "\n" + base64url(sha)).
    const QString raw = QStringLiteral("uid\n3\n/tsmedia/clip.mp4\n100\n") + kShaEmpty;
    QCOMPARE(MediaLink::parse(base + QStringLiteral("&sha=") + kShaEmpty).key(), QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex().left(20)));
    QVERIFY(MediaLink::parse(base + QStringLiteral("&sha=") + kShaEmpty).key() != MediaLink::parse(base + QStringLiteral("&sha=") + kShaAbc).key());

    // Nothing else changes it.
    const MediaLink full  = photo();
    MediaLink       other = full;
    other.spoiler         = true;
    other.albumId         = 9;
    other.albumIndex      = 1;
    other.albumCount      = 2;
    other.previewSha      = digest("x").left(16);
    other.blurHash.clear();
    other.dateTime += 5;
    QCOMPARE(other.key(), full.key());
    MediaLink voice = voiceMessage();
    const QString voiceKey = voice.key();
    voice.waveform.clear();
    voice.voice = false;
    QCOMPARE(voice.key(), voiceKey);

    // A hash that is never sent (plain TeamSpeak link, wrong size) is no part of the key.
    MediaLink plain = without22(full);
    const QString v1Key = plain.key();
    plain.protocol = 0;
    plain.sha256   = digest("");
    QCOMPARE(plain.key(), v1Key);
    plain.protocol = 2;
    plain.sha256   = QByteArray(20, 'x');
    QCOMPARE(plain.key(), v1Key);
}

void TestLink22::dropInvalidMetadata()
{
    MediaLink link = photo(QStringLiteral("archive_3f9a1c2e.zip"));
    link.spoiler   = true; // not a picture or video
    link.voice     = true; // not audio
    link.waveform  = rampLevels();
    link.dropInvalidMetadata();
    QVERIFY(!link.spoiler);
    QVERIFY(!link.voice);
    QVERIFY(link.waveform.isEmpty());
    QCOMPARE(link.sha256, digest("")); // fine on any kind of file

    MediaLink noPreview = photo();
    noPreview.previewFile.clear();
    noPreview.dropInvalidMetadata();
    QVERIFY(noPreview.previewSha.isEmpty());

    MediaLink half  = photo();
    half.albumId    = 5;
    half.albumIndex = 3;
    half.albumCount = 2;
    half.dropInvalidMetadata();
    QCOMPARE(albumText(half), QString());
}

void TestLink22::findInMessageSkipsNoparse()
{
    const MediaLink real = photo();
    MediaLink       fake = photo(QStringLiteral("fake_00000000.jpg"));
    const QString   bb   = fake.toBBCode();
    // A caption can carry link text, but only as plain text: TeamSpeak shows it, nothing fetches it.
    QCOMPARE(MediaLink::findInMessage(QStringLiteral("[noparse]") + bb + QStringLiteral("[/noparse]\n") + real.toBBCode()).size(), 1);
    QCOMPARE(MediaLink::findInMessage(QStringLiteral("[NoParse]") + bb + QStringLiteral("[/NOPARSE] ") + real.toBBCode()).first().fileName, real.fileName);
    // An unclosed [noparse] runs to the end of the message.
    QCOMPARE(MediaLink::findInMessage(real.toBBCode() + QStringLiteral(" [noparse]") + bb).size(), 1);
    // A [noparse] can't be closed early by the fake to expose the rest of it.
    const QString tricky = captionToBBCode(sanitizeCaption(QStringLiteral("[/noparse]") + bb));
    QCOMPARE(MediaLink::findInMessage(tricky).size(), 0);
    // A caption made by the composer never yields an extra link.
    ComposeOptions options;
    options.caption = bb + QStringLiteral(" [url=ts3file://x?serverUID=a&channel=1&filename=b.png]y[/url]");
    for (const QString& message : composeChatMessages({real}, options)) {
        for (const MediaLink& link : MediaLink::findInMessage(message))
            QCOMPARE(link.fileName, real.fileName);
    }
}

// ---- composer ----------------------------------------------------------------------------------

void TestLink22::composeEmptyCaptionMatches21()
{
    // No caption, 2.1 labels: the same text as 2.1 for 2.1-shaped links (see v21LinksAreByteIdentical),
    // and with friendly labels only the label differs.
    const MediaLink link = without22(photo());
    ComposeOptions  options;
    const QStringList messages = composeChatMessages({link}, options);
    QCOMPARE(messages.size(), 1);
    QString expected = v21::composeChatMessage(toV21(link), true, QString());
    expected.replace(QStringLiteral("]sunset_3f9a1c2e.jpg[/URL]"), QStringLiteral("]sunset.jpg[/URL]"));
    QCOMPARE(messages.first(), expected);
    QCOMPARE(composeChatMessages({}, options), QStringList());
    options.caption = QStringLiteral("   ");
    QCOMPARE(composeChatMessages({link}, options), messages); // a blank caption is no caption
}

void TestLink22::composeCaptionFirst()
{
    const MediaLink link = photo();
    ComposeOptions  options;
    options.caption = QStringLiteral("  Sunset at\nthe lake  ");
    const QVector<ComposedMessage> messages = composeChatMessagesDetailed({link}, options);
    QCOMPARE(messages.size(), 1);
    const QString text = messages.first().text;
    QVERIFY2(text.startsWith(QStringLiteral("Sunset at the lake") + QString::fromLatin1(kMessageSeparator) + QStringLiteral("[URL=ts3file://")), qPrintable(text));
    QVERIFY(text.endsWith(QStringLiteral("plugin required to view this in chat[/I][/COLOR]")));
    QVERIFY(text.contains(QStringLiteral("]sunset.jpg[/URL] [COLOR=#72767d]")));
    QVERIFY(utf8Bytes(text) < kMaxMessageBytes);
    QCOMPARE(messages.first().links, QVector<int>{0});
    QVERIFY(messages.first().dropped.isEmpty());
    QCOMPARE(MediaLink::findInMessage(text).size(), 1);
    QCOMPARE(MediaLink::findInMessage(text).first().toUrl(), link.toUrl());
}

void TestLink22::composeCaptionSplit()
{
    // A long caption next to a long name doesn't fit in one message: it goes first, on its own, and
    // the media message keeps everything, the note included.
    const MediaLink link = photo(QStringLiteral("فایل_خیلی_طولانی_برای_آزمایش_اندازه_پیام_3f9a1c2e.jpg"));
    ComposeOptions  options;
    options.downloadUrl = QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat");
    options.caption     = QString(kCaptionMaxChars, QChar(0x0633)); // 300 Persian letters = 600 bytes
    const QVector<ComposedMessage> messages = composeChatMessagesDetailed({link}, options);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).text, options.caption);
    QVERIFY(messages.at(0).links.isEmpty());
    QCOMPARE(messages.at(1).links, QVector<int>{0});
    QVERIFY(messages.at(1).text.contains(QStringLiteral("[URL=https://github.com/Metihttp/Teamspeak_Media_chat]TS Media chat[/URL] plugin required")));
    QVERIFY2(messages.at(1).dropped.isEmpty(), qPrintable(messages.at(1).dropped.join(QLatin1Char(','))));
    for (const ComposedMessage& m : messages)
        QVERIFY(utf8Bytes(m.text) < kMaxMessageBytes && !m.tooLong);

    // Before splitting, only steps that keep the note are tried: here dropping the preview hash is enough.
    options.caption = QString(80, QLatin1Char('c'));
    ComposeOptions bigger = options;
    const int      full   = utf8Bytes(composeChatMessages({link}, bigger).first());
    bigger.maxBytes       = full - 25; // "&ph=" + 22 characters = 26 bytes
    const QVector<ComposedMessage> tight = composeChatMessagesDetailed({link}, bigger);
    QCOMPARE(tight.size(), 1);
    QCOMPARE(tight.first().dropped, QStringList{QStringLiteral("ph")});
    QVERIFY(tight.first().text.startsWith(options.caption + QString::fromLatin1(kMessageSeparator)));
}

void TestLink22::composeCaptionNeverCut()
{
    // Even when nothing fits, the caption is sent whole (on its own) and the links follow.
    const MediaLink link = photo();
    ComposeOptions  options;
    options.caption  = QStringLiteral("Every word of this caption stays");
    options.maxBytes = 40;
    const QVector<ComposedMessage> messages = composeChatMessagesDetailed({link, link}, options);
    QCOMPARE(messages.first().text, options.caption);
    QVERIFY(messages.first().links.isEmpty());
    QCOMPARE(messages.size(), 3); // caption, then one link per message
    QVERIFY(messages.at(1).tooLong && messages.at(2).tooLong);
    // Linked addresses cost bytes; a caption that can't go out with them goes as plain text.
    options.caption = QStringLiteral("[x] ");
    for (int i = 0; i < 30; ++i)
        options.caption += QStringLiteral("http://a ");
    options.maxBytes = 300;
    QVERIFY(utf8Bytes(captionToBBCode(sanitizeCaption(options.caption))) >= 300);
    QCOMPARE(composeChatMessages({}, options).first(), QStringLiteral("[noparse]") + sanitizeCaption(options.caption) + QStringLiteral("[/noparse]"));
}

void TestLink22::composeCascadeOrder_data()
{
    QTest::addColumn<bool>("voice");
    QTest::addColumn<bool>("caption");
    QTest::addColumn<QStringList>("order");
    QTest::newRow("photo") << false << false << QStringList{"ph", "bh", "note link", "note", "pv", "w/h/d", "sha"};
    QTest::newRow("photo with caption") << false << true << QStringList{"ph", "bh", "note link", "note", "pv", "w/h/d", "sha"};
    QTest::newRow("voice") << true << false << QStringList{"wf", "note link", "note", "w/h/d", "sha"};
}

void TestLink22::composeCascadeOrder()
{
    QFETCH(bool, voice);
    QFETCH(bool, caption);
    QFETCH(QStringList, order);
    MediaLink link = voice ? voiceMessage() : photo();
    if (!voice) {
        link.spoiler    = true;
        link.albumId    = 0x7c1e09ab;
        link.albumIndex = 2;
        link.albumCount = 4;
    }
    ComposeOptions options;
    options.downloadUrl = QStringLiteral("https://example.com/tsmedia");
    if (caption)
        options.caption = QStringLiteral("A caption");

    // Shrink the limit one byte at a time: the dropped list only ever grows, in exactly this order.
    // With a caption: first only the steps that keep the note (up to the note's link) with the caption
    // in the message; then the caption goes on its own and the media message is the one without caption.
    int  last     = -1;
    bool wasSplit = false;
    for (int max = 1500; max > 100; --max) {
        options.maxBytes                       = max;
        const QVector<ComposedMessage> list    = composeChatMessagesDetailed({link}, options);
        const ComposedMessage&         media   = list.last();
        const QStringList              dropped = media.dropped;
        // (Compared as text: QList's operator== goes through MSVC's deprecated checked_array_iterator.)
        QVERIFY2(dropped.join(QLatin1Char(',')) == order.mid(0, dropped.size()).join(QLatin1Char(',')), qPrintable(QString::number(max) + QStringLiteral(": ") + dropped.join(QLatin1Char(','))));
        if (caption) {
            QVERIFY(list.first().text.startsWith(QStringLiteral("A caption")));
            const bool split = list.size() == 2;
            QVERIFY(!wasSplit || split); // once split, it stays split
            if (!split) {
                QVERIFY(!dropped.contains(QStringLiteral("note")));
                QVERIFY(dropped.size() >= last);
                last = dropped.size();
            } else {
                // Split only when even the leanest message that keeps the note was too long.
                if (!wasSplit)
                    QCOMPARE(last, order.indexOf(QStringLiteral("note link")) + 1);
                ComposeOptions alone = options;
                alone.caption.clear();
                QCOMPARE(media.text, composeChatMessages({link}, alone).first());
                wasSplit = true;
                last     = dropped.size();
            }
        } else {
            QVERIFY(dropped.size() >= last);
            last = dropped.size();
        }
        // Never dropped.
        const MediaLink sent = MediaLink::findInMessage(media.text).value(0);
        QVERIFY(sent.isValid() && sent.isTsMedia());
        QCOMPARE(sent.spoiler, link.spoiler);
        QCOMPARE(albumText(sent), albumText(link));
        QCOMPARE(sent.voice, link.voice);
        QCOMPARE(sent.remoteFile(), link.remoteFile());
        QCOMPARE(sent.size, link.size);
        if (!media.tooLong)
            QVERIFY(utf8Bytes(media.text) < max);
    }
    QCOMPARE(last, order.size()); // every step was reached
    QVERIFY(!caption || wasSplit);
}

void TestLink22::composeNeverDrops()
{
    // A pathological link: sp, the album and vm survive even when the message can't fit.
    MediaLink link  = photo(QString(48, QChar(0x4E2D)) + QStringLiteral("_3f9a1c2e.mp4"));
    link.host       = QString(64, QLatin1Char('h'));
    link.spoiler    = true;
    link.albumId    = 1;
    link.albumIndex = 1;
    link.albumCount = 2;
    ComposeOptions options;
    options.maxBytes = 200;
    const ComposedMessage message = composeChatMessagesDetailed({link}, options).first();
    QVERIFY(message.tooLong);
    const MediaLink sent = MediaLink::findInMessage(message.text).first();
    QVERIFY(sent.spoiler);
    QCOMPARE(albumText(sent), albumText(link));
    QVERIFY(sent.sha256.isEmpty() && sent.blurHash.isEmpty() && sent.previewFile.isEmpty() && sent.width == 0);
}

void TestLink22::composeAlbumPacking_data()
{
    QTest::addColumn<int>("maxBytes");
    QTest::addColumn<QString>("caption");
    QTest::newRow("1000, no caption") << 1000 << QString();
    QTest::newRow("1000, 300-byte caption") << 1000 << QString(300, QLatin1Char('w'));
    QTest::newRow("1000, Persian caption") << 1000 << QString(300, QChar(0x0645));
    QTest::newRow("8000") << 8000 << QStringLiteral("Trip photos");
    QTest::newRow("4500") << 4500 << QString();
}

void TestLink22::composeAlbumPacking()
{
    QFETCH(int, maxBytes);
    QFETCH(QString, caption);
    QList<MediaLink> links;
    for (int i = 0; i < MediaLink::kMaxAlbumItems; ++i) {
        MediaLink link  = photo(QStringLiteral("IMG_20%1_%2.jpg").arg(41 + i).arg(QStringLiteral("%1").arg(0x10000000u + static_cast<quint32>(i), 8, 16, QLatin1Char('0'))));
        link.albumId    = 0x7c1e09ab;
        link.albumIndex = i + 1;
        link.albumCount = MediaLink::kMaxAlbumItems;
        links << link;
    }
    ComposeOptions options;
    options.caption  = caption;
    options.maxBytes = maxBytes;
    const QVector<ComposedMessage> messages = composeChatMessagesDetailed(links, options);

    const QString note       = QStringLiteral("plugin required to view this in chat");
    int           withNote   = 0;
    int           withCaption = 0;
    QList<MediaLink> found;
    for (int m = 0; m < messages.size(); ++m) {
        const ComposedMessage& message = messages.at(m);
        QVERIFY2(utf8Bytes(message.text) < maxBytes && !message.tooLong, qPrintable(QString::number(utf8Bytes(message.text))));
        QVERIFY(message.dropped.isEmpty()); // packing never drops metadata
        withNote += message.text.contains(note) ? 1 : 0;
        withCaption += !caption.isEmpty() && message.text.startsWith(sanitizeCaption(caption)) ? 1 : 0;
        found += MediaLink::findInMessage(message.text);
        // Greedy: the next message's first link would not have fitted into this one.
        if (m + 1 < messages.size() && !message.links.isEmpty() && !messages.at(m + 1).links.isEmpty()) {
            const QString next = links.at(messages.at(m + 1).links.first()).toBBCode(linkLabel(links.at(messages.at(m + 1).links.first())));
            QString       grown = message.text;
            const int     at    = grown.indexOf(QStringLiteral(" [COLOR=#72767d]"));
            grown.insert(at < 0 ? grown.size() : at, QString::fromLatin1(kMessageSeparator) + next);
            QVERIFY(utf8Bytes(grown) >= maxBytes);
        }
    }
    QCOMPARE(withNote, 1);
    QCOMPARE(withCaption, caption.isEmpty() ? 0 : 1);
    QCOMPARE(found.size(), MediaLink::kMaxAlbumItems);
    for (int i = 0; i < found.size(); ++i) {
        QCOMPARE(found.at(i).albumIndex, i + 1);
        QCOMPARE(found.at(i).toUrl(), links.at(i).toUrl());
    }
    // The note is in the first message with links, after its last link.
    for (const ComposedMessage& message : messages) {
        if (message.links.isEmpty())
            continue;
        QVERIFY(message.text.endsWith(QStringLiteral("[/I][/COLOR]")));
        break;
    }
    if (maxBytes >= 8000)
        QCOMPARE(messages.size(), 1); // a whole album in one message
    if (maxBytes == 1000 && caption.isEmpty())
        QCOMPARE(messages.size(), 5); // two typical items per message
    if (maxBytes == 1000 && !caption.isEmpty())
        QCOMPARE(messages.size(), 6); // the caption takes the room of one item
}

void TestLink22::composeSizeRule22()
{
    // Names of every script at the 2.2 bounds, every field combination, captions up to the limit:
    // every message fits, nothing that must stay is lost, and the caption is never changed.
    const QStringList bases = {QString(60, QLatin1Char('a')), QString(60, QChar(0x0641)), QString(60, QChar(0x4E2D)),
                               QString::fromUtf8("😀😃😄😁😆😅🤣😂🙂🙃😉😊😇🥰😍🤩😘😗☺😚")};
    const QStringList exts    = {QStringLiteral(".jpg"), QStringLiteral(".gif"), QStringLiteral(".mp4"), QStringLiteral(".m4a"), QStringLiteral(".zip")};
    const QStringList captions = {QString(), QStringLiteral("Short one"), QString(kCaptionMaxChars, QLatin1Char('x')), QString(kCaptionMaxChars, QChar(0x0633)),
                                  QString(150, QChar(0x4E2D)), QStringLiteral("see [b]this[/b] https://example.com/x [noparse]")};
    int checked = 0;
    for (const QString& base : bases) {
        for (const QString& ext : exts) {
            for (int mask = 0; mask < 16; ++mask) {
                MediaLink link = photo(boundRemoteBase(base) + QStringLiteral("_3f9a1c2e") + ext);
                QVERIFY(utf8Bytes(boundRemoteBase(base)) <= kRemoteBaseMaxBytes);
                if (mask & 1)
                    link.spoiler = true;
                if (mask & 2) {
                    link.albumId    = 0x7c1e09ab;
                    link.albumIndex = 9;
                    link.albumCount = 10;
                }
                if (mask & 4) {
                    link.voice      = true;
                    link.waveform   = rampLevels();
                    link.durationMs = 3599000;
                }
                if (mask & 8)
                    link.sha256.clear();
                for (const QString& caption : captions) {
                    ComposeOptions options;
                    options.caption     = caption;
                    options.downloadUrl = QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat");
                    const QVector<ComposedMessage> messages = composeChatMessagesDetailed({link, link, link}, options);
                    QList<MediaLink> found;
                    for (const ComposedMessage& m : messages) {
                        QVERIFY2(!m.tooLong && utf8Bytes(m.text) < kMaxMessageBytes, qPrintable(m.text));
                        found += MediaLink::findInMessage(m.text);
                    }
                    QCOMPARE(found.size(), 3);
                    MediaLink valid = link;
                    valid.dropInvalidMetadata();
                    for (const MediaLink& f : found) {
                        QCOMPARE(f.remoteFile(), link.remoteFile());
                        QCOMPARE(f.spoiler, valid.spoiler);
                        QCOMPARE(albumText(f), albumText(valid));
                        QCOMPARE(f.voice, valid.voice);
                    }
                    const QString clean = sanitizeCaption(caption);
                    if (!clean.isEmpty())
                        QVERIFY(messages.first().text.startsWith(captionToBBCode(clean)));
                    ++checked;
                }
            }
        }
    }
    QVERIFY(checked > 1000);
}

void TestLink22::composeHostileCaptions()
{
    // Whatever the caption, the messages hold exactly the links they were given, plus at most the
    // note's web link, and every [noparse] is closed.
    const MediaLink   link     = photo();
    const QStringList captions = {QStringLiteral("[/noparse][URL=ts3file://evil?serverUID=a&channel=1&filename=x.png]x[/URL]"),
                                  QStringLiteral("[URL]javascript:alert(1)[/URL]"),
                                  QStringLiteral("[img]https://tracker.example/pixel.png[/img]"),
                                  QStringLiteral("https://a.example/[b] http://b.example\"onmouseover=x"),
                                  QStringLiteral("\u202Egpj.exe\u202C [noparse][/NOPARSE] [/noparse]"),
                                  QString(kCaptionMaxChars, QLatin1Char('[')),
                                  QStringLiteral("x") + QChar(0) + QStringLiteral("y") + QChar(7) + QStringLiteral("z") + QChar(0x85) + QStringLiteral("w") + QChar(0x2028) + QStringLiteral("v")};
    for (const QString& caption : captions) {
        ComposeOptions options;
        options.caption = caption;
        for (const QString& message : composeChatMessages({link}, options)) {
            QVERIFY(utf8Bytes(message) < kMaxMessageBytes);
            QVERIFY(!message.contains(QChar(0x202E)) && !message.contains(QChar(0)) && !message.contains(QChar(7)));
            QVERIFY2(noparseClosed(message), qPrintable(message));
            for (const MediaLink& l : MediaLink::findInMessage(message))
                QCOMPARE(l.toUrl(), link.toUrl());
            QVERIFY(!message.contains(QStringLiteral("[img]"), Qt::CaseInsensitive) || message.contains(QStringLiteral("[noparse][img]"), Qt::CaseInsensitive));
            QVERIFY(!message.contains(QStringLiteral("[URL]javascript"), Qt::CaseInsensitive) || message.contains(QStringLiteral("[noparse][URL]javascript"), Qt::CaseInsensitive));
        }
    }
}

// ---- captions, labels, names -------------------------------------------------------------------

void TestLink22::sanitizeCaptionTexts_data()
{
    QTest::addColumn<QString>("typed");
    QTest::addColumn<QString>("clean");
    QTest::newRow("plain") << QStringLiteral("Sunset at the lake") << QStringLiteral("Sunset at the lake");
    QTest::newRow("trim and collapse") << QStringLiteral("  a \t\t b\r\n\r\nc  ") << QStringLiteral("a b c");
    QTest::newRow("line separators") << QStringLiteral("a\u2028b\u2029c\u0085d") << QStringLiteral("a b c d");
    QTest::newRow("controls") << QStringLiteral("a\u0001b\u007fc\u009fd") << QStringLiteral("abcd");
    QTest::newRow("bidi") << QStringLiteral("\u202Egpj.exe\u202C \u2066x\u2069 \u200Fy\u200E") << QStringLiteral("gpj.exe x y");
    QTest::newRow("persian") << QStringLiteral("غروب آفتاب کنار دریاچه") << QStringLiteral("غروب آفتاب کنار دریاچه");
    QTest::newRow("zwj emoji kept") << QString::fromUtf8("👨\u200D👩\u200D👧 family") << QString::fromUtf8("👨\u200D👩\u200D👧 family");
    QTest::newRow("only spaces") << QStringLiteral(" \n\t ") << QString();
    QTest::newRow("long") << QString(400, QLatin1Char('a')) << QString(300, QLatin1Char('a'));
    QTest::newRow("emoji across the limit") << QString(299, QLatin1Char('a')) + QString::fromUtf8("😀") << QString(299, QLatin1Char('a'));
    QTest::newRow("emoji at the limit") << QString(298, QLatin1Char('a')) + QString::fromUtf8("😀b") << QString(298, QLatin1Char('a')) + QString::fromUtf8("😀");
    QTest::newRow("space at the limit") << QString(299, QLatin1Char('a')) + QStringLiteral(" b") << QString(299, QLatin1Char('a'));
    QTest::newRow("lone surrogates") << QString(QChar(0xD800)) + QStringLiteral("a") + QString(QChar(0xDC00)) << QStringLiteral("a");
}

void TestLink22::sanitizeCaptionTexts()
{
    QFETCH(QString, typed);
    QFETCH(QString, clean);
    const QString out = ::sanitizeCaption(typed);
    QCOMPARE(out, clean);
    QVERIFY(out.size() <= kCaptionMaxChars);
    QVERIFY(out.isEmpty() || !out.at(out.size() - 1).isHighSurrogate());
    QCOMPARE(::sanitizeCaption(out), out); // stable
}

void TestLink22::captionToBBCodeTexts_data()
{
    QTest::addColumn<QString>("caption");
    QTest::addColumn<QString>("bbcode");
    QTest::newRow("plain") << QStringLiteral("hello world") << QStringLiteral("hello world");
    QTest::newRow("example") << QStringLiteral("see [this] https://x.y/a") << QStringLiteral("[noparse]see [this] [/noparse][URL]https://x.y/a[/URL]");
    QTest::newRow("tags") << QStringLiteral("[b]x[/b]") << QStringLiteral("[noparse][b]x[/b][/noparse]");
    QTest::newRow("noparse defused") << QStringLiteral("a [/noparse] b") << QStringLiteral("[noparse]a [ /noparse] b[/noparse]");
    QTest::newRow("noparse any case") << QStringLiteral("[/NoParse]") << QStringLiteral("[noparse][ /noparse][/noparse]");
    QTest::newRow("http") << QStringLiteral("http://example.com") << QStringLiteral("[URL]http://example.com[/URL]");
    QTest::newRow("upper case scheme") << QStringLiteral("HTTPS://Example.com/A") << QStringLiteral("[URL]HTTPS://Example.com/A[/URL]");
    QTest::newRow("two addresses") << QStringLiteral("https://a.b c https://d.e") << QStringLiteral("[URL]https://a.b[/URL] c [URL]https://d.e[/URL]");
    QTest::newRow("javascript") << QStringLiteral("javascript:alert(1)") << QStringLiteral("javascript:alert(1)");
    QTest::newRow("ftp") << QStringLiteral("ftp://example.com") << QStringLiteral("ftp://example.com");
    QTest::newRow("glued") << QStringLiteral("xhttps://example.com") << QStringLiteral("xhttps://example.com");
    QTest::newRow("bracket ends the address") << QStringLiteral("https://a.b/[c]") << QStringLiteral("[URL]https://a.b/[/URL][noparse][c][/noparse]");
    QTest::newRow("quote ends the address") << QStringLiteral("https://a.b/\"x") << QStringLiteral("[URL]https://a.b/[/URL]\"x");
    QTest::newRow("scheme only") << QStringLiteral("https://") << QStringLiteral("https://");
    QTest::newRow("persian") << QStringLiteral("سلام [دنیا]") << QStringLiteral("[noparse]سلام [دنیا][/noparse]");
    QTest::newRow("draft") << QStringLiteral("[draft] a[1]") << QStringLiteral("[noparse][draft] a[1][/noparse]");
}

void TestLink22::captionToBBCodeTexts()
{
    QFETCH(QString, caption);
    QFETCH(QString, bbcode);
    QCOMPARE(::captionToBBCode(caption), bbcode);
}

void TestLink22::linkLabels_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<int>("protocol");
    QTest::addColumn<bool>("spoiler");
    QTest::addColumn<bool>("voice");
    QTest::addColumn<qint64>("durationMs");
    QTest::addColumn<QString>("label");
    QTest::newRow("upload") << QStringLiteral("holiday_3f9a1c2e.jpg") << 2 << false << false << qint64(0) << QStringLiteral("holiday.jpg");
    QTest::newRow("pasted") << QStringLiteral("new_photo_3f9a1c2e.png") << 2 << false << false << qint64(0) << QStringLiteral("Pasted image.png");
    QTest::newRow("plain link keeps its name") << QStringLiteral("holiday_3f9a1c2e.jpg") << 0 << false << false << qint64(0) << QStringLiteral("holiday_3f9a1c2e.jpg");
    QTest::newRow("spoiler image") << QStringLiteral("holiday_3f9a1c2e.jpg") << 2 << true << false << qint64(0) << QStringLiteral("Spoiler (image)");
    QTest::newRow("spoiler GIF") << QStringLiteral("cat_3f9a1c2e.gif") << 2 << true << false << qint64(0) << QStringLiteral("Spoiler (GIF)");
    QTest::newRow("spoiler video") << QStringLiteral("ending_3f9a1c2e.mp4") << 2 << true << false << qint64(9000) << QStringLiteral("Spoiler (video)");
    QTest::newRow("spoiler on a zip is ignored") << QStringLiteral("files_3f9a1c2e.zip") << 2 << true << false << qint64(0) << QStringLiteral("files.zip");
    QTest::newRow("voice") << QStringLiteral("voice_message_5b0c77e1.m4a") << 2 << false << true << qint64(12480) << QStringLiteral("Voice message (0:12)");
    QTest::newRow("voice, long") << QStringLiteral("voice_message_5b0c77e1.m4a") << 2 << false << true << qint64(3723000) << QStringLiteral("Voice message (1:02:03)");
    QTest::newRow("voice, no duration") << QStringLiteral("voice_message_5b0c77e1.m4a") << 2 << false << true << qint64(0) << QStringLiteral("Voice message");
    QTest::newRow("brackets") << QStringLiteral("[draft] notes_3f9a1c2e.txt") << 2 << false << false << qint64(0) << QStringLiteral("(draft) notes.txt");
    QTest::newRow("bidi stripped") << QStringLiteral("\u202Egpj.exe_3f9a1c2e.png") << 2 << false << false << qint64(0) << QStringLiteral("gpj.exe.png");
    QTest::newRow("nothing left") << QStringLiteral("\u202E\u202C") << 0 << false << false << qint64(0) << QStringLiteral("File");
}

void TestLink22::linkLabels()
{
    QFETCH(QString, fileName);
    QFETCH(int, protocol);
    QFETCH(bool, spoiler);
    QFETCH(bool, voice);
    QFETCH(qint64, durationMs);
    QFETCH(QString, label);
    MediaLink link  = photo(fileName);
    link.protocol   = protocol;
    link.spoiler    = spoiler;
    link.voice      = voice;
    link.durationMs = durationMs;
    QCOMPARE(::linkLabel(link), label);
    QVERIFY(!::linkLabel(link).contains(QLatin1Char('[')) && !::linkLabel(link).contains(QLatin1Char(']')));
}

void TestLink22::boundRemoteBaseNames_data()
{
    QTest::addColumn<QString>("base");
    QTest::addColumn<int>("chars"); // UTF-16 units left
    QTest::addColumn<int>("bytes");
    QTest::newRow("ascii 60") << QString(60, QLatin1Char('a')) << 48 << 48;
    QTest::newRow("ascii 48") << QString(48, QLatin1Char('a')) << 48 << 48;
    QTest::newRow("short") << QStringLiteral("holiday") << 7 << 7;
    QTest::newRow("persian 40") << QString(40, QChar(0x0641)) << 32 << 64;
    QTest::newRow("cjk 30") << QString(30, QChar(0x4E2D)) << 21 << 63;
    QTest::newRow("emoji 20") << QString::fromUtf8("😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀") << 32 << 64;
    QTest::newRow("ascii then emoji") << QString(61, QLatin1Char('a')) + QString::fromUtf8("😀") << 48 << 48;
    QTest::newRow("emoji does not split") << QString(62, QLatin1Char('a')) + QString::fromUtf8("😀") << 48 << 48;
    QTest::newRow("bytes bind first") << QString(30, QLatin1Char('a')) + QString(20, QChar(0x0641)) << 47 << 64;
    QTest::newRow("lone high surrogate") << QStringLiteral("ab") + QString(QChar(0xD83D)) + QStringLiteral("cd") << 4 << 4;
    QTest::newRow("lone low surrogate") << QString(QChar(0xDE00)) + QStringLiteral("x") << 1 << 1;
    QTest::newRow("empty") << QString() << 0 << 0;
}

void TestLink22::boundRemoteBaseNames()
{
    QFETCH(QString, base);
    QFETCH(int, chars);
    QFETCH(int, bytes);
    const QString out = ::boundRemoteBase(base);
    QCOMPARE(out.size(), chars);
    QCOMPARE(utf8Bytes(out), bytes);
    QVERIFY(out.size() <= kRemoteBaseMaxChars && utf8Bytes(out) <= kRemoteBaseMaxBytes);
    for (int i = 0; i < out.size(); ++i) {
        if (out.at(i).isHighSurrogate())
            QVERIFY(i + 1 < out.size() && out.at(++i).isLowSurrogate());
        else
            QVERIFY(!out.at(i).isLowSurrogate());
    }
}

void TestLink22::remoteSuffixes()
{
    QCOMPARE(::remoteSuffixOf(QStringLiteral("holiday_3f9a1c2e.jpg")), QStringLiteral("3f9a1c2e"));
    QCOMPARE(::remoteSuffixOf(QStringLiteral("new_photo_00ff00ff.png")), QStringLiteral("00ff00ff"));
    QCOMPARE(::remoteSuffixOf(QStringLiteral("archive.tar_3f9a1c2e.gz")), QStringLiteral("3f9a1c2e")); // makeRemoteName keeps ".tar" in the base
    QCOMPARE(::remoteSuffixOf(QStringLiteral("noext_3f9a1c2e")), QStringLiteral("3f9a1c2e"));
    QCOMPARE(::remoteSuffixOf(QStringLiteral("holiday_3F9A1C2E.jpg")), QString()); // ours are lower case
    QCOMPARE(::remoteSuffixOf(QStringLiteral("holiday.jpg")), QString());
    QCOMPARE(::remoteSuffixOf(QStringLiteral("x_3f9a1c2.jpg")), QString());
}

TSMEDIA_REGISTER_TEST(TestLink22)

#include "tst_link22.moc"
