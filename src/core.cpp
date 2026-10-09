#include "core.h"

#include <QAbstractButton>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <cmath>

#include "blurhash.h"
#include "i18n.h"
#include "settings.h"

#include <windows.h>
#undef PostMessage // clashes with OpType::PostMessage

namespace {

constexpr int     kMaxParallelDownloads = 3; // main files and previews together
constexpr int     kMaxParallelUploads   = 2; // main files; earlier files finish (and post) first
constexpr int     kMkdirTimeoutMs       = 4000;
constexpr int     kPostTimeoutMs        = 5000;
constexpr int     kJobLingerMs          = 30000;          // Done / Canceled jobs stay around so the toast can show them
constexpr int     kFailedJobLingerMs    = 30 * 60 * 1000; // Failed ones until dismissed; this only bounds jobs no one shows
constexpr int     kProbeThreads         = 2;
constexpr int     kProgressIntervalMs   = 250;
constexpr int     kCacheLimitDelayMs    = 1500;
constexpr int     kStartupCacheCheckMs  = 15000;
constexpr int     kStillCacheKB         = 64 * 1024;
constexpr qint64  kMaxDecodePixels      = 80LL * 1000 * 1000;
constexpr quint64 kMaxPreviewBytes      = 5ull * 1024 * 1024;
constexpr int     kMaxDimension         = 16384;
constexpr int     kMaxAspect            = 8;
constexpr int     kMaxBlurHashLength    = 120;
constexpr int     kBlurHashDecodeWidth  = 32;
constexpr int     kBlurHashMaxSide      = 2048; // it is a blur: larger placeholders only cost memory
constexpr int     kMaxRemotePathLength  = 512;
constexpr int     kMaxLocalNameLength   = 120;
constexpr qint64  kUseRefreshMs         = 5 * 60 * 1000;
constexpr int     kStillThreads         = 2;
constexpr qint64  kMaxStillPixels       = 3840LL * 2160; // stills are placeholders: a 4K screen at most
// Decodes cheap enough for the GUI thread whatever the file contains (decode time grows with the
// pixel count; JPEG decodes directly at a reduced size, so there it grows with the file size).
constexpr qint64  kSyncDecodePixels     = 4LL * 1000 * 1000;
constexpr qint64  kSyncJpegPixels       = 24LL * 1000 * 1000;
constexpr qint64  kSyncDecodeBytes      = 8LL * 1024 * 1024;
constexpr int     kResumeIntervalMs     = 2000;
constexpr int     kMaxResumeAttempts    = 3; // automatic restarts of a download without a success
constexpr int     kMaxUploadRenames     = 3; // new names after "file already exists"

Core* g_instance = nullptr;

QByteArray utf8Native(const QString& path)
{
    return QDir::toNativeSeparators(path).toUtf8();
}

const wchar_t* wide(const QString& nativePath)
{
    return reinterpret_cast<const wchar_t*>(nativePath.utf16());
}

QString joinRemote(const QString& dir, const QString& name)
{
    return dir == QLatin1String("/") ? dir + name : dir + QLatin1Char('/') + name;
}

QString normalizeRemoteDir(QString dir)
{
    dir.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!dir.startsWith(QLatin1Char('/')))
        dir.prepend(QLatin1Char('/'));
    while (dir.length() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    return dir;
}

quint64 megabytes(int mb)
{
    return static_cast<quint64>(qMax(0, mb)) * 1024 * 1024;
}

QString chopTrailingDotsAndSpaces(QString name)
{
    while (!name.isEmpty() && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))))
        name.chop(1);
    return name;
}

bool isRiskyToOpen(const QString& fileName)
{
    static const QStringList risky = {
        QStringLiteral("exe"), QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("com"), QStringLiteral("msi"), QStringLiteral("scr"),
        QStringLiteral("ps1"), QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("js"),  QStringLiteral("jse"), QStringLiteral("wsf"),
        QStringLiteral("hta"), QStringLiteral("lnk"), QStringLiteral("jar"), QStringLiteral("dll"), QStringLiteral("reg"), QStringLiteral("pif"),
        QStringLiteral("cpl"), QStringLiteral("msc"), QStringLiteral("url"), QStringLiteral("ts3_plugin"),
        // More things Windows runs on double-click.
        QStringLiteral("psm1"), QStringLiteral("wsh"), QStringLiteral("wsc"), QStringLiteral("ws"), QStringLiteral("msp"), QStringLiteral("mst"),
        QStringLiteral("chm"), QStringLiteral("hlp"), QStringLiteral("inf"), QStringLiteral("ins"), QStringLiteral("sct"), QStringLiteral("shb"),
        QStringLiteral("shs"), QStringLiteral("scf"), QStringLiteral("gadget"), QStringLiteral("application"), QStringLiteral("appref-ms"),
        QStringLiteral("settingcontent-ms")};
    // Windows ignores trailing dots and spaces: "x.exe." opens x.exe.
    return risky.contains(QFileInfo(chopTrailingDotsAndSpaces(fileName)).suffix().toLower());
}

void revealInExplorer(const QString& path)
{
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
}

