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
#include <utility>

#include "audioplayback.h" // 2.2 audio
#include "blurhash.h"
#include "filenames.h" // 2.2 drag-out
#include "hashing.h" // 2.2 sha
#include "i18n.h"
#include "ownedtimer.h"
#include "settings.h"
#include "video/mftranscode.h" // 2.4 compress
#include "video/mfvideo.h"     // 2.4 compress: mf::available()

#include <QStorageInfo> // 2.4 compress: free space for the compressed copy

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
constexpr int     kMaxFloodRetries      = 5; // folder / upload requests sent again after 0x020c (each after the pause)
constexpr qint64  kExportMaxAgeMs       = 24LL * 3600 * 1000; // 2.2 drag-out: staged copies are kept this long
constexpr qint64  kMaxExportCopyBytes   = 64LL * 1024 * 1024; // 2.2 drag-out: copied (when no hard link) only up to this
constexpr int     kShowCheckMs          = 300; // 2.2 sha: "Checking file…" only for checks that take longer

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

QString uploadLimitText(int limitMB, bool serverLimit = false)
{
    // 2.2 per-server settings: a server's own limit is changed where it was set.
    if (serverLimit)
        return i18n::t("This file is larger than this server's %1 MB upload limit. You can change it in Settings → Servers.").arg(limitMB);
    return i18n::t("This file is larger than your %1 MB upload limit. You can raise the limit in Settings → Sending.").arg(limitMB);
}

// 2.2 per-server settings: the server has its own upload size limit.
bool hasOwnUploadLimit(const QString& serverUid)
{
    const ServerOverrides* own = Settings::instance().overridesFor(serverUid);
    return own && own->uploadMaxMB.has_value();
}

