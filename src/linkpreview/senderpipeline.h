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
//    one job at a time (the others wait, Limits::maxQueued, at most Limits::queuedTtlMs).
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
    virtual void load(const Target& target, qint64 maxBytes, const QString& cacheDir) = 0;
    virtual void cancel(const QString& key) = 0;
};

class SenderPipeline : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        int    maxJobsPerMinute = 6;      // per connection (each link of a message is one job)
        int    maxQueued        = 8;
        qint64 queuedTtlMs      = 120000; // a job that couldn't start by then is dropped
        int    fetchTimeoutMs   = 90000;  // the Fetcher's own limits are shorter; this is the backstop
        int    jobTimeoutMs     = 300000; // the whole job (the upload included)
        qint64 reuseMs          = 30LL * 60 * 1000;
        int    maxReuse         = 32;
        qint64 thumbnailBytes   = 2LL * 1024 * 1024;  // what the Fetcher may bring for a YouTube thumbnail
        qint64 pictureBytes     = 20LL * 1024 * 1024; // ... for an image or a GIF (a preview is made from it)
        qint64 videoBytes       = 25LL * 1024 * 1024; // ... for a video (a poster is made from it; it goes up whole)
    };

    struct Counters {
        int started   = 0;
        int announced = 0;
        int failed    = 0;
        int skipped   = 0; // rate, queue, settings, no LP possible there
        int reused    = 0;
        int skipNotices = 0; // 2.4: LPNO sent
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
    void applySettings(); // a setting off: shutdown(); both on again: new messages start jobs

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
    };
    struct Reuse {
        Announcement files; // what is on the server (messageKey, sch and senderUid are filled per use)
        qint64       atMs = 0;
    };

    void   startNext();
    void   startFetch(Job& job);
    void   onMade(int id, const maker::Made& made);
    void   onUploaded(int batch, bool ok, const MediaLink& link);
    void   announce(Job& job, Announcement a);
    void   finish(int id, bool announced, const QString& why = {});
    void   cancelJob(Job& job);
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
    bool   takeRateToken(quint64 sch, qint64 nowMs);
    QString ownLocalKey(const Job& job) const;
    QString reuseKey(const QString& serverUid, quint64 channelId, const QString& urlKey) const;
    maker::Caps capsFor(const QString& serverUid) const;
    qint64 fetchBytesFor(Kind kind) const;
    qint64 nowMs() const;

    QPointer<Core>          m_core;
    QPointer<PluginLink>    m_link;
    QPointer<PreviewStore>  m_store;
    const textreact::Index* m_index = nullptr;
    FetchPort*              m_fetch = nullptr;
    Limits                  m_limits;
    Counters                m_counters;
    QList<Job>              m_jobs;   // the first one runs (unless Queued)
    QHash<quint64, QVector<qint64>> m_starts; // connection -> job starts in the last minute
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