// The cache is evicted least-recently-used by modification time; this marks a file as used now.
// FILE_WRITE_ATTRIBUTES works even while a player or decoder has the file open.
void refreshFileTime(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
    HANDLE        file   = CreateFileW(wide(native), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(file, nullptr, nullptr, &now);
    CloseHandle(file);
}

// Instant, no extra disk space; fails across volumes and on file systems without hard links.
bool hardLink(const QString& from, const QString& to)
{
    const QString target = QDir::toNativeSeparators(to);
    const QString source = QDir::toNativeSeparators(from);
    return CreateHardLinkW(wide(target), wide(source), nullptr) != FALSE;
}

// Hard link when possible, otherwise a copy. The target must not exist.
bool linkOrCopy(const QString& from, const QString& to)
{
    return hardLink(from, to) || QFile::copy(from, to);
}

DWORD CALLBACK stagingCopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
{
    return static_cast<const std::atomic<bool>*>(data)->load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// Copies a file to be uploaded into the staging folder (worker thread). Stops early once *cancel is
// set (upload canceled, plugin unloading). The copy is ours and never read-only, so removing the
// staging folder never has to change attributes.
bool copyForStaging(const QString& from, const QString& to, std::atomic<bool>* cancel)
{
    const QString source = QDir::toNativeSeparators(from);
    const QString target = QDir::toNativeSeparators(to);
    if (!CopyFileExW(wide(source), wide(target), stagingCopyProgress, cancel, nullptr, COPY_FILE_FAIL_IF_EXISTS))
        return false;
    const DWORD attributes = GetFileAttributesW(wide(target));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(wide(target), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    return !cancel->load();
}

// Random part of uploaded file names (32 bits). Uploads never overwrite: in the rare case the name
// is taken anyway, the file gets a new one (Core::renameUpload).
QString uniqueSuffix()
{
    return QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

QString nameTakenText()
{
    return i18n::t("A file with this name already exists on the server. Try again.");
}

QString fileChangedText()
{
    return i18n::t("The file on the server was replaced after it was sent.");
}

QString uploadLimitText(int limitMB)
{
    return i18n::t("This file is larger than your %1 MB upload limit. You can raise the limit in Settings → Sending.").arg(limitMB);
}

// Transfer failures caused by the connection rather than by the file: restarted automatically.
bool isInterruption(unsigned int status)
{
    switch (status) {
    case ERROR_not_connected:
    case ERROR_connection_lost:
    case ERROR_file_connection_lost:
    case ERROR_file_transfer_interrupted:
        return true;
    default:
        return false;
    }
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

// A name from a chat link made safe for the local file system: no reserved characters (':' would
// even create an NTFS stream), no trailing dots/spaces, bounded length.
QString localFileName(const QString& name)
{
    static const QString reserved = QStringLiteral("<>:\"/\\|?*");
    QString              result;
    result.reserve(name.size());
    for (const QChar c : name)
        result += (c.unicode() < 0x20 || reserved.contains(c)) ? QChar(QLatin1Char('_')) : c;
    result = chopTrailingDotsAndSpaces(result);
    if (result.length() > kMaxLocalNameLength) {
        const QString ext = QFileInfo(result).suffix().left(16);
        result = ext.isEmpty() ? result.left(kMaxLocalNameLength) : result.left(kMaxLocalNameLength - ext.length() - 1) + QLatin1Char('.') + ext;
    }
    return result.isEmpty() ? QStringLiteral("file") : result;
}

// Comparable form of a local path (Windows paths are case-insensitive).
QString pathKey(const QString& path)
{
    return QDir::cleanPath(path).toLower();
}

// The normalised remote preview path, or an empty string if it is not acceptable. Chat links are
// untrusted: the path must be absolute, without "." / ".." segments and not the main file itself.
QString safePreviewPath(const QString& pv, const QString& mainFile)
{
    if (pv.length() > kMaxRemotePathLength || !pv.startsWith(QLatin1Char('/')) || pv.endsWith(QLatin1Char('/')) || pv.contains(QLatin1Char('\\')))
        return {};
    const QStringList parts = pv.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    for (const QString& part : parts) {
        if (chopTrailingDotsAndSpaces(part).isEmpty()) // ".", "..", ". ." ...
            return {};
        for (const QChar c : part) {
            if (c.unicode() < 0x20)
                return {};
        }
    }
    const QString normalized = QLatin1Char('/') + parts.join(QLatin1Char('/'));
    if (normalized.compare(mainFile, Qt::CaseInsensitive) == 0)
        return {};
    return normalized;
}

MediaLink sanitized(MediaLink link)
{
    if (link.width > 0 && link.height > 0) {
        int w = qMin(link.width, kMaxDimension);
        int h = qMin(link.height, kMaxDimension);
        if (w > h * kMaxAspect)
            h = (w + kMaxAspect - 1) / kMaxAspect;
        else if (h > w * kMaxAspect)
            w = (h + kMaxAspect - 1) / kMaxAspect;
        link.width  = w;
        link.height = h;
    } else {
        link.width  = 0;
        link.height = 0;
    }
    link.durationMs = qMax<qint64>(0, link.durationMs);
    if (!link.blurHash.isEmpty() && (link.blurHash.length() > kMaxBlurHashLength || !blurhash::isValid(link.blurHash)))
        link.blurHash.clear();
    if (!link.previewFile.isEmpty())
        link.previewFile = safePreviewPath(link.previewFile, link.remoteFile());
    link.dropInvalidMetadata(); // 2.2 fields: after the preview check (ph needs a valid pv)
    return link;
}

QSize fitInto(const QSize& size, const QSize& bounds)
{
    if (size.isEmpty() || bounds.isEmpty())
        return {};
    return size.scaled(bounds, Qt::KeepAspectRatio).boundedTo(size).expandedTo(QSize(1, 1));
}

// Decodes an image file (first frame for animations) scaled to fit maxPixels (and kMaxStillPixels),
// never upscaled. Refuses files whose size is unknown before decoding or above kMaxDecodePixels.
// With onlyIfCheap, a decode that may take long (see kSyncDecodePixels) is not attempted: *expensive
// is set instead, so the caller can hand the file to a worker thread.
QImage decodeScaled(const QString& path, const QSize& maxPixels, bool onlyIfCheap = false, bool* expensive = nullptr)
{
    if (expensive)
        *expensive = false;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize  raw    = reader.size();
    const qint64 pixels = static_cast<qint64>(raw.width()) * raw.height();
    if (!raw.isValid() || raw.isEmpty() || pixels > kMaxDecodePixels)
        return {};
    if (onlyIfCheap) {
        const QByteArray format = reader.format().toLower();
        const bool       jpeg   = format == "jpeg" || format == "jpg";
        if (QFileInfo(path).size() > kSyncDecodeBytes || pixels > (jpeg ? kSyncJpegPixels : kSyncDecodePixels)) {
            if (expensive)
                *expensive = true;
            return {};
        }
    }

    // The scaled size applies before the EXIF rotation.
    const bool  rotated = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    const QSize shown   = rotated ? raw.transposed() : raw;
    QSize       target  = fitInto(shown, maxPixels);
    if (static_cast<qint64>(target.width()) * target.height() > kMaxStillPixels) {
        const double factor = std::sqrt(static_cast<double>(kMaxStillPixels) / (static_cast<double>(target.width()) * target.height()));
        target              = QSize(qMax(1, static_cast<int>(target.width() * factor)), qMax(1, static_cast<int>(target.height() * factor)));
    }
    if (target.isEmpty())
        return {};
    if (target != shown)
        reader.setScaledSize(rotated ? target.transposed() : target);

    QImage image = reader.read();
    if (image.isNull())
        return {};
    if (image.size() != target)
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// BlurHash placeholder at the link's aspect ratio: decoded small, smoothly scaled up.
QImage decodeBlurHash(const MediaLink& link, const QSize& maxPixels)
{
    const bool  hasSize = link.width > 0 && link.height > 0;
    const QSize aspect  = hasSize ? QSize(link.width, link.height) : QSize(4, 3);
    const int   height  = qBound(1, qRound(kBlurHashDecodeWidth * static_cast<double>(aspect.height()) / aspect.width()), kBlurHashDecodeWidth * kMaxAspect);
    const QImage tiny   = blurhash::decode(link.blurHash, QSize(kBlurHashDecodeWidth, height));
    if (tiny.isNull())
        return {};

    QSize target = hasSize ? fitInto(aspect, maxPixels) : aspect.scaled(maxPixels, Qt::KeepAspectRatio);
    if (target.width() > kBlurHashMaxSide || target.height() > kBlurHashMaxSide)
        target.scale(kBlurHashMaxSide, kBlurHashMaxSide, Qt::KeepAspectRatio);
    if (target.isEmpty())
        return {};
    return tiny.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// The client may place a download directly in the target folder or mirror the remote path below it.
QString findDownloaded(const QString& dir, const QString& fileName)
{
    QString      found;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidate = it.next();
        if (found.isEmpty() || QFileInfo(candidate).fileName() == fileName)
            found = candidate;
    }
    return found;
}

// The client lib reports a download as complete while it may still be writing the (preallocated) file:
// true while anyone else has it open for writing.
bool isOpenForWriting(const QString& path)
{
    const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

// A cached file taken before the client lib had finished writing it (versions up to 2.0.4) is the
// right size but ends in zeros. Images never end in 4 KB of zeros (PNG ends with IEND, JPEG with EOI,
// GIF with a trailer, WebP is length-prefixed); for videos a 64 KB run is required.
bool hasUnwrittenTail(const QString& path, const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    static const QStringList images = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"),
                                       QStringLiteral("gif"), QStringLiteral("webp")};
    static const QStringList videos = {QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mkv")};
    const qint64 run = images.contains(ext) ? 4096 : videos.contains(ext) ? 65536 : 0;
    if (run == 0)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 2 * run || !file.seek(file.size() - run))
        return false;
    const QByteArray tail = file.read(run);
    return tail.size() == run && tail.count('\0') == run;
}

// Previews are staged next to (not inside) the main staging folder so names can never collide.
QString previewStagingDir(const QString& stagingDir)
{
    return stagingDir + QStringLiteral(".pv");
}

void removeStaging(const QString& stagingDir)
{
    if (stagingDir.isEmpty())
        return;
    QDir(stagingDir).removeRecursively();
    QDir(previewStagingDir(stagingDir)).removeRecursively();
}

void removeEmptyDirectories(const QString& root)
{
    QStringList  dirs;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        dirs.append(it.next());
    // Deepest first so parents are empty by the time they are tried; rmdir() keeps non-empty ones.
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.length() > b.length(); });
    for (const QString& dir : qAsConst(dirs))
        QDir().rmdir(dir);
}

} // namespace

Core::Core(QObject* parent)
    : QObject(parent)
    , m_stills(kStillCacheKB) // cost is in KB
{
    g_instance = this;
    m_pool.setMaxThreadCount(kProbeThreads);
    m_stillPool.setMaxThreadCount(kStillThreads);
    m_clock.start();
}

Core::~Core()
{
    // Workers only read files and post their result back; stop running staging copies, then join them.
    for (const auto& cancel : qAsConst(m_stagingCancel))
        cancel->store(true);
    m_pool.clear();
    m_pool.waitForDone();
    m_stillPool.clear();
    m_stillPool.waitForDone();

    // Nothing may keep writing into (or reading from) our folders once the plugin is gone.
    for (auto it = m_downloadsByTransfer.cbegin(); it != m_downloadsByTransfer.cend(); ++it) {
        if (const MediaEntry* e = entry(it.value()))
            haltDownload(e->sch, it.key());
    }
    for (auto it = m_previewsByTransfer.cbegin(); it != m_previewsByTransfer.cend(); ++it)
        haltDownload(m_previewSch.value(it.value()), it.key());
    for (auto& job : m_uploads) {
        if (job.transferActive && ts3::funcs.haltTransfer)
            ts3::funcs.haltTransfer(job.target.sch, job.transferId, 1, nullptr);
        job.transferActive = false;
    }

    for (const QString& dir : qAsConst(m_probing))
        removeStaging(dir);
    m_probing.clear();
    m_stagingCancel.clear();
    for (auto& job : m_uploads) {
        cleanupUpload(job);
        releaseSource(job); // pasted images kept for a retry
    }
    for (const QString& path : qAsConst(m_deleteAfterProbe))
        QFile::remove(path);
    m_deleteAfterProbe.clear();

    if (g_instance == this)
        g_instance = nullptr;
}

Core* Core::instance()
{
    return g_instance;
}

void Core::start()
{
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(kProgressIntervalMs);
    connect(m_progressTimer, &QTimer::timeout, this, &Core::updateProgress);

    m_cacheTimer = new QTimer(this);
    m_cacheTimer->setSingleShot(true);
    m_cacheTimer->setInterval(kCacheLimitDelayMs);
    connect(m_cacheTimer, &QTimer::timeout, this, &Core::enforceCacheLimit);

    // Runs while downloads wait for their server to be reachable again (see noteInterrupted).
    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setInterval(kResumeIntervalMs);
    connect(m_resumeTimer, &QTimer::timeout, this, &Core::resumeInterrupted);
    if (!m_resume.isEmpty())
        m_resumeTimer->start();

    // 2.2: the next chat post, when a connection's FloodGovernor allows it.
    m_postTimer = new QTimer(this);
    m_postTimer->setSingleShot(true);
    connect(m_postTimer, &QTimer::timeout, this, &Core::pumpPosts);

    // Leftovers from a previous session (crash, client closed mid-transfer).
    QDir(cacheDir() + QStringLiteral("/.partial")).removeRecursively();
    QDir(ts3::dataDir() + QStringLiteral("/upload")).removeRecursively();
    QDir(ts3::dataDir() + QStringLiteral("/paste")).removeRecursively();

    // The limit may have been lowered while TeamSpeak was closed.
    QTimer::singleShot(kStartupCacheCheckMs, this, [this] { enforceCacheLimit(); });
}

// ============================================================================================
// Received media
// ============================================================================================

const MediaEntry* Core::entry(const QString& key) const
{
    auto it = m_entries.constFind(key);
    return it == m_entries.constEnd() ? nullptr : &it.value();
}

QStringList Core::keys() const
{
    return m_order;
}

QString Core::cacheDir() const
{
    const QString dir = ts3::dataDir() + QStringLiteral("/cache");
    QDir().mkpath(dir);
    return dir;
}

QString Core::cachePathFor(const MediaLink& link) const
{
    const QString server = QString::fromLatin1(QCryptographicHash::hash(link.serverUid.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    return cacheDir() + QLatin1Char('/') + server + QLatin1Char('/') + QString::number(link.channelId) + QLatin1Char('/') + link.key().left(8) + QLatin1Char('_')
           + localFileName(link.fileName);
}

QString Core::partialDir(const QString& key, bool preview) const
{
    return cacheDir() + QStringLiteral("/.partial/") + key + (preview ? QStringLiteral(".pv") : QString());
}

void Core::touch(MediaEntry& e)
{
    ++e.revision;
    emit entryChanged(e.link.key());
}

void Core::ensure(const MediaLink& rawLink)
{
    if (!rawLink.isValid())
        return;

    const MediaLink link = sanitized(rawLink);
    const QString   key  = link.key();
    if (m_entries.contains(key)) {
        // Seen while inline previews were off, or its files left the cache (cleared / evicted) while
        // it is still in a chat: start what would start for a new link.
        if (Settings::instance().inlinePreviews) {
            const bool pending = m_autoPending.remove(key);
            const bool rearm   = m_rearm.remove(key);
            if (pending || rearm)
                startAutoDownloads(key);
        }
        return;
    }

    MediaEntry e;
    e.link      = link;
    e.kind      = kindForFileName(link.fileName);
    e.localPath = cachePathFor(link);

    const QFileInfo cached(e.localPath);
    if (cached.isFile() && (link.size == 0 || static_cast<quint64>(cached.size()) == link.size)) {
        if (hasUnwrittenTail(e.localPath, link.fileName)) {
            ts3::log(QStringLiteral("Discarding damaged cached copy of %1; it is downloaded again").arg(link.remoteFile()), LogLevel_WARNING);
            QFile::remove(e.localPath);
        } else {
            e.state    = MediaState::Ready;
            e.progress = 1.0;
        }
    }

    const MediaLink preview = link.previewLink();
    if (!link.previewFile.isEmpty() && preview.isValid()) {
        e.previewPath = cachePathFor(preview);
        const QFileInfo pf(e.previewPath);
        if (pf.isFile() && hasUnwrittenTail(e.previewPath, e.previewPath))
            QFile::remove(e.previewPath);
        else if (pf.isFile() && static_cast<quint64>(pf.size()) <= kMaxPreviewBytes)
            e.previewState = MediaState::Ready;
    }

    m_entries.insert(key, e);
    m_order.append(key);

    if (!Settings::instance().inlinePreviews) {
        m_autoPending.insert(key);
        return;
    }
    startAutoDownloads(key);
}

void Core::startAutoDownloads(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry&     e          = it.value();
    const Settings& s          = Settings::instance();
    const bool      isImage    = isPreviewableImage(e.kind);
    const bool      isVideo    = e.kind == MediaKind::Video;
    const quint64   imageLimit = megabytes(s.autoDownloadMaxMB);
    const quint64   videoLimit = megabytes(s.videoAutoDownloadMB);
    if (isImage)
        e.tooLargeForAuto = e.link.size > imageLimit;
    else if (isVideo)
        e.tooLargeForAuto = videoLimit > 0 && e.link.size > videoLimit;

    // Images that are already cached do not need their preview; video posters are always useful.
    const bool wantPreview = e.previewState == MediaState::Idle && !e.previewPath.isEmpty() && (e.state != MediaState::Ready || !isImage);
    bool       wantMain    = false;
    if (e.state == MediaState::Idle) {
        if (isImage)
            wantMain = s.autoDownloadImages && !e.tooLargeForAuto;
        else if (isVideo)
            wantMain = videoLimit > 0 && e.link.size <= videoLimit;
    }

    // Previews first: they are what the chat shows until the real file is there.
    if (wantPreview)
        startPreviewDownload(key);
    if (wantMain) {
        m_autoDownloads.insert(key);
        startDownload(key);
    }
}

void Core::download(const QString& key, bool openWhenReady)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry& e = it.value();
    m_autoDownloads.remove(key); // the user asked for it: no automatic size cut-off

    // A video shows its poster whenever it is not playing: fetch it again if it is missing
    // (cache cleared / evicted, or its download failed).
    if (e.kind == MediaKind::Video && (e.previewState == MediaState::Idle || e.previewState == MediaState::Failed))
        startPreviewDownload(key);

    switch (e.state) {
    case MediaState::Ready:
        if (QFileInfo(e.localPath).isFile()) {
            refreshFileTime(e.localPath);
            if (openWhenReady)
                emit openRequested(key);
            return;
        }
        // Deleted behind our back (cache folder cleaned by hand): fetch it again.
        e.state    = MediaState::Idle;
        e.progress = 0.0;
        invalidateStill(key, MediaStill::Full);
        break;
    case MediaState::Queued:
        // Explicit requests jump the queue.
        m_downloadQueue.removeAll(key);
        m_downloadQueue.prepend(key);
        e.openWhenReady = e.openWhenReady || openWhenReady;
        return;
    case MediaState::Downloading:
        e.openWhenReady = e.openWhenReady || openWhenReady;
        return;
    case MediaState::Failed:
        if (e.error == MediaError::NotFound)
            return;
        break;
    case MediaState::Idle:
        break;
    }

    e.openWhenReady = openWhenReady;
    startDownload(key);
}

void Core::retry(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (it->state == MediaState::Failed && it->error == MediaError::NotFound)
        return;

    // A missing preview (failed, or removed from the cache) comes back too.
    const bool wantPreview  = !it->previewPath.isEmpty() && (it->state != MediaState::Ready || !isPreviewableImage(it->kind));
    const bool retryPreview = wantPreview && (it->previewState == MediaState::Failed || it->previewState == MediaState::Idle);
    const bool retryMain    = it->state == MediaState::Failed;
    if (retryPreview)
        startPreviewDownload(key);
    if (retryMain) {
        m_autoDownloads.remove(key);
        startDownload(key);
    }
}

void Core::startDownload(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry& e = it.value();
    if (e.state == MediaState::Downloading || e.state == MediaState::Queued)
        return;
    if (e.state == MediaState::Failed)
        e.state = MediaState::Idle; // so a new failure is reported

    // Link sizes are untrusted. When the real size of this file on the server is already known and
    // differs (the file was replaced, or the link has a made-up size, e.g. to fetch one file many
    // times under different keys), nothing is fetched automatically: it waits for a click, and an
    // explicit download then checks the size again.
    if (m_autoDownloads.contains(key) && e.link.size != 0) {
        const auto known = m_remoteSizes.constFind(remoteId(e.link));
        if (known != m_remoteSizes.constEnd() && known.value() != e.link.size) {
            m_autoDownloads.remove(key);
            e.progress = 0.0;
            touch(e);
            return;
        }
    }

    const uint64 sch = ts3::connectionForServerUid(e.link.serverUid);
    if (!sch) {
        failDownload(key, MediaError::NotConnected, {});
        return;
    }

    e.sch   = sch;
    e.error = MediaError::None;
    e.errorText.clear();
    e.progress = 0.0;

    if (m_activeDownloads >= kMaxParallelDownloads) {
        e.state = MediaState::Queued;
        m_downloadQueue.append(key);
        touch(e);
        return;
    }

    const QString partial = partialDir(key, false);
    QDir(partial).removeRecursively();
    QDir().mkpath(partial);

    const QString  rc  = registerOp(OpType::Download, key, 0);
    anyID          tid = 0;
    const unsigned err = ts3::funcs.requestFile(sch, e.link.channelId, "", e.link.remoteFile().toUtf8().constData(), 1, 0, utf8Native(partial).constData(), &tid,
                                                rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        failDownload(key, mapError(err), ts3::errorText(err));
        return;
    }
    setOpTransfer(rc, tid);

    ++m_activeDownloads;
    e.state      = MediaState::Downloading;
    e.transferId = tid;
    m_downloadsByTransfer.insert(tid, key);
    ensureProgressTimer();
    touch(e);
}

void Core::startPreviewDownload(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry& e = it.value();
    if (e.previewPath.isEmpty() || e.previewState == MediaState::Downloading || e.previewState == MediaState::Queued || e.previewState == MediaState::Ready)
        return;
    const MediaLink preview = e.link.previewLink();
    if (!preview.isValid())
        return;
    e.previewState = MediaState::Idle;

    // Links that differ only in size (or messages sharing a preview) use the same preview file:
    // fetched once.
    const QFileInfo cached(e.previewPath);
    if (cached.isFile() && cached.size() > 0 && static_cast<quint64>(cached.size()) <= kMaxPreviewBytes) {
        e.previewState = MediaState::Ready;
        invalidateStill(key, MediaStill::Preview);
        touch(e);
        return;
    }

    const uint64 sch = ts3::connectionForServerUid(e.link.serverUid);
    if (!sch) {
        failPreviewDownload(key, QStringLiteral("not connected"), true);
        return;
    }
    // Its own connection: the main file may be transferred over another tab to the same server.
    m_previewSch.insert(key, sch);

    if (m_activeDownloads >= kMaxParallelDownloads) {
        e.previewState = MediaState::Queued;
        m_previewQueue.append(key);
        touch(e);
        return;
    }

    const QString partial = partialDir(key, true);
    QDir(partial).removeRecursively();
    QDir().mkpath(partial);

    const QString  rc  = registerOp(OpType::Download, key, 0, true);
    anyID          tid = 0;
    const unsigned err = ts3::funcs.requestFile(sch, e.link.channelId, "", preview.remoteFile().toUtf8().constData(), 1, 0, utf8Native(partial).constData(), &tid,
                                                rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        failPreviewDownload(key, ts3::errorText(err), mapError(err) == MediaError::NotConnected);
        return;
    }
    setOpTransfer(rc, tid);

    ++m_activeDownloads;
    e.previewState      = MediaState::Downloading;
    e.previewTransferId = tid;
    m_previewsByTransfer.insert(tid, key);
    ensureProgressTimer();
    touch(e);
}

void Core::pumpDownloadQueue()
{
    while (m_activeDownloads < kMaxParallelDownloads && !m_previewQueue.isEmpty()) {
        const QString key = m_previewQueue.takeFirst();
        auto          it  = m_entries.find(key);
        if (it == m_entries.end() || it->previewState != MediaState::Queued)
            continue;
        it->previewState = MediaState::Idle;
        startPreviewDownload(key);
    }
    while (m_activeDownloads < kMaxParallelDownloads && !m_downloadQueue.isEmpty()) {
        const QString key = m_downloadQueue.takeFirst();
        auto          it  = m_entries.find(key);
        if (it == m_entries.end() || it->state != MediaState::Queued)
            continue;
        it->state = MediaState::Idle;
        startDownload(key);
    }
}

void Core::finishDownload(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->state != MediaState::Downloading)
        return;
    MediaEntry& e = it.value();

    if (m_downloadsByTransfer.value(e.transferId) == key)
        m_downloadsByTransfer.remove(e.transferId);
    --m_activeDownloads;
    e.transferId       = 0;
    const bool wasAuto = m_autoDownloads.remove(key);

    const QString partial = partialDir(key, false);
    const QString found   = findDownloaded(partial, e.link.fileName);
    if (!found.isEmpty()) {
        // The progress timer checks the real size too, but a fast transfer can finish between two
        // ticks (or before the size is known): check what actually arrived.
        const quint64 actual = static_cast<quint64>(QFileInfo(found).size());
        noteRemoteSize(e.link, actual);
        if (e.link.size != 0 && actual != e.link.size) {
            // Not the file this message refers to (replaced on the server, or a made-up size): never
            // cache it under this message's key.
            QDir(partial).removeRecursively();
            e.state = MediaState::Idle; // so failDownload's guard does not skip it
            failDownload(key, MediaError::NotFound, fileChangedText(), false, true);
            return;
        }
        if (wasAuto && actual > autoDownloadLimit(e)) {
            // Larger than the auto-download limit (link of unknown size): wait for a click, like
            // abortOversizedAutoDownload.
            QDir(partial).removeRecursively();
            e.state           = MediaState::Idle;
            e.progress        = 0.0;
            e.tooLargeForAuto = true;
            e.openWhenReady   = false;
            ts3::log(QStringLiteral("Automatic download of %1 discarded: the file is larger than the auto-download limit").arg(e.link.remoteFile()), LogLevel_WARNING, e.sch);
            touch(e);
            pumpDownloadQueue();
            return;
        }
    }

    QString error;
    if (found.isEmpty()) {
        error = i18n::t("TeamSpeak finished the download, but the file is missing. Try again.");
    } else {
        QDir().mkpath(QFileInfo(e.localPath).absolutePath());
        QFile::remove(e.localPath);
        if (!QFile::rename(found, e.localPath) && !QFile::copy(found, e.localPath))
            error = i18n::t("Couldn't save the file to the media cache. Check free disk space.");
    }
    QDir(partial).removeRecursively();
    if (!error.isEmpty()) {
        e.state = MediaState::Idle; // so failDownload's guard does not skip it
        failDownload(key, MediaError::Other, error, false, true);
        return;
    }

    refreshFileTime(e.localPath);
    e.state         = MediaState::Ready;
    e.progress      = 1.0;
    const bool open = e.openWhenReady;
    e.openWhenReady = false;
    m_resumeAttempts.remove(key);
    ts3::log(QStringLiteral("Downloaded %1 (%2)").arg(e.link.remoteFile(), formatSize(e.link.size)), LogLevel_INFO, e.sch);
    invalidateStill(key, MediaStill::Full); // the preview stays on screen until the image is decoded
    touch(e);

    if (open)
        emit openRequested(key);
    pumpDownloadQueue();
    scheduleCacheLimit(key);
}

// Called for a transfer the client lib reported complete. It may still be flushing the file it
// preallocated to its full size; taking it now would cache a file whose end is zeros. Wait until no one
// has it open for writing (checked every 100 ms, at most 30 s), then finish.
void Core::finishWhenWritten(const QString& key, bool preview, int attempt)
{
    constexpr int kPollMs      = 100;
    constexpr int kMaxAttempts = 300;

    const MediaEntry* e = entry(key);
    if (!e || (preview ? e->previewState : e->state) != MediaState::Downloading)
        return;
    const QString name  = preview ? QFileInfo(e->previewPath).fileName() : e->link.fileName;
    const QString found = findDownloaded(partialDir(key, preview), name);
    if (!found.isEmpty() && isOpenForWriting(found) && attempt < kMaxAttempts) {
        QTimer::singleShot(kPollMs, this, [this, key, preview, attempt] { finishWhenWritten(key, preview, attempt + 1); });
        return;
    }
    if (attempt >= kMaxAttempts)
        ts3::log(QStringLiteral("%1 was still open for writing 30 s after its transfer completed").arg(name), LogLevel_WARNING);
    else if (attempt > 0)
        ts3::log(QStringLiteral("Waited %1 ms for TeamSpeak to finish writing %2").arg(attempt * kPollMs).arg(name));
    if (preview)
        finishPreviewDownload(key);
    else
        finishDownload(key);
}

void Core::finishPreviewDownload(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->previewState != MediaState::Downloading)
        return;
    MediaEntry& e = it.value();

    if (m_previewsByTransfer.value(e.previewTransferId) == key)
        m_previewsByTransfer.remove(e.previewTransferId);
    --m_activeDownloads;
    e.previewTransferId = 0;

    const QString partial = partialDir(key, true);
    const QString found   = findDownloaded(partial, QFileInfo(e.previewPath).fileName());
    QString       error;
    if (found.isEmpty()) {
        error = QStringLiteral("the downloaded preview is missing");
    } else if (static_cast<quint64>(QFileInfo(found).size()) > kMaxPreviewBytes) {
        // Finished before the progress timer could stop it; never decode it.
        error = QStringLiteral("the preview is larger than 5 MB");
    } else {
        QDir().mkpath(QFileInfo(e.previewPath).absolutePath());
        QFile::remove(e.previewPath);
        if (!QFile::rename(found, e.previewPath) && !QFile::copy(found, e.previewPath))
            error = QStringLiteral("could not write to the media cache");
    }
    QDir(partial).removeRecursively();
    if (!error.isEmpty()) {
        e.previewState = MediaState::Idle;
        failPreviewDownload(key, error);
        return;
    }

    refreshFileTime(e.previewPath);
    e.previewState = MediaState::Ready;
    m_resumeAttempts.remove(key);
    invalidateStill(key, MediaStill::Preview);
    touch(e);
    pumpDownloadQueue();
    scheduleCacheLimit(key);
}

// interrupted: the connection failed, not the file (restarted once the server is reachable again).
// text: TeamSpeak's message, quoted by downloadErrorText(); with exactText it is the full text instead.
void Core::failDownload(const QString& key, MediaError error, const QString& text, bool interrupted, bool exactText)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry& e = it.value();
    if (e.state == MediaState::Failed || e.state == MediaState::Ready)
        return;

    if (e.state == MediaState::Downloading) {
        if (m_downloadsByTransfer.value(e.transferId) == key)
            m_downloadsByTransfer.remove(e.transferId);
        --m_activeDownloads;
        QDir(partialDir(key, false)).removeRecursively();
    }
    m_downloadQueue.removeAll(key);
    const bool wasAuto = m_autoDownloads.remove(key);

    e.state         = MediaState::Failed;
    e.error         = error;
    e.errorText     = exactText ? text : downloadErrorText(error, text);
    e.openWhenReady = false;
    e.transferId    = 0;
    ts3::log(QStringLiteral("Download of %1 failed: %2").arg(e.link.remoteFile(), e.errorText), LogLevel_WARNING, e.sch);

    if (interrupted || error == MediaError::NotConnected) {
        Resume what;
        what.main         = true;
        what.mainExplicit = !wasAuto;
        noteInterrupted(key, what);
    } else if (auto r = m_resume.find(key); r != m_resume.end()) {
        // Failed for a reason a reconnect does not fix.
        r->main = false;
        if (!r->preview)
            m_resume.erase(r);
    }
    touch(e);
    pumpDownloadQueue();
}

void Core::failPreviewDownload(const QString& key, const QString& text, bool interrupted)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry& e = it.value();
    if (e.previewState == MediaState::Failed || e.previewState == MediaState::Ready)
        return;

    if (e.previewState == MediaState::Downloading) {
        if (m_previewsByTransfer.value(e.previewTransferId) == key)
            m_previewsByTransfer.remove(e.previewTransferId);
        --m_activeDownloads;
        QDir(partialDir(key, true)).removeRecursively();
    }
    m_previewQueue.removeAll(key);

    e.previewState      = MediaState::Failed;
    e.previewTransferId = 0;
    ts3::log(QStringLiteral("Preview %1 of %2 not loaded: %3").arg(e.link.previewFile, e.link.remoteFile(), text), LogLevel_WARNING, m_previewSch.value(key));

    if (interrupted) {
        Resume what;
        what.preview = true;
        noteInterrupted(key, what);
    } else if (auto r = m_resume.find(key); r != m_resume.end()) {
        r->preview = false;
        if (!r->main)
            m_resume.erase(r);
    }
    touch(e);
    pumpDownloadQueue();
}

