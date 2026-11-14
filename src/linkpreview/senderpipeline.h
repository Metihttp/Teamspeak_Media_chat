#pragma once

// 2.4 link previews, the sender's side (the shared contract's SenderPipeline): your own sent chat message
// with a previewable link -> one fetch from the website (Fetcher) -> the preview files (previewmaker.h, on a
// worker) -> Core's upload path (a background send: the channel's file browser like any TS Media file, its
// folders, SHA-256, flood governor and the 2.3.2 server check, which never asks TeaSpeak for file info) ->
// ONE LP plugin command (announcement.h) to the same chat, so everyone with the plugin there shows the
// preview from the TeamSpeak server and nobody else contacts the website. Your own chat gets it at once
// (PreviewStore, and Core's cache already holds the files).
//
// Rules:
//  * Only your own messages (TeamSpeak reports them back to you: the sender id and uid are ours), in the
//    channel chat or a private chat (the server chat has no LP), without a TeamSpeak file link (TS Media's
//    media messages), and only while both settings are on (showLinkPreviews, makeLinkPreviews).
//  * At most message::kMaxLinks previewable links per message, Limits::maxJobsPerMinute jobs per connection,
//    one job at a time (the others wait, Limits::maxQueued, at most Limits::queuedTtlMs); 2.5: the allowance and the
//    queue each for the 2.4 classes and for the new ones on their own.
//  * Exactly one fetch per link and send; the same link sent again in the same channel within
//    Limits::reuseMs is announced with the files already on the server (no fetch, no upload).
//  * The LP goes only where the message went: the channel, if you are still in it (its files go up only
//    then too, never into another channel's file browser); the private partner, found again by identity.
//    No plugin commands on the server (PluginLink blocked): nothing is fetched.
//  * Failures are logged privately (the website's host and the reason, never the address). Nobody sees them;
//    2.4 auto-load: a job that was accepted and ends without an LP sends one LPNO (announcement.h) to the same
//    chat, so the people waiting for our preview load the link themselves (never at unload, after a setting
//    went off, after leaving the channel or when the partner left; never for links refused at intake). Each
//    job sends at most one of LP and LPNO: the most commands a sender can send is unchanged.
//  * 2.4 auto-load: the fetch goes into the chat's links folder (linksCacheDir: cache/links, under its cap,
//    removed by Clear cache); the maker copies from there. Our own chat shows our link from that download at
//    once: the pipeline keeps a record of each link of our recent messages (OwnLink) and signals each change.
//  * shutdown() (plugin unload, a setting switched off) cancels the fetch, joins the worker, cancels Core's
//    upload, removes the work files; nothing starts afterwards (applySettings() allows it again).
// 2.5 (page links):
//  * The links of a message are read exactly as every receiver reads them (linkurl.h): message::linkHrefs ->
//    trimLinkEnd -> classify(QString) -> selectLinks (at most 3, the 2.4 classes first). A link to another web page
//    gets no job while Settings::showWebCards is off (no notice either: our cap "lw" is off, nobody waits).
//  * Media pages, site cards and web cards go out as LPX (announcement.h), never LP; a job that ends without one
//    sends LPXNO, with r=n when the site had nothing to show (kNothing, kWalled, kExpired) and r=x when a web page
//    went to another website (its card is never shared: receivers load nothing by themselves). A card is uploaded
//    as a preview-only picture (or nothing, a text card), its text travels in the LPX, shortened to fit the
//    connection's cap (lpxSendCap: 900 bytes until live check L1). A card whose picture can't go up goes without it
//    when it has its text.
//  * The intake notice: links of one message refused at intake (the rate or the queue) are named in ONE LPXNO
//    without r, at most one every kIntakeNoticeMinMs per connection, so 2.5 receivers stop waiting for them.

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "announcement.h"
#include "fetcher.h"
#include "linkurl.h"
#include "mediaprobe.h"
#include "medialink.h"
#include "previewmaker.h"

