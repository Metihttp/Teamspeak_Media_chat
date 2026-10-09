#pragma once

// Pure drawing functions for everything TS Media puts into the chat. No state, no Core access:
// callers pass the entry and the pixels to draw. All sizes are logical pixels unless noted; returned
// images have devicePixelRatio == style.dpr.

#include <QFont>
#include <QImage>
#include <QPointF>
#include <QSize>

#include "core.h"

struct PreviewStyle {
    bool  dark = false;
    QFont font;
    qreal dpr       = 1.0;
    int   maxWidth  = 400;
    int   maxHeight = 300;
};

// Controls drawn on top of an inline video.
enum class VideoZone { None = 0, Body, PlayPause, Seek, Mute, Expand };

struct PlaybackOverlay {
    bool   controlsVisible = false; // bottom bar (hover or paused)
    bool   playing         = false;
    bool   muted           = false;
    bool   ended           = false;
    bool   busy            = false; // downloading / opening: spinner instead of the play button
    double busyProgress    = -1.0;  // 0..1 download progress, < 0 = indeterminate
    qint64 positionMs      = 0;
    qint64 durationMs      = 0;
    VideoZone hover        = VideoZone::None;
};

// The size a preview occupies in the chat. When the link carries dimensions (TS Media links) this
// is the same for every state of the entry, so the chat never jumps while media loads or plays.
QSize previewLogicalSize(const MediaEntry& entry, const PreviewStyle& style);

// Static preview: picture (full / preview / blurhash still), video poster with a play button and
// duration badge, or a Discord-like attachment card (icon, name, size, status, progress, errors).
QImage renderPreview(const MediaEntry& entry, const MediaStill& still, const PreviewStyle& style, QSize* logicalSize);

// A frame of an animated image (GIF / animated WebP) with rounded corners and a small GIF badge.
QImage renderAnimatedFrame(const MediaEntry& entry, const QImage& frame, const PreviewStyle& style, QSize* logicalSize);

// A video frame (or the poster when frame is null) with the playback overlay.
QImage renderVideo(const MediaEntry& entry, const QImage& frame, const MediaStill& poster, const PlaybackOverlay& overlay, const PreviewStyle& style, QSize* logicalSize);

// Hit testing for renderVideo() output of the given logical size; pos relative to its top-left.
VideoZone videoZoneAt(const QSize& logicalSize, const QPointF& pos);
double    seekFractionAt(const QSize& logicalSize, const QPointF& pos); // 0..1 along the seek bar

// A file name from a chat link (anyone can post one) prepared for display as plain text: control
// characters and bidi embedding/override/isolate marks are removed, so a name cannot reorder or
// hide parts of what is shown (e.g. fake its extension). Not for paths or keys.
QString displayFileName(const QString& name);
