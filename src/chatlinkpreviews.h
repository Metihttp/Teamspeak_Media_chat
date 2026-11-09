#pragma once

// 2.4 link previews in the chat. Owned by ChatIntegration, which calls it at the places marked "2.4 link
// previews" in chatintegration.cpp: after every scan of a chat document (after the text reactions, before the
// layout), in its event filter (after the polls), from refreshAll (settings changed), and for the context menu
// of a link's media. It:
//  * finds the previewable web links of every chat (linkdoc.h) and keeps one slot under each: a card of ours
//    (linkcard.h) or the link's media itself as a ChatIntegration preview (pictures, GIFs, videos, played and
//    opened like uploaded media), as linkstate::decide() says;
//  * sender-made previews (linkpreview::PreviewStore, sender-previews): the announced files are ts3file links
//    Core downloads from the TeamSpeak server and checks against their SHA-256 (no website is contacted);
//  * click and automatic loads: "Load preview" asks linkpreview::Fetcher for the link (one fetch per click; the
//    card says the site sees your IP address); a picture, GIF or video becomes a Core entry (Core::adoptLocalFile),
//    a video's poster is made with mediaprobe on a worker of ours; YouTube shows its title, channel and thumbnail
//    and a click opens it in the default browser;
//  * 2.4 "Load link previews automatically" (linkpreview/autoload.h has the rules): our own messages show
//    "Making the preview…" and then our sender pipeline's download at once (no request of ours); other people's
//    links wait for the sender's preview while the sender makes them (capability "l", LPNO ends the wait), and
//    otherwise load by themselves when they are on screen, within the automatic download limits, at most
//    autoload::kMaxRunning at once (one per sender) and a per-sender budget; a link that has to wait its turn is a
//    click card until then; links already in cache/links show from there with no request; a link whose sender's
//    preview Clear cache took never loads by itself;
//  * Hide preview per card (until TeamSpeak restarts), "Open in browser", "Copy link";
//  * gives every document back exactly when the setting goes off, the inline previews go off, or it goes
//    (every fetch canceled and its worker joined first).
// Everything read from a chat or a website is untrusted: only checked https addresses are ever fetched, on a click
// or automatically for links on screen without a sender-made preview (or, for the sender, by sender-previews).

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVector>

#include <functional>

#include "chattextreactions.h" // KeyDays
#include "core.h"
#include "linkpreview/autoload.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/linkstate.h"

class ChatIntegration;
class QEvent;
class QJsonObject;
class QMenu;
class QTextBrowser;
class QTextDocument;
class QTimer;

namespace linkpreview {
class Fetcher;
class Hub;
class PreviewStore;
class SenderPipeline;
struct Result;
struct Announcement;
} // namespace linkpreview
namespace peers {
class PeerDirectory;
}

class ChatLinkPreviews : public QObject
{
    Q_OBJECT

  public:
    ChatLinkPreviews(ChatIntegration* chat, Core* core);
    ~ChatLinkPreviews() override; // fetches canceled and joined, every slot out of every chat

    // Integration: sender-previews' store (linkpreview::Hub::store(), plugin.cpp); nullptr: no sender's previews.
    void                     setStore(linkpreview::PreviewStore* store);
    // 2.4 auto-load (plugin.cpp): the hub (its store, its LPNO notices, our sender pipeline's own-link records) and
    // the peer directory (who makes link previews). Both outlive us; prepareShutdown() lets go of them.
    void                     setHub(linkpreview::Hub* hub, peers::PeerDirectory* peers);
    // Plugin unload, before Core and the chat go: every website fetch canceled and joined, the poster worker
    // joined; nothing loads afterwards (the cards stay until the destructor).
    void                     prepareShutdown();
    linkpreview::PreviewStore* store() const { return m_store; }
    linkpreview::Fetcher*      fetcher() const { return m_fetcher; }
    // Inline previews and "Show previews for links to other websites" are both on.
    static bool enabled();

    void afterScan(QTextBrowser* browser);
    bool filterEvent(QObject* watched, QEvent* event); // true: the event was for a card
    void settingsChanged();                            // ChatIntegration::refreshAll, before its own rescan
    // 2.4 auto-load: the automatic limits or a server's data saver changed (also from the Plugins menu or a command,
    // plugin.cpp): a running automatic load that may not go on stops.
    void limitsChanged();

    // ChatIntegration's hooks for a link's media (a "tsmedia:<key>" preview in a slot of ours).
    bool ownsKey(const QString& key) const; // no media reactions on it (the message has text reactions)
    // The slot's own items for the context menu of key at viewportPos: Open in browser, Hide preview; webUrl:
    // the link (ChatIntegration's "Copy link" copies it, not the file's address). False when key isn't one of
    // ours there.
    bool addMediaMenu(QMenu* menu, QTextBrowser* browser, const QString& key, const QPoint& viewportPos, QUrl* webUrl);

