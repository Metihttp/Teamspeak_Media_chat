#include "shares.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

#include "i18n.h"
#include "linkurl.h"
#include "medialink.h"
#include "pagemeta.h"

namespace linkpreview::shares {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

// ---- path rules (2.2) ---------------------------------------------------------------------------------------------

// A prefix segment: never s, index.php, apps, public.php, remote.php, and never only dots (no derived address can
// hold a dot segment).
const char kPrefix[] = R"(((?:/(?!(?:s|index\.php|apps|public\.php|remote\.php|\.+)(?:/|$))[A-Za-z0-9._~-]{1,64}){0,2}))";
const char kToken[]   = R"(([A-Za-z0-9-]{6,32}))";
const char kToken15[] = R"(([A-Za-z0-9]{15}))";

struct Rule {
    Form form;
    bool indexPhp; // the form has "/index.php" before "/s/" (fixed)
    bool optionalIndexPhp; // a captured "(/index.php)?" (group 2, the token is group 3)
    QRegularExpression re;
};

QRegularExpression rx(const QString& pattern)
{
    QRegularExpression re(pattern);
    re.optimize();
    return re;
}

const QVector<Rule>& rules()
{
    static const QVector<Rule>* all = [] {
        const QString p   = latin(kPrefix);
        const QString t   = latin(kToken);
        const QString t15 = latin(kToken15);
        auto*         out = new QVector<Rule>; // never deleted: plain data, nothing of it runs at unload
        const auto    add = [out](Form form, bool indexPhp, bool optional, const QString& pattern) { out->append({form, indexPhp, optional, rx(pattern)}); };
        add(Form::Page, true, false, QLatin1Char('^') + p + latin(R"(/index\.php/s/)") + t + latin(R"((?:/\d{1,12})?/?$)"));
        add(Form::Page, false, false, QLatin1Char('^') + p + latin("/s/") + t15 + latin(R"((?:/\d{1,12})?/?$)"));
        add(Form::Download, true, false, QLatin1Char('^') + p + latin(R"(/index\.php/s/)") + t + latin("/download/?$"));
        add(Form::Download, false, false, QLatin1Char('^') + p + latin("/s/") + t15 + latin("/download/?$"));
        add(Form::Preview, true, false, QLatin1Char('^') + p + latin(R"(/index\.php/s/)") + t + latin("/preview/?$"));
        add(Form::Preview, false, false, QLatin1Char('^') + p + latin("/s/") + t15 + latin("/preview/?$"));
        add(Form::PublicPreview, false, true, QLatin1Char('^') + p + latin(R"((/index\.php)?/apps/files_sharing/publicpreview/)") + t + QLatin1Char('$'));
        add(Form::Dav, false, false, QLatin1Char('^') + p + latin(R"(/public\.php/dav/files/)") + t + latin("/?$"));
        return out;
    }();
    return *all;
}

const QRegularExpression& ownCloudPreviewRule()
{
    static const QRegularExpression* re = new QRegularExpression(QLatin1Char('^') + latin(kPrefix) + latin(R"((/index\.php)?/apps/files_sharing/publicpreview$)"));
    return *re;
}

const QRegularExpression& tokenRule()
{
    static const QRegularExpression* re = new QRegularExpression(latin(R"(^[A-Za-z0-9-]{6,32}$)"));
    return *re;
}

// ---- types --------------------------------------------------------------------------------------------------------

bool oneOf(const QString& value, std::initializer_list<const char*> list)
{
    for (const char* item : list) {
        if (value == QLatin1String(item))
            return true;
    }
    return false;
}

FileType byMime(const QString& raw)
{
    QString mime = raw.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (mime.isEmpty())
        return FileType::File;
    if (mime == QLatin1String("httpd/unix-directory"))
        return FileType::Folder;
    if (oneOf(mime, {"image/jpeg", "image/png", "image/webp", "image/heic", "image/heif", "image/tiff", "image/bmp", "image/x-ms-bmp", "image/avif"}))
        return FileType::Picture;
    if (mime == QLatin1String("image/gif"))
        return FileType::Gif;
    if (oneOf(mime, {"video/mp4", "video/webm", "video/quicktime"}))
        return FileType::Video;
    if (mime == QLatin1String("application/pdf"))
        return FileType::Pdf;
    if (oneOf(mime, {"application/msword", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "application/vnd.oasis.opendocument.text",
                     "application/rtf", "text/rtf"}))
        return FileType::Document;
    if (oneOf(mime, {"application/vnd.ms-excel", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                     "application/vnd.oasis.opendocument.spreadsheet", "text/csv"}))
        return FileType::Spreadsheet;
    if (oneOf(mime, {"application/vnd.ms-powerpoint", "application/vnd.openxmlformats-officedocument.presentationml.presentation",
                     "application/vnd.oasis.opendocument.presentation"}))
        return FileType::Presentation;
    if (oneOf(mime, {"application/zip", "application/x-7z-compressed", "application/x-rar-compressed", "application/vnd.rar", "application/x-tar",
                     "application/gzip", "application/x-gzip", "application/x-bzip2", "application/x-xz", "application/zstd"}))
        return FileType::Archive;
    if (mime.startsWith(QLatin1String("audio/")))
        return FileType::Audio;
    if (oneOf(mime, {"text/plain", "text/markdown"}))
        return FileType::Text;
    return FileType::File;
}

FileType byName(const QString& name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return FileType::File;
    const QString ext = name.mid(dot + 1).toLower();
    if (oneOf(ext, {"jpg", "jpeg", "png", "webp", "heic", "heif", "tif", "tiff", "bmp", "avif"}))
        return FileType::Picture;
    if (ext == QLatin1String("gif"))
        return FileType::Gif;
    if (oneOf(ext, {"mp4", "m4v", "webm", "mov"}))
        return FileType::Video;
    if (ext == QLatin1String("pdf"))
        return FileType::Pdf;
    if (oneOf(ext, {"doc", "docx", "odt", "rtf"}))
        return FileType::Document;
    if (oneOf(ext, {"xls", "xlsx", "ods", "csv"}))
        return FileType::Spreadsheet;
    if (oneOf(ext, {"ppt", "pptx", "odp"}))
        return FileType::Presentation;
    if (oneOf(ext, {"zip", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz", "zst"}))
        return FileType::Archive;
    if (oneOf(ext, {"mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wav", "wma"}))
        return FileType::Audio;
    if (oneOf(ext, {"txt", "md"}))
        return FileType::Text;
    return FileType::File;
}

// ---- markers ------------------------------------------------------------------------------------------------------

// Decimal digits only, at most 2^53.
qint64 sizeValue(const QString& text)
{
    const QString digits = text.trimmed();
    if (digits.isEmpty() || digits.size() > 16)
        return -1;
    for (const QChar ch : digits) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return -1;
    }
    bool         ok    = false;
    const qint64 value = digits.toLongLong(&ok);
    return ok && value >= 0 && value <= (qint64(1) << 53) ? value : -1;
}

// An initial state's value: base64 of JSON (entities decoded once), at most kMaxStateJsonBytes decoded. The JSON may be
// a scalar (Qt 5 parses only arrays and objects at the top: it is read as the one element of an array).
bool stateValue(const QByteArray& raw, QJsonValue* out)
{
    const int maxEncoded = 4 * ((kMaxStateJsonBytes + 2) / 3) + 8;
    if (raw.isEmpty() || raw.size() > maxEncoded * 2)
        return false;
    const QByteArray encoded = raw.contains('&') ? pagemeta::decodeEntities(QString::fromLatin1(raw)).toLatin1().trimmed() : raw.trimmed();
    if (encoded.isEmpty() || encoded.size() > maxEncoded)
        return false;
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok || decoded.decoded.isEmpty() || decoded.decoded.size() > kMaxStateJsonBytes)
        return false;
    QJsonParseError     error{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray("[") + decoded.decoded + QByteArray("]"), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1)
        return false;
    *out = doc.array().at(0);
    return true;
}

void readState(const QByteArray& key, const QByteArray& raw, Markers* m)
{
    // Presence alone counts (whatever their value says): a download limit would be used up by every viewer, and the
    // password form shows nothing of the share.
    if (key == "files_downloadlimit-download_limit") {
        m->downloadLimit = true;
        return;
    }
    if (key == "core-publicShareAuth") {
        m->passwordForm = true;
        return;
    }
    QJsonValue v;
    if (!stateValue(raw, &v))
        return;
    if (key == "files_sharing-sharingToken") {
        if (v.isString()) {
            m->hasStateToken = true;
            m->stateToken    = v.toString().left(64);
        }
    } else if (key == "files_sharing-filename") {
        if (v.isString()) {
            m->hasStateName = true;
            m->stateName    = v.toString().left(1024);
        }
    } else if (key == "files_sharing-view") {
        if (v.isString()) {
            m->hasView = true;
            m->view    = v.toString().left(64);
        }
    } else if (key == "files_sharing-isFileRequest") {
        if (v.isBool())
            m->isFileRequest = v.toBool() ? 1 : 0;
    } else if (key == "files_sharing-sharePermissions") {
        const double d = v.toDouble(-1);
        if (v.isDouble() && d >= 0 && d <= 2147483647.0 && d == static_cast<double>(static_cast<qint64>(d)))
            m->permissions = static_cast<qint64>(d);
    } else if (key == "core-public-page-menu") {
        if (!v.isArray())
            return;
        m->menuSeen = true;
        for (const QJsonValue& entry : v.toArray()) {
            const QJsonObject o = entry.toObject();
            if (o.value(QLatin1String("id")).toString() != QLatin1String("download"))
                continue;
            m->menuDownload = true;
            const QJsonValue details = o.value(QLatin1String("details"));
            if (details.isString())
                m->menuSize = sizeValue(details.toString());
            break;
        }
    }
}

void readTemplate(const QByteArray& key, const QByteArray& raw, const QTextCodec* codec, Markers* m)
{
    const QString value = pagemeta::decodeAttribute(raw, codec, 1024);
    if (key == "sharingToken") {
        m->hasTemplateToken = true;
        m->templateToken    = value.left(64);
    } else if (key == "filename") {
        m->hasTemplateName = true;
        m->templateName    = value;
    } else if (key == "mimetype") {
        m->hasMimetype = true;
        m->mimetype    = value.trimmed().toLower().left(128);
    } else if (key == "filesize") {
        m->hasFilesize = true;
        m->filesize    = sizeValue(value);
    } else if (key == "hideDownload") {
        m->hasHideDownload = true;
        m->hideDownload    = value.trimmed().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    }
}

bool isStateKey(const QByteArray& key)
{
    return key == "files_sharing-sharingToken" || key == "files_sharing-filename" || key == "files_sharing-view" || key == "files_sharing-isFileRequest"
           || key == "files_sharing-sharePermissions" || key == "core-public-page-menu" || key == "files_downloadlimit-download_limit"
           || key == "core-publicShareAuth";
}

bool isTemplateKey(const QByteArray& key)
{
    return key == "sharingToken" || key == "filename" || key == "mimetype" || key == "filesize" || key == "hideDownload";
}

void readMeta(const pagemeta::Tag& tag, const QTextCodec* codec, Markers* m)
{
    const QByteArray* property = tag.attr("property");
    const QByteArray* nameAttr = tag.attr("name");
    const QByteArray* content  = tag.attr("content");
    QByteArray        key      = property && !property->trimmed().isEmpty() ? *property : (nameAttr ? *nameAttr : QByteArray());
    key                        = key.trimmed().toLower();
    if (!content || key.isEmpty())
        return;
    if (key != "og:url" && key != "og:image" && key != "og:image:url" && key != "og:image:secure_url" && key != "og:image:type" && key != "og:video:type")
        return;
    const QString value = pagemeta::decodeAttribute(*content, codec).trimmed();
    if (value.isEmpty())
        return;
    if (key == "og:url") {
        if (m->ogUrls.size() < 8)
            m->ogUrls << value;
    } else if (key == "og:image:type") {
        if (m->ogImageType.isEmpty())
            m->ogImageType = value.toLower().left(128);
    } else if (key == "og:video:type") {
        if (m->ogVideoType.isEmpty())
            m->ogVideoType = value.toLower().left(128);
    } else if (!m->ogImages.contains(value) && m->ogImages.size() < 4) {
        m->ogImages << value;
    }
}

bool pathHasSegment(const QUrl& url, const char* name)
{
    const QStringList segments = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& segment : segments) {
        if (segment.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// The final address is a share's password form: .../s/<token>/authenticate[/...] (Nextcloud) or
// .../index.php/s/<token>/authenticate (ownCloud 10).
bool isPasswordForm(const QUrl& finalUrl)
{
    static const QRegularExpression* re = new QRegularExpression(latin(R"(/s/[A-Za-z0-9-]{6,32}/authenticate(?:/.*)?$)"));
    return re->match(finalUrl.path(QUrl::FullyDecoded)).hasMatch();
}

bool tokenProof(const Markers& m, const QString& token)
{
    return (m.hasStateToken && m.stateToken == token) || (m.hasTemplateToken && m.templateToken == token);
}

bool ogUrlProof(const Markers& m, const QUrl& finalUrl, const QString& token)
{
    const QString host = publicHostName(finalUrl);
    if (host.isEmpty())
        return false;
    const QString tail = latin("/s/") + token;
    for (const QString& raw : m.ogUrls) {
        const QUrl url(raw, QUrl::StrictMode);
        if (!url.isValid() || url.isRelative())
            continue;
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
            continue;
        if (publicHostName(url) == host && url.path(QUrl::FullyDecoded).endsWith(tail))
            return true;
    }
    return false;
}

QString nameOf(const Markers& m)
{
    return m.hasStateName ? m.stateName : m.templateName;
}

} // namespace

// ---- the link -----------------------------------------------------------------------------------------------------

ShareLink parse(const QUrl& url)
{
    ShareLink none;
    const QUrl page = normalizedPage(url);
    if (!page.isValid())
        return none;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return none;
    const QString host = publicHostName(page);
    const QString path = page.path(QUrl::FullyEncoded);
    if (host.isEmpty() || !path.contains(QLatin1Char('/')))
        return none;
    for (const Rule& rule : rules()) {
        const QRegularExpressionMatch m = rule.re.match(path);
        if (!m.hasMatch())
            continue;
        ShareLink out;
        out.host     = host;
        out.prefix   = m.captured(1);
        out.indexPhp = rule.optionalIndexPhp ? !m.captured(2).isEmpty() : rule.indexPhp;
        out.token    = m.captured(rule.optionalIndexPhp ? 3 : 2);
        out.form     = rule.form;
        return out;
    }
    const QRegularExpressionMatch oc = ownCloudPreviewRule().match(path);
    if (oc.hasMatch()) {
        const QUrlQuery query(page);
        QString         token;
        int             count = 0;
        for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
            if (item.first == QLatin1String("t")) {
                ++count;
                token = item.second;
            }
        }
        if (count != 1 || !tokenRule().match(token).hasMatch())
            return none;
        ShareLink out;
        out.host     = host;
        out.prefix   = oc.captured(1);
        out.indexPhp = !oc.captured(2).isEmpty();
        out.token    = token;
        out.form     = Form::PublicPreview;
        return out;
    }
    return none;
}

QString subOf(Form form)
{
    switch (form) {
    case Form::Page:
        return latin("page");
    case Form::Download:
        return latin("download");
    case Form::Preview:
        return latin("preview");
    case Form::PublicPreview:
        return latin("pp");
    case Form::Dav:
        return latin("dav");
    case Form::None:
        break;
    }
    return {};
}

int expectedRequestsOf(Form form)
{
    return form == Form::Page || form == Form::Download ? 2 : 1;
}

// ---- addresses ----------------------------------------------------------------------------------------------------

QString root(const ShareLink& s)
{
    return latin("https://") + s.host + s.prefix;
}

QString base(const ShareLink& s)
{
    return s.indexPhp ? root(s) + latin("/index.php") : root(s);
}

QUrl pageUrl(const ShareLink& s)
{
    return QUrl(base(s) + latin("/s/") + s.token, QUrl::StrictMode);
}

QUrl downloadUrl(const ShareLink& s)
{
    return QUrl(base(s) + latin("/s/") + s.token + latin("/download"), QUrl::StrictMode);
}

QUrl previewUrl(const ShareLink& s)
{
    return QUrl(base(s) + latin("/s/") + s.token + latin("/preview"), QUrl::StrictMode);
}

QUrl publicPreviewUrl(const ShareLink& s, int side)
{
    const QString n = QString::number(qBound(16, side, 4096));
    return QUrl(root(s) + latin("/index.php/apps/files_sharing/publicpreview/") + s.token + latin("?x=") + n + latin("&y=") + n + latin("&a=1"),
                QUrl::StrictMode);
}

QUrl ownCloudPreviewUrl(const ShareLink& s, const QString& fileName, int side)
{
    const QString name = encodeFileName(fileName);
    if (name.isEmpty())
        return {};
    const QString n = QString::number(qBound(16, side, 4096));
    return QUrl(root(s) + latin("/index.php/apps/files_sharing/publicpreview?file=/") + name + latin("&t=") + s.token + latin("&x=") + n + latin("&y=") + n
                    + latin("&a=1"),
                QUrl::StrictMode);
}

QString encodeFileName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return {};
    for (const QChar ch : name) {
        const ushort u = ch.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F) || u == '/' || u == '\\')
            return {};
    }
    const QByteArray utf8 = name.toUtf8();
    if (utf8.isEmpty() || utf8.size() > 255 || QString::fromUtf8(utf8) != name)
        return {}; // a lone surrogate doesn't survive the round trip
    // Only RFC 3986 unreserved characters stay literal (QUrl::toPercentEncoding's default).
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

