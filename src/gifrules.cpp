#include "gifrules.h"

#include <QDateTime>
#include <QLocale>
#include <QSet>
#include <QUrl>

#include "i18n.h"
#include "linkpreview/lpmessage.h"
#include "linkpreview/shares.h"

namespace gifs {

namespace {

// Every provider id of providers::ids(), with the subs (the row's form) that are recorded automatically. An id with
// an empty list is assigned and never automatic. Capability rows (share tokens, signed or personal links) are never
// automatic, whatever their kind.
struct ProviderRule {
    const char* id;
    const char* autoSubs; // space-separated; "*": every link of the id; "": none
};

const ProviderRule kProviderRules[] = {
    // The public GIF sites (3.1).
    {"giphy", "gifs embed stickers short"}, // not "clips" (videos with sound)
    {"tenor", "view"},
    {"tenor-short", "*"},                   // the Direct rewrite of tenor.com/<code>.gif
    {"klipy", "gifs stickers ai-gifs emojis"}, // not "clips", not "static-memes"
    {"imgur", "gifv"},                      // not posts or albums
    {"imgflip", "gif"},                     // not "i" (stills)
    {"gifer", "gif"},
    {"makeagif", "gif"},
    // Capability rows: never automatic (a share token, a personal server, any http link or a joiner).
    {"dropbox", ""},
    {"media-file", ""},
    {"nextcloud", ""},
    // Other media pages: by hand only (Save to GIFs, when the chat shows them as a GIF or loop).
    {"reddit-video", ""},
    {"pinterest", ""},
    {"streamable", ""},
    {"9gag", ""},
    {"coub", ""},
    {"gyazo", ""},
    {"lightshot", ""},
    {"imgbb", ""},
    {"postimages", ""},
    {"pixeldrain", ""},
    {"tumblr", ""},
    // Site cards and Exclude rows: never GIFs.
    {"gfycat", ""},
    {"redgifs", ""},
    {"tiktok", ""},
    {"imdb", ""},
    {"amazon", ""},
    {"googlemaps", ""},
    {"reddit-share", ""},
    {"twitch-vod", ""},
    {"telegram-private", ""},
    {"telegram-service", ""},
    {"discord-channels", ""},
    {"vimeo", ""},
    {"soundcloud", ""},
    {"spotify", ""},
    {"applemusic", ""},
    {"deezer", ""},
    {"dailymotion", ""},
    {"medal", ""},
    {"twitch", ""},
    {"kick", ""},
    {"bluesky", ""},
    {"x", ""},
    {"github", ""},
    {"steam", ""},
    {"wikipedia", ""},
    {"telegram", ""},
    {"discord", ""},
    {"reddit", ""},
    {"youtube-extra", ""},
};

const ProviderRule* ruleOf(const QString& id)
{
    for (const ProviderRule& rule : kProviderRules) {
        if (id == QLatin1String(rule.id))
            return &rule;
    }
    return nullptr;
}

// Hosts whose links are signed, expiring or personal: never recorded automatically, even as plain .gif links.
bool isNeverHost(const QString& host)
{
    static const char* const kHosts[] = {"cdn.discordapp.com", "media.discordapp.net", "www.dropbox.com", "dl.dropbox.com", "dropbox.com"};
    for (const char* never : kHosts) {
        if (host == QLatin1String(never))
            return true;
    }
    return host.endsWith(QLatin1String(".dropboxusercontent.com")) || host.endsWith(QLatin1String(".discordapp.net"))
           || host.endsWith(QLatin1String(".discordapp.com"));
}

QString lastSegment(const QUrl& url)
{
    const QString path = url.path(QUrl::FullyDecoded);
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1).toLower();
}

} // namespace

QString pidOf(const linkpreview::Target& target)
{
    if (target.cls != linkpreview::LinkClass::MediaPage || target.provider.isEmpty() || target.itemId.isEmpty())
        return {};
    if (target.provider == QLatin1String("tenor-short") || target.provider == QLatin1String("dropbox") || target.provider == QLatin1String("media-file")
        || target.provider == QLatin1String("nextcloud"))
        return {};
    return target.provider + QLatin1Char(':') + target.itemId;
}

bool providerAssigned(const QString& providerId)
{
    return ruleOf(providerId) != nullptr;
}

bool providerAutomatic(const QString& providerId, const QString& sub)
{
    const ProviderRule* rule = ruleOf(providerId);
    if (!rule || !*rule->autoSubs)
        return false;
    if (qstrcmp(rule->autoSubs, "*") == 0)
        return true;
    if (sub.isEmpty())
        return false;
    const QStringList subs = QString::fromLatin1(rule->autoSubs).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return subs.contains(sub);
}

