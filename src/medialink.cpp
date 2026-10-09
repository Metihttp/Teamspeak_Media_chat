#include "medialink.h"

#include <QCryptographicHash>
#include <QDateTime>
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
// kMaxMessageBytes (the message limit) is in medialink.h.

constexpr int kShaBytes         = 32;
constexpr int kPreviewShaBytes  = 16;
constexpr int kWaveformBytes    = MediaLink::kWaveformLevels / 2; // two 4-bit levels per byte

QString encode(const QString& value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

// LRM, RLM, ALM; LRE, RLE, PDF, LRO, RLO; LRI, RLI, FSI, PDI. (ZWNJ/ZWJ stay: some scripts need them.)
// Removed from file names on display and from captions.
bool isBidiControl(QChar ch)
{
    const ushort u = ch.unicode();
    return u == 0x200E || u == 0x200F || u == 0x061C || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069);
}

// ---- 2.2 metadata encodings ----------------------------------------------------------------------

QString toBase64Url(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// Exactly `bytes` bytes as unpadded base64url in its one canonical spelling (the unused low bits of the
// last character are zero), else empty. Values from chat links: strict, never "close enough".
QByteArray fromBase64UrlExact(const QString& text, int bytes)
{
    const int chars = (bytes * 4 + 2) / 3;
    if (text.length() != chars)
        return {};
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool   ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_';
        if (!ok)
            return {};
    }
    const QByteArray latin = text.toLatin1();
    const auto       result = QByteArray::fromBase64Encoding(latin, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals
                                                                        | QByteArray::AbortOnBase64DecodingErrors);
    if (result.decodingStatus != QByteArray::Base64DecodingStatus::Ok || result.decoded.size() != bytes)
        return {};
    if (toBase64Url(result.decoded) != text) // a non-canonical last character
        return {};
    return result.decoded;
}

// 64 levels of 0..15 -> 32 bytes, the first level of each pair in the high nibble.
QByteArray packWaveform(const QByteArray& levels)
{
    QByteArray packed(kWaveformBytes, '\0');
    for (int i = 0; i < kWaveformBytes; ++i)
        packed[i] = static_cast<char>(((static_cast<uchar>(levels.at(2 * i)) & 0x0f) << 4) | (static_cast<uchar>(levels.at(2 * i + 1)) & 0x0f));
    return packed;
}

QByteArray unpackWaveform(const QByteArray& packed)
{
    QByteArray levels(MediaLink::kWaveformLevels, '\0');
    for (int i = 0; i < kWaveformBytes; ++i) {
        const uchar b     = static_cast<uchar>(packed.at(i));
        levels[2 * i]     = static_cast<char>(b >> 4);
        levels[2 * i + 1] = static_cast<char>(b & 0x0f);
    }
    return levels;
}

bool isWaveform(const QByteArray& levels)
{
    if (levels.size() != MediaLink::kWaveformLevels)
        return false;
    for (const char level : levels) {
        if (static_cast<uchar>(level) > MediaLink::kWaveformMax)
            return false;
    }
    return true;
}

bool spoilerAllowed(MediaKind kind)
{
    return kind == MediaKind::Image || kind == MediaKind::AnimatedImage || kind == MediaKind::Video;
}

// "1".."99" without a sign, leading zero or spaces (album positions and sizes).
int smallNumber(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^[1-9][0-9]?$"));
    return re.match(text).hasMatch() ? text.toInt() : 0;
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
    // Without a sha it must stay identical to v1: cache file names and chat resources are derived from it.
    QString raw = serverUid + QLatin1Char('\n') + QString::number(channelId) + QLatin1Char('\n') + remoteFile() + QLatin1Char('\n') + QString::number(size);
    // The same condition as toUrl(): a hash that is never sent is no part of the identity either.
    if (isTsMedia() && sha256.size() == kShaBytes)
        raw += QLatin1Char('\n') + toBase64Url(sha256);
    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}

