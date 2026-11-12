#include "giflibrary.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>
#include <QTabBar>
#include <QThread>
#include <QTimer>

#include <algorithm>

#include "i18n.h"
#include "linkpreview/content.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/lphub.h"
#include "safeimage.h"
#include "settings.h"
#include "ts3api.h"

namespace {

GifLibrary* g_instance = nullptr;

constexpr int    kSaveDelayMs      = 2000;
constexpr int    kMaxUnconfirmed   = 64;
constexpr int    kMaxThumbWaits    = 16;
constexpr int    kSendSpacingMs    = 800;
constexpr int    kMaxQueuedText    = 2;
constexpr int    kThumbW           = 348; // 3x the 116 x 86 tile
constexpr int    kThumbH           = 258;
constexpr int    kThumbLongSide    = 696;
constexpr qint64 kMaxThumbSourcePx = 64LL * 1000 * 1000;
constexpr qint64 kCopyBlock        = 1 << 20;
constexpr int    kThumbCacheBytes  = 12 << 20;
constexpr qint64 kMaxFramePixels   = 1000 * 1000;
constexpr qint64 kMaxSequencePx    = 40LL * 1000 * 1000; // half of the chat's kMaxAnimationPixels
constexpr int    kMaxFrames        = 300;
constexpr qint64 kMaxFrameBytes    = 12LL << 20;
constexpr qint64 kMaxAnimFileBytes = 25LL << 20;
constexpr qint64 kOrphanAgeSec     = 24 * 3600;
constexpr qint64 kPartAgeSec       = 3600;

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

using Cancel = std::shared_ptr<std::atomic<bool>>;

bool canceled(const Cancel& cancel)
{
    return cancel && cancel->load();
}

// ---- worker functions: plain values in, plain values out; no QObject of ours ------------------------------------

// A picture scaled to cover box (the aspect kept), centre-cropped to exactly box.
QImage coverCrop(const QImage& source, const QSize& box)
{
    if (source.isNull() || box.isEmpty())
        return {};
    const QImage scaled = source.scaled(box, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect  crop((scaled.width() - box.width()) / 2, (scaled.height() - box.height()) / 2, box.width(), box.height());
    return scaled.copy(crop).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

bool hasTransparency(const QImage& image)
{
    if (!image.hasAlphaChannel())
        return false;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) < 255)
                return true;
        }
    }
    return false;
}

// The thumbnail's pixels for a first frame of w x h: covering cw x ch, never upscaled, the long side at most cap.
QSize thumbSizeFor(const QSize& source, int cw, int ch, int cap)
{
    if (source.isEmpty())
        return {};
    qreal s = qMax(static_cast<qreal>(cw) / source.width(), static_cast<qreal>(ch) / source.height());
    s       = qMin<qreal>(s, 1.0);
    QSize out(qMax(1, qRound(source.width() * s)), qMax(1, qRound(source.height() * s)));
    const int longSide = qMax(out.width(), out.height());
    if (longSide > cap)
        out = QSize(qMax(1, out.width() * cap / longSide), qMax(1, out.height() * cap / longSide));
    return out;
}

QByteArray encodeThumb(const QImage& image, bool alpha)
{
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, alpha ? QByteArray("png") : QByteArray("jpeg"));
    if (!alpha)
        writer.setQuality(80);
    if (!writer.write(alpha ? image.convertToFormat(QImage::Format_ARGB32) : image.convertToFormat(QImage::Format_RGB32)))
        return {};
    return bytes;
}

struct ThumbOut {
    QString    name; // "<id>.jpg|png", empty when none could be made
    QByteArray format;
    int        frames = 0;
    int        width = 0, height = 0;
};

// The first frame of source (safeimage, EXIF applied) as thumbs/<id>.jpg|png (4.3), and the facts of factsPath for
// judge() (3.6) when it is set.
ThumbOut makeThumb(const QString& source, safeimage::Source sourceKind, const QString& factsPath, safeimage::Source factsKind, const QString& thumbsDir,
                   const QString& id, qint64 maxThumbBytes, const Cancel& cancel)
{
    ThumbOut out;
    if (!factsPath.isEmpty()) {
        const std::unique_ptr<QImageReader> facts = safeimage::reader(factsPath, factsKind);
        out.format                                = facts->format();
        if (!out.format.isEmpty()) {
            out.frames       = facts->imageCount();
            const QSize size = facts->size();
            out.width        = size.width();
            out.height       = size.height();
        }
    }
    if (source.isEmpty() || canceled(cancel))
        return out;
    const std::unique_ptr<QImageReader> reader = safeimage::reader(source, sourceKind);
    if (reader->format().isEmpty())
        return out;
    reader->setAutoTransform(true);
    const QSize stored = reader->size();
    if (stored.isValid() && static_cast<qint64>(stored.width()) * stored.height() > kMaxThumbSourcePx)
        return out;
    const QImage frame = reader->read();
    if (frame.isNull() || canceled(cancel))
        return out;
    if (out.width <= 0 || out.height <= 0) {
        out.width  = frame.width();
        out.height = frame.height();
    }
    const bool alpha = hasTransparency(frame);
    QByteArray bytes;
    for (const int factor : {3, 2}) {
        const QSize size = thumbSizeFor(frame.size(), kThumbW * factor / 3, kThumbH * factor / 3, kThumbLongSide * factor / 3);
        bytes            = encodeThumb(frame.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), alpha);
        if (!bytes.isEmpty() && bytes.size() <= maxThumbBytes)
            break;
    }
    if (bytes.isEmpty())
        return out;
    QDir().mkpath(thumbsDir);
    const QString name = id + (alpha ? latin(".png") : latin(".jpg"));
    QSaveFile     file(thumbsDir + QLatin1Char('/') + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return out;
    out.name = name;
    return out;
}

struct CopyOut {
    bool       ok = false;
    QString    error; // neutral text for the user
    QString    file;  // "<id>.<ext>"
    QByteArray sha;
    qint64     size = 0;
    ThumbOut   thumb;
};

// A copy of source as files/<id>.<ext> (4.2): read and written in 1 MiB blocks, hashed on the way, the cancel flag
// checked between blocks; the extension from the bytes. anim: a hover copy (gif, webp or mp4).
CopyOut copyFile(const QString& source, safeimage::Source kind, const QString& filesDir, const QString& thumbsDir, const QString& id,
                 const QByteArray& expectSha, qint64 maxBytes, bool anim, qint64 maxThumbBytes, const Cancel& cancel)
{
    CopyOut out;
    QFile   in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        out.error = i18n::t("The file can't be read.");
        return out;
    }
    if (in.size() <= 0 || in.size() > maxBytes) {
        out.error = gifs::tooBigText();
        return out;
    }
    QString          ext;
    const QByteArray format = safeimage::formatOf(source, kind);
    if (format == "gif" || format == "webp")
        ext = QString::fromLatin1(format);
    else if (anim && linkpreview::content::sniff(in.peek(linkpreview::content::kSniffBytes)) == linkpreview::content::Format::Mp4)
        ext = latin("mp4");
    if (ext.isEmpty()) {
        out.error = i18n::t("It isn't a GIF.");
        return out;
    }
    QDir().mkpath(filesDir);
    const QString name  = id + QLatin1Char('.') + ext;
    const QString final = filesDir + QLatin1Char('/') + name;
    const QString part  = final + latin(".part");
    QFile         outFile(part);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.error = i18n::t("It couldn't be written to this computer.");
        return out;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64             total = 0;
    while (!in.atEnd()) {
        if (canceled(cancel)) {
            outFile.close();
            QFile::remove(part);
            out.error = i18n::t("It was canceled.");
            return out;
        }
        const QByteArray block = in.read(kCopyBlock);
        if (block.isEmpty())
            break;
        total += block.size();
        if (total > maxBytes || outFile.write(block) != block.size()) {
            outFile.close();
            QFile::remove(part);
            out.error = total > maxBytes ? gifs::tooBigText() : i18n::t("It couldn't be written to this computer.");
            return out;
        }
        hash.addData(block);
    }
    outFile.close();
    const QByteArray sha = hash.result();
    if (!expectSha.isEmpty() && sha != expectSha) {
        QFile::remove(part);
        out.error = i18n::t("The file doesn't match what was sent.");
        return out;
    }
    QFile::remove(final);
    if (!QFile::rename(part, final)) {
        QFile::remove(part);
        out.error = i18n::t("It couldn't be written to this computer.");
        return out;
    }
    out.ok   = true;
    out.file = name;
    out.sha  = sha;
    out.size = total;
    if (!anim)
        out.thumb = makeThumb(final, safeimage::Source::Peer, final, safeimage::Source::Peer, thumbsDir, id, maxThumbBytes, cancel);
    return out;
}

