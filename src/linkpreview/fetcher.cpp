#include "fetcher.h"

#include <windows.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <map>
#include <vector>

#include "content.h"
#include "mediaprobe.h"
#include "pagemeta.h"
#include "providers.h"
#include "safeimage.h"
#include "shares.h"
#include "textclean.h"
#include "transport.h"

namespace linkpreview {

struct Fetcher::Link {
    QMutex            mutex;
    QPointer<Fetcher> receiver; // cleared (under mutex) by shutdown(): nothing is posted after that
};

namespace {

constexpr int    kReadChunk        = 64 * 1024;
constexpr int    kProgressEveryMs  = 100;
constexpr qint64 kMaxMetaBytes     = 16 * 1024;
constexpr int    kMetaVersion      = 1;
constexpr int    kMetaVersion2     = 2;
constexpr qint64 kCardFreshSec     = 7 * 24 * 3600;
constexpr qint64 kNothingFreshSec  = 24 * 3600;
constexpr qint64 kWalledFreshSec   = 30 * 60;
constexpr int    kMaxPictureSide   = 16384;
constexpr int    kSniffPageBytes   = 512;
constexpr qint64 kLoopMaxMs        = 60 * 1000;

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

Result failure(const char* code)
{
    Result result;
    result.error = latin(code);
    return result;
}

Result failure(const QString& code)
{
    Result result;
    result.error = code;
    return result;
}

const char* errorFor(http::Error error)
{
    switch (error) {
    case http::Error::None:
        return "";
    case http::Error::Unavailable:
        return errors::kNoNetwork;
    case http::Error::Offline:
        return errors::kOffline;
    case http::Error::Timeout:
        return errors::kTimeout;
    case http::Error::Tls:
        return errors::kTls;
    case http::Error::Blocked:
        return errors::kBlocked;
    case http::Error::Canceled:
        return errors::kCanceled;
    case http::Error::BadResponse:
        break;
    }
    return errors::kBadResponse;
}

HostGate::Outcome outcomeFor(http::Error error)
{
    switch (error) {
    case http::Error::Offline:
        return HostGate::Outcome::Offline;
    case http::Error::Timeout:
        return HostGate::Outcome::Timeout;
    default:
        return HostGate::Outcome::Other;
    }
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const char* errorForStatus(int status, const http::Head& head)
{
    if (head.challenge)
        return errors::kWalled;
    if (status == 404 || status == 410)
        return errors::kNotFound;
    if (status == 401 || status == 403)
        return errors::kForbidden;
    if (status == 429 || (status == 503 && head.retryAfterMs >= 0))
        return errors::kRateLimited;
    if (status >= 500 && status <= 599)
        return errors::kServerError;
    return errors::kBadResponse;
}

QByteArray acceptFor(Kind kind)
{
    switch (kind) {
    case Kind::Gif:
        return "image/gif,image/*;q=0.8";
    case Kind::Image:
    case Kind::Card:
        return "image/png,image/jpeg,image/webp,image/gif;q=0.9";
    case Kind::Video:
    case Kind::Loop:
        return "video/mp4,video/webm,video/quicktime,video/*;q=0.8";
    case Kind::YouTube:
        return "image/jpeg,image/webp;q=0.9";
    case Kind::None:
        break;
    }
    return "*/*";
}

// The picture formats for Image / Gif / Card pictures; the video formats for Video / Loop.
Kind familyOf(Kind kind)
{
    return kind == Kind::Video || kind == Kind::Loop ? Kind::Video : Kind::Image;
}

bool replaceFile(const QString& from, const QString& to)
{
    return MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(from).utf16()), reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(to).utf16()),
                       MOVEFILE_REPLACE_EXISTING)
           != 0;
}

// Writes data next to path first, then puts it in place.
bool writeFileAtomic(const QString& path, const QByteArray& data, quint64 jobId)
{
    const QString part = path + QStringLiteral(".%1.part").arg(jobId);
    QFile         file(part);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool written = file.write(data) == data.size() && file.flush();
    file.close();
    if (!written || !replaceFile(part, path)) {
        QFile::remove(part);
        return false;
    }
    return true;
}

bool isMediaExtension(Kind kind, const QString& ext)
{
    if (kind == Kind::Video || kind == Kind::Loop)
        return ext == QLatin1String("mp4") || ext == QLatin1String("mov") || ext == QLatin1String("webm");
    return ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("gif") || ext == QLatin1String("webp");
}

QString metaPath(const QString& cacheDir, const QString& key)
{
    return cacheDir + QLatin1Char('/') + key + latin(".json");
}

Kind kindOfName(const QString& name)
{
    for (const Kind k : {Kind::Image, Kind::Gif, Kind::Video, Kind::Loop, Kind::Card}) {
        if (name == QLatin1String(kindName(k)))
            return k;
    }
    return Kind::None;
}

// The size of a picture file as stored (before any EXIF rotation), read with the format forced (safeimage);
// invalid if it can't be read.
QSize pictureSize(const QString& path)
{
    const std::unique_ptr<QImageReader> reader = safeimage::reader(path, safeimage::Source::Website);
    return reader->format().isEmpty() ? QSize() : reader->size();
}

// A card's picture is drawn at most 400 px wide (800 px on a 2x screen): one larger is made smaller here, on the
// worker, once (first frame; JPEG, or PNG with transparency), so the chat decodes a small file on the GUI thread
// (review fix). The picture's original format is kept when it is small enough. Empty: it couldn't be made.
constexpr int kCardPictureSide = 800;

QString shrinkPicture(const QString& path, const QString& base, QSize* size, qint64* bytes)
{
    QImage image;
    {
        // The reader holds the file open until it goes (live test fix): Windows can't remove or replace an open file,
        // so the copy of a JPEG (or of a PNG with alpha), which takes the original's own name, was lost with it.
        const std::unique_ptr<QImageReader> reader = safeimage::reader(path, safeimage::Source::Website);
        if (reader->format().isEmpty())
            return {};
        const QSize full = reader->size();
        if (!full.isValid() || full.isEmpty())
            return {};
        reader->setAutoTransform(true); // upright (the copy has no EXIF), as the chat drew the original
        reader->setScaledSize(full.scaled(QSize(kCardPictureSide, kCardPictureSide), Qt::KeepAspectRatio));
        image = reader->read();
    }
    if (image.isNull())
        return {};
    if (image.width() > kCardPictureSide || image.height() > kCardPictureSide)
        image = image.scaled(QSize(kCardPictureSide, kCardPictureSide), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const bool    alpha = image.hasAlphaChannel();
    const QString out   = base + (alpha ? QStringLiteral("-thumb.png") : QStringLiteral("-thumb.jpg"));
    const QString part  = out + QStringLiteral(".part");
    {
        QImageWriter writer(part, alpha ? "png" : "jpg");
        if (!alpha)
            writer.setQuality(85);
        if (!writer.write(alpha ? image.convertToFormat(QImage::Format_ARGB32) : image.convertToFormat(QImage::Format_RGB32)))
            return {};
    }
    if (out != path)
        QFile::remove(path);
    if (!replaceFile(part, out)) {
        QFile::remove(part);
        return {};
    }
    *size  = image.size();
    *bytes = QFileInfo(out).size();
    return out;
}

bool isWebpFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return content::sniff(file.read(16)) == content::Format::WebP;
}

// A WebP picture that may be decoded (lossy, well-formed); true for every other format.
bool webpAllowed(const QString& path)
{
    if (!isWebpFile(path))
        return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return content::webpVerdict(file.readAll()) == content::WebpVerdict::Ok;
}

// ---- the cache's records --------------------------------------------------------------------------------------

// 2.4.0's record of a legacy link; false unless it and its files are complete.
bool readMetaV1(const Target& target, const QString& cacheDir, Result* out)
{
    if (!target.isValid() || !isKey(target.key) || cacheDir.isEmpty())
        return false;
    QFile file(metaPath(cacheDir, target.key));
    if (file.size() > kMaxMetaBytes || !file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxMetaBytes));
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    if (o.value(QLatin1String("v")).toInt() != kMetaVersion || o.value(QLatin1String("kind")).toString() != latin(kindName(target.kind)))
        return false;

    Result        result;
    const QString prefix    = cacheDir + QLatin1Char('/');
    const QString fileName  = o.value(QLatin1String("file")).toString();
    const QString thumbName = o.value(QLatin1String("thumb")).toString();
    if (target.kind == Kind::YouTube) {
        if (!fileName.isEmpty())
            return false;
    } else {
        // Only names this Fetcher writes: "<key>.<ext>".
        if (!fileName.startsWith(target.key + QLatin1Char('.')) || !isMediaExtension(target.kind, fileName.mid(target.key.size() + 1)))
            return false;
        const QFileInfo media(prefix + fileName);
        if (!media.isFile() || media.size() != static_cast<qint64>(o.value(QLatin1String("size")).toDouble(-1)))
            return false;
        result.localFile = media.filePath();
    }
    if (!thumbName.isEmpty()) {
        const QString ext = thumbName.mid(target.key.size() + 7);
        if (!thumbName.startsWith(target.key + latin("-thumb.")) || !isMediaExtension(Kind::Image, ext) || !QFileInfo(prefix + thumbName).isFile())
            return false;
        result.thumbnailFile = prefix + thumbName;
    } else if (target.kind == Kind::YouTube) {
        return false;
    }
    result.ok         = true;
    result.mime       = o.value(QLatin1String("mime")).toString().left(40);
    result.size       = static_cast<qint64>(o.value(QLatin1String("size")).toDouble(-1));
    result.title      = content::cleanText(o.value(QLatin1String("title")).toString(), content::kMaxTitleChars);
    result.author     = content::cleanText(o.value(QLatin1String("author")).toString(), content::kMaxAuthorChars);
    result.width      = qBound(0, o.value(QLatin1String("w")).toInt(), 1 << 20);
    result.height     = qBound(0, o.value(QLatin1String("h")).toInt(), 1 << 20);
    result.durationMs = qMax<qint64>(0, static_cast<qint64>(o.value(QLatin1String("d")).toDouble()));
    result.kind       = target.kind;
    *out              = result;
    return true;
}

bool writeMetaV1(const Target& target, const QString& cacheDir, const Result& result, quint64 jobId)
{
    QJsonObject o;
    o.insert(QLatin1String("v"), kMetaVersion);
    o.insert(QLatin1String("kind"), latin(kindName(target.kind)));
    o.insert(QLatin1String("mime"), result.mime);
    o.insert(QLatin1String("size"), static_cast<double>(result.size));
    o.insert(QLatin1String("file"), result.localFile.isEmpty() ? QString() : QFileInfo(result.localFile).fileName());
    o.insert(QLatin1String("thumb"), result.thumbnailFile.isEmpty() ? QString() : QFileInfo(result.thumbnailFile).fileName());
    o.insert(QLatin1String("w"), result.width);
    o.insert(QLatin1String("h"), result.height);
    o.insert(QLatin1String("d"), static_cast<double>(result.durationMs));
    o.insert(QLatin1String("title"), result.title);
    o.insert(QLatin1String("author"), result.author);
    return writeFileAtomic(metaPath(cacheDir, target.key), QJsonDocument(o).toJson(QJsonDocument::Compact), jobId);
}

// The errors a new-class link remembers as "nothing to show" for a while (deterministic answers of the site).
bool isCachedNothing(const QString& error)
{
    return error == QLatin1String(errors::kNothing) || error == QLatin1String(errors::kWalled) || error == QLatin1String(errors::kNotFound)
           || error == QLatin1String(errors::kUnavailable) || error == QLatin1String(errors::kBadType) || error == QLatin1String(errors::kUnsupported)
           || error == QLatin1String(errors::kTooManyPixels);
}

// 2.5 shares (design2 2.3): the Web plan's answers after a share link's redirect to another host that a Web link shows as
// nothing by itself, and that are the site's own (not busy, not the network): a page that is gone, refused or no media.
bool quietAfterHandover(const QString& error)
{
    return error == QLatin1String(errors::kNotFound) || error == QLatin1String(errors::kUnavailable) || error == QLatin1String(errors::kBadType)
           || error == QLatin1String(errors::kUnsupported) || error == QLatin1String(errors::kTooManyPixels) || error == QLatin1String(errors::kRedirect);
}

QString colourName(QRgb colour)
{
    return QString::asprintf("#%02x%02x%02x", qRed(colour), qGreen(colour), qBlue(colour));
}

// 2.5: the record of a new-class link (v2). *out filled for a hit; a "none" hit: out->ok false with its error.
bool readMetaV2(const Target& target, const QString& cacheDir, qint64 nowSec, bool ignoreNothing, Result* out)
{
    if (!target.isNewClass() || !isKey(target.key) || cacheDir.isEmpty())
        return false;
    QFile file(metaPath(cacheDir, target.key));
    if (file.size() > kMaxMetaBytes || !file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxMetaBytes));
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    if (o.value(QLatin1String("v")).toInt() != kMetaVersion2 || o.value(QLatin1String("cls")).toString() != latin(className(target.cls)))
        return false;
    const qint64  at     = static_cast<qint64>(o.value(QLatin1String("at")).toDouble(0));
    const qint64  age    = nowSec - at;
    const QString kind   = o.value(QLatin1String("kind")).toString();
    const QString prefix = cacheDir + QLatin1Char('/');
    if (at <= 0 || age < -300)
        return false; // the clock went back a lot: fetch again
    if (kind == QLatin1String("none")) {
        const QString error = o.value(QLatin1String("error")).toString().left(40);
        if (ignoreNothing || !isCachedNothing(error))
            return false;
        const qint64 fresh = error == QLatin1String(errors::kWalled) ? kWalledFreshSec : kNothingFreshSec;
        if (age > fresh)
            return false;
        Result none = failure(error);
        *out        = none;
        return true;
    }
    const Kind k = kindOfName(kind);
    if (k == Kind::None)
        return false;
    Result        result;
    const QString thumbName = o.value(QLatin1String("thumb")).toString();
    if (!thumbName.isEmpty()) {
        const QString ext = thumbName.mid(target.key.size() + 7);
        if (!thumbName.startsWith(target.key + latin("-thumb.")) || !isMediaExtension(Kind::Image, ext) || !QFileInfo(prefix + thumbName).isFile())
            return false;
        result.thumbnailFile = prefix + thumbName;
    }
    if (k == Kind::Card) {
        if (age > kCardFreshSec)
            return false;
        const QJsonObject c = o.value(QLatin1String("card")).toObject();
        CardData          card;
        card.site        = textclean::clean(c.value(QLatin1String("site")).toString(), kCardSiteChars);
        card.author      = textclean::clean(c.value(QLatin1String("author")).toString(), kCardAuthorChars);
        card.title       = textclean::clean(c.value(QLatin1String("title")).toString(), kCardTitleChars);
        card.description = textclean::clean(c.value(QLatin1String("desc")).toString(), kCardDescriptionChars);
        if (card.title.isEmpty())
            return false;
        const QString pic = c.value(QLatin1String("pic")).toString();
        if (!pic.isEmpty()) {
            if (pic != thumbName || result.thumbnailFile.isEmpty())
                return false;
            card.pictureFile   = result.thumbnailFile;
            card.pictureWidth  = qBound(0, c.value(QLatin1String("pw")).toInt(), 1 << 20);
            card.pictureHeight = qBound(0, c.value(QLatin1String("ph")).toInt(), 1 << 20);
        }
        const QString layout = c.value(QLatin1String("layout")).toString();
        card.layout          = card.pictureFile.isEmpty() ? CardLayout::None
                               : layout == QLatin1String("large") ? CardLayout::Large
                                                                  : CardLayout::Thumb;
        QRgb colour = 0;
        if (pagemeta::parseHexColour(c.value(QLatin1String("color")).toString(), &colour) && pagemeta::isAcceptableThemeColour(colour)) {
            card.color    = colour;
            card.hasColor = true;
        }
        card.sensitive  = c.value(QLatin1String("sens")).toBool();
        card.play       = c.value(QLatin1String("play")).toBool();
        card.durationMs = card.play ? qBound<qint64>(0, static_cast<qint64>(c.value(QLatin1String("d")).toDouble()), 86400000) : 0;
        result.card     = card;
        result.mime     = o.value(QLatin1String("mime")).toString().left(40);
        result.finalHost = o.value(QLatin1String("fhost")).toString().left(253);
        if (!result.finalHost.isEmpty() && publicHostName(QUrl(latin("https://") + result.finalHost + QLatin1Char('/'))) != result.finalHost)
            return false;
    } else {
        const QString fileName = o.value(QLatin1String("file")).toString();
        if (!fileName.startsWith(target.key + QLatin1Char('.')) || !isMediaExtension(k, fileName.mid(target.key.size() + 1)))
            return false;
        const QFileInfo media(prefix + fileName);
        if (!media.isFile() || media.size() != static_cast<qint64>(o.value(QLatin1String("size")).toDouble(-1)))
            return false;
        result.localFile  = media.filePath();
        result.mime       = o.value(QLatin1String("mime")).toString().left(40);
        result.size       = media.size();
        result.width      = qBound(0, o.value(QLatin1String("w")).toInt(), 1 << 20);
        result.height     = qBound(0, o.value(QLatin1String("h")).toInt(), 1 << 20);
        result.durationMs = qMax<qint64>(0, static_cast<qint64>(o.value(QLatin1String("d")).toDouble()));
        result.hasAudio   = o.value(QLatin1String("audio")).toBool();
        result.card.sensitive = o.value(QLatin1String("sens")).toBool();
    }
    result.ok   = true;
    result.kind = k;
    *out        = result;
    return true;
}