// 2.2 drag-out: the Windows Mark-of-the-Web, as browsers add it to downloads. Written only when the
// file has no Zone.Identifier stream yet. Not through QFile: alternate data streams need Win32 calls.
bool markOfTheWeb(const QString& path)
{
    const QString stream = QDir::toNativeSeparators(path) + QLatin1String(":Zone.Identifier");
    const HANDLE  file   = CreateFileW(reinterpret_cast<LPCWSTR>(stream.utf16()), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_EXISTS; // already marked
    static const char kZone[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    DWORD             written = 0;
    const BOOL        ok      = WriteFile(file, kZone, static_cast<DWORD>(sizeof(kZone) - 1), &written, nullptr);
    CloseHandle(file);
    return ok && written == sizeof(kZone) - 1;
}

// An upload still on its way to the server (its chat message can't be composed yet).
bool isPreparing(UploadState state)
{
    return state == UploadState::Preparing || state == UploadState::Compressing || state == UploadState::Uploading;
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

// 2.2 sha: a preview's bytes for its ph check (at most kMaxPreviewBytes are read; previews are never
// larger, see finishPreviewDownload). Empty if it can't be read.
QByteArray readPreview(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(static_cast<qint64>(kMaxPreviewBytes) + 1);
}

// Previews are staged next to (not inside) the main staging folder so names can never collide.
QString previewStagingDir(const QString& stagingDir)
{
    return stagingDir + QStringLiteral(".pv");
}

// 2.4 compress: where a video's compressed copy is written, next to its staging folder.
QString compressDirFor(const QString& stagingDir)
{
    return stagingDir + QStringLiteral(".cz");
}

void removeStaging(const QString& stagingDir)
{
    if (stagingDir.isEmpty())
        return;
    QDir(stagingDir).removeRecursively();
    QDir(previewStagingDir(stagingDir)).removeRecursively();
    QDir(compressDirFor(stagingDir)).removeRecursively(); // 2.4 compress
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
    m_transcodePool.setMaxThreadCount(1); // 2.4 compress: one video at a time, in the order sent
    m_clock.start();

    // 2.2 sha: downloads whose link has a sha are checked on the verifier's own threads before they
    // are cached. Its second pass waits while TeamSpeak still has the file open for writing.
    fileverify::Environment check;
    check.isBeingWritten = [](const QString& path) { return isOpenForWriting(path); };
    m_verifier           = new fileverify::Verifier(check, this);
    connect(m_verifier, &fileverify::Verifier::progressChanged, this, &Core::onCheckProgress);
    connect(m_verifier, &fileverify::Verifier::finished, this, &Core::onCheckFinished);
}

Core::~Core()
{
    // 2.2 sha: running checks stop within one chunk read; nothing is reported any more.
    if (m_verifier)
        m_verifier->shutdown();
    // 2.4 compress: a running transcode stops within about a frame (no Finalize), a queued one returns at
    // once; joined first, its Media Foundation objects released on its own thread.
    for (const CompressTask& task : qAsConst(m_compressing)) {
        if (task.control)
            task.control->cancel.store(true);
    }
    m_transcodePool.clear();
    m_transcodePool.waitForDone();
    for (const CompressTask& task : qAsConst(m_compressing))
        QDir(task.outDir).removeRecursively();
    m_compressing.clear();

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
    // ... and the next folder request or upload start (same counter).
    m_fileTimer = new QTimer(this);
    m_fileTimer->setSingleShot(true);
    connect(m_fileTimer, &QTimer::timeout, this, &Core::runFileQueue);

    // Leftovers from a previous session (crash, client closed mid-transfer).
    QDir(cacheDir() + QStringLiteral("/.partial")).removeRecursively();
    QDir(ts3::dataDir() + QStringLiteral("/upload")).removeRecursively();
    QDir(ts3::dataDir() + QStringLiteral("/paste")).removeRecursively();
    pruneExports(kExportMaxAgeMs); // 2.2 drag-out
    QDir(ts3::dataDir() + QStringLiteral("/edit")).removeRecursively(); // 2.2 editor: edited copies

    // The limit may have been lowered while TeamSpeak was closed.
    singleShotOwned(kStartupCacheCheckMs, this, [this] { enforceCacheLimit(); });
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
    // 2.2 sha: links with a sha use the whole key (80 bits). Only checked files are stored under it, and
    // 8 hex digits would let a made-up hash be tuned to share the real file's cache name.
    const int keyChars = link.isTsMedia() && link.sha256.size() == fileverify::kShaBytes ? 20 : 8;
    return cacheDir() + QLatin1Char('/') + server + QLatin1Char('/') + QString::number(link.channelId) + QLatin1Char('/') + link.key().left(keyChars)
           + QLatin1Char('_') + localFileName(link.fileName);
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
    // 2.2 spoiler: one link with sp=1 makes the file a spoiler for the session (key() ignores sp).
    const bool newSpoiler = m_spoilers.noteSighting(key, link.spoiler && spoiler::appliesTo(kindForFileName(link.fileName)));
    if (m_entries.contains(key)) {
        // Seen while inline previews were off, or its files left the cache (cleared / evicted) while
        // it is still in a chat: start what would start for a new link.
        if (Settings::instance().inlinePreviews) {
            const bool pending = m_autoPending.remove(key);
            const bool rearm   = m_rearm.remove(key);
            if (pending || rearm)
                startAutoDownloads(key);
        }
        if (newSpoiler)
            emit entryChanged(key); // 2.2 spoiler: its previews get the cover
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
            // 2.2 sha: only files that matched are ever stored under a sha key, so this one did; it
            // isn't hashed again.
            e.check.verified = !link.sha256.isEmpty();
        }
    }

    const MediaLink preview = link.previewLink();
    if (!link.previewFile.isEmpty() && preview.isValid()) {
        e.previewPath = cachePathFor(preview);
        const QFileInfo pf(e.previewPath);
        if (pf.isFile() && hasUnwrittenTail(e.previewPath, e.previewPath))
            QFile::remove(e.previewPath);
        else if (pf.isFile() && static_cast<quint64>(pf.size()) <= kMaxPreviewBytes && previewFileMatches(e.previewPath, link.previewSha)) // 2.2 sha: ph
            e.previewState = MediaState::Ready;
    }

    m_entries.insert(key, e);
    m_order.append(key);
    refuseForgedHash(key); // 2.2 sha: a hash that contradicts this file's known one is refused at once

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
    // 2.2 per-server settings: the server the download goes to (where the data is spent) decides.
    const Settings  s          = Settings::instance().forServer(e.link.serverUid);
    const bool      isImage    = isPreviewableImage(e.kind);
    const bool      isVideo    = e.kind == MediaKind::Video;
    const quint64   imageLimit = megabytes(s.autoDownloadMaxMB);
    const quint64   videoLimit = megabytes(s.videoAutoDownloadMB);
    // 2.2 audio: audio files have their own rule (audioplayback.h); voice messages (vm) download like images.
    const bool    isAudio    = e.kind == MediaKind::Audio;
    const quint64 audioLimit = isAudio ? audioplayback::autoDownloadLimit(s, e.link.voice) : 0;
    if (isImage)
        e.tooLargeForAuto = e.link.size > imageLimit;
    else if (isVideo)
        e.tooLargeForAuto = videoLimit > 0 && e.link.size > videoLimit;
    else if (isAudio)
        e.tooLargeForAuto = audioLimit > 0 && e.link.size > audioLimit;

    // Images that are already cached do not need their preview; video posters are always useful.
    const bool wantPreview = e.previewState == MediaState::Idle && !e.previewPath.isEmpty() && (e.state != MediaState::Ready || !isImage);
    bool       wantMain    = false;
    if (e.state == MediaState::Idle) {
        if (isImage)
            wantMain = s.autoDownloadImages && !e.tooLargeForAuto;
        else if (isVideo)
            wantMain = videoLimit > 0 && e.link.size <= videoLimit;
        else if (isAudio) // 2.2 audio
            wantMain = audioLimit > 0 && e.link.size <= audioLimit;
        // 2.2 data saver: the full file waits for a click (previews still load).
        const bool held = wantMain && s.dataSaver;
        if (held)
            wantMain = false;
        if (held != e.heldByDataSaver) {
            e.heldByDataSaver = held;
            if (!wantPreview)
                touch(e); // the card's status line changes
        }
    }

    // 2.2 spoiler: a hidden spoiler loads only what its cover is made from (the preview); the file follows
    // the same rules once it is revealed (setSpoilerRevealed).
    if (wantMain && isSpoilerHidden(key)) {
        wantMain = false;
        m_spoilers.holdDownload(key);
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
        if (e.error == MediaError::NotFound || e.error == MediaError::Mismatch) // 2.2 sha: the same bytes again
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
    if (it->state == MediaState::Failed && (it->error == MediaError::NotFound || it->error == MediaError::Mismatch)) // 2.2 sha
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
    e.heldByDataSaver = false; // 2.2 data saver: fetched now

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
    // 2.2 sha: the real digest of this file became known meanwhile and contradicts the link's.
    if (refuseForgedHash(key))
        return;

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
    floodGovernor(sch).charge(FloodGovernor::Cost::Transfer, floodClockMs()); // the attempt counts either way
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
    if (cached.isFile() && cached.size() > 0 && static_cast<quint64>(cached.size()) <= kMaxPreviewBytes && previewFileMatches(e.previewPath, e.link.previewSha)) { // 2.2 sha: ph
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
    floodGovernor(sch).charge(FloodGovernor::Cost::Transfer, floodClockMs());
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

    // 2.2 sha: a link with a sha is cached only once the file matches it (checked off the GUI thread;
    // onCheckFinished stores it). The transfer slot is free meanwhile.
    if (!found.isEmpty() && startCheck(key, found)) {
        pumpDownloadQueue();
        return;
    }
    storeDownload(key, found);
}

// The tail of a finished download: moves the file from its partial folder into the cache.
void Core::storeDownload(const QString& key, const QString& found)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    MediaEntry&   e       = it.value();
    const QString partial = partialDir(key, false);

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
        singleShotOwned(kPollMs, this, [this, key, preview, attempt] { finishWhenWritten(key, preview, attempt + 1); });
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
    } else if (!e.link.previewSha.isEmpty() && fileverify::previewDigest(readPreview(found)) != e.link.previewSha) {
        // 2.2 sha: not the preview that was sent (ph). Never cached; the placeholder stays.
        fileverify::count(fileverify::Counter::PreviewMismatched);
        ts3::log(LogLevel_WARNING, m_previewSch.value(key), "Preview of %1 doesn't match its checksum; showing the placeholder instead", {ts3::file(e.link.remoteFile())});
        error = QStringLiteral("the preview doesn't match its checksum");
    } else {
        QDir().mkpath(QFileInfo(e.previewPath).absolutePath());
        QFile::remove(e.previewPath);
        m_previewDigests.remove(e.previewPath); // 2.2 sha: new bytes
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
    if (e.kind == MediaKind::Audio) // 2.2 audio
        return audioplayback::autoDownloadLimit(s, e.link.voice);
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
    if (sch && ts3::funcs.haltTransfer) {
        ts3::funcs.haltTransfer(sch, transferId, 1, nullptr);
        m_haltedTransfers.insert(transferId);
    }
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
    // Without the cache prefix and the random part. 2.2: also safe from device names ("CON.txt").
    const QString   name      = filenames::safeLocalFileName(displayNameFor(e->link));

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

// ---- 2.2 spoiler ---------------------------------------------------------------------------------

bool Core::isSpoilerHidden(const QString& key) const
{
    const MediaEntry* e = entry(key);
    return e && spoiler::appliesTo(e->kind) && m_spoilers.isHidden(key, Settings::instance().revealSpoilers);
}

bool Core::isSpoiler(const QString& key) const
{
    const MediaEntry* e = entry(key);
    return e && spoiler::appliesTo(e->kind) && m_spoilers.isSpoiler(key);
}

void Core::setSpoilerRevealed(const QString& key, bool revealed)
{
    if (!m_spoilers.setRevealed(key, revealed))
        return;
    // An automatic download that waited for the reveal starts now (the preview shows its progress).
    if (revealed && m_spoilers.releaseDownload(key) && Settings::instance().inlinePreviews)
        startAutoDownloads(key);
    emit entryChanged(key);
}

void Core::noteShownOpen(const QString& key)
{
    m_spoilers.noteShownOpen(key);
}

void Core::spoilerSettingChanged()
{
    const QStringList keys = m_spoilers.spoilers();
    for (const QString& key : keys) {
        if (!isSpoilerHidden(key) && m_spoilers.releaseDownload(key) && Settings::instance().inlinePreviews)
            startAutoDownloads(key); // shown without blurring now: loads like any other picture
        emit entryChanged(key);
    }
}

// ============================================================================================
// 2.2 drag-out: copies handed to Explorer and other apps
// ============================================================================================

QString Core::prepareExport(const QString& key, QString* error)
{
    const MediaEntry* e = entry(key);
    if (!e || e->state != MediaState::Ready || !QFileInfo(e->localPath).isFile()) {
        if (error)
            *error = QStringLiteral("not downloaded");
        return {};
    }
    const QString localPath = e->localPath;
    const MediaLink link    = e->link;
    pruneExports(kExportMaxAgeMs);

    // Outside the cache folder: the cache size and its eviction never count or touch it.
    const QString root = ts3::dataDir() + QStringLiteral("/export");
    QString       dir;
    for (int attempt = 0; attempt < 5 && dir.isEmpty(); ++attempt) {
        const QString candidate = root + QLatin1Char('/') + uniqueSuffix();
        if (!QFileInfo::exists(candidate) && QDir().mkpath(candidate))
            dir = candidate;
    }

    QString result;
    if (!dir.isEmpty()) {
        const QString target = dir + QLatin1Char('/') + filenames::exportFileName(link, QDir::toNativeSeparators(dir).length() + 1);
        // A hard link is instant and takes no space (Explorer copies it). Copies are for file systems
        // without hard links, and only for files that copy quickly: this runs on the GUI thread.
        if (hardLink(localPath, target) || (QFileInfo(localPath).size() <= kMaxExportCopyBytes && QFile::copy(localPath, target)))
            result = target;
        else
            QDir(dir).removeRecursively();
    }
    if (result.isEmpty()) {
        // The cache file itself: the copy then gets the cache name ("3f9a1c2e_holiday_3f9a1c2e.jpg").
        // No file name: 2.2 log lines name files only through the structured log API (diagnostics).
        ts3::log(QStringLiteral("Could not stage a file for dragging out; the cached file itself is used"), LogLevel_WARNING);
        result = localPath;
    }
    // With a hard link the stream lands on the file record the cache shares, so the cached file is
    // marked too: Save as and Explorer copies (CopyFileW copies streams) keep the mark as well.
    if (!markOfTheWeb(result))
        ts3::log(QStringLiteral("Could not add the downloaded-from-the-internet mark to a dragged-out file (error %1)").arg(GetLastError()), LogLevel_WARNING);
    refreshFileTime(localPath);
    return result;
}

void Core::pruneExports(qint64 maxAgeMs)
{
    const QString root = ts3::dataDir() + QStringLiteral("/export");
    if (!QFileInfo(root).isDir())
        return;
    const QDateTime   oldest  = QDateTime::currentDateTimeUtc().addMSecs(-maxAgeMs);
    const QFileInfoList folders = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& folder : folders) {
        if (folder.lastModified().toUTC() < oldest)
            QDir(folder.absoluteFilePath()).removeRecursively(); // files Explorer still copies stay (locked)
    }
}

// ============================================================================================
// 2.2 data saver
// ============================================================================================

void Core::onDataSaverChanged()
{
    QHash<QString, bool> saving; // server uid -> data saver on (resolved once per server)
    const Settings&      global = Settings::instance();
    auto                 saves  = [&saving, &global](const QString& uid) {
        auto it = saving.constFind(uid);
        if (it == saving.constEnd())
            it = saving.insert(uid, global.forServer(uid).dataSaver);
        return it.value();
    };

    // Collected first: touch() and the transfer calls below may call back into Core.
    QStringList pause;
    QStringList resume;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        const MediaEntry& e = it.value();
        if (saves(e.link.serverUid)) {
            if (m_autoDownloads.contains(it.key()) && (e.state == MediaState::Queued || e.state == MediaState::Downloading))
                pause.append(it.key());
        } else if (e.heldByDataSaver && e.state == MediaState::Idle) {
            resume.append(it.key());
        }
    }

    for (const QString& key : qAsConst(pause)) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue;
        MediaEntry& e = it.value();
        if (e.state == MediaState::Downloading) {
            haltDownload(e.sch, e.transferId);
            if (m_downloadsByTransfer.value(e.transferId) == key)
                m_downloadsByTransfer.remove(e.transferId);
            --m_activeDownloads;
            QDir(partialDir(key, false)).removeRecursively();
        }
        m_downloadQueue.removeAll(key);
        m_autoDownloads.remove(key);
        e.state           = MediaState::Idle;
        e.progress        = 0.0;
        e.transferId      = 0;
        e.openWhenReady   = false;
        e.heldByDataSaver = true;
        touch(e);
    }

    // Started again like media whose files left the cache: once a chat or the viewer shows them
    // (rearmIfNeeded), not every held item of a long chat history at once.
    for (const QString& key : qAsConst(resume)) {
        auto it = m_entries.find(key);
        if (it == m_entries.end())
            continue;
        it->heldByDataSaver = false;
        m_rearm.insert(key);
        touch(it.value());
    }
    if (!pause.isEmpty())
        pumpDownloadQueue();
    if (!pause.isEmpty() || !resume.isEmpty())
        ts3::log(QStringLiteral("Data saver changed: %1 automatic downloads paused, %2 held items released").arg(pause.size()).arg(resume.size()), LogLevel_INFO);
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
    // "new_photo_<hex>" is how pasted pictures are named (uploadImage), and receivers show those as
    // "Pasted image": a file of the user's own called "new photo" keeps its name.
    if (base == QLatin1String("new_photo"))
        base += QLatin1Char('_');

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

    const int batch                = ++m_nextBatch;
    m_batches[batch].caption       = request.caption;
    m_batches[batch].captionOrigin = batch;
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
    if (!request.caption.isEmpty())
        m_captionsPending.insert(batch); // captionSettled() says when it is posted (or can't be)
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
    if (target.serverUid.isEmpty())
        target.serverUid = ts3::serverUid(target.sch);

    // 2.2 per-server settings: the upload folder and size limit of the server it goes to.
    const QString   serverUid = target.serverUid;
    const Settings  s         = Settings::instance().forServer(serverUid);
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
    // 2.4 compress: a video that may be compressed is planned on the worker; its size against the limit
    // is the planner's business (it may be made small enough).
    const videocompress::Options compress  = compressOptions(item.quality, serverUid);
    const bool                   candidate = !item.voice && !item.pasted && videocompress::isCompressibleVideoName(remoteName)
                                           && item.quality != SendQuality::Original && compress.mediaFoundation
                                           && (item.quality != SendQuality::Auto || compress.compressLarge || compress.convertUnplayable);
    j.originalSize = j.size;
    if (j.size > limit && !candidate) {
        failUpload(id, uploadLimitText(s.uploadMaxMB, hasOwnUploadLimit(serverUid)));
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
    const quint64 originalSize = j.size;
    m_pool.start([self, id, copyFrom, staged, nested, previews, cancel, candidate, compress, originalSize] {
        // Jobs wait in the pool's queue until a worker is free; tell the toast this one has started.
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id] {
                if (self)
                    self->markProbeStarted(id);
            }, Qt::QueuedConnection);
        }
        LocalMediaInfo info;
        QByteArray     jpeg;
        bool           probed = false;
        // 2.4 compress: a video that may be compressed is probed and planned where it is: no staging copy
        // of a file that is about to be replaced by a smaller one.
        if (candidate && !cancel->load()) {
            const QString source = copyFrom.isEmpty() ? staged : copyFrom;
            info                 = probeLocalVideo(source, previews);
            jpeg                 = info.preview.isNull() ? QByteArray() : encodePreviewJpeg(info.preview);
            videocompress::VideoFacts facts;
            facts.bytes           = originalSize;
            facts.durationMs      = info.durationMs;
            facts.display         = QSize(info.width, info.height);
            const double measured = info.probed && info.hasVideo ? mf::measuredFrameRate(source) : 0.0;
            facts.fps             = measured > 0.0 ? measured : info.frameRate;
            facts.probed          = info.probed;
            facts.hasVideo        = info.hasVideo;
            facts.decodable       = info.decodable;
            facts.undecodableText = info.probeError;
            facts.hasAudio        = info.hasAudio;
            facts.audioChannels   = info.audioChannels;
            facts.videoCodec      = videocompress::codecId(info.videoCodec);
            facts.extension       = QFileInfo(staged).suffix().toLower();
            const videocompress::Plan plan = videocompress::planCompression(facts, compress);
            if (plan.decision != videocompress::Decision::SendOriginal) {
                if (Core* core = self.data()) {
                    QMetaObject::invokeMethod(core, [self, id, info, jpeg, plan] {
                        if (self)
                            self->onCompressPlanned(id, info, jpeg, plan);
                    }, Qt::QueuedConnection);
                }
                return;
            }
            // Sent as it is: the probe is reused, unless the name isn't a video's (a camcorder .mts goes as a
            // plain file, as before).
            probed = kindForFileName(staged) == MediaKind::Video;
            if (!probed) {
                info = LocalMediaInfo();
                jpeg.clear();
            }
        }
        bool ok = copyFrom.isEmpty() || copyForStaging(copyFrom, staged, cancel.get());
        if (ok && !nested.isEmpty()) {
            QDir().mkpath(QFileInfo(nested).absolutePath());
            ok = linkOrCopy(staged, nested);
        }
        const quint64 size = ok ? static_cast<quint64>(QFileInfo(staged).size()) : 0;
        if (ok && !cancel->load() && !probed) {
            info = probeLocalMedia(staged, previews);
            jpeg = info.preview.isNull() ? QByteArray() : encodePreviewJpeg(info.preview);
        }
        // 2.2 sha: the final staged bytes, hashed once, right before the preview and the file are
        // uploaded (queued before onProbed, so it arrives first).
        if (ok && !cancel->load()) {
            const fileverify::StagedDigest digest = fileverify::finalizeStaged(staged, jpeg, cancel.get());
            if (Core* core = self.data()) {
                QMetaObject::invokeMethod(core, [self, id, digest] {
                    if (self)
                        self->onStagedFinalized(id, digest);
                }, Qt::QueuedConnection);
            }
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
    // changed since it was checked). 2.2 per-server settings: the limit of the server it goes to.
    const QString  serverUid = it->target.serverUid;
    const Settings s         = Settings::instance().forServer(serverUid);
    if (stagedSize == 0) {
        failUpload(id, i18n::t("This file is empty."));
        return;
    }
    if (stagedSize > megabytes(s.uploadMaxMB)) {
        failUpload(id, uploadLimitText(s.uploadMaxMB, hasOwnUploadLimit(serverUid)));
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

QString Core::remoteDirKey(uint64 sch, uint64 channelId, const QString& dir)
{
    return QString::number(sch) + QLatin1Char('|') + QString::number(channelId) + QLatin1Char('|') + dir;
}

void Core::createRemoteDirectory(int id, bool previews)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    UploadJob& job = it.value();

    const QString dir = previews ? previewDirFor(job) : job.remoteDir;
    const QString key = remoteDirKey(job.target.sch, job.channelId, dir);
    // Asked once per folder and connection (each request costs flood points, S0).
    const auto known = m_remoteDirs.constFind(key);
    if (known != m_remoteDirs.constEnd()) {
        onDirectoryReady(id, previews, known.value());
        return;
    }
    if (!takeFileSlot(job.target.sch, id, previews ? FileStep::PreviewFolder : FileStep::MainFolder))
        return;
    const QString  rc  = registerOp(OpType::MakeDirectory, key, id, previews); // the answer records the folder
    const unsigned err = ts3::funcs.requestCreateDirectory(job.target.sch, job.channelId, "", dir.toUtf8().constData(), rc.toUtf8().constData());
    floodGovernor(job.target.sch).charge(FloodGovernor::Cost::Mkdir, floodClockMs());
    if (err != ERROR_ok) {
        forgetOp(rc);
        onDirectoryReady(id, previews, false);
        return;
    }
    // Not every server answers a mkdir for an existing folder; don't wait forever.
    singleShotOwned(kMkdirTimeoutMs, this, [this, rc, id, previews, key] {
        if (!m_ops.contains(rc))
            return;
        forgetOp(rc);
        m_remoteDirs.insert(key, true);
        onDirectoryReady(id, previews, true);
    });
}

// 2.2: a folder request or an upload start of upload id goes now if the governor of sch allows it,
// after those already waiting; otherwise it waits (the job says so) and runFileQueue runs it later.
bool Core::takeFileSlot(uint64 sch, int id, FileStep step)
{
    const bool                 head = std::exchange(m_fileStepHead, false);
    const FloodGovernor::Cost  cost = step == FileStep::MainFolder || step == FileStep::PreviewFolder ? FloodGovernor::Cost::Mkdir : FloodGovernor::Cost::Transfer;
    const auto                 queue = m_fileQueue.constFind(sch);
    const bool                 first = head || queue == m_fileQueue.constEnd() || queue->isEmpty();
    if (first && floodGovernor(sch).generalReady(cost, floodClockMs()))
        return true;
    QList<FileWait>& waiting = m_fileQueue[sch];
    if (head)
        waiting.prepend({id, step});
    else
        waiting.append({id, step});
    auto job = m_uploads.find(id);
    if (job != m_uploads.end() && job->state == UploadState::Preparing && !job->waiting)
        setUploadState(job.value(), UploadState::Preparing, i18n::t("Waiting to upload…"), true);
    scheduleFileQueue();
    return false;
}

void Core::runFileQueue()
{
    for (const uint64 sch : m_fileQueue.keys()) {
        for (;;) {
            auto queue = m_fileQueue.find(sch);
            if (queue == m_fileQueue.end())
                break;
            if (queue->isEmpty()) {
                m_fileQueue.erase(queue);
                break;
            }
            const FileWait next = queue->first();
            const auto     cost = next.step == FileStep::MainFolder || next.step == FileStep::PreviewFolder ? FloodGovernor::Cost::Mkdir : FloodGovernor::Cost::Transfer;
            if (!ts3::isConnected(sch) || !floodGovernor(sch).generalReady(cost, floodClockMs()))
                break;
            queue->removeFirst();
            // The step checks its job again (it may have been canceled meanwhile) and asks for its slot,
            // which it gets: it is the head and the governor allows it.
            m_fileStepHead = true;
            switch (next.step) {
            case FileStep::MainFolder:
                createRemoteDirectory(next.id, false);
                break;
            case FileStep::PreviewFolder:
                createRemoteDirectory(next.id, true);
                break;
            case FileStep::Preview:
                startPreviewSend(next.id);
                break;
            case FileStep::Main:
                startSend(next.id);
                break;
            }
            m_fileStepHead = false;
        }
    }
    scheduleFileQueue();
}

void Core::scheduleFileQueue()
{
    if (!m_fileTimer)
        return;
    const qint64 now  = floodClockMs();
    qint64       wake = -1;
    for (auto it = m_fileQueue.cbegin(); it != m_fileQueue.cend(); ++it) {
        if (it->isEmpty() || !ts3::isConnected(it.key()))
            continue;
        const FileStep step = it->first().step;
        const auto     cost = step == FileStep::MainFolder || step == FileStep::PreviewFolder ? FloodGovernor::Cost::Mkdir : FloodGovernor::Cost::Transfer;
        const qint64   at   = floodGovernor(it.key()).nextGeneralCheckMs(cost, now);
        wake                = wake < 0 ? at : qMin(wake, at);
    }
    if (wake < 0)
        m_fileTimer->stop();
    else
        m_fileTimer->start(static_cast<int>(qBound<qint64>(0, wake - now, 60 * 60 * 1000)));
}

// After 0x020c on a folder request or an upload start: true while it may be sent again (after the pause
// the governor now holds). Every rejected attempt costs full points (S0), so this is bounded.
bool Core::retryFlooded(int id)
{
    auto it = m_uploads.constFind(id);
    if (it == m_uploads.constEnd() || !isPreparing(it->state))
        return false;
    UploadExtra& extra = m_uploadExtra[id];
    if (extra.floodRetries >= kMaxFloodRetries)
        return false;
    ++extra.floodRetries;
    return true;
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
    if (!takeFileSlot(it->target.sch, id, FileStep::Preview))
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
    floodGovernor(job.target.sch).charge(FloodGovernor::Cost::Transfer, floodClockMs());
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
    if (!ok && error == ERROR_client_is_flooding && retryFlooded(id)) {
        startPreviewSend(id); // after the pause the governor holds now
        return;
    }
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
    if (ts3::funcs.haltTransfer) {
        ts3::funcs.haltTransfer(job.target.sch, xt->previewTransferId, 1, nullptr);
        m_haltedTransfers.insert(xt->previewTransferId);
    }
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
    if (!takeFileSlot(job.target.sch, id, FileStep::Main))
        return; // waits for the governor (runFileQueue starts it)
    job.waiting = false;

    // Never overwrite: everyone uploads into the same folder, and an existing file of that name
    // belongs to another message. "File already exists" gets a new name (resendWithNewName).
    const QString  remote = joinRemote(job.remoteDir, job.remoteName);
    const QString  rc     = registerOp(OpType::Upload, {}, id);
    anyID          tid    = 0;
    const unsigned err    = ts3::funcs.sendFile(job.target.sch, job.channelId, "", remote.toUtf8().constData(), 0, 0, utf8Native(job.stagingDir).constData(), &tid,
                                                rc.toUtf8().constData());
    floodGovernor(job.target.sch).charge(FloodGovernor::Cost::Transfer, floodClockMs());
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
    // 2.2 sha: what finalizeStaged measured. Without it (the staged file couldn't be read) the link goes
    // out without one; receivers then show the file unchecked, as from 2.1.
    if (const auto xt = m_uploadExtra.constFind(id); xt != m_uploadExtra.constEnd() && xt->hashed) {
        link.sha256     = xt->digest.sha256;
        link.previewSha = job.previewRemotePath.isEmpty() ? QByteArray() : xt->digest.previewSha;
    }
    link = sanitized(link); // what receivers will see
    if (!link.sha256.isEmpty()) {
        m_knownDigests.note(remoteId(link), link.size, link.sha256); // our own bytes: the forged-hash guard's surest source
        fileverify::count(fileverify::Counter::SentWithSha);
    } else {
        fileverify::count(fileverify::Counter::SentWithoutSha);
        ts3::log(LogLevel_WARNING, job.target.sch, "Sending %1 without a checksum: the file couldn't be read for hashing",
                 {ts3::file(joinRemote(job.remoteDir, job.remoteName))});
    }

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
    const int        origin  = b->captionOrigin ? b->captionOrigin : batch;
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

    const Settings s = Settings::instance().forServer(links.first().serverUid); // 2.2 per-server settings: the note
    ComposeOptions options;
    options.includeNotice = s.addRequiredNotice;
    options.downloadUrl   = s.pluginDownloadUrl;
    options.caption       = caption;
    if (!caption.isEmpty()) {
        auto info = m_batches.find(batch);
        if (info != m_batches.end())
            info->captionDone = true;
    }

    bool firstMessage = true;
    for (const ComposedMessage& message : composeChatMessagesDetailed(links, options)) {
        PostItem post;
        post.sch       = first.target.sch;
        post.target    = first.target;
        post.channelId = first.channelId;
        post.text      = message.text.toUtf8();
        post.batch     = batch;
        post.captionOf = firstMessage && !caption.isEmpty() ? origin : 0; // the caption is in the first message
        firstMessage   = false;
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
        enqueuePost(post, false);
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
        bool           held     = false;
        while (!m_postQueue[sch].isEmpty() && governor.postReady(now)) {
            // The messages of one send go strictly one after another: the next waits for the answer to
            // the one before it (or its timeout), so a late flood answer can't put them out of order.
            if (batchPostInFlight(sch, m_postQueue[sch].first().batch)) {
                held = true; // finishPost / unansweredPost pump again
                break;
            }
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
        if (held)
            continue;
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
    if (post.target.mode == TextMessageTarget_CLIENT) {
        // The upload may have taken long (or waited for earlier files) and client ids are reused once
        // someone leaves: find the partner again by identity, so it never reaches someone else.
        anyID client = post.target.clientId;
        if (!post.target.clientUid.isEmpty() && ts3::clientUid(post.sch, client) != post.target.clientUid)
            client = ts3::clientIdByUid(post.sch, post.target.clientUid);
        if (!client) {
            failPost(post, i18n::t("Uploaded, but the person it was meant for has left the server, so nothing was sent to them. The file is in the channel's file browser."));
            return false;
        }
        post.target.clientId = client;
    }
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
    // The governor lets the next post go after a second without an answer. A message that gets no
    // answer at all is never counted as sent (S0: TeamSpeak drops oversized commands without one): it
    // fails, and Retry posts it again.
    singleShotOwned(kPostTimeoutMs, this, [this, rc] {
        if (!m_ops.contains(rc))
            return;
        forgetOp(rc);
        unansweredPost(rc);
    });
    return true;
}

void Core::unansweredPost(const QString& returnCode)
{
    auto it = m_postsInFlight.find(returnCode);
    if (it == m_postsInFlight.end())
        return;
    const InFlightPost flight = it.value();
    m_postsInFlight.erase(it);
    floodGovernor(flight.item.sch).postAnswered(flight.ticket, floodClockMs(), false, flight.item.attempts);
    ts3::log(LogLevel_WARNING, flight.item.sch, "TeamSpeak didn't answer a chat message of %1 bytes within %2 s; it counts as not sent",
             {ts3::pub(escapedMessageSize(QString::fromUtf8(flight.item.text))), ts3::pub(kPostTimeoutMs / 1000)});
    failPost(flight.item, i18n::t("TeamSpeak didn't confirm the chat message, so it may not have been sent. Click Retry to post it again."),
             i18n::t("TeamSpeak didn't confirm it, so it may not have been sent"));
    pumpPosts();
}

// The answer to a chat post. retryHintMs: FloodGovernor::retryHintMs() of the answer.
void Core::finishPost(const QString& returnCode, unsigned int error, const QString& message, bool permissionError, int retryHintMs)
{
    auto it = m_postsInFlight.find(returnCode);
    if (it == m_postsInFlight.end())
        return;
    InFlightPost flight = it.value();
    m_postsInFlight.erase(it);
    FloodGovernor& governor = floodGovernor(flight.item.sch);
    const bool     flooded  = error == ERROR_client_is_flooding && !permissionError;
    const bool     retry    = governor.postAnswered(flight.ticket, floodClockMs(), flooded, flight.item.attempts, retryHintMs);
    if (flooded && retry) {
        ts3::log(LogLevel_WARNING, flight.item.sch, "TeamSpeak's flood protection held back a chat message (attempt %1); sending it again in %2 ms",
                 {ts3::pub(flight.item.attempts), ts3::pub(governor.counters().lastPauseMs)});
        for (int id : qAsConst(flight.item.jobs)) {
            auto job = m_uploads.find(id);
            if (job != m_uploads.end() && job->state == UploadState::Posting)
                setUploadState(job.value(), UploadState::Posting, i18n::t("TeamSpeak is limiting messages. Retrying…"));
        }
        enqueuePost(flight.item, true); // back to its place, before what came after it
    } else if (error == ERROR_ok && !permissionError) {
        if (flight.item.captionOf)
            settleCaption(flight.item.captionOf, true);
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

// The files are on the server, only their message didn't go: Retry posts the same message again
// (retryUpload), with every file it announces. captionText: the reason for a caption of its own.
void Core::failPost(const PostItem& post, const QString& text, const QString& captionText)
{
    if (post.jobs.isEmpty()) {
        // A caption of its own: the files are posted anyway (nothing posts it again).
        ts3::printWarning(post.sch, i18n::t("Couldn't send the caption: %1").arg(captionText.isEmpty() ? text : captionText));
        if (post.captionOf)
            settleCaption(post.captionOf, false);
        return;
    }
    for (int id : post.jobs) {
        if (const UploadJob* job = upload(id); job && job->uploaded)
            m_failedPosts.insert(id, post);
    }
    // One chat line for the message, however many files it announces.
    warnFailed(post.sch, post.jobs, text);
    for (int id : post.jobs)
        failUpload(id, text, true);
}

// One chat warning for uploads that fail together (a message for an album, a lost connection): the
// file's name for one, the count for several. Jobs that already finished are not counted.
void Core::warnFailed(uint64 sch, const QVector<int>& ids, const QString& text)
{
    QVector<int> failing;
    for (int id : ids) {
        const UploadJob* job = upload(id);
        if (job && job->state != UploadState::Done && job->state != UploadState::Failed && job->state != UploadState::Canceled && !failing.contains(id))
            failing.append(id);
    }
    if (failing.isEmpty())
        return;
    if (failing.size() == 1) {
        const UploadJob* job = upload(failing.first());
        ts3::printWarning(sch, job->pasted ? i18n::t("Couldn't send the pasted image: %1").arg(text)
                                           : i18n::t("Couldn't send “%1”: %2").arg(displayNameFor(*job), text));
        return;
    }
    ts3::printWarning(sch, i18n::t("Couldn't send %1 files: %2").arg(failing.size()).arg(text));
}

// Queues a chat post: new ones at the end; a flooded one (keepSeq) back at its place by its number.
void Core::enqueuePost(PostItem post, bool keepSeq)
{
    QList<PostItem>& queue = m_postQueue[post.sch];
    if (!keepSeq || post.seq == 0) {
        post.seq = ++m_nextPostSeq;
        queue.append(post);
        return;
    }
    int at = 0;
    while (at < queue.size() && queue.at(at).seq < post.seq)
        ++at;
    queue.insert(at, post);
}

bool Core::batchPostInFlight(uint64 sch, int batch) const
{
    if (batch == 0)
        return false;
    for (const InFlightPost& flight : m_postsInFlight) {
        if (flight.item.sch == sch && flight.item.batch == batch)
            return true;
    }
    return false;
}

// Whether a file of batch other than exceptId can still bring a message to the chat: uploading, or
// uploaded and waiting for its turn or in the post queue.
bool Core::batchCanStillPost(int batch, int exceptId) const
{
    for (const UploadJob& job : m_uploads) {
        if (job.batch == batch && job.id != exceptId && (isPreparing(job.state) || job.state == UploadState::Posting))
            return true;
    }
    return false;
}

void Core::settleCaption(int origin, bool posted)
{
    if (origin != 0 && m_captionsPending.remove(origin))
        emit captionSettled(origin, posted);
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
    if (m_fileQueue.contains(sch))
        scheduleFileQueue(); // a pause moves the next folder request / upload start too
}

void Core::forgetBatchIfDone(int batch)
{
    for (const UploadJob& job : qAsConst(m_uploads)) {
        if (job.batch == batch)
            return;
    }
    const auto info = m_batches.constFind(batch);
    if (info == m_batches.constEnd())
        return;
    // Its caption can't be posted any more, unless a message carrying it is still on its way (or it was
    // handed on to a retry, which reports it).
    const int  origin  = info->captionOrigin ? info->captionOrigin : batch;
    bool       carried = info->caption.isEmpty();
    for (auto q = m_postQueue.cbegin(); q != m_postQueue.cend() && !carried; ++q) {
        for (const PostItem& post : q.value())
            carried = carried || post.captionOf == origin;
    }
    for (const InFlightPost& flight : qAsConst(m_postsInFlight))
        carried = carried || flight.item.captionOf == origin;
    m_batches.remove(batch);
    if (!carried)
        settleCaption(origin, false);
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
    e.state          = MediaState::Ready;
    e.progress       = 1.0;
    e.check.verified = !link.sha256.isEmpty(); // 2.2 sha: the very bytes that were hashed

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

void Core::failUpload(int id, const QString& text, bool quiet)
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
    if (!quiet) {
        const QString warning = job.pasted ? i18n::t("Couldn't send the pasted image: %1").arg(text)
                                           : i18n::t("Couldn't send “%1”: %2").arg(displayNameFor(job), text);
        ts3::printWarning(job.target.sch, warning);
    }
    setUploadState(job, UploadState::Failed, text);
}

bool Core::canCancelUpload(int id) const
{
    const UploadJob* job = upload(id);
    return job
           && (isPreparing(job->state) || (job->state == UploadState::Posting && job->waiting));
}

void Core::cancelUpload(int id)
{
    if (!canCancelUpload(id))
        return;
    UploadJob& job = m_uploads[id];
    if (job.state == UploadState::Posting) {
        // Uploaded, but its message still waits for earlier files or the rest of its album: nothing
        // was posted, so no message will ever point at the file or its preview. Don't leave them in the
        // channel's file browser (without delete permission they stay; deleteRemoteFile logs it). The
        // cached copy is left to the cache limit. Its unit is composed without it, like a failed item.
        deleteRemoteFile(job.target.sch, job.channelId, joinRemote(job.remoteDir, job.remoteName));
        if (!job.previewRemotePath.isEmpty())
            deleteRemoteFile(job.target.sch, job.channelId, job.previewRemotePath);
        setUploadState(job, UploadState::Canceled, i18n::t("Canceled"));
        return;
    }
    if (job.transferActive) {
        if (ts3::funcs.haltTransfer) {
            ts3::funcs.haltTransfer(job.target.sch, job.transferId, 1, nullptr);
            m_haltedTransfers.insert(job.transferId);
        }
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
    if (!job || job->state != UploadState::Failed)
        return false;
    // Uploaded, only its message failed: the message is posted again (no second upload).
    if (job->uploaded)
        return m_failedPosts.contains(id);
    return QFileInfo(job->sourcePath).isFile();
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
    // The tab may have reconnected meanwhile, to another server or with new client ids: the file must
    // still reach the chat (and the person) it was meant for.
    ChatTarget target = job.target;
    if (ts3::serverUid(target.sch) != target.serverUid) {
        ts3::printWarning(ts3::currentConnection(), i18n::t("This file was meant for another server. Send it again from the right chat."));
        return 0;
    }
    if (target.mode == TextMessageTarget_CLIENT) {
        target.clientId = ts3::clientIdByUid(target.sch, target.clientUid);
        if (!target.clientId) {
            ts3::printWarning(ts3::currentConnection(), noRecipientText());
            return 0;
        }
    }
    if (job.uploaded)
        return repostFailed(id, target);
    SendItem item    = m_jobItems.value(id);
    item.path        = job.sourcePath;
    item.pasted      = job.pasted;
    item.ownTemp     = job.deleteSource;
    job.deleteSource = false; // a pasted image now belongs to the new job

    // A caption that nothing of its send can carry any more (every other file failed or was canceled)
    // goes with the retry. While an album or file of it is still on its way, that one keeps it.
    SendRequest request;
    request.target = target;
    request.items.append(item);
    int  captionOrigin = 0;
    auto batch         = m_batches.find(job.batch);
    if (batch != m_batches.end() && !batch->captionDone && !batch->caption.isEmpty() && !batchCanStillPost(job.batch, id)) {
        request.caption    = batch->caption;
        captionOrigin      = batch->captionOrigin ? batch->captionOrigin : job.batch;
        batch->captionDone = true;
        batch->caption.clear(); // handed on: the retry reports it (captionSettled)
    }
    forgetUpload(id);

    // A pasted image keeps its name; anything else gets a new random part, as a new send would.
    const int newBatch = send(request);
    if (captionOrigin != 0) {
        if (newBatch == 0) {
            settleCaption(captionOrigin, false);
        } else {
            m_captionsPending.remove(newBatch); // reported under the batch it came from
            m_captionsPending.insert(captionOrigin);
            m_batches[newBatch].captionOrigin = captionOrigin;
        }
    }
    if (newBatch == 0)
        return 0;
    const PostUnit& unit = m_batches[newBatch].units.first();
    return unit.jobs.value(0);
}

// Posts a failed chat message again, for every file it announces (they are on the server already).
int Core::repostFailed(int id, const ChatTarget& target)
{
    const auto failed = m_failedPosts.constFind(id);
    if (failed == m_failedPosts.constEnd())
        return 0;
    PostItem post = failed.value();
    post.target   = target; // checked by retryUpload: the same server, the partner found again
    post.attempts = 0;
    for (int other : qAsConst(post.jobs)) {
        m_failedPosts.remove(other);
        auto job = m_uploads.find(other);
        if (job != m_uploads.end() && job->state == UploadState::Failed)
            setUploadState(job.value(), UploadState::Posting, i18n::t("Posting to chat…"));
    }
    ts3::log(LogLevel_INFO, post.sch, "Posting a chat message again (%1 files)", {ts3::pub(post.jobs.size())});
    enqueuePost(post, false); // a new place at the end
    pumpPosts();
    return id;
}

void Core::forgetUpload(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end())
        return;
    m_failedPosts.remove(id);
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
        singleShotOwned(state == UploadState::Failed ? kFailedJobLingerMs : kJobLingerMs, this, [this, id] { forgetUpload(id); });
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
    for (QList<FileWait>& waiting : m_fileQueue) // its folder request or upload start won't go any more
        waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [&job](const FileWait& w) { return w.id == job.id; }), waiting.end());
    if (removePreview) {
        deleteRemoteFile(job.target.sch, job.channelId, job.previewRemotePath);
        job.previewRemotePath.clear();
    }

    // While the worker copies or probes the staged file it cannot be deleted: the copy is stopped
    // and onProbed() removes it (and a pasted source it was still reading) once the worker is done.
    const bool working = m_probing.contains(job.id);
    // 2.4 compress: a compression stops; its output folder goes once the transcoder let go (onCompressed).
    const auto compressing = m_compressing.constFind(job.id);
    if (compressing != m_compressing.constEnd() && compressing->control)
        compressing->control->cancel.store(true);
    if (working) {
        if (const auto cancel = m_stagingCancel.value(job.id))
            cancel->store(true);
    } else if (compressing != m_compressing.constEnd()) {
        QDir(job.stagingDir).removeRecursively();
        QDir(previewStagingDir(job.stagingDir)).removeRecursively();
    } else {
        removeStaging(job.stagingDir);
    }
    job.stagingDir.clear();
    // A failed file of our own (a pasted image; 2.2 editor: an edited copy) stays for a retry until its
    // job is dismissed (forgetUpload).
    if (job.state != UploadState::Failed)
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
// 2.4 compress: videos made smaller before they are sent
// ============================================================================================

videocompress::Options Core::compressOptions(SendQuality quality, const QString& serverUid)
{
    // 2.2 per-server settings: compression itself is global; the upload limit is the server's.
    const Settings         s = Settings::instance().forServer(serverUid);
    videocompress::Options options;
    options.mediaFoundation   = mf::available();
    options.compressLarge     = s.compressVideos;
    options.thresholdBytes    = megabytes(s.compressVideosOverMB);
    options.shortSide         = Settings::normalizeVideoQuality(s.compressVideoQuality);
    options.convertUnplayable = s.convertUnplayableVideos;
    options.limitBytes        = megabytes(s.uploadMaxMB);
    switch (quality) {
    case SendQuality::Auto:
        options.request = videocompress::Request::Auto;
        break;
    case SendQuality::Original:
        options.request = videocompress::Request::Original;
        break;
    case SendQuality::P1080:
        options.request = videocompress::Request::P1080;
        break;
    case SendQuality::P720:
        options.request = videocompress::Request::P720;
        break;
    case SendQuality::P480:
        options.request = videocompress::Request::P480;
        break;
    }
    return options;
}

void Core::queryEncoders()
{
    if (m_encodersAsked)
        return;
    m_encodersAsked = true;
    QPointer<Core> self(this);
    m_pool.start([self] {
        const mf::EncoderList list = mf::h264Encoders();
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, list] {
                if (!self)
                    return;
                self->m_encodersQueried  = list.queried;
                self->m_hardwareEncoders = list.hardware;
                self->m_softwareEncoders = list.software;
                ts3::log(LogLevel_INFO, 0, "H.264 encoders: %1 (graphics card); %2 (processor)",
                         {ts3::pub(list.hardware.isEmpty() ? QStringLiteral("none") : list.hardware.join(QStringLiteral(", "))),
                          ts3::pub(list.software.isEmpty() ? QStringLiteral("none") : list.software.join(QStringLiteral(", ")))});
                emit self->encodersKnown();
            }, Qt::QueuedConnection);
        }
    });
}

// The worker planned a compression (or a failure): the original was not copied.
void Core::onCompressPlanned(int id, const LocalMediaInfo& info, const QByteArray& previewJpeg, const videocompress::Plan& plan)
{
    const QString staging = m_probing.take(id);
    m_stagingCancel.remove(id);
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing) {
        removeStaging(staging);
        return;
    }
    UploadJob&     job = it.value();
    const Settings s   = Settings::instance().forServer(job.target.serverUid); // 2.2 per-server: its upload limit
    if (plan.decision == videocompress::Decision::Fail) {
        ts3::log(LogLevel_INFO, job.target.sch, "Not sending %1: %2", {ts3::file(job.remoteName), ts3::pub(videocompress::failureText(plan, s.uploadMaxMB))});
        failUpload(id, videocompress::failureText(plan, s.uploadMaxMB));
        return;
    }
    job.info      = info;
    job.info.kind = MediaKind::Video; // the result is an MP4 video, whatever the original's name said
    m_uploadExtra[id].previewJpeg = previewJpeg;

    CompressTask task;
    task.plan  = plan;
    task.limit = megabytes(s.uploadMaxMB);
    task.gpu   = s.compressUseGpu;
    // The size guard: the fit target when made to fit, the limit (and for a shrink, the original's size)
    // otherwise. The final file must be within the limit either way.
    if (plan.reason == videocompress::Reason::FitToLimit)
        task.abortAbove = static_cast<quint64>(static_cast<double>(task.limit) * videocompress::kFitTarget);
    else if (plan.reason == videocompress::Reason::Shrink)
        task.abortAbove = qMin(task.limit, job.originalSize);
    else
        task.abortAbove = task.limit;
    task.outDir = compressDirFor(job.stagingDir);
    m_compressing.insert(id, task);
    startCompression(id);
}

void Core::startCompression(int id)
{
    auto it   = m_uploads.find(id);
    auto task = m_compressing.find(id);
    if (it == m_uploads.end() || task == m_compressing.end() || it->state != UploadState::Preparing)
        return;
    UploadJob& job = it.value();
    // The compressed copy needs room next to the original (and the cache link afterwards).
    const quint64      needed  = task->plan.estimatedBytes + task->plan.estimatedBytes / 4 + 64ull * 1024 * 1024;
    const QStorageInfo storage(ts3::dataDir());
    if (storage.isValid() && storage.bytesAvailable() >= 0 && static_cast<quint64>(storage.bytesAvailable()) < needed) {
        const QString text  = videocompress::diskSpaceText(needed);
        const quint64 limit = task->limit; // the server's upload limit at send time (onCompressPlanned)
        m_compressing.erase(task);
        ts3::log(LogLevel_WARNING, job.target.sch, "Not compressing %1: %2", {ts3::file(job.remoteName), ts3::pub(text)});
        if (job.originalSize <= limit)
            stageOriginal(id, CompressOutcome::Fallback, text);
        else
            failUpload(id, text);
        return;
    }
    QDir(task->outDir).removeRecursively();
    QDir().mkpath(task->outDir);
    job.estimatedSize     = task->plan.estimatedBytes;
    job.compressLabel     = videocompress::resolutionLabel(task->plan.frameSize);
    job.compressFinishing = false;
    job.compressEncoder   = 0;
    job.progress          = 0.0;
    ts3::log(LogLevel_INFO, job.target.sch, "Compressing %1: %2 to %3 %4 fps, %5 kbps video + %6 kbps sound (about %7)",
             {ts3::file(job.remoteName), ts3::pub(formatSize(job.originalSize)), ts3::pub(job.compressLabel), ts3::pub(task->plan.fps),
              ts3::pub(task->plan.videoKbps), ts3::pub(task->plan.audioKbps), ts3::pub(formatSize(task->plan.estimatedBytes))});
    setUploadState(job, UploadState::Compressing, i18n::t("Waiting to compress…"), true);
    runTranscode(id);
    ensureProgressTimer();
}

// Queues one run of the transcoder for the job's current plan (a fresh control each time, so answers
// of an older run are told apart).
void Core::runTranscode(int id)
{
    auto task = m_compressing.find(id);
    auto it   = m_uploads.constFind(id);
    if (task == m_compressing.end() || it == m_uploads.constEnd())
        return;
    const auto control = std::make_shared<mf::TranscodeControl>();
    task->control      = control;
    task->started      = false;
    mf::TranscodeRequest request;
    // The original where it is; a file of ours (ownTemp) may have been moved into staging already.
    const QString staged    = it->stagingDir + QLatin1Char('/') + it->remoteName;
    request.source          = QFileInfo::exists(staged) ? staged : it->sourcePath;
    request.target          = task->outDir + QStringLiteral("/out.mp4");
    request.frameSize       = task->plan.frameSize;
    request.fps             = task->plan.fpsCap > 0 ? task->plan.fpsCap : task->plan.fps;
    request.videoKbps       = task->plan.videoKbps;
    request.audioKbps       = task->plan.audioKbps;
    request.audioChannels   = task->plan.audioChannels;
    request.allowHardware   = task->gpu;
    request.abortAboveBytes = task->abortAbove;
    request.durationMs      = it->info.durationMs;
    const QString    outDir      = task->outDir;
    const QByteArray previewJpeg = m_uploadExtra.value(id).previewJpeg; // the original's poster: the preview
    QPointer<Core>   self(this);
    m_transcodePool.start([self, id, request, control, outDir, previewJpeg] {
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id, control] {
                if (self)
                    self->markCompressStarted(id, control);
            }, Qt::QueuedConnection);
        }
        mf::TranscodeResult result;
        if (control->cancel.load())
            result.canceled = true; // canceled (or sent as it is) while it waited
        else
            result = mf::transcodeToMp4(request, control.get());
        // 2.2 sha: the result has been verified here and is the file that will be uploaded (moved, not
        // changed, into the staging folder by onCompressed). Its SHA-256 is taken now, on this worker, and
        // posted before onCompressed: the link's sha describes the compressed bytes, never the original's.
        // If onCompressed falls back to the original after all, stageOriginal hashes the original.
        if (result.ok && !control->cancel.load()) {
            const fileverify::StagedDigest digest = fileverify::finalizeStaged(request.target, previewJpeg, &control->cancel);
            if (Core* core = self.data()) {
                QMetaObject::invokeMethod(core, [self, id, control, digest] {
                    // Only for the run still awaited (not one replaced by a re-run, a cancel or Send original).
                    if (self && self->m_compressing.value(id).control == control)
                        self->onStagedFinalized(id, digest);
                }, Qt::QueuedConnection);
            }
        }
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id, control, outDir, result] {
                if (self)
                    self->onCompressed(id, control, outDir, result);
            }, Qt::QueuedConnection);
        }
    }, -id); // queued videos go in the order they were sent (the earlier id first)
}

