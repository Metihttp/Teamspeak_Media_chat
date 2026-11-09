#include "fetcher.h"

#include <windows.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QRandomGenerator>

#include <algorithm>
#include <map>
#include <vector>

#include "content.h"
#include "mediaprobe.h"
#include "transport.h"

namespace linkpreview {

struct Fetcher::Link {
    QMutex            mutex;
    QPointer<Fetcher> receiver; // cleared (under mutex) by shutdown(): nothing is posted after that
};

namespace {

constexpr int    kReadChunk       = 64 * 1024;
constexpr int    kProgressEveryMs = 100;
constexpr qint64 kMaxMetaBytes    = 16 * 1024;
constexpr int    kMetaVersion     = 1;

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

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const char* errorForStatus(int status)
{
    if (status == 404 || status == 410)
        return errors::kNotFound;
    if (status == 401 || status == 403)
        return errors::kForbidden;
    if (status == 429)
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
        return "image/png,image/jpeg,image/webp,image/gif;q=0.9";
    case Kind::Video:
        return "video/mp4,video/webm,video/quicktime,video/*;q=0.8";
    case Kind::YouTube:
        return "image/jpeg,image/webp;q=0.9";
    case Kind::None:
        break;
    }
    return "*/*";
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
    if (kind == Kind::Video)
        return ext == QLatin1String("mp4") || ext == QLatin1String("mov") || ext == QLatin1String("webm");
    return ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("gif") || ext == QLatin1String("webp");
}

QString metaPath(const QString& cacheDir, const QString& key)
{
    return cacheDir + QLatin1Char('/') + key + latin(".json");
}

// The cache's record of a link; false unless it and its files are complete.
bool readMeta(const Target& target, const QString& cacheDir, Result* out)
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
    *out              = result;
    return true;
}

bool writeMeta(const Target& target, const QString& cacheDir, const Result& result, quint64 jobId)
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

// The size of a picture file as stored (before any EXIF rotation); invalid if it can't be read.
QSize pictureSize(const QString& path)
{
    QImageReader reader(path);
    return reader.size();
}

struct GetSpec {
    QUrl                                   url;
    QByteArray                             accept;
    qint64                                 maxBytes = 0;
    std::function<bool(const QUrl&)>       hopAllowed;  // every hop, on top of net::isAllowedUrl
    std::function<bool(const QByteArray&)> typeAllowed; // the Content-Type header
    std::function<bool(const QByteArray&)> headAllowed; // the first content::kSniffBytes (or the whole shorter body)
    QFile*                                 file     = nullptr; // else the body is collected in Got::body
    bool                                   progress = false;
};

struct Got {
    QString    error; // errors::k..., empty when fine
    int        status   = 0;
    qint64     total    = -1; // the announced size
    qint64     received = 0;
    QByteArray head;          // the first content::kSniffBytes
    QByteArray body;
};

} // namespace

// ---- the worker -----------------------------------------------------------------------------------------------

namespace detail {

class FetchJob
{
  public:
    std::shared_ptr<Fetcher::Link>   link;
    std::shared_ptr<http::Cancel>    cancel;
    std::shared_ptr<http::Transport> transport;
    Fetcher::Options                 options;
    quint64                          id = 0;
    Target                           target;
    qint64                           maxBytes = 0;
    QString                          cacheDir;
    HMODULE                          module = nullptr; // the pin on our DLL