QImage decodeTile(const QString& path, const QSize& devicePx, bool blurred)
{
    const std::unique_ptr<QImageReader> reader = safeimage::reader(path, safeimage::Source::Local);
    const QImage                        image  = reader->read();
    QImage                              tile   = coverCrop(image, devicePx);
    if (blurred && !tile.isNull()) {
        const QSize small(qMax(1, tile.width() / 12), qMax(1, tile.height() / 12));
        tile = tile.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).scaled(devicePx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return tile;
}

struct AnimOut {
    bool            ok = false;
    QVector<QImage> frames;
    QVector<int>    delays;
};

// Every frame of a GIF / WebP scaled to cover devicePx, with the browsers' delay clamp (5.7). Refused (still only) over
// the budget: a frame over 1 Mpx, a sequence over 40 Mpx, more than 300 frames or 12 MiB of scaled frames.
AnimOut decodeAnimation(const QString& path, safeimage::Source kind, const QSize& devicePx, const Cancel& cancel)
{
    AnimOut out;
    if (QFileInfo(path).size() > kMaxAnimFileBytes)
        return out;
    const std::unique_ptr<QImageReader> reader = safeimage::reader(path, kind);
    if (reader->format().isEmpty())
        return out;
    const QSize  size   = reader->size();
    const qint64 pixels = size.isValid() ? static_cast<qint64>(size.width()) * size.height() : 0;
    const int    count  = reader->imageCount();
    if (pixels <= 0 || pixels > kMaxFramePixels || (count > 0 && pixels * count > kMaxSequencePx))
        return out;
    qint64 bytes = 0;
    while (out.frames.size() < kMaxFrames && reader->canRead()) {
        if (canceled(cancel))
            return AnimOut();
        const QImage frame = reader->read();
        if (frame.isNull())
            break;
        const int    delay = reader->nextImageDelay();
        const QImage tile  = coverCrop(frame, devicePx);
        bytes += tile.sizeInBytes();
        if (bytes > kMaxFrameBytes || pixels * (out.frames.size() + 1) > kMaxSequencePx)
            return AnimOut();
        out.frames.append(tile);
        out.delays.append(delay < 20 ? 100 : delay);
    }
    out.ok = !out.frames.isEmpty();
    return out;
}

} // namespace

// ============================================================================================
// The library
// ============================================================================================

GifLibrary::GifLibrary(Core* core, const QString& folder, const QString& linksDir, QObject* parent)
    : QObject(parent)
    , m_core(core)
    , m_folder(QDir::cleanPath(folder))
    , m_linksDir(linksDir)
    , m_cancel(std::make_shared<std::atomic<bool>>(false))
    , m_thumbs(kThumbCacheBytes)
{
    g_instance = this;
    m_clock.start();
    m_work.setMaxThreadCount(1);
    m_decode.setMaxThreadCount(2);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { writeNow(); });
    if (core) {
        // Direct: mediaPosted's slot reads the job while Core still has its link.
        watch(connect(core, &Core::mediaPosted, this, &GifLibrary::onMediaPosted, Qt::DirectConnection));
        watch(connect(core, &Core::uploadChanged, this, &GifLibrary::onUploadChanged, Qt::DirectConnection));
        watch(connect(core, &Core::entryChanged, this, &GifLibrary::onEntryChanged, Qt::DirectConnection));
    }
}

GifLibrary::~GifLibrary()
{
    prepareShutdown();
    if (g_instance == this)
        g_instance = nullptr;
}

GifLibrary* GifLibrary::instance()
{
    return g_instance;
}

void GifLibrary::watch(const QMetaObject::Connection& connection)
{
    if (connection)
        m_connections.append(connection);
}

qint64 GifLibrary::nowSec() const
{
    return m_nowForTests ? m_nowForTests() : QDateTime::currentSecsSinceEpoch();
}

GifLibrary::State GifLibrary::state() const
{
    if (m_unavailable)
        return State::Unavailable;
    return m_store.readOnly() ? State::ReadOnly : State::Ok;
}

bool GifLibrary::usable() const
{
    return !m_down && m_started && state() == State::Ok;
}

QString GifLibrary::thumbsDir() const
{
    return m_folder + latin("/thumbs");
}

QString GifLibrary::filesDir() const
{
    return m_folder + latin("/files");
}

QString GifLibrary::jsonPath() const
{
    return m_folder + latin("/gifs.json");
}

QString GifLibrary::linksDir() const
{
    return m_linksDir.isEmpty() ? linkpreview::linksCacheDir(m_core) : m_linksDir;
}

QString GifLibrary::copyPath(const gifs::Item& item) const
{
    return item.source == gifs::Source::File && !item.file.isEmpty() ? filesDir() + QLatin1Char('/') + item.file : QString();
}

bool GifLibrary::copyExists(const gifs::Item& item) const
{
    const QString path = copyPath(item);
    return !path.isEmpty() && QFileInfo(path).isFile();
}

QString GifLibrary::hostOf(const gifs::Item& item) const
{
    if (item.source != gifs::Source::Link)
        return {};
    const linkpreview::Target t = linkpreview::classifyCached(item.url);
    if (t.isValid())
        return t.host;
    return linkpreview::publicHostName(QUrl(item.url, QUrl::TolerantMode));
}

// ---- load, save, sweep --------------------------------------------------------------------------------------------