void Core::markCompressStarted(int id, const std::shared_ptr<mf::TranscodeControl>& control)
{
    auto task = m_compressing.find(id);
    auto it   = m_uploads.find(id);
    if (task == m_compressing.end() || task->control != control || it == m_uploads.end() || it->state != UploadState::Compressing)
        return;
    task->started = true;
    setUploadState(it.value(), UploadState::Compressing, i18n::t("Compressing…"));
}

bool Core::canSendOriginal(int id) const
{
    const UploadJob* job = upload(id);
    // No file system check here: the toast asks on every progress tick (a vanished original fails when
    // it is copied, with its own text).
    const auto task = m_compressing.constFind(id);
    return job && job->state == UploadState::Compressing && !job->compressFinishing && task != m_compressing.constEnd()
           && job->originalSize <= task->limit; // 2.2 per-server: the server's upload limit at send time
}

void Core::sendOriginal(int id)
{
    if (!canSendOriginal(id))
        return;
    auto task = m_compressing.find(id);
    if (task->control)
        task->control->cancel.store(true);
    if (!task->started) {
        // Still queued behind another video: the original goes now; the queued run returns at once.
        const QString outDir = task->outDir;
        m_compressing.erase(task);
        QDir(outDir).removeRecursively();
        stageOriginal(id, CompressOutcome::SentOriginal, QString());
        return;
    }
    task->skip = true; // the transcoder stops within about a frame; onCompressed sends the original
}

