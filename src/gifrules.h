#pragma once

// 2.5 GIFs: which links and files count as GIFs, which may be recorded automatically, and what a chat may send (design_gifs
// 3.1, 3.6, 6.1, 6.2, 7). Pure (QtCore and the link classifier), unit-tested in tst_gifs.
//
// Automatic records (3.1) only take links that can't carry a private capability: page links of the public GIF sites
// (Giphy, Tenor, Klipy, Imgur .gifv, Imgflip, Gifer, Make A Gif) and https direct .gif / .webp links without a query, user
// info or fragment, never on Discord's or Dropbox's hosts. Everything else the chat shows as a GIF goes in only by hand
// (Save to GIFs). The provider table (kProviderRules in gifrules.cpp) names every provider id: a provider added later is
// "not automatic" until someone decides otherwise (tst_gifs walks providers::ids()).

#include <QByteArray>
#include <QString>
#include <QVector>

#include "gifstore.h"
#include "linkpreview/linkurl.h"
#include "medialink.h"

namespace gifs {

// ---- links (3.1) ---------------------------------------------------------------------------------------------------

struct Candidate {
    QString             url;    // the link text as sent, after trimLinkEnd
    linkpreview::Target target; // classifyCached(url)
    Kind                kind = Kind::Gif; // the expected kind (a link item is never Webp)
    QString             pid;    // "<provider>:<itemId>" for MediaPage rows that read an id
    bool                gifLike     = false; // the link can show as a GIF or loop at all (Save to GIFs may take it)
    bool                autoRecord  = false; // recorded automatically when sent (3.1)
    bool                checkFrames = false; // a direct .webp: removed again unless its file has more than one frame
};

// One link as the pipeline reads it (trimLinkEnd, classifyCached) and what 3.1 says of it.
Candidate candidateOf(const QString& href);
// The links of a message that get a slot, exactly as the sender pipeline and every receiver pick them (linkHrefs,
// trimLinkEnd, classifyCached, selectLinks: at most 3, the reply quote line excluded), in message order.
QVector<Candidate> messageLinks(const QString& bbcode);
// "<provider>:<itemId>" for a MediaPage target with both, except the rewrites and the self-hosted share rows.
QString pidOf(const linkpreview::Target& target);
// The provider's rule in the table: whether the id is assigned at all (tests), and whether a sub of it is automatic.
bool providerAssigned(const QString& providerId);
bool providerAutomatic(const QString& providerId, const QString& sub);

// The chat message a link item goes out as: "[URL]<url>[/URL]" (url must be storableUrl).
QString linkText(const QString& url);

// ---- GIF detection (3.6) -------------------------------------------------------------------------------------------

struct Facts {
    QByteArray format; // safeimage::formatOf: "gif", "webp", "png", ...; empty: not a picture
    int        frames = 0; // QImageReader::imageCount(); 0: the reader can't tell
};
enum class Verdict { Gif, Webp, NotGif };
Verdict judge(const Facts& facts);

// ---- the chat menu (7) ---------------------------------------------------------------------------------------------

enum class ChatGif {
    None,         // no Save to GIFs item
    Link,         // a web GIF: saved as a link item
    File,         // a TS Media GIF file on this PC: copied
    FileNotReady, // a TS Media GIF file not downloaded yet: the item shown disabled ("download it first")
    TooBig,       // over the copy limit: no item
};
struct ChatGifFacts {
    bool      web = false;      // a web link's media (the slot's link)
    MediaKind kind = MediaKind::Other;
    bool      ready = false;    // the entry's file is on this PC
    bool      gone  = false;    // NotFound, Password: nothing can get the file
    qint64    size  = 0;        // the link's size (0: unknown)
    bool      webpName = false; // the file's name ends in .webp
    bool      animates = false; // InlineMediaController::animates(key)
    bool      loop     = false; // MediaLink::loop (a silent looping video from a website)
    bool      spoilerHidden = false;
};
struct ChatGifVerdict {
    ChatGif type = ChatGif::None;
    Kind    kind = Kind::Gif;
};
ChatGifVerdict chatGifOf(const ChatGifFacts& facts, qint64 maxCopyBytes = Limits().maxCopyBytes);

// ---- sending (6.1, 6.2) --------------------------------------------------------------------------------------------

// Which chat a send goes to. Taken when the picker opens and again at each click; any difference refuses the send.
struct ChatFingerprint {
    quint64 sch = 0;
    QString serverUid;
    int     mode = 0;      // TextMessageTarget_*
    QString clientUid;     // private chats: the partner
    quint64 channelId = 0; // your channel (channel messages go there; files are stored there)
    bool    valid = false;
    bool operator==(const ChatFingerprint& o) const;
    bool operator!=(const ChatFingerprint& o) const { return !(*this == o); }
};
struct SendState {
    bool    canText  = false; // link items can go (connected, a recipient)
    bool    canFiles = false; // file items can go (canText, no channel password, no refusal remembered)
    QString blocked;          // !canText: why
    QString filesBlocked;     // canText && !canFiles: why
    qint64  uploadLimit = 0;  // bytes (0: none known)
    QString uploadLimitText;
    QString destination;      // "the channel “Lobby”"
    bool    serverChat = false;
    ChatFingerprint chat;
};
// Why item can't be sent now (empty: it can). copyExists: a file item's copy is on disk.
QString blockOf(const Item& item, const SendState& state, bool copyExists);

// ---- texts (5.12) --------------------------------------------------------------------------------------------------

QString missingCopyText(); // T-miss
QString waitText();        // T-wait
QString changedText();     // T-changed
QString tooBigText();      // T-big
QString serverFileText();  // T-srvfile
// "Nov 9": English month names whatever the system's language (layoutart's pattern).
QString dateText(qint64 unixSec);

} // namespace gifs