// The real size of a file being downloaded differs from its link: halt it, it is not the file the
// message refers to.
void Core::abortChangedDownload(const QString& key, quint64 actualSize)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->state != MediaState::Downloading)
        return;
    noteRemoteSize(it->link, actualSize);
    haltDownload(it->sch, it->transferId);
    failDownload(key, MediaError::NotFound, fileChangedText(), false, true);
}

QString Core::remoteId(const MediaLink& link)
{
    return link.serverUid + QLatin1Char('\n') + QString::number(link.channelId) + QLatin1Char('\n') + link.remoteFile();
}

void Core::noteRemoteSize(const MediaLink& link, quint64 size)
{
    if (size > 0)
        m_remoteSizes.insert(remoteId(link), size);
}

// A transfer failed because of the connection: restart it once its server is reachable again
// (reconnected, or connected in another tab). Checked by m_resumeTimer: Core is not told about new
// connections.
void Core::noteInterrupted(const QString& key, const Resume& what)
{
    if (m_resumeAttempts.value(key) >= kMaxResumeAttempts)
        return; // keeps failing: it waits for a click
    Resume& r = m_resume[key];
    if (what.main) {
        r.main         = true;
        r.mainExplicit = what.mainExplicit;
    }
    r.preview = r.preview || what.preview;
    if (m_resumeTimer && !m_resumeTimer->isActive())
        m_resumeTimer->start();
}

void Core::resumeInterrupted()
{
    QHash<QString, uint64> connections; // server uid -> connection, resolved once per pass
    QStringList            ready;
    for (auto it = m_resume.begin(); it != m_resume.end();) {
        const MediaEntry* e = entry(it.key());
        // Retried by hand, cleared or reset meanwhile: nothing to do.
        if (!e || !((it->main && e->state == MediaState::Failed) || (it->preview && e->previewState == MediaState::Failed))) {
            it = m_resume.erase(it);
            continue;
        }
        auto sch = connections.constFind(e->link.serverUid);
        if (sch == connections.constEnd())
            sch = connections.insert(e->link.serverUid, ts3::connectionForServerUid(e->link.serverUid));
        if (sch.value())
            ready.append(it.key());
        ++it;
    }

    for (const QString& key : qAsConst(ready)) {
        const Resume r = m_resume.take(key);
        if (!m_entries.contains(key))
            continue;
        ++m_resumeAttempts[key];
        if (r.preview && entry(key)->previewState == MediaState::Failed)
            startPreviewDownload(key);

        auto it = m_entries.find(key);
        if (!r.main || it == m_entries.end() || it->state != MediaState::Failed)
            continue;
        ts3::log(QStringLiteral("Resuming the download of %1").arg(it->link.remoteFile()), LogLevel_INFO);
        if (r.mainExplicit) {
            startDownload(key); // the user asked for it
            continue;
        }
        // Automatic: the same rules as for a new link (settings may have changed meanwhile).
        it->state    = MediaState::Idle;
        it->progress = 0.0;
        it->error    = MediaError::None;
        it->errorText.clear();
        if (Settings::instance().inlinePreviews)
            startAutoDownloads(key);
        else
            m_autoPending.insert(key);
        auto again = m_entries.find(key);
        if (again != m_entries.end() && again->state == MediaState::Idle)
            touch(again.value()); // not fetched automatically any more: no longer an error either
    }

    if (m_resume.isEmpty() && m_resumeTimer)
        m_resumeTimer->stop();
}

quint64 Core::autoDownloadLimit(const MediaEntry& e) const
{
    const Settings& s = Settings::instance();
    return e.kind == MediaKind::Video ? megabytes(s.videoAutoDownloadMB) : megabytes(s.autoDownloadMaxMB);
}

// A link's size is untrusted: an automatic download whose real size exceeds the auto-download limit
// is stopped and waits for a click instead.
void Core::abortOversizedAutoDownload(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->state != MediaState::Downloading)
        return;
    MediaEntry& e = it.value();

    haltDownload(e.sch, e.transferId);
    if (m_downloadsByTransfer.value(e.transferId) == key)
        m_downloadsByTransfer.remove(e.transferId);
    --m_activeDownloads;
    QDir(partialDir(key, false)).removeRecursively();
    m_autoDownloads.remove(key);

    e.state           = MediaState::Idle;
    e.progress        = 0.0;
    e.transferId      = 0;
    e.tooLargeForAuto = true;
    e.openWhenReady   = false;
    ts3::log(QStringLiteral("Automatic download of %1 stopped: the file is larger than the auto-download limit").arg(e.link.remoteFile()), LogLevel_WARNING, e.sch);
    touch(e);
    pumpDownloadQueue();
}

void Core::haltDownload(uint64 sch, anyID transferId)
{
    if (sch && ts3::funcs.haltTransfer)
        ts3::funcs.haltTransfer(sch, transferId, 1, nullptr);
}

