#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

// TeamSpeak's limits for one chat message, measured by the S0 spike (TeamSpeak 3.6.2, server 3.13):
// the server takes up to 8192 UTF-8 bytes of the text (above: 0x0605), and the client silently drops a
// command whose escaped form is above about 9.1 KB (no answer, nothing delivered). The escaping is
// ServerQuery style: \ / space | and the control characters \a \b \f \n \r \t \v take two bytes.
// Every composed message keeps escapedMessageSize(text) <= kMaxMessageBytes (8192 - 24), which keeps
// both limits for every script. The one place that decides it: the composer, Core and the tests all
// use this constant.
constexpr int kMaxMessageBytes = 8168;

// The size of a message in TeamSpeak's command: its UTF-8 bytes, escaping included.
int escapedMessageSize(const QString& text);

// Between the caption and the first link, and between links packed into one message. S0: TeamSpeak
// shows '\n' as a line break inside the same message ("\r\n" would add a space).
constexpr char kMessageSeparator[] = "\n";

// A file in a channel's file browser, encoded the same way the TeamSpeak client encodes
// files dragged from the file browser into the chat: [URL=ts3file://host?...]name[/URL]
// Clients without the plugin can still click the link and download the file natively.
//
// TS Media adds extra query parameters (ignored by TeamSpeak itself):
//   tsm=<protocol>  marks a TS Media message (current protocol: 2)
//   w, h            media size in pixels (display orientation)
//   d               duration in milliseconds (video / audio)
//   bh              BlurHash placeholder
//   pv              remote path of a small JPEG preview / video poster in the same channel
// 2.2 adds these, always after pv and in this order (only on links with tsm >= 2; see dropInvalidMetadata):
//   sha             SHA-256 of the file: 43 base64url characters, no padding, canonical last character
//   ph              first 16 bytes of the preview's SHA-256: 22 base64url characters; only with a valid pv
//   sp=1            spoiler: pictures, GIFs and videos only
//   al, ai, an      album: id (8 lower-case hex, never 00000000), position 1..an, size 2..10; all or none
//   vm=1            voice message: audio only
//   wf              waveform of a voice message: 64 levels of 4 bits = 32 bytes, 43 base64url characters
// Receivers ignore unknown parameters, the first occurrence of a name wins, and an invalid value drops
// only its own field (never the link). Links without these fields are byte-identical to 2.1's.
struct MediaLink {
    static constexpr int kProtocol      = 2;
    static constexpr int kMaxAlbumItems = 10; // per album; larger sends become several albums
    static constexpr int kWaveformLevels = 64;
    static constexpr int kWaveformMax    = 15; // levels are 0..15 (4 bits)

    QString host;
    quint16 port = 0;
    QString serverUid;
    quint64 channelId = 0;
    QString path = QStringLiteral("/"); // directory inside the channel file browser
    QString fileName;
    quint64 size     = 0;
    qint64  dateTime = 0;
    bool    isDir    = false;

    // TS Media metadata (all optional)
    int     protocol   = 0; // 0 = plain TeamSpeak link
    int     width      = 0;
    int     height     = 0;
    qint64  durationMs = 0;
    QString blurHash;
    QString previewFile; // remote path, e.g. "/tsmedia/previews/3f9a1c2e.jpg"

    // 2.2 metadata (all optional; see above)
    QByteArray sha256;             // 32 bytes, or empty
    QByteArray previewSha;         // 16 bytes, or empty
    bool       spoiler    = false;
    quint32    albumId    = 0;     // 0 = not in an album
    int        albumIndex = 0;     // 1..albumCount
    int        albumCount = 0;     // 2..kMaxAlbumItems
    bool       voice      = false;
    QByteArray waveform;           // kWaveformLevels bytes of 0..kWaveformMax, or empty

    bool    isValid() const;
    bool    isTsMedia() const { return protocol >= 2; }
    bool    hasAlbum() const { return albumId != 0 && albumCount >= 2; }
    QString remoteFile() const; // "/dir/name.ext"
    // Stable id for the cache, chat resources, reactions and SYNC lists. Without a sha it is identical
    // to v1 (cache names and documents from 2.1 keep working); with one, "\n" + base64url(sha) is part
    // of it, so a link with a forged hash can neither reuse nor poison the genuine file's entry. Frozen
    // from 2.2 on. Ignores every other metadata field.
    QString key() const;
    QString toUrl() const;
    QString toBBCode() const;                     // [URL=...]fileName[/URL] (2.1's label)
    QString toBBCode(const QString& label) const; // [URL=...]label[/URL]; brackets in the label become ( )