void Core::onCompressed(int id, const std::shared_ptr<mf::TranscodeControl>& control, const QString& outDir, const mf::TranscodeResult& result)
{
    auto task = m_compressing.find(id);
    if (task == m_compressing.end() || task->control != control) {
        QDir(outDir).removeRecursively(); // a run nobody waits for any more
        return;
    }
    const CompressTask done = task.value();
    m_compressing.erase(task);
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Compressing) {
        QDir(done.outDir).removeRecursively(); // canceled, failed or disconnected meanwhile
        return;
    }
    UploadJob&    job      = it.value();
    const int     limitMB  = static_cast<int>(done.limit / (1024 * 1024));
    const QString encoder  = result.hardware ? QStringLiteral("graphics card") : QStringLiteral("processor");
    const double  seconds  = static_cast<double>(result.elapsedMs) / 1000.0;
    const double  realtime = seconds > 0.0 ? static_cast<double>(job.info.durationMs) / 1000.0 / seconds : 0.0;
    if (result.gpuFailed)
        ts3::log(LogLevel_WARNING, job.target.sch, "The graphics card couldn't compress %1 (%2); the processor took over", {ts3::file(job.remoteName), ts3::pub(result.gpuError)});

    if (done.skip) {
        ts3::log(LogLevel_INFO, job.target.sch, "Compressing %1 stopped: sending the original", {ts3::file(job.remoteName)});
        QDir(done.outDir).removeRecursively();
        stageOriginal(id, CompressOutcome::SentOriginal, QString());
        return;
    }
    if (result.exceeded && done.attempt == 0 && result.projectedBytes > 0) {
        // The encoder missed its rate: once more, with the bitrate scaled to land at 95% of the guard.
        CompressTask again   = done;
        const double scale   = static_cast<double>(done.abortAbove) * 0.95 / static_cast<double>(result.projectedBytes);
        again.attempt        = 1;
        again.plan.videoKbps = qMax(videocompress::kMinVideoKbps, static_cast<int>(again.plan.videoKbps * scale));
        again.plan.estimatedBytes = videocompress::estimateBytes(again.plan.videoKbps, again.plan.audioKbps, job.info.durationMs);
        ts3::log(LogLevel_INFO, job.target.sch, "Compressing %1 would end at %2, over %3: once more at %4 kbps",
                 {ts3::file(job.remoteName), ts3::pub(formatSize(result.projectedBytes)), ts3::pub(formatSize(done.abortAbove)), ts3::pub(again.plan.videoKbps)});
        QDir(done.outDir).removeRecursively();
        QDir().mkpath(done.outDir);
        m_compressing.insert(id, again);
        job.estimatedSize = again.plan.estimatedBytes;
        job.progress      = 0.0;
        setUploadState(job, UploadState::Compressing, i18n::t("Compressing…"));
        runTranscode(id);
        return;
    }

    const bool smaller = result.bytes < job.originalSize || done.plan.reason == videocompress::Reason::Convert;
    if (result.ok && result.bytes > 0 && result.bytes <= done.limit && smaller) {
        // The verified MP4 becomes the staged file, with the same random part in its name (the preview
        // keeps its name too).
        const QString name   = QFileInfo(job.remoteName).completeBaseName() + QStringLiteral(".mp4");
        const QString staged = job.stagingDir + QLatin1Char('/') + name;
        QDir(job.stagingDir).removeRecursively();
        QDir().mkpath(job.stagingDir);
        bool ok = QFile::rename(done.outDir + QStringLiteral("/out.mp4"), staged);
        if (ok && job.remoteDir != QLatin1String("/")) {
            const QString nested = job.stagingDir + job.remoteDir + QLatin1Char('/') + name;
            QDir().mkpath(QFileInfo(nested).absolutePath());
            ok = linkOrCopy(staged, nested);
        }
        QDir(done.outDir).removeRecursively();
        if (ok) {
            m_lastCompression = QStringLiteral("%1, %2, %3 s, %4x realtime, %5 to %6")
                                    .arg(job.compressLabel, encoder)
                                    .arg(seconds, 0, 'f', 1)
                                    .arg(realtime, 0, 'f', 1)
                                    .arg(formatSize(job.originalSize), formatSize(result.bytes));
            ts3::log(LogLevel_INFO, job.target.sch, "Compressed %1 in %2 s (%3x realtime) on the %4 (%5): %6 to %7",
                     {ts3::file(job.remoteName), ts3::pub(QString::number(seconds, 'f', 1)), ts3::pub(QString::number(realtime, 'f', 1)), ts3::pub(encoder),
                      ts3::pub(result.encoderName), ts3::pub(formatSize(job.originalSize)), ts3::pub(formatSize(result.bytes))});
            job.remoteName        = name;
            job.size              = result.bytes;
            job.info.width        = result.frameSize.width();
            job.info.height       = result.frameSize.height();
            job.info.durationMs   = result.durationMs;
            job.compression       = CompressOutcome::Compressed;
            job.compressFinishing = false;
            job.progress          = 0.0;
            setUploadState(job, UploadState::Preparing, i18n::t("Preparing…"));
            continueUpload(id);
            return;
        }
        ts3::log(LogLevel_WARNING, job.target.sch, "Could not move the compressed copy of %1 into place", {ts3::file(job.remoteName)});
    }
    QDir(done.outDir).removeRecursively();

    // Didn't work out: the cause in a few words for the log, the toast's tooltip and the error.
    QString cause;
    if (result.ok && result.bytes > done.limit)
        cause = i18n::t("the result was still larger than your limit");
    else if (result.ok)
        cause = i18n::t("the result wasn't smaller");
    else if (result.exceeded)
        cause = i18n::t("the result would have been too large");
    else if (result.stage == mf::TranscodeResult::Stage::Disk)
        cause = i18n::t("the disk is full");
    else if (result.stage == mf::TranscodeResult::Stage::Stall)
        cause = i18n::t("it stopped responding");
    else if (result.stage == mf::TranscodeResult::Stage::Verify)
        cause = result.shorter ? i18n::t("the video ends early, it may be damaged") : i18n::t("the result didn't check out");
    else
        cause = i18n::t("error %1").arg(QStringLiteral("0x") + QString::number(result.hr, 16).toUpper().rightJustified(8, QLatin1Char('0')));
    const QString stage = QString::fromLatin1(mf::stageName(result.stage));
    m_lastCompression   = QStringLiteral("%1, %2: %3 (%4)").arg(job.compressLabel, encoder, stage, result.detail);
    const bool sourceOk = QFileInfo(job.sourcePath).isFile();
    const bool fits     = job.originalSize <= done.limit && sourceOk;
    ts3::log(LogLevel_WARNING, job.target.sch, "Could not compress %1 (%2, %3: %4); %5",
             {ts3::file(job.remoteName), ts3::pub(stage), ts3::pub(cause), ts3::pub(result.detail),
              ts3::pub(fits ? QStringLiteral("sending the original") : QStringLiteral("not sending it"))});
    if (fits)
        stageOriginal(id, CompressOutcome::Fallback, videocompress::compressFailedNote(cause));
    else if (!sourceOk)
        failUpload(id, i18n::t("Couldn't read the file. It may be open in another program or no longer exist."));
    else
        failUpload(id, videocompress::compressFailedText(cause, limitMB));
}

