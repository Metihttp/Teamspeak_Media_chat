#include "announcement.h"

#include <QSet>
#include <QVector>

#include "pagemeta.h"
#include "textclean.h"
#include "textreact.h"

namespace linkpreview {

namespace {

constexpr int kMaxUrlKeyChars = 64;
constexpr int kMinUrlKeyChars = 8;
constexpr int kShaBytes       = 32;
constexpr int kMaxNumberDigits = 20;

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// A decimal number without a sign or leading zeros, within [min, max].
bool readNumber(const QByteArray& text, qint64 min, qint64 max, qint64* out)
{
    if (text.isEmpty() || text.size() > kMaxNumberDigits || (text.size() > 1 && text.at(0) == '0'))
        return false;
    for (const char c : text) {
        if (!isDigit(c))
            return false;
    }
    bool         ok    = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok || value < min || value > max)
        return false;
    if (out)
        *out = value;
    return true;
}

bool readChannel(const QByteArray& text, quint64* out)
{
    if (text.isEmpty() || text.size() > kMaxNumberDigits || text.at(0) == '0')
        return false;
    for (const char c : text) {
        if (!isDigit(c))
            return false;
    }
    bool          ok    = false;
    const quint64 value = text.toULongLong(&ok);
    if (!ok || value == 0)
        return false;
    *out = value;
    return true;
}

bool isUid(const QString& uid)
{
    if (uid.isEmpty() || uid.size() > 64)
        return false;
    for (const QChar ch : uid) {
        const ushort c  = ch.unicode();
        const bool   ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=' || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

// Bidi marks and invisible characters a title could reorder or hide text with.
bool isHiddenChar(uint c)
{
    return c == 0x00ad || c == 0x061c || c == 0x180e || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f)
           || c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb);
}

QString extensionOf(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const int dot   = path.lastIndexOf(QLatin1Char('.'));
    return dot > slash + 1 ? path.mid(dot + 1).toLower() : QString();
}

std::optional<QString> readText(const QByteArray& value, int maxBytes)
{
    // base64 of maxBytes is at most 4/3 as long; the decoded text must be valid UTF-8 as it is.
    const std::optional<QByteArray> bytes = fromBase64Url(value, maxBytes);
    if (!bytes || bytes->isEmpty())
        return std::nullopt;
    const QString text = QString::fromUtf8(*bytes);
    if (text.toUtf8() != *bytes)
        return std::nullopt;
    return text;
}

std::optional<QString> readPath(const QByteArray& value)
{
    const std::optional<QString> path = readText(value, kMaxPathBytes);
    if (!path || !isRemotePath(*path))
        return std::nullopt;
    return path;
}

QByteArray pathValue(const QString& path)
{
    return toBase64Url(path.toUtf8()).toLatin1();
}

} // namespace

// ---- the parts --------------------------------------------------------------------------------------

QString toBase64Url(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::optional<QByteArray> fromBase64Url(const QByteArray& text, int maxBytes)
{
    if (text.isEmpty() || text.size() > (maxBytes * 4 + 2) / 3)
        return std::nullopt;
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '-' || c == '_';
        if (!ok)
            return std::nullopt;
    }
    const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(text, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.size() > maxBytes)
        return std::nullopt;
    // One spelling per value (the unused low bits of the last character must be 0).
    if (toBase64Url(result.decoded).toLatin1() != text)
        return std::nullopt;
    return result.decoded;
}

QString shaText(const QByteArray& sha256)
{
    return sha256.size() == kShaBytes ? toBase64Url(sha256) : QString();
}

QByteArray shaBytes(const QString& text)
{
    if (text.size() != 43)
        return {};
    const std::optional<QByteArray> bytes = fromBase64Url(text.toLatin1(), kShaBytes);
    return bytes && bytes->size() == kShaBytes ? *bytes : QByteArray();
}

