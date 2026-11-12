#pragma once

// 2.4 link previews: the plugin-to-plugin side, owned by PeerHub (created, started and shut down with it,
// like TextReactHub) and built on its pieces: the transport (PluginLink), the index of the messages in the
// chats (TextReactHub's textreact::Index) and Core.
//
//  * Sending: the SenderPipeline (senderpipeline.h) gets our own chat messages (plugin.cpp's
//    onTextMessageEvent) and a Fetcher of its own.
//  * Receiving LP (announcement.h): only while Settings::showLinkPreviews is on; never from ServerQuery
//    clients; at most Limits::perSenderBurst per person and connection per minute (besides PluginLink's
//    own limit); s=c only from someone in our channel right now, for files in our channel; and only for a
//    message of the invoker's own (the uid the server filled in) found in that chat (textreact::Index,
//    senderOnly; a private chat for s=p, the channel's for s=c). An LP for a message not in a chat yet waits
//    Limits::waitTtlMs. Accepted ones go into the PreviewStore (link-previews.json, written 5 s after a
//    change and at shutdown; cleared with Clear cache), and their files are asked of Core at once
//    (requestFiles: the preview from the TeamSpeak server, checked against its SHA-256; the media file by
//    the automatic download rules). The store's announced() tells the chat.
//  * Receiving LPNO (2.4 auto-load, announcement.h): "the sender's plugin won't announce a preview of that link
//    of that message". The same checks as LP, in the same order, with LP's per-sender limit; matched to its
//    message the same way (waiting for it too). Kept in memory only (at most kMaxSkips, kSkipTtlMs) unless the
//    link's LP is stored already; skipped() tells the chat, which stops waiting and loads it itself.
//  * 2.5: LPX and LPXNO (the new link classes) with the same gates in the same order, with a per-sender limit of
//    their own (Limits::xPerSenderBurst; LP's stays as it was). An accepted LPX is stored like an LP, but its files
//    are NOT asked of Core here: the chat asks for a card's picture when its slot is on screen, paced (6.10).
//    LPXNO names 1 to 3 links with an optional reason (skipReason(): r=n nothing to show, r=x another website), kept
//    like LPNO.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <memory>

#include "announcement.h"
#include "peerprotocol.h"

class Core;
class PluginLink;
class QTimer;
namespace textreact {
class Index;
}