void MediaLink::dropInvalidMetadata()
{
    if (!isTsMedia()) {
        sha256.clear();
        previewSha.clear();
        spoiler    = false;
        albumId    = 0;
        albumIndex = 0;
        albumCount = 0;
        voice      = false;
        waveform.clear();
        return;
    }
    const MediaKind kind = kindForFileName(fileName);
    if (sha256.size() != kShaBytes)
        sha256.clear();
    if (previewSha.size() != kPreviewShaBytes || previewFile.isEmpty())
        previewSha.clear();
    if (spoiler && !spoilerAllowed(kind))
        spoiler = false;
    if (albumId == 0 || albumCount < 2 || albumCount > kMaxAlbumItems || albumIndex < 1 || albumIndex > albumCount) {
        albumId    = 0;
        albumIndex = 0;
        albumCount = 0;
    }
    if (voice && kind != MediaKind::Audio)
        voice = false;
    if (!voice || !isWaveform(waveform))
        waveform.clear();
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

    // 2.2: only what a receiver accepts, in the fixed order. Every value is base64url, hex or a decimal
    // number, so none of them is ever percent-encoded.
    MediaLink extra = *this;
    extra.dropInvalidMetadata();
    if (!extra.sha256.isEmpty())
        url += QStringLiteral("&sha=") + toBase64Url(extra.sha256);
    if (!extra.previewSha.isEmpty())
        url += QStringLiteral("&ph=") + toBase64Url(extra.previewSha);
    if (extra.spoiler)
        url += QStringLiteral("&sp=1");
    if (extra.albumId != 0) {
        url += QStringLiteral("&al=") + QStringLiteral("%1").arg(extra.albumId, 8, 16, QLatin1Char('0'));
        url += QStringLiteral("&ai=") + QString::number(extra.albumIndex);
        url += QStringLiteral("&an=") + QString::number(extra.albumCount);
    }
    if (extra.voice)
        url += QStringLiteral("&vm=1");
    if (!extra.waveform.isEmpty())
        url += QStringLiteral("&wf=") + toBase64Url(packWaveform(extra.waveform));
    return url;
}

QString MediaLink::toBBCode() const
{
    return toBBCode(fileName);
}

QString MediaLink::toBBCode(const QString& label) const
{
    QString text = label;
    text.replace(QLatin1Char('['), QLatin1Char('('));
    text.replace(QLatin1Char(']'), QLatin1Char(')'));
    // A backslash right before "[/URL]" would make TeamSpeak show the closing tag as text.
    for (int i = text.size() - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i)
        text[i] = QLatin1Char('/');
    return QStringLiteral("[URL=") + toUrl() + QStringLiteral("]") + text + QStringLiteral("[/URL]");
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

    // 2.2 metadata, TS Media links only. Each check is exact; a bad value drops only its own field
    // (the album: all three), and dropInvalidMetadata() applies the rules between fields.
    if (link.isTsMedia()) {
        if (has("sha"))
            link.sha256 = fromBase64UrlExact(value("sha"), kShaBytes);
        if (has("ph") && !link.previewFile.isEmpty())
            link.previewSha = fromBase64UrlExact(value("ph"), kPreviewShaBytes);
        link.spoiler = value("sp") == QLatin1String("1");
        if (has("al") || has("ai") || has("an")) {
            static const QRegularExpression hex8(QStringLiteral("^[0-9a-f]{8}$"));
            const QString                   al    = value("al");
            const int                       index = smallNumber(value("ai"));
            const int                       count = smallNumber(value("an"));
            if (hex8.match(al).hasMatch() && count >= 2 && count <= kMaxAlbumItems && index >= 1 && index <= count) {
                link.albumId    = al.toUInt(nullptr, 16);
                link.albumIndex = index;
                link.albumCount = count;
            }
        }
        link.voice = value("vm") == QLatin1String("1");
        if (link.voice && has("wf")) {
            const QByteArray packed = fromBase64UrlExact(value("wf"), kWaveformBytes);
            if (!packed.isEmpty())
                link.waveform = unpackWaveform(packed);
        }
        link.dropInvalidMetadata();
    }
    return link;
}

