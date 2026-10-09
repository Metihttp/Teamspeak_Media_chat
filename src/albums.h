#pragma once

// 2.2 album grid (feature 5): pictures and videos sent together are shown as one Discord-like grid.
// Everything here is pure (QtCore only, no TeamSpeak or widgets), so the plugin, the unit tests and
// tools/render_gallery share it: the feature switch, the grid geometry used by both drawing and hit
// testing, how the links in chat messages group into albums, TeamSpeak's message header, and who
// posted which album.

#include <QHash>
#include <QPair>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace albums {

// The album feature. Off: the send window hides "Send as an album", Core::send() sends no al/ai/an,
// and received albums are shown as separate previews. The one place to switch it.
inline bool enabled()
{
    return true;
}

// ---- grid geometry ---------------------------------------------------------------------------------

constexpr int kGap            = 4;   // between tiles, logical pixels (transparent)
constexpr int kMaxTiles       = 6;   // tiles shown; with more items the last one says "+N"
constexpr int kMaxTilesNarrow = 4;   // ... in a narrow grid
constexpr int kNarrowWidth    = 224; // narrower grids use at most two columns
constexpr int kMinTile        = 24;  // no tile is smaller than this in either direction

struct Geometry {
    QSize          box;          // the whole grid, logical pixels
    QVector<QRect> tiles;        // the tiles shown, in album order (the first items of the album)
    int            overflow = 0; // items the last tile stands for ("+N"; it covers its own item too), 0 = none
};

// The grid for an album of `items` pictures/videos, maxWidth x maxHeight being the chat's preview
// limits (PreviewStyle). Its width is maxWidth; its height at most round(maxHeight * 4 / 3), so an
// album is never much taller than a single picture. Mosaic by the number of tiles (Discord-like):
//   2: [2]   3: one large tile and two stacked on its right   4: [2,2]   5: [2,3]   6+: [3,3]
// Narrow (maxWidth < kNarrowWidth): 3: [1,2] with a 16:9 top row, 4+: [2,2]. Rows want square tiles
// and are scaled down together to fit the height. Deterministic; it depends on nothing that loads,
// so an album never changes size while its pictures arrive.
Geometry layout(int items, int maxWidth, int maxHeight);

// The tile at pos (relative to the grid's top-left), -1 for a gap or outside. Tile edges belong to
// the tile.
int tileAt(const Geometry& geometry, const QPointF& pos);

// ---- grouping chat messages into albums ------------------------------------------------------------

// One file link in a chat message, as the grouping sees it.
struct Link {
    QString key;         // MediaLink::key()
    quint32 albumId = 0; // 0: not an album item (also every link that isn't a picture, GIF or video)
    int     index   = 0; // 1-based position in its album (ai)
    int     count   = 0; // the album's size (an)
};

// One chat message (one QTextBlock), in document order.
struct Message {
    QString       sender;       // who posted it: "u:<unique id>", "n:<nickname>"; empty if unknown
    bool          bare = false; // nothing but TeamSpeak's header in front of its first link
    QVector<Link> links;        // its file links in order (also those that are not album items)
};

struct Album {
    quint32                  albumId = 0;
    QStringList              keys;              // keys in grid order; empty: hasn't arrived yet
    QVector<QPair<int, int>> members;            // (message, link) of each item, in document order
    int                      anchorMessage = -1; // the grid goes into this message,
    int                      anchorLink    = -1; // ... right after this link (its last item there)
    QVector<int>             hiddenMessages;     // later messages of the album: nothing in them but its items
    bool                     open = false;       // at the end of the chat and still waiting for items
};

// Albums in a chat, in document order. An album is a run of consecutive items with the same id and
// no position twice. It continues into the next message only if that message directly follows, has
// the same known sender, is bare and holds nothing but further items of it (the rare case of an
// album sent as several messages). The first run of an id that ends the chat is open: it shows all
// `count` keys, with placeholders for what hasn't arrived. Other runs show only their items, by
// position. A run with fewer than two keys is no album: its items stay single previews.
QVector<Album> group(const QVector<Message>& messages);