// The original goes after all: copied into staging on a worker as any file (onOriginalStaged).
void Core::stageOriginal(int id, CompressOutcome outcome, const QString& note)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || (it->state != UploadState::Compressing && it->state != UploadState::Preparing))
        return;
    UploadJob& job        = it.value();
    job.compression       = outcome;
    job.compressNote      = note;
    job.compressFinishing = false;
    job.progress          = 0.0;
    job.size              = job.originalSize;
    // A file that isn't a video by name (a camcorder .mts) goes as a plain file, without picture details.
    if (kindForFileName(job.remoteName) != MediaKind::Video) {
        job.info = LocalMediaInfo();
        job.info.kind = kindForFileName(job.remoteName);
        m_uploadExtra[id].previewJpeg.clear();
    }
    setUploadState(job, UploadState::Preparing, outcome == CompressOutcome::SentOriginal ? i18n::t("Sending the original…") : i18n::t("Couldn't compress. Sending the original…"));

    // The staging folder only holds the original when it is a file of ours that was moved there.
    QDir(previewStagingDir(job.stagingDir)).removeRecursively();
    QDir(compressDirFor(job.stagingDir)).removeRecursively();
    QDir().mkpath(job.stagingDir);
    const QString  staged = job.stagingDir + QLatin1Char('/') + job.remoteName;
    const QString  nested = job.remoteDir == QLatin1String("/") ? QString() : job.stagingDir + job.remoteDir + QLatin1Char('/') + job.remoteName;
    const QString  source = job.sourcePath;
    const auto     cancel = std::make_shared<std::atomic<bool>>(false);
    // 2.2 sha: a digest of a compressed copy no longer applies; the original is hashed on the worker.
    UploadExtra& extra = m_uploadExtra[id];
    extra.digest       = fileverify::StagedDigest();
    extra.hashed       = false;
    const QByteArray previewJpeg = extra.previewJpeg;
    QPointer<Core>   self(this);
    m_probing.insert(id, job.stagingDir);
    m_stagingCancel.insert(id, cancel);
    m_pool.start([self, id, source, staged, nested, cancel, previewJpeg] {
        bool ok = QFileInfo::exists(staged) || copyForStaging(source, staged, cancel.get());
        if (ok && !nested.isEmpty()) {
            QDir().mkpath(QFileInfo(nested).absolutePath());
            ok = linkOrCopy(staged, nested);
        }
        const quint64 size = ok ? static_cast<quint64>(QFileInfo(staged).size()) : 0;
        // 2.2 sha: the staged original is final here; hashed on this worker and posted before onOriginalStaged.
        if (ok && !cancel->load()) {
            const fileverify::StagedDigest digest = fileverify::finalizeStaged(staged, previewJpeg, cancel.get());
            if (Core* core = self.data()) {
                QMetaObject::invokeMethod(core, [self, id, digest] {
                    if (self)
                        self->onStagedFinalized(id, digest);
                }, Qt::QueuedConnection);
            }
        }
        if (Core* core = self.data()) {
            QMetaObject::invokeMethod(core, [self, id, ok, size] {
                if (self)
                    self->onOriginalStaged(id, ok, size);
            }, Qt::QueuedConnection);
        }
    });
}