QList<MediaLink> MediaLink::findInMessage(const QString& message)
{
    // Only the tag itself: "[" U+200B "URL=" and the like never match (TeamSpeak shows them as text).
    static const QRegularExpression re(QStringLiteral(R"(\[url=([^\]]*ts3file[^\]]*)\])"), QRegularExpression::CaseInsensitiveOption);

    QList<MediaLink> result;
    auto             it = re.globalMatch(message);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        // "\[URL=...]" is text in TeamSpeak's chat (any backslash run before a bracket escapes it), and
        // so is a tag whose "]" is escaped.
        const int start = match.capturedStart();
        if (start > 0 && message.at(start - 1) == QLatin1Char('\\'))
            continue;
        const QString href = match.captured(1);
        if (href.endsWith(QLatin1Char('\\')))
            continue;
        const MediaLink link = parse(href);
        if (link.isValid())
            result.append(link);
    }
    return result;
}

QString composeChatMessage(const MediaLink& link, bool includeNotice, const QString& downloadUrl)
{
    // 2.1's candidates (BlurHash, the note's link, the note, the preview, the sizes) are the same
    // steps in the same order, so this is the 2.2 composer with 2.1's label and 2.1's limit. Only the
    // compatibility tests use it; Core uses composeChatMessages() with kMaxMessageBytes.
    ComposeOptions options;
    options.includeNotice  = includeNotice;
    options.downloadUrl    = downloadUrl;
    options.friendlyLabels = false;
    options.maxBytes       = 1000; // 2.1's limit and size rule, whatever kMaxMessageBytes is
    options.legacySize     = true;
    return composeChatMessages({link}, options).value(0);
}

namespace {

// Metadata the cascade can leave out, in the order it does.
enum Drop : int {
    DropPh   = 1 << 0,
    DropBh   = 1 << 1,
    DropWf   = 1 << 2,
    DropPv   = 1 << 3,
    DropDims = 1 << 4,
    DropSha  = 1 << 5,
};

enum class Note { None, Plain, Linked };

struct Step {
    int  drops;
    Note note;
};

MediaLink reduced(MediaLink link, int drops)
{
    if (drops & DropPh)
        link.previewSha.clear();
    if (drops & DropBh)
        link.blurHash.clear();
    if (drops & DropWf)
        link.waveform.clear();
    if (drops & DropPv) {
        link.previewFile.clear();
        link.previewSha.clear();
    }
    if (drops & DropDims) {
        link.width      = 0;
        link.height     = 0;
        link.durationMs = 0;
    }
    if (drops & DropSha)
        link.sha256.clear();
    return link;
}

// Names of what a step really removed (for the log), in the cascade's order: the link's fields up to
// wf, then the note's parts (noteNames), then the rest.
QStringList droppedNames(const MediaLink& link, int drops, const QStringList& noteNames = {})
{
    MediaLink valid = link;
    valid.dropInvalidMetadata();
    QStringList names;
    if ((drops & DropPh) && !valid.previewSha.isEmpty())
        names << QStringLiteral("ph");
    if ((drops & DropBh) && !link.blurHash.isEmpty())
        names << QStringLiteral("bh");
    if ((drops & DropWf) && !valid.waveform.isEmpty())
        names << QStringLiteral("wf");
    names << noteNames;
    if ((drops & DropPv) && !link.previewFile.isEmpty())
        names << QStringLiteral("pv");
    if ((drops & DropDims) && ((link.width > 0 && link.height > 0) || link.durationMs > 0))
        names << QStringLiteral("w/h/d");
    if ((drops & DropSha) && !valid.sha256.isEmpty())
        names << QStringLiteral("sha");
    return names;
}

int utf8Size(const QString& text)
{
    return text.toUtf8().size();
}

// The caption as one message part (never cut: kCaptionMaxChars keeps it far below any limit).
QString captionPart(const QString& caption)
{
    const QString clean = sanitizeCaption(caption);
    return clean.isEmpty() ? QString() : captionToBBCode(clean);
}

} // namespace

int escapedMessageSize(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    int              size = utf8.size();
    for (const char ch : utf8) {
        switch (ch) {
        case '\\':
        case '/':
        case ' ':
        case '|':
        case '\a':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
        case '\v':
            ++size; // ServerQuery escaping: "\\", "\/", "\s", "\p", "\a", ... take two bytes
            break;
        default:
            break;
        }
    }
    return size;
}

