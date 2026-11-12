#pragma once

// 2.4 link previews: the announcement of a sender-made preview, and its tsm1 message LP (peerprotocol.h).
// Pure QtCore (unit-tested with hostile input): nothing here knows TeamSpeak.
//
// The sender's plugin fetched a link of its own message once, uploaded the preview (and, under the caps,
// the media file itself) to the channel's file browser and announces it with ONE plugin command to the
// same chat. Receivers show the preview downloaded from the TeamSpeak server, never from the website.
//
//   LP s=c|p m=<hash>-<age> u=<url key> k=y|i|g|v c=<channel id>
//      p=<preview path> h=<preview sha> ps=<preview bytes>
//      [f=<media path> fh=<media sha> fs=<media bytes>]
//      [w=<width> ht=<height>] [d=<duration ms>] [t=<title>] [a=<author>]
//
//  * s: the chat: c the channel (to the sender's channel), p a private chat (to that partner only). The
//    server chat has no LP (plugin commands have no target for it; receivers load its links themselves).
//  * m: the message, as text reactions name it on the wire (textreact.h: the content hash of the sender's
//    uid and text, and how many seconds ago the sender received it). Receivers accept an LP only for a
//    message of the invoker's (the uid the server filled in) found in that chat (textreact::Index,
//    senderOnly), never from ServerQuery clients.
//  * u: classify(url).key of the link (8..64 lower-case hex digits); k: YouTube, image, GIF, video.
//  * c: the channel whose file browser holds the files (channel chats: the receiver's own channel).
//  * p / f: remote paths, base64url (no padding) of their UTF-8: absolute, no "." or ".." parts, no
//    backslashes or control characters, at most kMaxPathBytes; the preview a .jpg/.jpeg/.png, the media
//    file's extension fitting k (i: png jpg jpeg webp, g: gif, v: mp4 webm mov m4v). YouTube has no f.
//  * h / fh: SHA-256 of the file, 43 base64url characters (canonical); ps / fs: its size in bytes, at
//    most kMaxPreviewBytes / kMaxMediaBytes. f, fh and fs come together or not at all.
//  * w / ht: the media's display size (both or none, 1..16384); d: a video's duration (1 ms..24 h).
//  * t / a: a YouTube video's title and channel, base64url of UTF-8, at most kMaxTitleBytes /
//    kMaxAuthorBytes; control characters and bidi marks are removed on both sides.
// Unknown fields are ignored; any field that breaks its rule rejects the whole message. A message stays
// within proto::kMaxSendBytes: the title, then the author are shortened (then left out) to fit.
//
//   LPNO s=c|p m=<hash>-<age> u=<url key>
//      2.4 auto-load: the sender's plugin will not announce a preview of that link of that message (its job
//      ended without one). Receivers waiting for the sender's preview load the link themselves. The same s, m
//      and u rules as LP, the same checks on the receiving side (the message's own sender only, never
//      ServerQuery, s=c only from our channel, LP's per-sender limit); unknown fields ignored.
//
// 2.5: the new link classes (media sites, site cards, web cards: linkurl.h) are announced with types of their own,
// which 2.4.0 and 2.3.x drop unread (unknown types), so they never show a preview they can't place. LP and LPNO
// stay exactly 2.4.0's, for the 2.4 classes only.
//
//   LPX s=c|p m=<hash>-<age> u=<url key> k=i|g|v|a|c
//       [c=<channel id> p=<preview path> h=<preview sha> ps=<preview bytes>]
//       [f=<media path> fh=<media sha> fs=<media bytes>]
//       [w=<width> ht=<height>] [d=<duration ms>]
//       [t=<title>] [a=<author>] [n=<site name>] [x=<description>]
//       [cl=<rrggbb>] [ly=l|t] [pg=1] [sn=1]
//
//  * s, m, u as LP; k: i a picture, g a GIF (animated GIF or WebP), v a video, a a silent looping video (a GIF-like
//    MP4 or WebM), c a card.
//  * c, p, h, ps come together or not at all: required for i g v a, optional for c (a card without a picture). The
//    same path, sha and size rules as LP. f, fh, fs together or not at all, only for i g v a; the media file's
//    extension by k (i: png jpg jpeg webp, g: gif webp, v: mp4 webm mov m4v, a: mp4 webm).
//  * w / ht both or none (1..16384), only with p or f (the media's size, for a card its picture's); d (1 ms..24 h)
//    only for v, a and c.
//  * t, a, n, x: only for c, base64url of UTF-8 (at most 256, 96, 64 and 384 bytes), cleaned again on receipt
//    (textclean); t is required. A card needs x or p. cl: only for c, the page's theme colour (6 hex digits); one
//    that isn't a theme colour a card may wear is ignored (no rejection). ly: only for c with p (l Large, t Thumb).
//    pg=1: only for c, a play glyph on the picture. sn=1: any k, the page says it is for adults (a spoiler cover).
// Any field that breaks its rule rejects the whole LPX; unknown fields are ignored. Its ceiling is
// proto::maxSendBytes("LPX") (1400); a sender uses lpxSendCap() of its connection (900 until live check L1) and
// shortens the description, then the title, then leaves out the author, the site name and the description to fit.
//
//   LPXNO s=c|p m=<hash>-<age> u=<url key>[,<url key>[,<url key>]] [r=n|x]
//      The sender's plugin won't announce (an LPX of) those links of that message: 1 to 3 distinct keys. r (with
//      one key only): n = it found nothing to show (no card, a wall, an expired link), x = the page redirected to
//      another website. 2.5 receivers then don't load it by themselves; without r (a failure, too large, the
//      intake limit) they may (as after LPNO). Any other r is read as none. The intake notice: one LPXNO without r
//      for every link of one message the sender's plugin refused at intake (its per-minute limit, its queue).

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