    // ---- tests, tools and the live-test driver --------------------------------------------------------
    int     slotCount(QTextBrowser* browser) const;
    QString describe(QTextBrowser* browser, const QString& find = QString()) const; // one line per slot (find: of the messages with that text)
    // Replaces Fetcher::load (tests: no network, results come through onFetched / onProgress).
    void setLoader(std::function<void(const linkpreview::Target&, qint64 maxBytes, const QString& cacheDir)> loader);
    void onFetched(const QString& key, const linkpreview::Result& result);
    void onProgress(const QString& key, qint64 received, qint64 total);
    // 2.4 auto-load: how many automatic loads run now, and of which links (tests, the live-test driver).
    int  autoRunning() const { return m_autoRuns.size(); }
    // The largest file a click loads for kind (pictures and GIFs 25 MB, videos 50 MB, YouTube's thumbnail 2 MB).
    static qint64 maxBytesFor(linkpreview::Kind kind);
#ifdef TSMEDIA_TESTHOOKS
    // The live-test driver's "links.*" commands (selftest.cpp): links.log (the visible chat's slots), links.click
    // {"index": n (card in document order, 0-based), "zone": "action|hide|body"} through the chat's real mouse path,
    // links.auto (2.4: the automatic loads, the queue, the waits).
    QString testCommand(const QString& cmd, const QJsonObject& c);
#endif

  private:
    // One slot as the pointer and the drawing need it (read from the document again after every change).
    struct SlotRef {
        linkdoc::Slot       slot;
        int                 block = -1;
        linkpreview::Target target;
        QString             slotId;     // messageKey|urlKey (or the block's text hash|urlKey)
        QString             messageKey; // textreact local key; empty: none (no sender's preview then)
        // 2.4 auto-load
        QTextBrowser*       browser   = nullptr; // the chat it is in
        QString             senderUid;           // the message header's uid (textreactdoc::identify; empty: unknown)
        qint64              timeSec   = -1;      // from messageKey; -1: the message has no key
        int                 budgetSec = 0;       // its wait from timeSec (autoload::SenderQueue over the chat's links so far)
    };
    // 2.4 auto-load: the chat a slot is in, as far as the waits need it.
    struct ChatContext {
        quint64                         sch  = 0; // its connection (0: not shown or not that server: no wait, never "own")
        linkpreview::autoload::Chat     chat = linkpreview::autoload::Chat::Unknown;
        QString                         serverUid; // for the server's own data saver
    };
    // 2.4 auto-load: an automatic load that runs (by urlKey).
    struct AutoRun {
        QString sender;        // senderKey(): one running load per sender (autoload::kMaxPerSender)
        QString serverUid;     // the chat's server (its own data saver)
        qint64  maxBytes = 0;  // the automatic limit it started with
        qint64  received = 0;  // so far (onProgress)
        qint64  total    = -1; // what the website announced (-1: not yet, or it doesn't say)
    };
    struct AutoWant {
        QPointer<QTextBrowser> browser;
        QString                object; // the slot's picture name (read again: the document may have changed)
    };
    enum class Origin { Click, Auto };
    struct View {
        QPointer<QTextBrowser>  browser;
        QPointer<QTextDocument> document;
        QHash<QString, QString> rendered;  // card picture name -> what it shows (drawn ones only)
        QVector<SlotRef>        refs;      // the slots as the document has them (refsValid)
        bool                    refsValid = false;
        QVector<QRect>          spinRects; // cards with a turning ring on screen (viewport coordinates)
        bool                    tracked = false;
    };
    struct Spot {
        QPointer<QTextBrowser> browser;
        QString                object; // card picture name
        linkcard::Zone         zone = linkcard::Zone::None;
        bool same(const Spot& o) const { return browser == o.browser && object == o.object && zone == o.zone; }
    };
    struct ServerFiles {
        MediaLink                 main;
        bool                      hasMedia = false;
        linkpreview::Announcement announcement; // with its file browser filled in
    };