bool writeMetaV2(const Target& target, const QString& cacheDir, const Result& result, qint64 nowSec, quint64 jobId)
{
    QJsonObject o;
    o.insert(QLatin1String("v"), kMetaVersion2);
    o.insert(QLatin1String("cls"), latin(className(target.cls)));
    o.insert(QLatin1String("at"), static_cast<double>(nowSec));
    if (!result.ok) {
        o.insert(QLatin1String("kind"), latin("none"));
        o.insert(QLatin1String("error"), result.error);
    } else {
        o.insert(QLatin1String("kind"), latin(kindName(result.kind)));
        o.insert(QLatin1String("mime"), result.mime);
        o.insert(QLatin1String("thumb"), result.thumbnailFile.isEmpty() ? QString() : QFileInfo(result.thumbnailFile).fileName());
        if (result.kind == Kind::Card) {
            const CardData& card = result.card;
            QJsonObject     c;
            c.insert(QLatin1String("site"), card.site);
            c.insert(QLatin1String("author"), card.author);
            c.insert(QLatin1String("title"), card.title);
            c.insert(QLatin1String("desc"), card.description);
            c.insert(QLatin1String("pic"), card.pictureFile.isEmpty() ? QString() : QFileInfo(card.pictureFile).fileName());
            c.insert(QLatin1String("pw"), card.pictureWidth);
            c.insert(QLatin1String("ph"), card.pictureHeight);
            c.insert(QLatin1String("layout"), latin(layoutName(card.layout)));
            if (card.hasColor)
                c.insert(QLatin1String("color"), colourName(card.color));
            c.insert(QLatin1String("sens"), card.sensitive);
            c.insert(QLatin1String("play"), card.play);
            c.insert(QLatin1String("d"), static_cast<double>(card.durationMs));
            o.insert(QLatin1String("card"), c);
            if (!result.finalHost.isEmpty())
                o.insert(QLatin1String("fhost"), result.finalHost);
        } else {
            o.insert(QLatin1String("file"), QFileInfo(result.localFile).fileName());
            o.insert(QLatin1String("size"), static_cast<double>(result.size));
            o.insert(QLatin1String("w"), result.width);
            o.insert(QLatin1String("h"), result.height);
            o.insert(QLatin1String("d"), static_cast<double>(result.durationMs));
            o.insert(QLatin1String("audio"), result.hasAudio);
            o.insert(QLatin1String("sens"), result.card.sensitive);
        }
    }
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (json.size() > kMaxMetaBytes)
        return false;
    return writeFileAtomic(metaPath(cacheDir, target.key), json, jobId);
}

// ---- requests -----------------------------------------------------------------------------------------------

struct Got {
    QString    error; // errors::k..., empty when fine
    int        status   = 0;
    qint64     total    = -1; // the announced size
    qint64     received = 0;
    QByteArray head;          // the first content::kSniffBytes
    QByteArray body;
    // 2.5
    QUrl       finalUrl;      // the address that answered
    QByteArray contentType;
    bool       stoppedEarly = false; // a page: its head was complete
    bool       truncated    = false; // a page: cut at its cap
    bool       challenge    = false; // "cf-mitigated: challenge"
    bool       redirectStop = false; // a redirect step: finalUrl is the Location that matched
    bool       switched     = false; // a Web page that answered with media (written to the media file)
    Kind       switchedKind = Kind::None;
    qint64     retryAfterMs = 0;
    bool       softCooling  = false;
};

// What a Web page's answer turned out to be, decided from its head (and for octet-stream from its first bytes).
enum class HeadAction { Text, Media, Fail };

struct GetSpec {
    QUrl                                   url;
    QByteArray                             accept;
    qint64                                 maxBytes = 0;
    HostGate::Lane                         lane     = HostGate::Lane::Media;
    std::function<bool(const QUrl&)>       hopAllowed;  // every hop, on top of net::isAllowedUrl
    std::function<bool(const QByteArray&)> typeAllowed; // the Content-Type header
    std::function<bool(const QByteArray&)> headAllowed; // the first content::kSniffBytes (or the whole shorter body)
    QFile*                                 file     = nullptr; // else the body is collected in Got::body
    bool                                   progress = false;
    // 2.5
    bool                                   text = false;        // kept in memory; a page read is cut at maxBytes and succeeds
    bool                                   truncateOk = false;
    std::function<bool(const QByteArray&)> enough;              // a page read: stop early (16, 32, ..., 512 KiB and the end)
    std::function<bool(const QUrl&)>       stopAt;              // a redirect step: stop at a Location that matches
    bool                                   headOnly = false;    // a redirect step: never read a body
    bool                                   pageClock = false;   // a page request: 15 s in all, and the slow-drip check
    // A Web page: Content-Type decides between a page and media (switches to file / mediaMax).
    std::function<HeadAction(const QByteArray& type, Kind* kind)> decide;
    QFile*                                 mediaFile = nullptr;
    qint64                                 mediaMax  = 0;
    // A media request of either family (a share's DAV link): the cap by the answer's media type.
    std::function<qint64(const QByteArray& type)> maxFor;
};

QString firstSegmentOf(const QUrl& url)
{
    const QStringList segments = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return segments.isEmpty() ? QString() : segments.first().toLower();
}

// Every hop of a Web page's request and of its picture's (the redirects too, not only the address the message or
// the page wrote): never an address that looks like an action or a token (sign-in, unsubscribe, token=, ...) and never
// a non-HTML file (2.5 rules). A refused redirect ends that request (no card, or the next picture candidate).
bool webHopAllowed(const QUrl& url)
{
    return !isActionLike(url) && !hasNonHtmlExtension(url);
}

// A Web page's picture (and every hop of its request): webHopAllowed, and never a share's file or preview address on
// any host (shares::isShareFileAddress): a page (a share page that isn't proven one, a share on another host, a page
// that shows a share's preview) can't make every viewer use up a download of a limited share.
bool webPictureAllowed(const QUrl& url)
{
    return webHopAllowed(url) && !shares::isShareFileAddress(url);
}

bool isRemovedImgur(const QUrl& url)
{
    return publicHostName(url) == QLatin1String("i.imgur.com") && url.path(QUrl::FullyDecoded) == QLatin1String("/removed.png");
}

// A page's first bytes: after an optional BOM, white space and comments, an HTML start.
bool looksLikeHtml(const QByteArray& first)
{
    QByteArray s = first;
    if (s.startsWith("\xEF\xBB\xBF"))
        s = s.mid(3);
    for (;;) {
        s = s.trimmed();
        if (s.startsWith("<!--")) {
            const int end = s.indexOf("-->");
            if (end < 0)
                return false;
            s = s.mid(end + 3);
            continue;
        }
        break;
    }
    const QByteArray lower = s.left(16).toLower();
    return lower.startsWith("<!doctype html") || lower.startsWith("<html") || lower.startsWith("<head") || lower.startsWith("<meta")
           || lower.startsWith("<title");
}

bool isHtmlType(const QByteArray& type)
{
    return type == "text/html" || type == "application/xhtml+xml";
}

bool isGenericType(const QByteArray& type)
{
    return type.isEmpty() || type == "application/octet-stream" || type == "binary/octet-stream" || type == "application/binary";
}

QString percentEncoded(const QString& text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

} // namespace

// ---- the worker -----------------------------------------------------------------------------------------------

namespace detail {

class FetchJob
{
  public:
    std::shared_ptr<Fetcher::Link>   link;
    std::shared_ptr<http::Cancel>    cancel;
    std::shared_ptr<http::Transport> transport;
    std::shared_ptr<HostGate>        gate;
    Fetcher::Options                 options;
    quint64                          id = 0;
    Target                           target;
    Caps                             caps;
    QString                          cacheDir;
    HostGate::Ticket                 firstTicket;
    HMODULE                          module = nullptr; // the pin on our DLL

    void run()
    {
        Result result;
        try {
            result = execute();
        } catch (...) {
            result = failure(errors::kInternal);
        }
        if (firstTicket.id != 0 && !m_firstUsed)
            gate->unused(firstTicket); // a cache hit, or canceled before the first request
        if (cancel->isSet() && result.ok)
            result = failure(errors::kCanceled);
        result.viaProxy   = m_viaProxy;
        result.requests   = m_requests;
        result.hopRefused = m_hopRefused;
        const quint64 jobId = id;
        const QString key   = target.key;
        post([jobId, key, result](Fetcher* fetcher) { fetcher->onJobDone(jobId, key, result); });
    }

  private:
    template <typename Fn>
    void post(Fn fn)
    {
        QMutexLocker lock(&link->mutex);
        if (Fetcher* receiver = link->receiver.data())
            QMetaObject::invokeMethod(receiver, [receiver, fn]() { fn(receiver); }, Qt::QueuedConnection);
    }

    void reportProgress(qint64 received, qint64 total, bool force)
    {
        const quint64 now = http::nowMs();
        if (!force && now - m_lastProgress < static_cast<quint64>(kProgressEveryMs))
            return;
        m_lastProgress      = now;
        const quint64 jobId = id;
        const QString key   = target.key;
        post([jobId, key, received, total](Fetcher* fetcher) { fetcher->onJobProgress(jobId, key, received, total); });
    }

    qint64 nowSec() const { return options.nowSec ? options.nowSec() : QDateTime::currentSecsSinceEpoch(); }

    QString base() const { return cacheDir + QLatin1Char('/') + target.key; }

    Result execute()
    {
        if (cancel->isSet())
            return failure(errors::kCanceled);
        if (!QDir().mkpath(cacheDir))
            return failure(errors::kDisk);
        Result cached;
        if (target.isNewClass()) {
            if (readMetaV2(target, cacheDir, nowSec(), caps.userAsked, &cached)) {
                const bool   video = cached.kind == Kind::Video || cached.kind == Kind::Loop;
                const qint64 cap   = video && caps.video >= 0 ? caps.video : caps.media;
                if (cached.ok && cached.kind != Kind::Card && cached.size > cap) {
                    Result tooLarge = failure(errors::kTooLarge);
                    tooLarge.size   = cached.size;
                    tooLarge.kind   = cached.kind;
                    return tooLarge;
                }
                return cached;
            }
        } else if (readMetaV1(target, cacheDir, &cached)) {
            if (target.kind != Kind::YouTube && cached.size > caps.media) {
                Result tooLarge = failure(errors::kTooLarge);
                tooLarge.size   = cached.size;
                return tooLarge;
            }
            return cached;
        }
        m_deadline = http::nowMs() + static_cast<quint64>(qMax(1, options.totalTimeoutMs));
        Result result;
        switch (target.cls) {
        case LinkClass::YouTube:
            return loadYouTube();
        case LinkClass::Direct:
            if (net::expiredSignedLink(target.url, nowSec()))
                return failure(errors::kExpired);
            if (target.provider == QLatin1String("tenor-short"))
                result = loadTenorShort();
            else if (target.provider == QLatin1String("dropbox"))
                result = loadDropbox();
            else
                result = loadMedia(target.url, target.kind, QString(), false, true);
            if (result.ok && result.kind == Kind::None)
                result.kind = target.kind;
            if (result.ok && !cancel->isSet())
                writeMetaV1(target, cacheDir, result, id); // 2.4.0's record (a downgrade keeps working)
            return result;
        case LinkClass::MediaPage:
        case LinkClass::Site:
            result = runProvider(target);
            break;
        case LinkClass::Web:
            result = runWeb();
            break;
        case LinkClass::None:
            return failure(errors::kNotPreviewable);
        }
        // The new classes remember cards and media, and "nothing to show" for a while (never time-outs, busy sites or
        // anything that depends on the caps).
        if (!cancel->isSet() && (result.ok || isCachedNothing(result.error)))
            writeMetaV2(target, cacheDir, result, nowSec(), id);
        return result;
    }

    // ---- the gate -------------------------------------------------------------------------------------------------

    // A ticket for this hop: the one the GUI thread took for the load's first request when it fits, else a new one.
    // Waits (on the cancel event, outside the load's deadline) at most options.maxGateWaitMs in all.
    bool acquireHop(const QUrl& url, HostGate::Lane lane, HostGate::Ticket* ticket, Got* got)
    {
        const QString host    = publicHostName(url);
        const QString segment = firstSegmentOf(url);
        if (firstTicket.id != 0 && !m_firstUsed && firstTicket.lane == lane && firstTicket.domain == healthKey(host)
            && firstTicket.group == spacingGroup(host)) {
            m_firstUsed = true;
            *ticket     = firstTicket;
            return true;
        }
        for (;;) {
            qint64                  ms   = 0;
            bool                    soft = false;
            const HostGate::Verdict v    = gate->acquire(host, lane, segment, caps.userAsked, &ms, ticket, &soft);
            switch (v) {
            case HostGate::Verdict::Go:
                return true;
            case HostGate::Verdict::Wait: {
                if (m_gateWaited >= options.maxGateWaitMs) {
                    got->error        = latin(errors::kGateBusy);
                    got->retryAfterMs = qMax<qint64>(ms, 1000);
                    return false;
                }
                const int wait = static_cast<int>(qBound<qint64>(1, ms, options.maxGateWaitMs - m_gateWaited));
                const quint64 before = http::nowMs();
                if (cancel->wait(wait)) {
                    got->error = latin(errors::kCanceled);
                    return false;
                }
                const quint64 waited = http::nowMs() - before;
                m_gateWaited += static_cast<int>(qMax<quint64>(waited, static_cast<quint64>(wait)));
                m_deadline += waited; // a wait for the site is not charged to the load's own time
                continue;
            }
            case HostGate::Verdict::Cooling:
            case HostGate::Verdict::Broken:
                got->error        = latin(errors::kRateLimited);
                got->retryAfterMs = ms;
                got->softCooling  = soft && v == HostGate::Verdict::Cooling;
                return false;
            case HostGate::Verdict::Walled:
                got->error        = latin(errors::kWalled);
                got->retryAfterMs = ms;
                return false;
            }
        }
    }

    void releaseHop(const HostGate::Ticket& ticket, HostGate::Outcome outcome, const http::Head* head = nullptr)
    {
        if (ticket.id == 0)
            return;
        if (head)
            gate->release(ticket, outcome, head->retryAfterMs, head->rateRemaining, head->rateResetMs);
        else
            gate->release(ticket, outcome);
    }

    // After a 429: how long the gate now keeps the site cooling.
    void coolingOf(const QUrl& url, HostGate::Lane lane, Got* got) const
    {
        qint64 ms   = 0;
        bool   soft = false;
        if (gate->state(publicHostName(url), lane, firstSegmentOf(url), &ms, &soft) == HostGate::Verdict::Cooling) {
            got->retryAfterMs = ms;
            got->softCooling  = soft;
        }
    }

    // ---- one GET -----------------------------------------------------------------------------------------------

    // One GET with redirects, every hop checked (netpolicy.h, the hop list) and gated (HostGate), the body checked
    // while it streams.
    Got get(const GetSpec& spec)
    {
        Got        got;
        const auto fail = [&got](const char* code) {
            got.error = latin(code);
            return got;
        };
        QUrl           url          = spec.url;
        quint64        started      = 0; // the request's own clock starts after its first gate wait
        quint64        hopStarted   = 0; // this hop's own start (after its gate wait): the slow-drip check
        bool           counted      = false;
        for (int hop = 0;; ++hop) {
            if (cancel->isSet())
                return fail(errors::kCanceled);
            if (http::msUntil(m_deadline) <= 0)
                return fail(errors::kTimeout);
            if (spec.pageClock && started != 0 && http::nowMs() - started > static_cast<quint64>(qMax(1, options.pageTimeoutMs)))
                return fail(errors::kTimeout);
            if (hop > net::kMaxRedirects)
                return fail(errors::kRedirect);
            if (!net::isAllowedUrl(url) || (spec.hopAllowed && !spec.hopAllowed(url))) {
                m_hopRefused = publicHostName(url);
                return fail(hop == 0 ? errors::kNotPreviewable : errors::kRedirect);
            }
            if (isRemovedImgur(url))
                return fail(errors::kNotFound); // Imgur's "this was removed" picture

            // Direct, or through the user's Windows proxy (netpolicy.h).
            const http::Route route = transport->route(url, *cancel, m_deadline);
            if (cancel->isSet())
                return fail(errors::kCanceled);
            if (route.viaProxy) {
                // The proxy may reach the intranet: never a host under the PC's own DNS suffixes.
                const QStringList suffixes = options.localSuffixes ? options.localSuffixes() : net::localDnsSuffixes();
                if (net::underLocalSuffix(publicHostName(url), suffixes))
                    return fail(errors::kBlocked);
            }

            HostGate::Ticket ticket;
            const quint64    beforeGate = http::nowMs();
            if (!acquireHop(url, spec.lane, &ticket, &got))
                return got;
            hopStarted = http::nowMs();
            if (started == 0)
                started = hopStarted;
            else
                started += hopStarted - beforeGate; // a later hop's wait for its site is not charged to the page clock
            if (route.viaProxy)
                m_viaProxy = true;

            // Direct: every address of the host must be public, before anything connects. Through a proxy
            // the proxy resolves the name (local DNS may be filtered) and connects.
            http::Error error = http::Error::None;
            if (!route.viaProxy) {
                QVector<net::Address> addresses;
                error = transport->resolve(publicHostName(url), &addresses, *cancel, m_deadline);
                if (error != http::Error::None) {
                    releaseHop(ticket, outcomeFor(error));
                    return fail(errorFor(error));
                }
                if (addresses.isEmpty()) {
                    releaseHop(ticket, HostGate::Outcome::Offline);
                    return fail(errors::kOffline);
                }
                for (const net::Address& address : qAsConst(addresses)) {
                    if (!net::isPublicAddress(address)) {
                        releaseHop(ticket, HostGate::Outcome::Other);
                        return fail(errors::kBlocked);
                    }
                }
            }

            http::HopOptions hopOptions;
            hopOptions.url              = url;
            hopOptions.route            = route;
            hopOptions.accept           = spec.accept;
            hopOptions.deadline         = m_deadline;
            hopOptions.connectTimeoutMs = options.connectTimeoutMs;
            hopOptions.receiveTimeoutMs = options.receiveTimeoutMs;
            hopOptions.userAgent        = net::userAgent();
            hopOptions.acceptEncoding   = "identity";
            if (route.viaProxy)
                hopOptions.allowPeer = [](const net::Address&) { return true; }; // the user's proxy (often on this PC)
            else
                hopOptions.allowPeer = [](const net::Address& address) { return net::isPublicAddress(address); };
            if (!counted) {
                counted = true;
                ++m_requests;
            }
            std::unique_ptr<http::Exchange> exchange = transport->open(hopOptions, *cancel, &error);
            if (!exchange || error != http::Error::None) {
                const http::Error e = error == http::Error::None ? http::Error::BadResponse : error;
                releaseHop(ticket, outcomeFor(e));
                return fail(errorFor(e));
            }
            http::Head head;
            error = exchange->head(&head);
            if (error != http::Error::None) {
                releaseHop(ticket, outcomeFor(error));
                return fail(errorFor(error));
            }
            // Direct: where the response really came from (DNS may have changed since the check above).
            if (!route.viaProxy && (head.peer.isNull() || !net::isPublicAddress(head.peer))) {
                releaseHop(ticket, HostGate::Outcome::Other);
                return fail(errors::kBlocked);
            }
            got.status    = head.status;
            got.challenge = got.challenge || head.challenge;
            if (isRedirect(head.status) && !head.challenge) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                const QUrl next = net::redirectTarget(url, head.location);
                if (!next.isValid())
                    return fail(errors::kRedirect);
                if (spec.stopAt && spec.stopAt(next)) {
                    got.finalUrl     = next;
                    got.redirectStop = true;
                    return got;
                }
                url = next;
                continue;
            }
            got.finalUrl    = url;
            got.contentType = head.contentType;
            if (head.status != 200) {
                const char* code = errorForStatus(head.status, head);
                HostGate::Outcome outcome = HostGate::Outcome::Ok;
                if (head.challenge)
                    outcome = HostGate::Outcome::Walled;
                else if (code == errors::kRateLimited)
                    outcome = HostGate::Outcome::RateLimited;
                else if (head.status >= 500 && !(target.provider == QLatin1String("gyazo") && head.status == 503))
                    outcome = HostGate::Outcome::ServerError;
                else if (head.status >= 500)
                    outcome = HostGate::Outcome::Other;
                releaseHop(ticket, outcome, &head);
                if (outcome == HostGate::Outcome::RateLimited)
                    coolingOf(url, spec.lane, &got);
                return fail(code);
            }
            if (spec.headOnly) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return got;
            }
            if (!head.contentEncoding.isEmpty() && head.contentEncoding != "identity") {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kBadType); // never decompressed: no card, no media
            }

