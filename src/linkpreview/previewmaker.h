#pragma once

// 2.4 link previews, the sender's side: the files a fetched link becomes (run on a worker thread). Website
// files are untrusted: the format is taken from the bytes (never from the link or the server's type), every
// picture is decoded within Core's limits (80 megapixels, 16384 pixels a side) and the preview is always a
// new picture encoded here (nothing of the original's metadata goes along).
//
//  * YouTube: the oEmbed thumbnail cut to 16:9 (hqdefault's black bars go), at most thumbMaxSide, as JPEG;
//    the title and channel cleaned.
//  * Images: a preview of at most previewMaxSide (JPEG; PNG for a picture with transparency when only the
//    preview goes up), and the file itself when it is at most imageMediaBytes.
//  * GIFs: a still of the first frame, and the GIF itself when it is at most gifMediaBytes.
//  * Videos: a poster frame and the duration (mediaprobe), and the video itself up to videoMediaBytes.

#include <QByteArray>
#include <QRect>
#include <QString>

#include <atomic>
#include <functional>

#include "fetcher.h"
#include "linkurl.h"
#include "mediaprobe.h"

namespace linkpreview::maker {

struct Caps {
    qint64 imageMediaBytes = 15LL * 1024 * 1024; // the plugin's limits: automatic downloads and uploads
    qint64 gifMediaBytes   = 8LL * 1024 * 1024;
    qint64 videoMediaBytes = 25LL * 1024 * 1024;
    int    previewMaxSide  = 1280; // pictures
    int    thumbMaxSide    = 640;  // YouTube thumbnails, GIF stills
    int    posterMaxSide   = 960;  // video posters
};

// What the bytes are.
enum class Format { Unknown, Png, Jpeg, Gif, Webp, Mp4, Mov, WebM };
Format  sniff(const QByteArray& head); // the first 32 bytes are enough
Format  sniffFile(const QString& path);
QString extensionOf(Format format); // "png", ... ("" for Unknown)

struct Input {
    Target  target;
    Result  fetched;
    QString workDir; // the files go here (it exists)
    bool    mayMove = true; // the fetched files are ours to move (else they are copied)
    Caps    caps;
    // The video probe (mediaprobe's probeLocalVideo with a poster); tests put a fake in its place.
    std::function<LocalMediaInfo(const QString& path)> probeVideo;
};

struct Made {
    bool       ok = false;
    QString    error; // for the log (never a URL)
    Kind       kind = Kind::None; // what the bytes turned out to be (an "image" link to a GIF is a GIF)
    QString    previewFile;  // <workDir>/linkpreview.jpg (or .png)
    QByteArray previewBytes; // its content
    QByteArray previewSha;   // its SHA-256
    QString    mediaFile;    // <workDir>/link.<ext>; empty: only the preview goes up
    qint64     mediaSize = 0;
    int        width = 0, height = 0;
    qint64     durationMs = 0;
    QString    title, author;
};

// Worker thread. cancel may be null. Never throws.
Made make(const Input& input, const std::atomic<bool>* cancel = nullptr);

// The 16:9 band in the middle of a picture taller than 16:9 (YouTube's 4:3 thumbnails), else all of it.
QRect crop16x9(const QSize& size);

} // namespace linkpreview::maker