MediaStill Core::still(const QString& key, const QSize& maxPixels)
{
    auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || maxPixels.isEmpty())
        return {};
    const MediaEntry& e = it.value();
    rearmIfNeeded(key);

    const QString base = key + QLatin1Char('@') + QString::number(maxPixels.width()) + QLatin1Char('x') + QString::number(maxPixels.height()) + QLatin1Char('#');
    const MediaStill::Source order[] = {MediaStill::Full, MediaStill::Preview, MediaStill::BlurHash};
    for (const MediaStill::Source source : order) {
        QString path;
        switch (source) {
        case MediaStill::Full:
            // Videos never have a full still; for animations it is the first frame.
            if (e.state != MediaState::Ready || !isPreviewableImage(e.kind))
                continue;
            path = e.localPath;
            break;
        case MediaStill::Preview:
            if (e.previewState != MediaState::Ready || e.previewPath.isEmpty())
                continue;
            path = e.previewPath;
            break;
        case MediaStill::BlurHash:
            if (e.link.blurHash.isEmpty())
                continue;
            break;
        case MediaStill::None:
            continue;
        }

        const QString cacheKey = base + QString::number(static_cast<int>(source));
        if (const MediaStill* cached = m_stills.object(cacheKey)) {
            if (cached->image.isNull())
                continue; // this source could not be decoded; remembered until the entry changes
            if (!path.isEmpty())
                markUsed(path);
            return *cached;
        }

        if (!path.isEmpty() && m_stillJobs.contains(cacheKey)) {
            // Being decoded on a worker: show this source at another size meanwhile, if there is one.
            const MediaStill other = otherSizeStill(key, source, maxPixels);
            if (!other.image.isNull())
                return other;
            continue;
        }

        MediaStill result;
        bool       expensive = false;
        result.image         = path.isEmpty() ? decodeBlurHash(e.link, maxPixels) : decodeScaled(path, maxPixels, true, &expensive);
        if (expensive) {
            // Chat files are untrusted: a large or slow image (e.g. a tiny PNG of 80 megapixels) is
            // never decoded on the GUI thread. A worker decodes it and entryChanged(key) announces it;
            // until then the next best still is shown.
            startStillDecode(key, cacheKey, source, path, maxPixels);
            const MediaStill other = otherSizeStill(key, source, maxPixels);
            if (!other.image.isNull())
                return other;
            continue;
        }
        if (!result.image.isNull()) {
            result.source = source;
            if (!path.isEmpty())
                markUsed(path);
        }
        const int cost = qMax(1, static_cast<int>(result.image.sizeInBytes() / 1024));
        m_stills.insert(cacheKey, new MediaStill(result), cost);
        if (!result.image.isNull())
            return result;
    }
    return {};
}

void Core::startStillDecode(const QString& key, const QString& cacheKey, MediaStill::Source source, const QString& path, const QSize& maxPixels)
{
    if (m_stillJobs.contains(cacheKey))
        return;
    const quint64 job = ++m_nextStillJob;
    m_stillJobs.insert(cacheKey, job);
    QPointer<Core> self(this);
    m_stillPool.start([self, key, cacheKey, job, source, path, maxPixels] {
        const QImage image = decodeScaled(path, maxPixels);
        // ~Core waits for this pool, so the object is alive here; the queued call is dropped if it dies first.
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, key, cacheKey, job, source, path, image] {
                if (self)
                    self->onStillDecoded(key, cacheKey, job, source, path, image);
            }, Qt::QueuedConnection);
        }
    });
}

void Core::onStillDecoded(const QString& key, const QString& cacheKey, quint64 job, MediaStill::Source source, const QString& path, const QImage& image)
{
    // Dropped if its file changed meanwhile (invalidateStills / cache cleared): a newer decode runs.
    const auto running = m_stillJobs.constFind(cacheKey);
    if (running == m_stillJobs.constEnd() || running.value() != job)
        return;
    m_stillJobs.remove(cacheKey);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;

    MediaStill result;
    result.image = image;
    if (!image.isNull()) {
        result.source = source;
        markUsed(path);
    }
    // A failed decode is remembered too (until the entry changes), so it is not retried.
    const int cost = qMax(1, static_cast<int>(image.sizeInBytes() / 1024));
    m_stills.insert(cacheKey, new MediaStill(result), cost);
    if (!image.isNull())
        touch(it.value()); // whoever asked for it renders again and gets it from the cache
}

// The same source decoded for another size, scaled to this one: a placeholder while a worker decodes
// the exact size (e.g. after the chat was resized). Not cached.
MediaStill Core::otherSizeStill(const QString& key, MediaStill::Source source, const QSize& maxPixels)
{
    const QString      prefix = key + QLatin1Char('@');
    const QString      suffix = QLatin1Char('#') + QString::number(static_cast<int>(source));
    const qint64       wanted = static_cast<qint64>(maxPixels.width()) * maxPixels.height();
    const MediaStill*  best   = nullptr;
    QString            bestKey;
    const QList<QString> cached = m_stills.keys();
    for (const QString& cacheKey : cached) {
        if (!cacheKey.startsWith(prefix) || !cacheKey.endsWith(suffix))
            continue;
        const MediaStill* candidate = m_stills.object(cacheKey);
        if (!candidate || candidate->image.isNull())
            continue;
        // The smallest one at least as large as wanted, else the largest (cheap to scale, sharp enough).
        const qint64 area = static_cast<qint64>(candidate->image.width()) * candidate->image.height();
        if (best) {
            const qint64 bestArea = static_cast<qint64>(best->image.width()) * best->image.height();
            const bool   better   = bestArea < wanted ? area > bestArea : (area >= wanted && area < bestArea);
            if (!better)
                continue;
        }
        best    = candidate;
        bestKey = cacheKey;
    }
    if (!best)
        return {};

    // "<key>@<w>x<h>#<source>": the box that still was fitted into. If it is smaller than its box,
    // it is the picture's own size (never upscaled), and the exact decode will be too.
    const QString     box   = bestKey.mid(prefix.size(), bestKey.size() - prefix.size() - suffix.size());
    const QStringList dims  = box.split(QLatin1Char('x'));
    const QSize       old   = dims.size() == 2 ? QSize(dims.at(0).toInt(), dims.at(1).toInt()) : QSize();
    const QSize       size  = best->image.size();
    const bool        whole = old.isValid() && size.width() < old.width() && size.height() < old.height();
    const QSize       target = whole ? fitInto(size, maxPixels) : size.scaled(maxPixels, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    if (target.isEmpty())
        return {};

    MediaStill result;
    result.source = source;
    result.image  = target == size ? best->image : best->image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return result;
}

// After a cache clear or eviction the automatic downloads of an entry start again once it is shown.
void Core::rearmIfNeeded(const QString& key)
{
    if (!Settings::instance().inlinePreviews || !m_rearm.remove(key))
        return;
    // still() runs while a chat or the viewer renders: start the transfers right after that.
    QMetaObject::invokeMethod(this, [this, key] { startAutoDownloads(key); }, Qt::QueuedConnection);
}

void Core::invalidateStills(const QString& key)
{
    const QString        prefix = key + QLatin1Char('@');
    const QList<QString> cached = m_stills.keys();
    for (const QString& cacheKey : cached) {
        if (cacheKey.startsWith(prefix))
            m_stills.remove(cacheKey);
    }
    // Decodes still running for the old files are dropped when they finish.
    for (auto it = m_stillJobs.begin(); it != m_stillJobs.end();) {
        if (it.key().startsWith(prefix))
            it = m_stillJobs.erase(it);
        else
            ++it;
    }
}

// Only one source changed (e.g. the full image arrived): the others stay on screen meanwhile.
void Core::invalidateStill(const QString& key, MediaStill::Source source)
{
    const QString        prefix = key + QLatin1Char('@');
    const QString        suffix = QLatin1Char('#') + QString::number(static_cast<int>(source));
    const QList<QString> cached = m_stills.keys();
    for (const QString& cacheKey : cached) {
        if (cacheKey.startsWith(prefix) && cacheKey.endsWith(suffix))
            m_stills.remove(cacheKey);
    }
    for (auto it = m_stillJobs.begin(); it != m_stillJobs.end();) {
        if (it.key().startsWith(prefix) && it.key().endsWith(suffix))
            it = m_stillJobs.erase(it);
        else
            ++it;
    }
}

void Core::markUsed(const QString& path)
{
    const qint64 now  = QDateTime::currentMSecsSinceEpoch();
    qint64&      last = m_usedAt[path];
    if (now - last < kUseRefreshMs)
        return;
    last = now;
    refreshFileTime(path);
}

void Core::openExternally(const QString& key) const
{
    const MediaEntry* e = entry(key);
    if (!e || e->state != MediaState::Ready || !QFileInfo(e->localPath).isFile())
        return;
    refreshFileTime(e->localPath);
    // Never launch executables received from chat; show them in Explorer instead.
    if (isUnsafeToOpen(key)) {
        revealInExplorer(e->localPath);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(e->localPath));
}

bool Core::isUnsafeToOpen(const QString& key) const
{
    const MediaEntry* e = entry(key);
    return e && (isRiskyToOpen(e->link.fileName) || isRiskyToOpen(e->localPath));
}

void Core::revealInFolder(const QString& key) const
{
    const MediaEntry* e = entry(key);
    if (!e || e->state != MediaState::Ready || !QFileInfo(e->localPath).isFile())
        return;
    revealInExplorer(e->localPath);
}

QString Core::saveAs(const QString& key, QWidget* parent) const
{
    const MediaEntry* e = entry(key);
    if (!e || e->state != MediaState::Ready || !QFileInfo(e->localPath).isFile())
        return {};
    // The dialog runs an event loop: copy what is needed, the entry may change meanwhile.
    const QString   localPath = e->localPath;
    const MediaKind kind      = e->kind;
    const QString   name      = localFileName(displayNameFor(e->link)); // without the cache prefix and the random part

    static QString lastDir;
    QString        dir = lastDir;
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        const auto location = isPreviewableImage(kind)      ? QStandardPaths::PicturesLocation
                              : kind == MediaKind::Video    ? QStandardPaths::MoviesLocation
                                                            : QStandardPaths::DownloadLocation;
        dir = QStandardPaths::writableLocation(location);
    }

    const QString ext    = QFileInfo(name).suffix();
    QString       filter = i18n::t("All files (*.*)");
    if (!ext.isEmpty())
        filter.prepend(i18n::t("%1 files (*.%2)").arg(ext.toUpper(), ext) + QStringLiteral(";;"));

    const QString title  = i18n::t("Save as");
    const QString target = QFileDialog::getSaveFileName(parent, title, QDir(dir).filePath(name), filter);
    if (target.isEmpty())
        return {};
    lastDir = QFileInfo(target).absolutePath();
    if (pathKey(QFileInfo(target).absoluteFilePath()) == pathKey(QFileInfo(localPath).absoluteFilePath()))
        return target; // the cached file itself: nothing to copy

    // The dialog already confirmed overwriting.
    const bool ok = QFileInfo(localPath).isFile() && (!QFile::exists(target) || QFile::remove(target)) && QFile::copy(localPath, target);
    if (!ok) {
        // Not QMessageBox::warning: its OK button follows TeamSpeak's language, and a top-level box
        // takes TeamSpeak's layout direction instead of the plugin's left-to-right one.
        QMessageBox box(QMessageBox::Warning, title,
                        i18n::t("Couldn't save the file to %1. Check that the folder exists and that you can write to it.").arg(QDir::toNativeSeparators(target)),
                        QMessageBox::Ok, parent);
        box.setLayoutDirection(Qt::LeftToRight);
        if (QAbstractButton* okButton = box.button(QMessageBox::Ok))
            okButton->setText(i18n::t("OK"));
        box.exec();
        return {};
    }
    return target;
}

void Core::setInUse(const QString& key, bool inUse)
{
    if (!inUse) {
        auto it = m_inUse.find(key);
        if (it != m_inUse.end() && --it.value() <= 0)
            m_inUse.erase(it);
        return;
    }
    ++m_inUse[key];
    if (const MediaEntry* e = entry(key)) {
        if (e->state == MediaState::Ready)
            refreshFileTime(e->localPath);
    }
}

// ============================================================================================
// Uploads
// ============================================================================================

QString Core::makeRemoteName(const QString& originalName)
{
    const QFileInfo fi(originalName);
    QString         base = fi.completeBaseName();
    const QString   ext  = fi.suffix().toLower();

    // Keep names readable but safe for the file browser, BBCode and every OS.
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\[\]%#&+=]+)")), QStringLiteral("_"));
    base.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral("_"));
    // 2.2: at most 48 characters and 64 UTF-8 bytes, so a non-Latin name can't crowd the message.
    base = boundRemoteBase(base);
    if (base.isEmpty())
        base = QStringLiteral("file");

    // Everyone uploads into the same folder: the random part keeps names apart (and an upload never
    // overwrites, see startSend).
    return base + QLatin1Char('_') + uniqueSuffix() + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
}

// The same name with a new random part, after the server reported that the name is taken.
QString Core::renamedRemoteName(const QString& remoteName)
{
    const QFileInfo fi(remoteName);
    QString         base = fi.completeBaseName();
    const QString   ext  = fi.suffix();
    base.remove(QRegularExpression(QStringLiteral("_[0-9a-f]{8}$")));
    base = boundRemoteBase(base);
    if (base.isEmpty())
        base = QStringLiteral("file");
    return base + QLatin1Char('_') + uniqueSuffix() + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
}

void Core::uploadFiles(const QStringList& paths, const ChatTarget& target)
{
    SendRequest request;
    request.target = target;
    for (const QString& path : paths) {
        SendItem item;
        item.path = path;
        request.items.append(item);
    }
    send(request);
}