QVector<ComposedMessage> composeChatMessagesDetailed(const QList<MediaLink>& links, const ComposeOptions& options)
{
    QVector<ComposedMessage> out;
    const QString            sep     = QString::fromLatin1(kMessageSeparator);
    const QString            url     = options.includeNotice ? bbcodeSafeUrl(options.downloadUrl) : QString();
    const QString            caption = captionPart(options.caption);
    const auto               fits    = [&options](const QString& text) {
        return options.legacySize ? utf8Size(text) < options.maxBytes : escapedMessageSize(text) <= options.maxBytes;
    };
    const auto               noteText = [&url](Note note) {
        switch (note) {
        case Note::Linked:
            return requiredNotice(url);
        case Note::Plain:
            return requiredNotice(QString());
        case Note::None:
            break;
        }
        return QString();
    };

    if (links.isEmpty()) {
        if (!caption.isEmpty())
            out.append({caption, {}, {}, !fits(caption)});
        return out;
    }

    QStringList labels;
    for (const MediaLink& link : links)
        labels << (options.friendlyLabels ? linkLabel(link) : link.fileName);
    const auto linkText = [&links, &labels](int i, int drops) { return reduced(links.at(i), drops).toBBCode(labels.at(i)); };

    // The cascade. The steps up to the note's link keep the note; only they are tried with the caption
    // in the same message.
    const int    keep = DropPh | DropBh | DropWf;
    QVector<Step> withNote;
    QVector<Step> leaner;
    if (options.includeNotice) {
        const Note top = url.isEmpty() ? Note::Plain : Note::Linked;
        withNote << Step{0, top} << Step{DropPh, top} << Step{DropPh | DropBh, top} << Step{keep, top};
        if (top == Note::Linked)
            withNote << Step{keep, Note::Plain};
        leaner << Step{keep, Note::None};
    } else {
        withNote << Step{0, Note::None} << Step{DropPh, Note::None} << Step{DropPh | DropBh, Note::None} << Step{keep, Note::None};
    }
    leaner << Step{keep | DropPv, Note::None} << Step{keep | DropPv | DropDims, Note::None} << Step{keep | DropPv | DropDims | DropSha, Note::None};

    const auto stepDropped = [&](int i, const Step& step) {
        QStringList note;
        if (options.includeNotice && step.note != Note::Linked && !url.isEmpty())
            note << QStringLiteral("note link");
        if (options.includeNotice && step.note == Note::None)
            note << QStringLiteral("note");
        return droppedNames(links.at(i), step.drops, note);
    };

    // ---- the first message: caption, first link, note -------------------------------------------
    Step    first     = leaner.last();
    bool    found     = false;
    QString prefix;
    if (!caption.isEmpty()) {
        for (const Step& step : qAsConst(withNote)) {
            if (fits(caption + sep + linkText(0, step.drops) + noteText(step.note))) {
                first  = step;
                found  = true;
                prefix = caption + sep;
                break;
            }
        }
        if (!found) // the caption goes first, as a message of its own (never shortened)
            out.append({caption, {}, {}, !fits(caption)});
    }
    if (!found) {
        for (const QVector<Step>* steps : {&withNote, &leaner}) {
            for (const Step& step : *steps) {
                if (!found && fits(linkText(0, step.drops) + noteText(step.note))) {
                    first = step;
                    found = true;
                }
            }
        }
    }

    // A message being filled: [prefix] body [tail]; only the first media message has a prefix (the
    // caption) and a tail (the note).
    struct Building {
        QString         prefix;
        QString         body;
        QString         tail;
        ComposedMessage message;
    };
    const auto finish = [&](Building& b) {
        b.message.text = b.prefix + b.body + b.tail;
        if (!b.message.tooLong)
            b.message.tooLong = !fits(b.message.text);
        b.message.dropped.removeDuplicates();
        out.append(b.message);
    };

    Building current{prefix, linkText(0, first.drops), noteText(first.note), {}};
    current.message.links << 0;
    current.message.dropped = stepDropped(0, first);
    current.message.tooLong = !found;

    // ---- the other links, each with the metadata it keeps in a message of its own ----------------
    QVector<Step> alone;
    alone << Step{0, Note::None} << Step{DropPh, Note::None} << Step{DropPh | DropBh, Note::None} << Step{keep, Note::None};
    alone << leaner.mid(options.includeNotice ? 1 : 0); // the steps after the note
    for (int i = 1; i < links.size(); ++i) {
        Step natural   = alone.last();
        bool fitsAlone = false;
        for (const Step& step : qAsConst(alone)) {
            if (fits(linkText(i, step.drops))) {
                natural   = step;
                fitsAlone = true;
                break;
            }
        }
        const QString text = linkText(i, natural.drops);
        if (fits(current.prefix + current.body + sep + text + current.tail)) {
            current.body += sep + text;
            current.message.links << i;
            current.message.dropped += droppedNames(links.at(i), natural.drops);
            continue;
        }
        finish(current);
        current = Building{QString(), text, QString(), {}};
        current.message.links << i;
        current.message.dropped = droppedNames(links.at(i), natural.drops);
        current.message.tooLong = !fitsAlone;
    }
    finish(current);
    return out;
}

