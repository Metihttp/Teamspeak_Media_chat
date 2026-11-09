#pragma once

// 2.4 link previews: the announcements this client accepted (link-previews.json in the data folder), so a
// chat reloaded from history, or after a restart, shows the server's preview again. QtCore only, plus the
// two helpers at the end that hand an announcement's files to Core.
//
// Entries are keyed by the message's local key (textreact.h: "<hash>-<time>") and the link's url key.
// Lookups forgive the time a little (find): header times are whole seconds, and the sender's own copy is
// stored under the time its plugin saw its own message. Bounded: at most maxPerMessage links per message,
// maxEntries in all (the least recently used go first), maxAgeSec without use, maxFileBytes on disk.
// Everything loaded is checked again like a received LP (announcement.h, isValid); bad entries are
// skipped, a file that isn't ours is moved aside to <path>.bad.

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include "announcement.h"
#include "medialink.h"
#include "textreact.h"

class Core;

namespace linkpreview {

class PreviewStore : public QObject
{
    Q_OBJECT

  public:
    struct Limits {
        int    maxEntries    = 2000;
        int    maxPerMessage = 4;
        qint64 maxAgeSec     = 90LL * 86400;
        qint64 maxFileBytes  = 2 * 1024 * 1024;
        qint64 toleranceSec  = textreact::kToleranceSec;
    };

    enum class Add {
        Added,     // new: announced() was emitted
        Replaced,  // the same sender announced that link of that message again, with other files
        Unchanged, // the same again
        Rejected,  // invalid, from someone else than the one who announced it first, or too many links
    };

    explicit PreviewStore(QObject* parent = nullptr);
    PreviewStore(const Limits& limits, QObject* parent = nullptr);

    // a.messageKey must be a local key (the receiver resolved it). Emits announced(a) and changed() for
    // Added and Replaced.
    Add add(const Announcement& a, qint64 nowSec);
    // What add(a) would do, changing nothing (the hub asks Core for the files before the chat hears of it).
    Add check(const Announcement& a) const;

    // The contract's lookup: the announcement of the link with urlKey in the message with messageKey (the
    // same hash, the nearest stored time within the tolerance), or nullptr. The pointer is good until the
    // store changes: copy what you need.
    const Announcement* find(const QString& messageKey, const QString& urlKey) const;
    QVector<Announcement> forMessage(const QString& messageKey) const;
    int                   count() const;
    // Clear cache: every announcement goes. Which links of which messages had one is kept in memory for the session
    // (the message's hash and time and the link's key only, at most maxEntries; never written), so the chat never
    // turns a sender-made preview into an automatic website load: clearedEarlier().
    void                  clear();
    // 2.4 auto-load: Clear cache took the announcement of the link with urlKey in the message with messageKey this
    // session (find()'s matching). The chat shows such a link as a click card instead of loading its website.
    bool                  clearedEarlier(const QString& messageKey, const QString& urlKey) const;
    int                   clearedCount() const { return m_clearedCount; }

    bool       load(const QString& path, qint64 nowSec);
    bool       save(const QString& path, qint64 nowSec) const;
    QByteArray toJson(qint64 nowSec) const;
    bool       fromJson(const QByteArray& json, qint64 nowSec); // false: not a link-previews document

  signals:
    void announced(const linkpreview::Announcement& announcement);
    void changed();

  private:
    struct Entry {
        Announcement   a;
        qint64         timeSec = 0; // of a.messageKey
        qint64         seenSec = 0; // when it was announced
        mutable qint64 usedSec = 0; // last find()
    };

    Entry*       locate(const QString& hash, qint64 timeSec, const QString& urlKey, qint64 toleranceSec);
    const Entry* locate(const QString& hash, qint64 timeSec, const QString& urlKey, qint64 toleranceSec) const;
    int          countForMessage(const QString& hash, qint64 timeSec) const;
    void         trim(qint64 nowSec);
    void         reset(); // every entry out (loading; clear() remembers what it takes)

    struct Cleared {
        qint64  timeSec = 0;
        QString urlKey;
    };

    Limits                          m_limits;
    QHash<QString, QVector<Entry>>  m_byHash; // message hash -> its entries
    int                             m_count = 0;
    QHash<QString, QVector<Cleared>> m_cleared; // message hash -> links whose announcement Clear cache took (memory only)
    int                             m_clearedCount = 0;
};

// The link Core knows an announcement's files by: the media file with the preview as its preview, or, when
// only the preview was uploaded, the preview itself. Its key() is what Core::entry / still / download take.
MediaLink linkFor(const Announcement& a);

// Asks Core for the files of an announcement shown in a chat: the preview always (it is small and checked
// against its SHA-256), the media file by the user's automatic download rules (Core::ensure). Nothing while
// Settings::showLinkPreviews or Settings::inlinePreviews ("show images in chat") is off (no card shows it then;
// the chat asks again when it shows the card).
void requestFiles(Core* core, const Announcement& a);

} // namespace linkpreview