void GifLibrary::start()
{
    if (m_started || m_down)
        return;
    m_started = true;
    const auto factsOf = [](const QString& url) {
        // Today's classifier, not its cache (at most 100 links, once per start).
        const linkpreview::Target t = linkpreview::classify(url);
        gifs::LinkFacts           facts;
        facts.key   = t.isValid() ? t.key : QString();
        facts.media = t.isValid() && (t.cls == linkpreview::LinkClass::Direct || t.cls == linkpreview::LinkClass::MediaPage);
        facts.pid   = gifs::pidOf(t);
        return facts;
    };
    QFile file(jsonPath());
    if (!file.exists()) {
        m_load = gifs::Store::Load::Missing;
        return;
    }
    if (m_openFailsForTests || !file.open(QIODevice::ReadOnly)) {
        // An antivirus scan, another program holding it: read-only for this session, nothing moved or deleted.
        m_load        = gifs::Store::Load::Unavailable;
        m_unavailable = true;
        m_store.setReadOnly(true);
        ts3::log(latin("GIF list couldn't be opened; left unchanged"), LogLevel_WARNING);
        return;
    }
    const QByteArray bytes = file.read(m_store.limits().maxFileBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        m_load        = gifs::Store::Load::Unavailable;
        m_unavailable = true;
        m_store.setReadOnly(true);
        ts3::log(latin("GIF list couldn't be opened; left unchanged"), LogLevel_WARNING);
        return;
    }
    file.close();
    m_load = m_store.fromJson(bytes, nowSec(), factsOf, &m_stats);
    switch (m_load) {
    case gifs::Store::Load::Corrupt:
    case gifs::Store::Load::TooLarge: {
        const QString bad = m_folder + latin("/gifs.json.bad");
        QFile::remove(bad);
        if (m_renameFailsForTests || !QFile::rename(jsonPath(), bad)) {
            // Not moved aside (held by another program, the old .bad couldn't go): as if unreadable for this session, so
            // no save overwrites it and the next start's sweep never deletes the copies it names (D5, review fix).
            m_load        = gifs::Store::Load::Unavailable;
            m_unavailable = true;
            m_store.setReadOnly(true);
            ts3::log(latin("GIF list unreadable and couldn't be moved aside; left unchanged"), LogLevel_WARNING);
            return;
        }
        ts3::log(latin("GIF list unreadable, moved aside"), LogLevel_WARNING);
        return;
    }
    case gifs::Store::Load::Newer:
        ts3::log(latin("GIF list was saved by a newer TS Media; left unchanged"), LogLevel_WARNING);
        return;
    default:
        break;
    }
    if (m_stats.dropped || m_stats.rekeyed)
        ts3::log(QString::fromLatin1("GIF list: %1 entries skipped, %2 re-keyed").arg(m_stats.dropped).arg(m_stats.rekeyed));
    if (m_load == gifs::Store::Load::Ok && (m_store.count() + m_store.hiddenCount()) > 0 && !QFileInfo::exists(m_folder + latin("/gifs.json.bad")))
        sweep();
}

void GifLibrary::sweep()
{
    const QSet<QString> named   = m_store.namedFiles();
    const QStringList   dirs    = {thumbsDir(), filesDir()};
    const qint64        now     = nowSec();
    const Cancel        cancel  = m_cancel;
    QPointer<GifLibrary> self(this);
    ++m_pending;
    m_work.start([self, named, dirs, now, cancel] {
        int removed = 0;
        for (const QString& dir : dirs) {
            const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo& entry : entries) {
                if (canceled(cancel))
                    break;
                const QString name = entry.fileName();
                if (!gifs::isOurPattern(name) || named.contains(name))
                    continue;
                const qint64 age = now - entry.lastModified().toSecsSinceEpoch();
                // A younger orphan may be another TeamSpeak's on the same profile that hasn't written its list yet.
                if (age > (name.endsWith(QLatin1String(".part")) ? kPartAgeSec : kOrphanAgeSec) && QFile::remove(entry.filePath()))
                    ++removed;
            }
        }
        QMetaObject::invokeMethod(self.data(), [self, removed] {
            if (!self)
                return;
            --self->m_pending;
            if (removed > 0)
                ts3::log(QString::fromLatin1("GIFs: sweep removed %1 files").arg(removed));
        }, Qt::QueuedConnection);
    });
}

void GifLibrary::scheduleSave()
{
    m_dirty = true;
    if (!m_down && m_saveTimer)
        m_saveTimer->start();
}

bool GifLibrary::writeNow()
{
    if (!m_dirty || m_store.readOnly())
        return true;
    QDir().mkpath(m_folder);
    QSaveFile        file(jsonPath());
    const QByteArray bytes = m_store.toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (!m_saveFailedLogged)
            ts3::log(QString::fromLatin1("GIF list couldn't be saved: %1").arg(file.errorString()), LogLevel_WARNING);
        m_saveFailedLogged = true;
        return false;
    }
    m_dirty            = false;
    m_saveFailedLogged = false;
    return true;
}

void GifLibrary::markChanged()
{
    scheduleSave();
    emit changed();
}

void GifLibrary::prepareShutdown()
{
    if (m_down)
        return;
    m_down = true;
    if (m_cancel)
        m_cancel->store(true);
    if (m_animCancel)
        m_animCancel->store(true);
    if (m_saveTimer)
        m_saveTimer->stop();
    ++m_gen;
    m_work.clear();
    m_decode.clear();
    m_work.waitForDone();
    m_decode.waitForDone();
    writeNow();
    for (const QMetaObject::Connection& c : qAsConst(m_connections))
        disconnect(c);
    m_connections.clear();
    m_thumbs.clear();
    m_decoding.clear();
    m_animFrames.clear();
    m_animDelays.clear();
    m_animId.clear();
    m_unconfirmed.clear();
    m_thumbWaits.clear();
    m_copying.clear();
}

void GifLibrary::waitForWorkers()
{
    for (int round = 0; round < 50; ++round) {
        m_work.waitForDone();
        m_decode.waitForDone();
        QCoreApplication::sendPostedEvents(this);
        QCoreApplication::processEvents();
        if (m_pending.load() <= 0 && m_work.activeThreadCount() == 0 && m_decode.activeThreadCount() == 0)
            return;
    }
}

// ---- removing ---------------------------------------------------------------------------------------------------------

bool GifLibrary::copyInUse(const QString& path) const
{
    if (!m_core || path.isEmpty())
        return false;
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        if (!job || QDir::cleanPath(QFileInfo(job->sourcePath).absoluteFilePath()).compare(clean, Qt::CaseInsensitive) != 0)
            continue;
        if (job->state != UploadState::Done && job->state != UploadState::Canceled)
            return true; // running, or Failed and retryable: the sweep takes it later
    }
    return false;
}

void GifLibrary::removeFiles(const gifs::Item& item)
{
    if (!item.thumb.isEmpty())
        QFile::remove(thumbsDir() + QLatin1Char('/') + item.thumb);
    if (!item.anim.isEmpty())
        QFile::remove(filesDir() + QLatin1Char('/') + item.anim);
    const QString copy = copyPath(item);
    if (!copy.isEmpty() && !copyInUse(copy))
        QFile::remove(copy);
}

void GifLibrary::dropFromSession(const QString& id)
{
    m_unconfirmed.removeAll(id);
    for (auto it = m_thumbWaits.begin(); it != m_thumbWaits.end();) {
        if (it.value() == id)
            it = m_thumbWaits.erase(it);
        else
            ++it;
    }
    const QString prefix = id + QLatin1Char('|');
    for (const QString& key : m_thumbs.keys()) {
        if (key.startsWith(prefix))
            m_thumbs.remove(key);
    }
    if (m_animId == id) {
        if (m_animCancel)
            m_animCancel->store(true);
        m_animId.clear();
        m_animFrames.clear();
        m_animDelays.clear();
    }
}

bool GifLibrary::remove(const QString& id)
{
    if (!usable())
        return false;
    const std::optional<gifs::Item> gone = m_store.remove(id);
    if (!gone)
        return false;
    dropFromSession(id);
    // The list first, then the files (2.5).
    m_dirty = true;
    writeNow();
    removeFiles(*gone);
    emit changed();
    return true;
}

