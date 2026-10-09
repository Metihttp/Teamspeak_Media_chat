#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <memory>

#include "medialink.h"
#include "mediaprobe.h"
#include "ts3api.h"

class QTimer;
class QWidget;

enum class MediaState { Idle, Queued, Downloading, Ready, Failed };
// MediaError and its texts (downloadErrorTitle / downloadErrorText) are in medialink.h.

struct MediaEntry {
    MediaLink  link;
    MediaKind  kind  = MediaKind::Other;
    MediaState state = MediaState::Idle; // main file
    MediaError error = MediaError::None;
    QString    errorText; // when Failed: the full explanation (downloadErrorText or a more specific one)
    QString    localPath; // cache path of the main file (exists when state == Ready)
    uint64     sch             = 0;
    anyID      transferId      = 0;
    double     progress        = 0.0; // main file download progress 0..1
    bool       openWhenReady   = false;
    bool       tooLargeForAuto = false;
    bool       isOwnUpload     = false;
    int        revision        = 0; // bumped on every change (entryChanged)

    // Small preview / video poster (link.previewFile), downloaded automatically when present.
    MediaState previewState      = MediaState::Idle;
    QString    previewPath; // exists when previewState == Ready
    anyID      previewTransferId = 0;
};

// Where the chat message announcing an upload is posted.
struct ChatTarget {
    uint64 sch      = 0;
    int    mode     = TextMessageTarget_CHANNEL;
    anyID  clientId = 0; // for TextMessageTarget_CLIENT
    // Who it was meant for when it was chosen. Client ids are reassigned on a reconnect and reused
    // after someone leaves, and a server tab can be connected to another server: a retry, or a message
    // posted long after the send started, checks the server and finds the partner again by identity.
    QString serverUid; // empty: filled in by Core when the upload is created
    QString clientUid; // for TextMessageTarget_CLIENT
};

// Preparing: copied to the staging folder, probed, its preview uploaded. Uploading: the file itself.
// Posting: the chat message announcing it is being sent. Done, Failed and Canceled are final.
enum class UploadState { Preparing, Uploading, Posting, Done, Failed, Canceled };

struct UploadJob {
    int         id    = 0;
    int         batch = 0; // jobs from one uploadFiles() call share it: their chat messages keep that order
    ChatTarget  target;
    uint64      channelId = 0;
    QString     sourcePath;
    bool        deleteSource = false;
    bool        pasted       = false; // uploadImage(): sourcePath is a temporary file with a made-up name
    QString     stagingDir;
    QString     remoteDir;
    QString     remoteName;
    quint64     size           = 0;
    anyID       transferId     = 0;
    bool        transferActive = false;
    UploadState state          = UploadState::Preparing;
    // Waiting for its turn rather than working. In Preparing: for a free worker or upload slot
    // ("Waiting to upload…"). In Posting: uploaded, the chat message waits until the earlier files of
    // its batch are posted ("Waiting for earlier files…"). Show it without a moving progress bar.
    bool        waiting  = false;
    bool        uploaded = false; // the file reached the server (it stays there even if posting fails)
    double      progress = 0.0;   // main file upload progress 0..1
    QString     message;          // status of the current step; the full error text when Failed

    // Filled by the probe step (worker thread) before uploading.
    LocalMediaInfo info;
    QString        previewRemotePath; // set once the preview/poster upload succeeded
};

// The title of an upload in the toast and in chat warnings: "Pasted image" for pasted pictures,
// otherwise the source file's name (displayFileName).
QString displayNameFor(const UploadJob& job);

// "You're not connected to a server. Connect to one to send files." Core's chat warning and the
// checks before a send (picker, paste, drop) say the same.
QString notConnectedText();
// A private chat whose partner can't be found (they left the server, were renamed, ...): nothing is
// sent. Said before a send and by a retry.
QString noRecipientText();

// Best still picture currently available for an entry.
struct MediaStill {
    enum Source { None, BlurHash, Preview, Full };
    QImage image; // scaled to fit the requested size (keeps aspect ratio), null if Source::None
    Source source = None;
};

class Core : public QObject
{
    Q_OBJECT

  public:
    explicit Core(QObject* parent = nullptr);
    ~Core() override; // waits for probe workers

