#pragma once

// Pure drawing functions for everything TS Media puts into the chat. No state, no Core access:
// callers pass the entry and the pixels to draw. All sizes are logical pixels unless noted; returned
// images have devicePixelRatio == style.dpr.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>

#include "core.h"

struct PreviewStyle {
    bool  dark = false;
    QFont font;
    qreal dpr       = 1.0;
    int   maxWidth  = 400;
    int   maxHeight = 300;

    // Per preview: pointer feedback (ChatIntegration sets them for the preview under the mouse).
    bool hovered = false; // the pointer is over it
    bool pressed = false; // the left button went down on it and is still held there
    // Windows "Show animations" (ui::animationsEnabled()). Off: busy rings are drawn as a static
    // waiting glyph instead of a spinner.
    bool animate = true;
    // A Ready file that only opens its folder when clicked (Core::isUnsafeToOpen: programs, scripts).
    bool revealOnly = false;
    // 2.2 spoiler: how much of the spoiler cover lies over a picture, GIF or video (spoiler.h): 1 while
    // it is hidden, falling to 0 during the reveal crossfade, 0 otherwise. The box never changes with it.
    // Cards of other files ignore it; a picture shown as a card (no size known) is named "Spoiler (image)".
    qreal concealOpacity = 0.0;
};

// Controls drawn on top of an inline video.
enum class VideoZone { None = 0, Body, PlayPause, Seek, Mute, Expand };

struct PlaybackOverlay {
    bool   controlsVisible = false; // bottom bar (hover or paused)
    double controlsOpacity = 1.0;   // < 1 while the bar fades out under a resting pointer
    bool   playing         = false;
    bool   muted           = false;
    bool   ended           = false;
    bool   busy            = false; // downloading / opening: spinner instead of the play button
    double busyProgress    = -1.0;  // 0..1 download progress, < 0 = indeterminate
    bool   externalOnly    = false; // can't be played inline: a click opens it in the default app
    qint64 positionMs      = 0;
    qint64 durationMs      = 0;
    VideoZone hover        = VideoZone::None;
    VideoZone pressed      = VideoZone::None; // the left button is held on this zone
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
// centerButton: the round button in the middle is drawn (the video is not playing); it then wins
// over the seek bar where they meet on small players.
VideoZone videoZoneAt(const QSize& logicalSize, const QPointF& pos, bool centerButton);
QRectF    videoZoneRect(const QSize& logicalSize, const QPointF& pos, bool centerButton); // the zone at pos (tooltips)
double    seekFractionAt(const QSize& logicalSize, const QPointF& pos); // 0..1 along the seek bar

// A failed download that a click (or "Retry download") can fix. Not for files that are gone from
// the server or in password-protected channels: those previews do nothing when clicked.
bool isRetryableDownload(const MediaEntry& entry);
// A left click on the preview does something (download, open, retry, play).
bool isPreviewActionable(const MediaEntry& entry);

// What a preview says about its state, in full (cards shorten it to fit): "Click to download",
// "Waiting to download…", "Downloading… 45% · 4.5 MB of 10.0 MB", "Click to open", the error title.
// Without the file size. cannotPreview: a Ready picture that can't be decoded.
QString previewStatusText(const MediaEntry& entry, bool cannotPreview, bool revealOnly = false);

// ---- 2.2 sha: a downloaded file being checked against its link's SHA-256 ---------------------------
// True while a preview says "Checking file…": Core checks the downloaded file and it has taken a moment
// (MediaEntry::check.shown; quicker checks never show, so small files don't flash it).
bool isCheckingShown(const MediaEntry& entry);
// The download progress previews draw: while a large file (over 256 MB) is checked, the check's own,
// which starts at 0 again (checkingText says so with its percentage); while a smaller one is, 1.0;
// otherwise entry.progress.
double shownDownloadProgress(const MediaEntry& entry);
// "Checking file…", "Checking file… 42%" (files over 256 MB), "Checking file again…" (the second pass
// after a first one that didn't match).
QString checkingText(const MediaEntry& entry);
// What a failed preview says under its title: "Click to retry" where retrying can help, "Ask the sender
// to send it again" for a file that doesn't match what was sent, else nothing.
QString errorHint(const MediaEntry& entry);

// Text colours with the background they are drawn on (translucent layers already flattened), and
// the contrast each needs: tools/render_gallery prints them so a regression shows up.
struct PreviewColorPair {
    QString name;
    QColor  foreground;
    QColor  background;
    double  minimum = 4.5; // 4.5 for text, 3.0 for icons, rings and bars
};
QVector<PreviewColorPair> previewColorPairs(bool dark);

// displayFileName() and displayNameFor() are in medialink.h.

// 2.2 drag-out: the attachment cards' file-type glyph (coloured page with the extension), drawn into
// rect (32 x 40 on a card), for the mini card that follows the pointer (dragpixmap.cpp).
class QPainter;
void drawFileTypeGlyph(QPainter& p, const QRectF& rect, const MediaEntry& entry, const PreviewStyle& style);

// ---- 2.2 album grid (the geometry is albums::layout(), shared with the hit test) ------------------

// One item of an album as the grid draws it.
struct AlbumTile {
    const MediaEntry* entry = nullptr; // nullptr: the item hasn't arrived yet (a placeholder tile)
    MediaStill        still;           // requested at albumTileStillPixels(): it covers the tile
    QImage            frame;           // the current GIF frame while it animates (device pixels), else null
    bool              concealed = false; // a spoiler not revealed yet (Core::isSpoilerHidden): the cover, never the sharp picture
    qreal             concealOpacity = 0.0; // 2.2 spoiler: the reveal crossfade (1 .. 0) over a revealed tile
    bool              hovered   = false;
    bool              pressed   = false;
};

// The size of an album of `items` in the chat: albums::layout() for the style's limits. Only the number
// of items decides it, so the grid keeps its size while its pictures load.
QSize  albumLogicalSize(int items, const PreviewStyle& style);
// The whole grid. tiles: one per item of the album, in order; the items past the grid's last tile are
// not drawn, and that tile says "+N".
QImage renderAlbum(const QVector<AlbumTile>& tiles, const PreviewStyle& style, QSize* logicalSize);
// Device pixels to ask Core::still() for: the picture scaled to cover a tile of that logical size
// (from the link's dimensions), so a tile is never blurry and nothing larger is decoded.
QSize  albumTileStillPixels(const MediaEntry& entry, const QSize& tile, qreal dpr);
// The album's text colours with what they are drawn on (tools/render_gallery checks them).
QVector<PreviewColorPair> albumColorPairs(bool dark);
