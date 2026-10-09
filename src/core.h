#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QVector>

#include <QElapsedTimer>

#include <atomic>
#include <memory>

#include "fileverify.h" // 2.2 sha
#include "albums.h" // 2.2 album
#include "floodgovernor.h"
#include "medialink.h"
#include "mediaprobe.h"
#include "spoiler.h" // 2.2 spoiler
#include "ts3api.h"

class QTimer;
class QWidget;

enum class MediaState { Idle, Queued, Downloading, Ready, Failed };
// MediaError and its texts (downloadErrorTitle / downloadErrorText) are in medialink.h.

// 2.2 sha: the main file checked against link.sha256 (fileverify.h). While the check runs the entry
// stays Downloading at progress 1.0, so every switch over MediaState keeps working. Previews say
// "Checking file…" once it has taken a moment (shown), so small files don't flash it; see
// previewrenderer.h (isCheckingShown, checkingText).
struct ShaCheck {
    bool       running  = false;
    bool       shown    = false; // running for a moment now
    bool       again    = false; // the second pass, after a first one that didn't match
    double     progress = -1.0;  // of the current pass, 0..1; < 0 before its first report
    bool       verified = false; // Ready and known to match link.sha256 (checked here, or our own upload)
    QByteArray received;         // MediaError::Mismatch: what the downloaded file hashed to
};

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
    // 2.2 data saver: Idle because data saver held back its automatic download (it waits for a click).
    bool       heldByDataSaver = false;
    int        revision        = 0; // bumped on every change (entryChanged)

    // Small preview / video poster (link.previewFile), downloaded automatically when present.
    MediaState previewState      = MediaState::Idle;
    QString    previewPath; // exists when previewState == Ready
    anyID      previewTransferId = 0;

    ShaCheck check; // 2.2 sha
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

// Preparing: copied to the staging folder, probed, its preview uploaded. Compressing (2.4): a video
// being made smaller before the upload; it counts as running wherever Preparing does. Uploading: the
// file itself. Posting: the chat message announcing it is being sent (or waits for its turn). Done,
// Failed and Canceled are final.
enum class UploadState { Preparing, Compressing, Uploading, Posting, Done, Failed, Canceled };

// ---- 2.2: one way to send ------------------------------------------------------------------------

// Video quality for the compression planner (2.4); until then every video is sent as it is.
enum class SendQuality { Auto, Original, P1080, P720, P480 };

struct SendItem {
    QString     path;              // the file to send
    QString     displayName;       // its title in the toast and in warnings; empty: the file's name
    bool        ownTemp    = false; // our own file (pasted, edited, compressed): deleted with the job
    bool        pasted     = false; // a pasted picture ("Pasted image", sent as new_photo_<hex>)
    bool        spoiler    = false; // sp=1; pictures, GIFs and videos only
    SendQuality quality    = SendQuality::Auto;
    bool        voice      = false; // a recorded voice message (vm=1; .m4a, sent as voice_message_<hex>)
    qint64      durationMs = 0;     // voice: the recorder's length (else the probe's)
    QByteArray  waveform;           // voice: MediaLink::kWaveformLevels levels of 0..15
};

struct SendRequest {
    ChatTarget        target;
    QVector<SendItem> items;   // their messages appear in this order (albums first, see Core::send)
    QString           caption; // as typed; shown above the first file or album (never cut)
    bool              album = false; // 2 or more pictures/videos go out as albums of up to 10
};

struct UploadJob {
    int         id    = 0;
    int         batch = 0; // jobs from one send() share it: their chat messages keep its order
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
    // its batch are posted ("Waiting for earlier files…") or until the rest of its album is uploaded
    // ("Waiting for the rest of the album…"). Show it without a moving progress bar.
    bool        waiting  = false;
    bool        uploaded = false; // the file reached the server (it stays there even if posting fails)
    double      progress = 0.0;   // main file upload progress 0..1
    QString     message;          // status of the current step; the full error text when Failed
    QString     displayName;      // SendItem::displayName (see displayNameFor)
    bool        spoiler = false;  // SendItem::spoiler
    bool        inAlbum = false;  // posted together with the other pictures/videos of its album