    static Core* instance();

    void start();

    // ---- received media ------------------------------------------------------------------
    const MediaEntry* entry(const QString& key) const;
    QStringList       keys() const; // in the order they were first seen

    // Registers a link seen in chat. Starts automatic downloads according to Settings:
    //  * preview/poster (link.previewFile) always, it is small
    //  * images/GIFs up to autoDownloadMaxMB (otherwise only the preview)
    //  * videos up to videoAutoDownloadMB (0 = never automatically)
    void ensure(const MediaLink& link);

    // Downloads the main file if it is not Ready / in progress. With openWhenReady, emits
    // openRequested(key) once it is available (immediately if already Ready).
    void download(const QString& key, bool openWhenReady);
    void retry(const QString& key); // after a failure (no-op for NotFound)

    // Full image if downloaded, else preview/poster, else the decoded BlurHash (at the link's
    // aspect ratio), scaled to fit maxPixels. Results are cached. Files that are slow to decode
    // (chat files are untrusted) are decoded on a worker thread: until then the next best still
    // is returned, and entryChanged(key) is emitted once the better one is available.
    MediaStill still(const QString& key, const QSize& maxPixels);

    // File actions for context menus / viewer (no-ops if the main file is not Ready).
    void openExternally(const QString& key) const; // default app; executables are only revealed
    void revealInFolder(const QString& key) const;
    // True if openExternally(key) only shows the file in its folder: programs and scripts from chat
    // (.exe, .js, .lnk, ...) are never run. Lets a UI say so instead of offering "Open".
    bool isUnsafeToOpen(const QString& key) const;
    // Asks where to save a copy (synchronous: the dialog, then the copy). Returns the saved file's
    // path, or an empty string if the user canceled or saving failed (a message box already said
    // why). Confirm a success with ui::savedToText(path).
    QString saveAs(const QString& key, QWidget* parent) const;

    // Files in use (playing video, open viewer) are never evicted from the cache. Counted: every
    // setInUse(key, true) must be balanced by one setInUse(key, false).
    void setInUse(const QString& key, bool inUse);

    // ---- uploads -------------------------------------------------------------------------
    // Each file is probed on a worker thread (size, duration, blurhash, preview), the preview is
    // uploaded to <uploadDirectory>/previews, then the file, then composeChatMessage() is posted.
    // At most two files are transferred at once; the chat messages of one uploadFiles() call appear
    // in the order of paths (a file that finishes early waits, see UploadJob::waiting).
    void             uploadFiles(const QStringList& paths, const ChatTarget& target);
    void             uploadImage(const QImage& image, const ChatTarget& target);
    // While Preparing or Uploading, or while uploaded but its chat message still waits for earlier
    // files (nothing was posted yet: the file and its preview are removed from the server again).
    void             cancelUpload(int id);
    bool             canCancelUpload(int id) const;
    const UploadJob* upload(int id) const; // nullptr once the job is gone
    QList<int>       uploadIds() const;    // every job Core still has, oldest first

    // Done and Canceled jobs are dropped after 30 s, Failed ones are kept until dismissed (or after
    // 30 min) so the user can retry them. Removing a job emits uploadChanged(id) once more.
    void dismissUpload(int id); // forgets a finished job; no-op while it runs
    // A failed job can be sent again while its source still exists, unless the file already reached
    // the server (only the chat message failed: a retry would upload a duplicate). Pasted images are
    // kept until their failed job is dismissed.
    bool canRetryUpload(int id) const;
    // Sends the file of a failed job again as a new job and removes the failed one. Returns the new
    // job's id, or 0 if it cannot be retried (also when not connected: the job then stays).
    int  retryUpload(int id);

    // ---- cache ---------------------------------------------------------------------------
    QString cacheDir() const;
    quint64 cacheSize() const;
    void    clearCache();
    void    openCacheFolder() const;
    void    applyCacheLimit(); // after Settings::cacheLimitMB changed: trims the cache to it shortly