            const QByteArray type    = content::mediaType(head.contentType);
            bool             text    = spec.text;
            QFile*           file    = spec.file;
            qint64           max     = spec.maxBytes;
            bool             sniffing = false; // a Web page of a generic type: its first bytes decide
            // The page clock and the slow-drip check are for pages: a Web link that answers with media is read under
            // the media rules (the per-read time-out and the load's deadline), like any other media file.
            bool             pageClock = spec.pageClock;
            std::function<bool(const QByteArray&)> headAllowed = spec.headAllowed;
            if (spec.decide) {
                Kind             kind   = Kind::None;
                const HeadAction action = spec.decide(type, &kind);
                if (action == HeadAction::Fail) {
                    releaseHop(ticket, HostGate::Outcome::Ok, &head);
                    return fail(errors::kBadType);
                }
                if (action == HeadAction::Media) {
                    got.switched     = true;
                    got.switchedKind = kind;
                    if (spec.mediaMax <= 0 || head.contentLength > spec.mediaMax) {
                        releaseHop(ticket, HostGate::Outcome::Ok, &head);
                        got.total = head.contentLength;
                        return fail(errors::kTooLarge); // not one byte of the body is read
                    }
                    text        = false;
                    pageClock   = false;
                    file        = spec.mediaFile;
                    max         = spec.mediaMax;
                    headAllowed = [kind](const QByteArray& first) { return content::fits(familyOf(kind), content::sniff(first)); };
                }
                sniffing = action == HeadAction::Text && isGenericType(type);
            } else if (spec.typeAllowed && !spec.typeAllowed(head.contentType)) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kBadType);
            }
            if (spec.maxFor)
                max = spec.maxFor(type);
            got.total = head.contentLength;
            if (head.contentLength > max && !(text && spec.truncateOk)) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kTooLarge); // announced: not one byte of the body is read
            }

            std::vector<char> buffer(kReadChunk);
            bool              sniffed   = false;
            qint64            nextCheck = 16 * 1024;
            // A Web page sent as octet-stream (or with no type): HTML goes on as a page, media magic becomes media (the
            // bytes so far go to the media file), anything else is nothing. "" when it goes on.
            const auto resolveSniff = [&]() -> const char* {
                sniffing = false;
                if (looksLikeHtml(got.body.left(kSniffPageBytes)))
                    return "";
                const content::Format format = content::sniff(got.body);
                if (format == content::Format::Unknown || !spec.mediaFile)
                    return errors::kBadType;
                const Kind kind  = content::fits(Kind::Video, format) ? Kind::Video : format == content::Format::Gif ? Kind::Gif : Kind::Image;
                got.switched     = true;
                got.switchedKind = kind;
                if (spec.mediaMax <= 0 || head.contentLength > spec.mediaMax || got.received > spec.mediaMax) {
                    got.total = head.contentLength;
                    return errors::kTooLarge;
                }
                text      = false;
                pageClock = false;
                file      = spec.mediaFile;
                max       = spec.mediaMax;
                if (file->write(got.body) != got.body.size())
                    return errors::kDisk;
                got.body.clear();
                sniffed = true;
                return "";
            };
            for (;;) {
                if (cancel->isSet()) {
                    releaseHop(ticket, HostGate::Outcome::Other);
                    return fail(errors::kCanceled);
                }
                // A page whose announced bytes are all here is complete: the next read only sees its end.
                if (pageClock && !(head.contentLength >= 0 && got.received >= head.contentLength)) {
                    const quint64 now     = http::nowMs();
                    const quint64 elapsed = now - started;
                    // A page (not a small JSON answer) that drips its bytes: less than 16 KiB 5 s after this hop
                    // started (its own wait for its site not counted).
                    const bool drip = spec.truncateOk && now - hopStarted > static_cast<quint64>(options.pageMinBytesAfterMs)
                                      && got.received < options.pageMinBytes;
                    if (elapsed > static_cast<quint64>(qMax(1, options.pageTimeoutMs)) || drip) {
                        releaseHop(ticket, HostGate::Outcome::Timeout);
                        return fail(errors::kTimeout); // a page that drips its bytes
                    }
                }
                // A page is read to its cap and not one byte further.
                const qint64 room = max - got.received + (spec.truncateOk ? 0 : 1); // one byte more tells "over the cap"
                const int    want = text ? static_cast<int>(qBound<qint64>(0, room, kReadChunk)) : kReadChunk;
                if (want <= 0) {
                    got.truncated = spec.truncateOk;
                    if (!spec.truncateOk) {
                        releaseHop(ticket, HostGate::Outcome::Ok, &head);
                        return fail(errors::kTooLarge);
                    }
                    break;
                }
                int count = 0;
                error     = exchange->read(buffer.data(), want, &count);
                if (error != http::Error::None) {
                    releaseHop(ticket, outcomeFor(error), &head);
                    return fail(errorFor(error));
                }
                if (count <= 0)
                    break;
                if (text && got.received + count > max) {
                    if (!spec.truncateOk) {
                        releaseHop(ticket, HostGate::Outcome::Ok, &head);
                        return fail(errors::kTooLarge);
                    }
                    count         = static_cast<int>(max - got.received);
                    got.truncated = true;
                }
                got.received += count;
                if (!text && got.received > max) {
                    if (head.contentLength >= 0 && got.received > head.contentLength)
                        got.total = -1; // it said less than it sent
                    releaseHop(ticket, HostGate::Outcome::Ok, &head);
                    return fail(errors::kTooLarge);
                }
                if (!sniffed) {
                    const int take = qMin(count, content::kSniffBytes - got.head.size());
                    got.head.append(buffer.data(), take);
                    if (got.head.size() >= content::kSniffBytes && !sniffing) {
                        sniffed = true;
                        if (headAllowed && !headAllowed(got.head)) {
                            releaseHop(ticket, HostGate::Outcome::Ok, &head);
                            return fail(errors::kBadType); // HTML or anything else: no further byte is read
                        }
                    }
                }
                if (file) {
                    if (file->write(buffer.data(), count) != count) {
                        releaseHop(ticket, HostGate::Outcome::Other);
                        return fail(errors::kDisk);
                    }
                } else {
                    got.body.append(buffer.data(), count);
                }
                if (sniffing && got.body.size() >= kSniffPageBytes) {
                    const char* code = resolveSniff();
                    if (*code) {
                        releaseHop(ticket, code == errors::kDisk ? HostGate::Outcome::Other : HostGate::Outcome::Ok, &head);
                        return fail(code);
                    }
                }
                if (spec.progress)
                    reportProgress(got.received, head.contentLength, false);
                if (text && spec.enough && got.received >= nextCheck) {
                    while (nextCheck <= got.received)
                        nextCheck *= 2;
                    if (spec.enough(got.body)) {
                        got.stoppedEarly = true;
                        break; // the exchange is closed at once
                    }
                }
                if (got.truncated)
                    break;
            }
            if (sniffing && !got.body.isEmpty()) {
                const char* code = resolveSniff();
                if (*code) {
                    releaseHop(ticket, code == errors::kDisk ? HostGate::Outcome::Other : HostGate::Outcome::Ok, &head);
                    return fail(code);
                }
            }
            if (!sniffed && !text && headAllowed && !headAllowed(got.head)) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kBadType);
            }
            if (!sniffed && text && !spec.decide && headAllowed && !headAllowed(got.head)) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kBadType);
            }
            if (!got.stoppedEarly && !got.truncated && head.contentLength >= 0 && got.received != head.contentLength) {
                releaseHop(ticket, HostGate::Outcome::Ok, &head);
                return fail(errors::kBadResponse); // cut off
            }
            if (file && !file->flush()) {
                releaseHop(ticket, HostGate::Outcome::Other);
                return fail(errors::kDisk);
            }
            if (spec.progress)
                reportProgress(got.received, head.contentLength, true);
            releaseHop(ticket, HostGate::Outcome::Ok, &head);
            return got;
        }
    }

    // ---- media and pictures -----------------------------------------------------------------------------------

    // A media file under the media rules: written as <key>.<ext>, its kind decided by its bytes (Image / Gif; Video,
    // or Loop when decideLoop and it is silent and at most 60 s). provider: the hop list (empty: any public host).
    // hop: the request's own hop rule (a share's download), else the provider's media list. maxBytes: the cap (-1:
    // caps.media).
    Result loadMedia(const QUrl& url, Kind expected, const QString& provider, bool decideLoop, bool legacy,
                     const std::function<bool(const QUrl&)>& hop = {}, qint64 maxBytes = -1)
    {
        const Kind    family = familyOf(expected);
        const qint64  cap    = maxBytes >= 0 ? maxBytes : caps.media;
        const QString part   = base() + QStringLiteral(".%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return failure(errors::kDisk);
        GetSpec spec;
        spec.url         = url;
        spec.accept      = acceptFor(expected == Kind::Loop ? Kind::Video : expected);
        spec.maxBytes    = cap;
        spec.lane        = HostGate::Lane::Media;
        if (hop) {
            spec.hopAllowed = hop;
        } else if (!provider.isEmpty() && provider != QLatin1String("media-file")) {
            const QString bound = m_boundHost;
            spec.hopAllowed     = [provider, bound](const QUrl& h) { return providers::mediaHopAllowed(provider, h, bound); };
        }
        spec.typeAllowed = [family](const QByteArray& type) { return content::contentTypeAllowed(family, type); };
        spec.headAllowed = [family](const QByteArray& head) { return content::fits(family, content::sniff(head)); };
        spec.file        = &file;
        spec.progress    = true;
        if (cap <= 0) {
            file.close();
            QFile::remove(part);
            Result none = failure(errors::kTooLarge);
            none.kind   = expected;
            return none;
        }
        const Got got = get(spec);
        file.close();
        if (!got.error.isEmpty()) {
            QFile::remove(part);
            Result result = failure(got.error);
            result.retryAfterMs = got.retryAfterMs;
            result.softCooling  = got.softCooling;
            if (got.error == QLatin1String(errors::kTooLarge)) {
                result.size = got.total;
                result.kind = legacy ? Kind::None : expected;
            }
            m_lastStatus = got.status;
            return result;
        }
        return finishMedia(part, content::sniff(got.head), got.received, decideLoop);
    }

    // The media file in place: checked (WebP, pixels), probed, its poster written.
    Result finishMedia(const QString& part, content::Format format, qint64 bytes, bool decideLoop)
    {
        const QString media = base() + QLatin1Char('.') + latin(content::extensionOf(format));
        const bool    ours  = replaceFile(part, media);
        if (!ours) {
            // 2.4 auto-load: the other side's Fetcher (the sender pipeline's or the chat's, one folder) put the same
            // link there a moment ago and still reads it (its poster): its copy, when it is as long as ours, is the
            // same download. Checked below like ours.
            const bool same = QFileInfo(media).isFile() && QFileInfo(media).size() == QFileInfo(part).size();
            QFile::remove(part);
            if (!same)
                return failure(errors::kDisk);
        }
        // Bad bytes: the file goes (only one we put there; the other Fetcher's copy is its own to judge).
        const auto drop = [&media, ours](const char* code) {
            if (ours)
                QFile::remove(media);
            return failure(code);
        };
        // Canceled once the file is in place: it is a complete download, and it may be the file the other Fetcher has
        // just handed out (race R9: our own chat shows it, the pipeline's maker copies it). It stays (the cache cap
        // keeps the folder in bounds).
        if (options.placed)
            options.placed(media);
        if (cancel->isSet())
            return failure(errors::kCanceled);
        const bool video = content::fits(Kind::Video, format);
        Result     result;
        if (!video) {
            if (!webpAllowed(media))
                return drop(errors::kUnsupported); // lossless WebP: never decoded here
            const QSize raw = pictureSize(media);
            if (!raw.isValid() || raw.isEmpty())
                return drop(errors::kBadType);
            if (static_cast<qint64>(raw.width()) * raw.height() > options.maxMediaPixels)
                return drop(errors::kTooManyPixels);
            // Size and a smaller picture, the way TS Media's own uploads get them.
            const LocalMediaInfo info = probeLocalMedia(media, true);
            if (cancel->isSet())
                return failure(errors::kCanceled); // the complete file stays (above)
            result.width  = info.width > 0 ? info.width : raw.width();
            result.height = info.height > 0 ? info.height : raw.height();
            result.kind   = format == content::Format::Gif || info.animated ? Kind::Gif : Kind::Image;
            if (!info.preview.isNull())
                writePoster(info.preview, &result);
        } else {
            VideoProbe probe;
            probeVideo(media, &probe);
            if (cancel->isSet())
                return failure(errors::kCanceled);
            if (probe.ok && !probe.poster.isNull()
                && static_cast<qint64>(probe.poster.width()) * probe.poster.height() > options.maxMediaPixels)
                probe.poster = QImage();
            result.width      = probe.width;
            result.height     = probe.height;
            result.durationMs = probe.durationMs;
            result.hasAudio   = probe.hasAudio;
            result.kind       = decideLoop && probe.ok && !probe.hasAudio && probe.durationMs > 0 && probe.durationMs <= kLoopMaxMs ? Kind::Loop : Kind::Video;
            if (!probe.poster.isNull())
                writePoster(probe.poster, &result);
        }
        result.localFile = media;
        result.mime      = latin(content::mimeOf(format));
        result.size      = bytes;
        result.ok        = true;
        return result;
    }

    void probeVideo(const QString& media, VideoProbe* out)
    {
        if (options.probe) {
            options.probe(media, out);
            return;
        }
        const LocalMediaInfo info = probeLocalMedia(media, true);
        out->ok         = info.probed && info.hasVideo;
        out->durationMs = info.durationMs;
        out->width      = info.width;
        out->height     = info.height;
        out->hasAudio   = info.hasAudio;
        out->poster     = info.preview;
    }

    void writePoster(const QImage& image, Result* result)
    {
        const QString    thumb = base() + latin("-thumb.jpg");
        const QByteArray jpeg  = encodePreviewJpeg(image);
        if (!jpeg.isEmpty() && writeFileAtomic(thumb, jpeg, id))
            result->thumbnailFile = thumb;
    }

    struct Picture {
        QString file, mime;
        int     width = 0, height = 0;
        qint64  bytes = 0;
    };

    // A card's picture: written as <key>-thumb.<ext> under caps.picture, the picture rules (no SVG, magic bytes,
    // lossy WebP only, the decode caps). provider: the hop list (empty: any public host, the Web class).
    QString loadPicture(const QUrl& url, const QString& provider, Picture* out)
    {
        if (caps.picture <= 0)
            return latin(errors::kTooLarge);
        const QString part = base() + QStringLiteral("-thumb.%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return latin(errors::kDisk);
        GetSpec spec;
        spec.url      = url;
        spec.accept   = acceptFor(Kind::Image);
        spec.maxBytes = qMin(caps.picture, net::kMaxThumbnailBytes);
        spec.lane     = HostGate::Lane::Media;
        if (!provider.isEmpty()) {
            const QString bound = m_boundHost;
            spec.hopAllowed     = [provider, bound](const QUrl& hop) { return providers::mediaHopAllowed(provider, hop, bound); };
        } else {
            spec.hopAllowed = [](const QUrl& hop) { return webPictureAllowed(hop); }; // a Web page's picture: any host, no action address
        }
        spec.typeAllowed = [](const QByteArray& type) { return content::contentTypeAllowed(Kind::Image, type); };
        spec.headAllowed = [](const QByteArray& head) { return content::fits(Kind::Image, content::sniff(head)); };
        spec.file        = &file;
        const Got got    = get(spec);
        file.close();
        if (!got.error.isEmpty()) {
            QFile::remove(part);
            m_lastPictureRetry = got.retryAfterMs;
            m_lastPictureSoft  = got.softCooling;
            return got.error;
        }
        const content::Format format = content::sniff(got.head);
        const QString         path   = base() + latin("-thumb.") + latin(content::extensionOf(format));
        if (!replaceFile(part, path)) {
            QFile::remove(part);
            return latin(errors::kDisk);
        }
        if (!webpAllowed(path)) {
            QFile::remove(path);
            return latin(errors::kUnsupported);
        }
        const QSize size = pictureSize(path);
        if (!size.isValid() || size.isEmpty()) {
            QFile::remove(path);
            return latin(errors::kBadType);
        }
        if (static_cast<qint64>(size.width()) * size.height() > options.maxPicturePixels || size.width() > kMaxPictureSide || size.height() > kMaxPictureSide) {
            QFile::remove(path);
            return latin(errors::kTooManyPixels);
        }
        // Filled in only once the picture is on disk (live test fix: a copy that failed left the card naming a file
        // that was gone, and the chat loaded that link again after every other load).
        Picture done;
        done.file   = path;
        done.mime   = latin(content::mimeOf(format));
        done.width  = size.width();
        done.height = size.height();
        done.bytes  = got.received;
        if (size.width() > kCardPictureSide || size.height() > kCardPictureSide) {
            // Larger than a card ever draws it: a smaller copy, made here once (never decoded in full on the GUI
            // thread, for every size the chat paints it at).
            QSize         small;
            qint64        smallBytes = 0;
            const QString shrunk     = shrinkPicture(path, base(), &small, &smallBytes);
            if (cancel->isSet()) {
                QFile::remove(shrunk.isEmpty() ? path : shrunk);
                return latin(errors::kCanceled);
            }
            if (shrunk.isEmpty()) {
                QFile::remove(path);
                return latin(errors::kBadType);
            }
            done.file   = shrunk;
            done.mime   = shrunk.endsWith(QLatin1String(".png")) ? latin("image/png") : latin("image/jpeg");
            done.width  = small.width();
            done.height = small.height();
            done.bytes  = smallBytes;
        }
        *out = done;
        return {};
    }

    // The first picture of the candidates (resolved against base) that loads; at most maxTries requests.
    bool pictureFrom(const QStringList& candidates, const QUrl& base, const QString& provider, bool web, int maxTries, Picture* out)
    {
        int tries = 0;
        for (const QString& raw : candidates) {
            if (tries >= maxTries || cancel->isSet())
                break;
            const QUrl url = net::resolveExtracted(base, raw);
            if (!url.isValid())
                continue;
            if (web && !webPictureAllowed(url))
                continue; // a page can't make every viewer ask for an action, a token or a share's file address
            if (!provider.isEmpty() && !providers::mediaHopAllowed(provider, url, m_boundHost)) {
                m_hopRefused = publicHostName(url);
                continue;
            }
            ++tries;
            const QString error = loadPicture(url, provider, out);
            if (error.isEmpty())
                return true;
            if (error == QLatin1String(errors::kCanceled) || error == QLatin1String(errors::kRateLimited) || error == QLatin1String(errors::kGateBusy)
                || error == QLatin1String(errors::kWalled)) {
                // Its site wants a pause (or the load ends): the whole load ends with it and is tried again later, so
                // the card comes with its picture then.
                m_pictureStop      = error;
                m_pictureStopRetry = m_lastPictureRetry;
                m_pictureStopSoft  = m_lastPictureSoft;
                break;
            }
        }
        return false;
    }

    // ---- pages, JSON, feeds ---------------------------------------------------------------------------------------

    struct Page {
        QString        error;
        pagemeta::Meta meta;
        QUrl           finalUrl;
        bool           challenge = false;
        Got            media; // a Web page that answered with media
    };

    // A page read up to its head (bodyMode: to caps.page), decoded and scanned. web: the Web class (Accept, media
    // answers, no hop list).
    Page readPage(const QUrl& url, bool bodyMode, bool web, const QString& provider, QFile* mediaFile = nullptr, Kind linkMedia = Kind::None)
    {
        Page    page;
        GetSpec spec;
        spec.url        = url;
        spec.accept     = web && linkMedia != Kind::None ? acceptFor(linkMedia)
                          : web ? QByteArray("text/html,application/xhtml+xml;q=0.9,image/*;q=0.8,video/*;q=0.7,*/*;q=0.1")
                                : QByteArray("text/html,application/xhtml+xml;q=0.9,*/*;q=0.1");
        spec.maxBytes   = qMin(qMax<qint64>(16 * 1024, caps.page), net::kMaxPageBytes);
        spec.lane       = HostGate::Lane::Page;
        spec.text       = true;
        spec.truncateOk = true;
        spec.pageClock  = true;
        if (!web)
            spec.hopAllowed = [provider](const QUrl& hop) { return providers::pageHopAllowed(provider, hop); };
        else
            spec.hopAllowed = [](const QUrl& hop) { return webHopAllowed(hop); };
        spec.enough =[bodyMode](const QByteArray& soFar) { return pagemeta::headDone(pagemeta::scan(soFar, nullptr, bodyMode), bodyMode); };
        spec.decide = [web, mediaFile](const QByteArray& type, Kind* kind) {
            if (isHtmlType(type) || isGenericType(type))
                return HeadAction::Text;
            if (web && mediaFile) {
                if (type.startsWith("image/") && !type.contains("svg")) {
                    *kind = type == "image/gif" ? Kind::Gif : Kind::Image;
                    return HeadAction::Media;
                }
                if (type == "video/mp4" || type == "video/webm" || type == "video/quicktime") {
                    *kind = Kind::Video;
                    return HeadAction::Media;
                }
            }
            return HeadAction::Fail;
        };
        spec.mediaFile = web ? mediaFile : nullptr;
        spec.mediaMax  = web ? caps.media : 0;
        const Got got  = get(spec);
        page.challenge = got.challenge;
        page.finalUrl  = got.finalUrl.isValid() ? got.finalUrl : url;
        if (got.switched) {
            page.media = got;
            page.error = got.error;
            return page;
        }
        if (!got.error.isEmpty()) {
            page.error = got.error;
            m_lastStatus = got.status;
            m_lastRetry  = got.retryAfterMs;
            m_lastSoft   = got.softCooling;
            return page;
        }
        const pagemeta::Decoder decoder = pagemeta::decoderFor(got.body, got.contentType);
        page.meta = pagemeta::scanPage(pagemeta::prepared(got.body, decoder), decoder.codec, bodyMode, got.truncated);
        return page;
    }

    // An oEmbed / keyless JSON answer (at most net::kMaxJsonBytes).
    QString readJson(const QUrl& url, const QString& provider, QByteArray* body)
    {
        GetSpec spec;
        spec.url         = url;
        spec.accept      = "application/json";
        spec.maxBytes    = net::kMaxJsonBytes;
        spec.lane        = HostGate::Lane::Page;
        spec.text        = true;
        spec.pageClock   = true;
        spec.hopAllowed  = [provider](const QUrl& hop) { return providers::pageHopAllowed(provider, hop); };
        spec.typeAllowed = [](const QByteArray& type) { return content::isJsonType(type); };
        spec.headAllowed = [](const QByteArray& head) { return head.trimmed().startsWith('{'); };
        const Got got    = get(spec);
        m_lastStatus     = got.status;
        m_lastRetry      = got.retryAfterMs;
        m_lastSoft       = got.softCooling;
        if (!got.error.isEmpty())
            return got.error;
        *body = got.body;
        return {};
    }

    // A feed (Atom) or a manifest (DASH), read with the xml-lite tokenizer.
    QString readXml(const QUrl& url, const QString& provider, qint64 cap, bool manifest, QByteArray* body)
    {
        GetSpec spec;
        spec.url        = url;
        spec.accept     = manifest ? "application/dash+xml,application/xml;q=0.9,*/*;q=0.1" : "application/atom+xml,application/xml;q=0.9,*/*;q=0.1";
        spec.maxBytes   = cap;
        spec.lane       = HostGate::Lane::Page;
        spec.text       = true;
        spec.pageClock  = true;
        spec.hopAllowed = [provider](const QUrl& hop) { return providers::pageHopAllowed(provider, hop); };
        spec.typeAllowed = [manifest](const QByteArray& contentType) {
            const QByteArray type = content::mediaType(contentType);
            if (manifest)
                return type == "application/dash+xml" || type == "application/xml" || type == "text/xml" || type == "video/vnd.mpeg.dash.mpd";
            return type == "application/atom+xml" || type == "application/rss+xml" || type == "application/xml" || type == "text/xml";
        };
        const Got got = get(spec);
        m_lastStatus  = got.status;
        m_lastRetry   = got.retryAfterMs;
        m_lastSoft    = got.softCooling;
        if (!got.error.isEmpty())
            return got.error;
        *body = got.body;
        return {};
    }

    // A Redirect step: the first Location that is a link of provider (re-classified). Never a body.
    QString followRedirect(const QUrl& url, const QString& provider, Target* next)
    {
        GetSpec spec;
        spec.url        = url;
        spec.accept     = "text/html,*/*;q=0.1";
        spec.maxBytes   = 0;
        spec.lane       = HostGate::Lane::Page;
        spec.headOnly   = true;
        spec.pageClock  = true;
        spec.hopAllowed = [provider](const QUrl& hop) { return providers::pageHopAllowed(provider, hop); };
        // The first Location that is a link of the provider ends the step; one on another site (a short link to
        // something the row doesn't preview) ends it too, with nothing and without a request to it.
        spec.stopAt = [provider, next](const QUrl& location) {
            const Target t = classify(location);
            if (t.provider == provider && t.cls != LinkClass::None && t.sub != QLatin1String("short")) {
                *next = t;
                return true;
            }
            return !providers::pageHopAllowed(provider, location);
        };
        const Got got = get(spec);
        if (!got.error.isEmpty()) {
            m_lastRetry = got.retryAfterMs;
            m_lastSoft  = got.softCooling;
            return got.error;
        }
        if (!got.redirectStop || !next->isValid())
            return latin(errors::kNothing); // a 200 (a home page) or another site: nothing to show
        return {};
    }

    Result fromError(const QString& error, qint64 retryAfterMs = 0, bool soft = false)
    {
        Result r       = failure(error);
        r.retryAfterMs = retryAfterMs;
        r.softCooling  = soft;
        return r;
    }

    Result pageError(const Page& page)
    {
        if (page.challenge || page.error == QLatin1String(errors::kWalled))
            return failure(errors::kWalled);
        return fromError(page.error, m_lastRetry, m_lastSoft);
    }

    // ---- cards ----------------------------------------------------------------------------------------------------

    // A card of a provider (Site rows, Tumblr text posts): its brand as the site line, never a page's colour.
    Result siteCard(const QString& provider, const QString& title, const QString& description, const QString& author, const Picture& picture,
                    CardLayout layout, bool play, qint64 durationMs, bool sensitive)
    {
        if (!m_pictureStop.isEmpty())
            return fromError(m_pictureStop, m_pictureStopRetry, m_pictureStopSoft);
        CardData card;
        const providers::Brand brand = providers::brandOf(provider);
        card.site        = brand.name ? latin(brand.name) : QString();
        card.title       = textclean::clean(title, kCardTitleChars);
        card.description = textclean::clean(description, kCardDescriptionChars);
        card.author      = textclean::clean(author, kCardAuthorChars);
        card.sensitive   = sensitive;
        card.play        = play;
        card.durationMs  = play ? qBound<qint64>(0, durationMs, 86400000) : 0;
        if (!picture.file.isEmpty()) {
            card.pictureFile   = picture.file;
            card.pictureWidth  = picture.width;
            card.pictureHeight = picture.height;
            card.layout        = layout == CardLayout::None ? CardLayout::Thumb : layout;
        }
        if (card.title.isEmpty() || (card.description.isEmpty() && card.pictureFile.isEmpty())) {
            if (!picture.file.isEmpty())
                QFile::remove(picture.file);
            return failure(errors::kNothing);
        }
        Result result;
        result.ok            = true;
        result.kind          = Kind::Card;
        result.card          = card;
        result.thumbnailFile = card.pictureFile;
        result.mime          = picture.mime;
        result.width         = picture.width;
        result.height        = picture.height;
        result.size          = picture.bytes;
        return result;
    }

    // Large when the picture is at least 400 px wide, Thumb otherwise.
    static CardLayout bySize(const Picture& picture)
    {
        return picture.width >= 400 ? CardLayout::Large : CardLayout::Thumb;
    }

    // ---- provider plans ---------------------------------------------------------------------------------------

    Result runProvider(const Target& t)
    {
        const QString& p = t.provider;
        if (p == QLatin1String("media-file"))
            return loadMedia(t.page, t.kind, latin("media-file"), true, false);
        if (p == QLatin1String("nextcloud"))
            return planShare(t);
        if (t.sub == QLatin1String("short") && p != QLatin1String("steam")) {
            Target next;
            // The short link without its tracking parameters (2.7), like every other request for the link itself.
            const QString error = followRedirect(t.fetchUrl, p, &next);
            if (!error.isEmpty())
                return fromError(error, m_lastRetry, m_lastSoft);
            return runProvider(next);
        }
        if (p == QLatin1String("klipy"))
            return planKlipy(t);
        if (p == QLatin1String("tenor"))
            return planTenor(t);
        if (p == QLatin1String("giphy"))
            return planGiphy(t);
        if (p == QLatin1String("imgur"))
            return planImgur(t);
        if (p == QLatin1String("reddit-video"))
            return planRedditVideo(t);
        if (p == QLatin1String("pinterest"))
            return planPinterest(t);
        if (p == QLatin1String("streamable"))
            return loadMedia(QUrl(latin("https://api-f.streamable.com/api/v1/videos/") + t.itemId + latin("/mp4")), Kind::Video, p, true, false);
        if (p == QLatin1String("imgflip"))
            return planImgflip(t);
        if (p == QLatin1String("9gag"))
            return derive({latin("https://img-9gag-fun.9cache.com/photo/") + t.itemId + latin("_460sv.mp4")},
                          {latin("https://img-9gag-fun.9cache.com/photo/") + t.itemId + latin("_700b.jpg")}, p);
        if (p == QLatin1String("coub"))
            return loadMedia(QUrl(latin("https://tg.coub.com/media/share/") + t.itemId + latin(".mp4")), Kind::Video, p, true, false);
        if (p == QLatin1String("gifer"))
            return derive({latin("https://i.gifer.com/") + t.itemId + latin(".mp4")}, {latin("https://i.gifer.com/") + t.itemId + latin(".gif")}, p);
        if (p == QLatin1String("makeagif"))
            return planPageMedia(t, true);
        if (p == QLatin1String("gyazo"))
            return planGyazo(t);
        if (p == QLatin1String("lightshot") || p == QLatin1String("imgbb") || p == QLatin1String("postimages"))
            return planPageMedia(t, false);
        if (p == QLatin1String("pixeldrain"))
            return planPixeldrain(t);
        if (p == QLatin1String("tumblr"))
            return planTumblr(t);
        if (p == QLatin1String("vimeo"))
            return planVimeo(t);
        if (p == QLatin1String("soundcloud"))
            return planOEmbedCard(t, QUrl(latin("https://soundcloud.com/oembed?format=json&url=") + percentEncoded(t.fetchUrl.toString(QUrl::FullyEncoded))),
                                  CardLayout::Thumb, false, true);
        if (p == QLatin1String("spotify"))
            return planSpotify(t);
        if (p == QLatin1String("applemusic"))
            return planAppleMusic(t);
        if (p == QLatin1String("deezer")) {
            const QString page = latin("https://www.deezer.com/") + t.sub + QLatin1Char('/') + t.itemId;
            return planOEmbedCard(t, QUrl(latin("https://api.deezer.com/oembed?url=") + percentEncoded(page)), CardLayout::Thumb, false, false);
        }
        if (p == QLatin1String("dailymotion")) {
            const QString page = latin("https://www.dailymotion.com/video/") + t.itemId;
            return planOEmbedCard(t, QUrl(latin("https://www.dailymotion.com/services/oembed?url=") + percentEncoded(page)), CardLayout::Large, true, false);
        }
        if (p == QLatin1String("medal"))
            return planOEmbedCard(t, QUrl(latin("https://medal.tv/api/oembed?url=") + percentEncoded(t.fetchUrl.toString(QUrl::FullyEncoded))),
                                  CardLayout::Large, true, false);
        if (p == QLatin1String("twitch"))
            return planTwitch(t);
        if (p == QLatin1String("kick"))
            return planKick(t);
        if (p == QLatin1String("bluesky"))
            return planBluesky(t);
        if (p == QLatin1String("x"))
            return planX(t);
        if (p == QLatin1String("github"))
            return planPageCard(t, t.fetchUrl, CardLayout::Large);
        if (p == QLatin1String("steam"))
            return planSteam(t);
        if (p == QLatin1String("wikipedia"))
            return planWikipedia(t);
        if (p == QLatin1String("telegram"))
            return planTelegram(t);
        if (p == QLatin1String("discord"))
            return planDiscord(t);
        if (p == QLatin1String("reddit"))
            return planReddit(t);
        if (p == QLatin1String("youtube-extra"))
            return planYouTubeExtra(t);
        return failure(errors::kNotPreviewable);
    }

    // Derive: the first address; the fallbacks on notFound / HTTP 400 / a wrong type.
    Result derive(const QStringList& first, const QStringList& fallbacks, const QString& provider, Kind expected = Kind::Video)
    {
        Result result = loadMedia(QUrl(first.value(0)), expected, provider, true, false);
        if (result.ok || !isFallbackError(result))
            return result;
        for (const QString& url : fallbacks) {
            const Kind kind = mediaKindOfName(url.mid(url.lastIndexOf(QLatin1Char('/')) + 1));
            result          = loadMedia(QUrl(url), kind == Kind::None ? Kind::Image : kind, provider, true, false);
            if (result.ok || !isFallbackError(result))
                return result;
        }
        return result;
    }

    // D24 / 4.4: media taken from a page that declares adult content is marked like the page's card would be (the
    // spoiler cover here, sn=1 on the sender's LPX).
    static Result pageMedia(Result result, const pagemeta::Meta& meta)
    {
        if (result.ok && pagemeta::isSensitive(meta))
            result.card.sensitive = true;
        return result;
    }

    bool isFallbackError(const Result& r) const
    {
        return r.error == QLatin1String(errors::kNotFound) || r.error == QLatin1String(errors::kBadType)
               || (r.error == QLatin1String(errors::kBadResponse) && m_lastStatus == 400);
    }

    Result planKlipy(const Target& t)
    {
        // Klipy's GIF pages are behind a bot challenge for an honest client (author's decision 2026-11-11): while the
        // gate remembers that wall the page isn't asked again; a walled page's GIF is asked of Klipy's oEmbed.
        const bool gifs = !providers::wallFallbackOf(t).host.isEmpty();
        if (gifs && pageWalled(t.fetchUrl))
            return klipyOEmbed(t);
        const Page page = readPage(t.fetchUrl, false, false, t.provider);
        if (!page.error.isEmpty())
            return gifs && (page.challenge || page.error == QLatin1String(errors::kWalled)) ? klipyOEmbed(t) : pageError(page);
        if (pagemeta::wallOf(page.meta, t.page, page.finalUrl, page.challenge) == pagemeta::Verdict::Walled)
            return gifs ? klipyOEmbed(t) : failure(errors::kWalled);
        for (const pagemeta::Meta::Video& v : page.meta.ogVideos) {
            const QUrl    url  = net::resolveExtracted(page.finalUrl, v.url);
            const QString path = url.path(QUrl::FullyDecoded).toLower();
            if (url.isValid() && (v.type == QLatin1String("video/mp4") || v.type == QLatin1String("video/webm") || path.endsWith(QLatin1String(".mp4"))
                                  || path.endsWith(QLatin1String(".webm"))))
                return pageMedia(loadMedia(url, Kind::Video, t.provider, true, false), page.meta);
        }
        const QUrl image = net::resolveExtracted(page.finalUrl, page.meta.ogImages.value(0));
        if (!image.isValid())
            return failure(errors::kNothing);
        return pageMedia(loadMedia(image, Kind::Gif, t.provider, true, false), page.meta);
    }

    // The gate remembers a bot challenge of this page's site and section (no request).
    bool pageWalled(const QUrl& url) const
    {
        qint64 ms = 0;
        return gate->state(publicHostName(url), HostGate::Lane::Page, firstSegmentOf(url), &ms) == HostGate::Verdict::Walled;
    }

    // A walled Klipy GIF page: Klipy's public oEmbed (no key; spaced and gated like every request to klipy.com). Its
    // thumbnail_url is the GIF when it names a real media file on Klipy's static hosts (providers::klipyOEmbedMedia),
    // loaded under the media rules. Anything else is the wall's own answer, quietly: nothing (kWalled, this link's
    // record kept 30 minutes). An answer that came through but names no GIF (2026-11-11: "https://static2.klipy.com/")
    // walls the oEmbed in the gate for 30 minutes too, like the page: the next GIF page makes no request meanwhile.
    // A busy site keeps its own answer (the load is tried again when the site allows).
    Result klipyOEmbed(const Target& t)
    {
        const providers::FirstRequest oembed = providers::wallFallbackOf(t);
        QByteArray                    body;
        const QString                 error = readJson(providers::klipyOEmbedUrl(t.itemId), t.provider, &body);
        if (error == QLatin1String(errors::kCanceled))
            return failure(error);
        if (error == QLatin1String(errors::kRateLimited) || error == QLatin1String(errors::kGateBusy))
            return fromError(error, m_lastRetry, m_lastSoft);
        const QUrl gif = error.isEmpty() ? providers::klipyOEmbedMedia(body) : QUrl();
        if (!gif.isValid()) {
            if (m_lastStatus == 200)
                gate->markWalled(oembed.host, oembed.firstSegment);
            return failure(errors::kWalled);
        }
        return loadMedia(gif, Kind::Gif, t.provider, true, false);
    }

    // Tenor's media id: the big ids encode it; small ones are asked of oEmbed (its thumbnail names it).
    QString tenorMediaId(const Target& t, const QString& pageUrl, QString* error)
    {
        bool          ok     = false;
        const quint64 postId = t.itemId.toULongLong(&ok);
        if (ok && postId > 0xFFFFFFFFULL)
            return providers::tenorMediaId(postId);
        return tenorMediaIdByOEmbed(pageUrl, error);
    }

    QString tenorMediaIdByOEmbed(const QString& pageUrl, QString* error)
    {
        QByteArray    body;
        const QString e = readJson(QUrl(latin("https://tenor.com/oembed?url=") + percentEncoded(pageUrl)), latin("tenor"), &body);
        if (!e.isEmpty()) {
            *error = e;
            return {};
        }
        content::OEmbedData data;
        if (!content::parseOEmbedData(body, &data, nullptr)) {
            *error = latin(errors::kBadResponse);
            return {};
        }
        const QUrl        thumb    = net::resolveExtracted(QUrl(latin("https://tenor.com/")), data.thumbnailUrl);
        const QStringList segments = thumb.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        static const QRegularExpression kId(QStringLiteral("^[A-Za-z0-9_-]{6,40}$"));
        const QString media = segments.isEmpty() ? QString() : segments.first().left(qMax(0, segments.first().size() - 5));
        if (!thumb.isValid() || !kId.match(media).hasMatch()) {
            *error = latin(errors::kNothing);
            return {};
        }
        return media;
    }

    Result planTenor(const Target& t)
    {
        bool          ok     = false;
        const quint64 postId = t.itemId.toULongLong(&ok);
        QString       error;
        if (ok && postId > 0xFFFFFFFFULL) {
            const Result direct = loadMedia(QUrl(latin("https://media.tenor.com/") + providers::tenorMediaId(postId) + latin("AAAPo/tenor.mp4")), Kind::Loop,
                                            t.provider, true, false);
            if (direct.ok || !isFallbackError(direct))
                return direct;
        }
        const QString media = tenorMediaIdByOEmbed(t.fetchUrl.toString(QUrl::FullyEncoded), &error);
        if (media.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        return loadMedia(QUrl(latin("https://media.tenor.com/") + media + latin("AAAPo/tenor.mp4")), Kind::Loop, t.provider, true, false);
    }

    // Tenor's short GIF link (tenor.com/<code>.gif, a Direct link): its GIF rendition, never the page.
    Result loadTenorShort()
    {
        Target        view;
        const QString error = followRedirect(target.url, latin("tenor"), &view);
        if (!error.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        QString       why;
        const QString media = tenorMediaId(view, view.page.toString(QUrl::FullyEncoded), &why);
        if (media.isEmpty())
            return fromError(why, m_lastRetry, m_lastSoft);
        Result result = loadMedia(QUrl(latin("https://media.tenor.com/") + media + latin("AAAAd/tenor.gif")), Kind::Gif, latin("tenor-short"), false, true);
        if (result.ok)
            result.kind = Kind::Gif;
        return result;
    }

    // Dropbox share links: the same address with raw=1 (dl=0 / dl=1 removed, every other parameter kept).
    Result loadDropbox()
    {
        QUrl        url = target.url;
        QStringList kept;
        for (const QString& item : url.query(QUrl::FullyEncoded).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            const QString name = item.section(QLatin1Char('='), 0, 0).toLower();
            if (name != QLatin1String("dl") && name != QLatin1String("raw"))
                kept << item;
        }
        kept << latin("raw=1");
        url.setQuery(kept.join(QLatin1Char('&')), QUrl::StrictMode);
        return loadMedia(url, target.kind, latin("dropbox"), false, true);
    }

    Result planGiphy(const Target& t)
    {
        const QString gif = t.itemId;
        if (t.sub == QLatin1String("stickers"))
            return loadMedia(QUrl(latin("https://i.giphy.com/") + gif + latin(".webp")), Kind::Gif, t.provider, true, false);
        if (t.sub == QLatin1String("clips"))
            return derive({latin("https://media.giphy.com/media/") + gif + latin("/giphy480p.mp4")}, {latin("https://i.giphy.com/") + gif + latin(".mp4")},
                          t.provider);
        return loadMedia(QUrl(latin("https://i.giphy.com/") + gif + latin(".mp4")), Kind::Loop, t.provider, true, false);
    }

    Result planImgur(const Target& t)
    {
        if (t.sub == QLatin1String("album")) {
            const Page page = readPage(t.fetchUrl, false, false, t.provider);
            if (!page.error.isEmpty())
                return pageError(page);
            const pagemeta::Meta& m = page.meta;
            const auto            mp4 = [&page](const QString& raw) {
                const QUrl url = net::resolveExtracted(page.finalUrl, raw);
                return url.isValid() && url.path(QUrl::FullyDecoded).toLower().endsWith(QLatin1String(".mp4")) ? url : QUrl();
            };
            QUrl video = mp4(m.twitterPlayerStream);
            for (int i = 0; !video.isValid() && i < m.ogVideos.size(); ++i)
                video = mp4(m.ogVideos.at(i).url); // og:video:type may lie: the path decides
            if (video.isValid())
                return pageMedia(loadMedia(video, Kind::Video, t.provider, true, false), m);
            QUrl image = net::resolveExtracted(page.finalUrl, m.ogImages.value(0));
            if (!image.isValid())
                return failure(errors::kNothing); // a shell with only og:site_name
            image.setQuery(QString());
            return pageMedia(loadMedia(image, Kind::Image, t.provider, true, false), m);
        }
        // gifv and post links: the MP4; an HTTP 400 means it is a picture.
        const Result video = loadMedia(QUrl(latin("https://i.imgur.com/") + t.itemId + latin(".mp4")), Kind::Loop, t.provider, true, false);
        if (video.ok || !(video.error == QLatin1String(errors::kBadResponse) && m_lastStatus == 400) )
            return video;
        return loadMedia(QUrl(latin("https://i.imgur.com/") + t.itemId + latin(".jpeg")), Kind::Image, t.provider, true, false);
    }

    Result planRedditVideo(const Target& t)
    {
        QByteArray    body;
        const QString error = readXml(QUrl(latin("https://v.redd.it/") + t.itemId + latin("/DASHPlaylist.mpd")), t.provider, net::kMaxManifestBytes, true, &body);
        if (!error.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        const pagemeta::Manifest manifest = pagemeta::readManifest(body);
        if (manifest.hasAudio)
            return failure(errors::kNothing); // a video with sound: its audio would need muxing
        const QString name = pagemeta::pickManifestVideo(manifest, 480);
        if (name.isEmpty())
            return failure(errors::kNothing);
        return loadMedia(QUrl(latin("https://v.redd.it/") + t.itemId + QLatin1Char('/') + name), Kind::Loop, t.provider, true, false);
    }

    Result planPinterest(const Target& t)
    {
        const QString pin = latin("https://www.pinterest.com/pin/") + t.itemId + QLatin1Char('/');
        QByteArray    body;
        const QString error = readJson(QUrl(latin("https://www.pinterest.com/oembed.json?url=") + percentEncoded(pin)), t.provider, &body);
        if (!error.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        content::OEmbedData data;
        if (!content::parseOEmbedData(body, &data, nullptr))
            return failure(errors::kBadResponse);
        const QUrl small = net::resolveExtracted(QUrl(pin), data.thumbnailUrl);
        if (!small.isValid())
            return failure(errors::kNothing);
        QUrl big = small;
        big.setPath(small.path(QUrl::FullyDecoded).replace(QLatin1String("/236x/"), QLatin1String("/736x/")));
        Result result = loadMedia(big, Kind::Image, t.provider, false, false);
        if (!result.ok && big != small && (result.error == QLatin1String(errors::kNotFound) || result.error == QLatin1String(errors::kForbidden)))
            result = loadMedia(small, Kind::Image, t.provider, false, false);
        return result;
    }

    Result planImgflip(const Target& t)
    {
        if (t.sub == QLatin1String("gif"))
            return derive({latin("https://i.imgflip.com/") + t.itemId + latin(".mp4")}, {latin("https://i.imgflip.com/") + t.itemId + latin(".gif")},
                          t.provider);
        Result result = loadMedia(QUrl(latin("https://i.imgflip.com/") + t.itemId + latin(".jpg")), Kind::Image, t.provider, false, false);
        if (result.ok || !isFallbackError(result))
            return result;
        return planPageMedia(t, false);
    }

    // A page whose og:video (an .mp4) or og:image is the media (Make A Gif, Lightshot, ImgBB, Postimages, Imgflip).
    Result planPageMedia(const Target& t, bool video)
    {
        const Page page = readPage(t.fetchUrl, false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        if (video) {
            QStringList streams;
            for (const pagemeta::Meta::Video& v : page.meta.ogVideos)
                streams << v.url;
            streams << page.meta.twitterPlayerStream;
            for (const QString& raw : qAsConst(streams)) {
                const QUrl url = net::resolveExtracted(page.finalUrl, raw);
                if (url.isValid() && url.path(QUrl::FullyDecoded).toLower().endsWith(QLatin1String(".mp4")))
                    return pageMedia(loadMedia(url, Kind::Video, t.provider, true, false), page.meta);
            }
        }
        const QUrl image = net::resolveExtracted(page.finalUrl, page.meta.ogImages.value(0));
        if (!image.isValid())
            return failure(errors::kNothing);
        return pageMedia(loadMedia(image, Kind::Image, t.provider, false, false), page.meta);
    }

    Result planGyazo(const Target& t)
    {
        QByteArray    body;
        const QString error =
            readJson(QUrl(latin("https://api.gyazo.com/api/oembed?url=") + percentEncoded(latin("https://gyazo.com/") + t.itemId)), t.provider, &body);
        if (!error.isEmpty()) {
            if (error == QLatin1String(errors::kServerError) && m_lastStatus == 503)
                return failure(errors::kNothing); // "temporarily unavailable" for this item
            return fromError(error, m_lastRetry, m_lastSoft);
        }
        content::OEmbedData data;
        if (!content::parseOEmbedData(body, &data, nullptr))
            return failure(errors::kBadResponse);
        if (data.type == QLatin1String("video"))
            return loadMedia(QUrl(latin("https://i.gyazo.com/") + t.itemId + latin(".mp4")), Kind::Video, t.provider, true, false);
        const QUrl url = net::resolveExtracted(QUrl(latin("https://gyazo.com/")), data.url);
        if (data.type != QLatin1String("photo") || !url.isValid())
            return failure(errors::kNothing);
        return loadMedia(url, Kind::Image, t.provider, true, false);
    }

    Result planPixeldrain(const Target& t)
    {
        QByteArray    body;
        const QString error = readJson(QUrl(latin("https://pixeldrain.com/api/file/") + t.itemId + latin("/info")), t.provider, &body);
        if (!error.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        content::PixeldrainInfo info;
        if (!content::parsePixeldrainInfo(body, &info, nullptr))
            return failure(errors::kBadResponse);
        static const QStringList kTypes = {latin("image/png"), latin("image/jpeg"), latin("image/gif"), latin("image/webp"),
                                           latin("video/mp4"), latin("video/webm"), latin("video/quicktime")};
        if (!kTypes.contains(info.mimeType))
            return failure(errors::kNothing);
        const Kind kind = info.mimeType.startsWith(QLatin1String("video/")) ? Kind::Video : info.mimeType == QLatin1String("image/gif") ? Kind::Gif : Kind::Image;
        if (info.size > caps.media || info.size < 0) {
            Result tooLarge = failure(errors::kTooLarge);
            tooLarge.size   = info.size;
            tooLarge.kind   = kind;
            return tooLarge;
        }
        return loadMedia(QUrl(latin("https://pixeldrain.com/api/file/") + t.itemId), kind, t.provider, true, false);
    }

    Result planTumblr(const Target& t)
    {
        const Page page = readPage(t.fetchUrl, false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        const pagemeta::Meta& m = page.meta;
        for (const pagemeta::Meta::Video& v : m.ogVideos) {
            const QUrl url = net::resolveExtracted(page.finalUrl, v.url);
            if (url.isValid() && url.path(QUrl::FullyDecoded).toLower().endsWith(QLatin1String(".mp4")) && publicHostName(url) == QLatin1String("va.media.tumblr.com"))
                return pageMedia(loadMedia(url, Kind::Video, t.provider, true, false), m);
        }
        const QUrl image = net::resolveExtracted(page.finalUrl, m.ogImages.value(0));
        if (image.isValid() && publicHostName(image).endsWith(QLatin1String(".media.tumblr.com")))
            return pageMedia(loadMedia(image, Kind::Image, t.provider, true, false), m);
        // A text post: a card with Tumblr's brand.
        if (pagemeta::wallOf(m, t.page, page.finalUrl, page.challenge) != pagemeta::Verdict::Card)
            return failure(errors::kNothing);
        return siteCard(t.provider, pagemeta::titleOf(m), pagemeta::descriptionOf(m), QString(), Picture(), CardLayout::None, false, 0, pagemeta::isSensitive(m));
    }

    // An oEmbed card: title, author, description (tags stripped when asked), the thumbnail as its picture.
    Result planOEmbedCard(const Target& t, const QUrl& endpoint, CardLayout layout, bool play, bool stripDescription,
                          const std::function<QUrl(const QUrl&)>& rewrite = {})
    {
        QByteArray    body;
        const QString error = readJson(endpoint, t.provider, &body);
        if (!error.isEmpty()) {
            if (t.provider == QLatin1String("youtube-extra") && (m_lastStatus == 401 || m_lastStatus == 403 || m_lastStatus == 404))
                return failure(errors::kNothing);
            return fromError(error, m_lastRetry, m_lastSoft);
        }
        content::OEmbedData data;
        if (!content::parseOEmbedData(body, &data, nullptr))
            return failure(errors::kBadResponse);
        Picture picture;
        QUrl    thumb = net::resolveExtracted(endpoint, data.thumbnailUrl);
        if (thumb.isValid() && rewrite)
            thumb = rewrite(thumb);
        if (thumb.isValid())
            pictureFrom({thumb.toString(QUrl::FullyEncoded)}, endpoint, t.provider, false, 1, &picture);
        const QString description = stripDescription ? content::stripTags(data.description) : QString();
        return siteCard(t.provider, data.title, description, data.authorName, picture, layout, play, data.durationMs, false);
    }

    Result planVimeo(const Target& t)
    {
        const QString video = latin("https://vimeo.com/") + t.itemId;
        return planOEmbedCard(t, QUrl(latin("https://vimeo.com/api/oembed.json?url=") + percentEncoded(video) + latin("&width=640")), CardLayout::Large, true,
                              false);
    }

    Result planAppleMusic(const Target& t)
    {
        return planOEmbedCard(t, QUrl(latin("https://music.apple.com/api/oembed?url=") + percentEncoded(t.fetchUrl.toString(QUrl::FullyEncoded))),
                              CardLayout::Thumb, false, false, [](const QUrl& thumb) {
                                  static const QRegularExpression re(QStringLiteral("/\\d+x\\d+bb\\.(jpg|png|webp)$"));
                                  QUrl    out  = thumb;
                                  QString path = thumb.path(QUrl::FullyDecoded);
                                  path.replace(re, QStringLiteral("/600x600bb.jpg"));
                                  out.setPath(path);
                                  return out;
                              });
    }

    Result planYouTubeExtra(const Target& t)
    {
        const QString u = t.sub == QLatin1String("clip") ? t.fetchUrl.toString(QUrl::FullyEncoded) : latin("https://www.youtube.com/watch?v=") + t.itemId;
        return planOEmbedCard(t, QUrl(latin("https://www.youtube.com/oembed?format=json&url=") + percentEncoded(u)), CardLayout::Large, true, false);
    }

    // A card from a page's own meta: og:title, og:description, og:image.
    Result planPageCard(const Target& t, const QUrl& url, CardLayout layout, bool bodyMode = false, bool play = false)
    {
        const Page page = readPage(url, bodyMode, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        const pagemeta::Verdict verdict = pagemeta::wallOf(page.meta, t.page, page.finalUrl, page.challenge);
        if (verdict == pagemeta::Verdict::Walled)
            return failure(errors::kWalled);
        if (verdict == pagemeta::Verdict::Nothing)
            return failure(errors::kNothing);
        Picture picture;
        pictureFrom(pagemeta::pictureCandidates(page.meta), page.finalUrl, t.provider, false, 2, &picture);
        return siteCard(t.provider, pagemeta::titleOf(page.meta), pagemeta::descriptionOf(page.meta), QString(), picture, layout, play,
                        pagemeta::durationMsOf(page.meta), pagemeta::isSensitive(page.meta));
    }

    Result planSpotify(const Target& t)
    {
        return planPageCard(t, QUrl(latin("https://open.spotify.com/") + t.sub + QLatin1Char('/') + t.itemId), CardLayout::Thumb);
    }

    static bool isTwitchShell(const pagemeta::Meta& m)
    {
        return m.ogTitle.trimmed() == QLatin1String("Twitch") || m.ogImages.value(0).contains(QLatin1String("/ttv-static-metadata/"));
    }

    Result planTwitch(const Target& t)
    {
        if (t.sub != QLatin1String("clip"))
            return planPageCard(t, QUrl(latin("https://www.twitch.tv/") + t.itemId), CardLayout::Thumb);
        const QUrl url(latin("https://clips.twitch.tv/") + t.itemId);
        Page       page;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt > 0 && cancel->wait(options.twitchRetryMs))
                return failure(errors::kCanceled);
            page = readPage(url, false, false, t.provider);
            if (!page.error.isEmpty())
                return pageError(page);
            if (!isTwitchShell(page.meta))
                break;
            if (attempt == 2)
                return failure(errors::kNothing);
        }
        QStringList candidates = pagemeta::pictureCandidates(page.meta);
        for (QString& c : candidates)
            c.replace(QLatin1String("-1920x1080.jpg"), QLatin1String("-480x272.jpg"));
        Picture picture;
        pictureFrom(candidates, page.finalUrl, t.provider, false, 2, &picture);
        return siteCard(t.provider, pagemeta::titleOf(page.meta), pagemeta::descriptionOf(page.meta), QString(), picture, CardLayout::Large, true,
                        pagemeta::durationMsOf(page.meta), pagemeta::isSensitive(page.meta));
    }

    Result planKick(const Target& t)
    {
        if (t.sub == QLatin1String("clip"))
            return planPageCard(t, t.fetchUrl, CardLayout::Large, true, true);
        return planPageCard(t, t.fetchUrl, CardLayout::Thumb);
    }

    Result planBluesky(const Target& t)
    {
        const Page page = readPage(t.fetchUrl, false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        const pagemeta::Meta& m = page.meta;
        if (textclean::clean(m.ogDescription, 400)
            == QLatin1String("This author has chosen to make their posts visible only to people who are signed in."))
            return failure(errors::kNothing);
        QStringList candidates;
        bool        post = false;
        for (const QString& raw : m.ogImages) {
            if (raw.contains(QLatin1String("/feed_thumbnail/"))) {
                QString full = raw;
                candidates << full.replace(QLatin1String("/feed_thumbnail/"), QLatin1String("/feed_fullsize/"));
                post = true;
            }
            candidates << raw;
        }
        const bool video = !m.ogVideos.isEmpty();
        Picture    picture;
        pictureFrom(candidates, page.finalUrl, t.provider, false, 2, &picture);
        const CardLayout layout = picture.file.isEmpty() ? CardLayout::None : (post || video) ? CardLayout::Large : CardLayout::Thumb;
        return siteCard(t.provider, pagemeta::titleOf(m), pagemeta::descriptionOf(m), QString(), picture, layout, video, 0, pagemeta::isSensitive(m));
    }

    Result planX(const Target& t)
    {
        const QString user = t.itemId.section(QLatin1Char('/'), 0, 0);
        const QString post = t.itemId.section(QLatin1Char('/'), 1, 1);
        const Page    page = readPage(QUrl(latin("https://x.com/") + user + latin("/status/") + post), false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        if (page.meta.ogTitle.trimmed().isEmpty())
            return failure(errors::kNothing);
        Picture picture;
        pictureFrom(pagemeta::pictureCandidates(page.meta), page.finalUrl, t.provider, false, 2, &picture);
        const CardLayout layout = pagemeta::layoutFor(page.meta.twitterCard, !picture.file.isEmpty(), picture.width, picture.height);
        return siteCard(t.provider, pagemeta::titleOf(page.meta), pagemeta::descriptionOf(page.meta), QString(), picture, layout, false, 0,
                        pagemeta::isSensitive(page.meta));
    }

    Result planSteam(const Target& t)
    {
        if (t.sub == QLatin1String("short")) {
            Target        next;
            const QString error = followRedirect(t.fetchUrl, t.provider, &next);
            if (!error.isEmpty())
                return fromError(error, m_lastRetry, m_lastSoft);
            if (publicHostName(next.page) != QLatin1String("store.steampowered.com"))
                return failure(errors::kNothing);
            return planSteam(next);
        }
        const Page page = readPage(t.fetchUrl, false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        if (page.finalUrl.path(QUrl::FullyDecoded).contains(QLatin1String("/agecheck/")))
            return failure(errors::kNothing);
        const pagemeta::Meta& m = page.meta;
        if (t.sub == QLatin1String("file") && m.ogTitle.startsWith(QLatin1String("Steam Community :: Screenshot"))) {
            QUrl image = net::resolveExtracted(page.finalUrl, m.ogImages.value(0));
            if (!image.isValid())
                return failure(errors::kNothing);
            image.setQuery(QString());
            return pageMedia(loadMedia(image, Kind::Image, t.provider, false, false), m);
        }
        Picture picture;
        pictureFrom(pagemeta::pictureCandidates(m), page.finalUrl, t.provider, false, 2, &picture);
        const CardLayout layout = t.sub == QLatin1String("page") ? CardLayout::Thumb : CardLayout::Large;
        return siteCard(t.provider, pagemeta::titleOf(m), pagemeta::descriptionOf(m), QString(), picture, layout, false, 0, pagemeta::isSensitive(m));
    }

    Result planWikipedia(const Target& t)
    {
        QString lang = t.sub;
        if (lang.endsWith(QLatin1String(".m")))
            lang.chop(2);
        // The title re-encoded: every byte but the unreserved ones, "/" as %2F.
        const QString title = QString::fromLatin1(QUrl::toPercentEncoding(t.itemId));
        QByteArray    body;
        const QString error = readJson(QUrl(latin("https://") + lang + latin(".wikipedia.org/api/rest_v1/page/summary/") + title, QUrl::StrictMode), t.provider, &body);
        if (!error.isEmpty()) {
            if (error == QLatin1String(errors::kNotFound))
                return failure(errors::kNothing);
            return fromError(error, m_lastRetry, m_lastSoft);
        }
        content::WikiSummary summary;
        if (!content::parseWikiSummary(body, &summary, nullptr))
            return failure(errors::kBadResponse);
        Picture picture;
        if (!summary.thumbnail.isEmpty())
            pictureFrom({summary.thumbnail}, QUrl(latin("https://") + lang + latin(".wikipedia.org/")), t.provider, false, 1, &picture);
        return siteCard(t.provider, summary.title, summary.extract, QString(), picture, bySize(picture), false, 0, false);
    }

    Result planTelegram(const Target& t)
    {
        const Page page = readPage(QUrl(latin("https://t.me/") + t.itemId), false, false, t.provider);
        if (!page.error.isEmpty())
            return pageError(page);
        Picture picture;
        pictureFrom(pagemeta::pictureCandidates(page.meta), page.finalUrl, t.provider, false, 2, &picture);
        return siteCard(t.provider, pagemeta::titleOf(page.meta), pagemeta::descriptionOf(page.meta), QString(), picture, bySize(picture), false, 0,
                        pagemeta::isSensitive(page.meta));
    }

    Result planDiscord(const Target& t)
    {
        QByteArray    body;
        const QString error = readJson(QUrl(latin("https://discord.com/api/v10/invites/") + t.itemId + latin("?with_counts=true")), t.provider, &body);
        if (!error.isEmpty()) {
            if (error == QLatin1String(errors::kNotFound))
                return failure(errors::kNothing); // an expired invite
            return fromError(error, m_lastRetry, m_lastSoft);
        }
        content::DiscordInvite invite;
        if (!content::parseDiscordInvite(body, &invite, nullptr) || invite.guildId.isEmpty())
            return failure(errors::kNothing);
        Picture    picture;
        CardLayout layout = CardLayout::None;
        if (!invite.splash.isEmpty()) {
            pictureFrom({latin("https://cdn.discordapp.com/splashes/") + invite.guildId + QLatin1Char('/') + invite.splash + latin(".jpg?size=512")},
                        QUrl(latin("https://discord.com/")), t.provider, false, 1, &picture);
            layout = CardLayout::Large;
        }
        if (picture.file.isEmpty() && !invite.icon.isEmpty()) {
            pictureFrom({latin("https://cdn.discordapp.com/icons/") + invite.guildId + QLatin1Char('/') + invite.icon + latin(".png?size=256")},
                        QUrl(latin("https://discord.com/")), t.provider, false, 1, &picture);
            layout = CardLayout::Thumb;
        }
        QString author;
        if (invite.members >= 0) {
            const QLocale english(QLocale::English);
            author = english.toString(invite.members) + latin(" members");
            if (invite.online >= 0)
                author += QString::fromUtf8(" \xC2\xB7 ") + english.toString(invite.online) + latin(" online");
        }
        return siteCard(t.provider, invite.name, invite.description, author, picture, layout, false, 0, invite.nsfw);
    }

    // The hop list a Reddit post's Direct file link is loaded under: Reddit's own media hosts, or the media hosts of a
    // media site of the table (imgur, giphy, tenor, ... : the nested row's lists, 2.6.1). Empty: not loaded.
    static QString redditDirectHops(const QUrl& url)
    {
        for (const char* id : {"reddit", "imgur", "giphy", "tenor", "imgflip", "gifer"}) {
            if (providers::mediaHopAllowed(latin(id), url))
                return latin(id);
        }
        return {};
    }

    Result planReddit(const Target& t)
    {
        QByteArray    body;
        const QString error =
            readXml(QUrl(latin("https://www.reddit.com/comments/") + t.itemId + latin("/.rss?limit=1")), t.provider, net::kMaxFeedBytes, false, &body);
        if (!error.isEmpty())
            return fromError(error, m_lastRetry, m_lastSoft);
        const pagemeta::FeedEntry entry = pagemeta::readFeed(body);
        if (!entry.found)
            return failure(errors::kNothing);
        // The post's own link: media through its own rule.
        const QString postLink = pagemeta::linkOfContent(entry.content);
        if (!postLink.isEmpty()) {
            const Target inner = classify(net::resolveExtracted(QUrl(latin("https://www.reddit.com/")), postLink));
            Result       media;
            bool         tried = false;
            const QString directHops = inner.cls == LinkClass::Direct && inner.provider.isEmpty() ? redditDirectHops(inner.url) : QString();
            if (!directHops.isEmpty()) {
                // A file on Reddit's own media hosts (or a known media site's, i.imgur.com): never a host the post's
                // author picked, which would learn every viewer's address. Any other file: the post's card.
                tried = true;
                media = loadMedia(inner.url, inner.kind, directHops, false, false);
            } else if (inner.cls == LinkClass::MediaPage && inner.provider != QLatin1String("media-file")) {
                tried = true;
                media = runProvider(inner);
            }
            if (tried && media.ok && media.kind != Kind::Card)
                return media;
            if (tried && (media.error == QLatin1String(errors::kCanceled) || media.error == QLatin1String(errors::kRateLimited)
                          || media.error == QLatin1String(errors::kGateBusy) || media.error == QLatin1String(errors::kWalled)))
                return media;
        }
        QString author = entry.category;
        QString name   = entry.author;
        if (name.startsWith(QLatin1Char('/')))
            name.remove(0, 1);
        if (!name.isEmpty())
            author = author.isEmpty() ? name : author + QString::fromUtf8(" \xC2\xB7 ") + name;
        Picture picture;
        if (!entry.thumbnail.isEmpty())
            pictureFrom({entry.thumbnail}, QUrl(latin("https://www.reddit.com/")), t.provider, false, 1, &picture);
        // A text post: its own text (Reddit's markdown block of the content, tags stripped) is the description.
        QString   description;
        const int md = entry.content.indexOf(QLatin1String("<div class=\"md\">"));
        if (md >= 0) {
            const int end = entry.content.indexOf(QLatin1String("<!-- SC_ON -->"), md);
            description   = content::stripTags(entry.content.mid(md, end < 0 ? 8192 : end - md).left(8192));
        }
        return siteCard(t.provider, entry.title, description, author, picture, bySize(picture), false, 0, false);
    }

    // ---- Nextcloud and ownCloud shares (design2 2.3-2.8) ------------------------------------------------------------

    // The most a video of this load may download (a share's video: Caps::video, else Caps::media).
    qint64 videoCap() const { return caps.video >= 0 ? caps.video : caps.media; }

    // Every address is built from the link (shares.h, N3) and every hop stays on the share's host (the "=" lists).
    Result planShare(const Target& t)
    {
        const shares::ShareLink shareLink = shares::parse(t.page);
        if (!shareLink.isValid() || shareLink.host != t.host)
            return failure(errors::kNotPreviewable);
        m_boundHost = shareLink.host;
        switch (shareLink.form) {
        case shares::Form::Page:
        case shares::Form::Download:
            return sharePage(shareLink);
        case shares::Form::Preview:
        case shares::Form::PublicPreview:
            // One media request for the link itself (without tracking parameters), a picture.
            return loadMedia(t.fetchUrl, Kind::Image, t.provider, false, false);
        case shares::Form::Dav:
            return shareDav(t);
        case shares::Form::None:
            break;
        }
        return failure(errors::kNotPreviewable);
    }

    // A DAV link of the shared file: a picture, a GIF or a video by what it answers (a folder's zip: kBadType).
    Result shareDav(const Target& t)
    {
        const QString part = base() + QStringLiteral(".%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return failure(errors::kDisk);
        const qint64 pictures = caps.media;
        const qint64 videos   = videoCap();
        GetSpec      spec;
        spec.url         = t.fetchUrl;
        spec.accept      = "image/png,image/jpeg,image/webp,image/gif,video/mp4,video/webm,video/quicktime;q=0.9";
        spec.maxBytes    = qMin(pictures, videos);
        spec.lane        = HostGate::Lane::Media;
        const QString bound = m_boundHost;
        spec.hopAllowed  = [bound](const QUrl& hop) { return providers::mediaHopAllowed(latin("nextcloud"), hop, bound); };
        spec.typeAllowed = [](const QByteArray& type) {
            return content::contentTypeAllowed(Kind::Image, type) || content::contentTypeAllowed(Kind::Video, type);
        };
        spec.headAllowed = [](const QByteArray& head) {
            const content::Format format = content::sniff(head);
            return content::fits(Kind::Image, format) || content::fits(Kind::Video, format);
        };
        spec.maxFor = [pictures, videos](const QByteArray& type) {
            if (type.startsWith("video/") || type == "application/mp4")
                return videos;
            if (type.startsWith("image/"))
                return pictures;
            return qMin(pictures, videos); // a generic type: the smaller cap (its first bytes decide its kind)
        };
        spec.file     = &file;
        spec.progress = true;
        if (pictures <= 0 && videos <= 0) {
            file.close();
            QFile::remove(part);
            return failure(errors::kTooLarge);
        }
        const Got got = get(spec);
        file.close();
        if (!got.error.isEmpty()) {
            QFile::remove(part);
            Result result       = failure(got.error);
            result.retryAfterMs = got.retryAfterMs;
            result.softCooling  = got.softCooling;
            if (got.error == QLatin1String(errors::kTooLarge)) {
                result.size = got.total;
                result.kind = content::mediaType(got.contentType).startsWith("video/") ? Kind::Video : Kind::Image;
            }
            return result;
        }
        return finishMedia(part, content::sniff(got.head), got.received, true);
    }

    struct SharePage {
        QString    error;
        QUrl       finalUrl;
        QUrl       away; // a redirect to another host: the Web class from there (2.3)
        QByteArray body, contentType;
        bool       truncated = false, challenge = false;
    };

    // The share page: body mode up to caps.page, early stop once the markers are complete (or the tokenizer's tag
    // limit), same-host redirects only (a hop to another host ends the plan).
    SharePage readSharePage(const QUrl& url, const QString& host)
    {
        SharePage page;
        GetSpec   spec;
        spec.url        = url;
        spec.accept     = "text/html,application/xhtml+xml;q=0.9,*/*;q=0.1";
        spec.maxBytes   = qMin(qMax<qint64>(16 * 1024, caps.page), net::kMaxPageBytes);
        spec.lane       = HostGate::Lane::Page;
        spec.text       = true;
        spec.truncateOk = true;
        spec.pageClock  = true;
        spec.hopAllowed = [host](const QUrl& hop) { return providers::pageHopAllowed(latin("nextcloud"), hop, host); };
        spec.stopAt     = [host](const QUrl& location) { return publicHostName(location) != host; };
        spec.enough     = [](const QByteArray& soFar) {
            const shares::Markers m = shares::readMarkers(soFar, nullptr);
            return m.tagLimit || shares::markersComplete(m);
        };
        spec.decide = [](const QByteArray& type, Kind*) { return isHtmlType(type) || isGenericType(type) ? HeadAction::Text : HeadAction::Fail; };
        const Got got  = get(spec);
        page.challenge = got.challenge;
        page.finalUrl  = got.finalUrl.isValid() ? got.finalUrl : url;
        if (got.redirectStop) {
            page.away = got.finalUrl;
            return page;
        }
        if (!got.error.isEmpty()) {
            page.error   = got.error;
            m_lastStatus = got.status;
            m_lastRetry  = got.retryAfterMs;
            m_lastSoft   = got.softCooling;
            return page;
        }
        page.body        = got.body;
        page.contentType = got.contentType;
        page.truncated   = got.truncated;
        return page;
    }

    // A share link that turned out to be another web page while cards for other web pages are off (Caps::webCards):
    // no further request. Never remembered (isCachedNothing), so the switch turned on again loads it at once. away: the
    // other host a redirect named; its host is the result's finalHost when it is another website (the sender then says
    // r=x, as for a Web card from another website, D18).
    Result webOff(const QUrl& away)
    {
        Result result = failure(errors::kWebOff);
        const QString host = away.isValid() ? publicHostName(away) : QString();
        if (!host.isEmpty() && net::registrableDomain(host) != net::registrableDomain(target.host))
            result.finalHost = host;
        return result;
    }

    Result sharePage(const shares::ShareLink& shareLink)
    {
        const SharePage page = readSharePage(shares::pageUrl(shareLink), shareLink.host);
        if (page.away.isValid()) {
            // Another host: the share plan ends; the link is a Web link from that hop on (its card, its footer, Show
            // cards for other web pages), never a share, and nothing of the share is requested.
            m_boundHost.clear();
            if (!caps.webCards)
                return webOff(page.away); // cards for other web pages are off: that page is never loaded
            Result web = runWebFrom(page.away, true);
            // A Web link shows nothing when its page is gone or not previewable (the quiet class, design 3.3), never a
            // share's "file isn't on the website any more" strip (live pass: a dead shortener target showed one). These
            // answers are the site's own, so "nothing" (remembered, r=n for receivers); busy and network failures keep
            // their own handling.
            if (!web.ok && quietAfterHandover(web.error)) {
                const QString finalHost = web.finalHost;
                web                     = failure(errors::kNothing);
                web.finalHost           = finalHost;
            }
            return web;
        }
        if (!page.error.isEmpty()) {
            if (page.challenge || page.error == QLatin1String(errors::kWalled))
                return failure(errors::kWalled);
            return fromError(page.error, m_lastRetry, m_lastSoft);
        }
        const pagemeta::Decoder decoder = pagemeta::decoderFor(page.body, page.contentType);
        const QByteArray        html    = pagemeta::prepared(page.body, decoder);
        shares::PageFacts       facts;
        facts.finalUrl                 = page.finalUrl;
        facts.link                     = shareLink;
        facts.markers                  = shares::readMarkers(html, decoder.codec);
        facts.videoCap                 = videoCap();
        const shares::Decision  d      = shares::decide(facts);
        const shares::ShareLink share  = shares::baseAfter(shareLink, page.finalUrl);
        const pagemeta::Meta    meta   = pagemeta::scanPage(html, decoder.codec, false, page.truncated);
        switch (d.action) {
        case shares::Action::NotFound:
            return failure(errors::kNotFound);
        case shares::Action::Nothing:
            return failure(errors::kNothing);
        case shares::Action::Unconfirmed:
            // Not proven a share: the generic web card rules on the bytes already read (its picture under the Web
            // picture rules, never the share's lists). With cards for other web pages off: nothing more (no picture
            // the page names is requested).
            m_boundHost.clear();
            if (!caps.webCards)
                return webOff(QUrl());
            return webCard(meta, page.finalUrl, page.challenge);
        case shares::Action::Card:
            return shareCard(d, share, facts.markers, meta, page.finalUrl);
        case shares::Action::Picture:
            return sharePicture(d, share, facts.markers);
        case shares::Action::Gif:
            return shareDownload(d, share, Kind::Gif, caps.media);
        case shares::Action::Video:
            return shareDownload(d, share, Kind::Video, videoCap());
        case shares::Action::TooLarge: {
            Result tooLarge = failure(errors::kTooLarge);
            tooLarge.size   = d.size;
            tooLarge.kind   = Kind::Video;
            return tooLarge; // no media request, no thumbnail
        }
        }
        return failure(errors::kNothing);
    }

    // The file itself through /download (GIFs, videos, a picture whose previews failed): over the cap by the size the
    // page said, no request; else same-host hops to the download or the DAV address only (shares.h).
    Result shareDownload(const shares::Decision& d, const shares::ShareLink& share, Kind kind, qint64 cap)
    {
        if (cap <= 0 || (d.size >= 0 && d.size > cap)) {
            Result tooLarge = failure(errors::kTooLarge);
            tooLarge.size   = d.size;
            tooLarge.kind   = kind;
            return tooLarge;
        }
        const auto hop = [share](const QUrl& url) { return shares::downloadHopAllowed(share, url); };
        return loadMedia(shares::downloadUrl(share), kind, latin("nextcloud"), kind == Kind::Video, false, hop, cap);
    }

    // A picture through the server's preview (2048 px, always JPEG or PNG, no "downloaded" activity for the owner):
    // Nextcloud's publicpreview, then /s/<token>/preview; ownCloud's publicpreview with the file name. Then, for a JPEG,
    // PNG or WebP within the cap, the file itself.
    Result sharePicture(const shares::Decision& d, const shares::ShareLink& share, const shares::Markers& markers)
    {
        Result     result = failure(errors::kNothing);
        const QUrl first  = d.ownCloud ? shares::ownCloudPreviewUrl(share, d.name, shares::kPictureSide) : shares::publicPreviewUrl(share, shares::kPictureSide);
        if (first.isValid()) {
            result = loadMedia(first, Kind::Image, latin("nextcloud"), false, false);
            if (result.ok || !isFallbackError(result))
                return result;
        }
        if (!d.ownCloud) {
            result = loadMedia(shares::previewUrl(share), Kind::Image, latin("nextcloud"), false, false);
            if (result.ok || !isFallbackError(result))
                return result;
        }
        if (shares::isWebPicture(markers))
            return shareDownload(d, share, Kind::Image, caps.media);
        return result;
    }

    // A share card (2.7): the name, "Type · size" (· View only), the page's theme colour under the Web rules, and the
    // server's preview as its picture when the page shows one (og:image a same-host preview address) and no file may
    // be read for it otherwise (a download limit, an incomplete read: no picture).
    Result shareCard(const shares::Decision& d, const shares::ShareLink& share, const shares::Markers& markers, const pagemeta::Meta& meta,
                     const QUrl& finalUrl)
    {
        CardData card;
        card.title = textclean::clean(d.name, kCardTitleChars);
        if (card.title.isEmpty())
            return failure(errors::kNothing);
        card.description = textclean::clean(shares::cardDescription(d.type, d.size, d.viewOnly), kCardDescriptionChars);
        QRgb colour      = 0;
        if (pagemeta::themeColourOf(meta, &colour)) {
            card.color    = colour;
            card.hasColor = true;
        }
        Picture picture;
        if (!d.noFiles) {
            bool previewable = false;
            for (const QString& raw : markers.ogImages)
                previewable = previewable || shares::isPreviewAddress(net::resolveExtracted(finalUrl, raw), share);
            const QUrl url = !previewable ? QUrl()
                             : d.ownCloud ? shares::ownCloudPreviewUrl(share, d.name, shares::kCardPictureSide)
                                          : shares::publicPreviewUrl(share, shares::kCardPictureSide);
            if (url.isValid()) {
                const QString error = loadPicture(url, latin("nextcloud"), &picture);
                if (error == QLatin1String(errors::kCanceled) || error == QLatin1String(errors::kRateLimited)
                    || error == QLatin1String(errors::kGateBusy) || error == QLatin1String(errors::kWalled))
                    return fromError(error, m_lastPictureRetry, m_lastPictureSoft); // tried again later, with its picture
            }
        }
        if (!picture.file.isEmpty()) {
            card.pictureFile   = picture.file;
            card.pictureWidth  = picture.width;
            card.pictureHeight = picture.height;
            card.layout        = d.type == shares::FileType::Picture ? CardLayout::Large : CardLayout::Thumb;
        }
        Result result;
        result.ok            = true;
        result.kind          = Kind::Card;
        result.card          = card;
        result.thumbnailFile = card.pictureFile;
        result.mime          = picture.mime;
        result.width         = picture.width;
        result.height        = picture.height;
        result.size          = picture.bytes;
        return result;
    }

    // ---- the Web class -----------------------------------------------------------------------------------------

    Result runWeb()
    {
        return runWebFrom(target.fetchUrl, false);
    }

    // The Web plan from start: the link itself, or (handover) the first address of another host a share page's
    // redirect named (design2 2.3: from that hop on exactly as a Web link).
    Result runWebFrom(const QUrl& start, bool handover)
    {
        const QString part = base() + QStringLiteral(".%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return failure(errors::kDisk);
        const Page page = readPage(start, false, true, QString(), &file);
        file.close();
        if (page.media.switched) {
            // The page answered with media: that media (Image / Gif / Video by bytes; never Loop), under the media cap.
            if (!page.media.error.isEmpty()) {
                QFile::remove(part);
                Result result = failure(page.media.error);
                if (page.media.error == QLatin1String(errors::kTooLarge)) {
                    result.size = page.media.total;
                    result.kind = page.media.switchedKind;
                }
                return result;
            }
            return finishMedia(part, content::sniff(page.media.head), page.media.received, false);
        }
        QFile::remove(part);
        if (handover && page.error == QLatin1String(errors::kNotPreviewable))
            return failure(errors::kRedirect); // a redirect to an address the Web class refuses
        if (!page.error.isEmpty())
            return pageError(page);
        return webCard(page.meta, page.finalUrl, page.challenge);
    }

    // A Web card from a page's meta (the Web class; a share page that isn't proven a share: design2 2.4).
    Result webCard(const pagemeta::Meta& m, const QUrl& finalUrl, bool challenge)
    {
        const pagemeta::Verdict verdict = pagemeta::wallOf(m, target.page, finalUrl, challenge);
        if (verdict == pagemeta::Verdict::Walled)
            return failure(errors::kWalled);
        if (verdict == pagemeta::Verdict::Nothing)
            return failure(errors::kNothing);
        const QString title = pagemeta::titleOf(m);
        if (title.isEmpty())
            return failure(errors::kNothing);
        Picture picture;
        pictureFrom(pagemeta::pictureCandidates(m), finalUrl, QString(), true, 2, &picture);
        if (!m_pictureStop.isEmpty())
            return fromError(m_pictureStop, m_pictureStopRetry, m_pictureStopSoft);

        CardData card;
        card.title       = title;
        card.description = pagemeta::descriptionOf(m);
        const QString finalHost = publicHostName(finalUrl);
        Result        result;
        if (!finalHost.isEmpty() && net::registrableDomain(finalHost) != net::registrableDomain(target.host))
            result.finalHost = finalHost;
        // A page on another website than the link's (a redirect, which may answer this plugin differently than a
        // browser): never that site's own name or colour, so its card can't pass for the link's (review fix of D18).
        const bool    crossSite = !result.finalHost.isEmpty();
        const QString site      = pagemeta::siteNameOf(m);
        if (!crossSite && providers::siteNameMatches(site, target.host))
            card.site = site;
        QRgb colour = 0;
        if (!crossSite && pagemeta::themeColourOf(m, &colour)) {
            card.color    = colour;
            card.hasColor = true;
        }
        card.sensitive = pagemeta::isSensitive(m);
        card.play      = !m.ogVideos.isEmpty() || m.ogType.startsWith(QLatin1String("video"));
        card.durationMs = card.play ? pagemeta::durationMsOf(m) : 0;
        if (!picture.file.isEmpty()) {
            card.pictureFile   = picture.file;
            card.pictureWidth  = picture.width;
            card.pictureHeight = picture.height;
            card.layout        = pagemeta::layoutFor(m.twitterCard, true, picture.width, picture.height);
        }
        if (card.description.isEmpty() && card.pictureFile.isEmpty())
            return failure(errors::kNothing); // a title alone is no card
        result.ok            = true;
        result.kind          = Kind::Card;
        result.card          = card;
        result.thumbnailFile = card.pictureFile;
        result.mime          = picture.mime;
        result.width         = picture.width;
        result.height        = picture.height;
        result.size          = picture.bytes;
        return result;
    }

    // ---- YouTube (2.4) ---------------------------------------------------------------------------------------------

    Result loadYouTube()
    {
        const QString videoId = target.videoId;
        GetSpec       json;
        json.url         = net::oEmbedUrl(videoId);
        json.accept      = "application/json";
        json.maxBytes    = net::kMaxOEmbedBytes;
        json.lane        = HostGate::Lane::Page;
        json.hopAllowed  = [](const QUrl& url) { return net::isOEmbedHop(url); };
        json.typeAllowed = [](const QByteArray& type) { return content::isJsonType(type); };
        json.headAllowed = [](const QByteArray& head) { return head.trimmed().startsWith('{'); };
        const Got meta   = get(json);

        content::OEmbed embed;
        bool            haveMeta = false;
        QUrl            thumbUrl = net::defaultThumbnail(videoId);
        if (meta.error.isEmpty()) {
            QString why;
            if (!content::parseOEmbed(meta.body, videoId, &embed, &why))
                return failure(errors::kBadResponse);
            haveMeta = true;
            thumbUrl = embed.thumbnail;
        } else if (meta.error == QLatin1String(errors::kNotFound)) {
            return failure(errors::kUnavailable); // no such video, or a private one
        } else if (meta.error != QLatin1String(errors::kForbidden)) {
            return fromError(meta.error, meta.retryAfterMs, meta.softCooling);
        }
        // 401 / 403: oEmbed is closed for this video (embedding turned off), the thumbnail still shows it.
        if (cancel->isSet())
            return failure(errors::kCanceled);

        const QString part = base() + QStringLiteral("-thumb.%1.part").arg(id);
        Result        result;
        Got           picture;
        bool          diskError = false;
        // One thumbnail request: the picture is taken when it is at least minWidth px wide.
        const auto fetchThumb = [&](const QUrl& url, int minWidth) {
            QFile file(part);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                diskError = true;
                return false;
            }
            GetSpec thumb;
            thumb.url         = url;
            thumb.accept      = acceptFor(Kind::YouTube);
            thumb.maxBytes    = qMin(caps.media, net::kMaxThumbnailBytes);
            thumb.lane        = HostGate::Lane::Media;
            thumb.hopAllowed  = [](const QUrl& hop) { return net::isThumbnailHop(hop); };
            thumb.typeAllowed = [](const QByteArray& type) { return content::contentTypeAllowed(Kind::YouTube, type); };
            thumb.headAllowed = [](const QByteArray& head) { return content::fits(Kind::YouTube, content::sniff(head)); };
            thumb.file        = &file;
            thumb.progress    = true;
            picture           = get(thumb);
            file.close();
            bool took = false;
            if (picture.error.isEmpty()) {
                const content::Format format = content::sniff(picture.head);
                const QString         path   = base() + latin("-thumb.") + latin(content::extensionOf(format));
                if (replaceFile(part, path)) {
                    const QSize size = webpAllowed(path) ? pictureSize(path) : QSize();
                    if (size.isValid() && !size.isEmpty() && size.width() >= minWidth
                        && static_cast<qint64>(size.width()) * size.height() <= options.maxMediaPixels) {
                        result.thumbnailFile = path;
                        result.mime          = latin(content::mimeOf(format));
                        result.size          = picture.received;
                        result.width         = size.width();
                        result.height        = size.height();
                        took                 = true;
                    } else {
                        QFile::remove(path);
                    }
                }
            }
            QFile::remove(part);
            return took;
        };
        // 2.5 zoom (design2 1.7): maxresdefault first (1280 x 720, sharp at 2x); a 404, another failure or YouTube's
        // small placeholder: the standard thumbnail (oEmbed's, hqdefault). A busy answer (Retry-After) stops there.
        const QUrl maxres = net::maxresThumbnail(videoId);
        bool       took   = maxres.isValid() && maxres != thumbUrl && fetchThumb(maxres, net::kMaxresMinWidth);
        if (!took && !diskError && !cancel->isSet() && picture.retryAfterMs <= 0)
            took = fetchThumb(thumbUrl, 0);
        Q_UNUSED(took);
        if (diskError)
            return failure(errors::kDisk);
        if (cancel->isSet())
            return failure(errors::kCanceled);
        if (!haveMeta && result.thumbnailFile.isEmpty())
            return picture.error == QLatin1String(errors::kNotFound) || picture.error.isEmpty() ? failure(errors::kUnavailable)
                                                                                                 : fromError(picture.error, picture.retryAfterMs, picture.softCooling);
        result.ok     = true;
        result.kind   = Kind::YouTube;
        result.title  = embed.title;
        result.author = embed.author;
        if (haveMeta && !result.thumbnailFile.isEmpty())
            writeMetaV1(target, cacheDir, result, id); // a partial answer is fetched again next time
        return result;
    }

    quint64 m_deadline         = 0;
    quint64 m_lastProgress     = 0;
    bool    m_viaProxy         = false; // a request of this load went through the Windows proxy
    bool    m_firstUsed        = false;
    int     m_gateWaited       = 0;
    int     m_requests         = 0;
    int     m_lastStatus       = 0;
    qint64  m_lastRetry        = 0;
    bool    m_lastSoft         = false;
    qint64  m_lastPictureRetry = 0;
    bool    m_lastPictureSoft  = false;
    QString m_pictureStop;      // a picture's site said "later" (or the load was canceled): no card now
    qint64  m_pictureStopRetry = 0;
    bool    m_pictureStopSoft  = false;
    QString m_boundHost;        // a share plan: the share's host ("=" hop lists, providers.h)

  public:
    QString m_hopRefused; // the host a hop list refused (the log says it)
};

} // namespace detail

namespace {

DWORD WINAPI jobThread(void* parameter)
{
    auto*   job    = static_cast<detail::FetchJob*>(parameter);
    HMODULE module = job->module;
    job->run();
    delete job;
    if (http::mustStayLoaded())
        ExitThread(0); // the system may still call into the DLL: its pin stays
    // Drops the pin and ends the thread without returning into a DLL that may be unloaded now.
    FreeLibraryAndExitThread(module, 0);
}

HANDLE launch(detail::FetchJob* job)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&jobThread), &module)) {
        delete job;
        return nullptr;
    }
    job->module   = module;
    HANDLE thread = CreateThread(nullptr, 0, &jobThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        FreeLibrary(module);
        return nullptr;
    }
    return thread;
}

QString outcomeText(const Result& result, qint64 ms);

QString outcomeWithHop(const Result& result, qint64 ms)
{
    const QString text = outcomeText(result, ms);
    return result.hopRefused.isEmpty() ? text : text + QStringLiteral(" (hop refused %1)").arg(result.hopRefused);
}

QString outcomeText(const Result& result, qint64 ms)
{
    if (result.ok) {
        if (result.kind == Kind::Card)
            return QStringLiteral("card, %1 requests, %2 ms").arg(result.requests).arg(ms);
        return QStringLiteral("loaded %1, %2 KB, %3 requests in %4 ms")
            .arg(latin(kindName(result.kind)))
            .arg((qMax<qint64>(0, result.size) + 1023) / 1024)
            .arg(result.requests)
            .arg(ms);
    }
    if (result.error == QLatin1String(errors::kWalled) || result.error == QLatin1String(errors::kNothing))
        return QStringLiteral("nothing (%1)").arg(result.error);
    if (result.error == QLatin1String(errors::kRateLimited))
        return QStringLiteral("%1, wait %2 s").arg(latin(result.softCooling ? "site busy (soft)" : "rate limited")).arg((result.retryAfterMs + 999) / 1000);
    if (result.error == QLatin1String(errors::kGateBusy))
        return QStringLiteral("gate busy");
    return QStringLiteral("%1 after %2 ms").arg(result.error).arg(ms);
}

} // namespace

// ---- Fetcher (GUI thread) --------------------------------------------------------------------------------

Fetcher::Fetcher(QObject* parent)
    : Fetcher(http::winHttpTransport(), Options(), parent)
{
}

Fetcher::Fetcher(std::shared_ptr<http::Transport> transport, const Options& options, QObject* parent)
    : QObject(parent)
    , m_transport(std::move(transport))
    , m_options(options)
    , m_link(std::make_shared<Link>())
{
    m_link->receiver = this;
    // 2.4 auto-load: the sender pipeline's Fetcher and the chat's write into the same folder (cache/links), maybe the
    // same link at the same moment. Their part files ("<key>.<id>.part") must never be one file: each Fetcher
    // numbers its jobs from its own random start (ids are only compared within one Fetcher).
    m_nextId = static_cast<quint64>(QRandomGenerator::global()->generate()) << 24;
    if (!m_options.gate)
        m_options.gate = std::make_shared<HostGate>(); // tests and tools: a private gate
    m_gateTimer = new QTimer(this);
    m_gateTimer->setSingleShot(true);
    connect(m_gateTimer, &QTimer::timeout, this, &Fetcher::startQueued);
}

Fetcher::~Fetcher()
{
    shutdown();
    QMutexLocker lock(&m_link->mutex);
    m_link->receiver = nullptr;
}

void Fetcher::load(const Target& target, qint64 maxBytes, const QString& cacheDir)
{
    Caps caps;
    caps.media   = maxBytes <= 0 ? kDefaultMaxBytes : qMin(maxBytes, kHardMaxBytes);
    caps.picture = qMin(caps.media, net::kMaxThumbnailBytes);
    load(target, caps, cacheDir);
}

void Fetcher::load(const Target& target, const Caps& caps, const QString& cacheDir)
{
    const QString& key = target.key;
    if (!m_enabled) {
        finishLater(key, failure(errors::kDisabled));
        return;
    }
    Pending pending;
    pending.target = target;
    if (pending.target.cls == LinkClass::None && target.isValid()) // a 2.4 caller's Target made by hand: its class by its kind
        pending.target.cls = target.kind == Kind::YouTube ? LinkClass::YouTube : LinkClass::Direct;
    if (!target.isValid() || !isKey(key) || !net::isAllowedUrl(target.url) || cacheDir.isEmpty()) {
        finishLater(key, failure(errors::kNotPreviewable));
        return;
    }
    if (isLoading(key))
        return; // one fetch per link: the running one answers
    if (pending.target.cls == LinkClass::Direct && net::expiredSignedLink(target.url, m_options.nowSec ? m_options.nowSec() : QDateTime::currentSecsSinceEpoch())) {
        finishLater(key, failure(errors::kExpired)); // a signed link whose time is over: no request
        return;
    }
    pending.caps          = caps;
    pending.caps.media    = qBound<qint64>(0, caps.media, kHardMaxBytes);
    pending.caps.video    = caps.video < 0 ? -1 : qMin(caps.video, kHardMaxBytes);
    pending.caps.picture  = qBound<qint64>(0, caps.picture, net::kMaxThumbnailBytes);
    pending.caps.page     = qBound<qint64>(16 * 1024, caps.page, net::kMaxPageBytes);
    pending.cacheDir      = QDir::cleanPath(cacheDir);
    if (m_running.size() >= qMax(1, m_options.maxConcurrent) || !m_queue.isEmpty()) {
        if (m_queue.size() >= m_options.maxQueued) {
            finishLater(key, failure(errors::kBusy));
            return;
        }
        m_queue.append(pending);
        startQueued();
        return;
    }
    HostGate::Ticket ticket;
    qint64           wake = 0;
    if (admit(pending, &ticket, &wake)) {
        start(pending, ticket);
        return;
    }
    if (wake < 0)
        return; // finished: its site is cooling, walled or broken
    if (m_queue.size() >= m_options.maxQueued) {
        finishLater(key, failure(errors::kBusy));
        return;
    }
    m_queue.append(pending); // its site wants a pause: it waits, others may start
    if (!m_gateTimer->isActive() || m_gateTimer->remainingTime() > wake)
        m_gateTimer->start(static_cast<int>(qBound<qint64>(1, wake, 60000)));
}

bool Fetcher::admit(const Pending& pending, HostGate::Ticket* ticket, qint64* wakeMs)
{
    const providers::FirstRequest first = providers::firstRequestOf(pending.target);
    qint64                        ms    = 0;
    bool                          soft  = false;
    HostGate::Verdict             v     = m_options.gate->acquire(first.host, first.lane, first.firstSegment, pending.caps.userAsked, &ms, ticket, &soft);
    if (v == HostGate::Verdict::Walled) {
        // A walled Klipy GIF page: the load starts with Klipy's oEmbed instead (planKlipy), under that request's own
        // verdict (walled too while a broken answer is remembered).
        const providers::FirstRequest fallback = providers::wallFallbackOf(pending.target);
        if (!fallback.host.isEmpty())
            v = m_options.gate->acquire(fallback.host, fallback.lane, fallback.firstSegment, pending.caps.userAsked, &ms, ticket, &soft);
    }
    if (v != HostGate::Verdict::Go && answersFromCache(pending)) {
        // A link the cache answers makes no request (execute() reads the same record): it starts without a ticket,
        // never held by its site's spacing, cooldown, wall or breaker (3.1).
        *ticket = HostGate::Ticket();
        return true;
    }
    switch (v) {
    case HostGate::Verdict::Go:
        return true;
    case HostGate::Verdict::Wait:
        *wakeMs = qMax<qint64>(1, ms);
        return false;
    case HostGate::Verdict::Cooling:
    case HostGate::Verdict::Broken: {
        Result result       = failure(errors::kRateLimited);
        result.retryAfterMs = ms;
        result.softCooling  = soft && v == HostGate::Verdict::Cooling;
        finishLater(pending.target.key, result);
        *wakeMs = -1;
        return false;
    }
    case HostGate::Verdict::Walled: {
        Result result       = failure(errors::kWalled);
        result.retryAfterMs = ms;
        finishLater(pending.target.key, result);
        *wakeMs = -1;
        return false;
    }
    }
    return false;
}

bool Fetcher::answersFromCache(const Pending& pending) const
{
    // The same records (and the same freshness and click rules) execute() reads first; one small file, read only
    // when the gate would hold or refuse the load.
    Result cached;
    if (pending.target.isNewClass()) {
        const qint64 now = m_options.nowSec ? m_options.nowSec() : QDateTime::currentSecsSinceEpoch();
        return readMetaV2(pending.target, pending.cacheDir, now, pending.caps.userAsked, &cached);
    }
    return readMetaV1(pending.target, pending.cacheDir, &cached);
}

void Fetcher::cancel(const QString& key)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).target.key == key) {
            m_queue.removeAt(i);
            finishLater(key, failure(errors::kCanceled));
            return;
        }
    }
    const auto it = m_active.find(key);
    if (it == m_active.end())
        return;
    it->cancel->set(); // the thread winds down at once; its own result is dropped
    m_active.erase(it);
    finishLater(key, failure(errors::kCanceled));
}

void Fetcher::shutdown()
{
    m_enabled = false;
    ++m_generation;
    m_queue.clear();
    if (m_gateTimer)
        m_gateTimer->stop();
    for (const std::shared_ptr<http::Cancel>& flag : qAsConst(m_running))
        flag->set();
    {
        QMutexLocker lock(&m_link->mutex);
        m_link->receiver = nullptr; // nothing more is posted by these jobs
    }
    m_link           = std::make_shared<Link>(); // for loads after setEnabled(true)
    m_link->receiver = this;

    const quint64 until = http::nowMs() + static_cast<quint64>(qMax(0, m_options.joinTimeoutMs));
    for (void* thread : qAsConst(m_threads)) {
        WaitForSingleObject(static_cast<HANDLE>(thread), static_cast<DWORD>(http::msUntil(until)));
        CloseHandle(static_cast<HANDLE>(thread)); // one still running keeps the DLL pinned and ends on its own
    }
    m_threads.clear();
    m_running.clear();
    m_active.clear();
}

void Fetcher::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    if (!enabled)
        shutdown();
    else
        m_enabled = true;
}

bool Fetcher::isLoading(const QString& key) const
{
    if (m_active.contains(key))
        return true;
    for (const Pending& pending : m_queue) {
        if (pending.target.key == key)
            return true;
    }
    return false;
}

void Fetcher::start(const Pending& pending, const HostGate::Ticket& ticket)
{
    auto* job        = new detail::FetchJob;
    job->link        = m_link;
    job->cancel      = std::make_shared<http::Cancel>();
    job->transport   = m_transport;
    job->gate        = m_options.gate; // each job holds its own copy: the gate lives as long as any job
    job->options     = m_options;
    job->id          = ++m_nextId;
    job->target      = pending.target;
    job->caps        = pending.caps;
    job->cacheDir    = pending.cacheDir;
    job->firstTicket = ticket;
    const quint64 id     = job->id;
    auto          cancel = job->cancel;
    auto          gate   = m_options.gate;
    HANDLE        thread = launch(job);
    if (!thread) {
        gate->unused(ticket);
        finishLater(pending.target.key, failure(errors::kInternal));
        return;
    }
    m_threads.append(thread);
    m_running.insert(id, cancel);
    Active active;
    active.id        = id;
    active.cancel    = cancel;
    active.target    = pending.target;
    active.startedMs = static_cast<qint64>(http::nowMs());
    m_active.insert(pending.target.key, active);
}

void Fetcher::startQueued()
{
    if (!m_enabled)
        return;
    qint64 wake = -1;
    for (int i = 0; i < m_queue.size() && m_running.size() < qMax(1, m_options.maxConcurrent);) {
        const Pending    pending = m_queue.at(i);
        HostGate::Ticket ticket;
        qint64           ms = 0;
        if (admit(pending, &ticket, &ms)) {
            m_queue.removeAt(i);
            start(pending, ticket);
            continue;
        }
        if (ms < 0) {
            m_queue.removeAt(i); // finished (cooling, walled, broken)
            continue;
        }
        wake = wake < 0 ? ms : qMin(wake, ms);
        ++i; // it waits for its site; the ones behind it may start
    }
    if (wake >= 0 && (!m_gateTimer->isActive() || m_gateTimer->remainingTime() > wake))
        m_gateTimer->start(static_cast<int>(qBound<qint64>(1, wake, 60000)));
}

void Fetcher::finishLater(const QString& key, const Result& result)
{
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(this, [this, generation, key, result] {
        if (generation == m_generation)
            emit finished(key, result);
    }, Qt::QueuedConnection);
}

void Fetcher::onJobProgress(quint64 id, const QString& key, qint64 received, qint64 total)
{
    const auto it = m_active.constFind(key);
    if (it != m_active.constEnd() && it->id == id)
        emit progress(key, received, total);
}

void Fetcher::onJobDone(quint64 id, const QString& key, const Result& result)
{
    m_running.remove(id);
    pruneThreads();
    const auto it = m_active.find(key);
    if (it != m_active.end() && it->id == id) {
        const Active active = *it;
        m_active.erase(it);
        const qint64 ms = static_cast<qint64>(http::nowMs()) - active.startedMs;
        LogLine      line;
        line.kind     = result.ok && result.kind != Kind::None ? result.kind : active.target.kind;
        line.cls      = active.target.cls;
        line.requests = result.requests;
        line.address  = logUrl(active.target.isNewClass() ? active.target.page : active.target.url);
        if (active.target.provider == QLatin1String("nextcloud"))
            line.address = QLatin1String("https://") + active.target.host + QLatin1Char('/') + QChar(0x2026); // a share's token opens it: never logged
        line.viaProxy = result.viaProxy;
        if (active.target.isLegacy() && result.ok)
            line.outcome = QStringLiteral("loaded, %1 KB in %2 ms").arg((qMax<qint64>(0, result.size) + 1023) / 1024).arg(ms);
        else
            line.outcome = outcomeWithHop(result, ms);
        log(line);
        QPointer<Fetcher> self(this);
        emit finished(key, result);
        if (!self)
            return;
    }
    startQueued();
}

void Fetcher::pruneThreads()
{
    for (int i = m_threads.size() - 1; i >= 0; --i) {
        if (WaitForSingleObject(static_cast<HANDLE>(m_threads.at(i)), 0) == WAIT_OBJECT_0) {
            CloseHandle(static_cast<HANDLE>(m_threads.at(i)));
            m_threads.remove(i);
        }
    }
}

void Fetcher::log(const LogLine& line) const
{
    if (m_logger)
        m_logger(line);
}

void Fetcher::setLogger(std::function<void(const QString& line)> logger)
{
    if (!logger) {
        m_logger = nullptr;
        return;
    }
    m_logger = [logger](const LogLine& line) { logger(line.text()); };
}

QString LogLine::text() const
{
    const QString what = cls == LinkClass::None || cls == LinkClass::YouTube || cls == LinkClass::Direct ? latin(kindName(kind))
                                                                                                         : latin(className(cls)) + QLatin1Char(' ') + latin(kindName(kind));
    return QStringLiteral("Link preview (%1) %2: %3%4").arg(what, address, outcome, viaProxy ? QStringLiteral(" (via the Windows proxy)") : QString());
}

bool Fetcher::cached(const Target& target, const QString& cacheDir, Result* out)
{
    if (target.isNewClass())
        return readMetaV2(target, QDir::cleanPath(cacheDir), QDateTime::currentSecsSinceEpoch(), false, out);
    return readMetaV1(target, QDir::cleanPath(cacheDir), out);
}

void Fetcher::pruneCache(const QString& cacheDir, qint64 maxTotalBytes)
{
    const QDir dir(cacheDir);
    if (cacheDir.isEmpty() || !dir.exists())
        return;
    struct Group {
        qint64      bytes = 0;
        QDateTime   newest;
        QStringList files;
    };
    std::map<QString, Group> groups;
    qint64                   total   = 0;
    const QDateTime          staleBy = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const QFileInfoList      entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        const QString key  = name.left(kKeyChars);
        if (!isKey(key) || name.size() <= kKeyChars || (name.at(kKeyChars) != QLatin1Char('.') && name.at(kKeyChars) != QLatin1Char('-')))
            continue; // not ours
        if (name.endsWith(QLatin1String(".part"))) {
            if (entry.lastModified().toUTC() < staleBy)
                QFile::remove(entry.filePath()); // left by a fetch that never finished (TeamSpeak closed)
            continue;
        }
        Group& group = groups[key];
        group.bytes += entry.size();
        group.files << entry.filePath();
        if (!group.newest.isValid() || entry.lastModified() > group.newest)
            group.newest = entry.lastModified();
        total += entry.size();
    }
    if (total <= maxTotalBytes)
        return;
    std::vector<std::pair<QDateTime, QString>> order;
    for (const auto& item : groups)
        order.emplace_back(item.second.newest, item.first);
    std::sort(order.begin(), order.end());
    for (const auto& item : order) {
        if (total <= maxTotalBytes)
            break;
        const Group& group = groups[item.second];
        // The record first: a half-deleted link is never taken for a complete one.
        QFile::remove(metaPath(QDir::cleanPath(cacheDir), item.second));
        for (const QString& file : group.files)
            QFile::remove(file);
        total -= group.bytes;
    }
}

} // namespace linkpreview