    void run()
    {
        Result result;
        try {
            result = execute();
        } catch (...) {
            result = failure(errors::kInternal);
        }
        if (cancel->isSet() && result.ok) {
            result = failure(errors::kCanceled);
        }
        result.viaProxy = m_viaProxy;
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

    Result execute()
    {
        if (cancel->isSet())
            return failure(errors::kCanceled);
        if (!QDir().mkpath(cacheDir))
            return failure(errors::kDisk);
        Result cached;
        if (readMeta(target, cacheDir, &cached)) {
            if (target.kind != Kind::YouTube && cached.size > maxBytes) {
                Result tooLarge = failure(errors::kTooLarge);
                tooLarge.size   = cached.size;
                return tooLarge;
            }
            return cached;
        }
        m_deadline = http::nowMs() + static_cast<quint64>(qMax(1, options.totalTimeoutMs));
        return target.kind == Kind::YouTube ? loadYouTube() : loadMedia();
    }

    // One GET with redirects, every hop checked (netpolicy.h), the body checked while it streams.
    Got get(const GetSpec& spec)
    {
        Got        got;
        const auto fail = [&got](const char* code) {
            got.error = latin(code);
            return got;
        };
        QUrl url = spec.url;
        for (int hop = 0;; ++hop) {
            if (cancel->isSet())
                return fail(errors::kCanceled);
            if (http::msUntil(m_deadline) <= 0)
                return fail(errors::kTimeout);
            if (hop > net::kMaxRedirects)
                return fail(errors::kRedirect);
            if (!net::isAllowedUrl(url) || (spec.hopAllowed && !spec.hopAllowed(url)))
                return fail(hop == 0 ? errors::kNotPreviewable : errors::kRedirect);

            // Direct, or through the user's Windows proxy (netpolicy.h).
            const http::Route route = transport->route(url, *cancel, m_deadline);
            if (cancel->isSet())
                return fail(errors::kCanceled);
            if (route.viaProxy)
                m_viaProxy = true;

            // Direct: every address of the host must be public, before anything connects. Through a proxy
            // the proxy resolves the name (local DNS may be filtered) and connects.
            http::Error error = http::Error::None;
            if (!route.viaProxy) {
                QVector<net::Address> addresses;
                error = transport->resolve(publicHostName(url), &addresses, *cancel, m_deadline);
                if (error != http::Error::None)
                    return fail(errorFor(error));
                if (addresses.isEmpty())
                    return fail(errors::kOffline);
                for (const net::Address& address : qAsConst(addresses)) {
                    if (!net::isPublicAddress(address))
                        return fail(errors::kBlocked);
                }
            }

            http::HopOptions hopOptions;
            hopOptions.url              = url;
            hopOptions.route            = route;
            hopOptions.accept           = spec.accept;
            hopOptions.deadline         = m_deadline;
            hopOptions.connectTimeoutMs = options.connectTimeoutMs;
            hopOptions.receiveTimeoutMs = options.receiveTimeoutMs;
            if (route.viaProxy)
                hopOptions.allowPeer = [](const net::Address&) { return true; }; // the user's proxy (often on this PC)
            else
                hopOptions.allowPeer = [](const net::Address& address) { return net::isPublicAddress(address); };
            std::unique_ptr<http::Exchange> exchange = transport->open(hopOptions, *cancel, &error);
            if (!exchange || error != http::Error::None)
                return fail(errorFor(error == http::Error::None ? http::Error::BadResponse : error));
            http::Head head;
            error = exchange->head(&head);
            if (error != http::Error::None)
                return fail(errorFor(error));
            // Direct: where the response really came from (DNS may have changed since the check above).
            if (!route.viaProxy && (head.peer.isNull() || !net::isPublicAddress(head.peer)))
                return fail(errors::kBlocked);
            got.status = head.status;
            if (isRedirect(head.status)) {
                const QUrl next = net::redirectTarget(url, head.location);
                if (!next.isValid())
                    return fail(errors::kRedirect);
                url = next;
                continue;
            }
            if (head.status != 200)
                return fail(errorForStatus(head.status));
            if (spec.typeAllowed && !spec.typeAllowed(head.contentType))
                return fail(errors::kBadType);
            got.total = head.contentLength;
            if (head.contentLength > spec.maxBytes)
                return fail(errors::kTooLarge); // announced: not one byte of the body is read

            std::vector<char> buffer(kReadChunk);
            bool              sniffed = false;
            for (;;) {
                if (cancel->isSet())
                    return fail(errors::kCanceled);
                int count = 0;
                error     = exchange->read(buffer.data(), kReadChunk, &count);
                if (error != http::Error::None)
                    return fail(errorFor(error));
                if (count <= 0)
                    break;
                got.received += count;
                if (got.received > spec.maxBytes) {
                    if (head.contentLength >= 0 && got.received > head.contentLength)
                        got.total = -1; // it said less than it sent
                    return fail(errors::kTooLarge);
                }
                if (!sniffed) {
                    const int take = qMin(count, content::kSniffBytes - got.head.size());
                    got.head.append(buffer.data(), take);
                    if (got.head.size() >= content::kSniffBytes) {
                        sniffed = true;
                        if (spec.headAllowed && !spec.headAllowed(got.head))
                            return fail(errors::kBadType); // HTML or anything else: no further byte is read
                    }
                }
                if (spec.file) {
                    if (spec.file->write(buffer.data(), count) != count)
                        return fail(errors::kDisk);
                } else {
                    got.body.append(buffer.data(), count);
                }
                if (spec.progress)
                    reportProgress(got.received, head.contentLength, false);
            }
            if (!sniffed && spec.headAllowed && !spec.headAllowed(got.head))
                return fail(errors::kBadType);
            if (head.contentLength >= 0 && got.received != head.contentLength)
                return fail(errors::kBadResponse); // cut off
            if (spec.file && !spec.file->flush())
                return fail(errors::kDisk);
            if (spec.progress)
                reportProgress(got.received, head.contentLength, true);
            return got;
        }
    }

    QString base() const { return cacheDir + QLatin1Char('/') + target.key; }

    Result loadMedia()
    {
        const Kind    kind = target.kind;
        const QString part = base() + QStringLiteral(".%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return failure(errors::kDisk);
        GetSpec spec;
        spec.url         = target.url;
        spec.accept      = acceptFor(kind);
        spec.maxBytes    = maxBytes;
        spec.typeAllowed = [kind](const QByteArray& type) { return content::contentTypeAllowed(kind, type); };
        spec.headAllowed = [kind](const QByteArray& head) { return content::fits(kind, content::sniff(head)); };
        spec.file        = &file;
        spec.progress    = true;
        const Got got    = get(spec);
        file.close();
        if (!got.error.isEmpty()) {
            QFile::remove(part);
            Result result;
            result.error = got.error;
            if (got.error == QLatin1String(errors::kTooLarge))
                result.size = got.total;
            return result;
        }

        const content::Format format = content::sniff(got.head);
        const QString         media  = base() + QLatin1Char('.') + latin(content::extensionOf(format));
        const bool ours = replaceFile(part, media);
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
        if (kind != Kind::Video) {
            const QSize raw = pictureSize(media);
            if (!raw.isValid() || raw.isEmpty())
                return drop(errors::kBadType);
            if (static_cast<qint64>(raw.width()) * raw.height() > Fetcher::kMaxPixels)
                return drop(errors::kTooManyPixels);
        }

        // Size, duration and a poster / smaller picture, the way TS Media's own uploads get them.
        const LocalMediaInfo info = probeLocalMedia(media, true);
        if (cancel->isSet())
            return failure(errors::kCanceled); // the complete file stays (above)
        Result result;
        result.localFile  = media;
        result.mime       = latin(content::mimeOf(format));
        result.size       = got.received;
        result.width      = info.width;
        result.height     = info.height;
        result.durationMs = info.durationMs;
        if (!info.preview.isNull()) {
            const QString    thumb = base() + latin("-thumb.jpg");
            const QByteArray jpeg  = encodePreviewJpeg(info.preview);
            if (!jpeg.isEmpty() && writeFileAtomic(thumb, jpeg, id))
                result.thumbnailFile = thumb;
        }
        result.ok = true;
        writeMeta(target, cacheDir, result, id);
        return result;
    }

    Result loadYouTube()
    {
        const QString videoId = target.videoId;
        GetSpec       json;
        json.url         = net::oEmbedUrl(videoId);
        json.accept      = "application/json";
        json.maxBytes    = net::kMaxOEmbedBytes;
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
            return failure(meta.error.toLatin1().constData());
        }
        // 401 / 403: oEmbed is closed for this video (embedding turned off), the thumbnail still shows it.
        if (cancel->isSet())
            return failure(errors::kCanceled);

        const QString part = base() + QStringLiteral("-thumb.%1.part").arg(id);
        QFile         file(part);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return failure(errors::kDisk);
        GetSpec thumb;
        thumb.url         = thumbUrl;
        thumb.accept      = acceptFor(Kind::YouTube);
        thumb.maxBytes    = qMin(maxBytes, net::kMaxThumbnailBytes);
        thumb.hopAllowed  = [](const QUrl& url) { return net::isThumbnailHop(url); };
        thumb.typeAllowed = [](const QByteArray& type) { return content::contentTypeAllowed(Kind::YouTube, type); };
        thumb.headAllowed = [](const QByteArray& head) { return content::fits(Kind::YouTube, content::sniff(head)); };
        thumb.file        = &file;
        thumb.progress    = true;
        const Got picture = get(thumb);
        file.close();

        Result result;
        if (picture.error.isEmpty()) {
            const content::Format format = content::sniff(picture.head);
            const QString         path   = base() + latin("-thumb.") + latin(content::extensionOf(format));
            if (replaceFile(part, path)) {
                const QSize size = pictureSize(path);
                if (size.isValid() && !size.isEmpty() && static_cast<qint64>(size.width()) * size.height() <= Fetcher::kMaxPixels) {
                    result.thumbnailFile = path;
                    result.mime          = latin(content::mimeOf(format));
                    result.size          = picture.received;
                    result.width         = size.width();
                    result.height        = size.height();
                } else {
                    QFile::remove(path);
                }
            }
        }
        QFile::remove(part);
        if (cancel->isSet())
            return failure(errors::kCanceled);
        if (!haveMeta && result.thumbnailFile.isEmpty())
            return failure(picture.error == QLatin1String(errors::kNotFound) || picture.error.isEmpty() ? errors::kUnavailable
                                                                                                     : picture.error.toLatin1().constData());
        result.ok     = true;
        result.title  = embed.title;
        result.author = embed.author;
        if (haveMeta && !result.thumbnailFile.isEmpty())
            writeMeta(target, cacheDir, result, id); // a partial answer is fetched again next time
        return result;
    }

    quint64 m_deadline     = 0;
    quint64 m_lastProgress = 0;
    bool    m_viaProxy     = false; // a request of this load went through the Windows proxy
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
}

Fetcher::~Fetcher()
{
    shutdown();
    QMutexLocker lock(&m_link->mutex);
    m_link->receiver = nullptr;
}

void Fetcher::load(const Target& target, qint64 maxBytes, const QString& cacheDir)
{
    const QString& key = target.key;
    if (!m_enabled) {
        finishLater(key, failure(errors::kDisabled));
        return;
    }
    if (!target.isValid() || !isKey(key) || !net::isAllowedUrl(target.url) || cacheDir.isEmpty()) {
        finishLater(key, failure(errors::kNotPreviewable));
        return;
    }
    if (isLoading(key))
        return; // one fetch per link: the running one answers
    Pending pending;
    pending.target   = target;
    pending.maxBytes = maxBytes <= 0 ? kDefaultMaxBytes : qMin(maxBytes, kHardMaxBytes);
    pending.cacheDir = QDir::cleanPath(cacheDir);
    if (m_running.size() < qMax(1, m_options.maxConcurrent)) {
        start(pending);
    } else if (m_queue.size() < m_options.maxQueued) {
        m_queue.append(pending);
    } else {
        finishLater(key, failure(errors::kBusy));
    }
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

void Fetcher::start(const Pending& pending)
{
    auto* job      = new detail::FetchJob;
    job->link      = m_link;
    job->cancel    = std::make_shared<http::Cancel>();
    job->transport = m_transport;
    job->options   = m_options;
    job->id        = ++m_nextId;
    job->target    = pending.target;
    job->maxBytes  = pending.maxBytes;
    job->cacheDir  = pending.cacheDir;
    const quint64 id     = job->id;
    auto          cancel = job->cancel;
    HANDLE        thread = launch(job);
    if (!thread) {
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
    while (m_enabled && !m_queue.isEmpty() && m_running.size() < qMax(1, m_options.maxConcurrent))
        start(m_queue.takeFirst());
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
        line.kind     = active.target.kind;
        line.address  = logUrl(active.target.url);
        line.viaProxy = result.viaProxy;
        if (result.ok)
            line.outcome = QStringLiteral("loaded, %1 KB in %2 ms").arg((qMax<qint64>(0, result.size) + 1023) / 1024).arg(ms);
        else
            line.outcome = QStringLiteral("%1 after %2 ms").arg(result.error).arg(ms);
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
    return QStringLiteral("Link preview (%1) %2: %3%4")
        .arg(latin(kindName(kind)), address, outcome, viaProxy ? QStringLiteral(" (via the Windows proxy)") : QString());
}

bool Fetcher::cached(const Target& target, const QString& cacheDir, Result* out)
{
    return readMeta(target, QDir::cleanPath(cacheDir), out);
}

void Fetcher::pruneCache(const QString& cacheDir, qint64 maxTotalBytes)
{
    const QDir dir(cacheDir);
    if (cacheDir.isEmpty() || !dir.exists())
        return;
    struct Group {
        qint64        bytes  = 0;
        QDateTime     newest;
        QStringList   files;
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
