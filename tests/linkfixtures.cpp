#include "linkfixtures.h"

#include <QBuffer>
#include <QImage>
#include <QImageWriter>
#include <QThread>
#include <QUrl>

#include <cstring>

#include "linkpreview/netpolicy.h"

namespace linkfixtures {

namespace {

QByteArray latin(const QString& text)
{
    return text.toUtf8();
}

// n bytes of harmless filler (an inline script's body, a page's body text).
QByteArray filler(int bytes, char c = 'x')
{
    QByteArray out;
    out.reserve(bytes);
    const QByteArray line = QByteArray(79, c) + '\n';
    while (out.size() + line.size() <= bytes)
        out += line;
    out += QByteArray(bytes - out.size(), c);
    return out;
}

QByteArray page(const QByteArray& head, const QByteArray& body = "<p>text</p>")
{
    return "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n" + head + "\n</head>\n<body>" + body + "</body></html>\n";
}

QByteArray meta(const char* property, const QByteArray& content)
{
    return QByteArray("<meta property=\"") + property + "\" content=\"" + content + "\">\n";
}

QByteArray metaName(const char* name, const QByteArray& content)
{
    return QByteArray("<meta name=\"") + name + "\" content=\"" + content + "\">\n";
}

QByteArray encoded(const QImage& image, const char* format, int quality = -1)
{
    QByteArray data;
    QBuffer    buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    writer.setQuality(quality);
    writer.write(image);
    return data;
}

void le32(QByteArray* out, quint32 v)
{
    out->append(static_cast<char>(v & 0xFF));
    out->append(static_cast<char>((v >> 8) & 0xFF));
    out->append(static_cast<char>((v >> 16) & 0xFF));
    out->append(static_cast<char>((v >> 24) & 0xFF));
}

QByteArray chunk(const char* fourcc, const QByteArray& payload)
{
    QByteArray out(fourcc, 4);
    le32(&out, static_cast<quint32>(payload.size()));
    out += payload;
    if (payload.size() & 1)
        out += '\0';
    return out;
}

QByteArray riff(const QByteArray& chunks)
{
    QByteArray out("RIFF");
    le32(&out, static_cast<quint32>(chunks.size() + 4));
    out += "WEBP";
    out += chunks;
    return out;
}

} // namespace

QByteArray utf8(std::initializer_list<uint> codes)
{
    return text(codes).toUtf8();
}

QString text(std::initializer_list<uint> codes)
{
    QString out;
    for (const uint code : codes)
        out += QString::fromUcs4(&code, 1);
    return out;
}

// ---- pages ------------------------------------------------------------------------------------------------------

QByteArray tenorView(const QString& id)
{
    return page(meta("og:title", "Cat Cute GIF - Cat Cute Angry - Discover &amp; Share GIFs") + meta("og:site_name", "Tenor")
                + meta("og:image", "https://media1.tenor.com/m/" + latin(id) + "/cat.gif") + metaName("twitter:card", "player"));
}

QByteArray tenorOEmbed(const QString& mediaId)
{
    return "{\"version\":\"1.0\",\"type\":\"video\",\"title\":\"Cat Cute GIF\",\"thumbnail_url\":\"https://media.tenor.com/" + latin(mediaId)
           + "AAAAe/cat-cute.png\",\"thumbnail_width\":498,\"thumbnail_height\":373,\"width\":498,\"height\":373,"
             "\"provider_name\":\"Tenor\",\"provider_url\":\"https://tenor.com\",\"html\":\"<iframe src=x></iframe>\"}";
}

QByteArray giphyGif(const QString& id)
{
    return page(meta("og:title", "Teacher GIF - Find &amp; Share on GIPHY") + meta("og:site_name", "GIPHY")
                + meta("og:image", "https://media0.giphy.com/media/" + latin(id) + "/giphy.gif") + meta("og:video", "https://media0.giphy.com/media/" + latin(id) + "/giphy.mp4")
                + meta("og:video:type", "video/mp4"));
}

QByteArray imgurAlbum(bool videoCover)
{
    QByteArray head = meta("og:site_name", "Imgur") + meta("og:title", "Funny cat - Album on Imgur")
                      + meta("og:image", "https://i.imgur.com/AbCdE12.jpeg?fb");
    if (videoCover)
        head += metaName("twitter:player:stream", "https://i.imgur.com/AbCdE12.mp4") + meta("og:video", "https://i.imgur.com/AbCdE12.mp4");
    return page(head);
}

QByteArray imgurPostMismatch()
{
    return page(meta("og:site_name", "Imgur") + meta("og:video", "https://i.imgur.com/XyZ9876.gif") + meta("og:video:type", "video/mp4")
                + metaName("twitter:player:stream", "https://i.imgur.com/XyZ9876.mp4") + meta("og:image", "https://i.imgur.com/XyZ9876.jpg?fb"));
}

QByteArray imgurShell()
{
    return page(meta("og:site_name", "Imgur") + "<title>Imgur: The magic of the Internet</title>");
}

QByteArray klipyClip()
{
    return page("<title>Angry Nod - Klipy</title>" + meta("og:title", "Angry Nod") + meta("og:image", "https://static.klipy.com/clips/angry-nod/poster.jpg")
                + meta("og:video:url", "https://static.klipy.com/clips/angry-nod/clip.mp4") + meta("og:video:type", "video/mp4")
                + metaName("twitter:player:stream", "https://klipy.com/embed/angry-nod"));
}

QByteArray klipySticker()
{
    return page("<title>Yes Excited Sticker - Klipy</title>" + meta("og:title", "Yes Excited") + meta("og:image", "https://static3.klipy.com/stickers/yes/sticker.gif"));
}

QByteArray klipyChallenge()
{
    return "<!DOCTYPE html><html lang=\"en-US\"><head><title>Just a moment...</title><meta http-equiv=\"refresh\" content=\"390\">"
           "<script>window._cf_chl_opt={}</script></head><body><noscript>Enable JavaScript and cookies to continue</noscript></body></html>";
}

QByteArray klipyOEmbed(const QByteArray& thumbnailUrl)
{
    return "{\"type\":\"video\",\"version\":\"1.0\",\"title\":\"Andy Samberg Awkward\",\"author_name\":\"KLIPY\",\"author_url\":\"https:\\/\\/klipy.com\","
           "\"provider_name\":\"KLIPY\",\"provider_url\":\"https:\\/\\/klipy.com\",\"cache_age\":60,\"thumbnail_url\":\""
           + thumbnailUrl
           + "\",\"thumbnail_width\":392,\"thumbnail_height\":498,\"width\":600,\"height\":600,\"html\":\"<iframe src=\\\"https:\\/\\/klipy.com\\/gifs\\/"
             "andy-samberg-awkward\\/player\\\" width=\\\"600\\\" height=\\\"600\\\" frameborder=\\\"no\\\" allowFullScreen><\\/iframe>\"}";
}

QByteArray kickClip()
{
    QByteArray head = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Kick</title><script>" + filler(8000) + "</script></head><body>";
    QByteArray body = "<div id=\"app\">" + filler(260 * 1024, 'k') + "</div>";
    body += meta("og:title", "xQc clip: the moment") + meta("og:image", "https://clips.kick.com/clips/clip_01H811MXG4FBR62FXPE1AXABDH/thumb.jpg")
            + meta("og:video", "https://clips.kick.com/clips/clip_01H811MXG4FBR62FXPE1AXABDH/playlist.m3u8") + meta("og:video:type", "application/x-mpegURL");
    return head + body + "</body></html>";
}

QByteArray kickChannel()
{
    return page(meta("og:title", "xQc | Kick") + meta("og:description", "Watch xQc live on Kick") + meta("og:image", "https://images.kick.com/channel/xqc/banner.webp"));
}

QByteArray blueskyPost(BlueskyKind kind)
{
    QByteArray head = "<script>" + filler(25 * 1024) + "</script>" + metaName("theme-color", "") + meta("og:site_name", "Bluesky Social")
                      + meta("og:title", "Bluesky (@bsky.app)");
    switch (kind) {
    case BlueskyKind::Image:
        head += meta("og:description", "A post with a picture") + meta("og:image", "https://cdn.bsky.app/img/feed_thumbnail/plain/did:plc:z72/bafk@jpeg")
                + metaName("twitter:card", "summary_large_image");
        break;
    case BlueskyKind::Video:
        head += meta("og:description", "A post with a video") + meta("og:image", "https://video.bsky.app/watch/did/bafk/thumbnail.jpg")
                + meta("og:video", "https://video.bsky.app/watch/did/bafk/playlist.m3u8");
        break;
    case BlueskyKind::Text:
        head += meta("og:description", "Just words in this post") + meta("og:image", "https://cdn.bsky.app/img/avatar/plain/did:plc:z72/bafk@jpeg")
                + metaName("twitter:card", "summary");
        break;
    case BlueskyKind::OptOut:
        head += meta("og:description", "This author has chosen to make their posts visible only to people who are signed in.");
        break;
    }
    return page(head);
}

QByteArray twitchShell()
{
    return page(meta("og:site_name", "Twitch") + meta("og:title", "Twitch") + meta("og:description", "Watch people play games live, and talk with them while they do")
                + meta("og:image", "https://static-cdn.jtvnw.net/ttv-static-metadata/twitch_logo3.jpg") + "<title>Twitch</title>");
}

QByteArray twitchClip()
{
    return page(meta("og:site_name", "Twitch") + meta("og:title", "shroud - That one shot") + meta("og:description", "Watch shroud's clip titled \"That one shot\"")
                + meta("og:image", "https://clips-media-assets2.twitch.tv/abc/AT-cm%7Cabc-preview-1920x1080.jpg") + meta("og:video:duration", "28")
                + meta("og:type", "video.other"));
}

QByteArray twitchChannel()
{
    return page(meta("og:site_name", "Twitch") + meta("og:title", "shroud - Twitch") + meta("og:description", "Shroud plays games")
                + meta("og:image", "https://static-cdn.jtvnw.net/jtv_user_pictures/shroud-profile_image-300x300.png"));
}

QByteArray redditRss(RedditLink link)
{
    QByteArray href;
    switch (link) {
    case RedditLink::IRedd:
        href = "https://i.redd.it/cx6f6vybmhq41.gif";
        break;
    case RedditLink::VRedd:
        href = "https://v.redd.it/if97zvl1wgxh1";
        break;
    case RedditLink::Imgur:
        href = "https://i.imgur.com/Rum0zSz.gifv";
        break;
    case RedditLink::External:
        href = "https://www.example.org/news/a-story";
        break;
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:media=\"http://search.yahoo.com/mrss/\">"
           "<category term=\"gifs\" label=\"r/gifs\"/><title>Feed title</title>"
           "<entry><author><name>/u/someone</name><uri>https://www.reddit.com/user/someone</uri></author>"
           "<category term=\"gifs\" label=\"r/gifs\"/>"
           "<content type=\"html\">&lt;table&gt; &lt;tr&gt;&lt;td&gt; submitted by &lt;a href=&quot;https://www.reddit.com/user/someone&quot;&gt; /u/someone &lt;/a&gt;"
           " &lt;br/&gt; &lt;span&gt;&lt;a href=&quot;"
           + href
           + "&quot;&gt;[link]&lt;/a&gt;&lt;/span&gt; &lt;span&gt;&lt;a href=&quot;https://www.reddit.com/r/gifs/comments/ftwv49/x/&quot;&gt;[comments]&lt;/a&gt;&lt;/span&gt;"
             "&lt;/td&gt;&lt;/tr&gt;&lt;/table&gt;</content>"
             "<id>t3_ftwv49</id><media:thumbnail url=\"https://preview.redd.it/cx6f6vybmhq41.gif?width=320&amp;crop=smart&amp;s=fcd5\"/>"
             "<link href=\"https://www.reddit.com/r/gifs/comments/ftwv49/x/\"/><updated>2020-04-02T12:00:00+00:00</updated>"
             "<title>Dog &amp; ball &#8212; a classic</title></entry></feed>";
}

QByteArray vredditMpd(bool audio, bool cmaf)
{
    const QByteArray ext = cmaf ? ".mp4" : "";
    const QByteArray pre = cmaf ? "CMAF_" : "DASH_";
    QByteArray       xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" minBufferTime=\"PT1.5S\" type=\"static\">"
                     "<Period duration=\"PT31S\"><AdaptationSet contentType=\"video\" mimeType=\"video/mp4\" segmentAlignment=\"true\">";
    for (const int h : {220, 270, 360, 480, 720})
        xml += "<Representation id=\"" + QByteArray::number(h) + "\" bandwidth=\"1000\" codecs=\"avc1.4d401f\" height=\"" + QByteArray::number(h)
               + "\" width=\"" + QByteArray::number(h * 16 / 9) + "\"><BaseURL>" + pre + QByteArray::number(h) + ext + "</BaseURL></Representation>";
    xml += "</AdaptationSet>";
    if (audio)
        xml += "<AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\"><Representation id=\"5\" bandwidth=\"64000\"><BaseURL>" + pre
               + "AUDIO_64" + ext + "</BaseURL></Representation></AdaptationSet>";
    return xml + "</Period></MPD>";
}

QByteArray pinterestOEmbed()
{
    return "{\"provider_name\":\"Pinterest\",\"type\":\"rich\",\"title\":\"A pin\",\"thumbnail_url\":\"https://i.pinimg.com/236x/ab/cd/ef/abcdef.jpg\","
           "\"thumbnail_width\":236,\"thumbnail_height\":354,\"html\":\"<iframe src=x></iframe>\",\"version\":\"1.0\"}";
}

QByteArray gyazoOEmbed(bool video)
{
    if (video)
        return "{\"version\":\"1.0\",\"type\":\"video\",\"provider_name\":\"Gyazo\",\"html\":\"<video></video>\",\"width\":640,\"height\":360}";
    return "{\"version\":\"1.0\",\"type\":\"photo\",\"provider_name\":\"Gyazo\",\"url\":\"https://i.gyazo.com/0123456789abcdef0123456789abcdef.png\","
           "\"width\":800,\"height\":600}";
}

QByteArray vimeoOEmbed()
{
    return "{\"type\":\"video\",\"version\":\"1.0\",\"provider_name\":\"Vimeo\",\"title\":\"The New Vimeo Player (You Know, For Videos)\","
           "\"author_name\":\"Vimeo Staff\",\"author_url\":\"https://vimeo.com/staff\",\"duration\":62,\"thumbnail_url\":\"https://i.vimeocdn.com/video/452001751-8216e1ed.jpg?region=us\","
           "\"thumbnail_width\":640,\"thumbnail_height\":360,\"html\":\"<iframe src=\\\"https://player.vimeo.com/video/76979871\\\"></iframe>\"}";
}

QByteArray soundcloudOEmbed()
{
    return "{\"version\":1.0,\"type\":\"rich\",\"provider_name\":\"SoundCloud\",\"title\":\"Flickermood by Forss\",\"author_name\":\"Forss\","
           "\"description\":\"From the Soulhack album,&nbsp;recently featured &amp; more <a href=\\\"https://example.com/x\\\">here</a>.\","
           "\"thumbnail_url\":\"https://i1.sndcdn.com/artworks-000067273316-smsiqx-t500x500.jpg\",\"html\":\"<iframe></iframe>\"}";
}

QByteArray spotifyTrack()
{
    return page("<title>Never Gonna Give You Up - song and lyrics by Rick Astley | Spotify</title>" + meta("og:site_name", "Spotify")
                + meta("og:title", "Never Gonna Give You Up") + meta("og:description", "Rick Astley &#183; Whenever You Need Somebody &#183; Song &#183; 1987")
                + meta("og:image", "https://i.scdn.co/image/ab67616d0000b273baf89eb11ec7c657805d2da0") + meta("og:type", "music.song")
                + metaName("twitter:card", "summary"));
}

QByteArray appleOEmbed()
{
    return "{\"version\":\"1.0\",\"type\":\"rich\",\"title\":\"Never Gonna Give You Up\",\"author_name\":\"Rick Astley\",\"provider_name\":\"Apple Music\","
           "\"thumbnail_url\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/v4/ab/cd/ef/source/100x100bb.jpg\",\"html\":\"<iframe></iframe>\"}";
}

QByteArray deezerOEmbed()
{
    return "{\"version\":\"1.0\",\"type\":\"rich\",\"title\":\"Harder, Better, Faster, Stronger\",\"author_name\":\"Daft Punk\",\"provider_name\":\"Deezer\","
           "\"thumbnail_url\":\"https://cdn-images.dzcdn.net/images/cover/abc/500x500-000000-80-0-0.jpg\",\"thumbnail_width\":500,\"thumbnail_height\":500}";
}

QByteArray dailymotionOEmbed()
{
    return "{\"type\":\"video\",\"version\":\"1.0\",\"provider_name\":\"Dailymotion\",\"title\":\"A Dailymotion video\",\"author_name\":\"Someone\","
           "\"thumbnail_url\":\"https://s1.dmcdn.net/v/TOKEN/x720\",\"thumbnail_width\":405,\"thumbnail_height\":720,\"duration\":95}";
}

QByteArray youtubeOEmbed()
{
    return "{\"title\":\"Lofi Girl live\",\"author_name\":\"Lofi Girl\",\"type\":\"video\",\"provider_name\":\"YouTube\","
           "\"thumbnail_url\":\"https://i.ytimg.com/vi/jfKfPfyJRdk/hqdefault.jpg\",\"thumbnail_width\":480,\"thumbnail_height\":360}";
}

QByteArray medalOEmbed()
{
    return "{\"type\":\"video\",\"version\":\"1.0\",\"title\":\"Ace on Haven\",\"author_name\":\"player1\",\"provider_name\":\"Medal\","
           "\"thumbnail_url\":\"https://cdn.medal.tv/123/thumbnail-1280.jpg?signature=abc&expires=9\",\"thumbnail_width\":1280,\"thumbnail_height\":720}";
}

QByteArray xStatus(bool largeImage)
{
    return page(metaName("theme-color", "#FFFFFF").replace("<meta ", "<meta media=\"(prefers-color-scheme: light)\" ")
                + metaName("theme-color", "#000000").replace("<meta ", "<meta media=\"(prefers-color-scheme: dark)\" ")
                + meta("og:site_name", "X (formerly Twitter)") + meta("og:title", "jack on X") + meta("og:description", "&quot;just setting up my twttr&quot;")
                + meta("og:image", largeImage ? "https://pbs.twimg.com/media/AbCdEf.jpg:large" : "https://pbs.twimg.com/profile_images/1/abc_200x200.jpg")
                + metaName("twitter:card", largeImage ? "summary_large_image" : "summary"));
}

QByteArray wikiSummary(bool rtlZwnj)
{
    if (rtlZwnj) {
        const QByteArray title = utf8({0x062A, 0x06CC, 0x0645, 0x200C, 0x0627, 0x0633, 0x067E, 0x06CC, 0x06A9});
        return "{\"type\":\"standard\",\"title\":\"" + title + "\",\"displaytitle\":\"<span>" + title
               + "</span>\",\"dir\":\"rtl\",\"extract\":\"" + title + " " + utf8({0x06CC, 0x06A9}) + " software.\",\"extract_html\":\"<p>x</p>\","
               "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/fa/thumb/a/ab/x.png/320px-x.png\",\"width\":320,\"height\":240}}";
    }
    return "{\"type\":\"standard\",\"title\":\"TeamSpeak\",\"displaytitle\":\"<span class=\\\"mw-page-title-main\\\">TeamSpeak</span>\",\"dir\":\"ltr\","
           "\"extract\":\"TeamSpeak is a proprietary voice-over-Internet Protocol application.\",\"extract_html\":\"<p><b>TeamSpeak</b> is ...</p>\","
           "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/TS.png/640px-TS.png\",\"width\":640,\"height\":360}}";
}

QByteArray discordInvite(bool splash, bool nsfw)
{
    return "{\"type\":0,\"code\":\"minecraft\",\"guild\":{\"id\":\"302094807046684672\",\"name\":\"MINECRAFT\",\"description\":\"The official Minecraft Discord!\","
           "\"splash\":"
           + QByteArray(splash ? "\"0123456789abcdef0123456789abcdef\"" : "null")
           + ",\"icon\":\"a_fedcba9876543210fedcba9876543210\",\"nsfw\":" + QByteArray(nsfw ? "true" : "false")
           + ",\"nsfw_level\":" + QByteArray(nsfw ? "3" : "0") + "},\"approximate_member_count\":1234567,\"approximate_presence_count\":89012}";
}

QByteArray pixeldrainInfo(const QString& mime, qint64 size)
{
    return "{\"id\":\"AbCd12\",\"name\":\"cat.png\",\"size\":" + QByteArray::number(size) + ",\"mime_type\":\"" + latin(mime) + "\",\"success\":true}";
}

QByteArray telegramPost()
{
    return page(meta("og:site_name", "Telegram") + meta("og:title", "Pavel Durov") + meta("og:description", "A post in the channel, with words.")
                + meta("og:image", "https://cdn4.telesco.pe/file/AbCdEf.jpg") + metaName("twitter:card", "summary_large_image")
                + meta("og:image:width", "800") + meta("og:image:height", "450"));
}

QByteArray steamStore()
{
    return page("<title>Counter-Strike 2 on Steam</title>" + meta("og:site_name", "Steam") + meta("og:title", "Counter-Strike 2 on Steam")
                + meta("og:description", "A team game of short competitive rounds, made for playing with friends.")
                + meta("og:image", "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/730/header.jpg?t=1"));
}

QByteArray steamScreenshot()
{
    return page(meta("og:title", "Steam Community :: Screenshot :: A view") + meta("og:image", "https://images.steamusercontent.com/ugc/123/ABC/?imw=5000&imh=5000"));
}

QByteArray steamAgecheck()
{
    return page("<title>Site Error</title>" + meta("og:title", "Welcome to Steam"));
}

QByteArray githubRepo()
{
    return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><link rel=\"stylesheet\" href=\"x.css\"><script>" + filler(25 * 1024)
           + "</script>" + meta("og:image", "https://opengraph.githubassets.com/abc/torvalds/linux") + meta("og:site_name", "GitHub")
           + meta("og:type", "object") + meta("og:title", "GitHub - torvalds/linux: Linux kernel source tree")
           + meta("og:description", "Linux kernel source tree. Contribute to torvalds/linux development by creating an account on GitHub.")
           + metaName("twitter:card", "summary_large_image") + "<title>GitHub - torvalds/linux: Linux kernel source tree</title></head><body>x</body></html>";
}

QByteArray cnnLike()
{
    QByteArray head = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>CNN International - Breaking News</title><style>" + filler(240 * 1024, 's')
                      + "</style>" + meta("og:title", "CNN International - Breaking News, US News, World News and Video")
                      + meta("og:description", "View the latest news and breaking news today.") + meta("og:image", "https://media.cnn.com/api/v1/images/x.jpg")
                      + "<script>" + filler(180 * 1024, 'j') + "</script>" + meta("og:site_name", "CNN");
    return head + "</head><body>x</body></html>";
}

QByteArray teamspeakHome()
{
    return page("<title>TeamSpeak | Voice Communication for Online Gaming</title>" + meta("og:title", "TeamSpeak") + meta("og:site_name", "TeamSpeak")
                + meta("og:description", "Use crystal clear sound to communicate with your team mates.") + meta("og:image", "/assets/images/og-image.png")
                + metaName("theme-color", "#2E7D32"));
}

QByteArray discourseSvg()
{
    return page("<title>TeamSpeak 6.0.0 Beta5.1 - Announcements - TeamSpeak Community</title>" + meta("og:site_name", "TeamSpeak Community")
                + meta("og:title", "TeamSpeak 6.0.0 Beta5.1") + meta("og:description", "The new beta is out.")
                + meta("og:image", "https://community.teamspeak.com/uploads/default/original/1X/logo.svg") + metaName("twitter:image", "https://community.teamspeak.com/uploads/default/original/1X/card.png")
                + metaName("twitter:card", "summary"));
}

QByteArray shiftJisPage()
{
    // <title> and og:title in Shift_JIS ("price comparison"), the charset only in <meta>.
    const QByteArray title("\x89\xbf\x8a\x69\x94\xe4\x8a\x72", 8);
    return "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=Shift_JIS\"><title>" + title + "</title>" + meta("og:title", title)
           + meta("og:description", "kakaku") + "</head><body>x</body></html>";
}

QByteArray cfChallenge403()
{
    return klipyChallenge();
}

QByteArray clientChallenge200()
{
    return "<!DOCTYPE html><html><head><title>Client Challenge</title><script>" + filler(2000) + "</script></head><body></body></html>";
}

QByteArray consentPage()
{
    return page(metaName("robots", "noindex,nofollow") + "<title>Golem</title>");
}

QByteArray mastodon404WithOg()
{
    return page(meta("og:site_name", "Mastodon") + meta("og:title", "Mastodon") + meta("og:description", "The original server operated by the Mastodon gGmbH")
                + meta("og:image", "https://files.mastodon.social/site_uploads/files/thumbnail.png"));
}

QByteArray twitchVodBoilerplate()
{
    return twitchShell();
}

QByteArray titleOnly()
{
    return page("<title>QNetworkAccessManager Class | Qt Network 5.15</title>");
}

QByteArray webCard(const QString& title, const QString& description, const QString& image, const QString& extraHead)
{
    QByteArray head = meta("og:title", latin(title));
    if (!description.isEmpty())
        head += meta("og:description", latin(description));
    if (!image.isEmpty())
        head += meta("og:image", latin(image));
    return page(head + latin(extraHead));
}

QByteArray makeagifPage()
{
    return page(meta("og:title", "Jayne Cobb GIF") + meta("og:video", "https://i.makeagif.com/media/1-01-2020/AbCdEf.mp4")
                + meta("og:image", "https://i.makeagif.com/media/1-01-2020/AbCdEf.gif"));
}

QByteArray lightshotPage()
{
    return page(meta("og:title", "Screenshot") + meta("og:image", "https://image.prntscr.com/image/AbCdEf.png"));
}

QByteArray tumblrPost(int kind)
{
    QByteArray head = meta("og:site_name", "Tumblr") + meta("og:title", "A tumblr post") + meta("og:description", "Words of the post");
    if (kind == 0)
        head += meta("og:video", "https://va.media.tumblr.com/tumblr_abc.mp4") + meta("og:image", "https://64.media.tumblr.com/abc/poster.jpg");
    else if (kind == 1)
        head += meta("og:image", "https://64.media.tumblr.com/abc/s1280x1920/photo.jpg");
    return page(head);
}

// ---- media bytes ----------------------------------------------------------------------------------------------

QByteArray png(int width, int height, bool alpha)
{
    QImage image(width, height, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    image.fill(alpha ? QColor(30, 120, 200, 128) : QColor(30, 120, 200));
    return encoded(image, "PNG");
}

QByteArray jpeg(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(200, 40, 40));
    return encoded(image, "JPG", 80);
}

QByteArray gif(int frames)
{
    // GIF89a, 1x1, a global table of 2 colours; frames > 1 get a NETSCAPE loop block and one image per frame.
    static const unsigned char header[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00};
    static const unsigned char loop[]   = {0x21, 0xff, 0x0b, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char frame[]  = {0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
                                          0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00};
    QByteArray out(reinterpret_cast<const char*>(header), sizeof(header));
    if (frames > 1)
        out += QByteArray(reinterpret_cast<const char*>(loop), sizeof(loop));
    for (int i = 0; i < qMax(1, frames); ++i)
        out += QByteArray(reinterpret_cast<const char*>(frame), sizeof(frame));
    out += ';';
    return out;
}

QByteArray webpLossy()
{
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(QColor(10, 200, 30));
    const QByteArray real = encoded(image, "WEBP", 80);
    if (real.startsWith("RIFF") && real.contains("VP8 "))
        return real;
    // No WebP writer here: a structurally valid container (not decodable).
    return riff(chunk("VP8 ", QByteArray(18, '\x10')));
}

QByteArray webpLossless()
{
    return riff(chunk("VP8L", QByteArray("\x2f\x07\xc0\x01\x00\x00\x00\x00\x00", 9)));
}

QByteArray webpAlphLossless()
{
    QByteArray vp8x(10, '\0');
    vp8x[0] = 0x10; // alpha
    return riff(chunk("VP8X", vp8x) + chunk("ALPH", QByteArray("\x01\x00\x00", 3)) + chunk("VP8 ", QByteArray(18, '\x10')));
}

QByteArray webpAnimatedLosslessFrame()
{
    QByteArray vp8x(10, '\0');
    vp8x[0] = 0x02; // animation
    const QByteArray frameHeader(16, '\0');
    return riff(chunk("VP8X", vp8x) + chunk("ANIM", QByteArray(6, '\0')) + chunk("ANMF", frameHeader + chunk("VP8 ", QByteArray(18, '\x10')))
                + chunk("ANMF", frameHeader + chunk("VP8L", QByteArray("\x2f\x07\xc0\x01\x00", 5))));
}

QByteArray mp4Header()
{
    QByteArray out("\x00\x00\x00\x18" "ftypisom\x00\x00\x02\x00isomiso2", 24);
    out += QByteArray("\x00\x00\x00\x08" "free", 8);
    out += QByteArray(4096, '\0');
    return out;
}

QByteArray svgAsJpg()
{
    return "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"10\" height=\"10\">"
           "<image xlink:href=\"//127.0.0.1/share/x.png\" width=\"10\" height=\"10\"/></svg>";
}

QByteArray xpmImage()
{
    return "/* XPM */\nstatic char * x[] = {\n\"2 2 1 1\",\n\"a c #FF0000\",\n\"aa\",\n\"aa\"};\n";
}

// ---- FakeHttp -----------------------------------------------------------------------------------------------------

namespace {

class FakeExchange : public linkpreview::http::Exchange
{
  public:
    FakeExchange(FakeHttp* owner, Reply reply, linkpreview::net::Address peer, const linkpreview::http::Cancel& cancel, quint64 deadline)
        : m_owner(owner)
        , m_reply(std::move(reply))
        , m_peer(peer)
        , m_cancel(cancel)
        , m_deadline(deadline)
    {
    }

    linkpreview::http::Error head(linkpreview::http::Head* out) override
    {
        if (m_reply.delayMs > 0 && m_cancel.wait(m_reply.delayMs))
            return linkpreview::http::Error::Canceled;
        out->status          = m_reply.status;
        out->contentType     = m_reply.contentType;
        out->contentLength   = m_reply.contentLength == -2 ? m_reply.body.size() : m_reply.contentLength;
        out->location        = m_reply.location;
        out->peer            = m_peer;
        out->contentEncoding = m_reply.contentEncoding;
        out->retryAfterMs    = m_reply.retryAfterMs;
        out->rateRemaining   = m_reply.rateRemaining;
        out->rateResetMs     = m_reply.rateResetMs;
        out->challenge       = m_reply.challenge;
        return linkpreview::http::Error::None;
    }

    linkpreview::http::Error read(char* buffer, int capacity, int* got) override
    {
        *got = 0;
        if (m_cancel.isSet())
            return linkpreview::http::Error::Canceled;
        if (m_reply.stallAfterBytes >= 0 && m_pos >= m_reply.stallAfterBytes) {
            if (m_cancel.wait(linkpreview::http::msUntil(m_deadline)))
                return linkpreview::http::Error::Canceled;
            return linkpreview::http::Error::Timeout;
        }
        if (m_reply.dripMs > 0 && m_cancel.wait(m_reply.dripMs))
            return linkpreview::http::Error::Canceled;
        int count = qMin(qMin(capacity, m_reply.chunk), m_reply.body.size() - m_pos);
        if (m_reply.stallAfterBytes >= 0)
            count = qMin(count, m_reply.stallAfterBytes - m_pos);
        if (count <= 0)
            return linkpreview::http::Error::None;
        memcpy(buffer, m_reply.body.constData() + m_pos, static_cast<size_t>(count));
        m_pos += count;
        *got = count;
        m_owner->m_bytesRead += count;
        return linkpreview::http::Error::None;
    }

  private:
    FakeHttp*                          m_owner;
    Reply                              m_reply;
    linkpreview::net::Address          m_peer;
    const linkpreview::http::Cancel&   m_cancel;
    quint64                            m_deadline;
    int                                m_pos = 0;
};

} // namespace

void FakeHttp::respond(const QString& url, const Reply& reply)
{
    respondSequence(url, {reply});
}

void FakeHttp::respondSequence(const QString& url, const QVector<Reply>& replies)
{
    QMutexLocker lock(&m_mutex);
    const QString key = QUrl(url).toString(QUrl::FullyEncoded);
    m_replies.insert(key, replies);
    m_served.remove(key);
}

QVector<FakeHttp::Request> FakeHttp::requests() const
{
    QMutexLocker lock(&m_mutex);
    return m_requests;
}

QStringList FakeHttp::urls() const
{
    QMutexLocker lock(&m_mutex);
    QStringList out;
    for (const Request& r : m_requests)
        out << r.url;
    return out;
}

int FakeHttp::count() const
{
    QMutexLocker lock(&m_mutex);
    return m_requests.size();
}

void FakeHttp::setDns(const QString& host, const QString& address)
{
    QMutexLocker lock(&m_mutex);
    m_dns.insert(host, address);
}

linkpreview::http::Error FakeHttp::resolve(const QString& host, QVector<linkpreview::net::Address>* out, const linkpreview::http::Cancel& cancel, quint64)
{
    if (cancel.isSet())
        return linkpreview::http::Error::Canceled;
    QMutexLocker lock(&m_mutex);
    *out = {linkpreview::net::parseAddress(m_dns.value(host, QStringLiteral("93.184.216.34")))};
    return linkpreview::http::Error::None;
}

linkpreview::http::Route FakeHttp::route(const QUrl&, const linkpreview::http::Cancel&, quint64)
{
    linkpreview::http::Route r;
    if (m_viaProxy) {
        r.viaProxy = true;
        r.proxy    = QStringLiteral("127.0.0.1:10809");
    }
    return r;
}

std::unique_ptr<linkpreview::http::Exchange> FakeHttp::open(const linkpreview::http::HopOptions& options, const linkpreview::http::Cancel& cancel,
                                                            linkpreview::http::Error* error)
{
    *error            = linkpreview::http::Error::None;
    const QString key = options.url.toString(QUrl::FullyEncoded);
    Reply         reply;
    QString       peer;
    {
        QMutexLocker lock(&m_mutex);
        const auto   it = m_replies.constFind(key);
        if (it != m_replies.constEnd() && !it->isEmpty()) {
            const int served = m_served.value(key);
            reply            = it->at(qMin(served, it->size() - 1));
            m_served.insert(key, served + 1);
        } else {
            reply.status      = 404;
            reply.contentType = "text/plain";
            reply.body        = "missing";
        }
        peer = reply.peer.isEmpty() ? m_dns.value(options.url.host(), QStringLiteral("93.184.216.34")) : reply.peer;
        if (options.route.viaProxy)
            peer = QStringLiteral("127.0.0.1");
    }
    const linkpreview::net::Address address = linkpreview::net::parseAddress(peer);
    if (options.allowPeer && !options.allowPeer(address)) {
        *error = linkpreview::http::Error::Blocked;
        return nullptr;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_requests.append({key, options.accept, options.userAgent, options.acceptEncoding, options.route.viaProxy});
    }
    return std::unique_ptr<linkpreview::http::Exchange>(new FakeExchange(this, reply, address, cancel, options.deadline));
}

} // namespace linkfixtures
