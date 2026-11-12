// Generated from refs/tags/v2.4.0 (src/linkpreview/announcement.cpp, peerprotocol.cpp, previewstore.cpp,
// lphub.cpp): do not edit. See compat24.h.

#include "compat24.h"

#include <QVector>

#include "textreact.h"

namespace compat24 {

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

// The longest start of text (whole characters) whose UTF-8 is at most maxBytes.
QString fitBytes(const QString& text, int maxBytes)
{
    QString out;
    int     bytes = 0;
    const QVector<uint> codes = text.toUcs4();
    for (const uint code : codes) {
        const int n = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        if (bytes + n > maxBytes)
            break;
        bytes += n;
        out += QString::fromUcs4(&code, 1);
    }
    return out;
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
    QString out;
    out.reserve(qMin(text.size(), maxBytes));
    bool                space = false;
    const QVector<uint> codes = text.left(4 * maxBytes + 64).toUcs4(); // a hostile title is cut long before it is looked at
    for (const uint code : codes) {
        if (QChar::isSpace(code)) { // line breaks and tabs too
            space = !out.isEmpty();
            continue;
        }
        if (code < 0x20 || (code >= 0x7f && code < 0xa0) || isHiddenChar(code) || code == 0xfffd || (code >= 0xd800 && code <= 0xdfff))
            continue;
        if (space)
            out += QLatin1Char(' ');
        space = false;
        out += QString::fromUcs4(&code, 1);
    }
    return fitBytes(out, maxBytes).trimmed();
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


// ---- 2.4.0's tsm1 parser and HELLO reader (peerprotocol.cpp at v2.4.0) ------------------------------------------

namespace {

constexpr char kMagic[]     = "tsm1";
constexpr int  kMaxTypeLen  = 8;
constexpr int  kMaxKeyLen   = 8;
constexpr int  kMaxCapLen   = 4;
constexpr int  kMaxCaps     = 8;

bool isLower24(char c)
{
    return c >= 'a' && c <= 'z';
}

bool isUpper24(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool isDigit24(char c)
{
    return c >= '0' && c <= '9';
}

bool isValueChar24(char c)
{
    return isLower24(c) || isUpper24(c) || isDigit24(c) || c == '.' || c == '_' || c == ',' || c == ':' || c == '-';
}

bool allOf24(const QByteArray& text, bool (*pred)(char))
{
    for (const char c : text) {
        if (!pred(c))
            return false;
    }
    return true;
}

bool isType24(const QByteArray& type)
{
    return !type.isEmpty() && type.size() <= kMaxTypeLen && allOf24(type, isUpper24);
}

bool isKey24(const QByteArray& key)
{
    return !key.isEmpty() && key.size() <= kMaxKeyLen && allOf24(key, isLower24);
}

bool isValue24(const QByteArray& value)
{
    return value.size() <= proto::kMaxValueChars && allOf24(value, isValueChar24);
}

bool isOurMagicFamily24(const QByteArray& token)
{
    if (token.size() <= 3 || !token.startsWith("tsm"))
        return false;
    for (int i = 3; i < token.size(); ++i) {
        if (!isDigit24(token.at(i)))
            return false;
    }
    return true;
}

bool isVersionText24(const QByteArray& v)
{
    const QList<QByteArray> parts = v.split('.');
    if (parts.isEmpty() || parts.size() > 4)
        return false;
    for (const QByteArray& part : parts) {
        if (part.isEmpty() || part.size() > 3 || !allOf24(part, isDigit24))
            return false;
    }
    return true;
}

QString str24(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

qint64 num24(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<qint64>(v.toDouble()) : 0;
}

} // namespace

std::optional<proto::Message> parse(const QByteArray& raw, proto::ParseResult* result)
{
    using proto::ParseResult;
    ParseResult dummy;
    ParseResult& r = result ? *result : dummy;
    if (raw.isEmpty()) {
        r = ParseResult::Empty;
        return std::nullopt;
    }
    if (raw.size() > proto::kMaxRecvBytes) {
        r = ParseResult::TooLong;
        return std::nullopt;
    }
    const int firstSpace = raw.indexOf(' ');
    const QByteArray magic = firstSpace < 0 ? raw : raw.left(firstSpace);
    if (magic != kMagic) {
        r = isOurMagicFamily24(magic) ? ParseResult::NewerVersion : ParseResult::NotOurs;
        return std::nullopt;
    }
    for (int i = 0; i < raw.size(); ++i) {
        const char c = raw.at(i);
        if (c < 0x20 || c > 0x7e) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
    }
    if (raw.endsWith(' ') || raw.contains("  ")) {
        r = ParseResult::Malformed;
        return std::nullopt;
    }
    const QList<QByteArray> tokens = raw.split(' ');
    if (tokens.size() < 2 || tokens.size() > proto::kMaxTokens || !isType24(tokens.at(1))) {
        r = ParseResult::Malformed;
        return std::nullopt;
    }
    proto::Message m;
    m.type = tokens.at(1);
    for (int i = 2; i < tokens.size(); ++i) {
        const QByteArray& token = tokens.at(i);
        const int         eq    = token.indexOf('=');
        if (eq <= 0) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
        const QByteArray key   = token.left(eq);
        const QByteArray value = token.mid(eq + 1);
        if (!isKey24(key) || !isValue24(value) || m.has(key.constData())) {
            r = ParseResult::Malformed;
            return std::nullopt;
        }
        m.fields.append({key, value});
    }
    r = ParseResult::Ok;
    return m;
}

bool Hello::hasCap(const char* cap) const
{
    return caps.contains(QString::fromLatin1(cap));
}

std::optional<Hello> readHello(const proto::Message& message)
{
    Hello hello;
    if (message.type == "HI")
        hello.hi = true;
    else if (message.type != "HELLO")
        return std::nullopt;
    bool speaksOne = false;
    for (const QByteArray& pv : message.value("pv").split(',')) {
        if (pv.size() != 1 || !isDigit24(pv.at(0)))
            return std::nullopt;
        speaksOne = speaksOne || pv == "1";
    }
    if (!speaksOne)
        return std::nullopt;
    const QByteArray v = message.value("v");
    if (isVersionText24(v))
        hello.version = QString::fromLatin1(v);
    const QByteArray caps = message.value("caps");
    if (!caps.isEmpty()) {
        for (const QByteArray& cap : caps.split(',')) {
            if (cap.isEmpty() || cap.size() > kMaxCapLen || !allOf24(cap, isLower24))
                continue;
            const QString name = QString::fromLatin1(cap);
            if (!hello.caps.contains(name) && hello.caps.size() < kMaxCaps)
                hello.caps.append(name);
        }
    }
    hello.replyRequested = !hello.hi && message.value("rr") == "1";
    return hello;
}

bool hubTakes(const proto::Message& message)
{
    const bool skip = message.type == "LPNO";
    return message.type == "LP" || skip;
}

bool storeEntry(const QJsonObject& o, Announcement* a)
{
    const QString k = str24(o, "k");
    bool          ok = false;
    a->messageKey  = str24(o, "m");
    a->urlKey      = str24(o, "u");
    a->kind        = k.size() == 1 ? kindOf(k.at(0).toLatin1()) : Kind::None;
    a->serverUid   = str24(o, "s");
    a->channelId   = str24(o, "c").toULongLong(&ok);
    a->previewPath = str24(o, "p");
    a->previewSha  = str24(o, "h");
    a->previewSize = num24(o, "ps");
    a->mediaPath   = str24(o, "f");
    a->mediaSha    = str24(o, "fh");
    a->mediaSize   = num24(o, "fs");
    a->width       = static_cast<int>(qBound<qint64>(-1, num24(o, "w"), kMaxSide + 1));
    a->height      = static_cast<int>(qBound<qint64>(-1, num24(o, "ht"), kMaxSide + 1));
    a->durationMs  = num24(o, "d");
    a->title       = str24(o, "t");
    a->author      = str24(o, "a");
    a->privateChat = o.value(QLatin1String("pc")).toBool();
    a->senderUid   = str24(o, "from");
    return ok && isValid(*a);
}

} // namespace compat24