void GifLibrary::clear()
{
    if (m_down || state() != State::Ok)
        return;
    if (m_cancel)
        m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    ++m_gen;
    if (m_animCancel)
        m_animCancel->store(true);
    m_store.clear();
    m_unconfirmed.clear();
    m_thumbWaits.clear();
    m_copying.clear();
    m_thumbs.clear();
    m_decoding.clear();
    m_animId.clear();
    m_animFrames.clear();
    m_animDelays.clear();
    m_dirty = true;
    writeNow();
    QFile::remove(m_folder + latin("/gifs.json.bad"));
    int removed = 0;
    for (const QString& dir : {thumbsDir(), filesDir()}) {
        const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& entry : entries) {
            if (gifs::isOurPattern(entry.fileName()) && QFile::remove(entry.filePath()))
                ++removed;
        }
    }
    ts3::log(QString::fromLatin1("GIFs: removed all (%1 files)").arg(removed));
    emit changed();
}

qint64 GifLibrary::leftoverBytes(int* files) const
{
    const QSet<QString> named = m_store.namedFiles();
    int                 count = 0;
    qint64              bytes = 0;
    for (const QString& dir : {thumbsDir(), filesDir()}) {
        for (const QFileInfo& entry : QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
            if (gifs::isOurPattern(entry.fileName()) && !named.contains(entry.fileName())) {
                ++count;
                bytes += entry.size();
            }
        }
    }
    if (files)
        *files = count;
    return bytes;
}

bool GifLibrary::hasAnythingToRemove() const
{
    if (m_down || state() != State::Ok)
        return false;
    if (m_store.count() + m_store.hiddenCount() > 0 || QFileInfo::exists(m_folder + latin("/gifs.json.bad")))
        return true;
    int files = 0;
    leftoverBytes(&files);
    return files > 0;
}

bool GifLibrary::touch(const QString& id)
{
    if (!usable() || !m_store.touch(id, nowSec()))
        return false;
    markChanged();
    return true;
}

void GifLibrary::evicted(const QStringList& ids, const QVector<gifs::Item>& before)
{
    if (ids.isEmpty())
        return;
    for (const QString& id : ids) {
        dropFromSession(id);
        for (const gifs::Item& item : before) {
            if (item.id == id)
                removeFiles(item);
        }
    }
    ts3::log(QString::fromLatin1("GIFs: %1 removed to stay within %2").arg(ids.size()).arg(m_store.limits().maxItems));
}

void GifLibrary::applyBudget()
{
    const QStringList over = m_store.overBudget();
    for (const QString& id : over) {
        const gifs::Item* item = m_store.find(id);
        if (!item)
            continue;
        if (item->source == gifs::Source::File) {
            const std::optional<gifs::Item> gone = m_store.remove(id);
            dropFromSession(id);
            if (gone)
                removeFiles(*gone);
        } else {
            gifs::Item changedItem = *item;
            QFile::remove(filesDir() + QLatin1Char('/') + changedItem.anim);
            changedItem.anim.clear();
            changedItem.animSize = 0;
            m_store.update(changedItem);
        }
    }
    if (!over.isEmpty())
        ts3::log(QString::fromLatin1("GIFs: %1 copies removed to stay within 256 MB").arg(over.size()));
}

// ---- recording ------------------------------------------------------------------------------------------------------

void GifLibrary::onTextMessage(uint64 sch, int mode, anyID toId, anyID fromId, const QString& fromUid, const QString& bbcode)
{
    Q_UNUSED(toId);
    if (!usable() || !Settings::instance().rememberGifs)
        return;
    if (fromId != ts3::ownClientId(sch) || fromUid.isEmpty() || fromUid != ts3::ownUid(sch))
        return; // only ours (the sender pipeline's check)
    if (mode != TextMessageTarget_CHANNEL && mode != TextMessageTarget_CLIENT && mode != TextMessageTarget_SERVER)
        return;
    const QVector<gifs::Candidate> links = gifs::messageLinks(bbcode);
    const qint64                   now   = nowSec();
    QStringList                    keys;
    bool                           any = false;
    // Backwards: the message's first link ends up in front (equal times keep the list's order).
    for (int i = links.size() - 1; i >= 0; --i) {
        const gifs::Candidate& c = links.at(i);
        if (!c.target.isValid() || !gifs::storableUrl(c.url))
            continue;
        if (const gifs::Item* existing = m_store.findLink(c.target.key, c.pid)) {
            gifs::Item touchItem;
            touchItem.source = gifs::Source::Link;
            touchItem.url    = c.url;
            touchItem.urlKey = existing->urlKey;
            touchItem.pid    = existing->pid;
            m_store.add(touchItem, now);
            keys.prepend(existing->urlKey);
            any = true;
            continue;
        }
        if (!c.autoRecord)
            continue;
        gifs::Item item;
        item.source = gifs::Source::Link;
        item.kind   = c.kind;
        item.how    = gifs::How::Sent;
        item.url    = c.url;
        item.urlKey = c.target.key;
        item.pid    = c.pid;
        const QVector<gifs::Item> before = m_store.items();
        QStringList               out;
        const QString             id = m_store.add(item, now, &out);
        if (id.isEmpty())
            continue;
        evicted(out, before);
        m_unconfirmed.append(id);
        while (m_unconfirmed.size() > kMaxUnconfirmed)
            m_unconfirmed.removeFirst(); // its item simply stays
        keys.prepend(item.urlKey);
        any = true;
        ts3::log(latin("GIFs: added (link)"));
    }
    if (!any)
        return;
    markChanged();
    for (const QString& key : qAsConst(keys))
        onLinkLoaded(key); // a resend, or a link loaded before
}

void GifLibrary::onLinkLoaded(const QString& urlKey)
{
    if (!usable() || urlKey.isEmpty())
        return;
    const gifs::Item* found = m_store.findLink(urlKey, QString());
    if (!found || found->plain)
        return;
    const gifs::Item item        = *found;
    const bool       unconfirmed = m_unconfirmed.contains(item.id);
    if (!unconfirmed && !item.thumb.isEmpty())
        return;
    const linkpreview::Target target = linkpreview::classifyCached(item.url);
    linkpreview::Result       result;
    if (!target.isValid() || target.key != urlKey || !linkpreview::Fetcher::cached(target, linksDir(), &result))
        return;
    if (!result.ok) {
        // A dead or mistyped link: an automatic record of this session goes again. Busy, too large, walled: nothing.
        if (unconfirmed && (result.error == QLatin1String(linkpreview::errors::kNotFound) || result.error == QLatin1String(linkpreview::errors::kBadType)))
            remove(item.id);
        return;
    }
    using linkpreview::Kind;
    const Kind kind      = result.kind != Kind::None ? result.kind : target.kind;
    const bool newClass  = target.isNewClass();
    const bool stillOrVideo = kind == Kind::Video || kind == Kind::Card || kind == Kind::YouTube || (newClass && kind == Kind::Image);
    const QString thumbSource = !result.thumbnailFile.isEmpty() ? result.thumbnailFile
                                : (kind == Kind::Image || kind == Kind::Gif) ? result.localFile
                                                                              : QString();
    if (unconfirmed && (result.card.sensitive || stillOrVideo)) {
        remove(item.id); // a page marked for adults, a still or a video: no automatic record
        return;
    }
    if (!unconfirmed) {
        if (item.thumb.isEmpty() && !thumbSource.isEmpty())
            startThumb(item.id, ThumbPurpose::Fill, thumbSource, true, QString());
        return;
    }
    if (kind == Kind::Loop) {
        // Loops are judged by the Fetcher (silent, at most 60 s): confirmed with the poster as thumbnail.
        gifs::Item confirmed = item;
        confirmed.kind       = gifs::Kind::Loop;
        confirmed.size       = qBound<qint64>(0, result.size, gifs::kMaxLinkSize);
        confirmed.width      = qBound(0, result.width, gifs::kMaxDimension);
        confirmed.height     = qBound(0, result.height, gifs::kMaxDimension);
        confirmed.durationMs = qBound<qint64>(0, result.durationMs, gifs::kMaxDurationMs);
        m_store.update(confirmed);
        m_unconfirmed.removeAll(item.id);
        scheduleSave();
        emit itemUpdated(item.id);
        if (!result.thumbnailFile.isEmpty())
            startThumb(item.id, ThumbPurpose::Fill, result.thumbnailFile, true, QString());
        return;
    }
    // A GIF page's file or a direct .gif / .webp: the worker reads its format and frames (judge) and makes the thumbnail.
    gifs::Item sized = item;
    sized.size       = qBound<qint64>(0, result.size, gifs::kMaxLinkSize);
    m_store.update(sized);
    startThumb(item.id, ThumbPurpose::Confirm, thumbSource, true, result.localFile);
}

