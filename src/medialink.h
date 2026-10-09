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
// people without the plugin, e.g. "— TS Media chat plugin required to view this in chat".
// The note is written in the sender's UI language; downloadUrl (optional) is linked from it.
// Clients with the plugin hide everything after a TS Media link in that message.
QString composeChatMessage(const MediaLink& link, bool includeNotice, const QString& downloadUrl);

enum class MediaKind { Image, AnimatedImage, Video, Audio, Archive, Document, Other };

MediaKind kindForFileName(const QString& fileName);
bool      isPreviewableImage(MediaKind kind);
QString   formatSize(quint64 bytes);
QString   formatDuration(qint64 ms); // "0:07", "12:34", "1:02:03"