// ---- keeping a chat document in step ---------------------------------------------------------------

// The keys of the album items in each message of group()'s result (message index -> keys). No link to
// one of these files in that message gets a single preview of its own: the grid there shows it. The
// chat's scan (which adds single previews) and its album plan (which takes stale ones out) both follow
// this, so a second link to an album item in the same message never gets a preview that the plan then
// takes out again, scan after scan.
QHash<int, QSet<QString>> memberKeys(const QVector<Album>& albums, const QVector<Message>& messages);

// A grid in a chat document, or one an album plan wants there: its message (block) and object id.
struct GridPlace {
    int     block = 0;
    QString id;
};
// What becomes of the grids of a document. remove: indexes into existing of grids that are no longer
// wanted (or a copy too many); insert: indexes into wanted of grids that aren't there yet. A message
// can hold several grids (two albums in one message, or one album split in two runs).
struct GridEdits {
    QVector<int> remove;
    QVector<int> insert;
};
GridEdits gridEdits(const QVector<GridPlace>& existing, const QVector<GridPlace>& wanted);

// ---- TeamSpeak's message header ----------------------------------------------------------------------

// TeamSpeak 3.6 starts a chat message block with an icon (U+FFFC), the time ("<20:02:13>", optional:
// it can be turned off), a space, the sender's nickname in quotes linked to
// client://<client id>/<unique id>~<nickname>, and ": ". (Measured in S0 with timestamps on.)
struct Header {
    bool    valid = false;
    QString nick;
    QString uid;        // from the nickname's link; empty without one
    int     length = 0; // characters of the header: the message text starts there
};

// before: the block's text in front of the nickname link; nickText / nickHref: that link's text and
// target; after: the block's text after it (at least up to the first file link).
Header parseHeader(const QString& before, const QString& nickText, const QString& nickHref, const QString& after);
// The same from the text alone (no nickname link found): `"Nick": ` after the optional icon and time.
// A nickname that contains `":` can't be told apart from the message, so it gives no header.
Header parseHeaderText(const QString& text);
// The unique id in a client:// link of a chat header; empty if it isn't one.
QString uidFromClientHref(const QString& href);

// ---- the album object in a chat document -----------------------------------------------------------

// The name of an album's grid in the chat document: "album.<8 hex id>.<key>.<key>..." with "-" for
// an item that hasn't arrived. It names what the grid shows, so the grid is redrawn under a new name
// when that changes.
QString objectId(quint32 albumId, const QStringList& keys);
bool    isObjectId(const QString& id);
bool    parseObjectId(const QString& id, quint32* albumId, QStringList* keys);

// ---- who posted which album ------------------------------------------------------------------------

// Albums seen in chat messages (Core::onTextMessage, with TeamSpeak's sender id) and posted by us.
// The first sender of an album owns it: items of the same album id from anyone else are ignored, so
// they can't join it in the chat. Remembers the last kMaxAlbums albums.
class Registry
{
  public:
    static constexpr int kMaxAlbums = 500;

    struct Entry {
        QString     sender;
        int         count = 0;
        QStringList keys; // by position - 1; empty: not seen yet
    };

    // An item of album albumId (on the server serverUid) posted by sender. Ignored when anything is
    // missing or out of range, when the album belongs to someone else, or its position is taken.
    void note(const QString& serverUid, quint32 albumId, int index, int count, const QString& key, const QString& sender);
    // The album's sender if key is its registered item at index; empty otherwise (unknown).
    QString      senderOf(const QString& serverUid, quint32 albumId, int index, const QString& key) const;
    const Entry* find(const QString& serverUid, quint32 albumId) const;
    int          size() const { return m_albums.size(); }
    void         clear();

  private:
    static QString idOf(const QString& serverUid, quint32 albumId);

    QHash<QString, Entry> m_albums;
    QStringList           m_order; // least recently noted first
};

} // namespace albums
