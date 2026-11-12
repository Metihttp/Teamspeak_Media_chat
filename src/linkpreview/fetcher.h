#pragma once

// Link previews: fetches one previewable link (linkurl.h) from its website, safely, on a worker thread: the
// sender's preview before upload, or a viewer's load (automatic or a click). Never loops or prefetches: one load()
// is one plan of at most a few requests:
//  * YouTube (2.4): its oEmbed data, then its thumbnail;
//  * Direct (2.4): the media file (Tenor's short GIF links and Dropbox share links get a fetch rewrite, providers.h);
//  * MediaPage, Site (2.5): the provider's plan (providers.h): a media file derived from the link, or the page /
//    official oEmbed / keyless JSON / feed / manifest first, then the media or the card's picture;
//  * Web (2.5): the page's head (pagemeta.h), then the card's picture.
//
// Every request follows netpolicy.h (https only, public addresses before and after connecting, at most 5
// re-checked redirects, 10 s to connect, 60 s for the whole load, no cookies / credentials / Referer, the honest
// TS Media user agent, no compression) and content.h (Content-Type and magic bytes, size caps while streaming,
// strict JSON, cleaned text). Every hop of a provider plan must stay on the provider's hop lists. Pictures are
// checked against the decode caps (80 megapixels for media and 40 for card pictures on x64, 16.7 everywhere on
// Win32), lossless WebP is refused (CVE-2023-4863); videos are probed with Media Foundation (duration, size, sound,
// poster). Pages are read up to their head (at most Caps::page), kept in memory only, never written to disk or
// logged.
//
// HostGate (hostgate.h): every hop takes a ticket for its host. A load's first request is gated on the GUI thread
// before its thread starts (a load that must wait stays queued, others start; an owned timer wakes the queue);
// later waits inside a load don't count against its deadline and end in errors::kGateBusy after
// Options::maxGateWaitMs. A site that is cooling, walled or broken ends the load at once (no request).
//
// Files go into the caller's cache folder, named by the link's key (never by anything the website said):
//   <key>.<png|jpg|gif|webp|mp4|mov|webm>  the media file (Image, Gif, Video, Loop)
//   <key>-thumb.<jpg|webp|png|gif>          YouTube's thumbnail; a card's picture; a poster / smaller picture otherwise
//   <key>.json                              what is known about them, written last (v1 for 2.4 classes, v2 for the
//                                           new classes: also cards and "nothing to show" for a while)
// A later load() of a cached link is answered from the folder without any network; cached() does that
// synchronously. Partial downloads (".part") never outlive their job.
//
// Threads: one Win32 thread per running load (at most Options::maxConcurrent, the rest wait in a short queue). A
// thread pins the plugin DLL until it ends. cancel() and shutdown() set a cancel event that wakes every wait at
// once (WinHTTP runs asynchronously), so a stalled website never holds them up; shutdown() then joins the threads
// (a bounded wait: one that is still in Media Foundation or a file write ends on its own, pinned). No signal is
// emitted for anything after shutdown(). No Qt metatype is registered.
//
// Signals are emitted on the Fetcher's (GUI) thread, never from inside load()/cancel().

#include <QHash>
#include <QImage>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "carddata.h"
#include "hostgate.h"
#include "linkurl.h"
#include "netpolicy.h"

class QTimer;

