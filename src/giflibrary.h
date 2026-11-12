#pragma once

// 2.5 GIFs: the library behind the emoji picker's GIFs tab (design_gifs 3, 4, 6, 7, 9). GUI thread; one per plugin
// (plugin.cpp's g_gifs), created after the chat and deleted after PeerHub and before Core.
//
//  * Records: our own chat messages' GIF links (onTextMessage, 3.2: only the links gifrules.h allows automatically) and
//    our own TS Media GIF uploads once their message is confirmed (onMediaPosted, 3.4). An automatic link record is
//    "unconfirmed" for this session until its local load (cache/links, Fetcher::cached: disk only) shows it is a GIF or
//    loop; a load that shows a still, a video, a page marked for adults or a dead link removes it again (3.3).
//  * Save to GIFs (7): a web GIF as a link item (plus a small animation copy of the file the chat already shows), a TS
//    Media GIF file that is on this PC as a copy. Never a download, never a web request.
//  * Sending (6): a link item as "[URL]<url>[/URL]" through Core::postText (the post queue and flood governor), a file
//    item uploaded again from its copy through Core::send. At most one send per 800 ms per connection; a Permission or
//    Quota refusal of a tab upload disables file items for that channel for the session.
//  * Files (2.1): <folder>/gifs.json, thumbs/<id>.jpg|png, files/<id>.gif|webp|mp4. Copies, hashes, thumbnails and the
//    startup sweep run on m_work (1 thread), thumbnail and animation decodes for the tab on m_decode (2 threads).
//    Results come back queued, as plain values; a result of an older generation (Remove all, shutdown) is dropped.
// Opening the tab never makes a network request: the only link-preview calls here are classifyCached and the static
// Fetcher::cached. No Qt metatype is registered; signals carry QString, int and bool only.

#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

#include "core.h"
#include "gifrules.h"
#include "gifstore.h"

class QTabBar;
class QTimer;
class GifLibrary;

namespace linkpreview {
struct Target;
}

namespace gifs {

// What the picker needs to send from its input (design_gifs 6.1).
struct Host {
    QPointer<GifLibrary>       library;
    QPointer<QTabBar>          chatTabs; // the input's chat tab bar: the picker closes when it switches
    std::function<SendState()> state;    // resolved now, for the picker's own input
    // Resolves again; refuses with changedText() when the chat differs from opened; else sends (6.3 / 6.4).
    std::function<bool(const QString& id, const ChatFingerprint& opened, QString* error)> send;
};

// The chat a host belongs to (ChatIntegration; tests build their own).
struct HostEnv {
    // The chat of the input now; false when a private partner can't be identified (target still filled in).
    std::function<bool(ChatTarget* target)>       resolve;
    std::function<QString(const ChatTarget&)>     describe;  // "the channel “Lobby”"
    std::function<QString(const ChatTarget&)>     replyLead; // the reply being written in that chat ("": none)
    std::function<void(const ChatTarget&)>        replySent; // ends that reply
    QPointer<QTabBar>                             chatTabs;
};

} // namespace gifs

class GifLibrary : public QObject
{
    Q_OBJECT

  public:
    enum class State { Ok, ReadOnly, Unavailable };

    // folder: <data>/gifs (created when something is written). linksDir: the chat's cache/links (empty:
    // linkpreview::linksCacheDir(core)).
    GifLibrary(Core* core, const QString& folder, const QString& linksDir = QString(), QObject* parent = nullptr);
    ~GifLibrary() override; // prepareShutdown()

    static GifLibrary* instance(); // the one alive (settings, diagnostics), or nullptr

    // Loads gifs.json; after a clean load of a non-empty list, sweeps old orphans (4.6).
    void start();
    // Plugin unload: workers canceled and joined, the list written when it changed, every source let go. Idempotent; every
    // entry point does nothing afterwards.
    void prepareShutdown();
    bool isDown() const { return m_down; }
    // A connection from a source of ours (plugin.cpp): disconnected in prepareShutdown.
    void watch(const QMetaObject::Connection& connection);

    // ---- hooks (GUI thread) --------------------------------------------------------------------------------------
    void onTextMessage(uint64 sch, int mode, anyID toId, anyID fromId, const QString& fromUid, const QString& bbcode); // 3.2
    void onMediaPosted(int id);                                                                                     // 3.4
    void onUploadChanged(int id);              // refusals of the tab's own uploads (6.2)
    void onLinkLoaded(const QString& urlKey);  // a local result of the link may be in cache/links (3.3)
    void onEntryChanged(const QString& key);   // a saved link item's thumbnail waits for its entry (7)

