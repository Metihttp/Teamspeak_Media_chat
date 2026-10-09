#include "composemodel.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

#include "i18n.h"

namespace compose {

namespace {

QString dot()
{
    return QStringLiteral(" · ");
}

bool isVideo(const Item& item)
{
    return item.kind == MediaKind::Video;
}

bool isPicture(const Item& item)
{
    return isPreviewableImage(item.kind);
}

// "%1 images" / "%1 videos" / "%1 pictures and videos" for a set of album items.
QString albumNoun(const QVector<Item>& items, const QVector<int>& members)
{
    bool pictures = false;
    bool videos   = false;
    for (int i : members) {
        pictures = pictures || isPicture(items.at(i));
        videos   = videos || isVideo(items.at(i));
    }
    if (pictures && !videos)
        return i18n::t("%1 images").arg(members.size());
    if (videos && !pictures)
        return i18n::t("%1 videos").arg(members.size());
    return i18n::t("%1 pictures and videos").arg(members.size());
}

// The name Core gives the file on the server (Core::makeRemoteName), with a made-up random part.
QString estimatedRemoteName(const Item& item)
{
    const QString suffix = QStringLiteral("_3f9a1c2e");
    if (item.isPasted())
        return QStringLiteral("new_photo") + suffix + QStringLiteral(".png");
    const QFileInfo fi(item.fileName);
    QString         base = fi.completeBaseName();
    const QString   ext  = fi.suffix().toLower();
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\[\]%#&+=]+)")), QStringLiteral("_"));
    base.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral("_"));
    base = boundRemoteBase(base);
    if (base.isEmpty())
        base = QStringLiteral("file");
    return base + suffix + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
}

QString randomHex8()
{
    return QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
}

} // namespace

Problem checkFile(bool exists, bool isFile, qint64 size, qint64 limitBytes)
{
    if (!exists || !isFile)
        return Problem::Missing;
    if (size <= 0)
        return Problem::Empty;
    if (size > limitBytes)
        return Problem::TooLarge;
    return Problem::None;
}

QString problemText(Problem problem, int limitMB)
{
    switch (problem) {
    case Problem::Missing:
        return i18n::t("Can't find this file. It may have been moved or deleted.");
    case Problem::Empty:
        return i18n::t("This file is empty");
    case Problem::TooLarge:
        return i18n::t("Over your %1 MB upload limit").arg(limitMB);
    case Problem::Unreadable:
        return i18n::t("Can't read this file. It may be open in another program.");
    case Problem::None:
        break;
    }
    return {};
}

