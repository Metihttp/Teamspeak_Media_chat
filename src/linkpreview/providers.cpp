#include "providers.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>
#include <vector>

#include "netpolicy.h"
#include "shares.h"

namespace linkpreview::providers {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

// ---- the rows -----------------------------------------------------------------------------------------------

// Hosts that a regular expression names (all anchored, all owned by the provider).
enum class Family { None, Pinterest, Wikipedia, Tumblr, Amazon, GoogleMaps, GoogleWww };

// Extra rules a regular expression can't say well.
enum class Check {
    None,
    TenorId,            // the id parses as a non-zero unsigned 64-bit number; small ids need oEmbed (2 requests)
    ImgurPost,          // the id holds a digit or an upper-case letter (/privacy, /upload are words)
    StreamableId,       // the id holds a digit
    SoundCloudProfile,  // the first segment isn't one of SoundCloud's own pages
    SoundCloudTrack,    // ... and the second isn't a profile tab
    TwitchClipId,       // not "embed" / "create"
    TwitchChannel,      // not one of Twitch's own pages
    KickChannel,        // not one of Kick's own pages
    GitHub,             // not one of GitHub's own pages; downloads refused
    SteamFile,          // ?id=<1-12 digits>
    WikiTitle,          // not Special:, not "." / ".."
    TelegramService,    // (an Exclude row) the first segment is one of Telegram's own action pages
};

struct Row {
    const char* id;
    LinkClass   cls;      // MediaPage, Site; None: an Exclude row
    const char* hosts;    // exact hosts, separated by spaces ("" with a family)
    Family      family;
    const char* path;     // anchored, over the decoded path
    const char* sub;      // a fixed form; nullptr: capture subGroup
    int         idGroup;  // the item's id (0: none)
    int         id2Group; // a second part of the id, joined with '/' (0: none)
    int         subGroup;
    Kind        expected;
    int         requests;
    bool        pageStep; // the first step reads the link's own page: non-HTML files and action links are refused
    Check       check;
    bool        active;
};

constexpr LinkClass M = LinkClass::MediaPage;
constexpr LinkClass S = LinkClass::Site;
constexpr LinkClass X = LinkClass::None;

// Order matters: the first active row whose host and path rule match wins (Exclude rows of a provider come before
// its other rows).
const Row kRows[] = {
    // ---- Exclude: never a preview, never a request ----
    {"gfycat", X, "gfycat.com www.gfycat.com thumbs.gfycat.com giant.gfycat.com", Family::None, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"redgifs", X, "redgifs.com www.redgifs.com i.redgifs.com v3.redgifs.com api.redgifs.com", Family::None, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"tiktok", X, "tiktok.com www.tiktok.com m.tiktok.com vm.tiktok.com vt.tiktok.com", Family::None, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"imdb", X, "imdb.com www.imdb.com m.imdb.com", Family::None, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"amazon", X, "a.co amzn.to amzn.eu amzn.asia", Family::Amazon, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"googlemaps", X, "maps.app.goo.gl", Family::GoogleMaps, R"(^.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"googlemaps", X, "", Family::GoogleWww, R"(^/maps(?:/.*)?$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"googlemaps", X, "goo.gl", Family::None, R"(^/maps/.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"reddit-share", X, "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com", Family::None, R"(^/r/[^/]+/s/.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"twitch-vod", X, "www.twitch.tv twitch.tv m.twitch.tv", Family::None, R"(^/videos/\d+.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"telegram-private", X, "t.me www.t.me telegram.me", Family::None, R"(^/(?:c|joinchat)/.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"telegram-private", X, "t.me www.t.me telegram.me", Family::None, R"(^/\+.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},
    {"telegram-service", X, "t.me www.t.me telegram.me", Family::None, R"(^/([^/]+)(?:/.*)?$)", nullptr, 0, 0, 1, Kind::None, 0, false, Check::TelegramService, true},
    {"discord-channels", X, "discord.com www.discord.com discordapp.com", Family::None, R"(^/channels/.*$)", nullptr, 0, 0, 0, Kind::None, 0, false, Check::None, true},

    // ---- MediaPage: the media itself ----
    {"klipy", M, "klipy.com www.klipy.com", Family::None, R"(^/(clips)/([a-z0-9][a-z0-9-]{0,127})/?$)", nullptr, 2, 0, 1, Kind::Video, 2, true, Check::None, true},
    {"klipy", M, "klipy.com www.klipy.com", Family::None, R"(^/(gifs|stickers|static-memes|emojis|ai-gifs)/([a-z0-9][a-z0-9-]{0,127})/?$)", nullptr, 2, 0, 1, Kind::Gif, 2, true, Check::None, true},
    {"tenor", M, "tenor.com www.tenor.com", Family::None, R"(^/(?:[a-z]{2}(?:-[A-Za-z]{2})?/)?view/[^/]*-(\d{1,20})/?$)", "view", 1, 0, 0, Kind::Loop, 1, false, Check::TenorId, true},
    {"giphy", M, "giphy.com www.giphy.com", Family::None, R"(^/(gifs|embed)/(?:[^/]*-)?([A-Za-z0-9]{6,40})/?(?:/(?:html5|tile|fullscreen))?$)", nullptr, 2, 0, 1, Kind::Loop, 1, false, Check::None, true},
    {"giphy", M, "giphy.com www.giphy.com", Family::None, R"(^/(stickers)/(?:[^/]*-)?([A-Za-z0-9]{6,40})/?(?:/(?:html5|tile|fullscreen))?$)", nullptr, 2, 0, 1, Kind::Gif, 1, false, Check::None, true},
    {"giphy", M, "giphy.com www.giphy.com", Family::None, R"(^/(clips)/(?:[^/]*-)?([A-Za-z0-9]{6,40})/?(?:/(?:html5|tile|fullscreen))?$)", nullptr, 2, 0, 1, Kind::Video, 1, false, Check::None, true},
    {"giphy", M, "gph.is", Family::None, R"(^/(?:g/)?[A-Za-z0-9]{4,16}/?$)", "short", 0, 0, 0, Kind::Loop, 2, false, Check::None, true},
    {"imgur", M, "i.imgur.com", Family::None, R"(^/([A-Za-z0-9]{5,8})\.gifv$)", "gifv", 1, 0, 0, Kind::Loop, 1, false, Check::None, true},
    {"imgur", M, "imgur.com www.imgur.com m.imgur.com", Family::None, R"(^/([A-Za-z0-9]{5}|[A-Za-z0-9]{7})/?$)", "post", 1, 0, 0, Kind::Gif, 1, false, Check::ImgurPost, true},
    {"imgur", M, "imgur.com www.imgur.com m.imgur.com", Family::None, R"(^/(?:a|gallery|t/[^/]{1,64})/(?:[^/]*-)?([A-Za-z0-9]{5,8})/?$)", "album", 1, 0, 0, Kind::Gif, 2, true, Check::None, true},
    {"reddit-video", M, "v.redd.it", Family::None, R"(^/([a-z0-9]{10,16})/?$)", "video", 1, 0, 0, Kind::Loop, 2, false, Check::None, true},
    // "/sent/" (with sender / invite_code parameters) is where the Pinterest app's pin.it share links end; the plan
    // rebuilds the pin's own address from the id, so those parameters are never sent on.
    {"pinterest", M, "", Family::Pinterest, R"(^/pin/(?:[^/]*--)?(\d{5,25})(?:/sent)?/?$)", "pin", 1, 0, 0, Kind::Image, 2, false, Check::None, true},
    {"pinterest", M, "pin.it", Family::None, R"(^/[A-Za-z0-9]{5,16}/?$)", "short", 0, 0, 0, Kind::Image, 3, false, Check::None, true},
    {"streamable", M, "streamable.com www.streamable.com", Family::None, R"(^/(?:e/|o/|s/)?([a-z0-9]{5,8})/?$)", "video", 1, 0, 0, Kind::Video, 1, false, Check::StreamableId, true},
    {"imgflip", M, "imgflip.com www.imgflip.com", Family::None, R"(^/gif/([a-z0-9]{5,8})/?$)", "gif", 1, 0, 0, Kind::Loop, 1, false, Check::None, true},
    {"imgflip", M, "imgflip.com www.imgflip.com", Family::None, R"(^/i/([a-z0-9]{5,8})/?$)", "i", 1, 0, 0, Kind::Image, 1, false, Check::None, true},
    {"9gag", M, "9gag.com www.9gag.com m.9gag.com", Family::None, R"(^/gag/([A-Za-z0-9]{7})/?$)", "gag", 1, 0, 0, Kind::Video, 1, false, Check::None, true},
    {"coub", M, "coub.com www.coub.com", Family::None, R"(^/view/([a-z0-9]{4,12})/?$)", "view", 1, 0, 0, Kind::Video, 1, false, Check::None, true},
    {"gifer", M, "gifer.com www.gifer.com", Family::None, R"(^/[a-z]{2}/([A-Za-z0-9]{3,12})/?$)", "gif", 1, 0, 0, Kind::Loop, 1, false, Check::None, true},
    {"makeagif", M, "makeagif.com www.makeagif.com", Family::None, R"(^/gif/([A-Za-z0-9_-]{2,128})/?$)", "gif", 1, 0, 0, Kind::Loop, 2, true, Check::None, true},
    {"gyazo", M, "gyazo.com www.gyazo.com", Family::None, R"(^/([0-9a-f]{32})/?$)", "capture", 1, 0, 0, Kind::Image, 2, false, Check::None, true},
    {"lightshot", M, "prnt.sc www.prnt.sc prntscr.com", Family::None, R"(^/([A-Za-z0-9]{6,12})/?$)", "capture", 1, 0, 0, Kind::Image, 2, true, Check::None, true},
    {"imgbb", M, "ibb.co", Family::None, R"(^/([A-Za-z0-9]{6,10})/?$)", "image", 1, 0, 0, Kind::Image, 2, true, Check::None, true},
    {"postimages", M, "postimg.cc www.postimg.cc", Family::None, R"(^/(?:image/)?([A-Za-z0-9]{6,10})/?$)", "image", 1, 0, 0, Kind::Image, 2, true, Check::None, true},
    {"pixeldrain", M, "pixeldrain.com www.pixeldrain.com", Family::None, R"(^/u/([A-Za-z0-9]{6,12})/?$)", "file", 1, 0, 0, Kind::Image, 2, false, Check::None, true},
    {"tumblr", M, "", Family::Tumblr, R"(^/post/(\d{6,25})(?:/.*)?$)", "post", 1, 0, 0, Kind::Image, 2, true, Check::None, true},
    {"tumblr", M, "www.tumblr.com tumblr.com", Family::None, R"(^/([a-z0-9-]{1,32})/(\d{6,25})(?:/.*)?$)", "post", 2, 0, 0, Kind::Image, 2, true, Check::None, true},

    // ---- Site: rich cards ----
    {"vimeo", S, "vimeo.com www.vimeo.com", Family::None, R"(^/(\d{4,12})(?:/([0-9a-f]{6,20}))?/?$)", "video", 1, 2, 0, Kind::Card, 2, false, Check::None, true},
    {"vimeo", S, "vimeo.com www.vimeo.com", Family::None, R"(^/channels/[^/]{1,64}/(\d{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"vimeo", S, "vimeo.com www.vimeo.com", Family::None, R"(^/groups/[^/]{1,64}/videos/(\d{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"vimeo", S, "vimeo.com www.vimeo.com", Family::None, R"(^/album/\d{1,12}/video/(\d{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"vimeo", S, "player.vimeo.com", Family::None, R"(^/video/(\d{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"soundcloud", S, "soundcloud.com www.soundcloud.com m.soundcloud.com", Family::None, R"(^/([A-Za-z0-9_-]{2,64})/sets/([A-Za-z0-9_-]{1,128})/?$)", "set", 1, 2, 0, Kind::Card, 2, false, Check::SoundCloudProfile, true},
    {"soundcloud", S, "soundcloud.com www.soundcloud.com m.soundcloud.com", Family::None, R"(^/([A-Za-z0-9_-]{2,64})/([A-Za-z0-9_-]{1,128})/?$)", "track", 1, 2, 0, Kind::Card, 2, false, Check::SoundCloudTrack, true},
    {"soundcloud", S, "soundcloud.com www.soundcloud.com m.soundcloud.com", Family::None, R"(^/([A-Za-z0-9_-]{2,64})/?$)", "profile", 1, 0, 0, Kind::Card, 2, false, Check::SoundCloudProfile, true},
    {"soundcloud", S, "on.soundcloud.com", Family::None, R"(^/[A-Za-z0-9]{4,16}/?$)", "short", 0, 0, 0, Kind::Card, 3, false, Check::None, true},
    {"spotify", S, "open.spotify.com", Family::None, R"(^/(?:intl-[a-z]{2}(?:-[a-z]{2})?/)?(track|album|playlist|artist|show|episode|audiobook)/([A-Za-z0-9]{22})/?$)", nullptr, 2, 0, 1, Kind::Card, 2, true, Check::None, true},
    {"applemusic", S, "music.apple.com", Family::None, R"(^/[a-z]{2}/(album|song|artist|playlist|music-video|station)/[^/]{1,200}/((?:pl\.)?[A-Za-z0-9.-]{3,64})/?$)", nullptr, 2, 0, 1, Kind::Card, 2, false, Check::None, true},
    // Switched off without requests (kOffWithoutRequest): cdn-images.dzcdn.net answers 403 to every client.
    {"deezer", S, "www.deezer.com deezer.com", Family::None, R"(^/(?:[a-z]{2}/)?(track|album|playlist|artist|episode|show)/(\d{1,15})/?$)", nullptr, 2, 0, 1, Kind::Card, 2, false, Check::None, false},
    {"deezer", S, "link.deezer.com", Family::None, R"(^/s/[A-Za-z0-9]{6,40}/?$)", "short", 0, 0, 0, Kind::Card, 3, false, Check::None, false},
    {"dailymotion", S, "www.dailymotion.com dailymotion.com", Family::None, R"(^/video/(x[0-9a-z]{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"dailymotion", S, "dai.ly", Family::None, R"(^/(x[0-9a-z]{4,12})/?$)", "video", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"medal", S, "medal.tv www.medal.tv", Family::None, R"(^/(?:[a-z]{2}/)?games/[^/]{1,64}/clips/([A-Za-z0-9_-]{8,24})(?:/[^/]{0,200})?/?$)", "clip", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"medal", S, "medal.tv www.medal.tv", Family::None, R"(^/clips/([A-Za-z0-9_-]{8,24})/?$)", "clip", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"twitch", S, "clips.twitch.tv", Family::None, R"(^/([A-Za-z0-9_-]{8,100})/?$)", "clip", 1, 0, 0, Kind::Card, 2, true, Check::TwitchClipId, true},
    {"twitch", S, "www.twitch.tv twitch.tv m.twitch.tv", Family::None, R"(^/([A-Za-z0-9_]{3,25})/clip/([A-Za-z0-9_-]{8,100})/?$)", "clip", 2, 0, 0, Kind::Card, 2, true, Check::TwitchClipId, true},
    {"twitch", S, "www.twitch.tv twitch.tv m.twitch.tv", Family::None, R"(^/clip/([A-Za-z0-9_-]{8,100})/?$)", "clip", 1, 0, 0, Kind::Card, 2, true, Check::TwitchClipId, true},
    {"twitch", S, "www.twitch.tv twitch.tv m.twitch.tv", Family::None, R"(^/([A-Za-z0-9_]{3,25})/?$)", "channel", 1, 0, 0, Kind::Card, 2, true, Check::TwitchChannel, true},
    {"kick", S, "kick.com www.kick.com", Family::None, R"(^/([A-Za-z0-9_-]{2,25})/clips/(clip_[0-9A-Z]{26})/?$)", "clip", 1, 2, 0, Kind::Card, 2, true, Check::None, true},
    {"kick", S, "kick.com www.kick.com", Family::None, R"(^/([A-Za-z0-9_-]{2,25})/?$)", "channel", 1, 0, 0, Kind::Card, 2, true, Check::KickChannel, true},
    {"bluesky", S, "bsky.app www.bsky.app", Family::None, R"(^/profile/([A-Za-z0-9._:-]{3,253})/post/([a-z0-9]{10,16})/?$)", "post", 1, 2, 0, Kind::Card, 2, true, Check::None, true},
    {"x", S, "x.com www.x.com twitter.com www.twitter.com mobile.twitter.com mobile.x.com", Family::None, R"(^/([A-Za-z0-9_]{1,15})/status(?:es)?/(\d{1,20})(?:/(?:photo|video)/\d)?/?$)", "status", 1, 2, 0, Kind::Card, 2, true, Check::None, true},
    {"github", S, "github.com www.github.com", Family::None, R"(^/([A-Za-z0-9-]{1,39})(?:/[^/]{1,100}){0,6}/?$)", "page", 1, 0, 0, Kind::Card, 2, true, Check::GitHub, true},
    {"steam", S, "store.steampowered.com", Family::None, R"(^/app/(\d{1,10})(?:/[^/]{0,200})?/?$)", "app", 1, 0, 0, Kind::Card, 2, true, Check::None, true},
    {"steam", S, "steamcommunity.com", Family::None, R"(^/sharedfiles/filedetails/?$)", "file", 0, 0, 0, Kind::Card, 2, true, Check::SteamFile, true},
    {"steam", S, "steamcommunity.com", Family::None, R"(^/(?:id|profiles)/([^/]{1,64})/?$)", "page", 1, 0, 0, Kind::Card, 2, true, Check::None, true},
    {"steam", S, "steamcommunity.com", Family::None, R"(^/app/(\d{1,10})/?$)", "page", 1, 0, 0, Kind::Card, 2, true, Check::None, true},
    {"steam", S, "s.team", Family::None, R"(^/a/(\d{1,10})/?$)", "short", 1, 0, 0, Kind::Card, 3, false, Check::None, true},
    {"wikipedia", S, "", Family::Wikipedia, R"(^/wiki/(.{1,256})$)", nullptr, 1, 0, 0, Kind::Card, 2, false, Check::WikiTitle, true},
    {"wikipedia", S, "w.wiki", Family::None, R"(^/[A-Za-z0-9~_-]{1,16}$)", "short", 0, 0, 0, Kind::Card, 3, false, Check::None, true},
    {"telegram", S, "t.me www.t.me telegram.me", Family::None, R"(^/(?:s/)?([A-Za-z][A-Za-z0-9_]{3,31})/(\d{1,10})/?$)", "post", 1, 2, 0, Kind::Card, 2, true, Check::None, true},
    {"telegram", S, "t.me www.t.me telegram.me", Family::None, R"(^/([A-Za-z][A-Za-z0-9_]{3,31})/?$)", "channel", 1, 0, 0, Kind::Card, 2, true, Check::None, true},
    {"discord", S, "discord.gg", Family::None, R"(^/([A-Za-z0-9-]{2,32})/?$)", "invite", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"discord", S, "discord.com www.discord.com discordapp.com", Family::None, R"(^/invite/([A-Za-z0-9-]{2,32})/?$)", "invite", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"reddit", S, "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com", Family::None, R"(^/r/([A-Za-z0-9_]{2,21})/comments/([a-z0-9]{4,10})(?:/.*)?$)", "post", 2, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"reddit", S, "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com", Family::None, R"(^/(?:u|user)/[A-Za-z0-9_-]{3,20}/comments/([a-z0-9]{4,10})(?:/.*)?$)", "post", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"reddit", S, "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com", Family::None, R"(^/comments/([a-z0-9]{4,10})(?:/.*)?$)", "post", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"reddit", S, "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com", Family::None, R"(^/gallery/([a-z0-9]{4,10})/?$)", "post", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"reddit", S, "redd.it", Family::None, R"(^/([a-z0-9]{4,10})/?$)", "post", 1, 0, 0, Kind::Card, 2, false, Check::None, true},
    {"youtube-extra", S, "www.youtube.com youtube.com m.youtube.com", Family::None, R"(^/(live|embed)/([A-Za-z0-9_-]{11})/?$)", nullptr, 2, 0, 1, Kind::Card, 2, false, Check::None, true},
    {"youtube-extra", S, "www.youtube.com youtube.com m.youtube.com", Family::None, R"(^/(clip)/([A-Za-z0-9_-]{20,64})/?$)", nullptr, 2, 0, 1, Kind::Card, 2, false, Check::None, true},
};

constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

bool isOneOf(const QString& value, const char* const* list)
{
    for (const char* const* item = list; *item; ++item) {
        if (value.compare(QLatin1String(*item), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

const char* const kSoundCloudReserved[] = {"discover", "search", "upload", "you", "stream", "charts", "pages", "terms-of-use", "settings", "messages",
                                           "notifications", "mobile", "pro", "jobs", "imprint", "people", "tags", "popular", "go", "artists", "signin",
                                           "signup", "logout", nullptr};
const char* const kSoundCloudTabs[] = {"sets", "tracks", "albums", "reposts", "likes", "followers", "following", "comments", "popular-tracks", "spotlight", nullptr};
const char* const kTwitchReserved[] = {"directory", "downloads", "jobs", "p", "settings", "subscriptions", "turbo", "prime", "store", "search", "friends",
                                       "inventory", "wallet", "messages", "videos", "login", "signup", "broadcast", "drops", "following", "moderator",
                                       "popout", "team", "u", "bits", "creatorcamp", "legal", "privacy", "security", "safety", "partners", "about",
                                       "communities", "embed", "clip", nullptr};
const char* const kKickReserved[] = {"categories", "browse", "following", "search", "dashboard", "settings", "terms", "privacy", "help", "signup", "login",
                                     "community-guidelines", nullptr};
const char* const kGitHubReserved[] = {"login", "join", "settings", "notifications", "marketplace", "explore", "topics", "trending", "sponsors", "features",
                                       "pricing", "about", "enterprise", "team", "security", "customer-stories", "readme", "orgs", "organizations", "apps",
                                       "codespaces", "new", "search", "collections", "events", "site", "contact", "pulls", "issues", "dashboard", "logout",
                                       "sessions", "signup", "password_reset", nullptr};
const char* const kGitHubDownloads[] = {"archive", "raw", "zipball", "tarball", "suites", "attachments", nullptr};
const char* const kTelegramService[] = {"login", "share", "proxy", "socks", "addstickers", "addemoji", "addtheme", "iv", "setlanguage", "confirmphone", "bg", nullptr};
const char* const kTumblrReserved[] = {"www", "api", "assets", "media", "static", "help", "staff", nullptr};

enum class Verdict { Pass, Fail, Refuse }; // Fail: the next row may match; Refuse: no preview at all

QStringList segmentsOf(const QString& decodedPath)
{
    return decodedPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

Verdict runCheck(const Row& row, const QRegularExpressionMatch& m, const QUrl& url, const QString& path, Match* out)
{
    switch (row.check) {
    case Check::None:
        return Verdict::Pass;
    case Check::TenorId: {
        bool          ok = false;
        const quint64 id = m.captured(1).toULongLong(&ok);
        if (!ok || id == 0)
            return Verdict::Fail;
        out->expectedRequests = id > 0xFFFFFFFFULL ? 1 : 2;
        return Verdict::Pass;
    }
    case Check::ImgurPost: {
        const QString id = m.captured(1);
        for (const QChar ch : id) {
            if (ch.isDigit() || ch.isUpper())
                return Verdict::Pass;
        }
        return Verdict::Fail;
    }
    case Check::StreamableId: {
        for (const QChar ch : m.captured(1)) {
            if (ch.isDigit())
                return Verdict::Pass;
        }
        return Verdict::Fail;
    }
    case Check::SoundCloudProfile:
        return isOneOf(m.captured(1), kSoundCloudReserved) ? Verdict::Fail : Verdict::Pass;
    case Check::SoundCloudTrack:
        return isOneOf(m.captured(1), kSoundCloudReserved) || isOneOf(m.captured(2), kSoundCloudTabs) ? Verdict::Fail : Verdict::Pass;
    case Check::TwitchClipId: {
        const QString id = m.captured(m.lastCapturedIndex());
        return id.compare(QLatin1String("embed"), Qt::CaseInsensitive) == 0 || id.compare(QLatin1String("create"), Qt::CaseInsensitive) == 0
                   ? Verdict::Fail
                   : Verdict::Pass;
    }
    case Check::TwitchChannel:
        return isOneOf(m.captured(1), kTwitchReserved) ? Verdict::Fail : Verdict::Pass;
    case Check::KickChannel:
        return isOneOf(m.captured(1), kKickReserved) ? Verdict::Fail : Verdict::Pass;
    case Check::GitHub: {
        const QStringList segments = segmentsOf(path);
        if (segments.isEmpty() || isOneOf(segments.first(), kGitHubReserved))
            return Verdict::Fail;
        if (segments.size() >= 3) {
            // Downloads (a file, not a page): no request at all.
            if (isOneOf(segments.at(2), kGitHubDownloads))
                return Verdict::Refuse;
            if (segments.size() >= 4 && segments.at(2).compare(QLatin1String("releases"), Qt::CaseInsensitive) == 0
                && segments.at(3).compare(QLatin1String("download"), Qt::CaseInsensitive) == 0)
                return Verdict::Refuse;
        }
        return Verdict::Pass;
    }
    case Check::SteamFile: {
        static const QRegularExpression kId(QStringLiteral("^\\d{1,12}$"));
        const QUrlQuery query(url);
        const QString   id = query.queryItemValue(latin("id"), QUrl::FullyDecoded);
        if (!kId.match(id).hasMatch())
            return Verdict::Fail;
        out->itemId = id;
        return Verdict::Pass;
    }
    case Check::WikiTitle: {
        const QString title = m.captured(1);
        // Special pages (random articles, log-ins, searches) and "." / "..": no request at all.
        if (title.startsWith(QLatin1String("Special:"), Qt::CaseInsensitive) || title == QLatin1String(".") || title == QLatin1String(".."))
            return Verdict::Refuse;
        return Verdict::Pass;
    }
    case Check::TelegramService:
        return isOneOf(m.captured(1), kTelegramService) ? Verdict::Pass : Verdict::Fail;
    }
    return Verdict::Fail;
}

struct Compiled {
    std::vector<QRegularExpression> paths;
    QHash<QString, QVector<int>>    byHost; // exact hosts -> row indices in table order
    QVector<int>                    pinterest, wikipedia, tumblr, amazon, googleMaps, googleWww;
};

const Compiled& compiled()
{
    static const Compiled* c = [] {
        auto* out = new Compiled; // never deleted: plain data, nothing of it runs at unload
        out->paths.reserve(kRowCount);
        for (int i = 0; i < kRowCount; ++i) {
            const Row& row = kRows[i];
            out->paths.emplace_back(QString::fromLatin1(row.path));
            out->paths.back().optimize();
            const QStringList hosts = QString::fromLatin1(row.hosts).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString& host : hosts)
                out->byHost[host].append(i);
            switch (row.family) {
            case Family::Pinterest:
                out->pinterest.append(i);
                break;
            case Family::Wikipedia:
                out->wikipedia.append(i);
                break;
            case Family::Tumblr:
                out->tumblr.append(i);
                break;
            case Family::Amazon:
                out->amazon.append(i);
                break;
            case Family::GoogleMaps:
                out->googleMaps.append(i);
                break;
            case Family::GoogleWww:
                out->googleWww.append(i);
                break;
            case Family::None:
                break;
            }
        }
        return out;
    }();
    return *c;
}

bool isPinterestHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral(
        "^(?:www\\.|[a-z]{2}\\.)?pinterest\\.(?:com|ca|co\\.uk|de|fr|es|it|at|ch|nl|se|dk|pt|ie|nz|com\\.au|com\\.mx|cl|ph|jp|co\\.kr|ru)$"));
    return host.contains(QLatin1String("pinterest.")) && re.match(host).hasMatch();
}

bool isWikipediaHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral("^([a-z]{2,3}(?:-[a-z]{2,8}){0,2}|simple)(?:\\.m)?\\.wikipedia\\.org$"));
    return host.endsWith(QLatin1String(".wikipedia.org")) && re.match(host).hasMatch();
}

bool isTumblrBlogHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral("^([a-z0-9-]{1,32})\\.tumblr\\.com$"));
    if (!host.endsWith(QLatin1String(".tumblr.com")))
        return false;
    const QRegularExpressionMatch m = re.match(host);
    return m.hasMatch() && !isOneOf(m.captured(1), kTumblrReserved);
}

bool isAmazonHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral(
        "^(?:www\\.|smile\\.)?amazon\\.(?:com|de|co\\.uk|fr|it|es|ca|co\\.jp|com\\.au|in|com\\.br|com\\.mx|nl|se|pl|com\\.tr|ae|sa|sg|eg)$"));
    return host.contains(QLatin1String("amazon.")) && re.match(host).hasMatch();
}

bool isGoogleMapsHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral("^maps\\.google\\.[a-z.]{2,6}$"));
    return host.startsWith(QLatin1String("maps.google.")) && re.match(host).hasMatch();
}

bool isGoogleWwwHost(const QString& host)
{
    static const QRegularExpression re(QStringLiteral("^(?:www\\.)?google\\.[a-z.]{2,6}$"));
    return host.contains(QLatin1String("google.")) && re.match(host).hasMatch();
}

QString joined(const QRegularExpressionMatch& m, int first, int second)
{
    QString out = first > 0 ? m.captured(first) : QString();
    if (second > 0 && !m.captured(second).isEmpty())
        out += QLatin1Char('/') + m.captured(second);
    return out;
}

// ---- brands ---------------------------------------------------------------------------------------------------

struct BrandRow {
    const char* id;
    const char* name;
    QRgb        light, dark;
};

const BrandRow kBrands[] = {
    {"vimeo", "Vimeo", 0xFF1AB7EA, 0xFF1AB7EA},
    {"soundcloud", "SoundCloud", 0xFFFF5500, 0xFFFF7A33},
    {"spotify", "Spotify", 0xFF1DB954, 0xFF1ED760},
    {"applemusic", "Apple Music", 0xFFFA243C, 0xFFFC5C6C},
    {"deezer", "Deezer", 0xFFA238FF, 0xFFB565FF},
    {"dailymotion", "Dailymotion", 0xFF0066DC, 0xFF4C9BFF},
    {"medal", "Medal", 0xFFF5B400, 0xFFFFC933},
    {"twitch", "Twitch", 0xFF9146FF, 0xFFA970FF},
    {"kick", "Kick", 0xFF53FC18, 0xFF53FC18},
    {"bluesky", "Bluesky", 0xFF1185FE, 0xFF4DA3FF},
    {"x", "X", 0xFF0F1419, 0xFFE7E9EA},
    {"github", "GitHub", 0xFF24292F, 0xFFF0F6FC},
    {"steam", "Steam", 0xFF1B2838, 0xFF66C0F4},
    {"wikipedia", "Wikipedia", 0xFF3366CC, 0xFF6B9BFF},
    {"telegram", "Telegram", 0xFF26A5E4, 0xFF4CB8EC},
    {"discord", "Discord", 0xFF5865F2, 0xFF7984F5},
    {"reddit", "Reddit", 0xFFFF4500, 0xFFFF5722},
    {"youtube-extra", "YouTube", 0xFFFF0000, 0xFFFF4E45},
    {"tumblr", "Tumblr", 0xFF001935, 0xFF8A9BB8},
};

// ---- hop lists ------------------------------------------------------------------------------------------------

QMutex      g_inactiveMutex;
QStringList g_inactive; // tests only
QStringList g_active;   // tests only

bool switchedOff(const char* id)
{
    QMutexLocker lock(&g_inactiveMutex);
    return !g_inactive.isEmpty() && g_inactive.contains(QString::fromLatin1(id));
}

bool switchedOnForTests(const char* id)
{
    QMutexLocker lock(&g_inactiveMutex);
    return !g_active.isEmpty() && g_active.contains(QString::fromLatin1(id));
}

struct HopRow {
    const char* id;
    const char* page;  // entries separated by spaces: "host", "*.suffix", "^regex$", "@family"; "-": none
    const char* media;
};

const HopRow kHops[] = {
    {"klipy", "klipy.com www.klipy.com api.klipy.com", R"(^static[0-9]*\.klipy\.com$)"}, // api.: the oEmbed of a walled GIF page
    {"tenor", "tenor.com www.tenor.com", "media.tenor.com media1.tenor.com c.tenor.com"},
    {"tenor-short", "tenor.com www.tenor.com", "media.tenor.com media1.tenor.com c.tenor.com"},
    {"giphy", "giphy.com www.giphy.com gph.is", R"(i.giphy.com media.giphy.com ^media[0-9]\.giphy\.com$)"},
    {"imgur", "imgur.com www.imgur.com m.imgur.com", "i.imgur.com"},
    {"reddit-video", "v.redd.it", "v.redd.it"},
    {"pinterest", "@pinterest www.pinterest.com api.pinterest.com pin.it", "i.pinimg.com"},
    {"streamable", "-", "api-f.streamable.com *.streamable.com"},
    {"imgflip", "imgflip.com www.imgflip.com", "i.imgflip.com"},
    {"9gag", "-", "img-9gag-fun.9cache.com *.9cache.com"},
    {"coub", "-", "tg.coub.com *.coub.com"},
    {"gifer", "-", "i.gifer.com"},
    {"makeagif", "makeagif.com www.makeagif.com", "i.makeagif.com"},
    {"gyazo", "api.gyazo.com", "i.gyazo.com"},
    {"lightshot", "prnt.sc www.prnt.sc prntscr.com", "*.lightshot.app *.prntscr.com *.prnt.sc i.imgur.com"},
    {"imgbb", "ibb.co", "i.ibb.co"},
    {"postimages", "postimg.cc www.postimg.cc", "i.postimg.cc"},
    {"pixeldrain", "pixeldrain.com www.pixeldrain.com", "pixeldrain.com"},
    {"tumblr", "@tumblr www.tumblr.com tumblr.com", "va.media.tumblr.com *.media.tumblr.com"},
    {"vimeo", "vimeo.com www.vimeo.com player.vimeo.com", "i.vimeocdn.com"},
    {"soundcloud", "soundcloud.com www.soundcloud.com m.soundcloud.com on.soundcloud.com", R"(^i[0-9]\.sndcdn\.com$)"},
    {"spotify", "open.spotify.com", R"(i.scdn.co mosaic.scdn.co ^image-cdn-[a-z0-9-]+\.spotifycdn\.com$)"},
    {"applemusic", "music.apple.com", R"(^is[0-9]-ssl\.mzstatic\.com$)"},
    {"deezer", "api.deezer.com www.deezer.com deezer.com link.deezer.com", "cdn-images.dzcdn.net e-cdns-images.dzcdn.net"},
    {"dailymotion", "www.dailymotion.com", "*.dmcdn.net"},
    {"medal", "medal.tv www.medal.tv", "cdn.medal.tv *.medal.tv"},
    {"twitch", "clips.twitch.tv www.twitch.tv twitch.tv m.twitch.tv", "static-cdn.jtvnw.net *.jtvnw.net clips-media-assets2.twitch.tv"},
    {"kick", "kick.com www.kick.com", "files.kick.com clips.kick.com images.kick.com"},
    {"bluesky", "bsky.app www.bsky.app", "cdn.bsky.app video.bsky.app"},
    {"x", "x.com www.x.com mobile.x.com twitter.com www.twitter.com mobile.twitter.com", "pbs.twimg.com abs.twimg.com"},
    {"github", "github.com www.github.com", "opengraph.githubassets.com avatars.githubusercontent.com repository-images.githubusercontent.com"},
    {"steam", "store.steampowered.com steamcommunity.com s.team", "*.steamstatic.com steamcdn-a.akamaihd.net images.steamusercontent.com steamuserimages-a.akamaihd.net"},
    {"wikipedia", "@wikipedia w.wiki", "upload.wikimedia.org thumb.wikimedia.org"}, // smoke 2026-11-09: REST thumbnails moved to thumb.
    {"telegram", "t.me www.t.me telegram.me", R"(^cdn[0-9]+\.telesco\.pe$ telegram.org)"},
    {"discord", "discord.com www.discord.com discord.gg discordapp.com", "cdn.discordapp.com"},
    {"reddit", "www.reddit.com reddit.com old.reddit.com new.reddit.com np.reddit.com m.reddit.com redd.it",
     "i.redd.it v.redd.it preview.redd.it external-preview.redd.it"},
    {"youtube-extra", "www.youtube.com", "i.ytimg.com"},
    {"dropbox", "-", "www.dropbox.com *.dropboxusercontent.com"},
    // "=": the share's own host, bound at run time (the share plan passes it: pageHopAllowed(id, url, boundHost)).
    {"nextcloud", "=", "="},
};

struct HopList {
    QStringList                 exact, suffixes, families;
    QVector<QRegularExpression> patterns;
    bool                        none  = false;
    bool                        bound = false; // "=": the plan's own host only, bound at run time
};

HopList parseHops(const char* text)
{
    HopList list;
    const QString all = QString::fromLatin1(text).trimmed();
    if (all == QLatin1String("-")) {
        list.none = true;
        return list;
    }
    if (all == QLatin1String("=")) {
        list.bound = true;
        return list;
    }
    for (const QString& entry : all.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (entry.startsWith(QLatin1String("*.")))
            list.suffixes << entry.mid(1); // ".x"
        else if (entry.startsWith(QLatin1Char('^')))
            list.patterns << QRegularExpression(entry);
        else if (entry.startsWith(QLatin1Char('@')))
            list.families << entry.mid(1);
        else
            list.exact << entry;
    }
    return list;
}

struct HopTables {
    QHash<QString, HopList> page, media;
};

const HopTables& hopTables()
{
    static const HopTables* t = [] {
        auto* out = new HopTables;
        for (const HopRow& row : kHops) {
            out->page.insert(QString::fromLatin1(row.id), parseHops(row.page));
            out->media.insert(QString::fromLatin1(row.id), parseHops(row.media));
        }
        return out;
    }();
    return *t;
}

bool listAllows(const HopList& list, const QString& host)
{
    if (list.none || list.bound || host.isEmpty())
        return false;
    if (list.exact.contains(host))
        return true;
    for (const QString& suffix : list.suffixes) {
        if (host.endsWith(suffix))
            return true;
    }
    for (const QRegularExpression& re : list.patterns) {
        if (re.match(host).hasMatch())
            return true;
    }
    for (const QString& family : list.families) {
        if ((family == QLatin1String("pinterest") && isPinterestHost(host)) || (family == QLatin1String("wikipedia") && isWikipediaHost(host))
            || (family == QLatin1String("tumblr") && isTumblrBlogHost(host)))
            return true;
    }
    return false;
}

bool hopAllowed(const QHash<QString, HopList>& table, const QString& id, const QUrl& url, const QString& boundHost = QString())
{
    if (id.isEmpty() || id == QLatin1String("media-file"))
        return true; // the Web class and media-file links: any allowed public host
    const auto it = table.constFind(id);
    if (it == table.constEnd())
        return false;
    const QString host = publicHostName(url);
    if (it->bound)
        return !boundHost.isEmpty() && !host.isEmpty() && host == boundHost;
    return listAllows(*it, host);
}

// The shape rows (2.2): Nextcloud and ownCloud 10 public shares on any host no provider owns. Release gate as a row's.
constexpr bool kShareRowsActive = true;

bool shareRowsOn()
{
    return (kShareRowsActive || switchedOnForTests("nextcloud")) && !switchedOff("nextcloud");
}

// Whether match() uses the row (the release gate, D39).
bool rowOn(const Row& row)
{
    return (row.active || switchedOnForTests(row.id)) && !switchedOff(row.id);
}

// Providers switched off WITHOUT requests: their rows' links get no preview at all (like an Exclude row) instead of
// becoming Web links, because the site answers every client in a way no preview can come from (a Web card would ask
// the same servers again). Their plans stay in fetcher.cpp: to switch one on again, set its rows' active flag and take
// it out of this list.
// Caps (design2 2.10 rules 2 and 4): these rows are NOT part of what "lx" means in the released 2.5.0 (a 2.5.0 sender
// makes no job and sends no LPXNO for them). Once 2.5.0 is published, a row of this list switched on again must go
// under a NEW cap name (and receivers wait for it only from senders that announce that cap); widening "lx" would make
// every newer receiver wait (kSenderQueueSec + 40 s) on these links from 2.5.0 senders that never answer them.
//  * deezer: cdn-images.dzcdn.net, where every Deezer cover is, answered 403 to every client in the live test
//    (2026-11-10), so no Deezer card had its picture. Switched off by the author's decision of 2026-11-11.
const char* const kOffWithoutRequest[] = {"deezer", nullptr};

bool offWithoutRequest(const char* id)
{
    for (const char* const* off = kOffWithoutRequest; *off; ++off) {
        if (qstrcmp(*off, id) == 0)
            return true;
    }
    return false;
}

QString firstSegmentOf(const QString& path)
{
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return segments.isEmpty() ? QString() : segments.first().toLower();
}

// (3) of match(): a share by its path shape (shares.h), only on a host no provider owns.
Match matchShape(const QUrl& url)
{
    Match none;
    if (!shareRowsOn())
        return none;
    const shares::ShareLink share = shares::parse(url);
    if (!share.isValid())
        return none;
    Match out;
    out.id               = latin("nextcloud");
    out.cls              = LinkClass::MediaPage;
    out.expected         = Kind::Image; // the strip's glyph; the load tells what it turned out to be
    out.itemId           = share.token;
    out.sub              = shares::subOf(share.form);
    out.expectedRequests = shares::expectedRequestsOf(share.form);
    return out;
}

} // namespace

// ---- matching ---------------------------------------------------------------------------------------------------

Match match(const QUrl& url, const QString& host)
{
    Match           none;
    const Compiled& c = compiled();
    QVector<int>    candidates = c.byHost.value(host);
    const auto      add        = [&candidates](const QVector<int>& rows) {
        candidates += rows;
        std::sort(candidates.begin(), candidates.end());
    };
    if (isPinterestHost(host))
        add(c.pinterest);
    if (isWikipediaHost(host))
        add(c.wikipedia);
    if (isTumblrBlogHost(host))
        add(c.tumblr);
    if (isAmazonHost(host))
        add(c.amazon);
    if (isGoogleMapsHost(host))
        add(c.googleMaps);
    if (isGoogleWwwHost(host))
        add(c.googleWww);
    // A host owned by a row or a family keeps its rules (t.me/s/..., reddit.com/r/x/s/..., link.deezer.com/s/...): no
    // shape row ever applies to it.
    if (candidates.isEmpty())
        return matchShape(url);

    const QString path = url.path(QUrl::FullyDecoded);
    for (const int index : qAsConst(candidates)) {
        const Row& row = kRows[index];
        if (!rowOn(row)) {
            // Switched off without requests: its links are no preview at all (never a Web card).
            if (row.active || !offWithoutRequest(row.id) || !c.paths[static_cast<size_t>(index)].match(path).hasMatch())
                continue;
            Match refused;
            refused.id = QString::fromLatin1(row.id);
            return refused;
        }
        const QRegularExpressionMatch m = c.paths[static_cast<size_t>(index)].match(path);
        if (!m.hasMatch())
            continue;
        Match out;
        out.id               = QString::fromLatin1(row.id);
        out.cls              = row.cls;
        out.expected         = row.expected;
        out.expectedRequests = row.requests;
        out.itemId           = joined(m, row.idGroup, row.id2Group);
        out.sub              = row.sub ? QString::fromLatin1(row.sub) : (row.subGroup > 0 ? m.captured(row.subGroup) : QString());
        const Verdict verdict = runCheck(row, m, url, path, &out);
        if (verdict == Verdict::Fail)
            continue;
        if (verdict == Verdict::Refuse || row.cls == LinkClass::None) {
            Match refused;
            refused.id = out.id;
            return refused; // no preview, no request
        }
        // A row that reads the link's own page: never a file download or an action link.
        if (row.pageStep && (hasNonHtmlExtension(url) || hasActionSegment(url))) {
            Match refused;
            refused.id = out.id;
            return refused;
        }
        if (row.family == Family::Wikipedia) {
            // The language (and the mobile form) of the host; the API is asked on the language's own host.
            QString lang = host.left(host.indexOf(QLatin1Char('.')));
            out.sub      = lang;
        }
        return out;
    }
    return none;
}

QString directRewriteOf(const QUrl& url, const QString& host)
{
    const QString path = url.path(QUrl::FullyDecoded);
    if (host == QLatin1String("tenor.com") || host == QLatin1String("www.tenor.com")) {
        static const QRegularExpression re(QStringLiteral("^/[A-Za-z0-9]{4,12}\\.gif$"));
        if (re.match(path).hasMatch())
            return latin("tenor-short");
    }
    if (host == QLatin1String("www.dropbox.com")) {
        static const QRegularExpression re(QStringLiteral("^/(?:scl/fi/[a-z0-9]{10,40}|s/[a-z0-9]{10,20})/[^/]+$"));
        if (re.match(path).hasMatch() && mediaKindOfName(path.mid(path.lastIndexOf(QLatin1Char('/')) + 1)) != Kind::None)
            return latin("dropbox");
    }
    return {};
}

// ---- brands, ids ------------------------------------------------------------------------------------------------

Brand brandOf(const QString& id)
{
    for (const BrandRow& row : kBrands) {
        if (id == QLatin1String(row.id))
            return Brand{row.name, row.light, row.dark};
    }
    return {};
}

QVector<QRgb> brandColours()
{
    QVector<QRgb> out;
    for (const BrandRow& row : kBrands) {
        if (!out.contains(row.light))
            out << row.light;
        if (!out.contains(row.dark))
            out << row.dark;
    }
    return out;
}

QStringList ids()
{
    QStringList out;
    for (const Row& row : kRows) {
        const QString id = QString::fromLatin1(row.id);
        if (!out.contains(id))
            out << id;
    }
    out << latin("nextcloud") << latin("tenor-short") << latin("dropbox") << latin("media-file");
    return out;
}

bool cardsFollowWebRules(LinkClass cls, const QString& provider)
{
    return cls == LinkClass::Web || (cls == LinkClass::MediaPage && provider == QLatin1String("nextcloud"));
}

// ---- hop lists --------------------------------------------------------------------------------------------------

bool pageHopAllowed(const QString& id, const QUrl& url)
{
    return hopAllowed(hopTables().page, id, url);
}

bool mediaHopAllowed(const QString& id, const QUrl& url)
{
    return hopAllowed(hopTables().media, id, url);
}

bool pageHopAllowed(const QString& id, const QUrl& url, const QString& boundHost)
{
    return hopAllowed(hopTables().page, id, url, boundHost);
}

bool mediaHopAllowed(const QString& id, const QUrl& url, const QString& boundHost)
{
    return hopAllowed(hopTables().media, id, url, boundHost);
}

bool hasHopLists(const QString& id)
{
    return hopTables().page.contains(id) && hopTables().media.contains(id);
}

// ---- HostGate overrides -----------------------------------------------------------------------------------------

int pageSpacingMs(const QString& spacingGroup)
{
    if (spacingGroup == QLatin1String("reddit"))
        return 6000; // Reddit allows clients without OAuth about 10 requests a minute
    return 1000;
}

int mediaSpacingMs(const QString& spacingGroup)
{
    if (spacingGroup == QLatin1String("githubassets.com"))
        return 1000; // it answered 429 after 4 quick requests
    return -1;
}

int mediaBurst(const QString& spacingGroup)
{
    if (spacingGroup == QLatin1String("githubassets.com"))
        return 3;
    return -1;
}

int minCooldownMs(const QString& domain)
{
    if (domain == QLatin1String("imgur.com"))
        return 600000; // Imgur answers 429 with "Retry-After: 0" and stays closed for minutes
    return 1000;
}

// ---- the first request of a load ---------------------------------------------------------------------------------

FirstRequest firstRequestOf(const Target& target)
{
    FirstRequest first;
    const auto   page = [&first](const QString& host, const QString& path) {
        first.host         = host;
        first.lane         = HostGate::Lane::Page;
        first.firstSegment = firstSegmentOf(path);
    };
    const auto media = [&first](const QString& host, const QString& path) {
        first.host         = host;
        first.lane         = HostGate::Lane::Media;
        first.firstSegment = firstSegmentOf(path);
    };
    const QString& id      = target.provider;
    const QString  path    = target.isNewClass() ? target.page.path(QUrl::FullyDecoded) : target.url.path(QUrl::FullyDecoded);
    const QString& sub     = target.sub;
    const QString& linkHost = target.host;

    switch (target.cls) {
    case LinkClass::None:
        return first;
    case LinkClass::YouTube:
        page(latin("www.youtube.com"), latin("/oembed"));
        return first;
    case LinkClass::Direct:
        if (id == QLatin1String("tenor-short"))
            page(linkHost, path);
        else
            media(linkHost, path);
        return first;
    case LinkClass::Web:
        page(linkHost, path);
        return first;
    case LinkClass::MediaPage:
    case LinkClass::Site:
        break;
    }

    if (id == QLatin1String("media-file")) {
        media(linkHost, path);
    } else if (id == QLatin1String("nextcloud")) {
        // The page and download forms read the share page first (base/s/<token>: its first segment is the link's);
        // the preview, publicpreview and DAV forms are one media request for the link itself.
        const shares::Form form = shares::parse(target.page).form;
        if (form == shares::Form::Page || form == shares::Form::Download)
            page(linkHost, path);
        else
            media(linkHost, path);
    } else if (id == QLatin1String("tenor")) {
        if (target.expectedRequests <= 1)
            media(latin("media.tenor.com"), latin("/"));
        else
            page(latin("tenor.com"), latin("/oembed"));
    } else if (id == QLatin1String("giphy")) {
        if (sub == QLatin1String("short"))
            page(linkHost, path);
        else if (sub == QLatin1String("clips"))
            media(latin("media.giphy.com"), latin("/media"));
        else
            media(latin("i.giphy.com"), latin("/"));
    } else if (id == QLatin1String("imgur")) {
        if (sub == QLatin1String("album"))
            page(linkHost, path);
        else
            media(latin("i.imgur.com"), latin("/"));
    } else if (id == QLatin1String("reddit-video")) {
        page(latin("v.redd.it"), QLatin1Char('/') + target.itemId);
    } else if (id == QLatin1String("pinterest")) {
        if (sub == QLatin1String("short"))
            page(linkHost, path);
        else
            page(latin("www.pinterest.com"), latin("/oembed.json"));
    } else if (id == QLatin1String("streamable")) {
        media(latin("api-f.streamable.com"), latin("/api"));
    } else if (id == QLatin1String("imgflip")) {
        media(latin("i.imgflip.com"), latin("/"));
    } else if (id == QLatin1String("9gag")) {
        media(latin("img-9gag-fun.9cache.com"), latin("/photo"));
    } else if (id == QLatin1String("coub")) {
        media(latin("tg.coub.com"), latin("/media"));
    } else if (id == QLatin1String("gifer")) {
        media(latin("i.gifer.com"), latin("/"));
    } else if (id == QLatin1String("gyazo")) {
        page(latin("api.gyazo.com"), latin("/api"));
    } else if (id == QLatin1String("pixeldrain")) {
        page(latin("pixeldrain.com"), latin("/api"));
    } else if (id == QLatin1String("vimeo")) {
        page(latin("vimeo.com"), latin("/api"));
    } else if (id == QLatin1String("soundcloud")) {
        if (sub == QLatin1String("short"))
            page(linkHost, path);
        else
            page(latin("soundcloud.com"), latin("/oembed"));
    } else if (id == QLatin1String("spotify")) {
        page(latin("open.spotify.com"), QLatin1Char('/') + sub);
    } else if (id == QLatin1String("applemusic")) {
        page(latin("music.apple.com"), latin("/api"));
    } else if (id == QLatin1String("deezer")) {
        if (sub == QLatin1String("short"))
            page(linkHost, path);
        else
            page(latin("api.deezer.com"), latin("/oembed"));
    } else if (id == QLatin1String("dailymotion")) {
        page(latin("www.dailymotion.com"), latin("/services"));
    } else if (id == QLatin1String("medal")) {
        page(latin("medal.tv"), latin("/api"));
    } else if (id == QLatin1String("twitch")) {
        if (sub == QLatin1String("clip"))
            page(latin("clips.twitch.tv"), QLatin1Char('/') + target.itemId);
        else
            page(latin("www.twitch.tv"), QLatin1Char('/') + target.itemId);
    } else if (id == QLatin1String("x")) {
        page(latin("x.com"), path);
    } else if (id == QLatin1String("steam")) {
        page(linkHost, path);
    } else if (id == QLatin1String("wikipedia")) {
        if (sub == QLatin1String("short")) {
            page(linkHost, path);
        } else {
            QString lang = sub;
            if (lang.endsWith(QLatin1String(".m")))
                lang.chop(2);
            page(lang + latin(".wikipedia.org"), latin("/api"));
        }
    } else if (id == QLatin1String("telegram")) {
        page(latin("t.me"), QLatin1Char('/') + target.itemId);
    } else if (id == QLatin1String("discord")) {
        page(latin("discord.com"), latin("/api"));
    } else if (id == QLatin1String("reddit")) {
        page(latin("www.reddit.com"), latin("/comments"));
    } else if (id == QLatin1String("youtube-extra")) {
        page(latin("www.youtube.com"), latin("/oembed"));
    } else {
        // Rows whose first step reads the link's own page (Klipy, Make A Gif, Lightshot, ImgBB, Postimages,
        // Tumblr, Kick, Bluesky, GitHub, and the short links' redirects).
        page(linkHost, path);
    }
    return first;
}

bool isActive(const QString& id)
{
    if (id == QLatin1String("media-file") || id == QLatin1String("tenor-short") || id == QLatin1String("dropbox"))
        return true;
    if (id == QLatin1String("nextcloud"))
        return shareRowsOn();
    for (const Row& row : kRows) {
        if (id == QLatin1String(row.id) && rowOn(row))
            return true;
    }
    return false;
}

void setInactiveForTests(const QStringList& ids)
{
    QMutexLocker lock(&g_inactiveMutex);
    g_inactive = ids;
}

void setActiveForTests(const QStringList& ids)
{
    QMutexLocker lock(&g_inactiveMutex);
    g_active = ids;
}

// ---- Klipy's GIF pages behind their wall --------------------------------------------------------------------------

QUrl klipyOEmbedUrl(const QString& slug)
{
    // The page's own address, rebuilt from the slug (no query, no www): what Klipy's oEmbed knows.
    const QString page = latin("https://klipy.com/gifs/") + slug;
    return QUrl(latin("https://api.klipy.com/api/v1/web/gifs/embed/oembed?format=json&url=") + QString::fromLatin1(QUrl::toPercentEncoding(page)));
}

FirstRequest wallFallbackOf(const Target& target)
{
    FirstRequest fallback;
    if (target.cls != LinkClass::MediaPage || target.provider != QLatin1String("klipy") || target.sub != QLatin1String("gifs") || target.itemId.isEmpty())
        return fallback;
    const QUrl url        = klipyOEmbedUrl(target.itemId);
    fallback.host         = url.host();
    fallback.lane         = HostGate::Lane::Page;
    fallback.firstSegment = firstSegmentOf(url.path(QUrl::FullyDecoded));
    return fallback;
}

QUrl klipyOEmbedMedia(const QByteArray& json)
{
    if (json.size() > net::kMaxJsonBytes)
        return {};
    QJsonParseError     error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    const QJsonValue thumbnail = doc.object().value(QLatin1String("thumbnail_url"));
    if (!thumbnail.isString())
        return {};
    // Written as https (never upgraded from http), then the rules of every address a website names.
    const QString raw = thumbnail.toString().trimmed();
    if (!raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        return {};
    const QUrl url = net::resolveExtracted(QUrl(latin("https://api.klipy.com/")), raw);
    if (!url.isValid() || !mediaHopAllowed(latin("klipy"), url))
        return {};
    // A file, not the host's root ("https://static2.klipy.com/" names no GIF).
    if (url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts).isEmpty())
        return {};
    return url;
}

QString tenorMediaId(quint64 id)
{
    if (id == 0)
        return {};
    QByteArray bytes(8, char(0));
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<char>(id & 0xFF);
        id >>= 8;
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// ---- site names -------------------------------------------------------------------------------------------------

bool siteNameMatches(const QString& siteName, const QString& host)
{
    const QString domain = net::registrableDomain(host);
    QString       label  = domain.left(domain.indexOf(QLatin1Char('.'))).toLower();
    label.remove(QLatin1Char('-'));
    if (label.isEmpty())
        return false;
    QStringList words;
    QString     current;
    QString     squeezed;
    for (const QChar ch : siteName) {
        if (ch.isLetterOrNumber()) {
            current += ch.toLower();
            squeezed += ch.toLower();
        } else if (!current.isEmpty()) {
            words << current;
            current.clear();
        }
    }
    if (!current.isEmpty())
        words << current;
    if (label.size() <= 3)
        return squeezed == label;
    for (int start = 0; start < words.size(); ++start) {
        QString run;
        for (int end = start; end < words.size() && run.size() < label.size(); ++end) {
            run += words.at(end);
            if (run == label)
                return true;
        }
    }
    return false;
}

} // namespace linkpreview::providers