void Core::onOriginalStaged(int id, bool ok, quint64 size)
{
    const QString staging = m_probing.take(id);
    m_stagingCancel.remove(id);
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing) {
        removeStaging(staging);
        return;
    }
    const QString  serverUid = it->target.serverUid;
    const Settings s         = Settings::instance().forServer(serverUid); // 2.2 per-server: its upload limit
    if (!ok) {
        failUpload(id, i18n::t("Couldn't read the file. It may be open in another program or no longer exist."));
        return;
    }
    if (size == 0) {
        failUpload(id, i18n::t("This file is empty."));
        return;
    }
    if (size > megabytes(s.uploadMaxMB)) {
        failUpload(id, uploadLimitText(s.uploadMaxMB, hasOwnUploadLimit(serverUid)));
        return;
    }
    it->size = size;
    continueUpload(id);
}

void Core::continueUpload(int id)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || it->state != UploadState::Preparing)
        return;
    it->waiting = false;
    if (it->remoteDir == QLatin1String("/"))
        onDirectoryReady(id, false, true);
    else
        createRemoteDirectory(id, false);
}

// Reads the running compressions' progress into their jobs (the 250 ms progress timer). A change of half
// a percent or of a flag is reported.
bool Core::pollCompressions()
{
    QList<int> changed;
    for (auto task = m_compressing.cbegin(); task != m_compressing.cend(); ++task) {
        auto it = m_uploads.find(task.key());
        if (!task->control || !task->started || it == m_uploads.end() || it->state != UploadState::Compressing)
            continue;
        const double progress  = qBound(0.0, task->control->permille.load() / 1000.0, 1.0);
        const bool   finishing = task->control->finishing.load();
        const int    encoder   = task->control->encoder.load();
        if (progress - it->progress >= 0.005 || finishing != it->compressFinishing || encoder != it->compressEncoder) {
            it->progress          = qMax(it->progress, progress);
            it->compressFinishing = finishing;
            it->compressEncoder   = encoder;
            if (finishing)
                it->message = i18n::t("Finishing compression…");
            changed.append(it->id);
        }
    }
    for (int id : qAsConst(changed))
        emit uploadChanged(id);
    return !m_compressing.isEmpty();
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
    const bool compressing = pollCompressions(); // 2.4 compress

    if (m_progressTimer && m_downloadsByTransfer.isEmpty() && m_previewsByTransfer.isEmpty() && m_uploadsByTransfer.isEmpty() && !compressing)
        m_progressTimer->stop();
}

