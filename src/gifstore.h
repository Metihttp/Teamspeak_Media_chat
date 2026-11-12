#pragma once

// 2.5 GIFs: the list behind the emoji picker's GIFs tab (design_gifs 2). Pure QtCore, unit-tested (tst_gifs): items,
// their JSON (gifs.json, format 1), validation on load, dedupe, the caps. No files are read or written here; the library
// (giflibrary.h) does the file work and hands the bytes in and out.
//
//  * A link item is sent again as its link text; a file item is uploaded again from a local copy (files/<id>.<ext>).
//  * Order: the most recently used first (`used`); an add or a touch moves the item to the front. Ties keep the array
//    order of the file.
//  * Load keeps what it can (2.3): only structurally invalid entries are dropped; a link whose key changed with a new
//    classifier is re-keyed, one that isn't media any more is kept as a plain link item. Entries of a newer version (an
//    unknown src or kind) are kept verbatim and written back unchanged, outside the caps. A document of a newer format
//    makes the store read-only.
// Strings: JSON keys go through QLatin1String; items hold only runtime strings (no QStringLiteral data).

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace gifs {

enum class Source { Link, File };
enum class Kind { Gif, Webp, Loop };
enum class How { Sent, Saved };

struct Limits {
    int    maxItems          = 100;
    qint64 maxCopyBytes      = 25LL << 20; // one file item's copy
    qint64 maxAnimCopyBytes  = 5LL << 20;  // a saved link item's hover copy
    qint64 maxTotalCopyBytes = 256LL << 20;
    qint64 maxFileBytes      = 1LL << 20; // gifs.json
    qint64 maxThumbBytes     = 160LL << 10;
};

constexpr qint64 kMaxLinkSize   = 1LL << 30; // a link item's informational size
constexpr int    kMaxDimension  = 65535;
constexpr qint64 kMaxDurationMs = 600000;
constexpr int    kMaxNameChars  = 120;
constexpr int    kMaxUrlChars   = 2048;

struct Item {
    QString     id;
    Source      source = Source::Link;
    Kind        kind   = Kind::Gif;
    How         how    = How::Sent;
    QString     url, urlKey, pid, anim; // Link (anim: optional hover copy, "<id>.gif|webp|mp4")
    qint64      animSize = 0;           // bytes of the anim copy ("asize"; 0 without one)
    QString     name, file;             // File
    QByteArray  sha;                    // File: 32 bytes
    bool        spoiler = false;        // "sp": shown covered; a file item goes out as a spoiler again
    bool        plain   = false;        // memory only: today's classification isn't media (2.3)
    qint64      size = 0;
    int         width = 0, height = 0;
    qint64      durationMs = 0;
    QString     thumb;                  // "<id>.jpg|png" or empty (placeholder tile)
    qint64      addedSec = 0, usedSec = 0;
    QJsonObject extra;                  // unknown fields, written back unchanged
};

// What the store needs to know about a link today (injected: the store stays free of linkpreview).
struct LinkFacts {
    QString key;           // classify(url).key ("" when it isn't classified any more)
    bool    media = false; // a MediaPage or Direct target today
    QString pid;           // "<provider>:<itemId>" when the row reads an id
};
using LinkFactsOf = std::function<LinkFacts(const QString& url)>;

// A link text that may be stored and sent as "[URL]<url>[/URL]": https (or http, as TeamSpeak links "www." text), at
// most kMaxUrlChars characters, nothing that could break out of the tag or the message (white space, controls, '[',
// ']', '"').
bool storableUrl(const QString& url);
// 16 lower-case hex digits.
bool isItemId(const QString& id);
// "<id>.<ext>" with ext one of exts.
bool isOwnFileName(const QString& name, const QString& id, std::initializer_list<const char*> exts);
// Our pattern in thumbs/ and files/ (2.1): 16 hex + .jpg/.png/.gif/.webp/.mp4, optionally + ".part".
bool isOurPattern(const QString& fileName);
// A file item's name (2.2): the display name through textclean (one line; no controls, bidi or invisible characters:
// it may be someone else's file name), at most kMaxNameChars with its extension kept whole (a long name is cut in its
// base, so ".gif" never goes), never empty ("GIF.gif").
QString itemName(const QString& displayName);
// The name a file item goes up as: the stored name's base with the copy's own extension (file "<id>.gif|webp"), so
// every receiver sees a GIF whatever the stored name says.
QString remoteNameFor(const QString& name, const QString& file);
// The SHA-256 of a copy as written in gifs.json: 43 base64url characters (MediaLink::sha256's form).
QString    shaText(const QByteArray& sha);
QByteArray shaFromText(const QString& text); // empty when invalid

const char* kindName(Kind kind); // "gif", "webp", "loop"

class Store
{
  public:
    enum class Load { Ok, Missing, Unavailable, Corrupt, TooLarge, Newer };
    struct LoadStats {
        int items = 0, hidden = 0, dropped = 0, rekeyed = 0, plain = 0;
    };

    explicit Store(const Limits& limits = {});

    const Limits& limits() const { return m_limits; }

    // The existing item for it (same urlKey or pid; same sha) is touched and its url / size / w / h / thumb updated when
    // given; otherwise item goes in front. Returns the item's id (empty: read-only, or an invalid item); *evicted gets
    // the ids removed by maxItems.
    QString add(Item item, qint64 nowSec, QStringList* evicted = nullptr);
    bool    touch(const QString& id, qint64 nowSec); // used = now, moved to the front
    bool    update(const Item& item);                // same id: fields replaced, place kept
    std::optional<Item> remove(const QString& id);
    void    clear(); // everything, hidden items included

    const Item* find(const QString& id) const;
    const Item* findLink(const QString& urlKey, const QString& pid) const;
    const Item* findFile(const QByteArray& sha) const;
    QVector<Item> items() const; // display order; hidden items are raw JSON, never Items
    int    count() const { return m_items.size(); }
    int    hiddenCount() const { return m_hidden.size(); }
    qint64 copyBytes() const; // file items' copies and anim copies
    // Least recently used first: ids whose copy must go so copyBytes() <= maxTotalCopyBytes (a file item: removed; a link
    // item: only its anim copy is dropped).
    QStringList   overBudget() const;
    QSet<QString> namedFiles() const; // file/thumb/anim names of every item, hidden ones included (2.4, 4.6)
    // A fresh id no item (hidden ones included) has.
    QString newId() const;

    QByteArray toJson() const;
    // bytes as read; Missing and Unavailable are decided by the caller (file I/O), never here. Corrupt and TooLarge leave
    // an empty, writable list; Newer an empty, read-only one.
    Load fromJson(const QByteArray& bytes, qint64 nowSec, const LinkFactsOf& factsOf, LoadStats* stats = nullptr);
    bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; } // the library: an Unavailable list

    static bool valid(const Item& item, const Limits& limits = {});

    // One item as JSON (tests).
    static QJsonObject itemToJson(const Item& item);

  private:
    int  indexOf(const QString& id) const;
    void evict(QStringList* evicted);

    Limits               m_limits;
    QVector<Item>        m_items;  // display order
    QVector<QJsonObject> m_hidden; // entries of a newer version, verbatim, in their order
    QJsonObject          m_top;    // unknown top-level fields
    bool                 m_readOnly = false;
};

} // namespace gifs