ShareLink baseAfter(const ShareLink& link, const QUrl& finalUrl)
{
    ShareLink out = link;
    out.form      = Form::Page;
    const ShareLink final = parse(finalUrl);
    if (final.form == Form::Page && final.host == link.host && final.token == link.token) {
        out.prefix   = final.prefix;
        out.indexPhp = final.indexPhp;
    }
    return out;
}

bool downloadHopAllowed(const ShareLink& s, const QUrl& hop)
{
    if (publicHostName(hop) != s.host || hop.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
        return false;
    const QString path  = hop.path(QUrl::FullyEncoded);
    const QString query = hop.query(QUrl::FullyEncoded);
    const QString tail  = latin("/s/") + s.token + latin("/download");
    if (query.isEmpty() && (path == s.prefix + tail || path == s.prefix + latin("/index.php") + tail))
        return true;
    const QString dav = s.prefix + latin("/public.php/dav/files/") + s.token;
    return (path == dav || path == dav + QLatin1Char('/')) && (query.isEmpty() || query == QLatin1String("accept=zip"));
}

bool isPreviewAddress(const QUrl& url, const ShareLink& s)
{
    if (!url.isValid() || url.isRelative() || publicHostName(url) != s.host)
        return false;
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(latin("/s/") + s.token + latin("/preview")))
        return true;
    static const QRegularExpression* re = new QRegularExpression(latin(R"(/apps/files_sharing/publicpreview(?:/([A-Za-z0-9-]{6,32}))?$)"));
    const QRegularExpressionMatch m = re->match(path);
    if (!m.hasMatch())
        return false;
    if (!m.captured(1).isEmpty())
        return m.captured(1) == s.token;
    return QUrlQuery(url).queryItemValue(latin("t"), QUrl::FullyDecoded) == s.token;
}

