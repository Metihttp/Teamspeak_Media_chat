#pragma once

// 2.2 reactions: who reacted how to which media. QtCore only (unit-tested).
//
// Every reactor has one complete set per media (a 6-bit mask, see peerprotocol.h); a new message from
// them replaces it, an empty set removes them. Identities are TeamSpeak client UIDs as the server
// reported them (never from a payload). Keys are MediaLink::key(): the same media has the same key on
// every client. Reactions for media this client doesn't know (yet) wait a short time in an orphan
// buffer. The store is kept in reactions.json so previews that survive a plugin reload keep them.

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "peerprotocol.h"

// What a reaction row shows for one media.
struct ReactionView {
    struct Entry {
        int         count = 0;
        bool        mine  = false;
        QStringList others; // display names of the other reactors, earliest first (raw: sanitise to show)
    };
    Entry per[proto::kReactionCount];

    bool isEmpty() const;
    int  ownMask() const;
};

class ReactionStore : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        int    maxMedia       = 500;          // least recently changed ones are forgotten beyond this
        int    maxReactors    = 100;          // per media; new reactors beyond this are ignored
        qint64 maxAgeSec      = 30LL * 86400; // older entries are dropped when saving / loading
        int    maxOrphans     = 64;
        qint64 orphanTtlMs    = 15000;
        qint64 maxFileBytes   = 2 * 1024 * 1024;
        int    maxNameChars   = 64;
    };

    enum class Apply {
        Applied,   // the view changed
        Unchanged, // the same set again (duplicate or idempotent repeat)
        Orphaned,  // the media isn't known (yet): kept in the orphan buffer for a while
        Rejected,  // invalid, or a new reactor beyond maxReactors
    };

    explicit ReactionStore(QObject* parent = nullptr);
    ReactionStore(const Limits& limits, QObject* parent = nullptr);

    // Whether a media key belongs to media this client has seen (Core has an entry for it on the server
    // the reaction came from). Without a check every key counts as known.
    using KnownKey = std::function<bool(const QString& key, const QString& serverUid)>;
    void setKnownKey(KnownKey check);

    // A reactor's complete set on key (from a message): replaces what they had; mask 0 removes them.
    Apply applyRemote(const QString& serverUid, const QString& key, const QString& uid, const QString& name, quint8 mask, qint64 nowMs);
    // Our own set (optimistic, before it is sent; reverting sets the old mask again).
    void   setOwn(const QString& key, const QString& ownUid, const QString& ownName, quint8 mask, qint64 nowMs);
    quint8 maskOf(const QString& key, const QString& uid) const;

    ReactionView view(const QString& key, const QString& ownUid) const;
    bool         hasReactions(const QString& key) const;
    QStringList  keys() const;
    int          reactorCount(const QString& key) const;

    // SYNC: our own non-empty sets on the requested keys that we know.
    QVector<proto::ReactItem> syncAnswer(const QStringList& requested, const QString& ownUid) const;

    // A key became known (Core registered it): its orphans are applied. Expired orphans are dropped.
    void resolveOrphans(const QString& key, qint64 nowMs);
    void resolveKnownOrphans(qint64 nowMs); // every orphan whose media is known by now
    void expireOrphans(qint64 nowMs);
    int  orphanCount() const { return m_orphans.size(); }

    void clear();

    // reactions.json. load() replaces the content; a file that can't be read as ours is renamed to
    // <path>.bad and the store starts empty. Every entry is checked again; bad ones are skipped.
    bool       load(const QString& path, qint64 nowSec);
    bool       save(const QString& path, qint64 nowSec) const;
    QByteArray toJson(qint64 nowSec) const;
    bool       fromJson(const QByteArray& json, qint64 nowSec); // false: not a reactions document

    static bool isClientUid(const QString& uid);

  signals:
    void changed(const QString& key); // every change, not coalesced

  private:
    struct Reactor {
        QString name;
        quint8  mask = 0;
        qint64  tSec = 0;
    };
    struct Media {
        qint64                   seenSec = 0;
        QHash<QString, Reactor>  by; // uid -> set
    };
    struct Orphan {
        QString serverUid, key, uid, name;
        quint8  mask = 0;
        qint64  atMs = 0;
    };

    Apply   set(const QString& key, const QString& uid, const QString& name, quint8 mask, qint64 nowSec);
    QString cleanName(const QString& name) const;
    void    trim(qint64 nowSec);

    Limits                m_limits;
    KnownKey              m_known;
    QHash<QString, Media> m_media;
    QVector<Orphan>       m_orphans;
};
