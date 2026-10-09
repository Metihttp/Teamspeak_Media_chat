#include "medialink.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <limits>
#include <optional>

#include "blurhash.h"
#include "i18n.h"

namespace {

// Limits for metadata read from chat links (untrusted: anyone can type a ts3file:// link).
constexpr qint64 kMaxDimension     = 16384;
constexpr qint64 kMaxAspect        = 8;
constexpr qint64 kMaxDurationMs    = 1000ll * 3600 * 1000; // 1000 hours
constexpr int    kMaxBlurHashChars = 120;
constexpr int    kMaxPathChars     = 1024;
constexpr int    kMaxMessageBytes  = 1000; // TeamSpeak rejects text messages above 1024 bytes

QString encode(const QString& value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString normalizeDir(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    while (path.length() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

// Decimal integer; out-of-range numbers saturate, anything else is rejected.
std::optional<qint64> parseNumber(const QString& text)
{
    const QString trimmed = text.trimmed();
    bool          ok      = false;
    const qint64  value   = trimmed.toLongLong(&ok);
    if (ok)
        return value;
    static const QRegularExpression digits(QStringLiteral("^[+-]?\\d+$"));
    if (!digits.match(trimmed).hasMatch())
        return std::nullopt;
    return trimmed.startsWith(QLatin1Char('-')) ? std::numeric_limits<qint64>::min() : std::numeric_limits<qint64>::max();
}

// An absolute remote path ("/dir/name") without "." / ".." segments or control characters,
// with duplicate slashes removed. Empty if the path is not acceptable.
QString sanitizeRemoteFile(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!path.startsWith(QLatin1Char('/')) || path.length() > kMaxPathChars)
        return {};
    for (const QChar c : qAsConst(path)) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return {};
    }
    static const QRegularExpression onlyDots(QStringLiteral("^\\.+$"));
    const QStringList               parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    for (const QString& part : parts) {
        if (onlyDots.match(part).hasMatch())
            return {};
    }
    if (path.endsWith(QLatin1Char('/')))
        return {}; // a directory, not a file
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

void applySize(MediaLink& link, const std::optional<qint64>& w, const std::optional<qint64>& h)
{
    if (!w || !h || *w <= 0 || *h <= 0)
        return;
    qint64 width  = qMin(*w, kMaxDimension);
    qint64 height = qMin(*h, kMaxDimension);
    if (width > height * kMaxAspect)
        height = (width + kMaxAspect - 1) / kMaxAspect;
    else if (height > width * kMaxAspect)
        width = (height + kMaxAspect - 1) / kMaxAspect;
    link.width  = static_cast<int>(width);
    link.height = static_cast<int>(height);
}

// A download link that is safe to put inside [URL=...]: http(s) only, no brackets or spaces.
QString bbcodeSafeUrl(const QString& input)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return {};
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl    url(text, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return {};
    QString result = url.toString(QUrl::FullyEncoded);
    result.replace(QLatin1Char('['), QLatin1String("%5B"));
    result.replace(QLatin1Char(']'), QLatin1String("%5D"));
    result.replace(QLatin1Char(' '), QLatin1String("%20"));
    return result;
}

QString requiredNotice(const QString& url)
{
    const QString note = url.isEmpty()
                             ? i18n::t("TS Media chat plugin required to view this in chat")
                             : i18n::t("[URL=%1]TS Media chat[/URL] plugin required to view this in chat")
                                   .arg(url);
    // The receiver's theme is unknown: #72767d keeps 4.5:1 on TeamSpeak's default white chat and
    // stays readable (3:1) on dark ones. Receivers with the plugin remove the note by its position.
    return QStringLiteral(" [COLOR=#72767d][I]— ") + note + QStringLiteral("[/I][/COLOR]");
}

// TeamSpeak's own error message inside one of ours: trimmed, without a final period.
QString quoted(const QString& serverText)
{
    QString text = serverText.trimmed();
    while (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

} // namespace

bool MediaLink::isValid() const
{
    return !serverUid.isEmpty() && channelId != 0 && !fileName.isEmpty() && !isDir;
}

QString MediaLink::remoteFile() const
{
    const QString dir = normalizeDir(path);
    return dir == QLatin1String("/") ? dir + fileName : dir + QLatin1Char('/') + fileName;
}

QString MediaLink::key() const
{
    // Must stay identical to v1: cache file names and chat resources are derived from it.
    const QString raw = serverUid + QLatin1Char('\n') + QString::number(channelId) + QLatin1Char('\n') + remoteFile() + QLatin1Char('\n') + QString::number(size);
    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}

QString MediaLink::toUrl() const
{
    QString url = QStringLiteral("ts3file://") + encode(host);
    url += QStringLiteral("?port=") + QString::number(port);
    url += QStringLiteral("&serverUID=") + encode(serverUid);
    url += QStringLiteral("&channel=") + QString::number(channelId);
    url += QStringLiteral("&path=") + encode(normalizeDir(path));
    url += QStringLiteral("&filename=") + encode(fileName);
    url += QStringLiteral("&isDir=") + QString::number(isDir ? 1 : 0);
    url += QStringLiteral("&size=") + QString::number(size);
    url += QStringLiteral("&fileDateTime=") + QString::number(dateTime);
    if (protocol > 0)
        url += QStringLiteral("&tsm=") + QString::number(protocol);
    if (width > 0 && height > 0) {
        url += QStringLiteral("&w=") + QString::number(width);
        url += QStringLiteral("&h=") + QString::number(height);
    }
    if (durationMs > 0)
        url += QStringLiteral("&d=") + QString::number(durationMs);
    if (!blurHash.isEmpty())
        url += QStringLiteral("&bh=") + encode(blurHash);
    if (!previewFile.isEmpty())
        url += QStringLiteral("&pv=") + encode(previewFile);
    return url;
}

QString MediaLink::toBBCode() const
{
    QString label = fileName;
    label.replace(QLatin1Char('['), QLatin1Char('('));
    label.replace(QLatin1Char(']'), QLatin1Char(')'));
    return QStringLiteral("[URL=") + toUrl() + QStringLiteral("]") + label + QStringLiteral("[/URL]");
}

MediaLink MediaLink::previewLink() const
{
    MediaLink preview;
    const QString file = sanitizeRemoteFile(previewFile);
    if (file.isEmpty() || file.compare(remoteFile(), Qt::CaseInsensitive) == 0)
        return preview;

    const int slash   = file.lastIndexOf(QLatin1Char('/'));
    preview.host      = host;
    preview.port      = port;
    preview.serverUid = serverUid;
    preview.channelId = channelId;
    preview.path      = slash > 0 ? file.left(slash) : QStringLiteral("/");
    preview.fileName  = file.mid(slash + 1);
    return preview;
}

MediaLink MediaLink::parse(const QString& href)
{
    MediaLink link;

    // Accept "ts3file://host?...", "file:///ts3file/...?..." and HTML-escaped variants.
    QString   text = href;
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    const int scheme = text.indexOf(QLatin1String("ts3file"), 0, Qt::CaseInsensitive);
    if (scheme < 0)
        return link;
    const int query = text.indexOf(QLatin1Char('?'), scheme);
    if (query < 0)
        return link;

    QString host = text.mid(scheme, query - scheme);
    host.remove(QRegularExpression(QStringLiteral("^ts3file:?/*"), QRegularExpression::CaseInsensitiveOption));
    link.host = QUrl::fromPercentEncoding(host.toUtf8());

    // QUrlQuery keeps '+' as-is in FullyDecoded mode, which is what we want for base64 server UIDs.
    const QUrlQuery q(text.mid(query + 1));
    const auto      items = q.queryItems(QUrl::FullyDecoded);
    auto            value = [&items](const char* name) {
        for (const auto& item : items) {
            if (item.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                return item.second;
        }
        return QString();
    };
    auto has = [&items](const char* name) {
        for (const auto& item : items) {
            if (item.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    link.port      = static_cast<quint16>(value("port").toUInt());
    link.serverUid = value("serverUID");
    link.channelId = value("channel").toULongLong();
    link.path      = normalizeDir(value("path").isEmpty() ? QStringLiteral("/") : value("path"));
    link.fileName  = value("filename");
    link.size      = value("size").toULongLong();
    link.dateTime  = value("fileDateTime").toLongLong();
    link.isDir     = value("isDir").toInt() != 0;

    // A file name must never be able to escape the cache directory.
    link.fileName = QFileInfo(link.fileName.replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();

    // TS Media metadata. Invalid values are dropped, never fatal: the link itself still works.
    if (has("tsm")) {
        if (const auto tsm = parseNumber(value("tsm")); tsm && *tsm > 0)
            link.protocol = static_cast<int>(qMin<qint64>(*tsm, std::numeric_limits<int>::max()));
    }
    if (has("w") || has("h"))
        applySize(link, parseNumber(value("w")), parseNumber(value("h")));
    if (has("d")) {
        if (const auto d = parseNumber(value("d")); d && *d > 0)
            link.durationMs = qMin(*d, kMaxDurationMs);
    }
    if (has("bh")) {
        const QString bh = value("bh");
        if (bh.length() <= kMaxBlurHashChars && blurhash::isValid(bh))
            link.blurHash = bh;
    }
    if (has("pv")) {
        const QString pv = sanitizeRemoteFile(value("pv"));
        if (!pv.isEmpty() && pv.compare(link.remoteFile(), Qt::CaseInsensitive) != 0)
            link.previewFile = pv;
    }
    return link;
}

QList<MediaLink> MediaLink::findInMessage(const QString& message)
{
    static const QRegularExpression re(QStringLiteral(R"(\[url=([^\]]*ts3file[^\]]*)\])"), QRegularExpression::CaseInsensitiveOption);

    QList<MediaLink> result;
    auto             it = re.globalMatch(message);
    while (it.hasNext()) {
        const MediaLink link = parse(it.next().captured(1));
        if (link.isValid())
            result.append(link);
    }
    return result;
}

QString composeChatMessage(const MediaLink& link, bool includeNotice, const QString& downloadUrl)
{
    const QString url = includeNotice ? bbcodeSafeUrl(downloadUrl) : QString();

    // Candidates from richest to leanest; the first one that fits is sent. The order follows the spec
    // (BlurHash first, then the note); the trailing steps only matter for absurdly long file names.
    MediaLink noHash = link;
    noHash.blurHash.clear();
    MediaLink noPreview = noHash;
    noPreview.previewFile.clear();
    MediaLink bare  = noPreview;
    bare.width      = 0;
    bare.height     = 0;
    bare.durationMs = 0;

    QStringList candidates;
    if (includeNotice) {
        candidates << link.toBBCode() + requiredNotice(url) << noHash.toBBCode() + requiredNotice(url);
        if (!url.isEmpty())
            candidates << noHash.toBBCode() + requiredNotice(QString());
    } else {
        candidates << link.toBBCode();
    }
    candidates << noHash.toBBCode() << noPreview.toBBCode() << bare.toBBCode();

    for (const QString& message : qAsConst(candidates)) {
        if (message.toUtf8().size() < kMaxMessageBytes)
            return message;
    }
    return candidates.last();
}

MediaKind kindForFileName(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    static const QStringList images   = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("jfif")};
    static const QStringList videos   = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("m4v")};
    static const QStringList audio    = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("flac"), QStringLiteral("m4a"), QStringLiteral("opus"), QStringLiteral("aac")};
    static const QStringList archives = {QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("tar"), QStringLiteral("gz")};
    static const QStringList docs     = {QStringLiteral("pdf"), QStringLiteral("txt"), QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("md")};

    if (ext == QLatin1String("gif"))
        return MediaKind::AnimatedImage;
    if (images.contains(ext))
        return MediaKind::Image;
    if (videos.contains(ext))
        return MediaKind::Video;
    if (audio.contains(ext))
        return MediaKind::Audio;
    if (archives.contains(ext))
        return MediaKind::Archive;
    if (docs.contains(ext))
        return MediaKind::Document;
    return MediaKind::Other;
}

bool isPreviewableImage(MediaKind kind)
{
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage;
}

QString displayFileName(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        const ushort u = ch.unicode();
        // LRM, RLM, ALM; LRE, RLE, PDF, LRO, RLO; LRI, RLI, FSI, PDI. (ZWNJ/ZWJ stay: some scripts need them.)
        const bool bidiControl = u == 0x200E || u == 0x200F || u == 0x061C || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069);
        const QChar::Category category = ch.category();
        if (bidiControl || category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            continue;
        out += ch;
    }
    return out;
}

QString displayNameFor(const MediaLink& link)
{
    if (link.protocol <= 0)
        return displayFileName(link.fileName);

    // Core::makeRemoteName: <base>_<8 lower-case hex digits>[.<ext>]. A leading dot starts a name.
    static const QRegularExpression randomPart(QStringLiteral("_[0-9a-f]{8}$"));
    static const QRegularExpression pasted(QStringLiteral("^new_photo_[0-9a-f]{8}$"));
    const int     dot  = link.fileName.lastIndexOf(QLatin1Char('.'));
    QString       base = dot > 0 ? link.fileName.left(dot) : link.fileName;
    const QString ext  = dot > 0 ? link.fileName.mid(dot) : QString();
    if (pasted.match(base).hasMatch()) {
        base = i18n::t("Pasted image");
    } else {
        const QString stripped = QString(base).remove(randomPart);
        if (!stripped.isEmpty())
            base = stripped;
    }
    return displayFileName(base + ext);
}

QString formatSize(quint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    static const char* const units[] = {"KB", "MB", "GB", "TB"};
    constexpr int            kLastUnit = 3;
    double                   value     = static_cast<double>(bytes) / 1024.0;
    int                      unit      = 0;
    // Promote while the value would round to four digits: 1023.9 KB is shown as 1.0 MB.
    while (value >= 999.5 && unit < kLastUnit) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = value < 99.95 ? 1 : 0;
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(units[unit]));
}

QString formatProgress(quint64 done, quint64 total)
{
    return i18n::t("%1 of %2").arg(formatSize(total > 0 ? qMin(done, total) : done), formatSize(total));
}

QString formatSpeed(double bytesPerSecond)
{
    const double rate = bytesPerSecond > 0.0 ? bytesPerSecond : 0.0;
    return i18n::t("%1/s").arg(formatSize(static_cast<quint64>(rate + 0.5)));
}

QString formatTimeLeft(qint64 ms)
{
    // Seconds round up (never "0 s left" while something remains), minutes to the nearest one.
    const qint64 seconds = qMax<qint64>(1, (qMax<qint64>(0, ms) + 999) / 1000);
    if (seconds < 60)
        return i18n::t("%1 s left").arg(seconds);
    const qint64 minutes = (seconds + 30) / 60;
    if (minutes < 60)
        return i18n::t("%1 min left").arg(minutes);
    const qint64 hours = minutes / 60;
    const qint64 rest  = minutes % 60;
    return rest == 0 ? i18n::t("%1 h left").arg(hours) : i18n::t("%1 h %2 min left").arg(hours).arg(rest);
}

QString downloadErrorTitle(MediaError error)
{
    switch (error) {
    case MediaError::NotFound:
        return i18n::t("File is no longer on the server");
    case MediaError::Permission:
        return i18n::t("No permission to download");
    case MediaError::Password:
        return i18n::t("Channel is password protected");
    case MediaError::NotConnected:
        return i18n::t("Not connected to this server");
    case MediaError::Quota:
        return i18n::t("Server transfer limit reached");
    case MediaError::None:
    case MediaError::Other:
        break;
    }
    return i18n::t("Download failed");
}

QString downloadErrorText(MediaError error, const QString& serverText)
{
    switch (error) {
    case MediaError::NotFound:
        return i18n::t("The file was deleted from the server or replaced after it was sent.");
    case MediaError::Permission:
        return i18n::t("You don't have permission to download files in this channel. Ask a server admin.");
    case MediaError::Password:
        return i18n::t("Downloading from password-protected channels isn't supported. Use TeamSpeak's file browser instead.");
    case MediaError::NotConnected:
        return i18n::t("Reconnect to this server to download the file. Interrupted downloads resume by themselves.");
    case MediaError::Quota:
        return i18n::t("This server's file transfer quota is used up. Ask a server admin.");
    case MediaError::None:
    case MediaError::Other:
        break;
    }
    const QString server = quoted(serverText);
    return server.isEmpty() ? i18n::t("Something went wrong while downloading this file. Try again.")
                            : i18n::t("Something went wrong while downloading this file (%1). Try again.").arg(server);
}

QString uploadErrorText(MediaError error, const QString& serverText)
{
    switch (error) {
    case MediaError::Permission:
        return i18n::t("You don't have permission to upload files in this channel. Ask a server admin for upload permission.");
    case MediaError::Quota:
        return i18n::t("The server's upload quota or storage is full. Ask a server admin.");
    case MediaError::NotConnected:
        return i18n::t("Disconnected from the server. Reconnect and try again.");
    case MediaError::Password:
        return i18n::t("Sending files to password-protected channels isn't supported. Use another channel.");
    case MediaError::NotFound:
        return i18n::t("The upload folder no longer exists on the server. Try again.");
    case MediaError::None:
    case MediaError::Other:
        break;
    }
    const QString server = quoted(serverText);
    return server.isEmpty() ? i18n::t("Upload failed. Try again.") : i18n::t("Upload failed (%1). Try again.").arg(server);
}

QString postErrorText(MediaError error, const QString& serverText)
{
    switch (error) {
    case MediaError::Permission:
        return i18n::t("Uploaded, but you don't have permission to write in this chat. The file is in the channel's file browser.");
    case MediaError::NotConnected:
        return i18n::t("Uploaded, but the connection was lost before the chat message was sent. The file is in the channel's file browser.");
    case MediaError::None:
    case MediaError::Password:
    case MediaError::NotFound:
    case MediaError::Quota:
    case MediaError::Other:
        break;
    }
    const QString server = quoted(serverText);
    return server.isEmpty() ? i18n::t("Uploaded, but the chat message couldn't be sent. The file is in the channel's file browser.")
                            : i18n::t("Uploaded, but the chat message couldn't be sent (%1). The file is in the channel's file browser.").arg(server);
}

QString formatDuration(qint64 ms)
{
    const qint64 total   = qMax<qint64>(0, ms) / 1000;
    const qint64 hours   = total / 3600;
    const qint64 minutes = (total / 60) % 60;
    const qint64 seconds = total % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}