bool isUrlKey(const QString& key)
{
    if (key.size() < kMinUrlKeyChars || key.size() > kMaxUrlKeyChars)
        return false;
    for (const QChar ch : key) {
        const ushort c = ch.unicode();
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool isRemotePath(const QString& path)
{
    if (path.size() < 2 || !path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/')) || path.toUtf8().size() > kMaxPathBytes)
        return false;
    for (const QChar ch : path) {
        const ushort c = ch.unicode();
        if (c < 0x20 || (c >= 0x7f && c < 0xa0) || c == '\\' || isHiddenChar(c) || ch.isSurrogate())
            return false;
    }
    const QStringList parts = path.mid(1).split(QLatin1Char('/'));
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    }
    return !extensionOf(path).isEmpty();
}

bool isPreviewPath(const QString& path)
{
    const QString ext = extensionOf(path);
    return isRemotePath(path) && (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("png"));
}

bool isMediaPath(Kind kind, const QString& path)
{
    if (!isRemotePath(path))
        return false;
    const QString ext = extensionOf(path);
    switch (kind) {
    case Kind::Image:
        return ext == QLatin1String("png") || ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("webp");
    case Kind::Gif:
        return ext == QLatin1String("gif");
    case Kind::Video:
        return ext == QLatin1String("mp4") || ext == QLatin1String("webm") || ext == QLatin1String("mov") || ext == QLatin1String("m4v");
    default:
        return false;
    }
}

QString cleanText(const QString& text, int maxBytes)
{
    return textclean::cleanToBytes(text, maxBytes);
}

char kindCode(Kind kind)
{
    switch (kind) {
    case Kind::YouTube:
        return 'y';
    case Kind::Image:
        return 'i';
    case Kind::Gif:
        return 'g';
    case Kind::Video:
        return 'v';
    default:
        return 0;
    }
}

Kind kindOf(char code)
{
    switch (code) {
    case 'y':
        return Kind::YouTube;
    case 'i':
        return Kind::Image;
    case 'g':
        return Kind::Gif;
    case 'v':
        return Kind::Video;
    default:
        return Kind::None;
    }
}

bool isValid(const Announcement& a)
{
    if (a.extended)
        return false; // an LPX (isValidX); the LP rules below are 2.4.0's
    if (!textreact::isLocalKey(a.messageKey) || !isUrlKey(a.urlKey) || kindCode(a.kind) == 0 || !isUid(a.senderUid) || a.serverUid.isEmpty()
        || a.serverUid.size() > 128 || a.channelId == 0)
        return false;
    if (!isPreviewPath(a.previewPath) || shaBytes(a.previewSha).isEmpty() || a.previewSize <= 0 || a.previewSize > kMaxPreviewBytes)
        return false;
    const bool media = !a.mediaPath.isEmpty() || !a.mediaSha.isEmpty() || a.mediaSize != 0;
    if (media) {
        if (a.kind == Kind::YouTube || !isMediaPath(a.kind, a.mediaPath) || shaBytes(a.mediaSha).isEmpty() || a.mediaSize <= 0 || a.mediaSize > kMaxMediaBytes
            || a.mediaPath.compare(a.previewPath, Qt::CaseInsensitive) == 0)
            return false;
    }
    if ((a.width == 0) != (a.height == 0) || a.width < 0 || a.height < 0 || a.width > kMaxSide || a.height > kMaxSide)
        return false;
    if (a.durationMs < 0 || a.durationMs > kMaxDurationMs || (a.durationMs > 0 && a.kind != Kind::Video))
        return false;
    if ((!a.title.isEmpty() || !a.author.isEmpty()) && a.kind != Kind::YouTube)
        return false;
    if (cleanText(a.title, kMaxTitleBytes) != a.title || cleanText(a.author, kMaxAuthorBytes) != a.author)
        return false;
    return true;
}

// ---- LP ---------------------------------------------------------------------------------------------

proto::Message makeMessage(const Announcement& input, proto::Scope scope, const QByteArray& wireRef)
{
    Announcement a = input;
    a.title        = cleanText(a.title, kMaxTitleBytes);
    a.author       = cleanText(a.author, kMaxAuthorBytes);
    // The rules of a stored announcement, with the wire ref in place of the local key.
    Announcement check = a;
    QString      hash;
    qint64       age = 0;
    if (!textreact::parseWireRef(wireRef, &hash, &age))
        return {};
    check.messageKey = textreact::localKey(hash, 1);
    // Who sent it and where the files are come from TeamSpeak on the other side, never from the wire.
    if (check.senderUid.isEmpty())
        check.senderUid = QStringLiteral("sender");
    if (check.serverUid.isEmpty())
        check.serverUid = QStringLiteral("server");
    if (!isValid(check))
        return {};

    proto::Message m;
    m.type = "LP";
    m.set("s", scope == proto::Scope::Private ? QByteArray("p") : QByteArray("c"));
    m.set("m", wireRef);
    m.set("u", a.urlKey.toLatin1());
    m.set("k", QByteArray(1, kindCode(a.kind)));
    m.set("c", QByteArray::number(a.channelId));
    m.set("p", pathValue(a.previewPath));
    m.set("h", a.previewSha.toLatin1());
    m.set("ps", QByteArray::number(a.previewSize));
    if (!a.mediaPath.isEmpty()) {
        m.set("f", pathValue(a.mediaPath));
        m.set("fh", a.mediaSha.toLatin1());
        m.set("fs", QByteArray::number(a.mediaSize));
    }
    if (a.width > 0) {
        m.set("w", QByteArray::number(a.width));
        m.set("ht", QByteArray::number(a.height));
    }
    if (a.durationMs > 0)
        m.set("d", QByteArray::number(a.durationMs));

    // Title and author last: shortened (a character at a time from the end), then left out, to fit.
    QString title  = a.title;
    QString author = a.author;
    for (;;) {
        proto::Message out = m;
        if (!title.isEmpty())
            out.set("t", pathValue(title));
        if (!author.isEmpty())
            out.set("a", pathValue(author));
        if (!proto::serialize(out).isEmpty())
            return out;
        if (!title.isEmpty()) {
            title = cleanText(title.left(qMax(0, title.size() - 8)), kMaxTitleBytes);
            continue;
        }
        if (!author.isEmpty()) {
            author = cleanText(author.left(qMax(0, author.size() - 8)), kMaxAuthorBytes);
            continue;
        }
        return {}; // too long without them: a path is too long
    }
}

std::optional<Incoming> readMessage(const proto::Message& m)
{
    if (m.type != "LP")
        return std::nullopt;
    Incoming in;
    const QByteArray s = m.value("s");
    if (s == "c")
        in.scope = proto::Scope::Channel;
    else if (s == "p")
        in.scope = proto::Scope::Private;
    else
        return std::nullopt;
    if (!textreact::parseWireRef(m.value("m"), &in.hash, &in.ageSec))
        return std::nullopt;

    Announcement& a = in.announcement;
    a.privateChat   = in.scope == proto::Scope::Private;
    a.urlKey        = QString::fromLatin1(m.value("u"));
    if (!isUrlKey(a.urlKey))
        return std::nullopt;
    const QByteArray k = m.value("k");
    if (k.size() != 1 || kindOf(k.at(0)) == Kind::None)
        return std::nullopt;
    a.kind = kindOf(k.at(0));
    if (!readChannel(m.value("c"), &a.channelId))
        return std::nullopt;

    const std::optional<QString> preview = readPath(m.value("p"));
    if (!preview || !isPreviewPath(*preview))
        return std::nullopt;
    a.previewPath = *preview;
    a.previewSha  = QString::fromLatin1(m.value("h"));
    if (shaBytes(a.previewSha).isEmpty() || !readNumber(m.value("ps"), 1, kMaxPreviewBytes, &a.previewSize))
        return std::nullopt;

    const bool media = m.has("f") || m.has("fh") || m.has("fs");
    if (media) {
        if (!m.has("f") || !m.has("fh") || !m.has("fs") || a.kind == Kind::YouTube)
            return std::nullopt;
        const std::optional<QString> file = readPath(m.value("f"));
        if (!file || !isMediaPath(a.kind, *file) || file->compare(a.previewPath, Qt::CaseInsensitive) == 0)
            return std::nullopt;
        a.mediaPath = *file;
        a.mediaSha  = QString::fromLatin1(m.value("fh"));
        if (shaBytes(a.mediaSha).isEmpty() || !readNumber(m.value("fs"), 1, kMaxMediaBytes, &a.mediaSize))
            return std::nullopt;
    }

    if (m.has("w") || m.has("ht")) {
        qint64 w = 0, h = 0;
        if (!readNumber(m.value("w"), 1, kMaxSide, &w) || !readNumber(m.value("ht"), 1, kMaxSide, &h))
            return std::nullopt;
        a.width  = static_cast<int>(w);
        a.height = static_cast<int>(h);
    }
    if (m.has("d")) {
        if (a.kind != Kind::Video || !readNumber(m.value("d"), 1, kMaxDurationMs, &a.durationMs))
            return std::nullopt;
    }
    if (m.has("t") || m.has("a")) {
        if (a.kind != Kind::YouTube)
            return std::nullopt;
        if (m.has("t")) {
            const std::optional<QString> title = readText(m.value("t"), kMaxTitleBytes);
            if (!title)
                return std::nullopt;
            a.title = cleanText(*title, kMaxTitleBytes);
        }
        if (m.has("a")) {
            const std::optional<QString> author = readText(m.value("a"), kMaxAuthorBytes);
            if (!author)
                return std::nullopt;
            a.author = cleanText(*author, kMaxAuthorBytes);
        }
    }
    return in;
}

// ---- LPNO ---------------------------------------------------------------------------------------------

proto::Message makeSkip(proto::Scope scope, const QByteArray& wireRef, const QString& urlKey)
{
    QString hash;
    qint64  age = 0;
    if (!textreact::parseWireRef(wireRef, &hash, &age) || !isUrlKey(urlKey))
        return {};
    proto::Message m;
    m.type = "LPNO";
    m.set("s", scope == proto::Scope::Private ? QByteArray("p") : QByteArray("c"));
    m.set("m", wireRef);
    m.set("u", urlKey.toLatin1());
    if (proto::serialize(m).isEmpty())
        return {};
    return m;
}

std::optional<Skip> readSkip(const proto::Message& m)
{
    if (m.type != "LPNO")
        return std::nullopt;
    Skip             skip;
    const QByteArray s = m.value("s");
    if (s == "c")
        skip.scope = proto::Scope::Channel;
    else if (s == "p")
        skip.scope = proto::Scope::Private;
    else
        return std::nullopt;
    if (!textreact::parseWireRef(m.value("m"), &skip.hash, &skip.ageSec))
        return std::nullopt;
    skip.urlKey = QString::fromLatin1(m.value("u"));
    if (!isUrlKey(skip.urlKey))
        return std::nullopt;
    return skip;
}

// ---- LPX, LPXNO (2.5) ---------------------------------------------------------------------------------------

namespace {

constexpr int kShortenStepBytes = 16;
constexpr int kShortenFloorBytes = 64;

bool readScope(const QByteArray& s, proto::Scope* scope)
{
    if (s == "c")
        *scope = proto::Scope::Channel;
    else if (s == "p")
        *scope = proto::Scope::Private;
    else
        return false;
    return true;
}

QByteArray scopeValue(proto::Scope scope)
{
    return scope == proto::Scope::Private ? QByteArray("p") : QByteArray("c");
}

bool isHex6(const QByteArray& v)
{
    if (v.size() != 6)
        return false;
    for (const char c : v) {
        if (!(isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

QByteArray colourValue(QRgb rgb)
{
    return QByteArray::number(rgb & 0xffffff, 16).rightJustified(6, '0');
}

// A card's text as the wire carries it (whole graphemes within the byte cap).
QString wireText(const QString& text, int maxBytes)
{
    return textclean::cleanToBytes(text, maxBytes);
}

bool sameText(const QString& text, int maxBytes)
{
    return wireText(text, maxBytes) == text;
}

bool emptyCard(const CardData& c)
{
    return c.site.isEmpty() && c.author.isEmpty() && c.title.isEmpty() && c.description.isEmpty() && c.layout == CardLayout::None && !c.hasColor && !c.play;
}

} // namespace

int lpxSendCap(bool teamSpeakOwn, bool longChecked)
{
    return teamSpeakOwn && longChecked ? proto::kMaxLpxSendBytes : kLpxShortCap;
}

char kindCodeX(Kind kind)
{
    switch (kind) {
    case Kind::Image:
        return 'i';
    case Kind::Gif:
        return 'g';
    case Kind::Video:
        return 'v';
    case Kind::Loop:
        return 'a';
    case Kind::Card:
        return 'c';
    default:
        return 0;
    }
}

Kind kindOfX(char code)
{
    switch (code) {
    case 'i':
        return Kind::Image;
    case 'g':
        return Kind::Gif;
    case 'v':
        return Kind::Video;
    case 'a':
        return Kind::Loop;
    case 'c':
        return Kind::Card;
    default:
        return Kind::None;
    }
}

bool isMediaPathX(Kind kind, const QString& path)
{
    if (!isRemotePath(path))
        return false;
    const QString ext = extensionOf(path);
    const auto    is  = [&ext](const char* e) { return ext == QLatin1String(e); };
    switch (kind) {
    case Kind::Image:
        return is("png") || is("jpg") || is("jpeg") || is("webp");
    case Kind::Gif:
        return is("gif") || is("webp");
    case Kind::Video:
        return is("mp4") || is("webm") || is("mov") || is("m4v");
    case Kind::Loop:
        return is("mp4") || is("webm");
    default:
        return false;
    }
}

bool loopTrusted(const Announcement& a)
{
    if (!a.extended || a.kind != Kind::Loop)
        return false;
    if (a.durationMs < 1 || a.durationMs > kMaxLoopDuration)
        return false;
    return a.mediaPath.isEmpty() || (a.mediaSize > 0 && a.mediaSize <= kMaxLoopBytes);
}

bool isValidX(const Announcement& a)
{
    if (!a.extended || kindCodeX(a.kind) == 0 || !textreact::isLocalKey(a.messageKey) || !isUrlKey(a.urlKey) || !isUid(a.senderUid)
        || a.serverUid.isEmpty() || a.serverUid.size() > 128)
        return false;
    const bool card    = a.kind == Kind::Card;
    const bool preview = !a.previewPath.isEmpty() || !a.previewSha.isEmpty() || a.previewSize != 0;
    if (preview) {
        if (a.channelId == 0 || !isPreviewPath(a.previewPath) || shaBytes(a.previewSha).isEmpty() || a.previewSize <= 0 || a.previewSize > kMaxPreviewBytes)
            return false;
    } else if (!card) {
        return false; // media kinds always have their preview
    }
    const bool media = !a.mediaPath.isEmpty() || !a.mediaSha.isEmpty() || a.mediaSize != 0;
    if (media) {
        if (card || !isMediaPathX(a.kind, a.mediaPath) || shaBytes(a.mediaSha).isEmpty() || a.mediaSize <= 0 || a.mediaSize > kMaxMediaBytes
            || a.mediaPath.compare(a.previewPath, Qt::CaseInsensitive) == 0)
            return false;
    }
    if ((a.width == 0) != (a.height == 0) || a.width < 0 || a.height < 0 || a.width > kMaxSide || a.height > kMaxSide || (a.width > 0 && !preview && !media))
        return false;
    if (a.durationMs < 0 || a.durationMs > kMaxDurationMs || (a.durationMs > 0 && a.kind != Kind::Video && a.kind != Kind::Loop && !card))
        return false;
    if (!a.title.isEmpty() || !a.author.isEmpty())
        return false; // YouTube's fields (LP); an LPX card's text is in card
    const CardData& c = a.card;
    if (!card)
        return emptyCard(c);
    if (c.title.isEmpty() || !sameText(c.title, kCardTitleBytes) || !sameText(c.author, kCardAuthorBytes) || !sameText(c.site, kCardSiteBytes)
        || !sameText(c.description, kCardDescriptionBytes))
        return false;
    if (c.description.isEmpty() && !preview)
        return false; // a card needs its text or its picture
    if (c.layout != CardLayout::None && !preview)
        return false;
    if (c.hasColor && !pagemeta::isAcceptableThemeColour(c.color))
        return false;
    return true;
}

bool isValidAny(const Announcement& a)
{
    return a.extended ? isValidX(a) : isValid(a);
}

proto::Message makeLpx(const Announcement& input, proto::Scope scope, const QByteArray& wireRef, int maxBytes)
{
    Announcement a = input;
    a.extended     = true;
    a.title.clear();
    a.author.clear();
    CardData& c = a.card;
    if (a.kind == Kind::Card) {
        c.title       = wireText(c.title, kCardTitleBytes);
        c.author      = wireText(c.author, kCardAuthorBytes);
        c.site        = wireText(c.site, kCardSiteBytes);
        c.description = wireText(c.description, kCardDescriptionBytes);
        if (c.hasColor && !pagemeta::isAcceptableThemeColour(c.color))
            c.hasColor = false;
        if (a.previewPath.isEmpty())
            c.layout = CardLayout::None;
    } else {
        const bool sensitive = a.sensitive;
        c                    = CardData();
        a.sensitive          = sensitive;
    }
    // The rules of a stored LPX, with the wire ref in place of the local key (as LP).
    Announcement check = a;
    QString      hash;
    qint64       age = 0;
    if (!textreact::parseWireRef(wireRef, &hash, &age))
        return {};
    check.messageKey = textreact::localKey(hash, 1);
    if (check.senderUid.isEmpty())
        check.senderUid = QStringLiteral("sender");
    if (check.serverUid.isEmpty())
        check.serverUid = QStringLiteral("server");
    if (!isValidX(check) || maxBytes <= 0)
        return {};

    const bool     preview = !a.previewPath.isEmpty();
    proto::Message m;
    m.type = "LPX";
    m.set("s", scopeValue(scope));
    m.set("m", wireRef);
    m.set("u", a.urlKey.toLatin1());
    m.set("k", QByteArray(1, kindCodeX(a.kind)));
    if (preview) {
        m.set("c", QByteArray::number(a.channelId));
        m.set("p", pathValue(a.previewPath));
        m.set("h", a.previewSha.toLatin1());
        m.set("ps", QByteArray::number(a.previewSize));
    }
    if (!a.mediaPath.isEmpty()) {
        m.set("f", pathValue(a.mediaPath));
        m.set("fh", a.mediaSha.toLatin1());
        m.set("fs", QByteArray::number(a.mediaSize));
    }
    if (a.width > 0) {
        m.set("w", QByteArray::number(a.width));
        m.set("ht", QByteArray::number(a.height));
    }
    if (a.durationMs > 0)
        m.set("d", QByteArray::number(a.durationMs));
    if (a.kind == Kind::Card) {
        if (c.hasColor)
            m.set("cl", colourValue(c.color));
        if (preview && c.layout != CardLayout::None)
            m.set("ly", c.layout == CardLayout::Large ? QByteArray("l") : QByteArray("t"));
        if (c.play)
            m.set("pg", "1");
    }
    if (a.sensitive)
        m.set("sn", "1");

    // The text last, shortened to fit: the description by 16 bytes at a time (whole graphemes) down to 64, then the
    // title the same way, then the author, the site name and the description are left out (a card without its
    // description needs its picture).
    QString title = c.title, author = c.author, site = c.site, text = c.description;
    for (;;) {
        proto::Message out = m;
        if (!title.isEmpty())
            out.set("t", pathValue(title));
        if (!author.isEmpty())
            out.set("a", pathValue(author));
        if (!site.isEmpty())
            out.set("n", pathValue(site));
        if (!text.isEmpty())
            out.set("x", pathValue(text));
        const QByteArray wire = proto::serialize(out);
        if (!wire.isEmpty() && wire.size() <= maxBytes)
            return out;
        if (a.kind != Kind::Card)
            return {};
        const int textBytes  = text.toUtf8().size();
        const int titleBytes = title.toUtf8().size();
        if (!text.isEmpty() && textBytes > kShortenFloorBytes) {
            text = wireText(text, qMax(kShortenFloorBytes, textBytes - kShortenStepBytes));
            continue;
        }
        if (titleBytes > kShortenFloorBytes) {
            title = wireText(title, qMax(kShortenFloorBytes, titleBytes - kShortenStepBytes));
            continue;
        }
        if (!author.isEmpty()) {
            author.clear();
            continue;
        }
        if (!site.isEmpty()) {
            site.clear();
            continue;
        }
        if (!text.isEmpty() && preview) {
            text.clear();
            continue;
        }
        return {}; // too long even so (a long upload folder)
    }
}

std::optional<Incoming> readLpx(const proto::Message& m)
{
    if (m.type != "LPX")
        return std::nullopt;
    Incoming in;
    if (!readScope(m.value("s"), &in.scope) || !textreact::parseWireRef(m.value("m"), &in.hash, &in.ageSec))
        return std::nullopt;
    Announcement& a = in.announcement;
    a.extended      = true;
    a.privateChat   = in.scope == proto::Scope::Private;
    a.urlKey        = QString::fromLatin1(m.value("u"));
    if (!isUrlKey(a.urlKey))
        return std::nullopt;
    const QByteArray k = m.value("k");
    if (k.size() != 1 || kindOfX(k.at(0)) == Kind::None)
        return std::nullopt;
    a.kind          = kindOfX(k.at(0));
    const bool card = a.kind == Kind::Card;

    const bool preview = m.has("c") || m.has("p") || m.has("h") || m.has("ps");
    if (preview) {
        if (!m.has("c") || !m.has("p") || !m.has("h") || !m.has("ps") || !readChannel(m.value("c"), &a.channelId))
            return std::nullopt;
        const std::optional<QString> path = readPath(m.value("p"));
        if (!path || !isPreviewPath(*path))
            return std::nullopt;
        a.previewPath = *path;
        a.previewSha  = QString::fromLatin1(m.value("h"));
        if (shaBytes(a.previewSha).isEmpty() || !readNumber(m.value("ps"), 1, kMaxPreviewBytes, &a.previewSize))
            return std::nullopt;
    } else if (!card) {
        return std::nullopt;
    }
    const bool media = m.has("f") || m.has("fh") || m.has("fs");
    if (media) {
        if (card || !m.has("f") || !m.has("fh") || !m.has("fs"))
            return std::nullopt;
        const std::optional<QString> file = readPath(m.value("f"));
        if (!file || !isMediaPathX(a.kind, *file) || file->compare(a.previewPath, Qt::CaseInsensitive) == 0)
            return std::nullopt;
        a.mediaPath = *file;
        a.mediaSha  = QString::fromLatin1(m.value("fh"));
        if (shaBytes(a.mediaSha).isEmpty() || !readNumber(m.value("fs"), 1, kMaxMediaBytes, &a.mediaSize))
            return std::nullopt;
    }
    if (m.has("w") || m.has("ht")) {
        qint64 w = 0, h = 0;
        if ((!preview && !media) || !readNumber(m.value("w"), 1, kMaxSide, &w) || !readNumber(m.value("ht"), 1, kMaxSide, &h))
            return std::nullopt;
        a.width  = static_cast<int>(w);
        a.height = static_cast<int>(h);
    }
    if (m.has("d")) {
        if ((a.kind != Kind::Video && a.kind != Kind::Loop && !card) || !readNumber(m.value("d"), 1, kMaxDurationMs, &a.durationMs))
            return std::nullopt;
    }
    if (m.has("sn")) {
        if (m.value("sn") != "1")
            return std::nullopt;
        a.sensitive = true;
    }
    const bool cardFields = m.has("t") || m.has("a") || m.has("n") || m.has("x") || m.has("cl") || m.has("ly") || m.has("pg");
    if (!card) {
        if (cardFields)
            return std::nullopt;
        return in;
    }

    CardData&  c        = a.card;
    const auto readCard = [&m](const char* key, int maxBytes, QString* out) {
        if (!m.has(key))
            return true;
        const std::optional<QString> text = readText(m.value(key), maxBytes);
        if (!text)
            return false;
        *out = wireText(*text, maxBytes);
        return true;
    };
    if (!m.has("t") || !readCard("t", kCardTitleBytes, &c.title) || c.title.isEmpty() || !readCard("a", kCardAuthorBytes, &c.author)
        || !readCard("n", kCardSiteBytes, &c.site) || !readCard("x", kCardDescriptionBytes, &c.description))
        return std::nullopt;
    if (c.description.isEmpty() && !preview)
        return std::nullopt;
    if (m.has("cl")) {
        const QByteArray v = m.value("cl");
        if (!isHex6(v))
            return std::nullopt;
        const QRgb colour = 0xff000000u | v.toUInt(nullptr, 16);
        if (pagemeta::isAcceptableThemeColour(colour)) { // else ignored (a card never wears a brand's bar)
            c.color    = colour;
            c.hasColor = true;
        }
    }
    if (m.has("ly")) {
        const QByteArray v = m.value("ly");
        if (!preview || (v != "l" && v != "t"))
            return std::nullopt;
        c.layout = v == "l" ? CardLayout::Large : CardLayout::Thumb;
    } else if (preview) {
        c.layout = pagemeta::layoutFor(QString(), true, a.width, a.height);
    }
    if (m.has("pg")) {
        if (m.value("pg") != "1")
            return std::nullopt;
        c.play = true;
    }
    c.pictureWidth  = a.width;
    c.pictureHeight = a.height;
    c.durationMs    = a.durationMs;
    c.sensitive     = a.sensitive;
    return in;
}

proto::Message makeLpxSkip(proto::Scope scope, const QByteArray& wireRef, const QStringList& urlKeys, SkipReason reason)
{
    QString hash;
    qint64  age = 0;
    if (!textreact::parseWireRef(wireRef, &hash, &age) || urlKeys.isEmpty() || urlKeys.size() > kMaxSkipKeys)
        return {};
    QSet<QString> seen;
    for (const QString& key : urlKeys) {
        if (!isUrlKey(key) || seen.contains(key))
            return {};
        seen.insert(key);
    }
    if (reason != SkipReason::Unknown && urlKeys.size() != 1)
        return {};
    proto::Message m;
    m.type = "LPXNO";
    m.set("s", scopeValue(scope));
    m.set("m", wireRef);
    m.set("u", urlKeys.join(QLatin1Char(',')).toLatin1());
    if (reason == SkipReason::Nothing)
        m.set("r", "n");
    else if (reason == SkipReason::CrossSite)
        m.set("r", "x");
    if (proto::serialize(m).isEmpty())
        return {};
    return m;
}

std::optional<SkipX> readLpxSkip(const proto::Message& m)
{
    if (m.type != "LPXNO")
        return std::nullopt;
    SkipX skip;
    if (!readScope(m.value("s"), &skip.scope) || !textreact::parseWireRef(m.value("m"), &skip.hash, &skip.ageSec))
        return std::nullopt;
    const QByteArray u = m.value("u");
    if (u.isEmpty() || u.size() > kMaxSkipKeys * 64 + kMaxSkipKeys - 1)
        return std::nullopt;
    for (const QByteArray& part : u.split(',')) {
        const QString key = QString::fromLatin1(part);
        if (!isUrlKey(key) || skip.urlKeys.contains(key))
            return std::nullopt;
        skip.urlKeys << key;
    }
    if (skip.urlKeys.isEmpty() || skip.urlKeys.size() > kMaxSkipKeys)
        return std::nullopt;
    // r only with one key; any other value (a newer version's) reads as none: receivers may load it.
    const QByteArray r = m.value("r");
    if (skip.urlKeys.size() == 1) {
        if (r == "n")
            skip.reason = SkipReason::Nothing;
        else if (r == "x")
            skip.reason = SkipReason::CrossSite;
    }
    return skip;
}

const char* skipReasonName(SkipReason reason)
{
    switch (reason) {
    case SkipReason::Nothing:
        return "nothing";
    case SkipReason::CrossSite:
        return "cross-site";
    case SkipReason::Unknown:
        break;
    }
    return "-";
}

} // namespace linkpreview
