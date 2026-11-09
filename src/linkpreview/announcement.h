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

#include <QByteArray>
#include <QString>

#include <optional>

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
// Text from a website made safe to show and send: C0/C1 controls, bidi marks and invisible characters
// removed, whitespace runs become one space, trimmed, at most maxBytes of UTF-8 (whole characters).
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

} // namespace linkpreview

Q_DECLARE_METATYPE(linkpreview::Announcement)