void GifLibrary::startThumb(const QString& id, ThumbPurpose purpose, const QString& source, bool sourceIsWeb, const QString& factsPath)
{
    const QPointer<GifLibrary> self(this);
    const int                  gen     = m_gen;
    const Cancel               cancel  = m_cancel;
    const QString              thumbs  = thumbsDir();
    const qint64               maxBytes = m_store.limits().maxThumbBytes;
    const safeimage::Source    kind    = sourceIsWeb ? safeimage::Source::Website : safeimage::Source::Peer;
    ++m_pending;
    m_work.start([self, gen, cancel, thumbs, maxBytes, kind, id, purpose, source, factsPath] {
        const ThumbOut out = makeThumb(source, kind, factsPath, kind, thumbs, id, maxBytes, cancel);
        QMetaObject::invokeMethod(self.data(), [self, gen, id, purpose, out] {
            if (!self)
                return;
            --self->m_pending;
            if (self->m_down || gen != self->m_gen) {
                if (!out.name.isEmpty() && !self->m_down)
                    QFile::remove(self->thumbsDir() + QLatin1Char('/') + out.name); // Remove all came in between
                return;
            }
            self->onThumbMade(id, purpose, out.name, out.format, out.frames, out.width, out.height);
        }, Qt::QueuedConnection);
    });
}

void GifLibrary::onThumbMade(const QString& id, ThumbPurpose purpose, const QString& thumb, const QByteArray& format, int frames, int w, int h)
{
    const gifs::Item* found = m_store.find(id);
    if (!found) {
        if (!thumb.isEmpty())
            QFile::remove(thumbsDir() + QLatin1Char('/') + thumb); // removed meanwhile
        return;
    }
    gifs::Item item = *found;
    if (purpose == ThumbPurpose::Confirm && m_unconfirmed.contains(id)) {
        gifs::Facts facts;
        facts.format         = format;
        facts.frames         = frames;
        const gifs::Verdict v = gifs::judge(facts);
        if (v == gifs::Verdict::NotGif) {
            if (!thumb.isEmpty() && thumb != item.thumb)
                QFile::remove(thumbsDir() + QLatin1Char('/') + thumb);
            remove(id); // a still: no automatic record
            return;
        }
        item.kind = gifs::Kind::Gif; // a link item stores an animated WebP as gif
        m_unconfirmed.removeAll(id);
    }
    if (!thumb.isEmpty())
        item.thumb = thumb;
    if (w > 0 && h > 0 && w <= gifs::kMaxDimension && h <= gifs::kMaxDimension && (item.width <= 0 || item.height <= 0)) {
        item.width  = w;
        item.height = h;
    }
    const QString prefix = id + QLatin1Char('|');
    for (const QString& key : m_thumbs.keys()) {
        if (key.startsWith(prefix))
            m_thumbs.remove(key);
    }
    if (m_store.update(item)) {
        scheduleSave();
        emit itemUpdated(id);
    }
}

void GifLibrary::onMediaPosted(int id)
{
    if (!usable() || !m_core || !Settings::instance().rememberGifs)
        return;
    const UploadJob* job  = m_core->upload(id);
    const MediaLink* link = m_core->postedLink(id);
    if (!job || !link || job->background || !job->info.animated)
        return;
    const MediaKind kind = kindForFileName(link->fileName);
    const bool      webp = link->fileName.endsWith(QLatin1String(".webp"), Qt::CaseInsensitive);
    if (!(kind == MediaKind::AnimatedImage || (kind == MediaKind::Image && webp)) || link->spoiler)
        return;
    if (static_cast<qint64>(link->size) > m_store.limits().maxCopyBytes) {
        ts3::log(latin("GIFs: upload over 25 MB not kept"));
        return;
    }
    // Copied now (the slot returns before the worker starts): what the job and its link say.
    const QByteArray sha = link->sha256;
    if (const gifs::Item* existing = m_store.findFile(sha)) {
        touch(existing->id); // a resend from the tab comes back here
        return;
    }
    QString           source;
    const MediaEntry* entry = m_core->entry(link->key());
    if (entry && entry->state == MediaState::Ready && entry->isOwnUpload && QFileInfo(entry->localPath).isFile())
        source = entry->localPath;
    else if (!job->deleteSource && QFileInfo(job->sourcePath).size() == static_cast<qint64>(link->size))
        source = job->sourcePath;
    if (source.isEmpty()) {
        ts3::log(latin("GIFs: an upload's file is gone; not kept"));
        return;
    }
    const QString name = gifs::itemName(displayNameFor(*link)); // cleaned, its extension kept
    startCopy(CopyPurpose::Upload, m_store.newId(), source, sha, name, false, link->width, link->height);
}

void GifLibrary::startCopy(CopyPurpose purpose, const QString& id, const QString& source, const QByteArray& sha, const QString& name, bool spoiler, int w,
                           int h)
{
    const QString shaKey = QString::fromLatin1(sha.toHex());
    if (!sha.isEmpty() && purpose != CopyPurpose::Anim) {
        if (m_copying.contains(shaKey))
            return; // the same file is being copied already
        m_copying.insert(shaKey);
    }
    const QPointer<GifLibrary> self(this);
    const int                  gen      = m_gen;
    const Cancel               cancel   = m_cancel;
    const QString              files    = filesDir();
    const QString              thumbs   = thumbsDir();
    const qint64               maxBytes = purpose == CopyPurpose::Anim ? m_store.limits().maxAnimCopyBytes : m_store.limits().maxCopyBytes;
    const qint64               maxThumb = m_store.limits().maxThumbBytes;
    const bool                 anim     = purpose == CopyPurpose::Anim;
    const safeimage::Source    kind     = safeimage::Source::Peer;
    ++m_pending;
    m_work.start([=] {
        const CopyOut out = copyFile(source, kind, files, thumbs, id, sha, maxBytes, anim, maxThumb, cancel);
        QMetaObject::invokeMethod(self.data(), [=] {
            if (!self)
                return;
            --self->m_pending;
            self->m_copying.remove(shaKey);
            if (self->m_down || gen != self->m_gen) {
                // Remove all or shutdown came in between: nothing comes back (its files go).
                if (!self->m_down && out.ok) {
                    QFile::remove(files + QLatin1Char('/') + out.file);
                    if (!out.thumb.name.isEmpty())
                        QFile::remove(thumbs + QLatin1Char('/') + out.thumb.name);
                }
                return;
            }
            self->onCopied(purpose, id, out.ok, out.error, out.file, out.sha, out.size, out.thumb.name, out.thumb.width > 0 ? out.thumb.width : w,
                           out.thumb.height > 0 ? out.thumb.height : h, name, spoiler);
        }, Qt::QueuedConnection);
    });
}

