#pragma once

#include <QList>
#include <QString>

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
struct MediaLink {
    static constexpr int kProtocol = 2;

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
    QString previewFile; // remote path, e.g. "/tsmedia/previews/clip_12345.jpg"

    bool    isValid() const;
    bool    isTsMedia() const { return protocol >= 2; }
    QString remoteFile() const; // "/dir/name.ext"
    QString key() const;        // stable id used for cache + preview resources (ignores metadata)
    QString toUrl() const;
    QString toBBCode() const;   // [URL=...]fileName[/URL]

    // The preview file as a link of its own (same server/channel, size unknown). Invalid if none.
    MediaLink previewLink() const;

    static MediaLink        parse(const QString& href);
    static QList<MediaLink> findInMessage(const QString& message);
};

// The complete chat message for an upload: the link, followed (optionally) by a short note for
// people without the plugin: "— TS Media chat plugin required to view this in chat", in a grey
// (#72767d) that has 4.56:1 contrast on TeamSpeak's white chat and 3.0:1 on a dark one (#2b2d31).
// downloadUrl (optional, http(s) only) is linked from the plugin name in the note.
// Clients with the plugin hide everything after a TS Media link in that message.
QString composeChatMessage(const MediaLink& link, bool includeNotice, const QString& downloadUrl);

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
enum class MediaError { None, Permission, Password, NotFound, NotConnected, Quota, Other };

// Downloads. The title is a short fragment for inline cards and pictures (no final period, never the
// server's own text). The text explains it for the viewer and tooltips in full sentences: the cause
// and what to do. serverText (TeamSpeak's message) is only quoted for None / Other.
QString downloadErrorTitle(MediaError error);
QString downloadErrorText(MediaError error, const QString& serverText = {});

// Uploads (the toast and the chat warning), and the chat message announcing a finished upload (the
// file is already on the server then). Full sentences with a next step.
QString uploadErrorText(MediaError error, const QString& serverText = {});
QString postErrorText(MediaError error, const QString& serverText = {});
