// render_gallery <outdir> [<media dir>]
//
// Renders every state of the chat previews (previewrenderer.cpp) without TeamSpeak or a Core
// instance: entries and stills are built by hand the way Core would produce them. Writes one PNG
// per state into <outdir>/<theme>_<dpr>x/ and a labelled contact sheet per theme and dpr
// (<outdir>/sheet_<theme>_<dpr>x.png). Persian variants are rendered for a few states.

#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QTextStream>
#include <QtMath>

#include <functional>

#include "blurhash.h"
#include "i18n.h"
#include "previewrenderer.h"

namespace {

// Example hash from the BlurHash reference implementation (https://github.com/woltapp/blurhash).
const QString kReferenceHash = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj");

QString g_mediaDir;

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
    bool                                                     persian = false;
    int                                                      maxWidth = 400;
    std::function<QImage(const PreviewStyle&, QSize*)>        render;
};

QList<Sample> buildSamples()
{
    QList<Sample> list;
    auto add = [&list](const QString& name, const QString& label, std::function<QImage(const PreviewStyle&, QSize*)> fn, bool persian = false, int maxWidth = 400) {
        Sample s;
        s.name     = name;
        s.label    = label;
        s.persian  = persian;
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
    }, false, 220);

    // ---- cards -----------------------------------------------------------------------------
    const MediaEntry zip = makeEntry(QStringLiteral("project_files.zip"), 24700, 0, 0, 0, MediaState::Idle);
    auto card = [&add](const QString& name, const QString& label, const MediaEntry& e, bool persian = false, int maxWidth = 400) {
        add(name, label, [e](const PreviewStyle& st, QSize* ls) { return renderPreview(e, MediaStill(), st, ls); }, persian, maxWidth);
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
    card(QStringLiteral("card_long_name"), QStringLiteral("long file name, narrow"), makeEntry(QStringLiteral("a_really_long_file_name_that_does_not_fit_anywhere_final_v2.tar.gz"), 734003200, 0, 0, 0, MediaState::Idle), false, 240);

    // ---- Persian ---------------------------------------------------------------------------
    card(QStringLiteral("fa_card_downloading"), QStringLiteral("FA card downloading"), downloading(makeEntry(QStringLiteral("پروژه.zip"), 24700000, 0, 0, 0, MediaState::Idle), 0.62), true);
    card(QStringLiteral("fa_card_permission"), QStringLiteral("FA card failed: permission"), failed(zip, MediaError::Permission), true);
    card(QStringLiteral("fa_card_ready"), QStringLiteral("FA card ready"), makeEntry(QStringLiteral("گزارش نهایی.pdf"), 1830000, 0, 0, 0, MediaState::Ready), true);
    add(QStringLiteral("fa_image_failed"), QStringLiteral("FA image failed: deleted"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = failed(photoReady, MediaError::NotFound);
        return renderPreview(e, hashStill(photoHash, 4000, 3000, stillPixels(e, st)), st, ls);
    }, true);
    add(QStringLiteral("fa_image_preview_only"), QStringLiteral("FA preview-only big image"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e      = makeEntry(QStringLiteral("huge_panorama.jpg"), 25794560, 4000, 3000, 0, MediaState::Idle);
        e.tooLargeForAuto = true;
        return renderPreview(e, pictureStill(fitScaled(photo, QSize(1280, 1280)), MediaStill::Preview, stillPixels(e, st)), st, ls);
    }, true);
    add(QStringLiteral("fa_video_idle"), QStringLiteral("FA video poster, not downloaded"), [=](const PreviewStyle& st, QSize* ls) {
        return renderPreview(clip, posterStill(clip, st), st, ls);
    }, true);
    add(QStringLiteral("fa_video_playing"), QStringLiteral("FA video playing with controls"), [=](const PreviewStyle& st, QSize* ls) {
        MediaEntry e = clip;
        e.state      = MediaState::Ready;
        PlaybackOverlay o;
        o.controlsVisible = true;
        o.playing         = true;
        o.positionMs      = 3200;
        o.durationMs      = 10000;
        return renderVideo(e, frameAt(e, st), MediaStill(), o, st, ls);
    }, true);
    add(QStringLiteral("fa_video_no_poster"), QStringLiteral("FA plain link video, no poster"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("ویدیو تولد.mp4"), 355293, 0, 0, 0, MediaState::Idle);
        return renderPreview(e, MediaStill(), st, ls);
    }, true);
    // Latin names starting with digits keep their order in the RTL UI (not "Track_48213.mp3_-_01").
    card(QStringLiteral("fa_card_digits_audio"), QStringLiteral("FA card 01_-_Track_48213.mp3"), makeEntry(QStringLiteral("01_-_Track_48213.mp3"), 4120000, 0, 0, 0, MediaState::Idle), true);
    card(QStringLiteral("fa_card_digits_pdf"), QStringLiteral("FA card 2024_report_12345.pdf"), makeEntry(QStringLiteral("2024_report_12345.pdf"), 1830000, 0, 0, 0, MediaState::Ready), true);
    add(QStringLiteral("fa_video_digits_no_poster"), QStringLiteral("FA video 2024-05-01_12-00-00.mkv, no poster"), [=](const PreviewStyle& st, QSize* ls) {
        const MediaEntry e = makeEntry(QStringLiteral("2024-05-01_12-00-00.mkv"), 355293, 0, 0, 0, MediaState::Idle);
        return renderPreview(e, MediaStill(), st, ls);
    }, true);
    // A name with a RIGHT-TO-LEFT OVERRIDE must not fake its extension ("invoice_exe.pdf" look).
    card(QStringLiteral("card_bidi_override"), QStringLiteral("name with U+202E (shown without it)"),
         makeEntry(QStringLiteral("invoice_") + QChar(0x202E) + QStringLiteral("fdp.exe"), 88000, 0, 0, 0, MediaState::Idle));
    return list;
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
        {QStringLiteral("پیش") + QChar(0x200C) + QStringLiteral("نمایش.png"), QStringLiteral("پیش") + QChar(0x200C) + QStringLiteral("نمایش.png")},
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
            QList<Rendered> en, fa;
            for (const Sample& s : samples) {
                i18n::setLanguage(s.persian ? Language::Persian : Language::English);
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
                (s.persian ? fa : en).append({s.label + QStringLiteral("  (%1x%2)").arg(logical.width()).arg(logical.height()), img, logical});
            }
            i18n::setLanguage(Language::English);
            writeSheet(en, dark, dpr, chatFont, QStringLiteral("%1/sheet_%2_%3x.png").arg(outDir, theme, scale));
            writeSheet(fa, dark, dpr, chatFont, QStringLiteral("%1/sheet_fa_%2_%3x.png").arg(outDir, theme, scale));
        }
    }
    QTextStream(stdout) << "rendered " << written << " previews into " << QDir::toNativeSeparators(outDir) << "\n";
    return 0;
}
