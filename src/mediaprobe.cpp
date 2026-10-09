#include "mediaprobe.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>

#include "blurhash.h"
#include "video/mfvideo.h"

namespace {

constexpr qint64 kMaxDecodePixels       = 80'000'000;
constexpr qint64 kStillPreviewMinBytes  = 1536 * 1024; // 1.5 MB
constexpr int    kStillPreviewMinSide   = 2560;
constexpr int    kStillPreviewMaxSide   = 1280;
constexpr qint64 kAnimPreviewMinBytes   = 4 * 1024 * 1024;
constexpr int    kAnimPreviewMaxSide    = 640;
constexpr int    kPosterMaxSide         = 960;
constexpr int    kBlurHashSourceMaxSide = 32;

QSize fitWithin(const QSize& size, int maxSide)
{
    if (size.width() <= maxSide && size.height() <= maxSide)
        return size;
    return size.scaled(maxSide, maxSide, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

QImage downscale(const QImage& image, int maxSide)
{
    const QSize target = fitWithin(image.size(), maxSide);
    return target == image.size() ? image : image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString blurHashOf(const QImage& image)
{
    if (image.isNull())
        return {};
    return blurhash::encode(downscale(image, kBlurHashSourceMaxSide), 4, 3);
}

// Decodes the first image of a file with EXIF orientation applied, at most maxSide pixels on the
// longest side (formats that support it, like JPEG, decode directly at the reduced size).
QImage decodeScaled(const QString& path, const QSize& rawSize, int maxSide)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // The scaled size applies before the EXIF rotation; fitting into a square box is rotation-safe.
    if (rawSize.isValid() && reader.supportsOption(QImageIOHandler::ScaledSize)) {
        const QSize target = fitWithin(rawSize, maxSide);
        if (target != rawSize)
            reader.setScaledSize(target);
    }
    const QImage image = reader.read();
    return image.isNull() ? image : downscale(image, maxSide);
}

void probeImage(const QString& path, qint64 fileBytes, bool generatePreviews, LocalMediaInfo& info)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize      raw      = reader.size();
    const bool rotated  = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    const bool animated = reader.supportsAnimation() && reader.imageCount() != 1;

    QImage decoded; // only for formats that cannot report their size without decoding
    if (!raw.isValid()) {
        QImageReader full(path);
        full.setAutoTransform(true);
        decoded = full.read();
        if (decoded.isNull())
            return;
        raw = rotated ? decoded.size().transposed() : decoded.size();
    }

    const QSize display = rotated ? raw.transposed() : raw;
    info.kind           = animated ? MediaKind::AnimatedImage : MediaKind::Image;
    info.animated       = animated;
    info.width          = display.width();
    info.height         = display.height();

    if (static_cast<qint64>(raw.width()) * raw.height() > kMaxDecodePixels)
        return; // too large to decode safely: receivers get a card with the size

    const int  longest = std::max(display.width(), display.height());
    const bool preview = generatePreviews
                         && (animated ? fileBytes > kAnimPreviewMinBytes
                                      : fileBytes > kStillPreviewMinBytes || longest > kStillPreviewMinSide);
    const int  maxSide = !preview ? kBlurHashSourceMaxSide : animated ? kAnimPreviewMaxSide : kStillPreviewMaxSide;

    const QImage image = decoded.isNull() ? decodeScaled(path, raw, maxSide) : downscale(decoded, maxSide);
    if (image.isNull())
        return;
    info.blurHash = blurHashOf(image);
    if (preview)
        info.preview = image;
}

void probeVideo(const QString& path, bool generatePreviews, LocalMediaInfo& info)
{
    const mf::ProbeResult probe = mf::probe(path, kPosterMaxSide);
    if (!probe.ok)
        return;
    info.durationMs = std::max<qint64>(0, probe.durationMs);
    if (!probe.hasVideo) {
        if (probe.hasAudio)
            info.kind = MediaKind::Audio; // e.g. an .mp4 with only a sound track
        return;
    }

    const QSize size = probe.size.isValid() && !probe.size.isEmpty() ? probe.size : probe.poster.size();
    info.width       = std::max(0, size.width());
    info.height      = std::max(0, size.height());
    if (probe.poster.isNull())
        return;
    const QImage poster = downscale(probe.poster, kPosterMaxSide);
    info.blurHash       = blurHashOf(poster);
    if (generatePreviews)
        info.preview = poster;
}

void probeAudio(const QString& path, LocalMediaInfo& info)
{
    const mf::ProbeResult probe = mf::probe(path, kPosterMaxSide);
    if (probe.ok)
        info.durationMs = std::max<qint64>(0, probe.durationMs);
}

} // namespace

LocalMediaInfo probeLocalMedia(const QString& path, bool generatePreviews)
{
    LocalMediaInfo info;
    try {
        const QFileInfo file(path);
        info.kind = kindForFileName(file.fileName());
        if (!file.isFile())
            return info;

        switch (info.kind) {
        case MediaKind::Image:
        case MediaKind::AnimatedImage:
            probeImage(path, file.size(), generatePreviews, info);
            break;
        case MediaKind::Video:
            probeVideo(path, generatePreviews, info);
            break;
        case MediaKind::Audio:
            probeAudio(path, info);
            break;
        default:
            break;
        }
    } catch (...) {
        // Metadata is optional: the file is still uploaded and posted without it.
    }
    return info;
}

QByteArray encodePreviewJpeg(const QImage& image)
{
    if (image.isNull())
        return {};

    QImage source = image;
    source.setDevicePixelRatio(1.0);
    QImage rgb;
    if (source.hasAlphaChannel()) {
        rgb = QImage(source.size(), QImage::Format_RGB32);
        if (rgb.isNull())
            return {};
        rgb.fill(Qt::black);
        QPainter painter(&rgb);
        painter.drawImage(0, 0, source);
    } else {
        rgb = source.convertToFormat(QImage::Format_RGB32);
    }

    QByteArray data;
    QBuffer    buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImageWriter writer(&buffer, "jpg");
    writer.setQuality(82);
    writer.setOptimizedWrite(true);
    if (!writer.write(rgb))
        return {};
    buffer.close();
    return data;
}