namespace {

bool isAlbumKind(const SendItem& item)
{
    if (item.voice)
        return false;
    const MediaKind kind = kindForFileName(item.path);
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

// The name a file gets on the server.
QString remoteNameFor(const SendItem& item, const QString& makeName)
{
    static const QRegularExpression pastedName(QStringLiteral("^new_photo_[0-9a-f]{8}\\.[a-z]+$"));
    const QFileInfo                 fi(item.path);
    if (item.pasted) {
        // Our own paste keeps the name it was written under (a retry sends the same name).
        if (pastedName.match(fi.fileName()).hasMatch())
            return fi.fileName();
        const QString ext = fi.suffix().toLower();
        return QStringLiteral("new_photo_") + uniqueSuffix() + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    if (item.voice)
        return QStringLiteral("voice_message_") + uniqueSuffix() + QStringLiteral(".m4a");
    return makeName;
}

} // namespace

int Core::send(const SendRequest& request)
{
    if (request.items.isEmpty())
        return 0;
    const auto dropTemps = [&request] {
        for (const SendItem& item : request.items) {
            if (item.ownTemp)
                QFile::remove(item.path);
        }
    };
    // Checked once, so a dropped batch gives one line in the chat, not one per file.
    ChatTarget target = request.target;
    if (!target.sch)
        target.sch = ts3::currentConnection();
    if (!ts3::isConnected(target.sch)) {
        ts3::printWarning(ts3::currentConnection(), notConnectedText());
        dropTemps();
        return 0;
    }

    QVector<SendItem> items;
    QStringList       missing;
    for (const SendItem& requested : request.items) {
        const QFileInfo fi(requested.path);
        if (!fi.exists() || !fi.isFile()) {
            missing.append(QDir::toNativeSeparators(requested.path));
            continue;
        }
        SendItem item = requested;
        item.path     = fi.absoluteFilePath();
        items.append(item);
    }
    // Names in curly quotes: the plugin log marks those as private (diagnostics leave them out).
    if (missing.size() == 1)
        ts3::printWarning(target.sch, i18n::t("Couldn't find “%1”. It may have been moved or deleted.").arg(missing.first()));
    else if (missing.size() > 1)
        ts3::printWarning(target.sch, i18n::t("Couldn't find %1 files: “%2”. They may have been moved or deleted.").arg(missing.size()).arg(missing.join(QStringLiteral("”, “"))));
    if (items.isEmpty())
        return 0;

    // Post order: the albums (pictures and videos in their order, up to kMaxAlbumItems each; a last
    // chunk of one is a single file), then everything else in its order.
    QVector<QVector<int>> units;
    QVector<int>          media;
    for (int i = 0; i < items.size(); ++i) {
        if (request.album && albums::enabled() && isAlbumKind(items.at(i))) // 2.2 album: the feature switch
            media.append(i);
    }
    if (media.size() >= 2) {
        for (int at = 0; at < media.size(); at += MediaLink::kMaxAlbumItems)
            units.append(media.mid(at, MediaLink::kMaxAlbumItems));
    } else {
        media.clear();
    }
    for (int i = 0; i < items.size(); ++i) {
        if (!media.contains(i))
            units.append({i});
    }

    const int batch            = ++m_nextBatch;
    m_batches[batch].caption   = request.caption;
    for (const QVector<int>& indexes : qAsConst(units)) {
        PostUnit unit;
        unit.album = indexes.size() >= 2;
        for (int index : indexes) {
            const SendItem& item = items.at(index);
            const int       id   = createUpload(item, remoteNameFor(item, makeRemoteName(QFileInfo(item.path).fileName())), target, batch);
            if (id == 0)
                continue;
            unit.jobs.append(id);
            if (unit.album)
                m_uploads[id].inAlbum = true;
        }
        if (!unit.jobs.isEmpty())
            m_batches[batch].units.append(unit);
    }
    if (m_batches.value(batch).units.isEmpty()) {
        m_batches.remove(batch);
        return 0;
    }
    scheduleUploadQueue(); // files that failed at once may let the rest of their batch post
    return batch;
}

void Core::uploadImage(const QImage& image, const ChatTarget& target)
{
    if (image.isNull())
        return;

    const QString dir = ts3::dataDir() + QStringLiteral("/paste");
    QDir().mkpath(dir);

    const QString number = uniqueSuffix();
    QByteArray    data;
    QBuffer       buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    QString name = QStringLiteral("new_photo_") + number + QStringLiteral(".png");

    // Photos pasted as PNG can be huge; JPEG is far smaller when there is no transparency.
    if (Settings::instance().convertLargePngToJpeg && data.size() > 2 * 1024 * 1024 && !image.hasAlphaChannel()) {
        data.clear();
        buffer.seek(0);
        image.save(&buffer, "JPG", 90);
        name = QStringLiteral("new_photo_") + number + QStringLiteral(".jpg");
    }
    buffer.close();

    const QString path = dir + QLatin1Char('/') + name;
    if (!writeFile(path, data)) {
        ts3::printWarning(target.sch ? target.sch : ts3::currentConnection(), i18n::t("Couldn't prepare the pasted image. Check free disk space and try again."));
        QFile::remove(path);
        return;
    }
    SendRequest request;
    request.target = target;
    SendItem item;
    item.path    = path;
    item.pasted  = true;
    item.ownTemp = true;
    request.items.append(item);
    send(request);
}

int Core::createUpload(const SendItem& item, const QString& remoteName, const ChatTarget& requested, int batch)
{
    const QString& sourcePath   = item.path;
    const bool     deleteSource = item.ownTemp;
    ChatTarget     target       = requested;
    if (!target.sch)
        target.sch = ts3::currentConnection();

    if (!ts3::isConnected(target.sch)) {
        ts3::printWarning(ts3::currentConnection(), notConnectedText());
        if (deleteSource)
            QFile::remove(sourcePath);
        return 0;
    }

    const Settings& s = Settings::instance();
    UploadJob       job;
    job.id           = ++m_nextUploadId;
    job.batch        = batch;
    job.target       = target;
    job.channelId    = ts3::ownChannel(target.sch);
    job.sourcePath   = sourcePath;
    job.deleteSource = deleteSource;
    job.pasted       = item.pasted;
    job.displayName  = item.displayName;
    job.spoiler      = item.spoiler;
    job.remoteDir    = normalizeRemoteDir(s.uploadDirectory);
    job.remoteName   = remoteName;
    job.size         = static_cast<quint64>(QFileInfo(sourcePath).size());
    job.state        = UploadState::Preparing;
    job.waiting      = true; // until a worker picks it up (markProbeStarted)
    job.message      = i18n::t("Waiting to upload…");
    const int id     = job.id;
    m_uploads.insert(id, job);
    m_jobItems.insert(id, item);
    emit uploadChanged(id);

    UploadJob&    j     = m_uploads[id];
    const quint64 limit = megabytes(s.uploadMaxMB);
    if (j.size > limit) {
        failUpload(id, uploadLimitText(s.uploadMaxMB));
        return id;
    }
    if (j.size == 0) {
        failUpload(id, i18n::t("This file is empty."));
        return id;
    }
    if (ts3::channelHasPassword(target.sch, j.channelId)) {
        failUpload(id, uploadErrorText(MediaError::Password));
        return id;
    }

    // The client lib resolves the local file relative to the source directory; depending on the
    // client version that is either <dir>/<name> or <dir>/<remote path>/<name>. Stage both.
    j.stagingDir = ts3::dataDir() + QStringLiteral("/upload/") + QString::number(id);
    removeStaging(j.stagingDir);
    QDir().mkpath(j.stagingDir);
    const QString staged = j.stagingDir + QLatin1Char('/') + remoteName;
    const QString nested = j.remoteDir == QLatin1String("/") ? QString() : j.stagingDir + j.remoteDir + QLatin1Char('/') + remoteName;

    // This runs inside a drop / paste event (the drag source waits for it too): never copy here. A
    // pasted image is our own file on the same volume and is simply linked (the original stays for a
    // retry, see cleanupUpload) or else moved; anything else (maybe a large video on a USB stick or a
    // network share) is copied on the worker below.
    QString copyFrom = sourcePath;
    if (deleteSource) {
        if (hardLink(sourcePath, staged)) {
            copyFrom.clear();
        } else if (QFile::rename(sourcePath, staged)) {
            copyFrom.clear();
            j.deleteSource = false; // it is the staged file now: removed with the staging folder
        }
    }

    // Then size, duration, BlurHash and the preview come from the staged copy, still on the worker:
    // decoding a large photo or opening a video takes far too long for the GUI thread.
    const bool     previews = s.generatePreviews;
    const auto     cancel   = std::make_shared<std::atomic<bool>>(false);
    QPointer<Core> self(this);
    m_probing.insert(id, j.stagingDir);
    m_stagingCancel.insert(id, cancel);
    m_pool.start([self, id, copyFrom, staged, nested, previews, cancel] {
        // Jobs wait in the pool's queue until a worker is free; tell the toast this one has started.
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id] {
                if (self)
                    self->markProbeStarted(id);
            }, Qt::QueuedConnection);
        }
        bool ok = copyFrom.isEmpty() || copyForStaging(copyFrom, staged, cancel.get());
        if (ok && !nested.isEmpty()) {
            QDir().mkpath(QFileInfo(nested).absolutePath());
            ok = linkOrCopy(staged, nested);
        }
        const quint64  size = ok ? static_cast<quint64>(QFileInfo(staged).size()) : 0;
        LocalMediaInfo info;
        QByteArray     jpeg;
        if (ok && !cancel->load()) {
            info = probeLocalMedia(staged, previews);
            jpeg = info.preview.isNull() ? QByteArray() : encodePreviewJpeg(info.preview);
        }
        // ~Core waits for this pool, so the object is alive here; the queued call is dropped if it dies first.
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id, ok, size, info, jpeg] {
                if (self)
                    self->onProbed(id, ok, size, info, jpeg);
            }, Qt::QueuedConnection);
        }
    });
    return id;
}

void Core::markProbeStarted(int id)
{
    auto it = m_uploads.find(id);
    if (it != m_uploads.end() && it->state == UploadState::Preparing && it->waiting)
        setUploadState(it.value(), UploadState::Preparing, i18n::t("Preparing…"));
}

void Core::onProbed(int id, bool staged, quint64 stagedSize, const LocalMediaInfo& info, const QByteArray& previewJpeg)
{
    const QString staging = m_probing.take(id);
    m_stagingCancel.remove(id);
    const QString source = m_deleteAfterProbe.take(id); // the worker no longer reads it
    if (!source.isEmpty())
        QFile::remove(source);
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing) {
        // Canceled or failed meanwhile; the staged file could not be deleted while it was open.
        removeStaging(staging);
        return;
    }
    if (!staged) {
        failUpload(id, i18n::t("Couldn't read the file. It may be open in another program or no longer exist."));
        return;
    }
    // The staged copy is what gets uploaded: its size goes into the link (the original may have
    // changed since it was checked).
    const Settings& s = Settings::instance();
    if (stagedSize == 0) {
        failUpload(id, i18n::t("This file is empty."));
        return;
    }
    if (stagedSize > megabytes(s.uploadMaxMB)) {
        failUpload(id, uploadLimitText(s.uploadMaxMB));
        return;
    }
    UploadJob& job = it.value();
    job.size       = stagedSize;
    job.info       = info;
    job.waiting    = false;
    m_uploadExtra[id].previewJpeg = previewJpeg;
    ts3::log(QStringLiteral("Probed %1: %2x%3, %4 ms, blurhash %5, preview %6")
                 .arg(job.remoteName)
                 .arg(info.width)
                 .arg(info.height)
                 .arg(info.durationMs)
                 .arg(info.blurHash.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                 .arg(previewJpeg.isEmpty() ? QStringLiteral("none") : formatSize(static_cast<quint64>(previewJpeg.size()))),
             LogLevel_DEBUG, job.target.sch);

    if (job.remoteDir == QLatin1String("/"))
        onDirectoryReady(id, false, true);
    else
        createRemoteDirectory(id, false);
}

QString Core::previewDirFor(const UploadJob& job) const
{
    return joinRemote(job.remoteDir, QStringLiteral("previews"));
}

void Core::createRemoteDirectory(int id, bool previews)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    UploadJob& job = it.value();

    const QString  dir = previews ? previewDirFor(job) : job.remoteDir;
    const QString  rc  = registerOp(OpType::MakeDirectory, {}, id, previews);
    const unsigned err = ts3::funcs.requestCreateDirectory(job.target.sch, job.channelId, "", dir.toUtf8().constData(), rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        onDirectoryReady(id, previews, false);
        return;
    }
    // Not every server answers a mkdir for an existing folder; don't wait forever.
    QTimer::singleShot(kMkdirTimeoutMs, this, [this, rc, id, previews] {
        if (!m_ops.contains(rc))
            return;
        forgetOp(rc);
        onDirectoryReady(id, previews, true);
    });
}

void Core::onDirectoryReady(int id, bool previews, bool ok)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    UploadJob&   job   = it.value();
    UploadExtra& extra = m_uploadExtra[id];

    if (!previews) {
        if (!ok)
            job.remoteDir = QStringLiteral("/"); // no permission for folders: fall back to the channel root
        if (extra.previewJpeg.isEmpty())
            startSend(id);
        else
            createRemoteDirectory(id, true);
        return;
    }

    // Without a previews folder the preview goes next to the file.
    extra.previewInFolder = ok;
    extra.previewRemote   = previewRemoteFor(job, ok);
    startPreviewSend(id);
}

// 2.2: the preview is named after the random part of the file's name, <dir>/previews/<8 hex>.jpg, or
// <dir>/<8 hex>.preview.jpg: short, so the name isn't in the message twice (2.1 accepts any pv path).
// A new name for the file (renameUpload) gives the preview a new one too.
QString Core::previewRemoteFor(const UploadJob& job, bool inFolder) const
{
    QString base = remoteSuffixOf(job.remoteName);
    if (base.isEmpty())
        base = QFileInfo(job.remoteName).completeBaseName();
    return inFolder ? joinRemote(previewDirFor(job), base + QStringLiteral(".jpg")) : joinRemote(job.remoteDir, base + QStringLiteral(".preview.jpg"));
}

void Core::startPreviewSend(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    UploadJob&   job   = it.value();
    UploadExtra& extra = m_uploadExtra[id];

    // Same two staging layouts as the main file.
    const int     slash     = extra.previewRemote.lastIndexOf(QLatin1Char('/'));
    const QString remoteDir = slash > 0 ? extra.previewRemote.left(slash) : QStringLiteral("/");
    const QString name      = extra.previewRemote.mid(slash + 1);
    const QString source    = previewStagingDir(job.stagingDir);
    bool          ok        = writeFile(source + QLatin1Char('/') + name, extra.previewJpeg);
    if (ok && remoteDir != QLatin1String("/"))
        ok = writeFile(source + remoteDir + QLatin1Char('/') + name, extra.previewJpeg);
    if (!ok) {
        ts3::log(QStringLiteral("Could not stage the preview of %1; sending without it").arg(job.remoteName), LogLevel_WARNING, job.target.sch);
        startSend(id);
        return;
    }

    // Never overwrite: an existing file of that name belongs to another message (finishPreviewUpload
    // renames on "file already exists").
    const QString  rc  = registerOp(OpType::Upload, {}, id, true);
    anyID          tid = 0;
    const unsigned err = ts3::funcs.sendFile(job.target.sch, job.channelId, "", extra.previewRemote.toUtf8().constData(), 0, 0, utf8Native(source).constData(), &tid,
                                             rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        ts3::log(QStringLiteral("Preview upload for %1 not started (%2); sending without it").arg(job.remoteName, ts3::errorText(err)), LogLevel_WARNING, job.target.sch);
        startSend(id);
        return;
    }
    setOpTransfer(rc, tid);
    extra.previewTransferId = tid;
    extra.previewActive     = true;
    m_previewUploadsByTransfer.insert(tid, id);
}

void Core::finishPreviewUpload(int id, bool ok, const QString& reason, unsigned int error)
{
    auto xt = m_uploadExtra.find(id);
    if (xt == m_uploadExtra.end() || !xt->previewActive)
        return;
    m_previewUploadsByTransfer.remove(xt->previewTransferId);
    xt->previewActive     = false;
    xt->previewTransferId = 0;
    const QString remote  = xt->previewRemote;

    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    if (!ok && error == ERROR_file_already_exists && renameUpload(it.value())) {
        // Someone else's preview has this name. The preview is named after the file, so both get
        // a new name.
        UploadExtra& extra  = m_uploadExtra[id];
        extra.previewRemote = previewRemoteFor(it.value(), extra.previewInFolder);
        startPreviewSend(id);
        return;
    }
    if (ok)
        it->previewRemotePath = remote;
    else
        ts3::log(QStringLiteral("Preview upload for %1 failed (%2); sending without it").arg(it->remoteName, reason), LogLevel_WARNING, it->target.sch);
    startSend(id);
}