bool spoilerAllowed(MediaKind kind)
{
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

bool isAlbumKind(const Item& item)
{
    // Core::send decides by the file name; a pasted picture is written as .png or .jpg.
    const MediaKind kind = item.isPasted() ? MediaKind::Image : kindForFileName(item.fileName);
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

QString displayName(const Item& item)
{
    return item.isPasted() ? i18n::t("Pasted image") : displayFileName(item.fileName);
}

QString typeText(const Item& item)
{
    if (item.isPasted())
        return i18n::t("Pasted image");
    // In English, not the Windows name of the file type.
    const QString ext = QFileInfo(item.fileName).suffix().toUpper();
    switch (item.kind) {
    case MediaKind::Image:
    case MediaKind::AnimatedImage:
        return i18n::t("%1 image").arg(ext);
    case MediaKind::Video:
        return i18n::t("%1 video").arg(ext);
    case MediaKind::Audio:
        return i18n::t("%1 audio").arg(ext);
    case MediaKind::Archive:
        return i18n::t("%1 archive").arg(ext);
    case MediaKind::Document:
        return i18n::t("%1 document").arg(ext);
    case MediaKind::Other:
        break;
    }
    return ext.isEmpty() ? i18n::t("File") : i18n::t("%1 file").arg(ext);
}

QString metaText(const Item& item)
{
    // 2.2 editor: an edited item says so first.
    const QString edited = item.edited ? i18n::t("Edited") + dot() : QString();
    if (item.isPasted()) // its name already says "Pasted image"
        return edited + (item.pixels.isValid() && !item.pixels.isEmpty() ? i18n::t("%1 × %2").arg(item.pixels.width()).arg(item.pixels.height()) : typeText(item));
    QString text = edited + typeText(item);
    if (isVideo(item) && item.durationMs > 0)
        text += dot() + formatDuration(item.durationMs);
    text += dot() + formatSize(static_cast<quint64>(qMax<qint64>(0, item.size)));
    if (isPicture(item) && item.pixels.isValid() && !item.pixels.isEmpty())
        text += dot() + i18n::t("%1 × %2").arg(item.pixels.width()).arg(item.pixels.height());
    return text;
}

QString accessibleName(const Item& item)
{
    QString name = displayName(item) + QStringLiteral(", ");
    if (item.isPasted())
        name += metaText(item);
    else
        name += typeText(item) + QStringLiteral(", ") + formatSize(static_cast<quint64>(qMax<qint64>(0, item.size)));
    if (item.spoiler)
        name += i18n::t(", spoiler");
    if (item.edited) // 2.2 editor
        name += i18n::t(", edited");
    return name;
}

int sendableCount(const QVector<Item>& items)
{
    return static_cast<int>(std::count_if(items.cbegin(), items.cend(), [](const Item& item) { return item.canSend(); }));
}

QString sendButtonText(const QVector<Item>& items)
{
    int count    = 0;
    int pictures = 0;
    int videos   = 0;
    for (const Item& item : items) {
        if (!item.canSend())
            continue;
        ++count;
        pictures += isPicture(item) ? 1 : 0;
        videos += isVideo(item) ? 1 : 0;
    }
    if (count <= 1)
        return i18n::t("Send");
    if (pictures == count)
        return i18n::t("Send %1 images").arg(count);
    if (videos == count)
        return i18n::t("Send %1 videos").arg(count);
    return i18n::t("Send %1 files").arg(count);
}

QString skippedText(const QVector<Item>& items)
{
    int  problems = 0;
    bool tooLarge = false;
    for (const Item& item : items) {
        if (!item.canSend()) {
            ++problems;
            tooLarge = tooLarge || item.problem == Problem::TooLarge;
        }
    }
    if (problems == 0 || items.size() == 1) // a single item says it in its own place
        return {};
    QString text;
    if (problems == items.size())
        text = i18n::t("None of these files can be sent.");
    else if (problems == 1)
        text = i18n::t("1 file can't be sent and will be skipped.");
    else
        text = i18n::t("%1 files can't be sent and will be skipped.").arg(problems);
    if (tooLarge)
        text += QLatin1Char(' ') + i18n::t("You can raise the limit in Settings → Sending.");
    return text;
}

int albumCandidates(const QVector<Item>& items)
{
    return static_cast<int>(std::count_if(items.cbegin(), items.cend(), [](const Item& item) { return item.canSend() && isAlbumKind(item); }));
}

QString albumHint(const QVector<Item>& items)
{
    QVector<int> members;
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).canSend() && isAlbumKind(items.at(i)))
            members.append(i);
    }
    const int count = members.size();
    if (count <= MediaLink::kMaxAlbumItems)
        return {};
    // Core::send: chunks of kMaxAlbumItems in order; a last chunk of one is sent on its own.
    QStringList sizes;
    for (int at = 0; at < count; at += MediaLink::kMaxAlbumItems)
        sizes << QString::number(qMin(MediaLink::kMaxAlbumItems, count - at));
    const bool    single = sizes.last() == QLatin1String("1");
    if (single)
        sizes.removeLast();
    const QString noun = albumNoun(items, members);
    if (single && sizes.size() == 1)
        return i18n::t("%1 will be sent as an album of %2 and one on its own.").arg(noun, sizes.first());
    if (single)
        return i18n::t("%1 will be sent as %2 albums (%3) and one on its own.").arg(noun).arg(sizes.size()).arg(sizes.join(QStringLiteral(" + ")));
    return i18n::t("%1 will be sent as %2 albums (%3).").arg(noun).arg(sizes.size()).arg(sizes.join(QStringLiteral(" + ")));
}

QVector<int> postOrder(const QVector<Item>& items, bool album)
{
    QVector<int> media;
    QVector<int> rest;
    for (int i = 0; i < items.size(); ++i) {
        if (!items.at(i).canSend())
            continue;
        if (album && isAlbumKind(items.at(i)))
            media.append(i);
        else
            rest.append(i);
    }
    if (media.size() < 2) { // no album: everything in its place
        QVector<int> all;
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).canSend())
                all.append(i);
        }
        return all;
    }
    return media + rest;
}

QString captionCounterText(int length)
{
    if (length < kCaptionCounterFrom)
        return {};
    return i18n::t("%1 left").arg(qMax(0, kCaptionMaxChars - length));
}