    // ---- TeamSpeak callbacks (called on the GUI thread) ------------------------------------
    bool isOwnReturnCode(const QString& returnCode) const; // thread-safe
    void onTextMessage(uint64 sch, const QString& message);
    void onServerError(uint64 sch, unsigned int error, const QString& returnCode, const QString& message, bool permissionError);
    void onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch);
    void onConnectionLost(uint64 sch);

  signals:
    void entryChanged(const QString& key);
    void uploadChanged(int id); // state, progress or waiting changed, or the job was removed
    void openRequested(const QString& key); // a download started with openWhenReady finished

    // ---- implementation (owned by core.cpp; may be reorganised freely) -----------------------
  private:
    enum class OpType { Download, MakeDirectory, Upload, PostMessage, RemoteDelete };
    struct PendingOp {
        OpType  type       = OpType::Download;
        QString key;          // downloads; the remote path for RemoteDelete
        int     uploadId = 0; // uploads
        bool    preview  = false;
        anyID   transferId = 0; // set once the transfer has started (ignores late answers for older attempts)
    };

    // Per-upload data that is not part of the public UploadJob.
    struct UploadExtra {
        QByteArray previewJpeg;   // encoded preview / poster, empty = none
        QString    previewRemote; // where the preview is being uploaded to
        bool       previewInFolder   = true; // <dir>/previews/<base>.jpg, else <dir>/<base>.preview.jpg
        anyID      previewTransferId = 0;
        bool       previewActive     = false;
        int        renames           = 0;     // new names after "file already exists"
        bool       mainUploaded      = false; // the main file is complete on the server
        MediaLink  link;                      // the link to post, once uploaded
    };

    // Downloads interrupted by a lost connection, restarted once the server is reachable again.
    struct Resume {
        bool main         = false;
        bool mainExplicit = false; // the user asked for it (no automatic size rules)
        bool preview      = false;
    };

    QString registerOp(OpType type, const QString& key, int uploadId, bool preview = false);
    void    setOpTransfer(const QString& returnCode, anyID transferId);
    void    forgetOp(const QString& returnCode);

    QString cachePathFor(const MediaLink& link) const;
    QString partialDir(const QString& key, bool preview) const;
    void    touch(MediaEntry& e);
    void    startAutoDownloads(const QString& key);
    void    startDownload(const QString& key);
    void    startPreviewDownload(const QString& key);
    void    pumpDownloadQueue();
    void    finishDownload(const QString& key);
    void    finishPreviewDownload(const QString& key);
    void    finishWhenWritten(const QString& key, bool preview, int attempt);
    void    failDownload(const QString& key, MediaError error, const QString& text, bool interrupted = false, bool exactText = false);
    void    failPreviewDownload(const QString& key, const QString& text, bool interrupted = false);
    void    abortOversizedAutoDownload(const QString& key);
    void    abortChangedDownload(const QString& key, quint64 actualSize);
    quint64 autoDownloadLimit(const MediaEntry& e) const;
    void    haltDownload(uint64 sch, anyID transferId);
    void    noteRemoteSize(const MediaLink& link, quint64 size);
    void    noteInterrupted(const QString& key, const Resume& what);
    void    resumeInterrupted();
    void    invalidateStills(const QString& key);
    void    invalidateStill(const QString& key, MediaStill::Source source);
    void    startStillDecode(const QString& key, const QString& cacheKey, MediaStill::Source source, const QString& path, const QSize& maxPixels);
    void    onStillDecoded(const QString& key, const QString& cacheKey, quint64 job, MediaStill::Source source, const QString& path, const QImage& image);
    MediaStill otherSizeStill(const QString& key, MediaStill::Source source, const QSize& maxPixels);
    void    rearmIfNeeded(const QString& key);
    void    markUsed(const QString& path);
    void    scheduleCacheLimit(const QString& justFinishedKey);
    void    enforceCacheLimit();

    int     createUpload(const QString& sourcePath, const QString& remoteName, const ChatTarget& target, bool deleteSource, bool pasted, int batch);
    void    markProbeStarted(int id);
    void    onProbed(int id, bool staged, quint64 stagedSize, const LocalMediaInfo& info, const QByteArray& previewJpeg);
    void    createRemoteDirectory(int id, bool previews);
    void    onDirectoryReady(int id, bool previews, bool ok);
    void    startPreviewSend(int id);
    void    finishPreviewUpload(int id, bool ok, const QString& reason, unsigned int error);
    void    abortPreviewUpload(UploadJob& job);
    void    startSend(int id);
    void    resendWithNewName(int id, anyID failedTransfer);
    bool    renameUpload(UploadJob& job);
    void    finishUpload(int id);
    void    postUploadMessage(int id);
    bool    waitsForEarlierPosts(const UploadJob& job) const;
    void    seedCache(const UploadJob& job, const MediaLink& link);
    void    failUpload(int id, const QString& text);
    void    setUploadState(UploadJob& job, UploadState state, const QString& message = {}, bool waiting = false);
    void    scheduleUploadQueue();
    void    runUploadQueue();
    void    forgetUpload(int id);
    void    releaseSource(UploadJob& job);
    void    cleanupUpload(UploadJob& job);
    void    deleteRemoteFile(uint64 sch, uint64 channelId, const QString& path);
    QString previewDirFor(const UploadJob& job) const;
    QString previewRemoteFor(const UploadJob& job, bool inFolder) const;

    void updateProgress();
    void ensureProgressTimer();

    static MediaError mapError(unsigned int error);
    static QString    makeRemoteName(const QString& originalName);
    static QString    renamedRemoteName(const QString& remoteName);
    static QString    remoteId(const MediaLink& link);

    QHash<QString, MediaEntry> m_entries;
    QStringList                m_order;
    QStringList                m_downloadQueue;
    QStringList                m_previewQueue; // served before m_downloadQueue
    QHash<anyID, QString>      m_downloadsByTransfer;
    QHash<anyID, QString>      m_previewsByTransfer;
    QHash<QString, uint64>     m_previewSch;          // key -> connection of its (last) preview transfer
    int                        m_activeDownloads = 0; // main files and previews
    QSet<QString>              m_autoDownloads;       // started automatically: aborted if larger than the auto limit
    QSet<QString>              m_autoPending;         // registered while inline previews were off
    QSet<QString>              m_rearm;               // cache cleared/evicted: automatic downloads restart when shown
    QSet<QString>              m_evictionRearmed;     // re-armed after an eviction already (no evict/refetch churn)
    QHash<QString, quint64>    m_remoteSizes;         // remoteId -> real size seen on the server
    QHash<QString, Resume>     m_resume;              // key -> what to restart once its server is reachable
    QHash<QString, int>        m_resumeAttempts;      // key -> automatic restarts without a success in between
    QHash<QString, int>        m_inUse;               // key -> number of users (players, viewer, animations)

    QHash<int, UploadJob>   m_uploads;
    QHash<int, UploadExtra> m_uploadExtra;
    QHash<anyID, int>       m_uploadsByTransfer;
    QSet<anyID>             m_haltedTransfers; // halted by us (any kind): their late "canceled" status is no news
    QHash<anyID, int>       m_previewUploadsByTransfer;
    QHash<int, QString>     m_probing; // upload id -> staging dir, while its staging copy / probe runs
    QHash<int, std::shared_ptr<std::atomic<bool>>> m_stagingCancel; // upload id -> stops its staging copy
    QHash<int, QString>     m_deleteAfterProbe; // upload id -> pasted file to delete once its worker is done
    QList<int>              m_sendQueue; // probed jobs waiting for an upload slot, by id (= selection order)
    int                     m_nextUploadId = 0;
    int                     m_nextBatch    = 0;
    bool                    m_uploadQueueScheduled = false;

    QHash<QString, PendingOp> m_ops;
    mutable QMutex            m_returnCodesMutex;
    QSet<QString>             m_returnCodes;

    QCache<QString, MediaStill> m_stills;
    QHash<QString, quint64>     m_stillJobs;     // still cache key -> id of the worker decode producing it
    quint64                     m_nextStillJob = 0;
    QHash<QString, qint64>      m_usedAt;         // path -> last LRU refresh (ms since epoch)
    QSet<QString>               m_recentlyFinished; // spared by the next cache limit pass
    QTimer*                     m_progressTimer = nullptr;
    QTimer*                     m_cacheTimer    = nullptr;
    QTimer*                     m_resumeTimer   = nullptr;
    QThreadPool                 m_pool;      // staging copies + probes; destroyed (and waited for) with Core
    QThreadPool                 m_stillPool; // expensive still decodes (untrusted files); waited for with Core
};