QStringList composeChatMessages(const QList<MediaLink>& links, const ComposeOptions& options)
{
    QStringList texts;
    for (const ComposedMessage& message : composeChatMessagesDetailed(links, options))
        texts << message.text;
    return texts;
}

QString sanitizeCaption(const QString& typed)
{
    QString out;
    out.reserve(qMin(typed.size(), kCaptionMaxChars + 1));
    bool space = false;
    for (int i = 0; i < typed.size(); ++i) {
        const QChar ch = typed.at(i);
        if (ch.isSpace()) { // also tabs, line breaks and line or paragraph separators
            space = !out.isEmpty();
            continue;
        }
        if (isBidiControl(ch) || ch.category() == QChar::Other_Control)
            continue;
        if (ch.isSurrogate()) {
            // Whole pairs only.
            if (!ch.isHighSurrogate() || i + 1 >= typed.size() || !typed.at(i + 1).isLowSurrogate()) {
                continue;
            }
            const int need = (space ? 1 : 0) + 2;
            if (out.size() + need > kCaptionMaxChars)
                break;
            if (space)
                out += QLatin1Char(' ');
            space = false;
            out += ch;
            out += typed.at(++i);
            continue;
        }
        const int need = (space ? 1 : 0) + 1;
        if (out.size() + need > kCaptionMaxChars)
            break;
        if (space)
            out += QLatin1Char(' ');
        space = false;
        out += ch;
    }
    return out;
}

QString bbcodeLiteral(const QString& text)
{
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        if (ch == QLatin1Char('[') || ch == QLatin1Char(']'))
            out += QLatin1Char('\\');
        out += ch;
    }
    return out;
}

QString captionToBBCode(const QString& sanitized)
{
    return bbcodeLiteral(sanitized);
}

QString linkLabel(const MediaLink& link)
{
    MediaLink valid = link;
    valid.dropInvalidMetadata();
    QString label;
    if (valid.voice) {
        label = valid.durationMs > 0 ? i18n::t("Voice message (%1)").arg(formatDuration(valid.durationMs)) : i18n::t("Voice message");
    } else if (valid.spoiler) {
        switch (kindForFileName(valid.fileName)) {
        case MediaKind::AnimatedImage:
            label = i18n::t("Spoiler (GIF)");
            break;
        case MediaKind::Video:
            label = i18n::t("Spoiler (video)");
            break;
        default:
            label = i18n::t("Spoiler (image)");
            break;
        }
    } else {
        label = displayNameFor(link);
    }
    if (label.trimmed().isEmpty())
        label = i18n::t("File");
    label.replace(QLatin1Char('['), QLatin1Char('('));
    label.replace(QLatin1Char(']'), QLatin1Char(')'));
    return label;
}