void Core::abortPreviewUpload(UploadJob& job)
{
    auto xt = m_uploadExtra.find(job.id);
    if (xt == m_uploadExtra.end() || !xt->previewActive)
        return;
    if (ts3::funcs.haltTransfer)
        ts3::funcs.haltTransfer(job.target.sch, xt->previewTransferId, 1, nullptr);
    m_previewUploadsByTransfer.remove(xt->previewTransferId);
    xt->previewActive     = false;
    xt->previewTransferId = 0;
}

void Core::startSend(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    UploadJob& job = it.value();

    // A few transfers at a time, earliest file first (runUploadQueue starts the next one): they
    // finish sooner one by one than all sharing the bandwidth, and post in order.
    if (m_uploadsByTransfer.size() >= kMaxParallelUploads) {
        if (!m_sendQueue.contains(id))
            m_sendQueue.insert(std::lower_bound(m_sendQueue.begin(), m_sendQueue.end(), id), id);
        if (!job.waiting)
            setUploadState(job, UploadState::Preparing, i18n::t("Waiting to upload…"), true);
        return;
    }
    m_sendQueue.removeAll(id);
    job.waiting = false;

    // Never overwrite: everyone uploads into the same folder, and an existing file of that name
    // belongs to another message. "File already exists" gets a new name (resendWithNewName).
    const QString  remote = joinRemote(job.remoteDir, job.remoteName);
    const QString  rc     = registerOp(OpType::Upload, {}, id);
    anyID          tid    = 0;
    const unsigned err    = ts3::funcs.sendFile(job.target.sch, job.channelId, "", remote.toUtf8().constData(), 0, 0, utf8Native(job.stagingDir).constData(), &tid,
                                                rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        if (err == ERROR_file_already_exists && renameUpload(job)) {
            startSend(id); // bounded by kMaxUploadRenames
            return;
        }
        failUpload(id, err == ERROR_file_already_exists ? nameTakenText() : uploadErrorText(mapError(err), ts3::errorText(err)));
        return;
    }
    setOpTransfer(rc, tid);

    job.transferId     = tid;
    job.transferActive = true;
    m_uploadsByTransfer.insert(tid, id);
    setUploadState(job, UploadState::Uploading, i18n::t("Uploading…"));
    ensureProgressTimer();
}

// The server refused the upload because a file of that name exists: upload under a new name.
void Core::resendWithNewName(int id, anyID failedTransfer)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Uploading || !it->transferActive || it->transferId != failedTransfer)
        return; // an answer for an earlier attempt
    UploadJob& job = it.value();
    m_uploadsByTransfer.remove(job.transferId);
    job.transferActive = false;
    job.transferId     = 0;
    if (!renameUpload(job)) {
        failUpload(id, nameTakenText());
        return;
    }
    job.state    = UploadState::Preparing; // startSend starts from there
    job.progress = 0.0;
    startSend(id);
}

// Gives the upload a new remote name (and renames its staged file). False once kMaxUploadRenames
// is reached or the staged file cannot be renamed.
bool Core::renameUpload(UploadJob& job)
{
    UploadExtra& extra = m_uploadExtra[job.id];
    if (extra.renames >= kMaxUploadRenames || job.stagingDir.isEmpty())
        return false;
    ++extra.renames;

    // Rename / hard link only: this runs on the GUI thread.
    const QString name = renamedRemoteName(job.remoteName);
    const QString from = job.stagingDir + QLatin1Char('/') + job.remoteName;
    const QString to   = job.stagingDir + QLatin1Char('/') + name;
    if (!QFile::rename(from, to)) {
        if (!hardLink(from, to))
            return false;
        QFile::remove(from);
    }
    if (job.remoteDir != QLatin1String("/")) {
        const QString nested = job.stagingDir + job.remoteDir + QLatin1Char('/');
        if (!QFile::rename(nested + job.remoteName, nested + name)) {
            QDir().mkpath(nested);
            if (!hardLink(to, nested + name)) {
                // Keep the staging folder matching the old name (the caller may go on with it).
                if (QFile::exists(from))
                    QFile::remove(to);
                else
                    QFile::rename(to, from);
                return false;
            }
        }
    }
    ts3::log(QStringLiteral("The name of %1 (or of its preview) is taken on the server; uploading as %2").arg(joinRemote(job.remoteDir, job.remoteName), name), LogLevel_INFO, job.target.sch);
    job.remoteName = name;
    return true;
}

void Core::finishUpload(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Uploading)
        return;
    UploadJob& job     = it.value();
    job.transferActive = false;
    job.uploaded       = true;
    job.progress       = 1.0;
    m_uploadsByTransfer.remove(job.transferId);
    m_uploadExtra[id].mainUploaded = true; // from here on the file (and its preview) stay on the server

    MediaLink link;
    ts3::getServerAddress(job.target.sch, &link.host, &link.port);
    link.serverUid   = ts3::serverUid(job.target.sch);
    link.channelId   = job.channelId;
    link.path        = job.remoteDir;
    link.fileName    = job.remoteName;
    link.size        = job.size;
    link.dateTime    = QDateTime::currentSecsSinceEpoch();
    link.protocol    = MediaLink::kProtocol;
    link.width       = job.info.width;
    link.height      = job.info.height;
    link.durationMs  = job.info.durationMs;
    link.blurHash    = job.info.blurHash;
    link.previewFile = job.previewRemotePath;
    if (const auto item = m_jobItems.constFind(id); item != m_jobItems.constEnd()) {
        link.spoiler = item->spoiler;
        link.voice   = item->voice;
        if (item->voice) {
            if (item->durationMs > 0)
                link.durationMs = item->durationMs;
            link.waveform = item->waveform;
        }
    }
    link = sanitized(link); // what receivers will see

    // Seed the cache so the sender sees the media instantly.
    seedCache(job, link);
    m_uploadExtra[id].link = link;
    ts3::log(QStringLiteral("Uploaded %1 (%2)").arg(joinRemote(job.remoteDir, job.remoteName), formatSize(job.size)), LogLevel_INFO, job.target.sch);
    scheduleCacheLimit(link.key());

    // The messages of one send appear in its order: a file that finishes before an earlier one (or
    // before the rest of its album) waits; pumpPostUnits posts it when its turn has come.
    const QString held = heldText(job);
    setUploadState(job, UploadState::Posting, held.isEmpty() ? i18n::t("Posting to chat…") : held, !held.isEmpty());
}

namespace {

bool isPreparing(UploadState state)
{
    return state == UploadState::Preparing || state == UploadState::Compressing || state == UploadState::Uploading;
}

} // namespace

// What an uploaded file's message waits for: an earlier part of its send that still has to be posted,
// or the rest of its album (an upload still preparing, compressing or uploading). Empty when its turn
// has come. The one rule for holding messages: pumpPostUnits and the toast texts follow it.
QString Core::heldText(const UploadJob& job) const
{
    const auto batch = m_batches.constFind(job.batch);
    if (batch == m_batches.constEnd())
        return {};
    for (const PostUnit& unit : batch->units) {
        if (!unit.jobs.contains(job.id)) {
            if (!unit.composed)
                return i18n::t("Waiting for earlier files…");
            continue;
        }
        if (unit.composed)
            return {};
        for (int id : unit.jobs) {
            const UploadJob* other = upload(id);
            if (id != job.id && other && isPreparing(other->state))
                return i18n::t("Waiting for the rest of the album…");
        }
        return {};
    }
    return {};
}

// Composes the units whose turn has come: in each batch, in order, every unit whose uploads are all
// settled (uploaded, failed or canceled) until one isn't. Also keeps the waiting texts right.
void Core::pumpPostUnits()
{
    QList<int> batches = m_batches.keys();
    std::sort(batches.begin(), batches.end());
    for (int batch : qAsConst(batches)) {
        bool earlierPending = false;
        // Looked up again on every step and copied: state changes emit signals, and whatever reacts
        // to them may change the batches.
        for (int u = 0;; ++u) {
            const auto b = m_batches.constFind(batch);
            if (b == m_batches.constEnd() || u >= b->units.size())
                break;
            const PostUnit unit = b->units.at(u);
            if (unit.composed)
                continue;
            bool settled = true;
            for (int id : unit.jobs) {
                const UploadJob* job = upload(id);
                if (job && isPreparing(job->state))
                    settled = false;
            }
            if (!earlierPending && settled) {
                composeUnit(batch, u);
                continue;
            }
            // Held: say what it waits for (only when that changed: every update runs the queue again).
            for (int id : unit.jobs) {
                auto job = m_uploads.find(id);
                if (job == m_uploads.end() || job->state != UploadState::Posting || !job->waiting)
                    continue;
                const QString text = heldText(job.value());
                if (!text.isEmpty() && job->message != text)
                    setUploadState(job.value(), UploadState::Posting, text, true);
            }
            earlierPending = true;
        }
    }
    pumpPosts();
}

void Core::composeUnit(int batch, int index)
{
    auto b = m_batches.find(batch);
    if (b == m_batches.end() || index < 0 || index >= b->units.size() || b->units.at(index).composed)
        return;
    b->units[index].composed = true;
    const PostUnit unit      = b->units.at(index);
    // The caption goes with the first unit that posts something (marked as done below).
    const QString    caption = b->captionDone ? QString() : b->caption;
    QVector<int>     ids;
    QList<MediaLink> links;
    QStringList      files; // remote paths, for the log
    for (int id : qAsConst(unit.jobs)) {
        const UploadJob* job = upload(id);
        if (job && job->uploaded && job->state == UploadState::Posting && m_uploadExtra.contains(id)) {
            ids.append(id);
            links.append(m_uploadExtra.value(id).link);
            files.append(joinRemote(job->remoteDir, job->remoteName));
        }
    }
    if (ids.isEmpty())
        return; // nothing of it reached the server: the caption waits for the next unit
    // Taken now: touch() below emits signals, and nothing may be assumed about the jobs afterwards.
    const UploadJob first = *upload(ids.first());

    // An album is numbered over what was uploaded; a single survivor is a normal file.
    if (unit.album && ids.size() >= 2) {
        quint32 album = 0;
        while (album == 0)
            album = QRandomGenerator::global()->generate();
        const QString own = ts3::ownUid(first.target.sch); // 2.2 album: our own albums group without waiting for the echo
        for (int i = 0; i < links.size(); ++i) {
            links[i].albumId    = album;
            links[i].albumIndex = i + 1;
            links[i].albumCount = links.size();
            m_albums.note(links.at(i).serverUid, album, i + 1, links.size(), links.at(i).key(), own); // 2.2 album
            m_uploadExtra[ids.at(i)].link = links.at(i);
            // The sender's own copy knows its album too (the key doesn't change).
            auto e = m_entries.find(links.at(i).key());
            if (e != m_entries.end() && e->isOwnUpload) {
                e->link.albumId    = album;
                e->link.albumIndex = i + 1;
                e->link.albumCount = links.size();
                touch(e.value());
            }
        }
    }

    const Settings& s = Settings::instance();
    ComposeOptions  options;
    options.includeNotice = s.addRequiredNotice;
    options.downloadUrl   = s.pluginDownloadUrl;
    options.caption       = caption;
    if (!caption.isEmpty()) {
        auto info = m_batches.find(batch);
        if (info != m_batches.end())
            info->captionDone = true;
    }

    for (const ComposedMessage& message : composeChatMessagesDetailed(links, options)) {
        PostItem post;
        post.sch       = first.target.sch;
        post.target    = first.target;
        post.channelId = first.channelId;
        post.text      = message.text.toUtf8();
        for (int link : message.links)
            post.jobs.append(ids.at(link));
        if (!message.dropped.isEmpty() || message.tooLong) {
            const QString file = message.links.isEmpty() ? QString() : files.at(message.links.first());
            if (!message.dropped.isEmpty())
                ts3::log(LogLevel_WARNING, post.sch, "Message for %1 dropped %2 to fit TeamSpeak's message limit",
                         {ts3::file(file), ts3::pub(message.dropped.join(QStringLiteral(", ")))});
            if (message.tooLong)
                ts3::log(LogLevel_WARNING, post.sch, "Message for %1 is %2 bytes, more than TeamSpeak's message limit allows (%3); sending it anyway",
                         {ts3::file(file), ts3::pub(post.text.size()), ts3::pub(kMaxMessageBytes)});
        }
        m_postQueue[post.sch].append(post);
    }
    for (int id : qAsConst(ids)) {
        auto job = m_uploads.find(id);
        if (job != m_uploads.end())
            setUploadState(job.value(), UploadState::Posting, i18n::t("Posting to chat…"));
    }
}

// Sends what the governors allow now and plans the next wake-up.
void Core::pumpPosts()
{
    const qint64 now  = floodClockMs();
    qint64       wake = -1;
    for (const uint64 sch : m_postQueue.keys()) {
        FloodGovernor& governor = floodGovernor(sch);
        while (!m_postQueue[sch].isEmpty() && governor.postReady(now)) {
            PostItem post = m_postQueue[sch].takeFirst();
            sendPost(post);
        }
        const int pending = m_postQueue.value(sch).size();
        governor.setPendingPosts(pending);
        if (pending == 0) {
            m_postQueue.remove(sch);
            emit floodGovernorChanged(sch); // plugin commands may go again
            continue;
        }
        const qint64 at = governor.nextPostCheckMs(now);
        wake            = wake < 0 ? at : qMin(wake, at);
    }
    if (m_postTimer) {
        if (wake >= 0)
            m_postTimer->start(static_cast<int>(qBound<qint64>(0, wake - now, 60 * 60 * 1000)));
        else
            m_postTimer->stop();
    }
}

bool Core::sendPost(PostItem& post)
{
    const QString rc  = registerOp(OpType::PostMessage, {}, post.jobs.value(0));
    unsigned      err = ERROR_ok;
    switch (post.target.mode) {
    case TextMessageTarget_SERVER:
        err = ts3::funcs.requestSendServerTextMsg(post.sch, post.text.constData(), rc.toUtf8().constData());
        break;
    case TextMessageTarget_CLIENT:
        err = ts3::funcs.requestSendPrivateTextMsg(post.sch, post.text.constData(), post.target.clientId, rc.toUtf8().constData());
        break;
    default:
        err = ts3::funcs.requestSendChannelTextMsg(post.sch, post.text.constData(), post.channelId, rc.toUtf8().constData());
        break;
    }
    ++post.attempts;
    if (err != ERROR_ok) {
        forgetOp(rc);
        failPost(post, postErrorText(mapError(err), ts3::errorText(err)));
        return false;
    }

    InFlightPost flight;
    flight.item   = post;
    flight.ticket = floodGovernor(post.sch).postSent(floodClockMs());
    m_postsInFlight.insert(rc, flight);
    for (int id : qAsConst(post.jobs)) {
        auto job = m_uploads.find(id);
        if (job != m_uploads.end() && job->state == UploadState::Posting && job->message != i18n::t("Posting to chat…"))
            setUploadState(job.value(), UploadState::Posting, i18n::t("Posting to chat…"));
    }
    // The governor lets the next post go after a second without an answer; the message counts as sent
    // when no answer comes at all.
    QTimer::singleShot(kPostTimeoutMs, this, [this, rc] {
        if (!m_ops.contains(rc))
            return;
        forgetOp(rc);
        finishPost(rc, ERROR_ok, QString(), false);
    });
    return true;
}

