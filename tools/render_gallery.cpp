// render_gallery <outdir> [<media dir>]
//
// Renders every state of the chat previews (previewrenderer.cpp) without TeamSpeak or a Core
// instance: entries and stills are built by hand the way Core would produce them. Writes one PNG
// per state into <outdir>/<theme>_<dpr>x/ and a labelled contact sheet per theme and dpr
// (<outdir>/sheet_<theme>_<dpr>x.png). Also prints the contrast of every text colour against what
// it is drawn on (previewColorPairs, also written to <outdir>/contrast.txt) and fails if one is too
// low.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QTextStream>
#include <QtMath>

#include <functional>

#include "audiocard.h"
#include "albums.h" // 2.2 album
#include "blurhash.h"
#include "dragpixmap.h" // 2.2 drag-out
#include "voicecard.h" // 2.2 voice
#include "emojidata.h"   // 2.2 emoji
#include "emojirender.h" // 2.2 emoji
#include "previewrenderer.h"
#include "reactionart.h" // 2.2 reactions
#include "replyart.h"   // 2.2 reply
#include "uiutil.h"

namespace {

// Example hash from the BlurHash reference implementation (https://github.com/woltapp/blurhash).
const QString kReferenceHash = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj");

QString g_mediaDir;
int     g_layoutFailures = 0; // samples whose box differs from what the chat reserves (a layout jump)
int     g_layoutErrors = 0; // 2.2 album: grids whose size depended on what their tiles show

QImage loadSource(const QString& fileName)
{
    static QHash<QString, QImage> cache;
    auto                          it = cache.find(fileName);
    if (it != cache.end())
        return it.value();
    QImageReader reader(g_mediaDir + QLatin1Char('/') + fileName);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        QTextStream(stderr) << "warning: cannot read " << reader.fileName() << ": " << reader.errorString() << "\n";
    else
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    cache.insert(fileName, image);
    return image;
}

QImage fitScaled(const QImage& image, const QSize& maxPixels)
{
    if (image.isNull() || maxPixels.isEmpty())
        return {};
    const QSize target = image.size().scaled(maxPixels, Qt::KeepAspectRatio).boundedTo(image.size());
    return image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Crop to an aspect ratio (used to fake video posters from photos).
QImage cropToAspect(const QImage& image, qreal aspect)
{
    if (image.isNull())
        return {};
    QRect r = image.rect();
    if (static_cast<qreal>(r.width()) / r.height() > aspect) {
        const int w = qRound(r.height() * aspect);
        r           = QRect((r.width() - w) / 2, 0, w, r.height());
    } else {
        const int h = qRound(r.width() / aspect);
        r           = QRect(0, (r.height() - h) / 2, r.width(), h);
    }
    return image.copy(r);
}

QString hashOf(const QImage& image)
{
    return blurhash::encode(image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation), 4, 3);
}

MediaEntry makeEntry(const QString& fileName, quint64 size, int width, int height, qint64 durationMs, MediaState state)
{
    MediaEntry e;
    e.link.host       = QStringLiteral("127.0.0.1");
    e.link.port       = 9987;
    e.link.serverUid  = QStringLiteral("galleryServerUid=");
    e.link.channelId  = 1;
    e.link.path       = QStringLiteral("/tsmedia");
    e.link.fileName   = fileName;
    e.link.size       = size;
    e.link.protocol   = width > 0 ? MediaLink::kProtocol : 0;
    e.link.width      = width;
    e.link.height     = height;
    e.link.durationMs = durationMs;
    e.kind            = kindForFileName(fileName);
    e.state           = state;
    e.progress        = state == MediaState::Ready ? 1.0 : 0.0;
    return e;
}

MediaEntry failed(MediaEntry e, MediaError error, const QString& text = {})
{
    e.state     = MediaState::Failed;
    e.error     = error;
    e.errorText = text;
    return e;
}

MediaEntry downloading(MediaEntry e, double progress)
{
    e.state    = MediaState::Downloading;
    e.progress = progress;
    return e;
}

// The pixel size ChatIntegration asks Core for.
QSize stillPixels(const MediaEntry& e, const PreviewStyle& style)
{
    if (e.link.width > 0 && e.link.height > 0)
        return previewLogicalSize(e, style) * style.dpr;
    return QSize(style.maxWidth, style.maxHeight) * style.dpr;
}

MediaStill pictureStill(const QImage& source, MediaStill::Source kind, const QSize& maxPixels)
{
    MediaStill s;
    s.image  = fitScaled(source, maxPixels);
    s.source = s.image.isNull() ? MediaStill::None : kind;
    return s;
}

// Like Core::still for BlurHash: decode small at the link's aspect ratio, smooth-scale up.
MediaStill hashStill(const QString& hash, int width, int height, const QSize& maxPixels)
{
    MediaStill s;
    const int  w     = 32;
    const int  h     = qBound(4, qRound(32.0 * height / qMax(1, width)), 256);
    const QImage small = blurhash::decode(hash, QSize(w, h));
    if (small.isNull())
        return s;
    const QSize target = QSize(width, height).scaled(maxPixels, Qt::KeepAspectRatio);
    s.image            = small.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    s.source           = MediaStill::BlurHash;
    return s;
}

QImage gifFrame(int index, const QSize& maxPixels)
{
    QImageReader reader(g_mediaDir + QStringLiteral("/funny.gif"));
    QImage       frame;
    for (int i = 0; i <= index && reader.canRead(); ++i)
        frame = reader.read();
    if (frame.isNull())
        frame = loadSource(QStringLiteral("funny.gif"));
    return fitScaled(frame.convertToFormat(QImage::Format_ARGB32_Premultiplied), maxPixels);
}

struct Sample {
    QString                                                  name;
    QString                                                  label;
    int                                                      maxWidth = 400;
    std::function<QImage(const PreviewStyle&, QSize*)>        render;
};

// Pointer and system states that ChatIntegration sets per preview.
PreviewStyle hovered(PreviewStyle st, bool pressed = false)
{
    st.hovered = true;
    st.pressed = pressed;
    return st;
}

PreviewStyle withChatFont(PreviewStyle st, qreal pointSize)
{
    st.font.setPointSizeF(pointSize);
    return st;
}

QList<Sample> buildSamples()
{
    QList<Sample> list;
    auto add = [&list](const QString& name, const QString& label, std::function<QImage(const PreviewStyle&, QSize*)> fn, int maxWidth = 400) {
        Sample s;
        s.name     = name;
        s.label    = label;
        s.maxWidth = maxWidth;
        s.render   = std::move(fn);
        list.append(s);
    };

    const QImage  photo     = loadSource(QStringLiteral("big_photo.jpg"));
    const QImage  sunset    = loadSource(QStringLiteral("sunset_photo.png"));
    const QImage  friendPic = loadSource(QStringLiteral("from_friend.jpg"));
    const QImage  gif       = loadSource(QStringLiteral("funny.gif"));
    const QString photoHash = hashOf(photo);
    const QString gifHash   = hashOf(gif);

    // ---- images ----------------------------------------------------------------------------
    const MediaEntry photoReady = [&] {
        MediaEntry e     = makeEntry(QStringLiteral("big_photo.jpg"), 1232754, 4000, 3000, 0, MediaState::Ready);
        e.link.blurHash  = photoHash;
        return e;
    }();

    add(QStringLiteral("image_ready"), QStringLiteral("image ready (full)"), [=](const PreviewStyle& st, QSize* ls) {
        return renderPreview(photoReady, pictureStill(photo, MediaStill::Full, stillPixels(photoReady, st)), st, ls);
    });
    add(QStringLiteral("image_blurhash_loading"), QStringLiteral("blurhash + loading 35%"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = downloading(photoReady, 0.35);
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_reference_hash_queued"), QStringLiteral("reference blurhash, queued"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = makeEntry(QStringLiteral("holiday.jpg"), 845221, 1600, 1200, 0, MediaState::Queued);
        return renderPreview(e, hashStill(kReferenceHash, 1600, 1200, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_preview_only"), QStringLiteral("big image: preview only (24.6 MB)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e      = makeEntry(QStringLiteral("huge_panorama.jpg"), 25794560, 4000, 3000, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_blurhash_idle"), QStringLiteral("blurhash only, not downloaded"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e      = makeEntry(QStringLiteral("scan.png"), 31457280, 4000, 3000, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_preview_downloading"), QStringLiteral("big image downloading 70% over preview"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = downloading(makeEntry(QStringLiteral("huge_panorama.jpg"), 25794560, 4000, 3000, 0, MediaState::Idle), 0.7);
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_failed_notfound"), QStringLiteral("image failed: deleted"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::NotFound);
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_failed_permission"), QStringLiteral("image failed: permission (preview)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::Permission);
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_cannot_preview"), QStringLiteral("ready but cannot decode"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("broken.webp"), 2048000, 1920, 1080, 0, MediaState::Ready);
        return renderPreview(e, MediaStill(), st, ls);
    });
    add(QStringLiteral("image_sunset_ready"), QStringLiteral("png ready"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("sunset_photo.png"), 34867, 900, 560, 0, MediaState::Ready);
        return renderPreview(e, pictureStill(sunset, MediaStill::Full, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_tall"), QStringLiteral("very tall image (1:6) with backdrop"), [=](const PreviewStyle& st, QSize* ls) {
        const QImage     tall = cropToAspect(friendPic, 1.0 / 6.0);
        const MediaEntry e    = makeEntry(QStringLiteral("receipt.jpg"), 120000, tall.width(), tall.height(), 0, MediaState::Ready);
        return renderPreview(e, pictureStill(tall, MediaStill::Full, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_tiny"), QStringLiteral("tiny 48x48 image"), [=](const PreviewStyle& st, QSize* ls) {
        const QImage     tiny = sunset.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const MediaEntry e    = makeEntry(QStringLiteral("emoji.png"), 3100, 48, 48, 0, MediaState::Ready);
        return renderPreview(e, pictureStill(tiny, MediaStill::Full, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_plain_link"), QStringLiteral("plain TS link (no dims) ready"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("from_friend.jpg"), 51934, 0, 0, 0, MediaState::Ready);
        return renderPreview(e, pictureStill(friendPic, MediaStill::Full, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("image_idle_hover"), QStringLiteral("blurhash idle, hover"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e      = makeEntry(QStringLiteral("scan.png"), 31457280, 4000, 3000, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), hovered(st), ls);
    });
    add(QStringLiteral("image_idle_pressed"), QStringLiteral("blurhash idle, pressed"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e      = makeEntry(QStringLiteral("scan.png"), 31457280, 4000, 3000, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), hovered(st, true), ls);
    });
    add(QStringLiteral("image_cannot_preview_hover"), QStringLiteral("cannot decode, hover"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("broken.webp"), 2048000, 1920, 1080, 0, MediaState::Ready);
        return renderPreview(e, MediaStill(), hovered(st), ls);
    });
    add(QStringLiteral("image_failed_permission_hover"), QStringLiteral("failed: permission, hover"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::Permission);
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), hovered(st), ls);
    });
    add(QStringLiteral("image_failed_password"), QStringLiteral("failed: password (no retry, hover ignored)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::Password);
        return renderPreview(e, pictureStill(fitScaled(sunset, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), hovered(st), ls);
    });
    add(QStringLiteral("image_failed_no_still"), QStringLiteral("failed, nothing behind it (not connected)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(makeEntry(QStringLiteral("holiday.jpg"), 845221, 1600, 1200, 0, MediaState::Idle), MediaError::NotConnected);
        return renderPreview(e, MediaStill(), st, ls);
    });
    add(QStringLiteral("image_queued_small"), QStringLiteral("queued, small picture (160x120)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = makeEntry(QStringLiteral("holiday.jpg"), 845221, 160, 120, 0, MediaState::Queued);
        return renderPreview(e, hashStill(kReferenceHash, 160, 120, stillPixels(e, st)), st, ls);
    });

    // ---- GIFs ------------------------------------------------------------------------------
    const MediaEntry gifReady = [&] {
        MediaEntry e    = makeEntry(QStringLiteral("funny.gif"), 387333, 480, 270, 0, MediaState::Ready);
        e.link.blurHash = gifHash;
        return e;
    }();
    add(QStringLiteral("gif_still"), QStringLiteral("GIF still (first frame)"), [=](const PreviewStyle& st, QSize* ls) {
        return renderPreview(gifReady, pictureStill(gif, MediaStill::Full, stillPixels(gifReady, st)), st, ls);
    });
    add(QStringLiteral("gif_frame"), QStringLiteral("GIF animation frame"), [=](const PreviewStyle& st, QSize* ls) {
        return renderAnimatedFrame(gifReady, gifFrame(12, stillPixels(gifReady, st)), st, ls);
    });
    add(QStringLiteral("gif_loading"), QStringLiteral("GIF blurhash loading 55%"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = downloading(gifReady, 0.55);
        return renderPreview(e, hashStill(gifHash, 480, 270, stillPixels(e, st)), st, ls);
    });

    // ---- 2.2 album grid ---------------------------------------------------------------------
    // Every grid must have albumLogicalSize(): what its tiles show never changes its size.
    {
        struct TileSpec {
            MediaEntry         entry;
            QImage             picture;                    // what the still is made from (Full / Preview)
            MediaStill::Source source    = MediaStill::Full; // BlurHash: decoded from entry.link.blurHash
            QImage             frame;                      // a GIF frame while it animates
            bool               arrived   = true;
            bool               concealed = false;
            qreal              fading    = 0.0; // 2.2 spoiler: the reveal crossfade over a revealed tile
            bool               hovered   = false;
            bool               pressed   = false;
        };
        const QImage gifStill = gif;
        const QImage pictures[] = {photo, sunset, friendPic, cropToAspect(friendPic, 1.0).mirrored(true, false), photo.mirrored(true, false), sunset.mirrored(true, true),
                                   cropToAspect(photo, 9.0 / 16.0), cropToAspect(sunset, 1.0)};
        auto ready = [&pictures](int n) {
            const QImage& pic = pictures[n % 8];
            TileSpec      t;
            t.entry         = makeEntry(QStringLiteral("trip_%1_3f9a1c2e.jpg").arg(n + 1), 845221 + static_cast<quint64>(n) * 91733, pic.width(), pic.height(), 0, MediaState::Ready);
            t.entry.link.protocol = MediaLink::kProtocol;
            t.entry.link.blurHash = hashOf(pic);
            t.picture       = pic;
            return t;
        };
        auto withState = [](TileSpec t, MediaState state, double progress = 0.0) {
            t.entry.state    = state;
            t.entry.progress = progress;
            t.source         = MediaStill::BlurHash;
            return t;
        };
        auto readyAlbum = [&ready](int count) {
            QVector<TileSpec> specs;
            for (int i = 0; i < count; ++i)
                specs.append(ready(i));
            return specs;
        };
        auto album = [&add](const QString& name, const QString& label, const QVector<TileSpec>& specs, int maxWidth = 400) {
            add(name, label, [specs, name](const PreviewStyle& st, QSize* ls) {
                const albums::Geometry g = albums::layout(specs.size(), st.maxWidth, st.maxHeight);
                QVector<AlbumTile>     tiles(specs.size());
                for (int i = 0; i < specs.size() && i < g.tiles.size(); ++i) {
                    const TileSpec& s = specs.at(i);
                    if (!s.arrived)
                        continue;
                    AlbumTile& t     = tiles[i];
                    t.entry          = &s.entry;
                    const QSize px   = albumTileStillPixels(s.entry, g.tiles.at(i).size(), st.dpr);
                    if (s.source == MediaStill::BlurHash)
                        t.still = hashStill(s.entry.link.blurHash, s.entry.link.width, s.entry.link.height, px);
                    else if (!s.picture.isNull())
                        t.still = pictureStill(s.picture, s.source, px);
                    if (!s.frame.isNull())
                        t.frame = fitScaled(s.frame, px);
                    t.concealed      = s.concealed;
                    t.concealOpacity = s.concealed ? 1.0 : s.fading;
                    t.hovered        = s.hovered;
                    t.pressed   = s.pressed;
                }
                const QImage image = renderAlbum(tiles, st, ls);
                if (!ls || *ls != albumLogicalSize(specs.size(), st)) {
                    QTextStream(stderr) << "error: " << name << " is not albumLogicalSize()\n";
                    ++g_layoutErrors;
                }
                return image;
            }, maxWidth);
        };

        for (const int count : {2, 3, 4, 5})
            album(QStringLiteral("album_%1").arg(count), QStringLiteral("album, %1 pictures").arg(count), readyAlbum(count));
        album(QStringLiteral("album_7_plus2"), QStringLiteral("album, 7 pictures (+2)"), readyAlbum(7));
        album(QStringLiteral("album_10_plus5"), QStringLiteral("album, 10 pictures (+5)"), readyAlbum(10));
        {
            QVector<TileSpec> specs = readyAlbum(4);
            specs[1]                = withState(specs[1], MediaState::Downloading, 0.35);
            specs[2]                = withState(specs[2], MediaState::Idle);
            specs[2].entry.link.size = 31457280;
            specs[3].entry          = failed(specs[3].entry, MediaError::Permission);
            specs[3].source         = MediaStill::Preview;
            album(QStringLiteral("album_4_mixed"), QStringLiteral("album: ready / 35% / not downloaded / failed"), specs);
        }
        {
            QVector<TileSpec> specs = readyAlbum(4);
            specs[2].arrived        = false;
            specs[3].arrived        = false;
            album(QStringLiteral("album_4_open_placeholders"), QStringLiteral("album: 2 of 4 arrived"), specs);
        }
        {
            QVector<TileSpec> specs = readyAlbum(3);
            for (TileSpec& s : specs)
                s = withState(s, MediaState::Queued);
            specs[0] = withState(specs[0], MediaState::Downloading, 0.6);
            album(QStringLiteral("album_3_loading"), QStringLiteral("album: loading (blurhash, queued)"), specs);
        }
        {
            QVector<TileSpec> specs = readyAlbum(4);
            TileSpec          g     = ready(0);
            g.entry                 = makeEntry(QStringLiteral("funny_3f9a1c2e.gif"), 387333, gifStill.width(), gifStill.height(), 0, MediaState::Ready);
            g.entry.link.protocol   = MediaLink::kProtocol;
            g.picture               = gifStill;
            g.frame                 = gifFrame(12, QSize(800, 800));
            specs[0]                = g;
            TileSpec v              = ready(1);
            v.entry                 = makeEntry(QStringLiteral("clip_3f9a1c2e.mp4"), 3993912, 1280, 720, 10000, MediaState::Idle);
            v.entry.link.protocol   = MediaLink::kProtocol;
            v.picture               = cropToAspect(sunset, 16.0 / 9.0);
            v.source                = MediaStill::Preview;
            specs[1]                = v;
            TileSpec v2             = v;
            v2.entry                = downloading(v.entry, 0.4);
            v2.entry.link.durationMs = 73000;
            v2.picture              = cropToAspect(friendPic, 16.0 / 9.0);
            specs[2]                = v2;
            album(QStringLiteral("album_gif_video"), QStringLiteral("album: GIF, video, video downloading"), specs);
        }
        {
            QVector<TileSpec> specs = readyAlbum(4);
            specs[0].concealed      = true;
            specs[3].concealed      = true;
            album(QStringLiteral("album_spoiler_tiles"), QStringLiteral("album: spoiler tiles (the chat's cover)"), specs);
            specs[3].hovered = true;
            specs[3].pressed = true;
            album(QStringLiteral("album_spoiler_pressed"), QStringLiteral("album: spoiler tile pressed"), specs);
            specs[3].hovered   = false;
            specs[3].pressed   = false;
            specs[3].concealed = false;
            specs[3].fading    = 0.5;
            album(QStringLiteral("album_spoiler_fading"), QStringLiteral("album: spoiler tile revealed, 50% crossfade"), specs);
            QVector<TileSpec> small = readyAlbum(7);
            for (TileSpec& s : small)
                s.concealed = true;
            album(QStringLiteral("album_spoiler_all_narrow"), QStringLiteral("album: all spoilers, narrow (200 px)"), small, 200);
        }
        {
            QVector<TileSpec> specs = readyAlbum(4);
            specs[1].hovered        = true;
            specs[1].pressed        = true;
            specs[2]                = withState(specs[2], MediaState::Idle);
            specs[2].hovered        = true;
            album(QStringLiteral("album_pressed_hover"), QStringLiteral("album: tile 2 pressed, tile 3 hovered"), specs);
        }
        {
            QVector<TileSpec> specs = readyAlbum(6);
            specs[0].entry          = failed(specs[0].entry, MediaError::NotFound);
            specs[4].entry          = failed(specs[4].entry, MediaError::Permission);
            specs[5]                = withState(specs[5], MediaState::Idle);
            album(QStringLiteral("album_6_failed_small"), QStringLiteral("album: small failed tiles"), specs);
        }
        album(QStringLiteral("album_narrow_3"), QStringLiteral("album, narrow chat (max 200 px), 3"), readyAlbum(3), 200);
        album(QStringLiteral("album_narrow_5"), QStringLiteral("album, narrow chat (max 200 px), 5 (+2)"), readyAlbum(5), 200);
        album(QStringLiteral("album_wide_4"), QStringLiteral("album, max width 600, 4"), readyAlbum(4), 600);
    }

    // ---- videos ----------------------------------------------------------------------------
    const QImage     poster      = cropToAspect(sunset, 16.0 / 9.0);
    const QImage     framePic    = cropToAspect(friendPic, 16.0 / 9.0);
    const MediaEntry clip        = makeEntry(QStringLiteral("demo_clip.mp4"), 3993912, 1280, 720, 10000, MediaState::Idle);
    auto             posterStill = [=](const MediaEntry& e, const PreviewStyle& st) { return pictureStill(poster, MediaStill::Preview, stillPixels(e, st)); };
    auto             frameAt     = [=](const MediaEntry& e, const PreviewStyle& st) { return fitScaled(framePic, stillPixels(e, st)); };

    add(QStringLiteral("video_poster_idle"), QStringLiteral("video poster, not downloaded"), [=](const PreviewStyle& st, QSize* ls) {
        return renderPreview(clip, posterStill(clip, st), st, ls);
    });
    add(QStringLiteral("video_poster_ready"), QStringLiteral("video poster, downloaded, hover"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.durationMs = 10000;
        o.hover      = VideoZone::Body;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_downloading_40"), QStringLiteral("video downloading 40% (pressed play)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = downloading(clip, 0.4);
        PlaybackOverlay  o;
        o.busy         = true;
        o.busyProgress = 0.4;
        o.durationMs   = 10000;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_auto_downloading"), QStringLiteral("video auto-downloading 60%"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = downloading(clip, 0.6);
        return renderPreview(e, posterStill(e, st), st, ls);
    });
    add(QStringLiteral("video_opening"), QStringLiteral("video opening (indeterminate)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.busy       = true;
        o.durationMs = 10000;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_playing_seek_hover"), QStringLiteral("playing, controls, hover on seek"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.playing         = true;
        o.positionMs      = 3200;
        o.durationMs      = 10000;
        o.hover           = VideoZone::Seek;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_playing_hidden"), QStringLiteral("playing, controls hidden"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.playing    = true;
        o.positionMs = 6100;
        o.durationMs = 10000;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_playing_muted_hidden"), QStringLiteral("playing muted, controls hidden"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.playing    = true;
        o.muted      = true;
        o.positionMs = 1500;
        o.durationMs = 10000;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_paused"), QStringLiteral("paused, hover on play/pause"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.positionMs      = 4100;
        o.durationMs      = 10000;
        o.hover           = VideoZone::PlayPause;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_ended"), QStringLiteral("ended (replay)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.ended           = true;
        o.positionMs      = 10000;
        o.durationMs      = 10000;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_muted_controls"), QStringLiteral("playing muted, hover on mute"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.playing         = true;
        o.muted           = true;
        o.positionMs      = 8800;
        o.durationMs      = 10000;
        o.hover           = VideoZone::Mute;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_vertical"), QStringLiteral("vertical video 720x1280 poster"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("vertical_clip.mp4"), 2509125, 720, 1280, 6000, MediaState::Ready);
        return renderPreview(e, pictureStill(cropToAspect(friendPic, 9.0 / 16.0), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("video_vertical_playing"), QStringLiteral("vertical video playing, controls"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("vertical_clip.mp4"), 2509125, 720, 1280, 6000, MediaState::Ready);
        PlaybackOverlay  o;
        o.controlsVisible = true;
        o.playing         = true;
        o.positionMs      = 2000;
        o.durationMs      = 6000;
        o.hover           = VideoZone::Expand;
        const QSize box   = previewLogicalSize(e, st);
        const QImage f    = fitScaled(cropToAspect(friendPic, 9.0 / 16.0), QSize(qRound(box.height() * st.dpr * 9.0 / 16.0), qRound(box.height() * st.dpr)));
        return renderVideo(e, f, MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_no_poster"), QStringLiteral("plain link video, no poster"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("web_clip.webm"), 355293, 0, 0, 0, MediaState::Idle);
        return renderPreview(e, MediaStill(), st, ls);
    });
    add(QStringLiteral("video_blurhash_poster"), QStringLiteral("video, poster still loading (blurhash)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e    = makeEntry(QStringLiteral("web_clip.webm"), 355293, 854, 480, 5000, MediaState::Idle);
        e.link.blurHash = kReferenceHash;
        return renderPreview(e, hashStill(kReferenceHash, 854, 480, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("video_failed"), QStringLiteral("video failed: no permission"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(clip, MediaError::Permission);
        return renderPreview(e, posterStill(e, st), st, ls);
    });
    add(QStringLiteral("video_narrow"), QStringLiteral("narrow chat (max 220 px), paused"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.positionMs      = 65000;
        o.durationMs      = 3723000;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    }, 220);
    add(QStringLiteral("video_poster_pressed"), QStringLiteral("poster, play pressed"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.durationMs = 10000;
        o.hover      = VideoZone::PlayPause;
        o.pressed    = VideoZone::PlayPause;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_mute_pressed"), QStringLiteral("playing, mute pressed"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.playing         = true;
        o.positionMs      = 5200;
        o.durationMs      = 10000;
        o.hover           = VideoZone::Mute;
        o.pressed         = VideoZone::Mute;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_controls_fading"), QStringLiteral("playing, controls fading out (50%)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.controlsOpacity = 0.5;
        o.playing         = true;
        o.positionMs      = 7400;
        o.durationMs      = 10000;
        o.hover           = VideoZone::Body;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    });
    add(QStringLiteral("video_external_only"), QStringLiteral("can't play inline (opens in default app)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = makeEntry(QStringLiteral("camera_clip.mkv"), 18400000, 1280, 720, 42000, MediaState::Ready);
        PlaybackOverlay o;
        o.externalOnly = true;
        o.durationMs   = 42000;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_external_only_narrow"), QStringLiteral("can't play inline, narrow (max 240 px)"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = makeEntry(QStringLiteral("camera_clip.mkv"), 18400000, 1280, 720, 3723000, MediaState::Ready);
        PlaybackOverlay o;
        o.externalOnly = true;
        o.durationMs   = 3723000;
        o.hover        = VideoZone::Body;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    }, 240);
    add(QStringLiteral("video_opening_static"), QStringLiteral("opening, Windows animations off"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.busy       = true;
        o.durationMs = 10000;
        PreviewStyle still = st;
        still.animate      = false;
        return renderVideo(e, QImage(), posterStill(e, st), o, still, ls);
    });
    add(QStringLiteral("video_busy_hover"), QStringLiteral("downloading 40% (pressed play), hover"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = downloading(clip, 0.4);
        PlaybackOverlay  o;
        o.busy         = true;
        o.busyProgress = 0.4;
        o.durationMs   = 10000;
        o.hover        = VideoZone::Body;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("video_queued"), QStringLiteral("video queued for download"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Queued;
        return renderPreview(e, posterStill(e, st), st, ls);
    });
    add(QStringLiteral("video_small_poster"), QStringLiteral("small player (max 220 px), poster: 44 px button"), [=](const PreviewStyle& st, QSize* ls) {
        return renderPreview(clip, posterStill(clip, st), st, ls);
    }, 220);
    add(QStringLiteral("video_failed_notfound"), QStringLiteral("video failed: deleted (no retry)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(clip, MediaError::NotFound);
        return renderPreview(e, posterStill(e, st), st, ls);
    });

    // ---- 2.2 spoiler: hidden (cover), during the reveal crossfade, revealed --------------------
    {
        auto cover = [](PreviewStyle st, qreal opacity = 1.0) {
            st.concealOpacity = opacity;
            return st;
        };
        const MediaEntry sunsetReady = makeEntry(QStringLiteral("sunset_photo.png"), 34867, 900, 560, 0, MediaState::Ready); // no BlurHash
        add(QStringLiteral("spoiler_image_hidden"), QStringLiteral("spoiler image hidden (BlurHash cover)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(photoReady, pictureStill(photo, MediaStill::Full, stillPixels(photoReady, st)), cover(st), ls);
        });
        add(QStringLiteral("spoiler_image_hover"), QStringLiteral("spoiler image hidden, hover"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(photoReady, pictureStill(photo, MediaStill::Full, stillPixels(photoReady, st)), cover(hovered(st)), ls);
        });
        add(QStringLiteral("spoiler_image_pressed"), QStringLiteral("spoiler image hidden, pressed"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(photoReady, pictureStill(photo, MediaStill::Full, stillPixels(photoReady, st)), cover(hovered(st, true)), ls);
        });
        add(QStringLiteral("spoiler_image_nohash"), QStringLiteral("spoiler image hidden, no BlurHash (12 px blur)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(sunsetReady, pictureStill(sunset, MediaStill::Full, stillPixels(sunsetReady, st)), cover(st), ls);
        });
        add(QStringLiteral("spoiler_image_idle_large"), QStringLiteral("spoiler hidden, too large (no download disc)"), [=](const PreviewStyle& st, QSize* ls) {
            MediaEntry e      = makeEntry(QStringLiteral("huge_panorama.jpg"), 25794560, 4000, 3000, 0, MediaState::Idle);
            e.tooLargeForAuto = true;
            return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), cover(st), ls);
        });
        add(QStringLiteral("spoiler_image_reveal_50"), QStringLiteral("spoiler image, reveal crossfade 50%"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(sunsetReady, pictureStill(sunset, MediaStill::Full, stillPixels(sunsetReady, st)), cover(st, 0.5), ls);
        });
        add(QStringLiteral("spoiler_image_revealed"), QStringLiteral("spoiler image revealed"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(sunsetReady, pictureStill(sunset, MediaStill::Full, stillPixels(sunsetReady, st)), cover(st, 0.0), ls);
        });
        add(QStringLiteral("spoiler_gif_hidden"), QStringLiteral("spoiler GIF hidden (no badge, no animation)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(gifReady, pictureStill(gif, MediaStill::Full, stillPixels(gifReady, st)), cover(st), ls);
        });
        add(QStringLiteral("spoiler_gif_revealed"), QStringLiteral("spoiler GIF revealed (animating)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderAnimatedFrame(gifReady, gifFrame(12, stillPixels(gifReady, st)), cover(st, 0.0), ls);
        });
        add(QStringLiteral("spoiler_video_hidden"), QStringLiteral("spoiler video hidden (no play, no duration)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(clip, posterStill(clip, st), cover(st), ls);
        });
        add(QStringLiteral("spoiler_video_reveal_30"), QStringLiteral("spoiler video, reveal crossfade 30%"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(clip, posterStill(clip, st), cover(st, 0.3), ls);
        });
        add(QStringLiteral("spoiler_video_revealed"), QStringLiteral("spoiler video revealed (poster, play)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(clip, posterStill(clip, st), cover(st, 0.0), ls);
        });
        add(QStringLiteral("spoiler_no_still"), QStringLiteral("spoiler hidden, nothing loaded yet"), [=](const PreviewStyle& st, QSize* ls) {
            const MediaEntry e = makeEntry(QStringLiteral("holiday.jpg"), 845221, 1600, 1200, 0, MediaState::Queued);
            return renderPreview(e, MediaStill(), cover(st), ls);
        });
        add(QStringLiteral("spoiler_failed_hidden"), QStringLiteral("spoiler hidden, failed: deleted (error under the cover)"), [=](const PreviewStyle& st, QSize* ls) {
            const MediaEntry e = failed(photoReady, MediaError::NotFound);
            return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), cover(st), ls);
        });
        add(QStringLiteral("spoiler_panorama_eyeoff"), QStringLiteral("spoiler, 40 px high (eye-off mark)"), [=](const PreviewStyle& st, QSize* ls) {
            const QImage     strip = cropToAspect(sunset, 8.0);
            const MediaEntry e     = makeEntry(QStringLiteral("strip.png"), 52000, strip.width(), strip.height(), 0, MediaState::Ready);
            return renderPreview(e, pictureStill(strip, MediaStill::Full, stillPixels(e, st)), cover(st), ls);
        }, 160);
        add(QStringLiteral("spoiler_card_image_nodims"), QStringLiteral("spoiler picture shown as a card (no name)"), [=](const PreviewStyle& st, QSize* ls) {
            const MediaEntry e = makeEntry(QStringLiteral("secret_ending_3f9a1c2e.jpg"), 845221, 0, 0, 0, MediaState::Idle);
            return renderPreview(e, MediaStill(), cover(st), ls);
        });
        add(QStringLiteral("spoiler_card_zip_ignored"), QStringLiteral("spoiler flag on a zip: ignored"), [=](const PreviewStyle& st, QSize* ls) {
            const MediaEntry e = makeEntry(QStringLiteral("project_files.zip"), 24700, 0, 0, 0, MediaState::Idle);
            return renderPreview(e, MediaStill(), cover(st), ls);
        });
    }

    // ---- cards -----------------------------------------------------------------------------
    const MediaEntry zip = makeEntry(QStringLiteral("project_files.zip"), 24700, 0, 0, 0, MediaState::Idle);
    auto card = [&add](const QString& name, const QString& label, const MediaEntry& e, int maxWidth = 400) {
        add(name, label, [e](const PreviewStyle& st, QSize* ls) { return renderPreview(e, MediaStill(), st, ls); }, maxWidth);
    };
    card(QStringLiteral("card_idle"), QStringLiteral("card idle"), zip);
    {
        MediaEntry e = zip;
        e.state      = MediaState::Queued;
        card(QStringLiteral("card_queued"), QStringLiteral("card queued"), e);
    }
    card(QStringLiteral("card_downloading"), QStringLiteral("card downloading 62%"), downloading(zip, 0.62));
    {
        MediaEntry e = zip;
        e.state      = MediaState::Ready;
        card(QStringLiteral("card_ready"), QStringLiteral("card ready"), e);
    }
    card(QStringLiteral("card_pdf_ready"), QStringLiteral("pdf ready"), makeEntry(QStringLiteral("Quarterly report 2026.pdf"), 1830000, 0, 0, 0, MediaState::Ready));
    card(QStringLiteral("card_audio_idle"), QStringLiteral("audio idle"), makeEntry(QStringLiteral("voice_note.mp3"), 412000, 0, 0, 0, MediaState::Idle));
    card(QStringLiteral("card_exe_idle"), QStringLiteral("other file (exe)"), makeEntry(QStringLiteral("setup_tool.exe"), 8812000, 0, 0, 0, MediaState::Idle));
    card(QStringLiteral("card_docx_downloading"), QStringLiteral("docx downloading 15%"), downloading(makeEntry(QStringLiteral("notes.docx"), 98000, 0, 0, 0, MediaState::Idle), 0.15));
    card(QStringLiteral("card_failed_notfound"), QStringLiteral("card failed: deleted"), failed(zip, MediaError::NotFound));
    card(QStringLiteral("card_failed_permission"), QStringLiteral("card failed: permission"), failed(zip, MediaError::Permission));
    card(QStringLiteral("card_failed_password"), QStringLiteral("card failed: password"), failed(zip, MediaError::Password));
    card(QStringLiteral("card_failed_notconnected"), QStringLiteral("card failed: not connected"), failed(zip, MediaError::NotConnected));
    card(QStringLiteral("card_failed_quota"), QStringLiteral("card failed: quota"), failed(zip, MediaError::Quota));
    card(QStringLiteral("card_failed_other"), QStringLiteral("card failed: other"), failed(zip, MediaError::Other, QStringLiteral("Transfer interrupted")));
    card(QStringLiteral("card_image_no_preview"), QStringLiteral("plain image link, cannot decode"), makeEntry(QStringLiteral("corrupt.png"), 5000, 0, 0, 0, MediaState::Ready));
    card(QStringLiteral("card_image_large"), QStringLiteral("plain image link, too large"), [&] {
        MediaEntry e      = makeEntry(QStringLiteral("gigantic_map.png"), 48000000, 0, 0, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return e;
    }());
    card(QStringLiteral("card_long_name"), QStringLiteral("long file name, narrow"), makeEntry(QStringLiteral("a_really_long_file_name_that_does_not_fit_anywhere_final_v2.tar.gz"), 734003200, 0, 0, 0, MediaState::Idle), 240);

    // A name with a RIGHT-TO-LEFT OVERRIDE must not fake its extension ("invoice_exe.pdf" look).
    card(QStringLiteral("card_bidi_override"), QStringLiteral("name with U+202E (shown without it)"),
         makeEntry(QStringLiteral("invoice_") + QChar(0x202E) + QStringLiteral("fdp.exe"), 88000, 0, 0, 0, MediaState::Idle));

    // Cards in pointer and other per-preview states.
    auto styled = [&add](const QString& name, const QString& label, const MediaEntry& e, std::function<PreviewStyle(const PreviewStyle&)> change, int maxWidth = 400) {
        add(name, label, [e, change](const PreviewStyle& st, QSize* ls) { return renderPreview(e, MediaStill(), change(st), ls); }, maxWidth);
    };
    styled(QStringLiteral("card_idle_hover"), QStringLiteral("card idle, hover"), zip, [](const PreviewStyle& st) { return hovered(st); });
    styled(QStringLiteral("card_idle_pressed"), QStringLiteral("card idle, pressed"), zip, [](const PreviewStyle& st) { return hovered(st, true); });
    {
        MediaEntry e = zip;
        e.state      = MediaState::Queued;
        styled(QStringLiteral("card_queued_hover"), QStringLiteral("card queued, hover"), e, [](const PreviewStyle& st) { return hovered(st); });
    }
    styled(QStringLiteral("card_failed_quota_hover"), QStringLiteral("card failed: quota, hover (Retry)"), failed(zip, MediaError::Quota),
           [](const PreviewStyle& st) { return hovered(st); });
    styled(QStringLiteral("card_failed_notfound_hover"), QStringLiteral("card failed: deleted, hover (inert)"), failed(zip, MediaError::NotFound),
           [](const PreviewStyle& st) { return hovered(st); });
    card(QStringLiteral("card_failed_quota_narrow"), QStringLiteral("card failed: quota, narrow (240 px)"), failed(zip, MediaError::Quota), 240);
    card(QStringLiteral("card_downloading_narrow"), QStringLiteral("card downloading 62%, narrow (240 px)"),
         downloading(makeEntry(QStringLiteral("holiday_video_raw_footage.zip"), 734003200, 0, 0, 0, MediaState::Idle), 0.62), 240);
    styled(QStringLiteral("card_exe_ready"), QStringLiteral("program, downloaded (shown in folder)"), makeEntry(QStringLiteral("setup_tool.exe"), 8812000, 0, 0, 0, MediaState::Ready),
           [](const PreviewStyle& st) {
               PreviewStyle s = st;
               s.revealOnly   = true;
               return s;
           });
    styled(QStringLiteral("card_large_font"), QStringLiteral("card downloading, 11 pt chat font"), downloading(zip, 0.4),
           [](const PreviewStyle& st) { return withChatFont(st, 11.0); });
    {
        // TS Media upload names: the random suffix is not shown, pasted pictures get a real name.
        MediaEntry e    = makeEntry(QStringLiteral("holiday_3f9a1c2e.zip"), 1530000, 0, 0, 0, MediaState::Idle);
        e.link.protocol = MediaLink::kProtocol;
        card(QStringLiteral("card_tsmedia_name"), QStringLiteral("TS Media upload holiday_3f9a1c2e.zip"), e);
    }

    // ---- 2.2 audio cards -------------------------------------------------------------------
    // Every state must keep the file card's box (checked: no layout jump when a card starts playing).
    const MediaEntry song = [] {
        MediaEntry e    = makeEntry(QStringLiteral("summer_mix_3f9a1c2e.mp3"), 4404019, 0, 0, 205000, MediaState::Idle);
        e.link.protocol = MediaLink::kProtocol;
        return e;
    }();
    auto audioOverlay = [](const MediaEntry& e) {
        PlaybackOverlay o;
        o.durationMs      = e.link.durationMs;
        o.controlsVisible = true;
        return o;
    };
    auto audio = [&add](const QString& name, const QString& label, const MediaEntry& e, const PlaybackOverlay& o,
                        std::function<PreviewStyle(const PreviewStyle&)> change = nullptr, int maxWidth = 400) {
        add(name, label, [e, o, change](const PreviewStyle& st, QSize* ls) {
            const PreviewStyle s = change ? change(st) : st;
            QSize              logical;
            const QImage       img = renderAudioCard(e, o, s, &logical);
            if (logical != previewLogicalSize(e, s)) {
                QTextStream(stderr) << "error: audio card " << e.link.fileName << " is " << logical.width() << "x" << logical.height()
                                    << ", the chat reserves " << previewLogicalSize(e, s).width() << "x" << previewLogicalSize(e, s).height() << "\n";
                ++g_layoutFailures;
            }
            if (ls)
                *ls = logical;
            return img;
        }, maxWidth);
    };
    audio(QStringLiteral("audio_idle"), QStringLiteral("audio idle (not downloaded)"), song, audioOverlay(song));
    audio(QStringLiteral("audio_idle_hover"), QStringLiteral("audio idle, hover on play"), song, [&] {
        PlaybackOverlay o = audioOverlay(song);
        o.hover           = VideoZone::PlayPause;
        return o;
    }(), [](const PreviewStyle& st) { return hovered(st); });
    {
        MediaEntry e      = song;
        e.link.durationMs = 0;
        e.link.protocol   = 0;
        e.link.fileName   = QStringLiteral("from_teamspeak.mp3");
        audio(QStringLiteral("audio_idle_no_duration"), QStringLiteral("plain link, no duration"), e, audioOverlay(e));
    }
    {
        MediaEntry e = song;
        e.state      = MediaState::Queued;
        PlaybackOverlay o = audioOverlay(e);
        o.busy            = true;
        audio(QStringLiteral("audio_queued"), QStringLiteral("audio queued (pressed play)"), e, o);
    }
    {
        const MediaEntry e = downloading(song, 0.45);
        PlaybackOverlay  o = audioOverlay(e);
        o.busy             = true;
        o.busyProgress     = 0.45;
        audio(QStringLiteral("audio_downloading"), QStringLiteral("audio downloading 45% (pressed play)"), e, o);
        audio(QStringLiteral("audio_auto_downloading"), QStringLiteral("audio auto-downloading 45%"), e, audioOverlay(e));
    }
    MediaEntry songReady = song;
    songReady.state      = MediaState::Ready;
    songReady.progress   = 1.0;
    {
        PlaybackOverlay o = audioOverlay(songReady);
        o.busy            = true;
        audio(QStringLiteral("audio_opening"), QStringLiteral("audio opening"), songReady, o);
        audio(QStringLiteral("audio_opening_static"), QStringLiteral("audio opening, animations off"), songReady, o, [](const PreviewStyle& st) {
            PreviewStyle s = st;
            s.animate      = false;
            return s;
        });
    }
    audio(QStringLiteral("audio_ready"), QStringLiteral("audio downloaded, at rest"), songReady, audioOverlay(songReady));
    {
        PlaybackOverlay o = audioOverlay(songReady);
        o.playing         = true;
        o.positionMs      = 71750;
        audio(QStringLiteral("audio_playing"), QStringLiteral("audio playing 35%"), songReady, o);
        o.hover = VideoZone::Seek;
        audio(QStringLiteral("audio_playing_seek_hover"), QStringLiteral("audio playing, hover on the seek bar"), songReady, o,
              [](const PreviewStyle& st) { return hovered(st); });
        o.hover   = VideoZone::PlayPause;
        o.pressed = VideoZone::PlayPause;
        audio(QStringLiteral("audio_pause_pressed"), QStringLiteral("audio playing, pause pressed"), songReady, o,
              [](const PreviewStyle& st) { return hovered(st, true); });
    }
    {
        PlaybackOverlay o = audioOverlay(songReady);
        o.positionMs      = 42000;
        audio(QStringLiteral("audio_paused"), QStringLiteral("audio paused at 0:42"), songReady, o);
        PlaybackOverlay e = audioOverlay(songReady);
        e.ended           = true;
        e.positionMs      = 205000;
        audio(QStringLiteral("audio_ended"), QStringLiteral("audio ended (replay)"), songReady, e);
        PlaybackOverlay x = audioOverlay(songReady);
        x.externalOnly    = true;
        audio(QStringLiteral("audio_external"), QStringLiteral("audio can't play here (opens externally)"), songReady, x);
    }
    audio(QStringLiteral("audio_failed_permission"), QStringLiteral("audio failed: permission (retry)"), failed(song, MediaError::Permission), audioOverlay(song));
    audio(QStringLiteral("audio_failed_permission_hover"), QStringLiteral("audio failed: permission, hover"), failed(song, MediaError::Permission), [&] {
        PlaybackOverlay o = audioOverlay(song);
        o.hover           = VideoZone::PlayPause;
        return o;
    }(), [](const PreviewStyle& st) { return hovered(st); });
    audio(QStringLiteral("audio_failed_notfound"), QStringLiteral("audio failed: deleted (no retry)"), failed(song, MediaError::NotFound), audioOverlay(song));
    {
        MediaEntry e      = songReady;
        e.link.durationMs = 3723000;
        e.link.fileName   = QStringLiteral("podcast_episode_42_the_long_one_3f9a1c2e.m4a");
        PlaybackOverlay o = audioOverlay(e);
        o.playing         = true;
        o.positionMs      = 1830000;
        audio(QStringLiteral("audio_1h_playing"), QStringLiteral("1 h audio playing, long name"), e, o);
        audio(QStringLiteral("audio_narrow_240"), QStringLiteral("long name, narrow (240 px)"), e, o, nullptr, 240);
        audio(QStringLiteral("audio_narrow_160"), QStringLiteral("smallest card (160 px)"), e, o, nullptr, 160);
    }
    {
        // Right-to-left name keeps its own direction; the card stays left-to-right.
        MediaEntry e    = songReady;
        e.link.fileName = QString::fromUtf8("آهنگ تابستانی_3f9a1c2e.ogg");
        e.kind          = kindForFileName(e.link.fileName);
        PlaybackOverlay o = audioOverlay(e);
        o.positionMs      = 12000;
        audio(QStringLiteral("audio_rtl_name"), QStringLiteral("right-to-left name, paused"), e, o);
    }
    audio(QStringLiteral("audio_large_font"), QStringLiteral("audio playing, 11 pt chat font"), songReady, [&] {
        PlaybackOverlay o = audioOverlay(songReady);
        o.playing         = true;
        o.positionMs      = 100000;
        return o;
    }(), [](const PreviewStyle& st) { return withChatFont(st, 11.0); });
    // ---- 2.2 data saver and drag-out -------------------------------------------------------
    {
        MediaEntry held      = makeEntry(QStringLiteral("voice_note.mp3"), 2516582, 0, 0, 0, MediaState::Idle);
        held.heldByDataSaver = true;
        card(QStringLiteral("card_data_saver"), QStringLiteral("card held by data saver"), held);
        styled(QStringLiteral("card_data_saver_hover"), QStringLiteral("card held by data saver, hover"), held, [](const PreviewStyle& st) { return hovered(st); });
        MediaEntry heldPhoto      = makeEntry(QStringLiteral("holiday.jpg"), 2516582, 4000, 3000, 0, MediaState::Idle);
        heldPhoto.heldByDataSaver = true; // looks exactly like any picture waiting for a click
        add(QStringLiteral("image_data_saver"), QStringLiteral("picture held by data saver (preview)"), [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(heldPhoto, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(heldPhoto, st)), st, ls);
        });
    }
    auto dragSample = [&add](const QString& name, const QString& label, std::function<DragImage(const PreviewStyle&)> fn) {
        add(name, label, [fn](const PreviewStyle& st, QSize* ls) {
            const DragImage d = fn(st);
            if (ls)
                *ls = (QSizeF(d.image.size()) / d.image.devicePixelRatio()).toSize();
            return d.image;
        });
    };
    // Stills as Core gives them: device pixels fitted into 160 x 160 logical pixels.
    auto dragStill = [](const QImage& source, const PreviewStyle& st) { return fitScaled(source, QSize(qRound(160 * st.dpr), qRound(160 * st.dpr))); };
    dragSample(QStringLiteral("drag_photo_landscape"), QStringLiteral("drag: landscape photo"),
               [=](const PreviewStyle& st) { return renderDragPicture(dragStill(photo, st), st.dpr, QPointF(0.3, 0.6)); });
    dragSample(QStringLiteral("drag_photo_portrait"), QStringLiteral("drag: portrait photo"),
               [=](const PreviewStyle& st) { return renderDragPicture(dragStill(cropToAspect(friendPic, 3.0 / 4.0), st), st.dpr, QPointF(0.5, 0.5)); });
    dragSample(QStringLiteral("drag_video_poster"), QStringLiteral("drag: video poster"),
               [=](const PreviewStyle& st) { return renderDragPicture(dragStill(poster, st), st.dpr, QPointF(0.9, 0.1)); });
    dragSample(QStringLiteral("drag_card_zip"), QStringLiteral("drag: file card"),
               [=](const PreviewStyle& st) { return renderDragCard(makeEntry(QStringLiteral("project_files.zip"), 24700, 0, 0, 0, MediaState::Ready), false, st.font, st.dpr); });
    dragSample(QStringLiteral("drag_card_program"), QStringLiteral("drag: program card"),
               [=](const PreviewStyle& st) { return renderDragCard(makeEntry(QStringLiteral("setup_tool.exe"), 2516582, 0, 0, 0, MediaState::Ready), true, st.font, st.dpr); });
    dragSample(QStringLiteral("drag_card_persian"), QStringLiteral("drag: long Persian name"), [=](const PreviewStyle& st) {
        MediaEntry e    = makeEntry(QStringLiteral("گزارش نهایی پروژه تابستان ۱۴۰۳ نسخه دوم_3f9a1c2e.pdf"), 1830000, 0, 0, 0, MediaState::Ready);
        e.link.protocol = MediaLink::kProtocol;
        return renderDragCard(e, false, st.font, st.dpr);
    });

    // ---- 2.2 sha: checking the downloaded file, and a file that doesn't match --------------------
    // Core shows "Checking file…" only once a check has taken a moment (check.shown); files over
    // 256 MB show the check's own progress.
    auto checking = [](MediaEntry e, double progress, bool again = false) {
        e.state          = MediaState::Downloading;
        e.progress       = 1.0;
        e.check.running  = true;
        e.check.shown    = true;
        e.check.again    = again;
        e.check.progress = progress;
        return e;
    };
    add(QStringLiteral("sha_image_checking"), QStringLiteral("image downloaded, checking (small file)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = checking(photoReady, 0.5);
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("sha_image_mismatch"), QStringLiteral("image doesn't match what was sent (preview behind)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::Mismatch);
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    });
    add(QStringLiteral("sha_image_mismatch_blurhash"), QStringLiteral("image mismatch over its blurhash, hover (inert)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::Mismatch);
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), hovered(st), ls);
    });
    const MediaEntry bigClip = makeEntry(QStringLiteral("concert_full.mp4"), 536870912, 1280, 720, 5400000, MediaState::Idle);
    add(QStringLiteral("sha_video_checking_512mb"), QStringLiteral("video 512 MB checking 42% (pressed play)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = checking(bigClip, 0.42);
        PlaybackOverlay  o;
        o.busy         = true;
        o.busyProgress = shownDownloadProgress(e);
        o.durationMs   = e.link.durationMs;
        return renderVideo(e, QImage(), posterStill(e, st), o, st, ls);
    });
    add(QStringLiteral("sha_video_checking_auto"), QStringLiteral("video checking, not playing (small file)"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = checking(clip, 0.3);
        return renderPreview(e, posterStill(e, st), st, ls);
    });
    add(QStringLiteral("sha_video_mismatch"), QStringLiteral("video doesn't match what was sent"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(clip, MediaError::Mismatch);
        return renderPreview(e, posterStill(e, st), st, ls);
    });
    card(QStringLiteral("sha_card_checking"), QStringLiteral("card checking (small file)"), checking(zip, 0.5));
    card(QStringLiteral("sha_card_checking_512mb"), QStringLiteral("card 512 MB checking 42%"),
         checking(makeEntry(QStringLiteral("backup_2026.zip"), 536870912, 0, 0, 0, MediaState::Idle), 0.42));
    card(QStringLiteral("sha_card_checking_again"), QStringLiteral("card 512 MB checking again 10%"),
         checking(makeEntry(QStringLiteral("backup_2026.zip"), 536870912, 0, 0, 0, MediaState::Idle), 0.10, true));
    card(QStringLiteral("sha_card_mismatch"), QStringLiteral("card doesn't match what was sent"), failed(zip, MediaError::Mismatch));
    card(QStringLiteral("sha_card_mismatch_narrow"), QStringLiteral("card mismatch, narrow (240 px)"), failed(zip, MediaError::Mismatch), 240);
    styled(QStringLiteral("sha_card_mismatch_hover"), QStringLiteral("card mismatch, hover (inert)"), failed(zip, MediaError::Mismatch),
           [](const PreviewStyle& st) { return hovered(st); });

    // ---- 2.2 reactions: the row under media, the add button, hover / pressed -----------------------
    {
        // A preview as ChatIntegration draws it: the media first, then rx::composeObject over it.
        using Picture = std::function<QImage(const PreviewStyle&, QSize*)>;
        // counts: (v1 reaction index, count); 2.2 emoji: (-emoji id - 1, count) for any other emoji.
        auto view = [](std::initializer_list<std::pair<int, int>> counts, int mineMask, QSet<int> mineIds = {}) {
            ReactionView v;
            for (const auto& c : counts) {
                ReactionView::Entry e;
                e.reaction = c.first >= 0 ? proto::legacyReactionEmoji(c.first) : -c.first - 1;
                e.count    = c.second;
                e.mine     = (c.first >= 0 && ((mineMask >> c.first) & 1)) || mineIds.contains(e.reaction);
                for (int n = 0; n < e.count - (e.mine ? 1 : 0) && n < 3; ++n)
                    e.others << QStringLiteral("Friend %1").arg(n + 1);
                v.entries.append(e);
            }
            return v;
        };
        const auto other = [](const char* code) { return -emoji::fromWireCode(QByteArray(code)) - 1; };
        auto reacted = [&add](const QString& name, const QString& label, Picture picture, ReactionView v, std::function<void(rx::ObjectState&)> change,
                              int maxWidth = 400) {
            add(name, label, [picture, v, change](const PreviewStyle& st, QSize* ls) {
                QSize           size;
                const QImage    img = picture(st, &size);
                rx::ObjectState state;
                state.dark     = st.dark;
                state.base     = st.dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff); // the sheet: TeamSpeak's chat
                state.font     = st.font;
                state.dpr      = st.dpr;
                state.maxWidth = st.maxWidth;
                state.canAdd   = true;
                change(state);
                return rx::composeObject(img, size, v, state, nullptr, ls);
            }, maxWidth);
        };
        const Picture photoPicture = [=](const PreviewStyle& st, QSize* ls) {
            return renderPreview(photoReady, pictureStill(photo, MediaStill::Full, stillPixels(photoReady, st)), st, ls);
        };
        const auto none  = [](rx::ObjectState&) {};
        const auto hover = [](rx::ObjectState& s) { s.hovered = true; };
        using rx::Zone;
        const int up = proto::ThumbsUp, heart = proto::Heart, lol = proto::Laughing, wow = proto::Surprised, sad = proto::Sad, fire = proto::Fire;

        reacted(QStringLiteral("reactions_none_hover"), QStringLiteral("reactions: none, hover (add button)"), photoPicture, ReactionView(), hover);
        reacted(QStringLiteral("reactions_button_hover"), QStringLiteral("reactions: add button under the pointer"), photoPicture, ReactionView(),
                [](rx::ObjectState& s) {
                    s.hovered   = true;
                    s.hoverZone = Zone::AddButton;
                });
        reacted(QStringLiteral("reactions_one_mine"), QStringLiteral("reactions: one, yours"), photoPicture, view({{up, 1}}, 1 << up), none);
        reacted(QStringLiteral("reactions_mixed_hover_pill"), QStringLiteral("reactions: mixed, hover on a pill (add pill shows)"), photoPicture,
                view({{up, 3}, {heart, 12}, {lol, 1}}, 1 << up), [](rx::ObjectState& s) {
                    s.hovered    = true;
                    s.hoverZone  = Zone::Pill;
                    s.hoverIndex = proto::legacyReactionEmoji(proto::Heart);
                });
        reacted(QStringLiteral("reactions_pressed_pill"), QStringLiteral("reactions: pill pressed"), photoPicture, view({{up, 3}, {fire, 2}}, 0),
                [](rx::ObjectState& s) {
                    s.hovered    = true;
                    s.hoverZone  = s.pressZone = Zone::Pill;
                    s.hoverIndex = s.pressIndex = proto::legacyReactionEmoji(proto::Fire);
                });
        reacted(QStringLiteral("reactions_six_99plus"), QStringLiteral("reactions: all six, 99+, two yours"), photoPicture,
                view({{up, 120}, {heart, 42}, {lol, 7}, {wow, 1}, {sad, 2}, {fire, 99}}, (1 << heart) | (1 << fire)), hover);
        // 2.2 emoji: any emoji as a reaction, HD pills, many different ones wrapping.
        reacted(QStringLiteral("reactions_emoji_mixed"), QStringLiteral("reactions: any emoji (HD), two yours, hover"), photoPicture,
                view({{up, 4}, {other("1f923"), 3}, {other("1f389"), 2}, {other("1fae1"), 1}, {other("1f480"), 5}, {heart, 2}}, 1 << up,
                     {emoji::fromWireCode("1f389")}),
                hover);
        reacted(QStringLiteral("reactions_emoji_twenty"), QStringLiteral("reactions: twenty different ones, wrapped"), photoPicture,
                view({{up, 9}, {heart, 7}, {lol, 5}, {wow, 1}, {sad, 1}, {fire, 3}, {other("1f923"), 2}, {other("1f389"), 4}, {other("1f480"), 1},
                      {other("1f440"), 2}, {other("1f64f"), 1}, {other("1f4af"), 6}, {other("1f60e"), 1}, {other("1f914"), 1}, {other("1f973"), 2},
                      {other("1f44f"), 3}, {other("1f308"), 1}, {other("1f355"), 1}, {other("1f680"), 2}, {other("1f468-200d-1f469-200d-1f467"), 1}},
                     1 << fire, {emoji::fromWireCode("1f4af")}),
                hover);
        reacted(QStringLiteral("reactions_add_pill_hover"), QStringLiteral("reactions: add pill under the pointer"), photoPicture, view({{sad, 1}}, 0),
                [](rx::ObjectState& s) {
                    s.hovered   = true;
                    s.hoverZone = Zone::AddPill;
                });
        reacted(QStringLiteral("reactions_row_kept_empty"), QStringLiteral("reactions: last one removed, pointer still on the row"), photoPicture,
                ReactionView(), [](rx::ObjectState& s) {
                    s.hovered = true;
                    s.keep    = true;
                });
        reacted(QStringLiteral("reactions_cant_react_hover"), QStringLiteral("reactions: hover where you can't react (no add pill)"), photoPicture,
                view({{up, 2}, {heart, 1}}, 0), [](rx::ObjectState& s) {
                    s.hovered = true;
                    s.canAdd  = false;
                });
        reacted(QStringLiteral("reactions_narrow_wrap"), QStringLiteral("reactions: narrow chat (160 px), wraps"),
                [=](const PreviewStyle& st, QSize* ls) {
                    const QImage     tall = cropToAspect(friendPic, 3.0 / 4.0);
                    const MediaEntry e    = makeEntry(QStringLiteral("portrait.jpg"), 120000, tall.width(), tall.height(), 0, MediaState::Ready);
                    return renderPreview(e, pictureStill(tall, MediaStill::Full, stillPixels(e, st)), st, ls);
                },
                view({{up, 4}, {heart, 2}, {lol, 13}, {wow, 1}, {fire, 5}}, 1 << lol), hover, 160);
        reacted(QStringLiteral("reactions_gif_badge_button"), QStringLiteral("reactions: GIF badge and add button"),
                [=](const PreviewStyle& st, QSize* ls) { return renderAnimatedFrame(gifReady, gifFrame(12, stillPixels(gifReady, st)), st, ls); },
                ReactionView(), hover);
        reacted(QStringLiteral("reactions_video_controls_row"), QStringLiteral("reactions: video controls and a row"),
                [=](const PreviewStyle& st, QSize* ls) {
                    MediaEntry e = clip;
                    e.state      = MediaState::Ready;
                    PlaybackOverlay o;
                    o.controlsVisible = true;
                    o.playing         = true;
                    o.positionMs      = 3200;
                    o.durationMs      = 10000;
                    o.hover           = VideoZone::Body;
                    return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
                },
                view({{fire, 3}, {wow, 1}}, 1 << wow), hover);
        // 2.2 integration: an album has one row under its grid (its first item's reactions); audio and voice
        // cards have no add button, only the row's add pill.
        const Picture albumPicture = [=](const PreviewStyle& st, QSize* ls) {
            const QImage            pics[] = {photo, sunset, friendPic};
            const albums::Geometry  g      = albums::layout(3, st.maxWidth, st.maxHeight);
            QVector<MediaEntry>     entries;
            for (int i = 0; i < 3; ++i) {
                MediaEntry e    = makeEntry(QStringLiteral("trip_%1_3f9a1c2e.jpg").arg(i + 1), 845221, pics[i].width(), pics[i].height(), 0, MediaState::Ready);
                e.link.protocol = MediaLink::kProtocol;
                entries.append(e);
            }
            QVector<AlbumTile> tiles(3);
            for (int i = 0; i < 3; ++i) {
                tiles[i].entry = &entries[i];
                tiles[i].still = pictureStill(pics[i], MediaStill::Full, albumTileStillPixels(entries[i], g.tiles.at(i).size(), st.dpr));
            }
            return renderAlbum(tiles, st, ls);
        };
        reacted(QStringLiteral("reactions_album_row"), QStringLiteral("reactions: album, one row under the grid, hover"), albumPicture,
                view({{heart, 4}, {lol, 2}}, 1 << heart), hover);
        reacted(QStringLiteral("reactions_album_none_hover"), QStringLiteral("reactions: album hovered, none yet (add button)"), albumPicture, ReactionView(), hover);
        const Picture audioPicture = [=](const PreviewStyle& st, QSize* ls) {
            PlaybackOverlay o;
            o.durationMs      = songReady.link.durationMs;
            o.controlsVisible = true;
            return renderAudioCard(songReady, o, st, ls);
        };
        const auto cardHover = [](rx::ObjectState& s) {
            s.hovered  = true;
            s.noButton = true; // ChatReactions sets it for audio and voice cards
        };
        reacted(QStringLiteral("reactions_audio_row_hover"), QStringLiteral("reactions: audio card with a row, hover (add pill)"), audioPicture,
                view({{fire, 2}, {up, 1}}, 1 << up), cardHover);
        reacted(QStringLiteral("reactions_audio_none_hover"), QStringLiteral("reactions: audio card hovered, none yet (no button)"), audioPicture, ReactionView(),
                cardHover);
        // The pictures themselves, large and at pill size, to look at the drawing (2.2 emoji: HD on top, the
        // vector pictures used without the colour renderer below). 64 px at a 70 px step: the sample stays
        // inside the contact sheet's 430 px column.
        add(QStringLiteral("reactions_icons"), QStringLiteral("reaction pictures at 64, 36, 24 and 16 px: HD, then vector"), [](const PreviewStyle& st, QSize* ls) {
            constexpr int block = 64 + 8 + 36 + 8 + 24 + 8 + 16;
            const QSize   size(6 * 70, 2 * block + 16);
            QImage        out(size * st.dpr, QImage::Format_ARGB32_Premultiplied);
            out.setDevicePixelRatio(st.dpr);
            out.fill(Qt::transparent);
            QPainter p(&out);
            for (int i = 0; i < proto::kReactionCount; ++i) {
                qreal y = 0;
                for (const int px : {64, 36, 24, 16}) {
                    rx::drawReaction(p, QRectF(i * 70 + (64 - px) / 2.0, y, px, px), proto::legacyReactionEmoji(i));
                    rx::drawClassicReaction(p, QRectF(i * 70 + (64 - px) / 2.0, y + block + 16, px, px), i);
                    y += px + 8;
                }
            }
            p.end();
            *ls = size;
            return out;
        });
    }

    // ---- 2.2 voice messages ------------------------------------------------------------------
    // Drawn through renderAudioCard (as the chat does); every state keeps the 56 px voice card's box.
    const MediaEntry voiceIdle = [] {
        MediaEntry e      = makeEntry(QStringLiteral("voice_message_3f9a1c2e.m4a"), 146320, 0, 0, 12040, MediaState::Idle);
        e.link.protocol   = MediaLink::kProtocol;
        e.link.voice      = true;
        // A spoken sentence: syllables, two pauses (64 levels, as the recorder sends them).
        const int shape[64] = {2, 6, 11, 14, 12, 9, 13, 15, 10, 6, 3, 1, 0, 0, 4, 9, 12, 14, 13, 11, 8, 12, 14, 10, 7, 4, 2, 1, 1, 0, 0, 3,
                               8, 13, 15, 14, 9, 6, 10, 13, 12, 8, 5, 9, 12, 11, 7, 4, 2, 0, 0, 5, 10, 14, 15, 12, 8, 11, 9, 6, 4, 2, 1, 0};
        for (const int level : shape)
            e.link.waveform.append(static_cast<char>(level));
        return e;
    }();
    MediaEntry voiceReady = voiceIdle;
    voiceReady.state      = MediaState::Ready;
    voiceReady.progress   = 1.0;
    auto voice = [&add](const QString& name, const QString& label, const MediaEntry& e, const PlaybackOverlay& o,
                        std::function<PreviewStyle(const PreviewStyle&)> change = nullptr, int maxWidth = 400) {
        add(name, label, [e, o, change](const PreviewStyle& st, QSize* ls) {
            const PreviewStyle s = change ? change(st) : st;
            QSize              logical;
            const QImage       img = renderAudioCard(e, o, s, &logical);
            if (logical != previewLogicalSize(e, s) || logical != voiceCardSize(s)) {
                QTextStream(stderr) << "error: voice card " << e.link.fileName << " is " << logical.width() << "x" << logical.height()
                                    << ", the chat reserves " << previewLogicalSize(e, s).width() << "x" << previewLogicalSize(e, s).height() << "\n";
                ++g_layoutFailures;
            }
            if (ls)
                *ls = logical;
            return img;
        }, maxWidth);
    };
    voice(QStringLiteral("voice_idle"), QStringLiteral("voice message, not downloaded yet"), voiceIdle, audioOverlay(voiceIdle));
    voice(QStringLiteral("voice_ready"), QStringLiteral("voice message 0:12, ready"), voiceReady, audioOverlay(voiceReady));
    voice(QStringLiteral("voice_ready_hover"), QStringLiteral("voice message, hover on play"), voiceReady, [&] {
        PlaybackOverlay o = audioOverlay(voiceReady);
        o.hover           = VideoZone::PlayPause;
        return o;
    }(), [](const PreviewStyle& st) { return hovered(st); });
    {
        const MediaEntry e = downloading(voiceIdle, 0.45);
        PlaybackOverlay  o = audioOverlay(e);
        o.busy             = true;
        o.busyProgress     = 0.45;
        voice(QStringLiteral("voice_downloading"), QStringLiteral("voice message downloading 45% (pressed play)"), e, o);
        voice(QStringLiteral("voice_auto_downloading"), QStringLiteral("voice message downloading by itself"), e, audioOverlay(e));
        PlaybackOverlay opening = audioOverlay(voiceReady);
        opening.busy            = true;
        voice(QStringLiteral("voice_opening"), QStringLiteral("voice message opening"), voiceReady, opening);
    }
    {
        PlaybackOverlay o = audioOverlay(voiceReady);
        o.playing         = true;
        o.positionMs      = 4214; // 35 %
        voice(QStringLiteral("voice_playing_35"), QStringLiteral("voice message playing 35%"), voiceReady, o);
        o.hover   = VideoZone::PlayPause;
        o.pressed = VideoZone::PlayPause;
        voice(QStringLiteral("voice_pause_pressed"), QStringLiteral("voice message, pause pressed"), voiceReady, o, [](const PreviewStyle& st) { return hovered(st, true); });
        PlaybackOverlay paused = audioOverlay(voiceReady);
        paused.positionMs      = 7800;
        paused.hover           = VideoZone::Seek;
        voice(QStringLiteral("voice_paused_hover_wave"), QStringLiteral("voice message paused at 0:07, hover on the waveform"), voiceReady, paused,
              [](const PreviewStyle& st) { return hovered(st); });
        PlaybackOverlay ended = audioOverlay(voiceReady);
        ended.ended           = true;
        ended.positionMs      = voiceReady.link.durationMs;
        voice(QStringLiteral("voice_ended"), QStringLiteral("voice message played to the end (back at rest)"), voiceReady, ended);
        PlaybackOverlay external = audioOverlay(voiceReady);
        external.externalOnly    = true;
        voice(QStringLiteral("voice_external"), QStringLiteral("voice message can't play here (opens externally)"), voiceReady, external);
    }
    {
        MediaEntry e = voiceReady;
        e.link.waveform.clear();
        PlaybackOverlay o = audioOverlay(e);
        o.playing         = true;
        o.positionMs      = 5000;
        voice(QStringLiteral("voice_no_waveform"), QStringLiteral("voice message without a waveform, playing"), e, o);
    }
    voice(QStringLiteral("voice_failed_retry"), QStringLiteral("voice message failed: not connected (retry)"), failed(voiceIdle, MediaError::NotConnected), audioOverlay(voiceIdle));
    voice(QStringLiteral("voice_failed_notfound"), QStringLiteral("voice message failed: deleted (no retry)"), failed(voiceIdle, MediaError::NotFound), audioOverlay(voiceIdle));
    {
        MediaEntry shortOne      = voiceReady;
        shortOne.link.durationMs = 1100;
        voice(QStringLiteral("voice_0_01"), QStringLiteral("voice message 0:01"), shortOne, audioOverlay(shortOne));
        MediaEntry longest      = voiceReady;
        longest.link.durationMs = 300000;
        PlaybackOverlay o       = audioOverlay(longest);
        o.positionMs            = 245000;
        voice(QStringLiteral("voice_5_00_paused"), QStringLiteral("voice message 5:00, paused at 4:05"), longest, o);
        MediaEntry unknown      = voiceReady;
        unknown.link.durationMs = 0;
        voice(QStringLiteral("voice_no_duration"), QStringLiteral("voice message without a length"), unknown, audioOverlay(unknown));
    }
    {
        PlaybackOverlay o = audioOverlay(voiceReady);
        o.playing         = true;
        o.positionMs      = 6000;
        voice(QStringLiteral("voice_narrow_200"), QStringLiteral("voice message, narrow chat (200 px)"), voiceReady, o, nullptr, 200);
        voice(QStringLiteral("voice_narrow_160"), QStringLiteral("voice message, smallest (160 px, no time)"), voiceReady, o, nullptr, 160);
        voice(QStringLiteral("voice_large_font"), QStringLiteral("voice message playing, 11 pt chat font"), voiceReady, o,
              [](const PreviewStyle& st) { return withChatFont(st, 11.0); });
    }
    // 2.2 integration: reactions on a voice card, as ChatIntegration composes them (no hover button on
    // cards: the row's add pill, decisions).
    {
        // counts: (v1 reaction index, count); 2.2 emoji: (-emoji id - 1, count) for any other emoji.
        auto voiceReacted = [&add, voiceReady, audioOverlay](const QString& name, const QString& label, std::initializer_list<std::pair<int, int>> counts,
                                                            int mineMask, bool hover, rx::Zone zone) {
            ReactionView v;
            for (const auto& c : counts) {
                ReactionView::Entry e;
                e.reaction = c.first >= 0 ? proto::legacyReactionEmoji(c.first) : -c.first - 1;
                e.count    = c.second;
                e.mine     = c.first >= 0 && ((mineMask >> c.first) & 1);
                for (int n = 0; n < e.count - (e.mine ? 1 : 0) && n < 3; ++n)
                    e.others << QStringLiteral("Friend %1").arg(n + 1);
                v.entries.append(e);
            }
            add(name, label, [=](const PreviewStyle& st, QSize* ls) {
                QSize           size;
                const QImage    img = renderAudioCard(voiceReady, audioOverlay(voiceReady), st, &size);
                rx::ObjectState state;
                state.dark      = st.dark;
                state.base      = st.dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff);
                state.font      = st.font;
                state.dpr       = st.dpr;
                state.maxWidth  = st.maxWidth;
                state.canAdd    = true;
                state.noButton  = true; // ChatReactions sets it for audio and voice cards
                state.hovered   = hover;
                state.hoverZone = zone;
                return rx::composeObject(img, size, v, state, nullptr, ls);
            });
        };
        const auto other = [](const char* code) { return -emoji::fromWireCode(QByteArray(code)) - 1; };
        voiceReacted(QStringLiteral("reactions_voice_row"), QStringLiteral("reactions: voice message with a row"), {{proto::Heart, 3}, {proto::Laughing, 1}},
                     1 << proto::Heart, false, rx::Zone::None);
        voiceReacted(QStringLiteral("reactions_voice_row_hover"), QStringLiteral("reactions: voice message with a row, hover (add pill)"),
                     {{proto::Heart, 3}, {proto::Laughing, 1}}, 1 << proto::Heart, true, rx::Zone::None);
        voiceReacted(QStringLiteral("reactions_voice_none_hover"), QStringLiteral("reactions: voice message hovered, none yet (no button)"), {}, 0, true,
                     rx::Zone::None);
        voiceReacted(QStringLiteral("reactions_voice_add_pill_hover"), QStringLiteral("reactions: voice message, add pill under the pointer"),
                     {{proto::ThumbsUp, 2}}, 0, true, rx::Zone::AddPill);
        voiceReacted(QStringLiteral("reactions_voice_emoji"), QStringLiteral("reactions: voice message, any emoji (HD)"),
                     {{other("1f923"), 2}, {proto::Fire, 1}, {other("1f3b5"), 3}}, 0, true, rx::Zone::None);
    }
    // 2.2 reply: the reply line that replaces a reply's quote line (found / hovered / lost original, a
    // file, a narrow chat), at the height of a 9 pt message line.
    {
        const auto replyLine = [](const QString& nick, const QString& snippet, bool media, bool found, bool hover, const QColor& nickColor) {
            return [=](const PreviewStyle& st, QSize* ls) {
                replyart::Header header;
                header.nick      = nick;
                header.snippet   = snippet;
                header.media     = media;
                header.found     = found;
                header.nickColor = nickColor;
                replyart::HeaderStyle hs;
                hs.dark       = st.dark;
                hs.base       = st.dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff);
                hs.font       = st.font;
                hs.dpr        = st.dpr;
                hs.lineHeight = 13;
                hs.hovered    = hover;
                const QSize size = replyart::headerSize(header, hs, st.maxWidth);
                *ls              = size;
                return replyart::renderHeader(header, hs, size);
            };
        };
        const QColor  blue(0x1c, 0xb0, 0xf4);
        const QString text = QStringLiteral("Anyone up for a match tonight? I'm thinking around nine, the usual server.");
        add(QStringLiteral("reply_line_found"), QStringLiteral("reply line: original in the chat"), replyLine(QStringLiteral("Alice"), text, false, true, false, blue), 520);
        add(QStringLiteral("reply_line_hover"), QStringLiteral("reply line: hovered (click goes to the original)"), replyLine(QStringLiteral("Alice"), text, false, true, true, blue), 520);
        add(QStringLiteral("reply_line_lost"), QStringLiteral("reply line: original not in the chat any more"),
            replyLine(QStringLiteral("Sara"), QStringLiteral("Did anyone see where I left the map files?"), false, false, false, QColor()), 520);
        add(QStringLiteral("reply_line_file"), QStringLiteral("reply line: to a file"), replyLine(QStringLiteral("Reza"), QStringLiteral("alpine-lake.jpg"), true, true, false, blue), 520);
        add(QStringLiteral("reply_line_narrow"), QStringLiteral("reply line: narrow chat, long name"),
            replyLine(QStringLiteral("A very long nickname indeed"), text, false, true, false, QColor(0x00, 0x2f, 0x5d)), 220);
        add(QStringLiteral("reply_line_persian"), QStringLiteral("reply line: Persian"), replyLine(QStringLiteral("مهدی"), QStringLiteral("سلام، امشب بازی داریم؟"), false, true, false, blue), 520);
        add(QStringLiteral("reply_line_mixed"), QStringLiteral("reply line: Persian and English mixed"),
            replyLine(QStringLiteral("Ali"), QStringLiteral("سلام دنیا and hello"), false, true, false, blue), 520);
    }
    return list;
}

// 2.2 drag-out: the mini card floats over other apps, always dark: both text colours need 4.5:1.
int checkDragCardContrast()
{
    int failures = 0;
    for (const QColor& text : {dragCardTitleColor(), dragCardDetailColor()}) {
        const double ratio = ui::contrastRatio(text, dragCardBackground());
        QTextStream(stdout) << "drag card " << text.name() << " on " << dragCardBackground().name() << "  " << QString::number(ratio, 'f', 2) << ":1\n";
        if (ratio < 4.5) {
            QTextStream(stderr) << "error: drag card text below 4.5:1\n";
            ++failures;
        }
    }
    return failures;
}

// Every text colour against what it is drawn on; the result is also saved to <outdir>/contrast.txt.
int contrastReport(const QString& outDir)
{
    QString     report;
    QTextStream out(&report);
    int         failures = 0;
    for (const bool dark : {false, true}) {
        out << (dark ? "dark theme\n" : "light theme\n");
        QVector<PreviewColorPair> pairs = previewColorPairs(dark) + audioCardColorPairs(dark) + albumColorPairs(dark) + voiceCardColorPairs(dark); // 2.2 audio, album, voice
        // 2.2 reactions: the row's colours on TeamSpeak's chat background
        for (const rx::ColorPair& pair : rx::colorPairs(dark, dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff)))
            pairs.append({pair.name, pair.foreground, pair.background, pair.minimum});
        for (const PreviewColorPair& pair : qAsConst(pairs)) {
            const double ratio = ui::contrastRatio(pair.foreground, pair.background);
            const bool   ok    = ratio + 0.005 >= pair.minimum;
            failures += ok ? 0 : 1;
            out << QStringLiteral("  %1 %2 on %3  %4:1 (needs %5)%6\n")
                       .arg(pair.name, -38)
                       .arg(pair.foreground.name(), pair.background.name())
                       .arg(ratio, 5, 'f', 2)
                       .arg(pair.minimum, 0, 'f', 1)
                       .arg(ok ? QString() : QStringLiteral("  FAIL"));
        }
    }
    out.flush();
    QTextStream(stdout) << report;
    QFile file(outDir + QStringLiteral("/contrast.txt"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(report.toUtf8());
    if (failures)
        QTextStream(stderr) << "error: " << failures << " colour pair(s) below their contrast minimum\n";
    return failures;
}

// displayFileName() must strip what could reorder or hide parts of a name, and nothing else.
int checkDisplayFileName()
{
    struct Case {
        QString in, out;
    };
    const QList<Case> cases = {
        {QStringLiteral("holiday.png"), QStringLiteral("holiday.png")},
        {QStringLiteral("invoice_") + QChar(0x202E) + QStringLiteral("fdp.exe"), QStringLiteral("invoice_fdp.exe")},
        {QChar(0x2067) + QStringLiteral("a") + QChar(0x2069) + QChar(0x200F) + QStringLiteral("b.jpg"), QStringLiteral("ab.jpg")},
        {QStringLiteral("line\nbreak\t.txt") + QChar(0x2028), QStringLiteral("linebreak.txt")},
        {QStringLiteral("a") + QChar(0x200C) + QStringLiteral("b") + QChar(0x200D) + QStringLiteral("c.png"), QStringLiteral("a") + QChar(0x200C) + QStringLiteral("b") + QChar(0x200D) + QStringLiteral("c.png")}, // ZWNJ/ZWJ stay
        {QStringLiteral("<b>x</b>.png"), QStringLiteral("<b>x</b>.png")}, // markup is escaped by widgets, not removed
    };
    int failures = 0;
    for (const Case& c : cases) {
        if (displayFileName(c.in) != c.out) {
            QTextStream(stderr) << "error: displayFileName(" << c.in << ") = " << displayFileName(c.in) << ", expected " << c.out << "\n";
            ++failures;
        }
    }
    return failures;
}

// 2.2 spoiler: the cover never moves the chat (same box hidden, fading and revealed) and shows nothing
// of what has loaded (with a BlurHash the cover is the same whatever still is there).
int checkSpoilerCovers()
{
    PreviewStyle st;
    st.font = QFont(QStringLiteral("Segoe UI"));
    st.font.setPointSizeF(9.0);
    int  failures = 0;
    auto fail     = [&failures](const QString& text) {
        QTextStream(stderr) << "error: spoiler: " << text << "\n";
        ++failures;
    };
    const QImage photo = loadSource(QStringLiteral("big_photo.jpg"));
    for (const char* name : {"photo.jpg", "funny.gif", "clip.mp4", "plain.png"}) {
        MediaEntry e = makeEntry(QString::fromLatin1(name), 500000, 1280, 720, 8000, MediaState::Ready);
        if (QByteArray(name) == "plain.png")
            e = makeEntry(QString::fromLatin1(name), 500000, 0, 0, 0, MediaState::Ready); // card or still-sized
        e.link.blurHash = kReferenceHash;
        const MediaStill still = pictureStill(photo, MediaStill::Full, stillPixels(e, st));
        QSize            sizes[3];
        QImage           images[3];
        for (int i = 0; i < 3; ++i) {
            PreviewStyle s   = st;
            s.concealOpacity = i * 0.5;
            images[i]        = renderPreview(e, still, s, &sizes[i]);
        }
        if (sizes[0] != sizes[1] || sizes[0] != sizes[2] || (e.link.width > 0 && sizes[0] != previewLogicalSize(e, st)))
            fail(QStringLiteral("%1 changes size when covered").arg(QString::fromLatin1(name)));
        if (e.link.width > 0) {
            PreviewStyle s   = st;
            s.concealOpacity = 1.0;
            const QImage bare = renderPreview(e, MediaStill(), s, nullptr);
            if (bare != images[2])
                fail(QStringLiteral("%1: the cover shows what has loaded").arg(QString::fromLatin1(name)));
        }
    }
    return failures;
}

struct Rendered {
    QString label;
    QImage  image;
    QSize   logical;
};

void writeSheet(const QList<Rendered>& items, bool dark, qreal dpr, const QFont& font, const QString& path)
{
    const int    columns = 3;
    const int    cellW   = 430;
    const int    margin  = 24;
    const QFontMetricsF fm(font);
    const int    labelH  = qCeil(fm.height()) + 6;

    QList<int> rowHeights;
    for (int i = 0; i < items.size(); i += columns) {
        int h = 0;
        for (int j = i; j < qMin(i + columns, items.size()); ++j)
            h = qMax(h, items[j].logical.height());
        rowHeights.append(h + labelH + 20);
    }
    int totalH = margin * 2;
    for (int h : rowHeights)
        totalH += h;

    QImage sheet(QSize(margin * 2 + columns * cellW, totalH) * dpr, QImage::Format_ARGB32_Premultiplied);
    sheet.setDevicePixelRatio(dpr);
    sheet.fill(dark ? QColor(0x31, 0x33, 0x38) : QColor(0xff, 0xff, 0xff));
    QPainter p(&sheet);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setFont(font);

    int y = margin;
    for (int row = 0; row < rowHeights.size(); ++row) {
        for (int col = 0; col < columns; ++col) {
            const int i = row * columns + col;
            if (i >= items.size())
                break;
            const int x = margin + col * cellW;
            p.setPen(dark ? QColor(0x94, 0x9b, 0xa4) : QColor(0x5c, 0x5e, 0x66));
            p.drawText(QRectF(x, y, cellW - 16, labelH), Qt::AlignLeft | Qt::AlignVCenter, items[i].label);
            p.drawImage(QRectF(QPointF(x, y + labelH), QSizeF(items[i].logical)), items[i].image);
        }
        y += rowHeights[row];
    }
    p.end();
    sheet.save(path);
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        QTextStream(stderr) << "usage: render_gallery <outdir> [<media dir>]\n";
        return 2;
    }
    const QString outDir = QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1]));
    // Pictures used in the samples (e.g. created with ffmpeg, see README): defaults to ./testmedia.
    g_mediaDir           = argc > 2 ? QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[2])) : QStringLiteral("testmedia");
    QDir().mkpath(outDir);
    if (checkDisplayFileName() != 0)
        return 1;
    if (checkSpoilerCovers() != 0) // 2.2 spoiler
        return 1;