    // ---- the list ------------------------------------------------------------------------------------------------
    State                 state() const;
    gifs::Store::Load     loadOutcome() const { return m_load; }
    gifs::Store::LoadStats loadStats() const { return m_stats; }
    QVector<gifs::Item>   items() const { return m_store.items(); }
    const gifs::Item*     find(const QString& id) const { return m_store.find(id); }
    const gifs::Item*     findLink(const QString& urlKey, const QString& pid) const { return m_store.findLink(urlKey, pid); }
    const gifs::Item*     findFile(const QByteArray& sha) const { return m_store.findFile(sha); }
    int                   count() const { return m_store.count(); }
    int                   hiddenCount() const { return m_store.hiddenCount(); }
    qint64                copyBytes() const { return m_store.copyBytes(); }
    bool                  isUnconfirmed(const QString& id) const { return m_unconfirmed.contains(id); }
    int                   unconfirmedCount() const { return m_unconfirmed.size(); }
    int                   thumbWaitCount() const { return m_thumbWaits.size(); }
    QString               folder() const { return m_folder; }
    QString               copyPath(const gifs::Item& item) const; // files/<file> of a file item ("" otherwise)
    bool                  copyExists(const gifs::Item& item) const;
    QString               hostOf(const gifs::Item& item) const;   // a link item's host (punycode), "" otherwise

    bool remove(const QString& id); // the item and its files at once (a copy a Core job still uses is left to the sweep)
    void clear();                   // Remove all (2.7): every item, hidden ones too, gifs.json.bad, every file of our pattern
    // What Remove all would free besides the list's own copies: files of our pattern no item names (a moved-aside list's
    // copies, another version's), in bytes; *files gets their number.
    qint64 leftoverBytes(int* files = nullptr) const;
    // Remove all has something to do (review fix): items (hidden ones too), a moved-aside list, or leftover copies.
    bool   hasAnythingToRemove() const;
    bool touch(const QString& id);

    // ---- Save to GIFs (7) ----------------------------------------------------------------------------------------
    // A web GIF: added at once (how = saved); its thumbnail and animation copy come from the Core entry when it is
    // Ready. false (*message says why) when it can't be kept.
    bool saveLink(const QString& href, const linkpreview::Target& target, const QString& entryKey, bool spoiler, gifs::Kind kind, QString* message);
    // A TS Media GIF file of this PC (Ready): copied on the worker; saveFinished() follows. false: not started (*message).
    bool saveFile(const QString& entryKey, QString* message);

    // ---- sending (6) ---------------------------------------------------------------------------------------------
    static gifs::ChatFingerprint fingerprintOf(const ChatTarget& target);
    // The state of a resolved chat (ChatIntegration::checkSend's rules, the remembered refusals, the upload limit).
    gifs::SendState stateFor(const ChatTarget& target, bool recipientKnown, const QString& destination) const;
    // Sends item id to target now (the host compared the fingerprint). false: nothing was sent (*error).
    bool send(const QString& id, const ChatTarget& target, const gifs::SendState& state, const QString& replyLead, QString* error);
    gifs::Host makeHost(const gifs::HostEnv& env);
    QString    blockOf(const gifs::Item& item, const gifs::SendState& state) const { return gifs::blockOf(item, state, copyExists(item)); }

    // ---- the tab's pictures (4.3, 4.4, 5.7) ----------------------------------------------------------------------
    // The thumbnail covering devicePx (centre-cropped), blurred for a covered item; null while it is decoded
    // (thumbReady(id) follows) or when the item has none.
    QImage  thumbnail(const QString& id, const QSize& devicePx, bool blurred = false);
    // A local file for the hover animation, or "" (never fetched). *kind: Loop for a video.
    QString animationSource(const QString& id, gifs::Kind* kind) const;
    // GIF / WebP: decoded and scaled on a worker; animationReady(id) follows (also when it failed or was refused).
    void requestAnimation(const QString& id, const QSize& devicePx);
    bool takeAnimation(const QString& id, QVector<QImage>* frames, QVector<int>* delaysMs);
    void cancelAnimation();

    QStringList diagnosticLines() const;

    // ---- tests -----------------------------------------------------------------------------------------------------
    void setClockForTests(std::function<qint64()> nowSec) { m_nowForTests = std::move(nowSec); }
    void setOpenFailsForTests(bool fails) { m_openFailsForTests = fails; }
    void setRenameFailsForTests(bool fails) { m_renameFailsForTests = fails; } // moving a corrupt list aside fails
    void waitForWorkers(); // both pools idle, their results delivered
    int  animationJobs() const { return m_animationJobs; }
    bool animationRanOnWorker() const { return m_animationOnWorker.load(); }
    int  thumbnailDecodes() const { return m_thumbDecodes; }