Candidate candidateOf(const QString& href)
{
    using linkpreview::Kind;
    using linkpreview::LinkClass;
    Candidate c;
    c.url    = linkpreview::trimLinkEnd(href);
    c.target = linkpreview::classifyCached(c.url);
    const linkpreview::Target& t = c.target;
    if (!t.isValid() || !storableUrl(c.url))
        return c;
    c.pid = pidOf(t);
    const QString segment = lastSegment(t.url);
    if (t.cls == LinkClass::Direct) {
        const bool gif  = t.kind == Kind::Gif;
        const bool webp = t.kind == Kind::Image && segment.endsWith(QLatin1String(".webp"));
        c.gifLike       = gif || webp || t.kind == Kind::Video;
        c.kind          = t.kind == Kind::Video ? gifs::Kind::Loop : gifs::Kind::Gif;
        if (!gif && !webp)
            return c; // a direct video: by hand only, when the chat plays it as a loop (C2)
        // The link as written: https, no user info, no query, no fragment, not on a host of signed or personal links.
        const QUrl written(c.url, QUrl::TolerantMode);
        const bool plainLink = written.scheme() == QLatin1String("https") && written.userInfo().isEmpty() && !written.hasQuery()
                               && !written.hasFragment() && !c.url.contains(QLatin1Char('?')) && !c.url.contains(QLatin1Char('#'));
        if (t.provider == QLatin1String("dropbox") || isNeverHost(t.host) || !plainLink)
            return c;
        // A self-hosted share's file link carries its share token in the path (Nextcloud and ownCloud
        // .../s/<token>/download/<name>.gif and .../public.php/dav/files/<token>/..., oCIS .../dav/public-files/<token>/...,
        // Seafile .../seafhttp/files/<token>/...): a capability, by hand only (D3, C6).
        if (linkpreview::shares::isShareFileAddress(written) || linkpreview::shares::isShareFileAddress(t.url))
            return c;
        if (!t.provider.isEmpty() && !providerAutomatic(t.provider, t.sub))
            return c;
        c.autoRecord  = true;
        c.checkFrames = webp;
        return c;
    }
    if (t.cls != LinkClass::MediaPage)
        return c; // YouTube, Site, Web: never GIFs
    c.gifLike = t.kind == Kind::Gif || t.kind == Kind::Loop || t.kind == Kind::Video || t.kind == Kind::Image;
    c.kind    = t.kind == Kind::Gif || t.kind == Kind::Image ? gifs::Kind::Gif : gifs::Kind::Loop;
    c.autoRecord = (t.kind == Kind::Gif || t.kind == Kind::Loop) && providerAutomatic(t.provider, t.sub) && !isNeverHost(t.host);
    return c;
}

QVector<Candidate> messageLinks(const QString& bbcode)
{
    QVector<linkpreview::Target> targets;
    QHash<QString, QString>      hrefOf; // key -> the first link text of it
    for (const QString& href : linkpreview::message::linkHrefs(bbcode, linkpreview::kMaxScannedLinks)) {
        const QString             text = linkpreview::trimLinkEnd(href);
        const linkpreview::Target t    = linkpreview::classifyCached(text);
        if (!t.isValid() || hrefOf.contains(t.key))
            continue;
        hrefOf.insert(t.key, text);
        targets.append(t);
    }
    QVector<Candidate> out;
    for (const linkpreview::Target& t : linkpreview::selectLinks(targets))
        out.append(candidateOf(hrefOf.value(t.key)));
    return out;
}

QString linkText(const QString& url)
{
    return QLatin1String("[URL]") + url + QLatin1String("[/URL]");
}

Verdict judge(const Facts& facts)
{
    if (facts.format == "gif" && facts.frames != 1)
        return Verdict::Gif;
    if (facts.format == "webp" && facts.frames > 1)
        return Verdict::Webp;
    return Verdict::NotGif;
}

ChatGifVerdict chatGifOf(const ChatGifFacts& f, qint64 maxCopyBytes)
{
    ChatGifVerdict out;
    if (f.spoilerHidden)
        return out;
    if (f.web) {
        if (f.kind == MediaKind::AnimatedImage || (f.kind == MediaKind::Image && f.webpName && f.animates)) {
            out.type = ChatGif::Link;
            out.kind = Kind::Gif;
        } else if (f.kind == MediaKind::Video && f.loop) {
            out.type = ChatGif::Link;
            out.kind = Kind::Loop;
        }
        return out;
    }
    if (f.gone)
        return out;
    const bool gifFile  = f.kind == MediaKind::AnimatedImage;
    const bool webpFile = f.kind == MediaKind::Image && f.webpName;
    if (!gifFile && !webpFile)
        return out;
    if (f.size > maxCopyBytes) {
        out.type = f.ready ? ChatGif::TooBig : ChatGif::None;
        return out;
    }
    out.kind = webpFile ? Kind::Webp : Kind::Gif;
    if (webpFile) {
        // Only one the chat animates (a still WebP is no GIF), and that is known once it is here.
        out.type = f.ready && f.animates ? ChatGif::File : ChatGif::None;
        return out;
    }
    out.type = f.ready ? ChatGif::File : ChatGif::FileNotReady;
    return out;
}

bool ChatFingerprint::operator==(const ChatFingerprint& o) const
{
    return valid == o.valid && sch == o.sch && serverUid == o.serverUid && mode == o.mode && clientUid == o.clientUid && channelId == o.channelId;
}

QString blockOf(const Item& item, const SendState& state, bool copyExists)
{
    if (!state.canText)
        return state.blocked;
    if (item.source == Source::Link)
        return {};
    if (!copyExists)
        return missingCopyText();
    if (!state.canFiles)
        return state.filesBlocked;
    if (state.uploadLimit > 0 && item.size > state.uploadLimit)
        return state.uploadLimitText;
    return {};
}

QString missingCopyText()
{
    return i18n::t("The saved copy of this GIF is missing. Remove it from the list.");
}

QString waitText()
{
    return i18n::t("Wait a moment before sending another GIF.");
}

QString changedText()
{
    return i18n::t("The chat changed. Open the picker again.");
}

QString tooBigText()
{
    return i18n::t("This GIF is too large to keep (over 25 MB).");
}

QString serverFileText()
{
    return i18n::t("Uploads to your channel's files; others may lack access.");
}

QString dateText(qint64 unixSec)
{
    const QDate date = QDateTime::fromSecsSinceEpoch(unixSec).date();
    return QLocale(QLocale::English).toString(date, QString::fromLatin1("MMM d"));
}

} // namespace gifs
