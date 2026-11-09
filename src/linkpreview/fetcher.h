#pragma once

// 2.4 link previews: fetches one previewable link (linkurl.h) from its website, safely, on a worker
// thread: the sender's preview before upload, or a viewer's "Load preview" click. Never loops or
// prefetches: one load() is one fetch (YouTube: its oEmbed data, then its thumbnail).
//
// Every fetch follows netpolicy.h (https only, public addresses before and after connecting, at most 5
// re-checked redirects, 10 s to connect, 60 s for the whole load, no cookies / credentials / Referer, a
// generic User-Agent) and content.h (Content-Type and magic bytes, size caps while streaming, strict
// oEmbed JSON, thumbnails only from i.ytimg.com, titles cleaned). Pictures are checked against the 80
// megapixel decode cap; videos are probed with Media Foundation (duration, size, poster).
//
// Files go into the caller's cache folder, named by the link's key (never by anything the website said):
//   <key>.<png|jpg|gif|webp|mp4|mov|webm>  the media file (Image, Gif, Video)
//   <key>-thumb.<jpg|webp|png|gif>          YouTube's thumbnail; a poster / smaller picture otherwise
//   <key>.json                              what is known about them, written last
// A later load() of a cached link is answered from the folder without any network; cached() does that
// synchronously. Partial downloads (".part") never outlive their job.
//
// Threads: one Win32 thread per running load (at most Options::maxConcurrent, the rest wait in a short
// queue). A thread pins the plugin DLL until it ends, like the updater's. cancel() and shutdown() set a
// cancel event that wakes every wait at once (WinHTTP runs asynchronously), so a stalled website never
// holds them up; shutdown() then joins the threads (a bounded wait: one that is still in Media Foundation
// or a file write ends on its own, pinned). No signal is emitted for anything after shutdown().
//
// Signals are emitted on the Fetcher's (GUI) thread, never from inside load()/cancel().

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "linkurl.h"
#include "netpolicy.h"

namespace linkpreview {

namespace http {
class Transport;
class Cancel;
} // namespace http

struct Result {
    bool    ok = false;
    QString error;         // errors::k... when !ok (empty when ok)
    QString localFile;     // Image / Gif / Video: the media file; empty for YouTube
    QString mime;          // of localFile; YouTube: of the thumbnail
    qint64  size = -1;     // bytes of localFile (YouTube: of the thumbnail). With error tooLarge: the
                           // size the website announced, -1 when it didn't say
    QString title;         // YouTube: the video's title (cleaned, at most 200 characters)
    QString author;        // YouTube: the channel's name (cleaned, at most 100 characters)
    QString thumbnailFile; // YouTube: the thumbnail; Video: a poster frame; large pictures: a smaller JPEG
    int     width = 0, height = 0; // of the picture / video (YouTube: of the thumbnail), as displayed
    qint64  durationMs = 0;        // Video
    bool    viaProxy = false;      // a request went through the user's Windows proxy (transport.h)
};

// One line of the Fetcher's log, in parts: the plugin tags the address as private (logtext.h).
struct LogLine {
    Kind    kind = Kind::None;
    QString address;  // logUrl(): scheme, host, the last path segment (never a query): who visited what
    QString outcome;  // "loaded, 12 KB in 340 ms" / "timeout after 10000 ms": no names, no addresses
    bool    viaProxy = false;
    QString text() const; // "Link preview (image) https://cdn.example.com/…/cat.png: loaded, ..."
};

// Result::error values.
namespace errors {
inline constexpr char kCanceled[]       = "canceled";       // cancel()
inline constexpr char kDisabled[]       = "disabled";       // load() after shutdown() / setEnabled(false)
inline constexpr char kBusy[]           = "busy";           // too many loads waiting
inline constexpr char kNotPreviewable[] = "notPreviewable"; // not a Target of classify(), or no cache folder
inline constexpr char kBlocked[]        = "blocked";        // the host is (or resolved to) a non-public address
inline constexpr char kRedirect[]       = "redirect";       // more than 5 redirects, or one to a refused address
inline constexpr char kOffline[]        = "offline";        // name not found, no connection
inline constexpr char kTimeout[]        = "timeout";
inline constexpr char kTls[]            = "tls";            // certificate or secure channel problem
inline constexpr char kNoNetwork[]      = "noNetwork";      // winhttp.dll / ws2_32.dll unusable
inline constexpr char kNotFound[]       = "notFound";       // HTTP 404 / 410
inline constexpr char kForbidden[]      = "forbidden";      // HTTP 401 / 403
inline constexpr char kRateLimited[]    = "rateLimited";    // HTTP 429
inline constexpr char kServerError[]    = "serverError";    // HTTP 5xx
inline constexpr char kBadResponse[]    = "badResponse";    // any other status, a cut-off body, bad oEmbed JSON
inline constexpr char kBadType[]        = "badType";        // Content-Type or first bytes are not media of the link's kind
inline constexpr char kTooLarge[]       = "tooLarge";       // over maxBytes (Result::size: the announced size or -1)
inline constexpr char kTooManyPixels[]  = "tooManyPixels";  // a picture over 80 megapixels
inline constexpr char kUnavailable[]    = "unavailable";    // YouTube: no such (public) video
inline constexpr char kDisk[]           = "disk";           // the cache folder can't be written
inline constexpr char kInternal[]       = "internal";
} // namespace errors

namespace detail {
class FetchJob;
}

class Fetcher : public QObject
{
    Q_OBJECT