  signals:
    void changed();                       // items were added, removed or reordered
    void itemUpdated(const QString& id);  // a shown item changed (thumbnail, kind)
    void thumbReady(const QString& id);   // a thumbnail() asked for is decoded
    void animationReady(const QString& id);
    void saveFinished(const QString& id, bool ok, const QString& message);

  private:
    enum class CopyPurpose { Upload, SaveFile, Anim };
    enum class ThumbPurpose { Confirm, Fill, ConfirmLoop };

    qint64  nowSec() const;
    bool    usable() const; // not down, not read-only, not unavailable
    void    scheduleSave();
    bool    writeNow();
    void    markChanged(); // save + changed()
    QString thumbsDir() const;
    QString filesDir() const;
    QString jsonPath() const;
    QString linksDir() const;
    void    removeFiles(const gifs::Item& item);
    bool    copyInUse(const QString& path) const;
    void    dropFromSession(const QString& id);
    void    applyBudget();
    void    evicted(const QStringList& ids, const QVector<gifs::Item>& before);
    void    startThumb(const QString& id, ThumbPurpose purpose, const QString& source, bool sourceIsWeb, const QString& factsPath);
    void    onThumbMade(const QString& id, ThumbPurpose purpose, const QString& thumb, const QByteArray& format, int frames, int w, int h);
    void    startCopy(CopyPurpose purpose, const QString& id, const QString& source, const QByteArray& sha, const QString& name, bool spoiler, int w, int h);
    void    onCopied(CopyPurpose purpose, const QString& id, bool ok, const QString& error, const QString& file, const QByteArray& sha, qint64 size,
                     const QString& thumb, int w, int h, const QString& name, bool spoiler);
    void    fillFromEntry(const QString& id, const QString& entryKey);
    void    sweep();
    QString refusalKey(const QString& serverUid, uint64 channelId) const;

    QPointer<Core>          m_core;
    QString                 m_folder;
    QString                 m_linksDir;
    gifs::Store             m_store;
    gifs::Store::Load       m_load  = gifs::Store::Load::Missing;
    gifs::Store::LoadStats  m_stats;
    bool                    m_unavailable = false;
    bool                    m_started     = false;
    bool                    m_down        = false;
    bool                    m_dirty       = false;
    bool                    m_saveFailedLogged = false;
    QTimer*                 m_saveTimer = nullptr;
    QList<QMetaObject::Connection> m_connections;

    QStringList             m_unconfirmed;      // automatic link records of this session (at most 64, oldest first)
    QHash<QString, QString> m_thumbWaits;       // entry key -> saved link item id (at most 16)
    QSet<QString>           m_copying;          // shas of copies on the worker (one copy per file)
    QHash<QString, QString> m_refused;          // serverUid/channel -> the refusal's text (6.2)
    QHash<int, QString>     m_sentBatches;      // batches of the tab's file sends -> their refusal key
    QHash<uint64, qint64>   m_lastSend;         // connection -> m_clock time of the last send (800 ms spacing)
    QElapsedTimer           m_clock;

    QThreadPool                         m_work;   // copies, hashes, thumbnails, frame counts, the sweep
    QThreadPool                         m_decode; // the tab's thumbnail and animation decodes
    std::shared_ptr<std::atomic<bool>>  m_cancel; // the current generation's flag
    int                                 m_gen = 0;

    QCache<QString, QImage>             m_thumbs;   // id|w|h(|b) -> decoded tile picture (cost: bytes)
    QSet<QString>                       m_decoding; // cache keys being decoded
    std::shared_ptr<std::atomic<bool>>  m_animCancel;
    QString                             m_animId;     // the one sequence kept (or being decoded)
    QSize                               m_animSize;
    bool                                m_animDone = false;
    bool                                m_animOk   = false;
    QVector<QImage>                     m_animFrames;
    QVector<int>                        m_animDelays;

    std::function<qint64()> m_nowForTests;
    bool                    m_openFailsForTests = false;
    bool                    m_renameFailsForTests = false;
    int                     m_animationJobs     = 0;
    int                     m_thumbDecodes      = 0;
    std::atomic<bool>       m_animationOnWorker{false};
    std::atomic<int>        m_pending{0}; // results posted back but not handled yet (waitForWorkers)
};