    // Clears the 2.2 fields a receiver would ignore: wrong sizes, a spoiler or voice flag on the wrong
    // kind of file, a preview hash without a preview, an incomplete album, a waveform without the
    // voice flag, and all of them on links that aren't TS Media (tsm < 2). parse() and toUrl() apply it.
    void dropInvalidMetadata();

    // The preview file as a link of its own (same server/channel, size unknown). Invalid if none.
    MediaLink previewLink() const;

    static MediaLink parse(const QString& href);
    // Every valid file link in a raw chat message (BBCode) that TeamSpeak shows as a link. An opening tag
    // escaped with a backslash ("\[URL=", the way captions are escaped) or broken by an invisible
    // character ("[" U+200B "URL=", U+2060) is plain text in TeamSpeak's chat, so it is never fetched
    // either: a caption can't smuggle in a link. ([noparse] is no protection: TeamSpeak 3.6 drops the
    // tags and still parses what is inside.)
    static QList<MediaLink> findInMessage(const QString& message);
};

// The complete chat message for an upload: the link, followed (optionally) by a short note for
// people without the plugin: "— TS Media chat plugin required to view this in chat", in a grey
// (#72767d) that has 4.56:1 contrast on TeamSpeak's white chat and 3.0:1 on a dark one (#2b2d31).
// downloadUrl (optional, http(s) only) is linked from the plugin name in the note.
// Clients with the plugin hide everything after a TS Media link in that message.
// 2.1's composer, kept as is (the file name as label, 2.1's limit of 1000 bytes) for the compatibility
// tests: composeChatMessages() with one link, no caption, friendlyLabels off and maxBytes 1000 gives the
// same text. New code uses composeChatMessages().
QString composeChatMessage(const MediaLink& link, bool includeNotice, const QString& downloadUrl);

// ---- 2.2 composer: captions, several links per message, one cascade ------------------------------
//
// Messages: [caption kMessageSeparator] LINK (kMessageSeparator LINK)* [note], then LINK (sep LINK)*.
// The caption goes before the links (2.1 receivers delete text after a TS Media link), the note once,
// after the last link of the first media message. Each message holds as many complete links as fit
// below maxBytes; packing never drops metadata to fit more links.
// When a link doesn't fit, optional metadata is dropped in one order: ph, bh, wf, the note's link,
// [then the caption moves to a message of its own, right above the media], the note, pv, w/h/d, sha.
// TeamSpeak's own parameters, tsm, sp, al/ai/an and vm are never dropped, and the caption is never cut
// (it is at most kCaptionMaxChars). Nothing left to drop: the message is sent anyway (tooLong).
struct ComposeOptions {
    QString caption;               // as typed: sanitizeCaption() and captionToBBCode() run here
    bool    includeNotice  = true; // the note for people without the plugin
    QString downloadUrl;           // linked from the note (http(s) only)
    int     maxBytes       = kMaxMessageBytes; // escapedMessageSize() of each message is at most this
    bool    friendlyLabels = true;  // linkLabel(); false: the remote file name, like 2.1
    bool    legacySize     = false; // 2.1's rule instead: UTF-8 bytes, strictly below maxBytes (composeChatMessage)
};

struct ComposedMessage {
    QString      text;
    QVector<int> links;       // indexes of the links it carries; empty for the caption on its own
    QStringList  dropped;     // what was left out to fit: "ph", "bh", "wf", "note link", "note", "pv", "w/h/d", "sha"
    bool         tooLong = false; // still at or above maxBytes with nothing left to drop
};

QVector<ComposedMessage> composeChatMessagesDetailed(const QList<MediaLink>& links, const ComposeOptions& options);
QStringList              composeChatMessages(const QList<MediaLink>& links, const ComposeOptions& options);

// ---- captions -------------------------------------------------------------------------------------

constexpr int kCaptionMaxChars = 300; // UTF-16 units, as the send window counts them

// One line of the sender's text made safe to post: C0/C1 controls and bidi marks removed, every run of
// whitespace (line breaks too) becomes one space, trimmed, at most kCaptionMaxChars (never half of a
// surrogate pair).
QString sanitizeCaption(const QString& typed);

// Text that TeamSpeak shows as written: a backslash before every [ and ] (S0: the chat and printMessage
// remove the backslash run before a bracket and show the bracket). A backslash of the text right before
// a bracket is lost that way; nothing else changes, and no invisible characters are added.
QString bbcodeLiteral(const QString& text);

// A sanitized caption as BBCode: bbcodeLiteral(), so a caption can never open a tag or fake a file
// link. Web addresses need no [URL]: TeamSpeak links them on display.
QString captionToBBCode(const QString& sanitized);