// The answer to a chat post (or its timeout, as ERROR_ok).
void Core::finishPost(const QString& returnCode, unsigned int error, const QString& message, bool permissionError)
{
    auto it = m_postsInFlight.find(returnCode);
    if (it == m_postsInFlight.end())
        return;
    InFlightPost flight = it.value();
    m_postsInFlight.erase(it);
    FloodGovernor& governor = floodGovernor(flight.item.sch);
    const bool     flooded  = error == ERROR_client_is_flooding && !permissionError;
    const bool     retry    = governor.postAnswered(flight.ticket, floodClockMs(), flooded, flight.item.attempts);
    if (flooded && retry) {
        ts3::log(LogLevel_WARNING, flight.item.sch, "TeamSpeak's flood protection held back a chat message (attempt %1); sending it again shortly",
                 {ts3::pub(flight.item.attempts)});
        for (int id : qAsConst(flight.item.jobs)) {
            auto job = m_uploads.find(id);
            if (job != m_uploads.end() && job->state == UploadState::Posting)
                setUploadState(job.value(), UploadState::Posting, i18n::t("TeamSpeak is limiting messages. Retrying…"));
        }
        m_postQueue[flight.item.sch].prepend(flight.item); // before what came after it
    } else if (error == ERROR_ok && !permissionError) {
        for (int id : qAsConst(flight.item.jobs)) {
            auto job = m_uploads.find(id);
            if (job != m_uploads.end() && job->state == UploadState::Posting)
                setUploadState(job.value(), UploadState::Done, i18n::t("Sent"));
        }
    } else {
        failPost(flight.item, postErrorText(permissionError ? MediaError::Permission : mapError(error), message));
    }
    pumpPosts();
}

void Core::failPost(const PostItem& post, const QString& text)
{
    if (post.jobs.isEmpty()) {
        // A caption of its own: the files are posted anyway.
        ts3::printWarning(post.sch, i18n::t("Couldn't send the caption: %1").arg(text));
        return;
    }
    for (int id : post.jobs)
        failUpload(id, text);
}

FloodGovernor& Core::floodGovernor(uint64 sch)
{
    return m_flood[sch];
}

qint64 Core::floodClockMs() const
{
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

void Core::floodStateChanged(uint64 sch)
{
    if (m_postQueue.contains(sch))
        pumpPosts();
}

void Core::forgetBatchIfDone(int batch)
{
    for (const UploadJob& job : qAsConst(m_uploads)) {
        if (job.batch == batch)
            return;
    }
    m_batches.remove(batch);
}

void Core::seedCache(const UploadJob& job, const MediaLink& link)
{
    const QString key = link.key();
    noteRemoteSize(link, job.size);
    MediaEntry e;
    e.link        = link;
    e.kind        = kindForFileName(link.fileName);
    e.localPath   = cachePathFor(link);
    e.isOwnUpload = true;

    QDir().mkpath(QFileInfo(e.localPath).absolutePath());
    QFile::remove(e.localPath);
    if (!linkOrCopy(job.stagingDir + QLatin1Char('/') + job.remoteName, e.localPath))
        return; // the posted message registers it like any other link
    refreshFileTime(e.localPath);
    e.state    = MediaState::Ready;
    e.progress = 1.0;

    const MediaLink  preview = link.previewLink();
    const QByteArray jpeg    = m_uploadExtra.value(job.id).previewJpeg;
    if (!link.previewFile.isEmpty() && preview.isValid()) {
        e.previewPath = cachePathFor(preview);
        if (!jpeg.isEmpty() && writeFile(e.previewPath, jpeg))
            e.previewState = MediaState::Ready;
    }

    if (!m_entries.contains(key))
        m_order.append(key);
    m_entries.insert(key, e);
    invalidateStills(key);
    touch(m_entries[key]);
}

void Core::failUpload(int id, const QString& text)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end())
        return;
    UploadJob& job = it.value();
    if (job.state == UploadState::Done || job.state == UploadState::Failed || job.state == UploadState::Canceled)
        return;
    if (job.transferActive)
        m_uploadsByTransfer.remove(job.transferId);
    job.transferActive = false;
    m_sendQueue.removeAll(id);
    const QString warning = job.pasted ? i18n::t("Couldn't send the pasted image: %1").arg(text)
                                       : i18n::t("Couldn't send “%1”: %2").arg(displayNameFor(job), text);
    ts3::printWarning(job.target.sch, warning);
    setUploadState(job, UploadState::Failed, text);
}

void Core::cancelUpload(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end())
        return;
    UploadJob& job = it.value();
    if (!isPreparing(job.state))
        return;
    if (job.transferActive) {
        if (ts3::funcs.haltTransfer)
            ts3::funcs.haltTransfer(job.target.sch, job.transferId, 1, nullptr);
        m_uploadsByTransfer.remove(job.transferId);
        job.transferActive = false;
    }
    m_sendQueue.removeAll(id);
    // A running probe, folder request or preview upload notices the state change (cleanupUpload
    // halts the preview transfer).
    setUploadState(job, UploadState::Canceled, i18n::t("Canceled"));
}

const UploadJob* Core::upload(int id) const
{
    auto it = m_uploads.constFind(id);
    return it == m_uploads.constEnd() ? nullptr : &it.value();
}

QList<int> Core::uploadIds() const
{
    QList<int> ids = m_uploads.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

void Core::dismissUpload(int id)
{
    const UploadJob* job = upload(id);
    if (job && (job->state == UploadState::Done || job->state == UploadState::Failed || job->state == UploadState::Canceled))
        forgetUpload(id);
}

bool Core::canRetryUpload(int id) const
{
    const UploadJob* job = upload(id);
    return job && job->state == UploadState::Failed && !job->uploaded && QFileInfo(job->sourcePath).isFile();
}

int Core::retryUpload(int id)
{
    if (!canRetryUpload(id))
        return 0;
    UploadJob& job = m_uploads[id];
    if (!ts3::isConnected(job.target.sch)) {
        ts3::printWarning(ts3::currentConnection(), notConnectedText());
        return 0; // stays, to be retried once connected
    }
    const ChatTarget target = job.target;
    SendItem         item   = m_jobItems.value(id);
    item.path               = job.sourcePath;
    item.pasted             = job.pasted;
    item.ownTemp            = job.deleteSource;
    job.deleteSource        = false; // a pasted image now belongs to the new job

    // A caption that nothing of its send carried (every file failed) goes with the retry.
    SendRequest request;
    request.target = target;
    request.items.append(item);
    auto batch = m_batches.find(job.batch);
    if (batch != m_batches.end() && !batch->captionDone && !batch->caption.isEmpty()) {
        request.caption    = batch->caption;
        batch->captionDone = true;
    }
    forgetUpload(id);

    // A pasted image keeps its name; anything else gets a new random part, as a new send would.
    const int newBatch = send(request);
    if (newBatch == 0)
        return 0;
    const PostUnit& unit = m_batches[newBatch].units.first();
    return unit.jobs.value(0);
}

void Core::forgetUpload(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end())
        return;
    const int batch = it->batch;
    releaseSource(it.value());
    m_uploads.erase(it);
    m_jobItems.remove(id);
    m_sendQueue.removeAll(id);
    forgetBatchIfDone(batch);
    emit uploadChanged(id); // upload(id) is nullptr now
    scheduleUploadQueue();
}

// A pasted image is a temporary file of ours: removed with its job (or once the worker that may
// still read it is done).
void Core::releaseSource(UploadJob& job)
{
    if (!job.deleteSource)
        return;
    if (m_probing.contains(job.id))
        m_deleteAfterProbe.insert(job.id, job.sourcePath);
    else
        QFile::remove(job.sourcePath);
    job.deleteSource = false;
}

// waiting: see UploadJob::waiting. Every state change may free an upload slot or let a held chat
// message go, so the queue runs afterwards.
void Core::setUploadState(UploadJob& job, UploadState state, const QString& message, bool waiting)
{
    job.state   = state;
    job.waiting = waiting;
    if (!message.isNull())
        job.message = message;
    const int id = job.id;
    if (state == UploadState::Done || state == UploadState::Failed || state == UploadState::Canceled) {
        cleanupUpload(job);
        // Keep finished jobs around so the toast can show the final state; failed ones until they
        // are dismissed or retried.
        QTimer::singleShot(state == UploadState::Failed ? kFailedJobLingerMs : kJobLingerMs, this, [this, id] { forgetUpload(id); });
    }
    emit uploadChanged(id);
    scheduleUploadQueue();
}

void Core::scheduleUploadQueue()
{
    if (m_uploadQueueScheduled)
        return;
    m_uploadQueueScheduled = true;
    // Deferred: state changes happen deep inside other upload steps.
    QMetaObject::invokeMethod(this, [this] {
        m_uploadQueueScheduled = false;
        runUploadQueue();
    }, Qt::QueuedConnection);
}

// Starts waiting uploads in free slots and posts the held messages whose earlier files are done.
void Core::runUploadQueue()
{
    while (m_uploadsByTransfer.size() < kMaxParallelUploads && !m_sendQueue.isEmpty()) {
        const int id = m_sendQueue.takeFirst();
        auto      it = m_uploads.constFind(id);
        if (it != m_uploads.constEnd() && it->state == UploadState::Preparing && it->waiting)
            startSend(id);
    }

    // Held chat messages whose turn has come (2.2: per send unit, an album or a file).
    pumpPostUnits();
}

void Core::cleanupUpload(UploadJob& job)
{
    // A preview uploaded for a file that never made it to the server is referenced by no message:
    // don't leave it in the channel's file browser. (Once the file is up, both stay.)
    const auto xt            = m_uploadExtra.constFind(job.id);
    const bool removePreview = xt != m_uploadExtra.constEnd() && !xt->mainUploaded && !job.previewRemotePath.isEmpty()
                               && (job.state == UploadState::Failed || job.state == UploadState::Canceled);
    abortPreviewUpload(job);
    m_uploadExtra.remove(job.id);
    if (removePreview) {
        deleteRemoteFile(job.target.sch, job.channelId, job.previewRemotePath);
        job.previewRemotePath.clear();
    }

    // While the worker copies or probes the staged file it cannot be deleted: the copy is stopped
    // and onProbed() removes it (and a pasted source it was still reading) once the worker is done.
    const bool working = m_probing.contains(job.id);
    if (working) {
        if (const auto cancel = m_stagingCancel.value(job.id))
            cancel->store(true);
    } else {
        removeStaging(job.stagingDir);
    }
    job.stagingDir.clear();
    // A failed pasted image stays for a retry until its job is dismissed (forgetUpload).
    if (!(job.pasted && job.state == UploadState::Failed))
        releaseSource(job);
}

void Core::deleteRemoteFile(uint64 sch, uint64 channelId, const QString& path)
{
    if (!ts3::funcs.requestDeleteFile || !ts3::isConnected(sch)) {
        ts3::log(QStringLiteral("Could not remove %1 from the file browser: not connected").arg(path), LogLevel_WARNING, sch);
        return;
    }
    const QByteArray file    = path.toUtf8();
    const char*      files[] = {file.constData(), nullptr};
    const QString    rc      = registerOp(OpType::RemoteDelete, path, 0);
    const unsigned   err     = ts3::funcs.requestDeleteFile(sch, channelId, "", files, rc.toUtf8().constData());
    if (err != ERROR_ok) {
        forgetOp(rc);
        ts3::log(QStringLiteral("Could not remove %1 from the file browser: %2").arg(path, ts3::errorText(err)), LogLevel_WARNING, sch);
    }
}

// ============================================================================================
// Progress
// ============================================================================================

void Core::ensureProgressTimer()
{
    if (m_progressTimer && !m_progressTimer->isActive())
        m_progressTimer->start();
}

void Core::updateProgress()
{
    // Collect first: receivers of entryChanged/uploadChanged may call back into Core.
    QStringList                      progressed;
    QStringList                      oversized;         // automatic downloads larger than the auto-download limit
    QVector<QPair<QString, quint64>> changed;           // the file on the server is not the one the link describes
    QStringList                      oversizedPreviews; // preview transfers above 5 MB
    for (auto it = m_downloadsByTransfer.constBegin(); it != m_downloadsByTransfer.constEnd(); ++it) {
        auto   e     = m_entries.find(it.value());
        uint64 done  = 0;
        uint64 total = 0;
        if (e == m_entries.end() || ts3::funcs.getTransferFileSize(it.key(), &total) != ERROR_ok || total == 0)
            continue;
        noteRemoteSize(e->link, total);
        if (e->link.size != 0 && total != e->link.size) {
            changed.append({it.value(), total});
            continue;
        }
        if (m_autoDownloads.contains(it.value()) && total > autoDownloadLimit(e.value())) {
            oversized.append(it.value());
            continue;
        }
        if (ts3::funcs.getTransferFileSizeDone(it.key(), &done) != ERROR_ok)
            continue;
        const double p = qBound(0.0, static_cast<double>(done) / static_cast<double>(total), 1.0);
        if (p - e->progress >= 0.02) {
            e->progress = p;
            progressed.append(it.value());
        }
    }
    for (auto it = m_previewsByTransfer.constBegin(); it != m_previewsByTransfer.constEnd(); ++it) {
        uint64 total = 0;
        if (ts3::funcs.getTransferFileSize(it.key(), &total) == ERROR_ok && total > kMaxPreviewBytes)
            oversizedPreviews.append(it.value());
    }
    QList<int> uploads;
    for (auto it = m_uploadsByTransfer.constBegin(); it != m_uploadsByTransfer.constEnd(); ++it) {
        auto   job   = m_uploads.find(it.value());
        uint64 done  = 0;
        uint64 total = 0;
        if (job == m_uploads.end() || ts3::funcs.getTransferFileSizeDone(it.key(), &done) != ERROR_ok || ts3::funcs.getTransferFileSize(it.key(), &total) != ERROR_ok
            || total == 0)
            continue;
        const double p = qBound(0.0, static_cast<double>(done) / static_cast<double>(total), 1.0);
        if (p - job->progress >= 0.01) {
            job->progress = p;
            uploads.append(job->id);
        }
    }

    for (const QString& key : qAsConst(progressed)) {
        auto it = m_entries.find(key);
        if (it != m_entries.end())
            touch(it.value());
    }
    for (const QString& key : qAsConst(oversized))
        abortOversizedAutoDownload(key);
    for (const auto& c : qAsConst(changed))
        abortChangedDownload(c.first, c.second);
    for (const QString& key : qAsConst(oversizedPreviews)) {
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->previewState != MediaState::Downloading)
            continue;
        haltDownload(m_previewSch.value(key), it->previewTransferId);
        failPreviewDownload(key, QStringLiteral("the preview is larger than 5 MB"));
    }
    for (int id : qAsConst(uploads))
        emit uploadChanged(id);

    if (m_progressTimer && m_downloadsByTransfer.isEmpty() && m_previewsByTransfer.isEmpty() && m_uploadsByTransfer.isEmpty())
        m_progressTimer->stop();
}