class Core;
class PluginLink;
class QTimer;
namespace textreact {
class Index;
}
namespace peers {
struct Target;
}

namespace linkpreview {

class PreviewStore;

// What the pipeline needs of a Fetcher: the plugin hands it the real one (lphub.cpp), tests a fake. The
// owner connects the Fetcher's finished(key, result) to SenderPipeline::fetched.
class FetchPort
{
  public:
    virtual ~FetchPort() = default;
    // 2.5: what the load may download (Caps: media, a card's picture, a page's read); 2.4's maxBytes is caps.media.
    virtual void load(const Target& target, const Caps& caps, const QString& cacheDir) = 0;
    virtual void cancel(const QString& key) = 0;
};

class SenderPipeline : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        // Per connection, each link of a message is one job. 2.5: the 2.4 classes (YouTube, files) and the new ones
        // (pages, sites, media sites) each have their own allowance and queue, so page links never use up what 2.4.0
        // receivers (who don't read the intake notice) depend on: a 2.4 link is refused exactly when a 2.4.0
        // sender's would be.
        int    maxJobsPerMinute = 6;
        int    maxQueued        = 8;
        qint64 queuedTtlMs      = 120000; // a job that couldn't start by then is dropped
        int    fetchTimeoutMs   = 90000;  // the Fetcher's own limits are shorter; this is the backstop
        int    jobTimeoutMs     = 300000; // the whole job (the upload included)
        qint64 reuseMs          = 30LL * 60 * 1000;
        int    maxReuse         = 32;
        qint64 thumbnailBytes   = 2LL * 1024 * 1024;  // what the Fetcher may bring for a YouTube thumbnail
        qint64 pictureBytes     = 20LL * 1024 * 1024; // ... for an image or a GIF (a preview is made from it)
        qint64 videoBytes       = 25LL * 1024 * 1024; // ... for a video (a poster is made from it; it goes up whole)
        qint64 intakeNoticeMinMs = 10000; // 2.5: at most one intake notice (LPXNO) per connection this often
        qint64 refusedUploadMs   = 10LL * 60 * 1000; // 2.5: a channel that refused our upload gets none for this long
    };

    struct Counters {
        int started   = 0;
        int announced = 0;
        int failed    = 0;
        int skipped   = 0; // rate, queue, settings, no LP possible there
        int reused    = 0;
        int skipNotices = 0; // 2.4: LPNO sent
        // 2.5
        int lpxSent           = 0;
        int lpxSkips          = 0; // LPXNO sent for a job (with or without r)
        int lpxSkipsNothing   = 0; // ... r=n
        int lpxSkipsCrossSite = 0; // ... r=x
        int intakeNotices     = 0; // LPXNO for links refused at intake
        int lpxCapUsed        = 0; // the cap of the last LPX (900 / 1400)
        int uploadsSkipped    = 0; // previews not uploaded: the channel refused our last upload (Limits::refusedUploadMs)
    };

    // 2.4 auto-load: what became of each link of our own recent messages, for our own chat (ChatLinkPreviews):
    // "Making the preview…" while it is queued or fetched, the website's files the moment the fetch is done.
    struct OwnLink {
        enum class State { Queued, Fetching, Fetched, Failed, Skipped, Reused };
        int     jobId = 0;            // 0: never a job (refused at intake)
        State   state = State::Queued;
        QString hash;                 // the message's identity (textreact.h), as every receiver computes it
        qint64  receivedSec = 0;      // when our own message came back (the time in its local key)
        Target  target;
        Result  result;               // Fetched: the website's files (in linksCacheDir); Failed: error and size
        qint64  changedMs = 0;
    };
    static constexpr int    kMaxOwnLinks  = 32;
    static constexpr qint64 kOwnLinkTtlMs = 15LL * 60 * 1000;

    SenderPipeline(Core* core, PluginLink* link, PreviewStore* store, const textreact::Index* index, FetchPort* fetch, QObject* parent = nullptr);
    SenderPipeline(Core* core, PluginLink* link, PreviewStore* store, const textreact::Index* index, FetchPort* fetch, const Limits& limits,
                   QObject* parent = nullptr);
    ~SenderPipeline() override; // shutdown()

    // TeamSpeak's onTextMessageEvent (GUI thread; any message: ours with previewable links start jobs).
    void onTextMessage(quint64 sch, int targetMode, quint16 toId, quint16 fromId, const QString& fromUid, const QString& bbcode);
    // The Fetcher's finished(key, result): only results of the running job's fetch are taken.
    void fetched(const QString& key, const linkpreview::Result& result);

    void shutdown();      // see above; idempotent
    // A setting off: shutdown(); both on again: new messages start jobs. Show cards for other web pages changed: Web
    // links' jobs stop (off), a share link's running fetch starts again under it.
    void applySettings();

    // The folder the jobs work in (default <data dir>/linkpreview/send): cleared at the first job.
    void setWorkRoot(const QString& dir);
    // The folder the website's files go to (default linksCacheDir(core): the chat's cache/links).
    void setCacheDir(const QString& dir);
    QString cacheDir() const;
    // The video probe (default probeLocalVideo with a poster): tests put a fake in its place.
    void setVideoProbe(std::function<LocalMediaInfo(const QString&)> probe);

    int      jobCount() const { return m_jobs.size(); }
    bool     busy() const { return !m_jobs.isEmpty(); }
    Counters counters() const { return m_counters; }
    // 2.5: what a sender's load of target may download (the sender makes the preview for everyone: data saver
    // doesn't apply): YouTube its thumbnail, files and media pages 20 MB (videos 25 MB), cards a 2 MB picture
    // (and media up to 20 MB, a web page 25 MB, when the page turns out to be media), pages read up to 512 KB.
    Caps     fetchCapsFor(const Target& target) const;
    // 2.5: the links of our own message that get a slot, as every receiver picks them (linkurl.h).
    static QVector<Target> selectedLinks(const QString& bbcode);

    // 2.4 auto-load: the record of urlKey in our message with this identity: the same hash, the nearest
    // receivedSec within textreact::kToleranceSec of timeSec. nullptr: none. Good until the pipeline changes.
    const OwnLink* ownLink(const QString& hash, qint64 timeSec, const QString& urlKey) const;
    const OwnLink* latestOwnLink(const QString& urlKey) const; // the newest record of that link, any message
    bool           fetching(const QString& urlKey) const;      // a job of ours is loading it from the website now
    bool           pending(const QString& urlKey) const;       // ... or will (queued): its download comes
    int            ownLinkCount() const { return m_own.size(); }

  signals:
    // A job ended: announced (the LP was handed to the transport), or not (failed, canceled, dropped).
    void jobFinished(const QString& urlKey, bool announced);
    // 2.4 auto-load: a record of our own links was added or changed state (GUI thread; never while closed).
    void ownLinkChanged(const QString& urlKey);

  public:
    // 2.6 chat proxy (design 6 step 5): the route changed: a running fetch starts again on the new route (an answer of
    // the old one is dropped); queued jobs simply use the new route.
    void followRoute();

  private:
    enum class Step { Queued, Fetching, Making, Uploading, Announcing };
    struct Job {
        int          id = 0;
        Step         step = Step::Queued;
        quint64      sch  = 0;
        proto::Scope scope = proto::Scope::Channel;
        quint64      channelId = 0;  // our channel when the message went (channel chats: where the LP goes)
        quint16      partnerId = 0;  // private chats
        QString      partnerUid;
        QString      serverUid;
        QString      ownUid;
        QString      hash;           // the message's identity (textreact.h)
        qint64       receivedSec = 0; // when our own message came back (the time its key is made of)
        qint64       queuedMs    = 0;
        qint64       startedMs   = 0;
        Target       target;
        QString      dir;            // <work root>/<id>: out/ (the fetch goes to cacheDir())
        int          batch = 0;      // Core's background send
        std::shared_ptr<std::atomic<bool>> cancel;
        maker::Made  made;
        quint64      uploadChannel = 0; // 2.5: the channel its files go up into (ours when the upload starts)
        bool         webCards = true;   // Fetching: the Caps::webCards its fetch holds (followWebCards)
        quint64      routeGeneration = 0; // 2.6: Fetching again after a route change: an older route's result is dropped
    };
    struct Reuse {
        Announcement files; // what is on the server (messageKey, sch and senderUid are filled per use)
        qint64       atMs = 0;
    };

    void   startNext();
    void   startFetch(Job& job);
    void   onMade(int id, const maker::Made& made);
    void   onUploaded(int batch, bool ok, const MediaLink& link, MediaError failure = MediaError::None);
    void   announceTextCard(Job& job); // 2.5: a card's text without its picture
    bool   uploadsRefused(const QString& serverUid, quint64 channelId); // 2.5: Limits::refusedUploadMs
    void   announce(Job& job, Announcement a);
    // 2.5: reason: what LPXNO says for a new-class job that ends without its LPX (r=n / r=x; Unknown: no r).
    void   finish(int id, bool announced, const QString& why = {}, SkipReason reason = SkipReason::Unknown);
    void   sendIntakeNotice(const Job& base, const QStringList& refusedKeys); // 2.5
    void   cancelJob(Job& job);
    void   followWebCards(); // review fix: Show cards for other web pages as it is now (applySettings)
    void   schedule();
    void   onTimer();
    Job*   find(int id);
    Job*   running();
    bool   enabled() const;
    bool   inItsChannel(const Job& job) const; // a channel message's job: you are still in that channel
    // Where the job's LP / LPNO goes now: our channel (if we are still in it), or the partner (found again by
    // identity). False (why says it) when it can't go.
    bool   targetOf(const Job& job, peers::Target* target, QString* why = nullptr) const;
    bool   canNotify(const Job& job) const; // an LPNO for it can go now
    void   addOwn(const OwnLink& record);
    void   setOwn(int jobId, OwnLink::State state, const Result* result = nullptr);
    bool   takeRateToken(quint64 sch, qint64 nowMs, bool legacy);
    int    queuedOf(bool legacy) const; // jobs of the 2.4 classes, or of the new ones
    QString ownLocalKey(const Job& job) const;
    QString reuseKey(const QString& serverUid, quint64 channelId, const QString& urlKey) const;
    maker::Caps makerCapsFor(const QString& serverUid) const;
    qint64 nowMs() const;

    QPointer<Core>          m_core;
    QPointer<PluginLink>    m_link;
    QPointer<PreviewStore>  m_store;
    const textreact::Index* m_index = nullptr;
    FetchPort*              m_fetch = nullptr;
    Limits                  m_limits;
    Counters                m_counters;
    QList<Job>              m_jobs;   // the first one runs (unless Queued)
    QHash<quint64, QVector<qint64>> m_starts;  // connection -> job starts of 2.4 links in the last minute
    QHash<quint64, QVector<qint64>> m_startsX; // 2.5: ... of page, site and media-site links (their own allowance)
    QHash<quint64, qint64>  m_lastIntakeNotice; // 2.5: connection -> when its last intake notice went
    QHash<QString, qint64>  m_refusedUploads;   // 2.5: "<server uid>|<channel id>" -> when it refused our upload
    QHash<QString, Reuse>   m_reuse;
    QThreadPool             m_pool;   // the preview maker (one thread); joined in shutdown()
    QTimer*                 m_timer = nullptr;
    QString                 m_root;
    QString                 m_cacheDir; // empty: linksCacheDir(m_core)
    QList<OwnLink>          m_own;      // newest last; at most kMaxOwnLinks, none older than kOwnLinkTtlMs
    bool                    m_rootCleared = false;
    bool                    m_closed      = false;
    int                     m_nextId      = 0;
    std::function<LocalMediaInfo(const QString&)> m_probe;
};

} // namespace linkpreview
