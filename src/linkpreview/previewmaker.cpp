#include "previewmaker.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

#include "announcement.h" // cleanText, kMaxPreviewBytes, kMaxTitleBytes, kMaxAuthorBytes
#include "pagemeta.h"     // layoutFor
#include "safeimage.h"
#include "textclean.h"

namespace linkpreview::maker {

namespace {

constexpr qint64 kMaxDecodePixels = sizeof(void*) >= 8 ? 80LL * 1000 * 1000 : 4096LL * 4096; // Core's limit (2.5: 16.7 MP in 32-bit TeamSpeak)
constexpr int    kMaxDimension    = 16384;
constexpr int    kJpegQuality     = 82; // Core's previews
constexpr int    kCardLargeSide   = 800;
constexpr int    kCardThumbSide   = 256;
constexpr int    kCardQuality     = 85;
constexpr int    kCardQualityLow  = 70;

Made failed(const QString& why)
{
    Made m;
    m.error = why;
    return m;
}

bool canceled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load();
}

QSize fitInto(const QSize& size, int maxSide)
{
    if (size.isEmpty())
        return {};
    if (size.width() <= maxSide && size.height() <= maxSide)
        return size;
    return size.scaled(QSize(maxSide, maxSide), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

// A picture file decoded (its first frame) at most maxSide, or a null image with *why set. A website's file: its
// format is taken from its bytes and forced (safeimage.h: never SVG, XPM or a lossless WebP).
QImage decode(const QString& path, int maxSide, QSize* fullSize, QString* why)
{
    const std::unique_ptr<QImageReader> safe = safeimage::reader(path, safeimage::Source::Website);
    if (!safe || !safe->canRead()) {
        *why = QStringLiteral("not a picture the plugin decodes");
        return {};
    }
    QImageReader& reader = *safe;
    reader.setAutoTransform(true);
    const QSize  raw    = reader.size();
    const qint64 pixels = static_cast<qint64>(raw.width()) * raw.height();
    if (!raw.isValid() || raw.isEmpty()) {
        *why = QStringLiteral("not a picture");
        return {};
    }
    if (pixels > kMaxDecodePixels || raw.width() > kMaxDimension || raw.height() > kMaxDimension) {
        *why = QStringLiteral("picture too large to decode");
        return {};
    }
    const bool  rotated = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    const QSize shown   = rotated ? raw.transposed() : raw;
    const QSize target  = fitInto(shown, maxSide);
    if (target != shown)
        reader.setScaledSize(rotated ? target.transposed() : target);
    QImage image = reader.read();
    if (image.isNull()) {
        *why = QStringLiteral("picture could not be decoded");
        return {};
    }
    if (image.size() != target)
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (fullSize)
        *fullSize = shown;
    return image;
}

QByteArray encode(const QImage& image, const char* format, int quality = kJpegQuality)
{
    QByteArray data;
    QBuffer    buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImageWriter writer(&buffer, format);
    if (qstrcmp(format, "jpg") == 0) {
        writer.setQuality(quality);
        writer.setOptimizedWrite(true);
    }
    QImage source = image;
    source.setDevicePixelRatio(1.0);
    if (!writer.write(source))
        return {};
    buffer.close();
    return data;
}

// JPEG (alpha flattened, like Core's previews) or PNG, smaller until it is within kMaxPreviewBytes.
QByteArray encodePreview(QImage image, bool png)
{
    for (int attempt = 0; attempt < 4 && !image.isNull(); ++attempt) {
        QByteArray bytes;
        if (png) {
            bytes = encode(image.convertToFormat(QImage::Format_ARGB32), "png");
        } else {
            QImage rgb(image.size(), QImage::Format_RGB32);
            rgb.fill(Qt::black);
            QPainter painter(&rgb);
            painter.drawImage(0, 0, image);
            painter.end();
            bytes = encode(rgb, "jpg");
        }
        if (!bytes.isEmpty() && bytes.size() <= kMaxPreviewBytes)
            return bytes;
        image = image.scaled(image.size() * 0.7, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {};
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

// The fetched file under our name (moved when it can be: it is ours, in the same folder tree).
QString takeMedia(const QString& from, const QString& workDir, Format format, bool mayMove)
{
    const QString to = workDir + QStringLiteral("/link.") + extensionOf(format);
    QFile::remove(to);
    if ((mayMove && QFile::rename(from, to)) || QFile::copy(from, to))
        return to;
    return {};
}

bool setPreview(Made& made, const QImage& image, bool png, const QString& workDir)
{
    made.previewBytes = encodePreview(image, png);
    if (made.previewBytes.isEmpty())
        return false;
    made.previewSha  = QCryptographicHash::hash(made.previewBytes, QCryptographicHash::Sha256);
    made.previewFile = workDir + (png ? QStringLiteral("/linkpreview.png") : QStringLiteral("/linkpreview.jpg"));
    return writeFile(made.previewFile, made.previewBytes);
}

Made makeYouTube(const Input& in, const std::atomic<bool>* cancel)
{
    const QString thumb  = in.fetched.thumbnailFile;
    const Format  format = sniffFile(thumb);
    if (format != Format::Jpeg && format != Format::Png && format != Format::Webp)
        return failed(QStringLiteral("the thumbnail is not a picture"));
    QString why;
    QSize   full;
    QImage  image = decode(thumb, in.caps.youTubeMaxSide, &full, &why);
    if (image.isNull())
        return failed(why);
    if (canceled(cancel))
        return failed(QStringLiteral("canceled"));
    // hqdefault is 480 x 360 with the 16:9 picture between black bars: only the picture goes up (the card is
    // 16:9 too). 2.5 zoom: maxresdefault is 16:9 already (crop16x9 leaves it as it is).
    const QRect crop = crop16x9(image.size());
    if (crop.size() != image.size()) {
        image = image.copy(crop);
        full  = QSize(full.width(), qMax(1, qRound(full.width() * 9.0 / 16.0)));
    }
    Made made;
    made.kind   = Kind::YouTube;
    made.width  = full.width();
    made.height = full.height();
    made.title  = cleanText(in.fetched.title, kMaxTitleBytes);
    made.author = cleanText(in.fetched.author, kMaxAuthorBytes);
    if (!setPreview(made, image, false, in.workDir))
        return failed(QStringLiteral("the preview could not be written"));
    made.ok = true;
    return made;
}

Made makePicture(const Input& in, Format format, const std::atomic<bool>* cancel)
{
    const QString file   = in.fetched.localFile;
    const qint64  size   = QFileInfo(file).size();
    // 2.5: an animated WebP of a media page (a Giphy sticker) goes up as a GIF-kind WebP (LPX k=g).
    const bool    gif    = format == Format::Gif || (in.target.isNewClass() && in.fetched.kind == Kind::Gif && format == Format::Webp);
    QString       why;
    QSize         full;
    const QImage  image  = decode(file, gif ? in.caps.thumbMaxSide : in.caps.previewMaxSide, &full, &why);
    if (image.isNull())
        return failed(why);
    if (canceled(cancel))
        return failed(QStringLiteral("canceled"));
    Made made;
    made.kind      = gif ? Kind::Gif : Kind::Image;
    made.width     = full.width();
    made.height    = full.height();
    made.sensitive = in.target.isNewClass() && in.fetched.card.sensitive; // D24: from a page that declares adult content (LPX sn=1)
    const qint64 cap = gif ? in.caps.gifMediaBytes : in.caps.imageMediaBytes;
    const bool   withMedia = size > 0 && size <= cap && size <= kMaxMediaBytes;
    // Only a preview going up keeps its transparency (PNG); next to the file it is a JPEG (Core's previews).
    const bool png = !withMedia && !gif && image.hasAlphaChannel();
    if (!setPreview(made, image, png, in.workDir))
        return failed(QStringLiteral("the preview could not be written"));
    if (withMedia) {
        made.mediaFile = takeMedia(file, in.workDir, format, in.mayMove);
        made.mediaSize = made.mediaFile.isEmpty() ? 0 : QFileInfo(made.mediaFile).size();
        if (made.mediaFile.isEmpty() || made.mediaSize != size)
            return failed(QStringLiteral("the file could not be prepared"));
    }
    made.ok = true;
    return made;
}

Made makeVideo(const Input& in, Format format, const std::atomic<bool>* cancel)
{
    const QString file = in.fetched.localFile;
    const qint64  size = QFileInfo(file).size();
    const LocalMediaInfo info = in.probeVideo ? in.probeVideo(file) : probeLocalVideo(file, true);
    if (canceled(cancel))
        return failed(QStringLiteral("canceled"));
    if (info.preview.isNull())
        return failed(QStringLiteral("no frame of the video could be read"));
    QImage poster = info.preview;
    const QSize fit = fitInto(poster.size(), in.caps.posterMaxSide);
    if (fit != poster.size())
        poster = poster.scaled(fit, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // 2.5: a media page's silent looping video (MP4 or WebM) goes up like a GIF (its cap) and plays like one.
    const bool loop = in.target.isNewClass() && in.fetched.kind == Kind::Loop && format != Format::Mov;
    Made made;
    made.kind       = loop ? Kind::Loop : Kind::Video;
    made.sensitive  = in.target.isNewClass() && in.fetched.card.sensitive; // D24 (LPX sn=1)
    made.width      = info.width > 0 && info.width <= kMaxSide && info.height > 0 && info.height <= kMaxSide ? info.width : 0;
    made.height     = made.width > 0 ? info.height : 0;
    made.durationMs = qBound<qint64>(0, info.durationMs, kMaxDurationMs);
    if (!setPreview(made, poster, false, in.workDir))
        return failed(QStringLiteral("the preview could not be written"));
    const qint64 cap = loop ? in.caps.gifMediaBytes : in.caps.videoMediaBytes;
    if (size > 0 && size <= cap && size <= kMaxMediaBytes) {
        made.mediaFile = takeMedia(file, in.workDir, format, in.mayMove);
        made.mediaSize = made.mediaFile.isEmpty() ? 0 : QFileInfo(made.mediaFile).size();
        if (made.mediaFile.isEmpty() || made.mediaSize != size)
            return failed(QStringLiteral("the file could not be prepared"));
    }
    made.ok = true;
    return made;
}

// 2.5: a card: its text cleaned to the wire's limits, its picture made anew (or none).
Made makeCard(const Input& in, const std::atomic<bool>* cancel)
{
    const CardData& from = in.fetched.card;
    Made            made;
    made.kind             = Kind::Card;
    made.sensitive        = from.sensitive;
    CardData& card        = made.card;
    card.title            = textclean::cleanToBytes(from.title, kCardTitleBytes);
    card.author           = textclean::cleanToBytes(from.author, kCardAuthorBytes);
    card.site             = textclean::cleanToBytes(from.site, kCardSiteBytes);
    card.description      = textclean::cleanToBytes(from.description, kCardDescriptionBytes);
    card.hasColor         = from.hasColor && pagemeta::isAcceptableThemeColour(from.color);
    card.color            = card.hasColor ? from.color : 0;
    card.play             = from.play;
    card.sensitive        = from.sensitive;
    card.layout           = from.layout;
    if (card.title.isEmpty())
        return failed(QStringLiteral("a card without a title"));
    const QString picture = from.pictureFile;
    if (!picture.isEmpty() && QFileInfo(picture).isFile()) {
        QString why;
        QSize   full;
        CardLayout layout = card.layout;
        if (layout == CardLayout::None)
            layout = pagemeta::layoutFor(QString(), true, from.pictureWidth, from.pictureHeight);
        const QImage image = decode(picture, layout == CardLayout::Thumb ? kCardThumbSide : kCardLargeSide, &full, &why);
        if (canceled(cancel))
            return failed(QStringLiteral("canceled"));
        bool             png   = false;
        const QByteArray bytes = image.isNull() ? QByteArray() : encodeCardPicture(image, layout, &png);
        if (!bytes.isEmpty()) {
            made.previewBytes = bytes;
            made.previewSha   = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
            made.previewFile  = in.workDir + (png ? QStringLiteral("/linkpreview.png") : QStringLiteral("/linkpreview.jpg"));
            if (!writeFile(made.previewFile, bytes))
                return failed(QStringLiteral("the preview could not be written"));
            made.width  = full.width() <= kMaxSide && full.height() <= kMaxSide ? full.width() : 0;
            made.height = made.width > 0 ? full.height() : 0;
            card.layout = layout;
        }
    }
    if (made.previewFile.isEmpty()) {
        card.layout = CardLayout::None;
        card.play   = false;
        if (card.description.isEmpty())
            return failed(QStringLiteral("a card without its picture needs its text"));
    }
    made.durationMs = card.play ? qBound<qint64>(0, from.durationMs, kMaxDurationMs) : 0;
    made.ok         = true;
    return made;
}

} // namespace

QByteArray encodeCardPicture(const QImage& picture, CardLayout layout, bool* png)
{
    if (picture.isNull())
        return {};
    const int side  = layout == CardLayout::Thumb ? kCardThumbSide : kCardLargeSide;
    QImage    image = picture;
    const QSize fit = fitInto(image.size(), side);
    if (fit != image.size())
        image = image.scaled(fit, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const bool alpha = image.hasAlphaChannel();
    if (png)
        *png = alpha;
    for (int attempt = 0; attempt < 4 && !image.isNull(); ++attempt) {
        QByteArray bytes;
        if (alpha) {
            bytes = encode(image.convertToFormat(QImage::Format_ARGB32), "png");
        } else {
            bytes = encode(image.convertToFormat(QImage::Format_RGB32), "jpg", attempt == 0 ? kCardQuality : kCardQualityLow);
        }
        if (!bytes.isEmpty() && bytes.size() <= kMaxPreviewBytes)
            return bytes;
        if (attempt >= 1)
            image = image.scaled(image.size() * 0.75, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {};
}

QRect crop16x9(const QSize& size)
{
    if (size.isEmpty())
        return {};
    // Taller than 16:9 (a 4:3 YouTube thumbnail): the middle band; wider ones stay as they are.
    const int height = qMax(1, qRound(size.width() * 9.0 / 16.0));
    if (height >= size.height())
        return QRect(QPoint(0, 0), size);
    return QRect(0, (size.height() - height) / 2, size.width(), height);
}

Format sniff(const QByteArray& head)
{
    const auto at = [&head](int offset, const char* magic) {
        const int n = static_cast<int>(qstrlen(magic));
        return head.size() >= offset + n && head.mid(offset, n) == QByteArray(magic, n);
    };
    if (at(0, "\x89PNG\r\n\x1a\n"))
        return Format::Png;
    if (head.size() >= 3 && static_cast<uchar>(head.at(0)) == 0xff && static_cast<uchar>(head.at(1)) == 0xd8 && static_cast<uchar>(head.at(2)) == 0xff)
        return Format::Jpeg;
    if (at(0, "GIF87a") || at(0, "GIF89a"))
        return Format::Gif;
    if (at(0, "RIFF") && at(8, "WEBP"))
        return Format::Webp;
    if (head.size() >= 4 && static_cast<uchar>(head.at(0)) == 0x1a && static_cast<uchar>(head.at(1)) == 0x45 && static_cast<uchar>(head.at(2)) == 0xdf
        && static_cast<uchar>(head.at(3)) == 0xa3)
        return Format::WebM;
    if (at(4, "ftyp"))
        return at(8, "qt  ") ? Format::Mov : Format::Mp4;
    return Format::Unknown;
}

Format sniffFile(const QString& path)
{
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return Format::Unknown;
    return sniff(file.read(32));
}

QString extensionOf(Format format)
{
    switch (format) {
    case Format::Png:
        return QStringLiteral("png");
    case Format::Jpeg:
        return QStringLiteral("jpg");
    case Format::Gif:
        return QStringLiteral("gif");
    case Format::Webp:
        return QStringLiteral("webp");
    case Format::Mp4:
        return QStringLiteral("mp4");
    case Format::Mov:
        return QStringLiteral("mov");
    case Format::WebM:
        return QStringLiteral("webm");
    default:
        return {};
    }
}

Made make(const Input& in, const std::atomic<bool>* cancel)
{
    try {
        if (!in.fetched.ok)
            return failed(QStringLiteral("not fetched"));
        if (in.workDir.isEmpty() || !QFileInfo(in.workDir).isDir())
            return failed(QStringLiteral("no work folder"));
        if (in.target.kind == Kind::YouTube)
            return makeYouTube(in, cancel);
        // 2.5: a page link is what its load turned out to be (by its bytes), not what the link suggested.
        const Kind kind = in.target.isNewClass() ? in.fetched.kind : in.target.kind;
        if (kind == Kind::Card)
            return makeCard(in, cancel);
        const bool   moving = kind == Kind::Video || kind == Kind::Loop;
        const Format format = sniffFile(in.fetched.localFile);
        switch (format) {
        case Format::Png:
        case Format::Jpeg:
        case Format::Gif:
        case Format::Webp:
            if (moving)
                return failed(QStringLiteral("a picture where a video was expected"));
            return makePicture(in, format, cancel);
        case Format::Mp4:
        case Format::Mov:
        case Format::WebM:
            if (!moving)
                return failed(QStringLiteral("a video where a picture was expected"));
            return makeVideo(in, format, cancel);
        default:
            return failed(QStringLiteral("unknown file type"));
        }
    } catch (...) {
        return failed(QStringLiteral("unexpected error"));
    }
}

} // namespace linkpreview::maker
