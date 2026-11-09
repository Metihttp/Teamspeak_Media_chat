#include "linkpreview/linkstate.h"

#include <QFileInfo>

#include "i18n.h"
#include "linkpreview/fetcher.h"

namespace linkstate {

namespace {

constexpr int kMaxSide = 16384;

int boundedSide(int v)
{
    return v > 0 && v <= kMaxSide ? v : 0;
}

QString extensionFor(const QString& mime, const linkpreview::Target& target)
{
    const QString m = mime.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    static const struct {
        const char* mime;
        const char* ext;
    } table[] = {{"image/gif", "gif"},   {"image/png", "png"},   {"image/jpeg", "jpg"},       {"image/jpg", "jpg"},
                 {"image/webp", "webp"}, {"video/mp4", "mp4"},   {"video/webm", "webm"},      {"video/quicktime", "mov"},
                 {"video/x-m4v", "m4v"}};
    for (const auto& row : table) {
        if (m == QLatin1String(row.mime))
            return QString::fromLatin1(row.ext);
    }
    const QString fromUrl = QFileInfo(target.url.path()).suffix().toLower();
    for (const char* ext : {"png", "jpg", "jpeg", "gif", "webp", "mp4", "webm", "mov", "m4v"}) {
        if (fromUrl == QLatin1String(ext))
            return fromUrl == QLatin1String("jpeg") ? QString::fromLatin1("jpg") : fromUrl;
    }
    switch (target.kind) {
    case linkpreview::Kind::Gif:
        return QString::fromLatin1("gif");
    case linkpreview::Kind::Video:
        return QString::fromLatin1("mp4");
    default:
        return QString::fromLatin1("jpg");
    }
}

} // namespace

void readEntry(ServerState* server, const MediaEntry* entry)
{
    if (!server || !entry)
        return;
    server->state = entry->state;
    server->error = entry->error;
    // The preview picture, for good only (as serverFailed() reads the main file): gone from the server, not ours to
    // download there, or not the one announced (a preview is up before its LP goes: no upload still arriving). A
    // lost connection or a full quota keeps the sender's preview; Core loads the picture again (after a reconnect)
    // or on a click.
    const MediaError pe   = entry->previewError;
    server->previewFailed = entry->previewState == MediaState::Failed &&
                            (pe == MediaError::NotFound || pe == MediaError::Permission || pe == MediaError::Password || pe == MediaError::Mismatch);
}

bool kindFits(linkpreview::Kind announced, linkpreview::Kind link)
{
    using linkpreview::Kind;
    const auto picture = [](Kind k) { return k == Kind::Image || k == Kind::Gif; };
    if (announced == Kind::None || link == Kind::None)
        return false;
    return announced == link || (picture(announced) && picture(link));
}

bool serverFailed(const ServerState& server)
{
    if (!server.announced)
        return false;
    // The main file, for good: it is gone, or this viewer may not download it there. Anything else (a lost
    // connection, a full quota, a file that didn't match while the sender's upload was still arriving) keeps the
    // sender's preview with Core's own "Click to try again": a TeamSpeak hiccup never turns it into a card that asks
    // the website.
    if (server.state == MediaState::Failed)
        return server.error == MediaError::NotFound || server.error == MediaError::Permission || server.error == MediaError::Password;
    // An announced full file that isn't loaded automatically (a video, the data saver, a lower limit here): its
    // preview picture is all there is to show. Gone from the server, not ours to download there or not the one
    // announced (previewFailed, read by readEntry for good only): nothing is left.
    return server.hasMedia && server.previewFailed && server.state == MediaState::Idle;
}

Decision decide(linkpreview::Kind kind, bool hidden, const LocalState& local, const ServerState& server, Wait wait)
{
    Decision d;
    if (hidden) {
        d.look = linkcard::Look::Hidden;
        return d;
    }
    if (local.state == Local::Loaded) {
        if (kind == linkpreview::Kind::YouTube) {
            d.look = linkcard::Look::YouTube;
        } else if (!local.mediaKey.isEmpty()) {
            d.media    = true;
            d.mediaKey = local.mediaKey;
        }
        if (d.media || d.look == linkcard::Look::YouTube)
            return d;
    }
    if (server.announced && !serverFailed(server) && !server.mainKey.isEmpty()) {
        d.fromServer = true;
        if (kind == linkpreview::Kind::YouTube) {
            d.look = linkcard::Look::YouTube;
        } else if (kind == linkpreview::Kind::Video && !server.hasMedia) {
            d.look = linkcard::Look::Poster;
        } else {
            d.media    = true;
            d.mediaKey = server.mainKey;
        }
        return d;
    }
    switch (local.state) {
    case Local::Loading:
        d.look = linkcard::Look::Loading;
        break;
    case Local::Failed:
        d.look = linkcard::Look::Error;
        break;
    case Local::TooLarge:
        d.look = linkcard::Look::TooLarge;
        break;
    case Local::Idle:
    case Local::Loaded:
    case Local::OverAutoLimit:
        // Waiting for our pipeline or the sender: whatever size we learned elsewhere (their preview is small).
        // An automatic load: only a link nobody tried here yet (a file above the limit stays a click card).
        if (wait == Wait::Own || wait == Wait::Sender || wait == Wait::Checking || (wait == Wait::Auto && local.state == Local::Idle)) {
            d.look = linkcard::Look::Loading;
            d.wait = wait;
        } else {
            d.look = linkcard::Look::Click;
        }
        break;
    }
    return d;
}

// ---- Core links ---------------------------------------------------------------------------------------

linkpreview::Announcement withFileBrowser(linkpreview::Announcement a, const QString& serverUid, quint64 channelId)
{
    if (a.serverUid.isEmpty())
        a.serverUid = serverUid;
    if (a.channelId == 0)
        a.channelId = channelId;
    return a;
}

bool serverLink(const linkpreview::Announcement& a, MediaLink* main)
{
    if (!main || a.serverUid.isEmpty() || a.channelId == 0 || !linkpreview::isValid(a))
        return false; // nothing that can be checked: nothing is downloaded
    const MediaLink link = linkpreview::linkFor(a);
    if (!link.isValid() || link.sha256.size() != 32)
        return false;
    *main = link;
    return true;
}

QUrl webAddress(const QString& href, const linkpreview::Target& target)
{
    // Only an href that classify() takes as this very link (its rules: no user info, standard ports, a public host).
    if (!href.isEmpty() && target.isValid() && linkpreview::classify(href).key == target.key) {
        QUrl url(href, QUrl::TolerantMode);
        if (url.scheme() == QLatin1String("http")) // YouTube only (classify takes no other http link)
            url.setScheme(QString::fromLatin1("https"));
        url.setPort(-1); // the standard one (classify takes no other)
        if (url.isValid() && url.scheme() == QLatin1String("https"))
            return url;
    }
    return target.url;
}

QString errorText(const QString& code)
{
    // The website's details (blocked, offline, timeout, tls, ...) go to the private log only: one neutral line
    // for all of them. Only what the user can act on is said.
    if (code == QLatin1String(linkpreview::errors::kUnavailable))
        return i18n::t("This video isn't available");
    if (code == QLatin1String(linkpreview::errors::kNotFound))
        return i18n::t("The file isn't on the website any more");
    if (code == QLatin1String(linkpreview::errors::kBadType))
        return i18n::t("This link isn't a picture or video");
    if (code == QLatin1String(linkpreview::errors::kTooManyPixels))
        return i18n::t("The picture is too large to show");
    return {};
}

QString webServerUid()
{
    return QString::fromLatin1("tsmedia-web");
}

MediaLink webLink(const linkpreview::Target& target, const QString& mime, qint64 size, int width, int height, qint64 durationMs)
{
    MediaLink link;
    link.serverUid = webServerUid();
    link.channelId = 1;
    link.path      = QString::fromLatin1("/web/") + target.key;
    link.protocol  = MediaLink::kProtocol;
    link.size      = size > 0 ? static_cast<quint64>(size) : 0;
    link.width     = boundedSide(width);
    link.height    = boundedSide(height);
    link.durationMs = durationMs > 0 ? durationMs : 0;
    // The name for Save as and the viewer's title: the address' file name, letters, digits and . _ - only.
    QString base = QFileInfo(target.url.path()).completeBaseName();
    QString clean;
    for (const QChar c : base) {
        if (clean.size() >= 60)
            break;
        clean += (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')) && c.unicode() < 0x2000 ? c : QLatin1Char('_');
    }
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    if (clean.isEmpty())
        clean = target.kind == linkpreview::Kind::Video ? QString::fromLatin1("video") : QString::fromLatin1("image");
    link.fileName = clean + QLatin1Char('.') + extensionFor(mime, target);
    return link;
}

bool isWebLink(const MediaLink& link)
{
    return link.serverUid == webServerUid();
}

} // namespace linkstate