    View*   viewOf(QTextBrowser* browser);
    void    track(QTextBrowser* browser, View& view);
    void    reconcile(QTextBrowser* browser);
    void    restore(View& view); // every slot of view's document out
    void    restoreAll();
    // days: the chat's days for a whole pass over an unchanged document (ChatTextReactions::KeyDays), or none.
    QString messageKeyOf(QTextBrowser* browser, int block, ChatTextReactions::KeyDays* days = nullptr, QString* sender = nullptr) const;
    QString slotIdOf(QTextBrowser* browser, int block, const QString& urlKey, ChatTextReactions::KeyDays* days = nullptr,
                     QString* senderUid = nullptr) const;
    // 2.4 auto-load: a slot's message fields (time, its wait) from its key and the sender's links before it (queue: one
    // per pass over a chat, fed in the chat's order).
    static void fillMessage(SlotRef* ref, linkpreview::autoload::SenderQueue* queue);
    QStringList excludedHrefs() const;
    QString     linksDir() const;      // <cache>/links: the files of links loaded here
    static qint64 linkCacheBytes();    // its cap
    void        pruneLinkCache();      // Fetcher::pruneCache, then forgetMissingFiles()
    void        forgetMissingFiles();  // links loaded here whose files left the cache: a click loads them again

    linkstate::LocalState  localOf(const QString& urlKey);
    linkstate::ServerState serverOf(const QString& messageKey, const linkpreview::Target& target, bool ensure);
    bool                   serverFilesOf(const linkpreview::Announcement& a, ServerFiles* files) const;
    // What the slot reads of the link's shared local state (2.4 auto-load): our own message while our pipeline has the
    // link reads Idle ("Making the preview…", not an earlier load's error); a file known to be larger than a click may
    // load reads TooLarge (no Load preview that can only fail).
    linkstate::LocalState  slotLocal(const SlotRef& ref, linkstate::LocalState local);
    linkstate::Decision    decisionFor(const SlotRef& ref, bool ensure);
    linkcard::Card         cardFor(const SlotRef& ref, const linkstate::Decision& d, const PreviewStyle& style);
    // ---- 2.4 auto-load (no side effects: they run in paints, hit tests, tool tips and menus) ----
    ChatContext                     contextOf(QTextBrowser* browser) const;
    bool                            isOwn(const SlotRef& ref, const ChatContext& ctx) const;
    linkpreview::autoload::Presence presentOf(quint64 sch, const QString& uid); // the same answer for every chat kind
    linkpreview::autoload::Input    inputFor(const SlotRef& ref);
    linkpreview::autoload::Wait     waitOf(const SlotRef& ref);
    // An automatic load may start for this slot now: by the rules, and in its turn (freeTurn, a budget token); with
    // ignoreBudget by the rules only (it starts in its turn). Never checks the hold (runAutoLoads' own).
    bool                            autoEligible(const SlotRef& ref, const linkstate::LocalState& local, const linkstate::ServerState& server,
                                                 bool ignoreBudget = false);
    // runAutoLoads may start it: due now (Wait::Auto), or held back (a click card until its turn).
    bool                            autoCandidate(const SlotRef& ref, const linkstate::Decision& d);
    // A load of sender's links may start: fewer than kMaxRunning run, and none of theirs (kMaxPerSender).
    bool                            freeTurn(const QString& sender) const;
    // A private partner's capability is asked (again) when their message may still get their preview: true while
    // the answer is due (the slot waits; nothing starts).
    bool                            askPartner(const SlotRef& ref, const ChatContext& ctx);
    qint64                          shownSize(const SlotRef& ref, const linkstate::LocalState& local) const; // the Click card's size
    static QString                  senderKey(const ChatContext& ctx, const QString& senderUid);
    void                            runAutoLoads();
    void                            applyOwnRecord(const QString& urlKey); // our pipeline's newest record of urlKey
    void                            onOwnLinkChanged(const QString& urlKey);
    void                            onSkipped(const QString& urlKey);
    void                            onPresenceChanged(quint64 sch);
    void                            tryCache(const linkpreview::Target& target);
    void                            updateVisibleViewports();

    PreviewStyle styleOf(QTextBrowser* browser) const;
    QSize        cardSize(const linkcard::Card& card, const PreviewStyle& style) const; // with the gaps
    QString      signatureOf(const linkcard::Card& card, const PreviewStyle& style, const linkcard::Pointer& pointer) const;
    linkcard::Pointer pointerFor(QTextBrowser* browser, const QString& object) const;
    QString      newCardName();
    QVector<SlotRef> slotsOf(QTextBrowser* browser) const;
    bool         refOf(QTextBrowser* browser, const QString& object, SlotRef* ref) const;
    QRectF       objectRect(QTextBrowser* browser, int position, const QSizeF& size) const; // the card (no gaps), viewport coordinates
    void         renderVisible(QTextBrowser* browser);
    void         renderCard(QTextBrowser* browser, View& view, const SlotRef& ref);
    void         repaintCard(QTextBrowser* browser, const QString& object);
    void         scheduleRefresh();
    Spot         spotAt(QTextBrowser* browser, const QPoint& pos, QRectF* rect = nullptr, SlotRef* refOut = nullptr) const;
    void         setHover(const Spot& spot);
    void         applyCursor(QTextBrowser* browser, const Spot& spot);
    void         activate(QTextBrowser* browser, const SlotRef& ref, linkcard::Zone zone);
    void         showCardMenu(QTextBrowser* browser, const SlotRef& ref, const QPoint& globalPos);
    QImage       thumbnailFor(const SlotRef& ref, const linkstate::Decision& d, const QSize& pixels);