// ============================================================================================
// TeamSpeak events
// ============================================================================================

QString Core::registerOp(OpType type, const QString& key, int uploadId, bool preview)
{
    const QString rc = ts3::newReturnCode();
    PendingOp     op;
    op.type     = type;
    op.key      = key;
    op.uploadId = uploadId;
    op.preview  = preview;
    m_ops.insert(rc, op);
    QMutexLocker lock(&m_returnCodesMutex);
    m_returnCodes.insert(rc);
    return rc;
}

void Core::setOpTransfer(const QString& returnCode, anyID transferId)
{
    auto it = m_ops.find(returnCode);
    if (it != m_ops.end())
        it->transferId = transferId;
}

void Core::forgetOp(const QString& returnCode)
{
    m_ops.remove(returnCode);
}

bool Core::isOwnReturnCode(const QString& returnCode) const
{
    QMutexLocker lock(&m_returnCodesMutex);
    return m_returnCodes.contains(returnCode);
}

void Core::onTextMessage(uint64 sch, const QString& message, const QString& senderUid)
{
    Q_UNUSED(sch);
    for (const MediaLink& link : MediaLink::findInMessage(message)) {
        ensure(link);
        // 2.2 album: the first sender of an album owns it (ChatIntegration groups by it).
        if (link.hasAlbum())
            m_albums.note(link.serverUid, link.albumId, link.albumIndex, link.albumCount, link.key(), senderUid);
    }
}

// 2.2 album
QString Core::albumSender(const QString& serverUid, quint32 albumId, int index, const QString& key) const
{
    return m_albums.senderOf(serverUid, albumId, index, key);
}

void Core::onServerError(uint64 sch, unsigned int error, const QString& returnCode, const QString& message, bool permissionError)
{
    auto it = m_ops.find(returnCode);
    if (it == m_ops.end())
        return;
    const PendingOp  op     = it.value();
    const bool       ok     = error == ERROR_ok && !permissionError;
    const MediaError mapped = permissionError ? MediaError::Permission : mapError(error);
    forgetOp(returnCode);

    switch (op.type) {
    case OpType::Download: {
        if (ok)
            break;
        // Only the attempt this answer belongs to; a retry may already be running.
        auto e = m_entries.find(op.key);
        if (e == m_entries.end())
            break;
        if (op.preview) {
            if (e->previewState == MediaState::Downloading && e->previewTransferId == op.transferId)
                failPreviewDownload(op.key, downloadErrorText(mapped, message), isInterruption(error));
        } else if (e->state == MediaState::Downloading && e->transferId == op.transferId) {
            failDownload(op.key, mapped, message, isInterruption(error));
        }
        break;
    }

    case OpType::MakeDirectory:
        onDirectoryReady(op.uploadId, op.preview, ok || error == ERROR_file_already_exists);
        break;

    case OpType::Upload: {
        if (ok)
            break;
        if (op.preview) {
            auto xt = m_uploadExtra.constFind(op.uploadId);
            if (xt != m_uploadExtra.constEnd() && xt->previewActive && xt->previewTransferId == op.transferId)
                finishPreviewUpload(op.uploadId, false, uploadErrorText(mapped, message), error);
            break;
        }
        // Only the attempt this answer belongs to (a renamed attempt may already be running).
        auto job = m_uploads.constFind(op.uploadId);
        if (job == m_uploads.constEnd() || !job->transferActive || job->transferId != op.transferId)
            break;
        if (error == ERROR_file_already_exists && !permissionError)
            resendWithNewName(op.uploadId, op.transferId);
        else
            failUpload(op.uploadId, uploadErrorText(mapped, message));
        break;
    }

    case OpType::RemoteDelete:
        if (!ok)
            ts3::log(QStringLiteral("Could not remove %1 from the file browser: %2").arg(op.key, message), LogLevel_WARNING, sch);
        break;

    case OpType::PostMessage:
        finishPost(returnCode, error, message, permissionError);
        break;
    }
}

void Core::onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch)
{
    Q_UNUSED(sch);
    const bool complete = status == ERROR_file_transfer_complete;
    if (auto it = m_downloadsByTransfer.constFind(transferId); it != m_downloadsByTransfer.constEnd()) {
        const QString key = it.value();
        if (complete) {
            // Transfer ids may be reused while we wait for the file to be written.
            m_downloadsByTransfer.remove(transferId);
            finishWhenWritten(key, false, 0);
        } else {
            failDownload(key, mapError(status), message, isInterruption(status));
        }
        return;
    }
    if (auto it = m_previewsByTransfer.constFind(transferId); it != m_previewsByTransfer.constEnd()) {
        const QString key = it.value();
        if (complete) {
            m_previewsByTransfer.remove(transferId);
            finishWhenWritten(key, true, 0);
        } else {
            failPreviewDownload(key, downloadErrorText(mapError(status), message), isInterruption(status));
        }
        return;
    }
    if (auto it = m_uploadsByTransfer.constFind(transferId); it != m_uploadsByTransfer.constEnd()) {
        const int id = it.value();
        if (complete)
            finishUpload(id);
        else if (status == ERROR_file_already_exists)
            resendWithNewName(id, transferId);
        else if (status != ERROR_file_transfer_canceled)
            failUpload(id, uploadErrorText(mapError(status), message));
        return;
    }
    if (auto it = m_previewUploadsByTransfer.constFind(transferId); it != m_previewUploadsByTransfer.constEnd()) {
        const int id = it.value();
        finishPreviewUpload(id, complete, uploadErrorText(mapError(status), message), status);
    }
}

void Core::onConnectionLost(uint64 sch)
{
    // The main file and the preview of an entry may use different connections (the same server
    // open in two tabs). Both are restarted once the server is reachable again (noteInterrupted).
    QStringList mains;
    QStringList previews;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->sch == sch && (it->state == MediaState::Downloading || it->state == MediaState::Queued))
            mains.append(it.key());
        if ((it->previewState == MediaState::Downloading || it->previewState == MediaState::Queued) && m_previewSch.value(it.key()) == sch)
            previews.append(it.key());
    }
    for (const QString& key : qAsConst(mains))
        failDownload(key, MediaError::NotConnected, {});
    for (const QString& key : qAsConst(previews))
        failPreviewDownload(key, QStringLiteral("disconnected"), true);

    QList<int> ids;
    QList<int> held; // uploaded, their chat message still waiting for earlier files (or in the post queue)
    for (const auto& job : qAsConst(m_uploads)) {
        if (job.target.sch != sch)
            continue;
        if (isPreparing(job.state))
            ids.append(job.id);
        else if (job.state == UploadState::Posting && job.waiting)
            held.append(job.id);
    }
    // Messages that were waiting for their turn won't be sent on this connection any more.
    for (const PostItem& post : m_postQueue.take(sch)) {
        for (int id : post.jobs)
            held.append(id);
    }
    std::sort(ids.begin(), ids.end());
    std::sort(held.begin(), held.end());
    for (int id : qAsConst(ids))
        failUpload(id, uploadErrorText(MediaError::NotConnected));
    for (int id : qAsConst(held))
        failUpload(id, postErrorText(MediaError::NotConnected));
    m_flood.remove(sch); // a new connection starts with a fresh budget
}

// ============================================================================================
// Cache
// ============================================================================================

quint64 Core::cacheSize() const
{
    quint64      total = 0;
    QDirIterator it(cacheDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += static_cast<quint64>(it.fileInfo().size());
    }
    return total;
}

void Core::clearCache()
{
    // Files in use (playing, open in the viewer) and running transfers stay.
    const QString root        = cacheDir();
    const QString partialRoot = pathKey(root + QStringLiteral("/.partial")) + QLatin1Char('/');
    QSet<QString> keep;
    for (auto use = m_inUse.cbegin(); use != m_inUse.cend(); ++use) {
        if (const MediaEntry* e = entry(use.key())) {
            keep.insert(pathKey(e->localPath));
            if (!e->previewPath.isEmpty())
                keep.insert(pathKey(e->previewPath));
        }
    }

    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString pk   = pathKey(path);
        if (!pk.startsWith(partialRoot) && !keep.contains(pk))
            QFile::remove(path);
    }
    removeEmptyDirectories(root);
    m_stills.clear();
    m_stillJobs.clear(); // decodes still running are dropped when they finish
    m_usedAt.clear();
    m_evictionRearmed.clear();

    QStringList changed;
    for (auto e = m_entries.begin(); e != m_entries.end(); ++e) {
        bool modified = false;
        if ((e->state == MediaState::Ready && !QFileInfo::exists(e->localPath)) || e->state == MediaState::Failed) {
            e->state    = MediaState::Idle;
            e->progress = 0.0;
            e->error    = MediaError::None;
            e->errorText.clear();
            modified = true;
        }
        if ((e->previewState == MediaState::Ready && !QFileInfo::exists(e->previewPath)) || e->previewState == MediaState::Failed) {
            e->previewState = MediaState::Idle;
            modified        = true;
        }
        if (modified) {
            changed.append(e.key());
            // Media still shown in a chat loads again (as for a new link) once it is rendered or
            // rescanned, instead of staying a placeholder for the rest of the session.
            m_rearm.insert(e.key());
        }
    }
    for (const QString& key : qAsConst(changed)) {
        auto e = m_entries.find(key);
        if (e != m_entries.end())
            touch(e.value());
    }
}

void Core::openCacheFolder() const
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(cacheDir()));
}

void Core::applyCacheLimit()
{
    scheduleCacheLimit(QString());
}

void Core::scheduleCacheLimit(const QString& justFinishedKey)
{
    if (!justFinishedKey.isEmpty())
        m_recentlyFinished.insert(justFinishedKey);
    if (!m_cacheTimer) {
        enforceCacheLimit();
        return;
    }
    if (!m_cacheTimer->isActive())
        m_cacheTimer->start();
}

void Core::enforceCacheLimit()
{
    QSet<QString> recent;
    recent.swap(m_recentlyFinished);

    const quint64 limit = megabytes(Settings::instance().cacheLimitMB);
    if (limit == 0)
        return; // no limit configured

    struct CachedFile {
        QString path;
        qint64  mtime = 0;
        quint64 size  = 0;
    };
    const QString       root        = cacheDir();
    const QString       partialRoot = pathKey(root + QStringLiteral("/.partial")) + QLatin1Char('/');
    QVector<CachedFile> files;
    quint64             total = 0;
    QDirIterator        it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (pathKey(path).startsWith(partialRoot))
            continue;
        const QFileInfo fi   = it.fileInfo();
        const quint64   size = static_cast<quint64>(fi.size());
        files.append({path, fi.lastModified().toMSecsSinceEpoch(), size});
        total += size;
    }
    if (total <= limit)
        return;

    // Which entry each file belongs to, and which files must stay (in use / just finished).
    struct Owner {
        QString key;
        bool    preview = false;
    };
    QMultiHash<QString, Owner> owners; // several links can share a preview file
    QSet<QString>              keep;
    for (auto e = m_entries.cbegin(); e != m_entries.cend(); ++e) {
        const bool spare = m_inUse.contains(e.key()) || recent.contains(e.key());
        owners.insert(pathKey(e->localPath), {e.key(), false});
        if (spare)
            keep.insert(pathKey(e->localPath));
        if (!e->previewPath.isEmpty()) {
            owners.insert(pathKey(e->previewPath), {e.key(), true});
            if (spare)
                keep.insert(pathKey(e->previewPath));
        }
    }

    std::sort(files.begin(), files.end(), [](const CachedFile& a, const CachedFile& b) { return a.mtime < b.mtime; });

    const quint64 before = total;
    QSet<QString> changed;
    for (const CachedFile& file : qAsConst(files)) {
        if (total <= limit)
            break;
        const QString pk = pathKey(file.path);
        if (keep.contains(pk) || !QFile::remove(file.path))
            continue;
        total -= file.size;
        m_usedAt.remove(file.path);

        for (auto owner = owners.constFind(pk); owner != owners.constEnd() && owner.key() == pk; ++owner) {
            auto e = m_entries.find(owner->key);
            if (e == m_entries.end())
                continue;
            if (owner->preview) {
                if (e->previewState == MediaState::Ready) {
                    e->previewState = MediaState::Idle;
                    invalidateStill(owner->key, MediaStill::Preview);
                    changed.insert(owner->key);
                }
            } else if (e->state == MediaState::Ready) {
                e->state    = MediaState::Idle;
                e->progress = 0.0;
                invalidateStill(owner->key, MediaStill::Full);
                changed.insert(owner->key);
            }
        }
    }
    removeEmptyDirectories(root);
    ts3::log(QStringLiteral("Media cache above its %1 limit: %2 -> %3").arg(formatSize(limit), formatSize(before), formatSize(total)), LogLevel_INFO);

    for (const QString& key : qAsConst(changed)) {
        // Shown again in a chat: fetched again, but only once per entry (until the cache is
        // cleared), so a cache too small for what is on screen cannot evict and refetch forever.
        if (!m_evictionRearmed.contains(key)) {
            m_evictionRearmed.insert(key);
            m_rearm.insert(key);
        }
        auto e = m_entries.find(key);
        if (e != m_entries.end())
            touch(e.value());
    }
}

// ============================================================================================
// Helpers
// ============================================================================================

MediaError Core::mapError(unsigned int error)
{
    switch (error) {
    case ERROR_ok:
        return MediaError::None;
    case ERROR_permissions_client_insufficient:
        return MediaError::Permission;
    case ERROR_channel_invalid_password:
        return MediaError::Password;
    case ERROR_file_not_found:
        return MediaError::NotFound;
    case ERROR_file_transfer_server_quota_exceeded:
    case ERROR_file_transfer_client_quota_exceeded:
    case ERROR_file_transfer_channel_quota_exceeded:
    case ERROR_file_no_space_left_on_device:
        return MediaError::Quota;
    case ERROR_not_connected:
    case ERROR_connection_lost:
        return MediaError::NotConnected;
    default:
        return MediaError::Other;
    }
}

QString displayNameFor(const UploadJob& job)
{
    if (!job.displayName.isEmpty())
        return displayFileName(job.displayName);
    return job.pasted ? i18n::t("Pasted image") : displayFileName(QFileInfo(job.sourcePath).fileName());
}

QString notConnectedText()
{
    return i18n::t("You're not connected to a server. Connect to one to send files.");
}