bool isShareFileAddress(const QUrl& url)
{
    if (!url.isValid())
        return false;
    // Decoded as the server reads it (a %2F splits a segment there), in any letter case (refusing too much is harmless).
    static const QRegularExpression* re = new QRegularExpression(
        latin(R"((?:^|/)(?:apps/files_sharing/publicpreview|s/[A-Za-z0-9-]{6,64}/(?:preview|download)|public\.php/(?:dav/files/[^/]+|webdav))"
              R"(|remote\.php/dav/public-files/[^/]+|seafhttp/files/[^/]+)(?:/|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return re->match(url.path(QUrl::FullyDecoded)).hasMatch() || re->match(url.path(QUrl::FullyEncoded)).hasMatch();
}

// ---- markers ------------------------------------------------------------------------------------------------------

Markers readMarkers(const QByteArray& html, const QTextCodec* codec)
{
    Markers        m;
    bool           inBody = false;
    QSet<QByteArray> seen; // the first of each id (or name) wins
    // At most 64 KiB of JSON per state: its base64 (and a little room for entities) is kept, longer values are dropped.
    const int maxValue = 2 * (4 * ((kMaxStateJsonBytes + 2) / 3) + 8);
    const pagemeta::Walk walk = pagemeta::walkTags(html, maxValue, [&](const pagemeta::Tag& tag) {
        if (!inBody) {
            if (tag.end)
                return true;
            if (tag.name == "body")
                inBody = true;
            else if (tag.name == "meta")
                readMeta(tag, codec, &m);
            return true;
        }
        const bool        input = !tag.end && tag.name == "input";
        const QByteArray* type  = input ? tag.attr("type") : nullptr;
        const bool        hidden = type && type->trimmed().toLower() == "hidden";
        const QByteArray* idAttr = input ? tag.attr("id") : nullptr;
        const QByteArray  id     = idAttr ? idAttr->trimmed() : QByteArray();
        const bool        state  = hidden && id.startsWith("initial-state-");
        if (m.blockStarted && !m.blockEnded && !state)
            m.blockEnded = true; // the first tag after the block that isn't one of its inputs
        if (!hidden)
            return true;
        const QByteArray* value = tag.attr("value");
        if (state) {
            m.blockStarted = true;
            const QByteArray key = id.mid(int(sizeof("initial-state-") - 1));
            if (isStateKey(key) && !seen.contains(id)) {
                seen.insert(id);
                readState(key, value ? *value : QByteArray(), &m);
            }
            return true;
        }
        // The old template: by id; by name only on an input without an id.
        QByteArray key = id;
        if (key.isEmpty()) {
            const QByteArray* nameAttr = tag.attr("name");
            key                        = nameAttr ? nameAttr->trimmed() : QByteArray();
        }
        if (isTemplateKey(key) && !seen.contains(key)) {
            seen.insert(key);
            readTemplate(key, value ? *value : QByteArray(), codec, &m);
        }
        return true;
    });
    m.tagLimit = walk.tagLimit;
    return m;
}

bool markersComplete(const Markers& m)
{
    const bool token    = m.hasStateToken || m.hasTemplateToken;
    const bool template_ = m.hasMimetype && m.hasFilesize;
    const bool type     = m.hasView || template_;
    const bool block    = m.blockEnded || (!m.blockStarted && m.hasTemplateToken && template_);
    return block && token && type;
}

bool insideBlock(const Markers& m)
{
    return m.blockStarted && !m.blockEnded;
}

// ---- the decision -------------------------------------------------------------------------------------------------

FileType typeOf(const Markers& m)
{
    if (m.hasView && m.view == QLatin1String("public-share"))
        return FileType::Folder;
    for (const QString& mime : {m.mimetype, m.ogVideoType, m.ogImageType}) {
        const FileType type = byMime(mime);
        if (type != FileType::File)
            return type;
    }
    return byName(nameOf(m));
}

qint64 sizeOf(const Markers& m)
{
    if (m.menuDownload && m.menuSize >= 0)
        return m.menuSize;
    return m.hasFilesize ? m.filesize : -1;
}

bool downloadHidden(const Markers& m)
{
    if (m.hasView)
        return !m.menuDownload; // NC 31+: the menu offers a download only when the owner allows one
    return m.hasHideDownload && m.hideDownload;
}

bool isWebPicture(const Markers& m)
{
    for (const QString& raw : {m.mimetype, m.ogImageType}) {
        const QString mime = raw.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        if (!mime.isEmpty())
            return oneOf(mime, {"image/jpeg", "image/png", "image/webp"});
    }
    const QString name = nameOf(m);
    const QString ext  = name.mid(name.lastIndexOf(QLatin1Char('.')) + 1).toLower();
    return name.contains(QLatin1Char('.')) && oneOf(ext, {"jpg", "jpeg", "png", "webp"});
}

Decision decide(const PageFacts& f)
{
    Decision d;
    const Markers& m = f.markers;
    if (f.status == 404 || f.status == 410) {
        d.action = Action::NotFound;
        d.reason = latin("gone");
        return d;
    }
    if (isPasswordForm(f.finalUrl) || m.passwordForm) {
        d.reason = latin("password");
        return d;
    }
    if (pathHasSegment(f.finalUrl, "login")) {
        d.reason = latin("login");
        return d;
    }
    if (!tokenProof(m, f.link.token) || !ogUrlProof(m, f.finalUrl, f.link.token)) {
        d.action = Action::Unconfirmed;
        d.reason = latin("unconfirmed");
        return d;
    }
    if (m.isFileRequest == 1 || (m.permissions >= 0 && (m.permissions & 1) == 0)) {
        d.reason = latin("drop");
        return d;
    }
    d.type     = typeOf(m);
    d.size     = sizeOf(m);
    d.name     = nameOf(m);
    d.ownCloud = !m.blockStarted;
    d.viewOnly = downloadHidden(m);
    d.action   = Action::Card;
    if (m.downloadLimit || insideBlock(m)) {
        // Any file read (a preview too) would use up one of the owner's downloads, and a read cut inside the block
        // may have missed a limit.
        d.noFiles = true;
        d.reason  = latin(m.downloadLimit ? "limit" : "incomplete");
        return d;
    }
    if (d.type == FileType::Folder) {
        d.reason = latin("folder");
        return d;
    }
    if (d.viewOnly) {
        d.reason = latin("view only");
        return d;
    }
    switch (d.type) {
    case FileType::Picture:
        d.action = Action::Picture;
        return d;
    case FileType::Gif:
        d.action = Action::Gif;
        return d;
    case FileType::Video:
        d.action = f.videoCap <= 0 || (d.size >= 0 && d.size > f.videoCap) ? Action::TooLarge : Action::Video;
        return d;
    default:
        break;
    }
    d.reason = latin("file");
    return d;
}

// ---- the share card -----------------------------------------------------------------------------------------------

QString typeLabel(FileType type)
{
    switch (type) {
    case FileType::Folder:
        return i18n::t("Folder");
    case FileType::Pdf:
        return i18n::t("PDF document");
    case FileType::Document:
        return i18n::t("Document");
    case FileType::Spreadsheet:
        return i18n::t("Spreadsheet");
    case FileType::Presentation:
        return i18n::t("Presentation");
    case FileType::Archive:
        return i18n::t("Archive");
    case FileType::Audio:
        return i18n::t("Audio");
    case FileType::Text:
        return i18n::t("Text file");
    case FileType::Picture:
        return i18n::t("Picture");
    case FileType::Gif:
        return i18n::t("GIF");
    case FileType::Video:
        return i18n::t("Video");
    case FileType::File:
        break;
    }
    return i18n::t("File");
}

QString cardDescription(FileType type, qint64 size, bool viewOnly)
{
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    QString       out = typeLabel(type);
    if (size >= 0)
        out += dot + formatSize(static_cast<quint64>(size));
    if (viewOnly)
        out += dot + i18n::t("View only");
    return out;
}

} // namespace linkpreview::shares
