#pragma once

// Sender-side inspection of a file before upload: dimensions, duration, blurhash and an optional
// small preview/poster JPEG that is uploaded next to the file.

#include <QByteArray>
#include <QImage>
#include <QString>

#include "medialink.h"

struct LocalMediaInfo {
    MediaKind kind       = MediaKind::Other;
    bool      animated   = false; // GIF / animated WebP
    int       width      = 0;     // display size after EXIF rotation
    int       height     = 0;
    qint64    durationMs = 0;     // video / audio
    QString   blurHash;
    QImage    preview;            // non-null => upload as preview/poster

    // 2.4 compress: what the compression planner needs (filled for videos).
    bool    probed        = false; // mf::probe could read the container
    bool    hasVideo      = false;
    bool    decodable     = false; // a frame could be decoded here
    double  frameRate     = 0.0;   // nominal, 0 if unknown
    bool    hasAudio      = false;
    int     audioChannels = 0;
    QString videoCodec;            // readable codec name ("HEVC (H.265)"), empty if unknown
    QString probeError;            // why it can't be decoded (mf's text)
};

// Synchronous; call from a worker thread. Never throws. Rules:
//  * still images: size + blurhash; preview (max side 1280) if generatePreviews and the file is
//    larger than 1.5 MB or its longest side exceeds 2560 px
//  * animated images: size + blurhash of the first frame; preview (first frame, max side 640) if
//    generatePreviews and the file is larger than 4 MB
//  * video: mf::probe -> size, duration, poster as preview (max side 960, always if generatePreviews), blurhash of the poster
//  * audio: duration via mf::probe
//  * anything else: kind only
LocalMediaInfo probeLocalMedia(const QString& path, bool generatePreviews);

// 2.4 compress: the video rules above for any file (camcorder .mts, phone .3gp ... are not video kinds by
// name), kind Video. For files that may be compressed.
LocalMediaInfo probeLocalVideo(const QString& path, bool generatePreviews);

// JPEG (quality 82) of an image, flattened onto black if it has alpha.
QByteArray encodePreviewJpeg(const QImage& image);