// The label of a link in the chat (what people without the plugin click): "Voice message (0:12)" for
// voice messages, "Spoiler (image)" / "Spoiler (GIF)" / "Spoiler (video)" for spoilers (the name would
// give it away), otherwise displayNameFor(). Brackets become parentheses.
QString linkLabel(const MediaLink& link);

// ---- remote names (2.2) ---------------------------------------------------------------------------

constexpr int kRemoteBaseMaxChars = 48; // UTF-16 units
constexpr int kRemoteBaseMaxBytes = 64; // UTF-8

// The base of an uploaded file's name (before "_<8 hex>.<ext>") cut to kRemoteBaseMaxChars and
// kRemoteBaseMaxBytes at a code-point boundary: Persian names keep 32 letters, CJK 21, emoji 16.
// Lone surrogates are dropped.
QString boundRemoteBase(const QString& base);

// The random part of a name made by Core: "holiday_3f9a1c2e.jpg" -> "3f9a1c2e"; empty if there is none.
QString remoteSuffixOf(const QString& remoteName);

enum class MediaKind { Image, AnimatedImage, Video, Audio, Archive, Document, Other };

MediaKind kindForFileName(const QString& fileName);
bool      isPreviewableImage(MediaKind kind);

// ---- names ---------------------------------------------------------------------------------------

// A file name from a chat link (anyone can post one) prepared for display as plain text: control
// characters and bidi embedding/override/isolate marks are removed, so a name cannot reorder or
// hide parts of what is shown (e.g. fake its extension). Not for paths or keys.
QString displayFileName(const QString& name);

// The name to show for a link's file: card and viewer titles, tooltips, the Save as suggestion.
// TS Media uploads (link.protocol > 0) end in a random "_3f9a1c2e" that keeps names apart on the
// server; it is left out ("holiday_3f9a1c2e.jpg" -> "holiday.jpg"), and a pasted picture
// ("new_photo_<hex>.png") is shown as "Pasted image.png". Plain TeamSpeak links keep their name.
// Always passed through displayFileName(). Keys, cache paths, "Copy link" and anything sent to the
// server keep using link.fileName.
QString displayNameFor(const MediaLink& link);

// 2.2 voice: a voice message (vm=1) is "Voice message.m4a" in displayNameFor(), whatever its file is
// called; Save as suggests "Voice message 2026-10-09 18-02.m4a" (the link's time, local; without one
// "Voice message.m4a").
QString voiceSaveName(const MediaLink& link);

// ---- numbers -------------------------------------------------------------------------------------

// "512 B", "1.0 KB", "24.1 KB", "700 MB", "1.5 GB": 1024-based, one decimal below 100 (so the width
// stays steady while a number grows) and none from 100 on; never four digits ("1023.9 KB" -> "1.0 MB").
QString formatSize(quint64 bytes);
QString formatDuration(qint64 ms); // "0:07", "12:34", "1:02:03"

// Progress texts; every surface puts them in the same order: verb, percent, rate, amounts, time left
// ("Uploading… 45% · 1.2 MB/s · 4.5 MB of 10.0 MB"), so a cut-off line loses the least useful part.
QString formatProgress(quint64 done, quint64 total); // "4.5 MB of 10.0 MB" (done is capped at total)
QString formatSpeed(double bytesPerSecond);          // "1.2 MB/s"
QString formatTimeLeft(qint64 ms);                   // "8 s left", "3 min left", "1 h 5 min left"

// ---- transfer errors: one wording per cause, shared by every surface -----------------------------

// Why a transfer failed (MediaEntry::error for downloads). Core maps TeamSpeak's error codes to it.
// 2.2 sha: Mismatch, the downloaded file isn't the one its link's SHA-256 names (deleted, final: a new
// download gets the same bytes). New values go at the end (diagnostics count them by number).
enum class MediaError { None, Permission, Password, NotFound, NotConnected, Quota, Other, Mismatch };

// Downloads. The title is a short fragment for inline cards and pictures (no final period, never the
// server's own text). The text explains it for the viewer and tooltips in full sentences: the cause
// and what to do. serverText (TeamSpeak's message) is only quoted for None / Other.
QString downloadErrorTitle(MediaError error);
QString downloadErrorText(MediaError error, const QString& serverText = {});

// Uploads (the toast and the chat warning), and the chat message announcing a finished upload (the
// file is already on the server then). Full sentences with a next step.
QString uploadErrorText(MediaError error, const QString& serverText = {});
QString postErrorText(MediaError error, const QString& serverText = {});