#include "carddata.h"
#include "linkurl.h"
#include "peerprotocol.h"

namespace linkpreview {

constexpr qint64 kMaxPreviewBytes = 2LL * 1024 * 1024;   // an uploaded preview picture
constexpr qint64 kMaxMediaBytes   = 100LL * 1024 * 1024; // an uploaded media file (the sender's caps are lower)
constexpr int    kMaxPathBytes    = 240;                 // a remote path (UTF-8)
constexpr int    kMaxTitleBytes   = 120;                 // a YouTube title (UTF-8, after cleaning)
constexpr int    kMaxAuthorBytes  = 64;                  // a YouTube channel name
constexpr int    kMaxSide         = 16384;
constexpr qint64 kMaxDurationMs   = 24LL * 3600 * 1000;

// The contract's announcement, plus what a receiver needs to download the files (sender-previews:
// serverUid, channelId, previewSize, privateChat).
struct Announcement {
    QString messageKey;  // the message's local key (textreact::localKey: "<hash>-<time>")
    QString urlKey;      // classify(url).key
    Kind    kind = Kind::None;
    QString previewPath; // "/tsmedia/previews/3f9a1c2e.jpg"
    QString previewSha;  // base64url SHA-256 (43 characters)
    QString mediaPath;   // empty: only the preview is on the server
    QString mediaSha;
    qint64  mediaSize  = 0;
    int     width      = 0, height = 0;
    qint64  durationMs = 0;
    QString title, author; // cleaned (cleanText)
    quint64 sch = 0;       // the connection it came on (not stored)
    QString senderUid;     // TeamSpeak's invoker uid
    // sender-previews additions
    QString serverUid;       // the server whose file browser holds the files
    quint64 channelId   = 0; // ... and the channel
    qint64  previewSize = 0;
    bool    privateChat = false; // announced in a private chat (s=p)
    // 2.5: an LPX (a link of the new classes); false: an LP. title / author stay YouTube's (LP); an LPX card's text is
    // in card (site, author, title, description, layout, colour, play). Its picture size and duration are width /
    // height / durationMs above (card.pictureWidth/Height and card.durationMs mirror them when read).
    bool     extended  = false;
    CardData card;
    bool     sensitive = false; // sn=1 (any kind)
};

// An LP as read from the wire: the announcement (messageKey still empty: the receiver resolves m) and m.
struct Incoming {
    Announcement announcement;
    proto::Scope scope = proto::Scope::Channel;
    QString      hash;       // m's content hash
    qint64       ageSec = 0; // m's age
};

// ---- the parts --------------------------------------------------------------------------------------

QString    toBase64Url(const QByteArray& bytes);
// Exactly that text decodes (canonical, no padding, at most maxBytes); nullopt otherwise.
std::optional<QByteArray> fromBase64Url(const QByteArray& text, int maxBytes);
QString    shaText(const QByteArray& sha256);    // 43 characters; empty unless 32 bytes
QByteArray shaBytes(const QString& text);        // 32 bytes, or empty for anything but a canonical sha
bool       isUrlKey(const QString& key);         // 8..64 lower-case hex digits
// A remote path a receiver may download: see above. kind None: any picture or media extension.
bool       isRemotePath(const QString& path);
bool       isPreviewPath(const QString& path);   // ... a picture (.jpg .jpeg .png)
bool       isMediaPath(Kind kind, const QString& path);
// Text from a website made safe to show and send: textclean::cleanToBytes (one line, invisible and reordering
// characters removed, ZWNJ/ZWJ kept between visible characters, at most maxBytes of UTF-8, whole graphemes).
QString    cleanText(const QString& text, int maxBytes);
char       kindCode(Kind kind); // 'y' 'i' 'g' 'v'; 0 for None
Kind       kindOf(char code);

// Everything of a (stored or received) announcement within the rules above, messageKey a local key.
bool isValid(const Announcement& a);

// ---- LP ---------------------------------------------------------------------------------------------

// The LP for a, in that chat, for the message with this wire ref (textreact::wireRef). An empty message
// (no type) when a breaks a rule or wouldn't fit even without title and author.
proto::Message makeMessage(const Announcement& a, proto::Scope scope, const QByteArray& wireRef);
// LP checked field by field (see above); nullopt for anything else.
std::optional<Incoming> readMessage(const proto::Message& message);

// ---- LPNO (2.4 auto-load) -----------------------------------------------------------------------------

struct Skip {
    proto::Scope scope = proto::Scope::Channel;
    QString      hash;       // m's content hash
    qint64       ageSec = 0; // m's age
    QString      urlKey;
};

// The LPNO for that link of the message with this wire ref; an empty message (no type) on bad input.
proto::Message      makeSkip(proto::Scope scope, const QByteArray& wireRef, const QString& urlKey);
// LPNO checked field by field (s c|p, m a wire ref, u a url key); nullopt for anything else.
std::optional<Skip> readSkip(const proto::Message& message);

// ---- LPX, LPXNO (2.5) ---------------------------------------------------------------------------------------

// The cap an LPX may use on a connection: 1400 on TeamSpeak's own server once live check L1 showed that such plugin
// commands pass and cost what short ones cost (longChecked); 900 everywhere else (TeaSpeak, unknown servers).
constexpr bool kLpxLongCommandsChecked = false;
constexpr int  kLpxShortCap            = 900;
int            lpxSendCap(bool teamSpeakOwn, bool longChecked = kLpxLongCommandsChecked);

char kindCodeX(Kind kind); // Image 'i', Gif 'g', Video 'v', Loop 'a', Card 'c'; 0 otherwise
Kind kindOfX(char code);
// The media file's extension fits an LPX of that kind (see above).
bool isMediaPathX(Kind kind, const QString& path);
// Everything of a (stored or received) LPX within the rules above, messageKey a local key.
bool isValidX(const Announcement& a);
// 2.5 (review fix): whether an LPX k=a is played like a GIF (downloaded by the picture rule, autoplayed, muted and
// looping). Only when it says what an honest sender's loop is: a length of 1 ms to 60 s (d) and, with its file, a file
// of at most kMaxLoopBytes (the GIF cap the sender's maker uses). Anything else a client calls a loop is a video (the
// video rule: click to play), never an automatic download and decode of any file.
constexpr qint64 kMaxLoopBytes    = 8LL * 1024 * 1024;
constexpr qint64 kMaxLoopDuration = 60LL * 1000;
bool loopTrusted(const Announcement& a);
// isValid() of an LP, isValidX() of an LPX.
bool isValidAny(const Announcement& a);

// The LPX for a, in that chat, for the message with this wire ref, shortened to fit maxBytes (see above). An empty
// message (no type) when a breaks a rule or doesn't fit even shortened.
proto::Message makeLpx(const Announcement& a, proto::Scope scope, const QByteArray& wireRef, int maxBytes);
// LPX checked field by field; nullopt for anything else. The text is cleaned (card limits of carddata.h).
std::optional<Incoming> readLpx(const proto::Message& message);

enum class SkipReason { Unknown, Nothing, CrossSite }; // r absent (or unknown) / r=n / r=x
struct SkipX {
    proto::Scope scope = proto::Scope::Channel;
    QString      hash;
    qint64       ageSec = 0;
    QStringList  urlKeys; // 1..3, distinct
    SkipReason   reason = SkipReason::Unknown;
};
constexpr int kMaxSkipKeys = 3;
// The LPXNO for those links (1..3 distinct keys; a reason only with one key); an empty message on bad input.
proto::Message       makeLpxSkip(proto::Scope scope, const QByteArray& wireRef, const QStringList& urlKeys, SkipReason reason = SkipReason::Unknown);
std::optional<SkipX> readLpxSkip(const proto::Message& message);
const char*          skipReasonName(SkipReason reason); // "-", "nothing", "cross-site"

} // namespace linkpreview

Q_DECLARE_METATYPE(linkpreview::Announcement)