QString boundRemoteBase(const QString& base)
{
    QString out;
    int     bytes = 0;
    for (int i = 0; i < base.size();) {
        const QChar c     = base.at(i);
        const bool  pair  = c.isHighSurrogate() && i + 1 < base.size() && base.at(i + 1).isLowSurrogate();
        const int   units = pair ? 2 : 1;
        if (c.isSurrogate() && !pair) { // a lone half: not a character
            ++i;
            continue;
        }
        const uint code = pair ? QChar::surrogateToUcs4(c, base.at(i + 1)) : c.unicode();
        const int  size = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (out.size() + units > kRemoteBaseMaxChars || bytes + size > kRemoteBaseMaxBytes)
            break;
        out += base.midRef(i, units);
        bytes += size;
        i += units;
    }
    return out;
}

QString remoteSuffixOf(const QString& remoteName)
{
    static const QRegularExpression suffix(QStringLiteral("_([0-9a-f]{8})$"));
    const QString                   base  = QFileInfo(remoteName).completeBaseName();
    const QRegularExpressionMatch   match = suffix.match(base);
    return match.hasMatch() ? match.captured(1) : QString();
}

MediaKind kindForFileName(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    static const QStringList images   = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("jfif")};
    static const QStringList videos   = {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("m4v")};
    static const QStringList audio    = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("flac"), QStringLiteral("m4a"), QStringLiteral("opus"), QStringLiteral("aac"),
                                         QStringLiteral("wma"), QStringLiteral("mka"), QStringLiteral("weba")}; // 2.2 audio: measured to play inline
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
        const QChar::Category category = ch.category();
        if (isBidiControl(ch) || category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            continue;
        out += ch;
    }
    return out;
}

QString displayNameFor(const MediaLink& link)
{
    if (link.protocol <= 0)
        return displayFileName(link.fileName);
    if (link.voice && link.isTsMedia() && kindForFileName(link.fileName) == MediaKind::Audio) { // 2.2 voice: from the vm flag
        const QString ext = QFileInfo(link.fileName).suffix().toLower();
        return i18n::t("Voice message") + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }

    // Core::makeRemoteName: <base>_<8 lower-case hex digits>[.<ext>]. A leading dot starts a name.
    static const QRegularExpression randomPart(QStringLiteral("_[0-9a-f]{8}$"));
    static const QRegularExpression pasted(QStringLiteral("^new_photo_[0-9a-f]{8}$"));
    const int     dot  = link.fileName.lastIndexOf(QLatin1Char('.'));
    QString       base = dot > 0 ? link.fileName.left(dot) : link.fileName;
    const QString ext  = dot > 0 ? link.fileName.mid(dot) : QString();
    // Only the formats Core::uploadImage writes: a video or an archive is never a pasted picture.
    // (Core::makeRemoteName gives a user's own "new photo" another name.)
    const bool pictureExt = ext == QLatin1String(".png") || ext == QLatin1String(".jpg");
    if (pictureExt && pasted.match(base).hasMatch()) {
        base = i18n::t("Pasted image");
    } else {
        const QString stripped = QString(base).remove(randomPart);
        if (!stripped.isEmpty())
            base = stripped;
    }
    return displayFileName(base + ext);
}

// 2.2 voice
QString voiceSaveName(const MediaLink& link)
{
    QString ext = QFileInfo(link.fileName).suffix().toLower();
    if (ext.isEmpty() || ext.size() > 8)
        ext = QStringLiteral("m4a");
    if (link.dateTime <= 0)
        return i18n::t("Voice message") + QLatin1Char('.') + ext;
    const QDateTime when = QDateTime::fromSecsSinceEpoch(link.dateTime).toLocalTime();
    return i18n::t("Voice message %1").arg(when.toString(QStringLiteral("yyyy-MM-dd HH-mm"))) + QLatin1Char('.') + ext;
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
    case MediaError::Mismatch: // 2.2 sha
        return i18n::t("File doesn't match what was sent");
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
    case MediaError::Mismatch: // 2.2 sha
        return i18n::t("The downloaded file is different from the one that was sent. It may have been replaced on the server, so it wasn't "
                       "shown or saved. Ask the sender to send it again.");
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
    case MediaError::Mismatch: // downloads only
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
    case MediaError::Mismatch: // downloads only
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