// ============================================================================================
// 2.2 sha: hashing what is sent, checking what is received (fileverify.h)
// ============================================================================================

void Core::onStagedFinalized(int id, const fileverify::StagedDigest& digest)
{
    auto it = m_uploads.find(id);
    if (it == m_uploads.end() || (it->state != UploadState::Preparing && it->state != UploadState::Compressing))
        return; // canceled or failed meanwhile
    UploadExtra& extra = m_uploadExtra[id];
    extra.digest       = digest;
    extra.hashed       = true;
    if (digest.sha256.isEmpty())
        ts3::log(LogLevel_WARNING, it->target.sch, "Couldn't hash %1 (%2)", {ts3::file(it->remoteName), ts3::pub(digest.error)});
}

// Starts checking a finished download against its link's sha. The entry stays Downloading at 100%
// until onCheckFinished.
bool Core::startCheck(const QString& key, const QString& found)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || !m_verifier || it->link.sha256.size() != fileverify::kShaBytes)
        return false;
    MediaEntry& e   = it.value();
    e.progress      = 1.0;
    e.check         = ShaCheck();
    e.check.running = true;
    m_checks.insert(key, {floodClockMs(), found});
    if (!m_checkTimer) {
        // A member timer, never a functor singleShot: nothing of ours may be pending once the plugin is gone.
        m_checkTimer = new QTimer(this);
        m_checkTimer->setSingleShot(true);
        connect(m_checkTimer, &QTimer::timeout, this, &Core::showSlowChecks);
    }
    if (!m_checkTimer->isActive())
        m_checkTimer->start(kShowCheckMs);
    m_verifier->start(key, found, e.link.sha256);
    touch(e);
    return true;
}

// Checks running for kShowCheckMs say so ("Checking file…"); quicker ones never do (no flashing).
void Core::showSlowChecks()
{
    const qint64 now  = floodClockMs();
    qint64       next = -1;
    QStringList  shown;
    for (auto it = m_checks.cbegin(); it != m_checks.cend(); ++it) {
        auto e = m_entries.find(it.key());
        if (e == m_entries.end() || !e->check.running || e->check.shown)
            continue;
        const qint64 due = it->startedAt + kShowCheckMs;
        if (due > now) {
            next = next < 0 ? due : qMin(next, due);
            continue;
        }
        e->check.shown    = true;
        e->check.progress = qMax(e->check.progress, m_verifier->progress(it.key()));
        shown.append(it.key());
    }
    if (next >= 0 && m_checkTimer)
        m_checkTimer->start(static_cast<int>(qMax<qint64>(1, next - now)));
    for (const QString& key : qAsConst(shown)) {
        auto e = m_entries.find(key);
        if (e != m_entries.end())
            touch(e.value());
    }
}

void Core::onCheckProgress(const QString& key, double fraction, bool secondPass)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || !it->check.running)
        return;
    ShaCheck&    check  = it->check;
    const bool   again  = check.again != secondPass;
    const double before = check.progress;
    check.again         = secondPass;
    check.progress      = fraction;
    // Repaint only for what a preview shows: whole percents of a shown check, or its second pass.
    if (check.shown && (again || qRound(before * 100.0) != qRound(fraction * 100.0)))
        touch(it.value());
}