namespace linkpreview {

class FetchPort;
class Fetcher;
class PreviewStore;
class SenderPipeline;

class Hub : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        int    perSenderBurst    = 10;    // LP per person and connection ...
        qint64 perSenderWindowMs = 60000; // ... per this long
        int    maxRateEntries    = 256;
        qint64 waitTtlMs         = 15000; // an LP for a message that isn't in a chat (yet) waits this long
        int    maxWaiting        = 32;
        int    saveDelayMs       = 5000;
        // How far the message's time may be from the LP's age. Its age is written when the sender makes it, and
        // the transport may hold a background command behind presence and reactions or through a flood pause:
        // it arrives that much "younger". The message is the sender's own with that very text (its hash), and
        // the nearest one is taken, so a wider window than text reactions' kToleranceSec costs nothing.
        qint64 matchToleranceSec = 60;
        // 2.5: LPX and LPXNO per person and connection per perSenderWindowMs (a bucket of their own).
        int xPerSenderBurst = 20;
    };

    struct Counters {
        int received    = 0; // LP messages
        int accepted    = 0;
        int rejected    = 0; // malformed, ServerQuery, not from our channel, files elsewhere
        int rateLimited = 0;
        int unmatched   = 0; // no message of the invoker's for it (gave up waiting)
        int skipsReceived = 0; // 2.4 auto-load: LPNO messages
        int skipsAccepted = 0;
        // 2.5
        int lpxReceived      = 0;
        int lpxAccepted      = 0;
        int lpxSkipsReceived = 0; // LPXNO messages
        int lpxSkipsAccepted = 0; // ... each accepted key
    };

    static constexpr int    kMaxSkips  = 256;
    static constexpr qint64 kSkipTtlMs = 15LL * 60 * 1000;

    // The plugin: a Fetcher of its own for the sender pipeline.
    Hub(Core* core, PluginLink* link, const textreact::Index* index, QObject* parent = nullptr);
    // Tests: fetch stands in for the Fetcher (the caller keeps it alive).
    Hub(Core* core, PluginLink* link, const textreact::Index* index, FetchPort* fetch, const Limits& limits, QObject* parent = nullptr);
    ~Hub() override;

    static Hub* instance(); // GUI thread; nullptr before PeerHub made it and after it went

    void start();           // link-previews.json
    void prepareShutdown(); // the pipeline stops (and joins its worker), the store is written, timers off
    void applySettings();   // Settings::showLinkPreviews / makeLinkPreviews changed

    PreviewStore*   store() const { return m_store; }
    SenderPipeline* pipeline() const { return m_pipeline; }
    Counters        counters() const { return m_counters; }
    int             waitingCount() const { return m_waiting.size(); }
    QString         storePath() const;

    // TeamSpeak's onTextMessageEvent, on the GUI thread (plugin.cpp): our own messages start previews.
    void onTextMessage(quint64 sch, int targetMode, quint16 toId, quint16 fromId, const QString& fromUid, const QString& bbcode);
    // A tsm1 message from the transport (PluginLink::received): LP is ours, everything else is ignored.
    void received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message);
    // The chats' messages changed: LPs waiting for their message are matched again (also polled).
    void indexChanged();

    // 2.4 auto-load: the sender of the message with this local key said its plugin won't announce a preview of
    // that link (an LPNO matched to it; the nearest time within textreact::kToleranceSec).
    bool isSkipped(const QString& messageKey, const QString& urlKey) const;
    int  skipCount() const { return m_skips.size(); }
    // 2.5: the reason of an accepted LPXNO for that link (Unknown for an LPNO, an LPXNO without r, or none at all:
    // isSkipped() says whether there is one).
    SkipReason skipReason(const QString& messageKey, const QString& urlKey) const;

  signals:
    void skipped(const QString& urlKey); // an LPNO or LPXNO was accepted (GUI thread)

  private:
    class FetcherPort;
    enum class Type { Lp, Lpno, Lpx, Lpxno };
    struct Waiting {
        Announcement a; // messageKey still empty
        proto::Scope scope = proto::Scope::Channel;
        QString      hash;
        qint64       estimatedSec = 0;
        qint64       atMs         = 0;
        Type         type         = Type::Lp;
        // LPNO / LPXNO: only a.sch, a.senderUid, a.serverUid and these keys (2.4: a.urlKey alone)
        QStringList  keys;
        SkipReason   reason = SkipReason::Unknown;
        bool skip() const { return type == Type::Lpno || type == Type::Lpxno; }
    };
    struct SkipEntry {
        QString    hash;
        qint64     timeSec = 0;
        QString    urlKey;
        qint64     atMs = 0;
        SkipReason reason = SkipReason::Unknown; // 2.5
    };
    struct Bucket {
        double tokens = 0.0;
        qint64 atMs   = -1;
    };

    QString resolve(const Waiting& w) const;
    void    accept(Announcement a);
    void    acceptSkip(const Waiting& w, const QString& messageKey);
    void    matchWaiting();
    bool    takeToken(quint64 sch, const QString& uid, qint64 nowMs, bool extended = false);
    void    scheduleSave();
    void    save();

    QPointer<Core>                  m_core;
    QPointer<PluginLink>            m_link;
    const textreact::Index*         m_index = nullptr;
    Limits                          m_limits;
    Counters                        m_counters;
    PreviewStore*                   m_store    = nullptr;
    SenderPipeline*                 m_pipeline = nullptr;
    Fetcher*                        m_fetcher  = nullptr; // the plugin's (null in tests)
    std::unique_ptr<FetcherPort>    m_ownPort;
    QVector<Waiting>                m_waiting;
    QVector<SkipEntry>              m_skips; // 2.4 auto-load: accepted LPNOs, oldest first
    QHash<QString, Bucket>          m_rates; // "sch/uid" -> LP tokens
    QHash<QString, Bucket>          m_xRates; // 2.5: "sch/uid" -> LPX / LPXNO tokens
    QTimer*                         m_waitTimer = nullptr;
    QTimer*                         m_saveTimer = nullptr;
    bool                            m_started      = false;
    bool                            m_shuttingDown = false;
    bool                            m_dirty        = false;
};

// The plugin log for a Fetcher's lines (one per finished load): the address tagged private (logtext.h: left
// out of the diagnostic info), the kind and the outcome public. origin: "send" (the pipeline), "chat" (the chat's clicks
// and automatic loads; ChatLinkPreviews logs which one when a load starts).
void usePluginLog(Fetcher* fetcher, const char* origin);

// The folder of website files shown in the chat (Core's cache): the chat's click and automatic loads, and the
// sender pipeline's own fetches (2.4 auto-load: your own link shows from that download at once). Fetcher's file
// names (<key>.<ext>, <key>-thumb.*, <key>.json) in it. Empty when core is null.
QString linksCacheDir(const Core* core);

} // namespace linkpreview