void GifLibrary::onCopied(CopyPurpose purpose, const QString& id, bool ok, const QString& error, const QString& file, const QByteArray& sha, qint64 size,
                          const QString& thumb, int w, int h, const QString& name, bool spoiler)
{
    const auto dropCopy = [this, &file, &thumb] {
        if (!file.isEmpty())
            QFile::remove(filesDir() + QLatin1Char('/') + file);
        if (!thumb.isEmpty())
            QFile::remove(thumbsDir() + QLatin1Char('/') + thumb);
    };
    if (purpose == CopyPurpose::Anim) {
        const gifs::Item* found = m_store.find(id);
        if (!ok || !found || !found->anim.isEmpty()) {
            if (ok)
                dropCopy();
            return;
        }
        gifs::Item item = *found;
        item.anim       = file;
        item.animSize   = size;
        if (!m_store.update(item)) {
            dropCopy();
            return;
        }
        applyBudget();
        scheduleSave();
        emit itemUpdated(id);
        return;
    }
    if (!ok) {
        if (purpose == CopyPurpose::SaveFile)
            emit saveFinished(QString(), false, i18n::t("Couldn't save the GIF: %1").arg(error));
        else
            ts3::log(latin("GIFs: an upload's copy couldn't be made; not kept"));
        return;
    }
    if (purpose == CopyPurpose::Upload && !Settings::instance().rememberGifs) {
        dropCopy(); // switched off meanwhile
        return;
    }
    gifs::Item item;
    item.id         = id;
    item.source     = gifs::Source::File;
    item.kind       = file.endsWith(QLatin1String(".webp")) ? gifs::Kind::Webp : gifs::Kind::Gif;
    item.how        = purpose == CopyPurpose::Upload ? gifs::How::Sent : gifs::How::Saved;
    item.name       = name;
    item.file       = file;
    item.sha        = sha;
    item.spoiler    = spoiler;
    item.size       = size;
    item.width      = qBound(0, w, gifs::kMaxDimension);
    item.height     = qBound(0, h, gifs::kMaxDimension);
    item.thumb      = thumb;
    const QVector<gifs::Item> before = m_store.items();
    QStringList               out;
    const QString             added = m_store.add(item, nowSec(), &out);
    if (added != id) {
        dropCopy(); // the same file is in the list already (it was touched), or it can't be kept
        if (purpose == CopyPurpose::SaveFile)
            emit saveFinished(added, !added.isEmpty(), added.isEmpty() ? i18n::t("Couldn't save the GIF: %1").arg(i18n::t("It can't be kept."))
                                                                        : i18n::t("Already in your GIFs"));
        if (!added.isEmpty())
            markChanged();
        return;
    }
    evicted(out, before);
    applyBudget();
    markChanged();
    ts3::log(QString::fromLatin1("GIFs: added (file, %1)").arg(formatSize(static_cast<quint64>(size))));
    if (purpose == CopyPurpose::SaveFile)
        emit saveFinished(id, true, i18n::t("Saved to GIFs"));
}

// ---- Save to GIFs ---------------------------------------------------------------------------------------------------

bool GifLibrary::saveLink(const QString& href, const linkpreview::Target& target, const QString& entryKey, bool spoiler, gifs::Kind kind, QString* message)
{
    const auto fail = [message](const QString& text) {
        if (message)
            *message = text;
        return false;
    };
    if (!usable())
        return fail(i18n::t("Couldn't save the GIF: %1").arg(i18n::t("Your GIF list can't be changed right now.")));
    const QString url = linkpreview::trimLinkEnd(href);
    if (!target.isValid() || !gifs::storableUrl(url))
        return fail(i18n::t("Couldn't save the GIF: %1").arg(i18n::t("This link can't be kept.")));
    const QString pid = gifs::pidOf(target);
    if (const gifs::Item* existing = m_store.findLink(target.key, pid)) {
        touch(existing->id);
        if (message)
            *message = i18n::t("Already in your GIFs");
        return true;
    }
    gifs::Item item;
    item.source  = gifs::Source::Link;
    item.kind    = kind == gifs::Kind::Loop ? gifs::Kind::Loop : gifs::Kind::Gif;
    item.how     = gifs::How::Saved;
    item.url     = url;
    item.urlKey  = target.key;
    item.pid     = pid;
    item.spoiler = spoiler;
    if (const MediaEntry* e = m_core ? m_core->entry(entryKey) : nullptr) {
        item.size   = qBound<qint64>(0, static_cast<qint64>(e->link.size), gifs::kMaxLinkSize);
        item.width  = qBound(0, e->link.width, gifs::kMaxDimension);
        item.height = qBound(0, e->link.height, gifs::kMaxDimension);
        item.durationMs = qBound<qint64>(0, e->link.durationMs, gifs::kMaxDurationMs);
    }
    const QVector<gifs::Item> before = m_store.items();
    QStringList               out;
    const QString             id = m_store.add(item, nowSec(), &out);
    if (id.isEmpty())
        return fail(i18n::t("Couldn't save the GIF: %1").arg(i18n::t("This link can't be kept.")));
    evicted(out, before);
    markChanged();
    fillFromEntry(id, entryKey);
    if (message)
        *message = i18n::t("Saved to GIFs");
    return true;
}

// The thumbnail and the hover copy of a saved link item from the Core entry the chat shows (never a download): now when
// it is Ready, else at its next entryChanged.
void GifLibrary::fillFromEntry(const QString& id, const QString& entryKey)
{
    const gifs::Item* item  = m_store.find(id);
    const MediaEntry* entry = m_core ? m_core->entry(entryKey) : nullptr;
    if (!item || !entry)
        return;
    const bool video  = entry->kind == MediaKind::Video;
    QString    source;
    if (video && entry->previewState == MediaState::Ready && QFileInfo(entry->previewPath).isFile())
        source = entry->previewPath;
    else if (!video && entry->state == MediaState::Ready && QFileInfo(entry->localPath).isFile())
        source = entry->localPath;
    const bool needThumb = item->thumb.isEmpty();
    const bool needAnim  = item->anim.isEmpty() && entry->state == MediaState::Ready
                          && QFileInfo(entry->localPath).size() <= m_store.limits().maxAnimCopyBytes;
    if (needThumb && !source.isEmpty())
        startThumb(id, ThumbPurpose::Fill, source, false, QString());
    if (needAnim && QFileInfo(entry->localPath).isFile())
        startCopy(CopyPurpose::Anim, id, entry->localPath, QByteArray(), QString(), false, 0, 0);
    if ((needThumb && source.isEmpty()) || (item->anim.isEmpty() && entry->state != MediaState::Ready)) {
        m_thumbWaits.insert(entryKey, id);
        while (m_thumbWaits.size() > kMaxThumbWaits)
            m_thumbWaits.erase(m_thumbWaits.begin());
    } else {
        m_thumbWaits.remove(entryKey);
    }
}

void GifLibrary::onEntryChanged(const QString& key)
{
    if (!usable() || !m_thumbWaits.contains(key) || !m_core)
        return;
    const MediaEntry* entry = m_core->entry(key);
    if (!entry || (entry->state != MediaState::Ready && entry->previewState != MediaState::Ready))
        return;
    const QString id = m_thumbWaits.take(key);
    fillFromEntry(id, key);
}