  public:
    struct Options {
        int connectTimeoutMs = net::kConnectTimeoutMs;
        int receiveTimeoutMs = net::kReceiveTimeoutMs;
        int totalTimeoutMs   = net::kTotalTimeoutMs;
        int maxConcurrent    = 3;
        int maxQueued        = 32;
        int joinTimeoutMs    = 3000; // how long shutdown() waits for running threads
        // Tests: called on the worker right after a picture's or video's file is in place in cacheDir.
        std::function<void(const QString& file)> placed;
    };

    static constexpr qint64 kDefaultMaxBytes = 50LL * 1024 * 1024; // maxBytes <= 0
    static constexpr qint64 kHardMaxBytes    = 200LL * 1024 * 1024; // never more, whatever maxBytes says
    static constexpr qint64 kMaxPixels       = 80LL * 1000 * 1000;

    explicit Fetcher(QObject* parent = nullptr); // the real network (WinHTTP)
    Fetcher(std::shared_ptr<http::Transport> transport, const Options& options, QObject* parent = nullptr); // tests
    ~Fetcher() override; // shutdown()

    // Starts (or queues) one fetch; the outcome comes with finished(target.key, ...). maxBytes caps the
    // media file (YouTube: the thumbnail, at most 2 MB); a larger file ends with errors::kTooLarge before
    // its body is read when the website announces its size. A key that is already loading is not
    // fetched twice (one finished() for both).
    void load(const Target& target, qint64 maxBytes, const QString& cacheDir);
    // Stops that load at once; finished(key, kCanceled) follows. Nothing if the key isn't loading.
    void cancel(const QString& key);
    // Cancels and joins everything (also queued loads, without signals); load() then answers
    // kDisabled until setEnabled(true). Called by the destructor.
    void shutdown();
    void setEnabled(bool enabled); // false: shutdown(); true: loads are accepted again
    bool isEnabled() const { return m_enabled; }
    bool isLoading(const QString& key) const; // running or queued
    int  runningThreads() const { return m_running.size(); }

    // One line per finished load for the plugin's log (no query strings, no paths); set by the owner.
    void setLogger(std::function<void(const QString& line)> logger);
    // The same, in parts (the plugin: linkpreview::usePluginLog in lphub.h).
    void setLogSink(std::function<void(const LogLine& line)> sink) { m_logger = std::move(sink); }

    // A link loaded before, from the cache folder alone (no network; reads one small JSON file and
    // checks the files exist). False if it isn't there complete.
    static bool cached(const Target& target, const QString& cacheDir, Result* out);
    // Deletes the oldest cached links (all their files) until the folder's cached files take at most
    // maxTotalBytes, and leftovers of interrupted fetches. Only names this Fetcher writes are touched.
    static void pruneCache(const QString& cacheDir, qint64 maxTotalBytes);

  signals:
    void progress(const QString& key, qint64 received, qint64 total); // total -1: unknown; about every 100 ms
    void finished(const QString& key, const linkpreview::Result& result);

  private:
    friend class detail::FetchJob;

    struct Pending {
        Target  target;
        qint64  maxBytes = 0;
        QString cacheDir;
    };
    struct Active {
        quint64                       id = 0;
        std::shared_ptr<http::Cancel> cancel;
        Target                        target;
        qint64                        startedMs = 0;
    };
    struct Link;

    void start(const Pending& pending);
    void startQueued();
    void finishLater(const QString& key, const Result& result);
    void onJobProgress(quint64 id, const QString& key, qint64 received, qint64 total);
    void onJobDone(quint64 id, const QString& key, const Result& result);
    void pruneThreads();
    void log(const LogLine& line) const;

    std::shared_ptr<http::Transport>                      m_transport;
    Options                                               m_options;
    std::shared_ptr<Link>                                 m_link;
    bool                                                  m_enabled    = true;
    quint64                                               m_nextId     = 0; // from a random start (part file names)
    quint64                                               m_generation = 0; // shutdown() drops posted finishLater()s
    QHash<QString, Active>                                m_active;  // by key: the load whose result counts
    QHash<quint64, std::shared_ptr<http::Cancel>>         m_running; // every job whose thread hasn't reported back
    QList<Pending>                                        m_queue;
    QVector<void*>                                        m_threads; // thread handles, joined by shutdown()
    std::function<void(const LogLine&)>                   m_logger;
};

} // namespace linkpreview

Q_DECLARE_METATYPE(linkpreview::Result)