void Core::onCheckFinished(const fileverify::Result& result)
{
    const QString      key   = result.key;
    const RunningCheck check = m_checks.take(key);
    auto               it    = m_entries.find(key);
    if (it == m_entries.end() || !it->check.running || it->state != MediaState::Downloading)
        return; // nothing waits for it any more
    MediaEntry&   e       = it.value();
    const QString remote  = e.link.remoteFile();
    const QString partial = partialDir(key, false);
    e.check               = ShaCheck();

    switch (result.outcome) {
    case fileverify::Outcome::Match:
        e.check.verified = true;
        m_knownDigests.note(remoteId(e.link), e.link.size, result.received);
        if (result.rechecked)
            ts3::log(LogLevel_WARNING, e.sch, "%1 matched its SHA-256 only on the second check: it was still being written", {ts3::file(remote)});
        ts3::log(LogLevel_DEBUG, e.sch, "Checked %1 against its SHA-256 in %2 ms", {ts3::file(remote), ts3::pub(result.elapsedMs)});
        storeDownload(key, QFileInfo::exists(check.path) ? check.path : findDownloaded(partial, e.link.fileName));
        return;

    case fileverify::Outcome::Mismatch:
        // Final: a new download would get the same bytes. The file is never shown, cached or saved.
        e.check.received = result.received;
        if (result.stableMismatch())
            m_knownDigests.note(remoteId(e.link), e.link.size, result.received); // later links with this made-up hash fail at once
        ts3::log(LogLevel_WARNING, e.sch, "SHA-256 mismatch for %1: expected %2, got %3",
                 {ts3::file(remote), ts3::pub(hashing::toHex(result.expected)), ts3::pub(hashing::toHex(result.received))});
        if (result.rechecked && result.firstReceived != result.received)
            ts3::log(LogLevel_WARNING, e.sch, "The first check of %1 got %2: the file changed between the checks",
                     {ts3::file(remote), ts3::pub(result.firstReceived.isEmpty() ? QStringLiteral("nothing") : hashing::toHex(result.firstReceived))});
        QDir(partial).removeRecursively();
        e.state = MediaState::Idle; // finishDownload already let go of the transfer (failDownload's guard)
        failDownload(key, MediaError::Mismatch, downloadErrorText(MediaError::Mismatch), false, true);
        return;

    case fileverify::Outcome::ReadError:
        ts3::log(LogLevel_WARNING, e.sch, "Couldn't check %1 against its SHA-256: %2", {ts3::file(remote), ts3::pub(result.error)});
        QDir(partial).removeRecursively();
        e.state = MediaState::Idle;
        failDownload(key, MediaError::Other, i18n::t("Couldn't check the downloaded file. Try again."), false, true);
        return;
    }
}

// A link whose sha contradicts the real digest of the same file (same server path and size) can only
// be a made-up hash, or one for a file that was replaced: it fails as Mismatch without a download, so
// links with ever new made-up hashes can't make everyone fetch the file again and again.
bool Core::refuseForgedHash(const QString& key)
{
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->link.sha256.isEmpty() || it->state == MediaState::Ready || it->state == MediaState::Failed
        || it->state == MediaState::Downloading)
        return false;
    MediaEntry& e = it.value();
    if (m_knownDigests.check(remoteId(e.link), e.link.size, e.link.sha256) != fileverify::KnownDigests::Verdict::Contradicts)
        return false;
    fileverify::count(fileverify::Counter::ForgedBlocked);
    e.check          = ShaCheck();
    e.check.received = m_knownDigests.known(remoteId(e.link), e.link.size);
    ts3::log(LogLevel_WARNING, e.sch, "Not downloading %1: its link names another SHA-256 than the file has", {ts3::file(e.link.remoteFile())});
    if (e.state == MediaState::Queued)
        e.state = MediaState::Idle;
    failDownload(key, MediaError::Mismatch, downloadErrorText(MediaError::Mismatch), false, true);
    return true;
}

// A cached preview matches ph (true without one). Hashed once per file version: the result is kept
// until the file's size or time changes.
bool Core::previewFileMatches(const QString& path, const QByteArray& previewSha)
{
    if (previewSha.isEmpty())
        return true;
    const QFileInfo info(path);
    const qint64    size     = info.size();
    const qint64    modified = info.lastModified().toMSecsSinceEpoch();
    auto            it       = m_previewDigests.find(path);
    if (it == m_previewDigests.end() || it->size != size || it->modified != modified) {
        PreviewDigest digest;
        digest.size     = size;
        digest.modified = modified;
        digest.digest   = fileverify::previewDigest(readPreview(path));
        it              = m_previewDigests.insert(path, digest);
        if (it->digest != previewSha)
            ts3::log(LogLevel_INFO, 0, "The cached preview %1 isn't the one a link names; that one is downloaded", {ts3::local(QFileInfo(path).fileName())});
    }
    return it->digest == previewSha;
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

void Core::onServerError(uint64 sch, unsigned int error, const QString& returnCode, const QString& message, bool permissionError, const QString& extraMessage)
{
    auto it = m_ops.find(returnCode);
    if (it == m_ops.end())
        return;
    const PendingOp  op     = it.value();
    const bool       ok     = error == ERROR_ok && !permissionError;
    const MediaError mapped = permissionError ? MediaError::Permission : mapError(error);
    forgetOp(returnCode);
    // TeamSpeak's flood protection says how long to wait ("retry in <n>ms"). Transfers, folders and file
    // info count on the same counter as chat messages: posts wait too.
    const bool flooded   = error == ERROR_client_is_flooding && !permissionError;
    const int  retryHint = flooded ? FloodGovernor::retryHintMs(extraMessage) : -1;
    if (flooded && op.type != OpType::PostMessage) {
        floodGovernor(sch).generalFlooded(floodClockMs(), retryHint);
        ts3::log(LogLevel_WARNING, sch, "TeamSpeak's flood protection held back a file request; chat posts wait %1 ms",
                 {ts3::pub(floodGovernor(sch).counters().lastPauseMs)});
        floodStateChanged(sch);
    }

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

    case OpType::MakeDirectory: {
        // Flooded: asked again after the pause, never the channel root because of it.
        if (flooded) {
            if (retryFlooded(op.uploadId))
                createRemoteDirectory(op.uploadId, op.preview);
            else if (op.preview)
                onDirectoryReady(op.uploadId, true, false); // the preview goes next to the file
            else
                failUpload(op.uploadId, uploadErrorText(mapped, message));
            break;
        }
        const bool created = ok || error == ERROR_file_already_exists;
        if (!op.key.isEmpty())
            m_remoteDirs.insert(op.key, created); // refused (no permission): the next file doesn't ask again
        onDirectoryReady(op.uploadId, op.preview, created);
        break;
    }

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
        auto job = m_uploads.find(op.uploadId);
        if (job == m_uploads.end() || !job->transferActive || job->transferId != op.transferId)
            break;
        if (error == ERROR_file_already_exists && !permissionError) {
            resendWithNewName(op.uploadId, op.transferId);
        } else if (flooded && retryFlooded(op.uploadId)) {
            // Not started: it goes again once the governor allows it (the pause is on now).
            m_uploadsByTransfer.remove(job->transferId);
            job->transferActive = false;
            job->transferId     = 0;
            job->state          = UploadState::Preparing; // startSend starts from there
            job->progress       = 0.0;
            startSend(op.uploadId);
        } else {
            // The folder may be gone (deleted on the server): the next file asks for it again.
            m_remoteDirs.remove(remoteDirKey(sch, job->channelId, job->remoteDir));
            failUpload(op.uploadId, uploadErrorText(mapped, message));
        }
        break;
    }

    case OpType::RemoteDelete:
        if (!ok)
            ts3::log(QStringLiteral("Could not remove %1 from the file browser: %2").arg(op.key, message), LogLevel_WARNING, sch);
        break;

    case OpType::PostMessage:
        finishPost(returnCode, error, message, permissionError, retryHint);
        break;
    }
}

void Core::onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch)
{
    Q_UNUSED(sch);
    const bool complete = status == ERROR_file_transfer_complete;
    // The answer to our own haltTransfer. Its id may already belong to a new transfer.
    const bool ownHalt = m_haltedTransfers.remove(transferId) && status == ERROR_file_transfer_canceled;
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
    if (ownHalt)
        return;
    if (auto it = m_uploadsByTransfer.constFind(transferId); it != m_uploadsByTransfer.constEnd()) {
        const int id = it.value();
        if (complete)
            finishUpload(id);
        else if (status == ERROR_file_already_exists)
            resendWithNewName(id, transferId);
        else if (status == ERROR_file_transfer_canceled) // stopped elsewhere (TeamSpeak's transfer list, another plugin): frees its slot
            failUpload(id, i18n::t("The upload was stopped. Try again."));
        else
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
        // 2.2 sha: a file being checked is complete already; the check goes on without the server.
        if (it->sch == sch && (it->state == MediaState::Downloading || it->state == MediaState::Queued) && !it->check.running)
            mains.append(it.key());
        if ((it->previewState == MediaState::Downloading || it->previewState == MediaState::Queued) && m_previewSch.value(it.key()) == sch)
            previews.append(it.key());
    }
    for (const QString& key : qAsConst(mains))
        failDownload(key, MediaError::NotConnected, {});
    for (const QString& key : qAsConst(previews))
        failPreviewDownload(key, QStringLiteral("disconnected"), true);

    QVector<int> ids;
    QList<int>   heldBatches; // sends with uploaded files whose message waits for earlier files or the rest of the album
    for (const auto& job : qAsConst(m_uploads)) {
        if (job.target.sch != sch)
            continue;
        if (isPreparing(job.state))
            ids.append(job.id);
        else if (job.state == UploadState::Posting && job.waiting && !heldBatches.contains(job.batch))
            heldBatches.append(job.batch);
    }
    std::sort(ids.begin(), ids.end());
    std::sort(heldBatches.begin(), heldBatches.end());
    m_fileQueue.remove(sch);
    for (auto it = m_remoteDirs.begin(); it != m_remoteDirs.end();) { // folders are asked for again
        if (it.key().startsWith(QString::number(sch) + QLatin1Char('|')))
            it = m_remoteDirs.erase(it);
        else
            ++it;
    }
    // Uploaded files that wait for their turn: their messages are composed now, without the files still
    // on their way, so they fail like the queued ones below and Retry posts them once connected again.
    for (int batch : qAsConst(heldBatches)) {
        for (int u = 0;; ++u) {
            const auto b = m_batches.constFind(batch);
            if (b == m_batches.constEnd() || u >= b->units.size())
                break;
            if (!b->units.at(u).composed)
                composeUnit(batch, u);
        }
    }
    // Messages that were waiting for their turn won't be sent on this connection any more.
    const QList<PostItem> queued = m_postQueue.take(sch);
    QVector<int>          posted; // uploaded files whose message failed
    for (const PostItem& post : queued) {
        for (int id : post.jobs) {
            if (const UploadJob* job = upload(id); job && job->uploaded) {
                m_failedPosts.insert(id, post);
                posted.append(id);
            }
        }
    }
    // One chat line for all of them; each job keeps its own reason.
    warnFailed(sch, ids + posted, ids.isEmpty() ? postErrorText(MediaError::NotConnected) : uploadErrorText(MediaError::NotConnected));
    for (int id : qAsConst(ids))
        failUpload(id, uploadErrorText(MediaError::NotConnected), true);
    for (const PostItem& post : queued) {
        if (post.jobs.isEmpty())
            failPost(post, postErrorText(MediaError::NotConnected)); // a caption of its own: says so
        for (int id : post.jobs)
            failUpload(id, postErrorText(MediaError::NotConnected), true);
    }
    QVector<int> leftovers; // held without a message of their own (none expected)
    for (const auto& job : qAsConst(m_uploads)) {
        if (job.target.sch == sch && job.state == UploadState::Posting && job.waiting)
            leftovers.append(job.id);
    }
    for (int id : qAsConst(leftovers))
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
    emit cacheCleared(); // 2.2 protocol: reactions.json goes with it
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

QString noRecipientText()
{
    return i18n::t("Can't tell who this private chat is with (they may have left the server). Nothing was sent.");
}