bool GifLibrary::saveFile(const QString& entryKey, QString* message)
{
    const auto fail = [message](const QString& text) {
        if (message)
            *message = text;
        return false;
    };
    if (!usable())
        return fail(i18n::t("Couldn't save the GIF: %1").arg(i18n::t("Your GIF list can't be changed right now.")));
    const MediaEntry* entry = m_core ? m_core->entry(entryKey) : nullptr;
    if (!entry || entry->state != MediaState::Ready || !QFileInfo(entry->localPath).isFile())
        return fail(i18n::t("Couldn't save the GIF: %1").arg(i18n::t("Download it first.")));
    if (QFileInfo(entry->localPath).size() > m_store.limits().maxCopyBytes)
        return fail(gifs::tooBigText());
    if (const gifs::Item* existing = m_store.findFile(entry->link.sha256)) {
        touch(existing->id);
        return fail(i18n::t("Already in your GIFs"));
    }
    // Someone else's file name: cleaned (no bidi or control characters), its extension kept (design_gifs 2.2).
    const QString name = gifs::itemName(displayNameFor(entry->link));
    // The link's sha when the bytes were checked against it; otherwise the copy is hashed.
    const QByteArray sha = entry->check.verified ? entry->link.sha256 : QByteArray();
    startCopy(CopyPurpose::SaveFile, m_store.newId(), entry->localPath, sha, name, entry->link.spoiler, entry->link.width, entry->link.height);
    if (message)
        message->clear();
    return true;
}

// ---- sending --------------------------------------------------------------------------------------------------------

gifs::ChatFingerprint GifLibrary::fingerprintOf(const ChatTarget& t)
{
    gifs::ChatFingerprint f;
    if (!t.sch || !ts3::isConnected(t.sch))
        return f;
    f.sch       = t.sch;
    f.serverUid = ts3::serverUid(t.sch);
    f.mode      = t.mode;
    f.clientUid = t.mode == TextMessageTarget_CLIENT ? t.clientUid : QString();
    f.channelId = ts3::ownChannel(t.sch);
    f.valid     = true;
    return f;
}

QString GifLibrary::refusalKey(const QString& serverUid, uint64 channelId) const
{
    return serverUid + QLatin1Char('/') + QString::number(channelId);
}

gifs::SendState GifLibrary::stateFor(const ChatTarget& target, bool recipientKnown, const QString& destination) const
{
    gifs::SendState s;
    s.destination = destination;
    s.serverChat  = target.mode == TextMessageTarget_SERVER;
    s.chat        = fingerprintOf(target);
    if (!recipientKnown) {
        s.blocked = noRecipientText();
        return s;
    }
    if (!target.sch || !ts3::isConnected(target.sch)) {
        s.blocked = notConnectedText();
        return s;
    }
    s.canText                 = true;
    const QString   serverUid = target.serverUid.isEmpty() ? ts3::serverUid(target.sch) : target.serverUid;
    const Settings  settings  = Settings::instance().forServer(serverUid);
    s.uploadLimit             = static_cast<qint64>(qMax(0, settings.uploadMaxMB)) << 20;
    s.uploadLimitText         = uploadLimitTextFor(serverUid);
    const uint64 channel      = ts3::ownChannel(target.sch);
    if (ts3::channelHasPassword(target.sch, channel)) {
        s.filesBlocked = uploadErrorText(MediaError::Password);
        return s;
    }
    const QString refused = m_refused.value(refusalKey(serverUid, channel));
    if (!refused.isEmpty()) {
        s.filesBlocked = refused;
        return s;
    }
    s.canFiles = true;
    return s;
}

bool GifLibrary::send(const QString& id, const ChatTarget& target, const gifs::SendState& state, const QString& replyLead, QString* error)
{
    const auto fail = [error](const QString& text) {
        if (error)
            *error = text;
        return false;
    };
    if (m_down || !m_core)
        return fail(notConnectedText());
    const gifs::Item* found = m_store.find(id);
    if (!found)
        return fail(QString());
    const gifs::Item item = *found;
    const QString    block = blockOf(item, state);
    if (!block.isEmpty())
        return fail(block);
    const uint64 sch  = target.sch ? target.sch : ts3::currentConnection();
    const qint64 now  = m_clock.elapsed();
    const auto   last = m_lastSend.constFind(sch);
    if ((last != m_lastSend.constEnd() && now - last.value() < kSendSpacingMs) || m_core->queuedTextPosts(sch) >= kMaxQueuedText)
        return fail(gifs::waitText());
    m_lastSend.insert(sch, now);
    QString reason;
    if (item.source == gifs::Source::Link) {
        const QString text = replyLead.isEmpty() ? gifs::linkText(item.url) : replyLead + QLatin1Char('\n') + gifs::linkText(item.url);
        if (m_core->postText(target, text, &reason) <= 0)
            return fail(reason);
    } else {
        SendRequest request;
        request.target    = target;
        request.replyLead = replyLead;
        SendItem file;
        file.path        = copyPath(item);
        file.displayName = item.name;
        file.remoteName  = gifs::remoteNameFor(item.name, item.file); // the copy's real format, whatever the name says
        file.spoiler     = item.spoiler; // a covered item goes out as a spoiler again
        request.items    = {file};
        const int batch  = m_core->send(request);
        if (batch <= 0)
            return fail(i18n::t("Try again."));
        const QString serverUid = target.serverUid.isEmpty() ? ts3::serverUid(sch) : target.serverUid;
        m_sentBatches.insert(batch, refusalKey(serverUid, ts3::ownChannel(sch)));
    }
    // Telegram reorders at the click (the tab's order shows it at the next opening).
    if (m_store.touch(id, nowSec()))
        markChanged();
    return true;
}

void GifLibrary::onUploadChanged(int id)
{
    if (m_down || !m_core)
        return;
    const UploadJob* job = m_core->upload(id);
    if (!job || !m_sentBatches.contains(job->batch))
        return;
    if (job->state == UploadState::Failed && (job->failure == MediaError::Permission || job->failure == MediaError::Quota))
        m_refused.insert(m_sentBatches.value(job->batch), job->message);
    if (job->state == UploadState::Done || job->state == UploadState::Failed || job->state == UploadState::Canceled)
        m_sentBatches.remove(job->batch);
}

gifs::Host GifLibrary::makeHost(const gifs::HostEnv& env)
{
    gifs::Host host;
    host.library  = this;
    host.chatTabs = env.chatTabs;
    const QPointer<GifLibrary> self(this);
    host.state = [self, env]() -> gifs::SendState {
        ChatTarget target;
        const bool known = env.resolve ? env.resolve(&target) : false;
        if (!self)
            return {};
        return self->stateFor(target, known, env.describe ? env.describe(target) : QString());
    };
    host.send = [self, env](const QString& id, const gifs::ChatFingerprint& opened, QString* error) -> bool {
        if (!self)
            return false;
        ChatTarget target;
        const bool            known = env.resolve ? env.resolve(&target) : false;
        const gifs::SendState state = self->stateFor(target, known, QString());
        if (state.chat != opened) {
            if (error)
                *error = gifs::changedText(); // another chat, server or partner since the picker opened: nothing is sent
            return false;
        }
        const QString lead = env.replyLead ? env.replyLead(target) : QString();
        if (!self->send(id, target, state, lead, error))
            return false;
        if (!lead.isEmpty() && env.replySent)
            env.replySent(target);
        return true;
    };
    return host;
}

// ---- the tab's pictures -----------------------------------------------------------------------------------------