MediaLink estimatedLink(const Item& item, const LinkContext& context, bool inAlbum, int albumCount)
{
    MediaLink link;
    link.host      = context.host;
    link.port      = context.port;
    link.serverUid = context.serverUid;
    link.channelId = context.channelId;
    link.path      = context.remoteDir;
    link.fileName  = estimatedRemoteName(item);
    link.size      = static_cast<quint64>(qMax<qint64>(item.size, item.isPasted() ? 9'999'999 : 1));
    link.dateTime  = 1'760'000'000;
    link.protocol  = MediaLink::kProtocol;
    link.sha256    = QByteArray(32, '\x5a');

    const MediaKind kind = item.isPasted() ? MediaKind::Image : kindForFileName(item.fileName);
    if (kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video) {
        const bool known = item.pixels.isValid() && !item.pixels.isEmpty();
        link.width       = known ? item.pixels.width() : 4096;
        link.height      = known ? item.pixels.height() : 3072;
        link.blurHash    = QStringLiteral("LEHV6nWB2yk8pyo0adR*.7kCMdnj");
        if (kind == MediaKind::Video)
            link.durationMs = item.durationMs > 0 ? item.durationMs : 3'599'000;
        if (context.previews) {
            link.previewFile = (context.remoteDir == QLatin1String("/") ? QString() : context.remoteDir) + QStringLiteral("/previews/3f9a1c2e.jpg");
            link.previewSha  = QByteArray(16, '\x5a');
        }
        link.spoiler = item.spoiler;
        if (inAlbum && albumCount >= 2) {
            link.albumId    = 0x7c1e09ab;
            link.albumIndex = 1;
            link.albumCount = qMin(albumCount, MediaLink::kMaxAlbumItems);
        }
    } else if (kind == MediaKind::Audio) {
        link.durationMs = item.durationMs > 0 ? item.durationMs : 3'599'000;
    }
    link.dropInvalidMetadata();
    return link;
}

bool captionGoesAlone(const QString& caption, const QVector<Item>& items, bool album, const LinkContext& context)
{
    if (sanitizeCaption(caption).isEmpty())
        return false;
    const QVector<int> order = postOrder(items, album);
    if (order.isEmpty())
        return false;
    const Item& first   = items.at(order.first());
    int         members = 0;
    if (album) {
        for (int i : order)
            members += isAlbumKind(items.at(i)) ? 1 : 0;
    }
    const bool inAlbum = members >= 2 && isAlbumKind(first);

    ComposeOptions options;
    options.caption       = caption;
    options.includeNotice = context.notice;
    options.downloadUrl   = context.downloadUrl;
    const QVector<ComposedMessage> messages = composeChatMessagesDetailed({estimatedLink(first, context, inAlbum, members)}, options);
    return !messages.isEmpty() && messages.first().links.isEmpty();
}

QImage coverThumbnail(const QImage& image, const QSize& size)
{
    if (image.isNull() || size.isEmpty())
        return {};
    const QImage scaled = image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect  crop((scaled.width() - size.width()) / 2, (scaled.height() - size.height()) / 2, size.width(), size.height());
    return scaled.copy(crop);
}

QImage spoilerCover(const QImage& image)
{
    if (image.isNull())
        return {};
    const QSize  tiny  = image.size().scaled(16, 16, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const QImage small = image.scaled(tiny, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // Two smooth steps up: one big step would show the 16 px grid.
    const QImage middle = small.scaled(small.size() * 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage       out    = middle.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter     p(&out);
    p.fillRect(out.rect(), QColor(0, 0, 0, 77)); // 30% darker
    p.end();
    out.setDevicePixelRatio(image.devicePixelRatio());
    return out;
}

QString savePastedImage(const QImage& image, const QString& dir, bool convertLargePngToJpeg)
{
    if (image.isNull())
        return {};
    QByteArray data;
    QBuffer    buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    QString extension = QStringLiteral(".png");
    // Photos pasted as PNG can be huge; JPEG is far smaller when there is no transparency.
    if (convertLargePngToJpeg && data.size() > 2 * 1024 * 1024 && !image.hasAlphaChannel()) {
        data.clear();
        buffer.seek(0);
        if (!image.save(&buffer, "JPG", 90))
            return {};
        extension = QStringLiteral(".jpg");
    }
    buffer.close();

    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/new_photo_") + randomHex8() + extension;
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size()) {
        file.close();
        QFile::remove(path);
        return {};
    }
    return path;
}

} // namespace compose
