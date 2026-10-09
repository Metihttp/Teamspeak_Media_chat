#pragma once

// 2.2 compose: the send window's rules without any widget (QtCore/QtGui only, unit-tested): which items
// can be sent and why not, the texts the window shows, the order Core posts them in, and whether the
// caption fits in one message with the first file. ComposeDialog (composedialog.h) shows them.

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include "medialink.h"
#include "videocompress.h" // 2.4 compress

namespace compose {

constexpr int kMaxItems           = 100; // files in one send window
constexpr int kCaptionCounterFrom = 250; // the "37 left" counter shows from this many characters

// Thumbnails are decoded on a worker, and only up to these sizes (memory and time). JPEG decodes
// straight to the small size, so it may be larger.
constexpr qint64 kMaxThumbnailPixels     = 40LL * 1000 * 1000;
constexpr qint64 kMaxJpegThumbnailPixels = 100LL * 1000 * 1000;
constexpr qint64 kMaxThumbnailFileBytes  = 64LL * 1024 * 1024;

// Why an item can't be sent. Checked before Send, so nothing fails right after it.
enum class Problem { None, Missing, Empty, TooLarge, Unreadable };

struct Item {
    int       id = 0;
    QString   path;            // the file; empty for a pasted picture
    QImage    image;           // the pasted picture (written to a file only when it is sent)
    QString   fileName;        // the file's name; "Pasted image.png" for a paste
    MediaKind kind = MediaKind::Other;
    qint64    size = 0;        // bytes (0 for a paste: not written yet)
    QSize     pixels;          // display size, when known
    qint64    durationMs = 0;  // videos, when known
    bool      probed     = false; // the worker has looked at it (thumbnail, size, readable)
    Problem   problem    = Problem::None;
    bool      spoiler    = false;
    // 2.2 editor: edited in the send window. path, fileName, size and pixels (image, for a paste) then
    // describe the edited copy; the window keeps the original for "Revert to original".
    bool      edited     = false;

    // 2.4 compress: a video that may be compressed. Its facts come from the window's probe; choices are the
    // planner's Quality entries (empty: nothing to choose); quality is the picked entry (-1: the default).
    bool                            compressible = false;
    bool                            factsKnown   = false;
    videocompress::VideoFacts       facts;
    QVector<videocompress::Choice>  choices;
    int                             quality = -1;
    QString                         compressProblem; // why it can't be sent even compressed (the planner's text)

    bool isPasted() const { return path.isEmpty(); }
    bool canSend() const { return problem == Problem::None; }
};

// A file as found on disk (stat only, no reading).
Problem checkFile(bool exists, bool isFile, qint64 size, qint64 limitBytes);
// "Over your 100 MB upload limit", "This file is empty", ...: a short line for the item's row.
QString problemText(Problem problem, int limitMB);

// Pictures, GIFs and videos can be marked as spoilers.
bool spoilerAllowed(MediaKind kind);
// Pictures, GIFs and videos go into albums (the same rule as Core::send).
bool isAlbumKind(const Item& item);

QString displayName(const Item& item);    // "holiday.jpg" (display-safe), "Pasted image"
QString typeText(const Item& item);       // "JPG image", "MP4 video", "ZIP archive", "Pasted image"
QString metaText(const Item& item);       // "JPG image · 2.4 MB · 4032 × 3024", "MP4 video · 0:42 · 180 MB" ("Edited · …")
QString accessibleName(const Item& item); // "holiday.jpg, JPG image, 2.4 MB, spoiler" (", edited")

int     sendableCount(const QVector<Item>& items);
// The Send button: "Send" for one item, "Send 3 images" / "Send 2 videos" / "Send 5 files" (sendable only).
QString sendButtonText(const QVector<Item>& items);
// Under the list when some items can't be sent ("2 files can't be sent and will be skipped."); empty
// when all can, or when a single item shows its own problem.
QString skippedText(const QVector<Item>& items);

// Sendable pictures and videos: "Send as an album" is offered from 2.
int albumCandidates(const QVector<Item>& items);
// How more than MediaLink::kMaxAlbumItems are split ("12 images will be sent as 2 albums (10 + 2)."),
// empty for 10 or fewer.
QString albumHint(const QVector<Item>& items);
// Indexes of the sendable items in the order Core::send posts them: with album, the pictures and videos
// first, then the rest; otherwise as listed.
QVector<int> postOrder(const QVector<Item>& items, bool album);

// "37 left", from kCaptionCounterFrom characters on; empty below.
QString captionCounterText(int length);

// Where the first message's link points, for estimating its size.
struct LinkContext {
    QString host = QStringLiteral("ts.example.com"); // as the server address is shown in links
    quint16 port = 9987;
    QString serverUid = QStringLiteral("Wn5SbAbcR9xQ0pRu7Zy3pCt+Ys0=");
    quint64 channelId = 12;
    QString remoteDir = QStringLiteral("/tsmedia");
    bool    previews  = true; // Settings::generatePreviews
    bool    notice    = true; // Settings::addRequiredNotice
    QString downloadUrl;      // Settings::pluginDownloadUrl
};

// The link the item will most likely get, with every field it may carry (an upper bound: hashes,
// BlurHash and the preview are assumed).
MediaLink estimatedLink(const Item& item, const LinkContext& context, bool inAlbum, int albumCount);
// True when the caption won't fit in the first media message and goes out as a message of its own,
// right above it (composeChatMessages decides; this asks it about the estimated first link).
bool captionGoesAlone(const QString& caption, const QVector<Item>& items, bool album, const LinkContext& context);

// ---- pictures -------------------------------------------------------------------------------------

// image cut to fill size (device pixels), centred. (Spoiler thumbnails get the receivers' cover,
// spoiler::drawCover, in the send window.)
QImage coverThumbnail(const QImage& image, const QSize& size);

// Writes a pasted picture into dir as new_photo_<8 hex>.png, or .jpg (quality 90) when the PNG is over
// 2 MB, has no transparency and convertLargePngToJpeg is on: the same rule as Core::uploadImage.
// Returns the file's path, or an empty string if it couldn't be written (nothing is left behind).
QString savePastedImage(const QImage& image, const QString& dir, bool convertLargePngToJpeg);

} // namespace compose