    QFont chatFont(QStringLiteral("Segoe UI"));
    chatFont.setPointSizeF(9.0);

    const QList<Sample> samples = buildSamples();
    int                 written = 0;
    for (const bool dark : {false, true}) {
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
            const QString scale = QString::number(dpr);
            const QString sub   = QStringLiteral("%1/%2_%3x").arg(outDir, theme, scale);
            QDir().mkpath(sub);
            QList<Rendered> rendered;
            for (const Sample& s : samples) {
                PreviewStyle style;
                style.dark      = dark;
                style.font      = chatFont;
                style.dpr       = dpr;
                style.maxWidth  = s.maxWidth;
                style.maxHeight = 300;
                QSize        logical;
                const QImage img = s.render(style, &logical);
                if (img.isNull()) {
                    QTextStream(stderr) << "error: " << s.name << " rendered nothing\n";
                    continue;
                }
                img.save(QStringLiteral("%1/%2.png").arg(sub, s.name));
                ++written;
                rendered.append({s.label + QStringLiteral("  (%1x%2)").arg(logical.width()).arg(logical.height()), img, logical});
            }
            writeSheet(rendered, dark, dpr, chatFont, QStringLiteral("%1/sheet_%2_%3x.png").arg(outDir, theme, scale));
        }
    }
    QTextStream(stdout) << "rendered " << written << " previews into " << QDir::toNativeSeparators(outDir) << "\n";
    const int contrastFailures = contrastReport(outDir);
    const int dragCard         = checkDragCardContrast(); // 2.2 drag-out
    if (g_layoutFailures)
        QTextStream(stderr) << "error: " << g_layoutFailures << " sample(s) would make the chat jump\n";
    // 2.2 album: and every grid's size (g_layoutErrors)
    emoji::shutdown(); // 2.2 emoji: the renderer's worker and engines
    return contrastFailures == 0 && dragCard == 0 && g_layoutFailures == 0 && g_layoutErrors == 0 ? 0 : 1;
}