namespace linkpreview {

namespace http {
class Transport;
class Cancel;
} // namespace http

struct Result {
    bool    ok = false;
    QString error;         // errors::k... when !ok (empty when ok)
    QString localFile;     // Image / Gif / Video / Loop: the media file; empty for YouTube and cards
    QString mime;          // of localFile; YouTube: of the thumbnail; a card: of its picture
    qint64  size = -1;     // bytes of localFile (YouTube: of the thumbnail). With error tooLarge: the
                           // size the website announced, -1 when it didn't say
    QString title;         // YouTube: the video's title (cleaned, at most 200 characters)
    QString author;        // YouTube: the channel's name (cleaned, at most 100 characters)
    QString thumbnailFile; // YouTube: the thumbnail; Video / Loop: a poster frame; large pictures: a smaller JPEG
    int     width = 0, height = 0; // of the picture / video (YouTube: of the thumbnail), as displayed
    qint64  durationMs = 0;        // Video / Loop
    bool    viaProxy = false;      // a request went through the user's Windows proxy (transport.h)
    // 2.5
    Kind     kind = Kind::None;    // what it turned out to be (legacy loads too: by bytes); with kTooLarge also when a
                                   // page answered with media over the media cap (Web class)
    CardData card;                 // kind == Card: the card's text and picture (card.pictureFile in the cache folder)
    QString  finalHost;            // Web: the last page hop's host when its registrable domain differs from the link's
    qint64   retryAfterMs = 0;     // kRateLimited / kGateBusy: how long until the site may be asked again
    bool     softCooling  = false; // kRateLimited from an x-ratelimit soft cooldown, not from a 429/503
    int      requests     = 0;     // HTTP requests made (redirect hops not counted)
    bool     hasAudio     = false; // Video / Loop
    QString  hopRefused;           // the host a provider's hop list refused (the log and the smoke tool say it)
};

// One line of the Fetcher's log, in parts: the plugin tags the address as private (logtext.h).
struct LogLine {
    Kind      kind = Kind::None;
    QString   address;  // logUrl(): scheme, host, the last path segment (never a query): who visited what
    QString   outcome;  // "loaded, 12 KB in 340 ms" / "card, 2 requests, 412 ms": no names, no titles, no addresses
    bool      viaProxy = false;
    LinkClass cls      = LinkClass::None; // 2.5
    int       requests = 0;               // 2.5
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
inline constexpr char kRateLimited[]    = "rateLimited";    // HTTP 429 (or 503 with Retry-After), or the site is cooling
inline constexpr char kServerError[]    = "serverError";    // HTTP 5xx
inline constexpr char kBadResponse[]    = "badResponse";    // any other status, a cut-off body, bad oEmbed JSON
inline constexpr char kBadType[]        = "badType";        // Content-Type or first bytes are not media of the link's kind
inline constexpr char kTooLarge[]       = "tooLarge";       // over the cap (Result::size: the announced size or -1)
inline constexpr char kTooManyPixels[]  = "tooManyPixels";  // a picture over the decode cap
inline constexpr char kUnavailable[]    = "unavailable";    // YouTube: no such (public) video
inline constexpr char kDisk[]           = "disk";           // the cache folder can't be written
inline constexpr char kInternal[]       = "internal";
// 2.5
inline constexpr char kWalled[]      = "walled";      // a bot challenge, consent, login or age page: no preview
inline constexpr char kNothing[]     = "nothing";     // the page has nothing to show (no card by the card rules)
inline constexpr char kExpired[]     = "expired";     // a signed link whose time is over (Discord CDN), no request made
inline constexpr char kUnsupported[] = "unsupported"; // a picture the plugin refuses to decode (lossless WebP)
inline constexpr char kGateBusy[]    = "gatebusy";    // a later request of the load waited for its site longer than
                                                      // Options::maxGateWaitMs: try later; never a failure
inline constexpr char kWebOff[]      = "weboff";      // a share link that turned out to be another web page (a redirect to
                                                      // another host, an unconfirmed page) while Caps::webCards is off: no
                                                      // request for it; never remembered (the switch may come on again)
} // namespace errors

// What one load may download. media: the file of a Direct / MediaPage / media-file load, and a Web page that
// answers with media; 0 = no media at all (the load stops before the body with kTooLarge and the size from
// Content-Length, -1 when unknown). picture: a card's picture (at most net::kMaxThumbnailBytes). page: how much of
// a page is read (at most net::kMaxPageBytes). userAsked: a click (Load preview, Retry, Load link previews): it
// passes an open breaker (one try) and a cached "nothing", never a cooldown or a wall. video (2.5): the media cap of
// a link that may turn out to be a video whatever its expected kind (a Nextcloud share: videoAutoDownloadMB as for
// other link videos); -1: media. webCards (2.5): Settings::showWebCards of whoever loads it. Off, a share link
// (design2 2.3/2.7) that turns out to be another web page (a redirect to another host, a page that isn't proven a
// share) ends there with kWebOff: neither that page nor a picture it names is requested.
struct Caps {
    qint64 media     = 0;
    qint64 picture   = 2 * 1024 * 1024;
    qint64 page      = 512 * 1024;
    bool   userAsked = false;
    qint64 video     = -1;
    bool   webCards  = true;
};

// A video's facts as the Media Foundation probe gives them (tests inject their own).
struct VideoProbe {
    bool   ok         = false;
    qint64 durationMs = 0;
    int    width = 0, height = 0;
    bool   hasAudio   = false;
    QImage poster;
};

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
        // 2.5
        // The plugin passes linkpreview::sharedGate(); null (tests, tools): a private gate made in the constructor.
        std::shared_ptr<HostGate> gate;
        // Tests: the video probe (default: Media Foundation through probeLocalMedia).
        std::function<bool(const QString& file, VideoProbe* out)> probe;
        // Tests: the PC's DNS suffixes (default: net::localDnsSuffixes()); the wall clock in seconds (Discord expiry).
        std::function<QStringList()> localSuffixes;
        std::function<qint64()>      nowSec;
        int    maxGateWaitMs       = 20000; // later hops of a load wait at most this long in all (kGateBusy)
        int    pageTimeoutMs       = net::kPageTimeoutMs;
        qint64 pageMinBytes        = net::kPageMinBytes;
        int    pageMinBytesAfterMs = net::kPageMinBytesAfterMs;
        int    twitchRetryMs       = 3000; // a Twitch clip page that answered the site-wide shell is asked again after this
        qint64 maxMediaPixels      = sizeof(void*) >= 8 ? 80LL * 1000 * 1000 : 4096LL * 4096; // media stills and posters
        qint64 maxPicturePixels    = sizeof(void*) >= 8 ? 40LL * 1000 * 1000 : 4096LL * 4096; // card pictures
    };

    static constexpr qint64 kDefaultMaxBytes = 50LL * 1024 * 1024; // maxBytes <= 0
    static constexpr qint64 kHardMaxBytes    = 200LL * 1024 * 1024; // never more, whatever maxBytes says
    static constexpr qint64 kMaxPixels       = 80LL * 1000 * 1000;  // 2.4.0's media cap (x64)

    explicit Fetcher(QObject* parent = nullptr); // the real network (WinHTTP), a private gate
    Fetcher(std::shared_ptr<http::Transport> transport, const Options& options, QObject* parent = nullptr);
    ~Fetcher() override; // shutdown()

    // Starts (or queues) one fetch; the outcome comes with finished(target.key, ...). caps: what it may download
    // (Caps). A key that is already loading is not fetched twice (one finished() for both).
    void load(const Target& target, const Caps& caps, const QString& cacheDir);
    // 2.4.0's form: Caps{maxBytes (<= 0: the default), min(maxBytes, 2 MB)}.
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
    int  queuedLoads() const { return m_queue.size(); }
    std::shared_ptr<HostGate> gate() const { return m_options.gate; }

    // One line per finished load for the plugin's log (no query strings, no paths); set by the owner.
    void setLogger(std::function<void(const QString& line)> logger);
    // The same, in parts (the plugin: linkpreview::usePluginLog in lphub.h).
    void setLogSink(std::function<void(const LogLine& line)> sink) { m_logger = std::move(sink); }

    // A link loaded before, from the cache folder alone (no network; reads one small JSON file and
    // checks the files exist). False if it isn't there complete. 2.5: for a new-class link also a card, and a
    // fresh "nothing to show" record (out->ok false with its error; then the result is still true).
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
        Caps    caps;
        QString cacheDir;
    };
    struct Active {
        quint64                       id = 0;
        std::shared_ptr<http::Cancel> cancel;
        Target                        target;
        qint64                        startedMs = 0;
    };
    struct Link;

    void start(const Pending& pending, const HostGate::Ticket& ticket);
    void startQueued();
    // Asks the gate about a load's first request. Go: true and the ticket. Wait: false with *wakeMs. Cooling, walled,
    // broken: false with *wakeMs = -1 and the load finished (kRateLimited / kWalled). A load the cache answers is
    // admitted at once without a ticket (it makes no request).
    bool admit(const Pending& pending, HostGate::Ticket* ticket, qint64* wakeMs);
    bool answersFromCache(const Pending& pending) const;
    void finishLater(const QString& key, const Result& result);
    void onJobProgress(quint64 id, const QString& key, qint64 received, qint64 total);
    void onJobDone(quint64 id, const QString& key, const Result& result);
    void pruneThreads();
    void log(const LogLine& line) const;

    std::shared_ptr<http::Transport>              m_transport;
    Options                                       m_options;
    std::shared_ptr<Link>                         m_link;
    bool                                          m_enabled    = true;
    quint64                                       m_nextId     = 0; // from a random start (part file names)
    quint64                                       m_generation = 0; // shutdown() drops posted finishLater()s
    QHash<QString, Active>                        m_active;  // by key: the load whose result counts
    QHash<quint64, std::shared_ptr<http::Cancel>> m_running; // every job whose thread hasn't reported back
    QList<Pending>                                m_queue;
    QVector<void*>                                m_threads; // thread handles, joined by shutdown()
    std::function<void(const LogLine&)>           m_logger;
    QTimer*                                       m_gateTimer = nullptr; // owned (a child): wakes the queue for the gate
};

} // namespace linkpreview

Q_DECLARE_METATYPE(linkpreview::Result)