QImage GifLibrary::thumbnail(const QString& id, const QSize& devicePx, bool blurred)
{
    const gifs::Item* item = m_store.find(id);
    if (m_down || !item || item->thumb.isEmpty() || devicePx.isEmpty())
        return {};
    const QString key = id + QLatin1Char('|') + QString::number(devicePx.width()) + QLatin1Char('|') + QString::number(devicePx.height())
                        + (blurred ? latin("|b") : QString());
    if (const QImage* cached = m_thumbs.object(key))
        return *cached;
    if (m_decoding.contains(key))
        return {};
    m_decoding.insert(key);
    ++m_thumbDecodes;
    const QPointer<GifLibrary> self(this);
    const QString              path = thumbsDir() + QLatin1Char('/') + item->thumb;
    const int                  gen  = m_gen;
    ++m_pending;
    m_decode.start([self, path, devicePx, blurred, key, id, gen] {
        const QImage tile = decodeTile(path, devicePx, blurred);
        QMetaObject::invokeMethod(self.data(), [self, tile, key, id, gen] {
            if (!self)
                return;
            --self->m_pending;
            self->m_decoding.remove(key);
            if (self->m_down || gen != self->m_gen || tile.isNull() || !self->m_store.find(id))
                return;
            self->m_thumbs.insert(key, new QImage(tile), qMax<int>(1, static_cast<int>(tile.sizeInBytes())));
            emit self->thumbReady(id);
        }, Qt::QueuedConnection);
    });
    return {};
}

QString GifLibrary::animationSource(const QString& id, gifs::Kind* kind) const
{
    const gifs::Item* item = m_store.find(id);
    if (!item)
        return {};
    if (item->source == gifs::Source::File) {
        if (kind)
            *kind = item->kind;
        return copyExists(*item) ? copyPath(*item) : QString();
    }
    if (!item->anim.isEmpty()) {
        const QString path = filesDir() + QLatin1Char('/') + item->anim;
        if (QFileInfo(path).isFile()) {
            if (kind)
                *kind = item->anim.endsWith(QLatin1String(".mp4")) ? gifs::Kind::Loop : gifs::Kind::Gif;
            return path;
        }
    }
    if (item->plain)
        return {};
    const linkpreview::Target target = linkpreview::classifyCached(item->url);
    linkpreview::Result       result;
    if (!target.isValid() || !linkpreview::Fetcher::cached(target, linksDir(), &result) || !result.ok || result.localFile.isEmpty())
        return {};
    if (QFileInfo(result.localFile).size() > kMaxAnimFileBytes)
        return {};
    const linkpreview::Kind k = result.kind != linkpreview::Kind::None ? result.kind : target.kind;
    if (k == linkpreview::Kind::Loop) {
        if (kind)
            *kind = gifs::Kind::Loop;
        return result.localFile;
    }
    if (k == linkpreview::Kind::Gif || (k == linkpreview::Kind::Image && result.localFile.endsWith(QLatin1String(".webp")))) {
        if (kind)
            *kind = gifs::Kind::Gif;
        return result.localFile;
    }
    return {};
}

void GifLibrary::requestAnimation(const QString& id, const QSize& devicePx)
{
    if (m_down)
        return;
    if (m_animId == id && m_animSize == devicePx && (m_animDone || m_animCancel)) {
        if (m_animDone)
            emit animationReady(id); // the kept sequence: at once
        return;
    }
    if (m_animCancel)
        m_animCancel->store(true);
    m_animCancel = std::make_shared<std::atomic<bool>>(false);
    m_animId     = id;
    m_animSize   = devicePx;
    m_animDone   = false;
    m_animOk     = false;
    m_animFrames.clear();
    m_animDelays.clear();
    gifs::Kind    kind   = gifs::Kind::Gif;
    const QString source = animationSource(id, &kind);
    if (source.isEmpty() || kind == gifs::Kind::Loop) {
        m_animDone = true;
        emit animationReady(id);
        return;
    }
    const bool              web    = QDir::cleanPath(source).startsWith(QDir::cleanPath(linksDir()) + QLatin1Char('/'), Qt::CaseInsensitive);
    const safeimage::Source sk     = web ? safeimage::Source::Website : safeimage::Source::Peer;
    const QPointer<GifLibrary> self(this);
    const Cancel               cancel = m_animCancel;
    const int                  gen    = m_gen;
    ++m_animationJobs;
    ++m_pending;
    std::atomic<bool>* onWorker = &m_animationOnWorker;
    m_decode.start([self, source, sk, devicePx, cancel, id, gen, onWorker] {
        onWorker->store(QThread::currentThread() != QCoreApplication::instance()->thread());
        const AnimOut out = decodeAnimation(source, sk, devicePx, cancel);
        QMetaObject::invokeMethod(self.data(), [self, out, cancel, id, gen] {
            if (!self)
                return;
            --self->m_pending;
            if (self->m_down || gen != self->m_gen || canceled(cancel) || self->m_animCancel != cancel)
                return;
            self->m_animDone   = true;
            self->m_animOk     = out.ok;
            self->m_animFrames = out.frames;
            self->m_animDelays = out.delays;
            emit self->animationReady(id);
        }, Qt::QueuedConnection);
    });
}

bool GifLibrary::takeAnimation(const QString& id, QVector<QImage>* frames, QVector<int>* delaysMs)
{
    if (m_animId != id || !m_animDone || !m_animOk)
        return false;
    if (frames)
        *frames = m_animFrames;
    if (delaysMs)
        *delaysMs = m_animDelays;
    return true;
}

void GifLibrary::cancelAnimation()
{
    // A decode still running stops; a finished sequence stays kept (hovering it again starts at once).
    if (m_animCancel && !m_animDone) {
        m_animCancel->store(true);
        m_animCancel.reset();
        m_animId.clear();
    }
}

// ---- diagnostics --------------------------------------------------------------------------------------------------------

QStringList GifLibrary::diagnosticLines() const
{
    int links = 0, files = 0;
    for (const gifs::Item& item : m_store.items())
        (item.source == gifs::Source::Link ? links : files) += 1;
    qint64 thumbBytes = 0;
    for (const QFileInfo& entry : QDir(thumbsDir()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
        thumbBytes += gifs::isOurPattern(entry.fileName()) ? entry.size() : 0;
    QString stateText = latin("ok");
    if (state() == State::ReadOnly)
        stateText = latin("read-only");
    else if (state() == State::Unavailable)
        stateText = latin("unavailable");
    QStringList lines;
    lines << QString::fromLatin1("GIFs: %1 (links %2, files %3, hidden %4), copies %5, thumbnails %6")
                 .arg(m_store.count())
                 .arg(links)
                 .arg(files)
                 .arg(m_store.hiddenCount())
                 .arg(formatSize(static_cast<quint64>(m_store.copyBytes())), formatSize(static_cast<quint64>(thumbBytes)));
    lines << QString::fromLatin1("Remember sent: %1; state: %2; at load: %3 skipped, %4 re-keyed")
                 .arg(Settings::instance().rememberGifs ? latin("on") : latin("off"), stateText)
                 .arg(m_stats.dropped)
                 .arg(m_stats.rekeyed);
    if (QFileInfo::exists(m_folder + latin("/gifs.json.bad"))) {
        int          count = 0;
        const qint64 bytes = leftoverBytes(&count);
        lines << QString::fromLatin1("Unlisted files from an unreadable list: %1 (%2)").arg(count).arg(formatSize(static_cast<quint64>(bytes)));
    }
    return lines;
}