    // Filled by the probe step (worker thread) before uploading.
    LocalMediaInfo info;
    QString        previewRemotePath; // set once the preview/poster upload succeeded
};

// The title of an upload in the toast and in chat warnings: its SendItem::displayName, "Pasted image"
// for pasted pictures, otherwise the source file's name (always through displayFileName).
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
    //  * audio files by audioplayback::autoDownloadLimit() (2.2 audio: plain audio like videos)
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

    // ---- 2.2 drag-out ----------------------------------------------------------------------
    // A copy of a Ready file to hand to Explorer or another app (drag or "Copy file"): a hard link
    // with a clean name (filenames::exportFileName) in <data dir>/export/<random>/, else a copy for
    // files up to 64 MB, else the cache file itself. It carries the Windows Mark-of-the-Web
    // (Zone.Identifier, ZoneId=3: programs get SmartScreen, documents open in Protected View).
    // Empty if the file is not Ready (*error says why, for the log).
    QString prepareExport(const QString& key, QString* error = nullptr);
    // Removes export folders older than maxAgeMs (still locked ones are skipped). Never at shutdown:
    // a copied file must still paste after TeamSpeak is closed.
    void pruneExports(qint64 maxAgeMs);

    // ---- 2.2 data saver --------------------------------------------------------------------
    // After the data saver changed (globally or for a server): automatic downloads of servers that
    // now save data stop (their items wait for a click), held items of servers that no longer do
    // start again once they are shown. Downloads the user asked for are never touched.
    void onDataSaverChanged();

    // ---- spoilers (2.2) -------------------------------------------------------------------------
    // A picture, GIF or video seen with sp=1 (on any of its links this session) that the user hasn't
    // revealed: previews and the viewer show the blurred cover, it doesn't animate or play, and a click
    // only reveals it. Reactions, drags and copies must leave it covered (ask this before starting one).
    // False for everything while Settings::revealSpoilers is on.
    bool isSpoilerHidden(const QString& key) const;
    bool isSpoiler(const QString& key) const; // flagged, revealed or not, whatever the setting
    // Reveals a spoiler, or covers it again, until TeamSpeak restarts. The chat and the viewer follow
    // through entryChanged(key).
    void setSpoilerRevealed(const QString& key, bool revealed);
    // The content of key was drawn uncovered: a later repost with sp=1 can't cover it any more.
    void noteShownOpen(const QString& key);
    // Settings::revealSpoilers changed: every spoiler is drawn again (entryChanged).
    void spoilerSettingChanged();

    // ---- uploads -------------------------------------------------------------------------
    // 2.2: every send goes through send(). Each file is probed on a worker thread (size, duration,
    // blurhash, preview), the preview is uploaded to <uploadDirectory>/previews/<8 hex>.jpg, then the
    // file. At most two files are transferred at once. The chat messages (composeChatMessages: caption
    // first, several links per message when they fit) appear in the request's order: albums first,
    // each posted once all of its items are uploaded, failed or canceled (failed ones are left out and
    // the rest renumbered), then the other files one by one. A file that finishes early waits (see
    // UploadJob::waiting). Posts go out one at a time through the connection's FloodGovernor.
    // Returns the batch id, 0 if nothing was started (not connected, no file found).
    int              send(const SendRequest& request);
    // The 2.1 entry points: send() with default options (no caption, no spoiler, no album).
    void             uploadFiles(const QStringList& paths, const ChatTarget& target);
    void             uploadImage(const QImage& image, const ChatTarget& target);
    // While Preparing, Compressing or Uploading, or while uploaded but its chat message still waits for
    // earlier files or the rest of its album (nothing was posted yet: the file and its preview are
    // removed from the server again).
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

    // ---- flood control (2.2) -------------------------------------------------------------------
    // The FloodGovernor of a connection, shared by Core's chat posts and the plugin-command transport
    // (PluginLink): ask it before each command (commandReady), then report commandSent and the answer
    // (commandFlooded on ERROR_client_is_flooding, answeredOk otherwise). Created on first use, dropped
    // when the connection is lost: use the reference right away, never keep it. GUI thread only. Times
    // are floodClockMs(); floodGovernorChanged(sch) says when commands may be worth trying again.
    FloodGovernor& floodGovernor(uint64 sch);
    qint64         floodClockMs() const; // the governors' monotonic clock
    // A command was flooded (or answered) on sch: Core re-plans its posts (a pause applies to both).
    void           floodStateChanged(uint64 sch);

    // ---- albums (2.2) ----------------------------------------------------------------------------
    // Who posted an album item: the album's sender (TeamSpeak's unique id) when key is the item at
    // index of that album as it was received (onTextMessage) or posted by us; empty when unknown, e.g.
    // after a plugin reload or for anyone else's item under the same album id.
    QString albumSender(const QString& serverUid, quint32 albumId, int index, const QString& key) const;

    // ---- TeamSpeak callbacks (called on the GUI thread) ------------------------------------
    bool isOwnReturnCode(const QString& returnCode) const; // thread-safe
    // 2.2 album: senderUid is TeamSpeak's unique id of the sender (fromUniqueIdentifier).
    void onTextMessage(uint64 sch, const QString& message, const QString& senderUid = QString());
    // extraMessage: TeamSpeak's extra text (on 0x020c "retry in <n>ms").
    void onServerError(uint64 sch, unsigned int error, const QString& returnCode, const QString& message, bool permissionError,
                       const QString& extraMessage = {});
    void onTransferStatus(anyID transferId, unsigned int status, const QString& message, uint64 sch);
    void onConnectionLost(uint64 sch);

  signals:
    void entryChanged(const QString& key);
    void uploadChanged(int id); // state, progress or waiting changed, or the job was removed
    void openRequested(const QString& key); // a download started with openWhenReady finished
    // The posts of sch are all out (or a flood pause ended): plugin commands may go again.
    void floodGovernorChanged(uint64 sch);
    void cacheCleared(); // 2.2 protocol: clearCache() ran (the reactions cache is cleared with it)

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
        fileverify::StagedDigest digest;      // 2.2 sha: of the final staged bytes (finalizeStaged)
        bool       hashed = false;            // 2.2 sha: digest is set (empty sha = couldn't be read)
    };

    // ---- 2.2 posting ----------------------------------------------------------------------
    // A send is a batch of units posted in order: an album (its messages go once all of its uploads
    // are settled) or a single file.
    struct PostUnit {
        QVector<int> jobs;             // upload ids, in post order
        bool         album    = false;
        bool         composed = false; // its messages are queued (or nothing of it reached the server)
    };
    struct BatchInfo {
        QString           caption;             // as typed
        bool              captionDone = false; // queued with a unit (or handed on to a retry)
        QVector<PostUnit> units;
    };
    // One chat message waiting in its connection's queue.
    struct PostItem {
        uint64       sch       = 0;
        ChatTarget   target;
        uint64       channelId = 0;
        QByteArray   text;          // UTF-8
        QVector<int> jobs;          // the uploads it announces; empty for a caption of its own
        int          attempts = 0;  // times sent
    };
    struct InFlightPost {
        PostItem item;
        quint64  ticket = 0; // FloodGovernor::postSent
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

    int     createUpload(const SendItem& item, const QString& remoteName, const ChatTarget& target, int batch);
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
    QString heldText(const UploadJob& job) const; // why an uploaded file's message waits; empty: its turn
    void    pumpPostUnits();                      // composes the units whose turn has come
    void    composeUnit(int batch, int index);
    void    pumpPosts();                          // sends queued messages as their governors allow
    bool    sendPost(PostItem& item);             // false: refused at once (already reported)
    void    finishPost(const QString& returnCode, unsigned int error, const QString& message, bool permissionError, int retryHintMs = -1);
    void    unansweredPost(const QString& returnCode); // no answer within kPostTimeoutMs: failed, never "sent"
    void    failPost(const PostItem& item, const QString& text, const QString& captionText = {});
    int     repostFailed(int id, const ChatTarget& target);
    void    forgetBatchIfDone(int batch);
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

    // ---- 2.2 sha: hashing what is sent, checking what is received (fileverify.h) -----------------
    // The digest of the final staged bytes. The staging worker computes it (fileverify::finalizeStaged)
    // right after the probe; whatever changes the staged file later (2.4: a transcode, an export) must
    // run finalizeStaged again on its worker and hand the result here before the upload starts.
    void onStagedFinalized(int id, const fileverify::StagedDigest& digest);
    void storeDownload(const QString& key, const QString& found); // a finished (and checked) download into the cache
    bool startCheck(const QString& key, const QString& found);   // false: the link has no sha
    void onCheckProgress(const QString& key, double fraction, bool secondPass);
    void onCheckFinished(const fileverify::Result& result);
    void showSlowChecks();
    bool refuseForgedHash(const QString& key); // its sha contradicts the file's known one: failed at once
    bool previewFileMatches(const QString& path, const QByteArray& previewSha);

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
    SpoilerState               m_spoilers;            // 2.2 spoiler: flagged and revealed keys (this session)

    QHash<int, UploadJob>   m_uploads;
    QHash<int, UploadExtra> m_uploadExtra;
    QHash<anyID, int>       m_uploadsByTransfer;
    QSet<anyID>             m_haltedTransfers; // halted by us (any kind): their late "canceled" status is no news
    QHash<anyID, int>       m_previewUploadsByTransfer;
    QHash<int, QString>     m_probing; // upload id -> staging dir, while its staging copy / probe runs
    QHash<int, std::shared_ptr<std::atomic<bool>>> m_stagingCancel; // upload id -> stops its staging copy
    QHash<int, QString>     m_deleteAfterProbe; // upload id -> pasted file to delete once its worker is done
    QList<int>              m_sendQueue; // probed jobs waiting for an upload slot, by id (= post order)
    int                     m_nextUploadId = 0;
    int                     m_nextBatch    = 0;
    bool                    m_uploadQueueScheduled = false;

    // 2.2 posting
    QHash<int, SendItem>            m_jobItems;      // upload id -> what was asked for (kept for a retry)
    QHash<int, BatchInfo>           m_batches;       // batch id -> its units and caption
    QHash<uint64, QList<PostItem>>  m_postQueue;     // connection -> messages waiting to be sent, in order
    QHash<QString, InFlightPost>    m_postsInFlight; // return code -> a message waiting for its answer
    QHash<int, PostItem>            m_failedPosts;   // upload id -> its message that failed or got no answer (Retry posts it)
    QHash<uint64, FloodGovernor>    m_flood;         // connection -> its governor (posts and plugin commands)
    QTimer*                         m_postTimer = nullptr; // wakes pumpPosts() when a governor allows the next post
    QElapsedTimer                   m_clock;
    albums::Registry                m_albums;        // 2.2 album: who posted which album

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

    // 2.2 sha
    struct RunningCheck {
        qint64  startedAt = 0; // floodClockMs()
        QString path;          // the downloaded file being checked (in its partial folder)
    };
    struct PreviewDigest {
        qint64     size     = -1;
        qint64     modified = 0;
        QByteArray digest; // fileverify::previewDigest of the file
    };
    fileverify::Verifier*         m_verifier = nullptr;   // checks finished downloads; shut down first in ~Core
    fileverify::KnownDigests      m_knownDigests;         // real digests of server files (forged-hash guard)
    QHash<QString, RunningCheck>  m_checks;               // key -> its running check
    QTimer*                       m_checkTimer = nullptr; // "Checking file…" for checks that take a moment
    QHash<QString, PreviewDigest> m_previewDigests;       // cached preview path -> its digest, until the file changes
};