    // serverUid: the chat's server (an automatic load's limit is that server's data saver); sender: senderKey().
    void startFetch(const linkpreview::Target& target, Origin origin = Origin::Click, const QString& serverUid = QString(),
                    const QString& sender = QString());
    void cancelFetch(const QString& urlKey);
    void applyFetched(const QString& key, const linkpreview::Result& result, bool fromCache);
    void setHidden(const QString& slotId, const QString& urlKey, bool hidden);
    void openInBrowser(const QUrl& url) const;
    void adoptVideo(const linkpreview::Target& target, const linkpreview::Result& result);
    void onEntryChanged(const QString& key);
    void onAnnounced(const linkpreview::Announcement& a);
    void stopUnneededFetches(); // a load no slot shows any more (the sender's preview came first)

    ChatIntegration*                     m_chat;
    Core*                                m_core;
    QPointer<linkpreview::PreviewStore>  m_store;
    linkpreview::Fetcher*                m_fetcher = nullptr;
    std::function<void(const linkpreview::Target&, qint64, const QString&)> m_loader;
    QHash<QTextBrowser*, View>           m_views;
    QHash<QString, linkstate::LocalState> m_local;    // urlKey -> this viewer's fetch
    QHash<QString, linkpreview::Target>  m_targets;   // urlKey -> the link (for fetches and menus)
    QSet<QString>                        m_hidden;    // slot ids (this session)
    QSet<QString>                        m_upgraded;  // urlKeys a sender announced since the last refresh
    QSet<QString>                        m_ensured;   // Core keys of announced files already handed to Core
    QSet<QString>                        m_serverKeys; // Core keys of announced files (ownsKey)
    QSet<QString>                        m_watched;   // Core keys our slots show or draw from
    QHash<QString, int>                  m_lastState; // ... their MediaState when last seen (-1: no entry)
    QHash<QString, QImage>               m_thumbs;    // decoded thumbnails (file path + size), a few
    QStringList                          m_thumbOrder;
    QString                              m_tag;
    quint32                              m_serial = 0;
    Spot                                 m_hover;
    Spot                                 m_press;
    bool                                 m_cursorSet   = false;
    Qt::CursorShape                      m_savedCursor = Qt::ArrowCursor;
    QTimer*                              m_refreshTimer = nullptr; // coalesces reconciles after changes
    QTimer*                              m_spinTimer    = nullptr; // repaints rings while a card loads
    QThreadPool                          m_pool;                   // video posters (mediaprobe); joined on destruction
    bool                                 m_down    = false;
    bool                                 m_stopped = false; // prepareShutdown(): no fetch starts

    // 2.4 auto-load (all GUI thread)
    QPointer<linkpreview::Hub>             m_hub;
    QPointer<linkpreview::SenderPipeline>  m_pipeline;
    QPointer<peers::PeerDirectory>         m_peers;
    QTimer*                                m_autoTimer = nullptr; // child; single-shot 100 ms -> runAutoLoads (never in a paint)
    QTimer*                                m_waitTimer = nullptr; // child; 1000 ms while a visible slot waits
    QVector<AutoWant>                      m_autoWanted;          // the last on-screen pass's candidates (per chat)
    QHash<QString, AutoRun>                m_autoRuns;            // urlKeys whose current load is automatic
    QSet<QString>                          m_autoHeld;            // slot ids runAutoLoads held back (drawn as click cards)
    QSet<QString>                          m_pipelineWaits;       // urlKeys waiting on our pipeline's fetch (no request of ours)
    QSet<QString>                          m_cacheMiss;           // urlKeys not in cache/links (looked once)
    QHash<QString, qint64>                 m_sizeHints;           // urlKey -> bytes (our pipeline found it too large)
    linkpreview::autoload::PresenceMemo    m_present;             // the senders, for a second
    QHash<int, int>                        m_ownApplied;          // pipeline job id -> the record state applied
    bool                                   m_waitSeen = false;    // a visible slot waited since the last wait tick
    linkpreview::autoload::SenderBudget    m_budget;
};
