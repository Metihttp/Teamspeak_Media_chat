#include "safeimage.h"

#include <QFile>
#include <QImageReader>
#include <QMovie>

#include "linkpreview/content.h"

namespace safeimage {

namespace {

constexpr qint64 kMaxWebpCheckBytes = 64LL * 1024 * 1024; // a WebP larger than this is not checked: refused

QByteArray head(const QString& path, int bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(bytes);
}

bool webpIsSafe(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxWebpCheckBytes)
        return false;
    return linkpreview::content::webpVerdict(file.readAll()) == linkpreview::content::WebpVerdict::Ok;
}

} // namespace

QByteArray formatOf(const QString& path, Source source)
{
    const QByteArray first = head(path, 32);
    if (first.isEmpty())
        return {};
    switch (linkpreview::content::sniff(first)) {
    case linkpreview::content::Format::Png:
        return "png";
    case linkpreview::content::Format::Jpeg:
        return "jpeg";
    case linkpreview::content::Format::Gif:
        return "gif";
    case linkpreview::content::Format::WebP:
        return webpIsSafe(path) ? QByteArray("webp") : QByteArray();
    default:
        break;
    }
    if (source != Source::Website && first.startsWith("BM") && first.size() >= 14)
        return "bmp";
    return {};
}

std::unique_ptr<QImageReader> reader(const QString& path, Source source)
{
    const QByteArray format = formatOf(path, source);
    if (format.isEmpty())
        return std::make_unique<QImageReader>(); // no device, no format: canRead() is false
    auto out = std::make_unique<QImageReader>(path, format);
    out->setAutoDetectImageFormat(false);
    out->setDecideFormatFromContent(false);
    return out;
}

QMovie* movie(const QString& path, Source source, QObject* parent)
{
    const QByteArray format = formatOf(path, source);
    if (format != "gif" && format != "webp")
        return nullptr;
    return new QMovie(path, format, parent);
}

} // namespace safeimage
