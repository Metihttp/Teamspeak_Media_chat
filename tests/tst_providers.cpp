// Unit tests for 2.5 link previews: the link classes and page keys (linkurl), the provider table (providers), and
// the pure network rules that came with them (netpolicy: registrable domains, extracted addresses, Retry-After
// and rate-limit headers, the PC's DNS suffixes). No network.
//
// Every non-ASCII character is written as a code point: no invisible character in the source.

#include <QElapsedTimer>
#include <QtTest>

#include "linkpreview/linkurl.h"
#include "linkpreview/netpolicy.h"
#include "linkpreview/providers.h"
#include "previewpaint.h"
#include "testmain.h"
#include "uiutil.h"
#include "version.h"

using namespace linkpreview;

namespace {

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

QString key(const char* href)
{
    return classify(QString::fromLatin1(href)).key;
}

// A Persian Wikipedia title with a raw ZWNJ ("TeamSpeak" in Persian), as TeamSpeak's anchor may hold it.
QString persianWikiHref()
{
    QString title;
    for (const uint code : {0x062A, 0x06CC, 0x0645, 0x200C, 0x0627, 0x0633, 0x067E, 0x06CC, 0x06A9})
        title += QString::fromUcs4(&code, 1);
    return a("https://fa.wikipedia.org/wiki/") + title;
}

} // namespace

class TestProviders : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        providers::setInactiveForTests({});
        providers::setActiveForTests({});
    }

    // ---- keys -------------------------------------------------------------------------------------------------

    void pageKeys_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("expected");
        QTest::newRow("teamspeak") << a("https://www.teamspeak.com/en/") << a("b39bfc9f8227b9210fd47cb780de4bea");
        QTest::newRow("http upper port fragment") << a("http://WWW.TeamSpeak.com:80/en/#top") << a("b39bfc9f8227b9210fd47cb780de4bea");
        QTest::newRow("port 443") << a("https://www.teamspeak.com:443/en/") << a("b39bfc9f8227b9210fd47cb780de4bea");
        QTest::newRow("percent") << a("https://example.com/a%7eb?x=%2f") << a("fd92ce03cc58de448101a04874d78f1d");
        QTest::newRow("bare host") << a("https://example.com") << a("645abb12aea2ddfd8ea0f90937220726");
        QTest::newRow("tenor") << a("https://tenor.com/view/cat-cute-cat-angry-tantrum-hmph-gif-11594218182993649802") << a("8eb4311921f7b1964785a11a4cc1f418");
        QTest::newRow("giphy") << a("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32") << a("3dc3998d88230cab704869ba95332b17");
        QTest::newRow("gifv") << a("https://i.imgur.com/Rum0zSz.gifv") << a("e4dc34a46b438011b72b45d4ef0bb07e");
        QTest::newRow("spotify si") << a("https://open.spotify.com/intl-de/track/4cOdK2wGLETKBW3PvgPWqT?si=abcdef0123456789") << a("7ee2bc9770976e95075ae78af631d772");
        QTest::newRow("youtube channel web") << a("https://www.youtube.com/@TeamSpeak") << a("c7f7c4ab58b58aee42ff8b4371b307a8");
        QTest::newRow("fa wikipedia") << a("https://fa.wikipedia.org/wiki/%D8%AA%DB%8C%D9%85%E2%80%8C%D8%A7%D8%B3%D9%BE%DB%8C%DA%A9")
                                      << a("557d5a7e4b885fb9215174f7839222ac");
        QTest::newRow("tenor short direct") << a("https://tenor.com/bTSmr.gif") << a("2019fab9d770c5f007bced59a5abfa1e");
    }
    void pageKeys()
    {
        QFETCH(QString, href);
        QFETCH(QString, expected);
        QCOMPARE(classify(href).key, expected);
    }

    void legacyKeysUnchanged()
    {
        // 2.4.0's formulas: SHA-256 of "yt:<id>" and of the normalized media address.
        QCOMPARE(key("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), a("07c34182ce3a961b237f5b4968301d6e"));
        QCOMPARE(key("https://youtu.be/dQw4w9WgXcQ?t=5"), a("07c34182ce3a961b237f5b4968301d6e"));
        QCOMPARE(key("http://www.youtube.com/watch?v=dQw4w9WgXcQ"), a("07c34182ce3a961b237f5b4968301d6e"));
        QCOMPARE(key("https://tenor.com/bTSmr.gif"), keyOf(a("https://tenor.com/bTSmr.gif")));
        QCOMPARE(key("https://EXAMPLE.com:443/%7eme/cat.png?a=1#x"), keyOf(a("https://example.com/~me/cat.png?a=1")));
        QCOMPARE(key("https://i.imgur.com/abc1234.png"), keyOf(a("https://i.imgur.com/abc1234.png")));
        QCOMPARE(key("https://media.tenor.com/oObwAjW2IIoAAAAd/tenor.gif"), keyOf(a("https://media.tenor.com/oObwAjW2IIoAAAAd/tenor.gif")));
        QCOMPARE(key("https://www.dropbox.com/s/abcdefghij12/cat.png?dl=0"), keyOf(a("https://www.dropbox.com/s/abcdefghij12/cat.png?dl=0")));
        for (const char* href : {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "https://youtu.be/dQw4w9WgXcQ", "https://example.com/a.png",
                                 "https://example.com/b.gif", "https://example.com/c.mp4", "https://tenor.com/bTSmr.gif"})
            QVERIFY2(classify(a(href)).isLegacy(), href);
    }

    void classOrder()
    {
        const Target channel = classify(a("https://www.youtube.com/@TeamSpeak"));
        QCOMPARE(channel.cls, LinkClass::Web);
        QCOMPARE(channel.kind, Kind::Card);
        QCOMPARE(channel.expectedRequests, 2);
        const Target shortGif = classify(a("https://tenor.com/bTSmr.gif"));
        QCOMPARE(shortGif.cls, LinkClass::Direct);
        QCOMPARE(shortGif.kind, Kind::Gif);
        QCOMPARE(shortGif.provider, a("tenor-short"));
        const Target dropbox = classify(a("https://www.dropbox.com/scl/fi/abcdefghij1234567890/cat.mp4?rlkey=x&dl=0"));
        QCOMPARE(dropbox.cls, LinkClass::Direct);
        QCOMPARE(dropbox.kind, Kind::Video);
        QCOMPARE(dropbox.provider, a("dropbox"));
        const Target http = classify(a("http://x.example.net/cat.png"));
        QCOMPARE(http.cls, LinkClass::MediaPage);
        QCOMPARE(http.provider, a("media-file"));
        QCOMPARE(http.kind, Kind::Image);
        QCOMPARE(http.page, QUrl(a("https://x.example.net/cat.png")));
        QCOMPARE(http.url, http.page);
        const Target direct = classify(a("https://x.example.net/cat.png"));
        QCOMPARE(direct.cls, LinkClass::Direct);
        QVERIFY(direct.provider.isEmpty());
        QVERIFY(direct.page.isEmpty());
        const Target watch = classify(a("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
        QCOMPARE(watch.cls, LinkClass::YouTube);
        QCOMPARE(watch.expectedRequests, 1);
    }

    // ---- the table --------------------------------------------------------------------------------------------

    void providerMatches_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<int>("cls");
        QTest::addColumn<QString>("provider");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("itemId");
        QTest::addColumn<QString>("sub");
        QTest::addColumn<int>("requests");
        const auto row = [](const char* href, LinkClass cls, const char* provider, Kind kind, const char* itemId, const char* sub, int requests) {
            QTest::newRow(href) << a(href) << int(cls) << a(provider) << int(kind) << a(itemId) << a(sub) << requests;
        };
        const LinkClass M = LinkClass::MediaPage, S = LinkClass::Site, W = LinkClass::Web, N = LinkClass::None;
        const Kind      L = Kind::Loop, G = Kind::Gif, V = Kind::Video, I = Kind::Image, C = Kind::Card, Z = Kind::None;

        // Klipy
        row("https://klipy.com/clips/angry-nod", M, "klipy", V, "angry-nod", "clips", 2);
        row("https://klipy.com/stickers/yes-excited-yes-sticker", M, "klipy", G, "yes-excited-yes-sticker", "stickers", 2);
        row("https://www.klipy.com/gifs/andy-samberg-awkward/", M, "klipy", G, "andy-samberg-awkward", "gifs", 2);
        row("https://klipy.com/static-memes/x1", M, "klipy", G, "x1", "static-memes", 2);
        row("https://klipy.com/emojis/smile", M, "klipy", G, "smile", "emojis", 2);
        row("https://klipy.com/ai-gifs/robot", M, "klipy", G, "robot", "ai-gifs", 2);
        row("https://klipy.com/gifs/UPPER", W, "", C, "", "", 2);
        row("https://klipy.com/about", W, "", C, "", "", 2);
        row("https://klipy.com/gifs/a/b", W, "", C, "", "", 2);
        // Tenor
        row("https://tenor.com/view/cat-cute-cat-angry-tantrum-hmph-gif-11594218182993649802", M, "tenor", L, "11594218182993649802", "view", 1);
        row("https://tenor.com/view/boo-gif-19787475173016375", M, "tenor", L, "19787475173016375", "view", 1);
        row("https://tenor.com/view/blahaj-go-spinny-blahaj-blahaj-spin-spin-shark-spin-gif-25670993", M, "tenor", L, "25670993", "view", 2);
        row("https://tenor.com/de/view/x-gif-123456", M, "tenor", L, "123456", "view", 2);
        row("https://tenor.com/pt-BR/view/x-gif-123456/", M, "tenor", L, "123456", "view", 2);
        row("https://www.tenor.com/view/x-gif-99", M, "tenor", L, "99", "view", 2);
        row("https://tenor.com/view/x-gif-0", W, "", C, "", "", 2);
        row("https://tenor.com/view/no-id-gif", W, "", C, "", "", 2);
        row("https://tenor.com/view/x-gif-99999999999999999999", W, "", C, "", "", 2); // over 64 bits
        row("https://tenor.com/search/cat-gifs", W, "", C, "", "", 2);
        // Giphy
        row("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32", M, "giphy", L, "8FhXc8w45aN32", "gifs", 1);
        row("https://giphy.com/gifs/movie-80s-ferris-buellers-day-off-bkIgPwdV3Afzq", M, "giphy", L, "bkIgPwdV3Afzq", "gifs", 1);
        row("https://giphy.com/gifs/8FhXc8w45aN32", M, "giphy", L, "8FhXc8w45aN32", "gifs", 1);
        row("https://giphy.com/embed/8FhXc8w45aN32", M, "giphy", L, "8FhXc8w45aN32", "embed", 1);
        row("https://giphy.com/gifs/x-8FhXc8w45aN32/html5", M, "giphy", L, "8FhXc8w45aN32", "gifs", 1);
        row("https://www.giphy.com/stickers/happy-abc123DEF", M, "giphy", G, "abc123DEF", "stickers", 1);
        row("https://giphy.com/clips/show-abcdefGHIJ12", M, "giphy", V, "abcdefGHIJ12", "clips", 1);
        row("https://gph.is/2KH8MQ0", M, "giphy", L, "", "short", 2);
        row("https://gph.is/g/ZnXy12", M, "giphy", L, "", "short", 2);
        row("https://giphy.com/explore/cats", W, "", C, "", "", 2);
        row("https://giphy.com/gifs/abc", W, "", C, "", "", 2);
        // Imgur
        row("https://i.imgur.com/Rum0zSz.gifv", M, "imgur", L, "Rum0zSz", "gifv", 1);
        row("https://i.imgur.com/abcde.gifv", M, "imgur", L, "abcde", "gifv", 1);
        row("https://i.imgur.com/abcd.gifv", W, "", C, "", "", 2);
        row("https://imgur.com/Rum0zSz", M, "imgur", G, "Rum0zSz", "post", 1);
        row("https://m.imgur.com/AbCdE", M, "imgur", G, "AbCdE", "post", 1);
        row("https://imgur.com/privacy", W, "", C, "", "", 2);
        row("https://imgur.com/upload", W, "", C, "", "", 2);
        row("https://imgur.com/emerald", W, "", C, "", "", 2);
        row("https://imgur.com/a/bcOmNQt", M, "imgur", G, "bcOmNQt", "album", 2);
        row("https://imgur.com/gallery/funny-cat-bcOmNQt", M, "imgur", G, "bcOmNQt", "album", 2);
        row("https://imgur.com/t/cats/bcOmNQt", M, "imgur", G, "bcOmNQt", "album", 2);
        row("https://i.imgur.com/Rum0zSz.png", LinkClass::Direct, "", I, "", "", 1);
        // Reddit videos and posts
        row("https://v.redd.it/if97zvl1wgxh1", M, "reddit-video", L, "if97zvl1wgxh1", "video", 2);
        row("https://v.redd.it/if97zvl1wgxh1/", M, "reddit-video", L, "if97zvl1wgxh1", "video", 2);
        row("https://v.redd.it/short", W, "", C, "", "", 2);
        row("https://www.reddit.com/r/gifs/comments/ftwv49", S, "reddit", C, "ftwv49", "post", 2);
        row("https://www.reddit.com/r/IAmA/comments/z1c9z/i_am_barack_obama_president_of_the_united_states/", S, "reddit", C, "z1c9z", "post", 2);
        row("https://old.reddit.com/r/gifs/comments/ftwv49/title/", S, "reddit", C, "ftwv49", "post", 2);
        row("https://reddit.com/user/someone/comments/abc12/x", S, "reddit", C, "abc12", "post", 2);
        row("https://www.reddit.com/comments/ftwv49", S, "reddit", C, "ftwv49", "post", 2);
        row("https://www.reddit.com/gallery/ftwv49", S, "reddit", C, "ftwv49", "post", 2);
        row("https://redd.it/ftwv49", S, "reddit", C, "ftwv49", "post", 2);
        row("https://www.reddit.com/r/gifs/s/AbCdEf1234", N, "", Z, "", "", 1);
        row("https://www.reddit.com/r/gifs/", W, "", C, "", "", 2);
        row("https://i.redd.it/cx6f6vybmhq41.gif", LinkClass::Direct, "", G, "", "", 1);
        // Pinterest
        row("https://www.pinterest.com/pin/1008173066603825557/", M, "pinterest", I, "1008173066603825557", "pin", 2);
        row("https://pinterest.com/pin/some-title--1008173066603825557/", M, "pinterest", I, "1008173066603825557", "pin", 2);
        row("https://de.pinterest.de/pin/123456789/", M, "pinterest", I, "123456789", "pin", 2);
        row("https://www.pinterest.co.uk/pin/123456789/", M, "pinterest", I, "123456789", "pin", 2);
        row("https://pin.it/AbCdEf", M, "pinterest", I, "", "short", 3);
        // The Pinterest app's share links (pin.it) end on the pin's /sent/ form with share parameters.
        row("https://www.pinterest.com/pin/858146903966145191/sent/?sender=1&invite_code=x", M, "pinterest", I, "858146903966145191", "pin", 2);
        row("https://www.pinterest.com/pin/858146903966145191/sent", M, "pinterest", I, "858146903966145191", "pin", 2);
        row("https://www.pinterest.com/pin/858146903966145191/other/", W, "", C, "", "", 2);
        row("https://pinterest.evil.example.net/pin/123456789/", W, "", C, "", "", 2);
        row("https://www.pinterest.com/someone/boards/", W, "", C, "", "", 2);
        // Streamable, Imgflip, 9GAG, Coub, Gifer, Make A Gif
        row("https://streamable.com/1hzf2p", M, "streamable", V, "1hzf2p", "video", 1);
        row("https://streamable.com/e/1hzf2p", M, "streamable", V, "1hzf2p", "video", 1);
        row("https://streamable.com/abcdef", W, "", C, "", "", 2);
        row("https://imgflip.com/gif/b3nmqx", M, "imgflip", L, "b3nmqx", "gif", 1);
        row("https://imgflip.com/i/b5c1yf", M, "imgflip", I, "b5c1yf", "i", 1);
        row("https://imgflip.com/memegenerator", W, "", C, "", "", 2);
        row("https://9gag.com/gag/aBZRrjx", M, "9gag", V, "aBZRrjx", "gag", 1);
        row("https://m.9gag.com/gag/aBZRrjx", M, "9gag", V, "aBZRrjx", "gag", 1);
        row("https://coub.com/view/3tgo9z", M, "coub", V, "3tgo9z", "view", 1);
        row("https://gifer.com/en/Ab1", M, "gifer", L, "Ab1", "gif", 1);
        row("https://makeagif.com/gif/jayne-cobb-6yRsMO", M, "makeagif", L, "jayne-cobb-6yRsMO", "gif", 2);
        row("https://makeagif.com/gif/a-title-z84J_a", M, "makeagif", L, "a-title-z84J_a", "gif", 2);
        // Screenshots and file hosts
        row("https://gyazo.com/0123456789abcdef0123456789abcdef", M, "gyazo", I, "0123456789abcdef0123456789abcdef", "capture", 2);
        row("https://prnt.sc/abc123", M, "lightshot", I, "abc123", "capture", 2);
        row("https://prntscr.com/abc123", M, "lightshot", I, "abc123", "capture", 2);
        row("https://ibb.co/AbC123x", M, "imgbb", I, "AbC123x", "image", 2);
        row("https://postimg.cc/image/AbC123x", M, "postimages", I, "AbC123x", "image", 2);
        row("https://pixeldrain.com/u/AbCd12", M, "pixeldrain", I, "AbCd12", "file", 2);
        // Tumblr
        row("https://staff-picks.tumblr.com/post/123456789/a-title", M, "tumblr", I, "123456789", "post", 2);
        row("https://www.tumblr.com/someblog/123456789", M, "tumblr", I, "123456789", "post", 2);
        row("https://api.tumblr.com/post/123456789", W, "", C, "", "", 2);
        // Vimeo
        row("https://vimeo.com/76979871", S, "vimeo", C, "76979871", "video", 2);
        row("https://vimeo.com/76979871/8272103f6e", S, "vimeo", C, "76979871/8272103f6e", "video", 2);
        row("https://vimeo.com/channels/staffpicks/76979871", S, "vimeo", C, "76979871", "video", 2);
        row("https://vimeo.com/groups/name/videos/76979871", S, "vimeo", C, "76979871", "video", 2);
        row("https://vimeo.com/album/123/video/76979871", S, "vimeo", C, "76979871", "video", 2);
        row("https://player.vimeo.com/video/76979871", S, "vimeo", C, "76979871", "video", 2);
        row("https://vimeo.com/about", W, "", C, "", "", 2);
        row("https://vimeo.com.attacker.example.net/76979871", W, "", C, "", "", 2);
        // SoundCloud
        row("https://soundcloud.com/forss/flickermood", S, "soundcloud", C, "forss/flickermood", "track", 2);
        row("https://soundcloud.com/forss/sets/soulhack", S, "soundcloud", C, "forss/soulhack", "set", 2);
        row("https://soundcloud.com/forss", S, "soundcloud", C, "forss", "profile", 2);
        row("https://soundcloud.com/forss/likes", W, "", C, "", "", 2);
        row("https://soundcloud.com/discover", W, "", C, "", "", 2);
        row("https://soundcloud.com/charts/top", W, "", C, "", "", 2);
        row("https://on.soundcloud.com/AbCd1", S, "soundcloud", C, "", "short", 3);
        // Spotify, Apple Music, Deezer
        row("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT", S, "spotify", C, "4cOdK2wGLETKBW3PvgPWqT", "track", 2);
        row("https://open.spotify.com/intl-de/track/4cOdK2wGLETKBW3PvgPWqT?si=abcdef0123456789", S, "spotify", C, "4cOdK2wGLETKBW3PvgPWqT", "track", 2);
        row("https://open.spotify.com/intl-pt-br/album/4cOdK2wGLETKBW3PvgPWqT", S, "spotify", C, "4cOdK2wGLETKBW3PvgPWqT", "album", 2);
        row("https://open.spotify.com/episode/4cOdK2wGLETKBW3PvgPWqT", S, "spotify", C, "4cOdK2wGLETKBW3PvgPWqT", "episode", 2);
        row("https://open.spotify.com/track/short", W, "", C, "", "", 2);
        row("https://music.apple.com/us/album/never-gonna-give-you-up/1559885420?i=1559885421", S, "applemusic", C, "1559885420", "album", 2);
        row("https://music.apple.com/gb/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb", S, "applemusic", C,
            "pl.f4d106fed2bd41149aaacabb233eb5eb", "playlist", 2);
        // Deezer: switched off without requests (cdn-images.dzcdn.net answers 403 to every client): no preview at all,
        // never a Web card; its other pages stay Web links (deezerSwitchedOff checks the rows themselves).
        row("https://www.deezer.com/track/14408104", N, "", Z, "", "", 1);
        row("https://www.deezer.com/fr/album/302127", N, "", Z, "", "", 1);
        row("https://link.deezer.com/s/30AbCdEfGh", N, "", Z, "", "", 1);
        row("https://www.deezer.com/en/", W, "", C, "", "", 2);
        // Dailymotion, Medal
        row("https://www.dailymotion.com/video/xbv3lcm", S, "dailymotion", C, "xbv3lcm", "video", 2);
        row("https://dai.ly/xbv3lcm", S, "dailymotion", C, "xbv3lcm", "video", 2);
        row("https://medal.tv/games/valorant/clips/nNUxTEDYDM4ts-dIt", S, "medal", C, "nNUxTEDYDM4ts-dIt", "clip", 2);
        row("https://medal.tv/de/games/valorant/clips/nNUxTEDYDM4ts-dIt/some-title", S, "medal", C, "nNUxTEDYDM4ts-dIt", "clip", 2);
        row("https://medal.tv/clips/nNUxTEDYDM4ts", S, "medal", C, "nNUxTEDYDM4ts", "clip", 2);
        // Twitch, Kick
        row("https://clips.twitch.tv/IgnorantBelovedKathyCopyThis-eRU45Tis0o2zS27d", S, "twitch", C, "IgnorantBelovedKathyCopyThis-eRU45Tis0o2zS27d", "clip", 2);
        row("https://clips.twitch.tv/embed", W, "", C, "", "", 2);
        row("https://www.twitch.tv/shroud/clip/IgnorantBelovedKathyCopyThis", S, "twitch", C, "IgnorantBelovedKathyCopyThis", "clip", 2);
        row("https://m.twitch.tv/clip/IgnorantBelovedKathyCopyThis", S, "twitch", C, "IgnorantBelovedKathyCopyThis", "clip", 2);
        row("https://www.twitch.tv/shroud", S, "twitch", C, "shroud", "channel", 2);
        row("https://www.twitch.tv/directory", W, "", C, "", "", 2);
        row("https://www.twitch.tv/videos/2912355720", N, "", Z, "", "", 1);
        row("https://kick.com/xqc", S, "kick", C, "xqc", "channel", 2);
        row("https://kick.com/xqc/clips/clip_01H811MXG4FBR62FXPE1AXABDH", S, "kick", C, "xqc/clip_01H811MXG4FBR62FXPE1AXABDH", "clip", 2);
        row("https://kick.com/browse", W, "", C, "", "", 2);
        // Bluesky, X
        row("https://bsky.app/profile/bsky.app/post/3mzjjovisxk2h", S, "bluesky", C, "bsky.app/3mzjjovisxk2h", "post", 2);
        row("https://bsky.app/profile/bsky.app", W, "", C, "", "", 2);
        row("https://x.com/jack/status/20", S, "x", C, "jack/20", "status", 2);
        row("https://twitter.com/jack/status/20", S, "x", C, "jack/20", "status", 2);
        row("https://mobile.twitter.com/jack/statuses/20", S, "x", C, "jack/20", "status", 2);
        row("https://x.com/jack/status/20/photo/1", S, "x", C, "jack/20", "status", 2);
        row("https://x.com/jack", W, "", C, "", "", 2);
        // GitHub
        row("https://github.com/torvalds/linux", S, "github", C, "torvalds", "page", 2);
        row("https://github.com/microsoft/vscode/issues/1", S, "github", C, "microsoft", "page", 2);
        row("https://github.com/torvalds", S, "github", C, "torvalds", "page", 2);
        row("https://github.com/login", N, "", Z, "", "", 1);
        row("https://github.com/explore", W, "", C, "", "", 2);
        row("https://github.com/Metihttp/Teamspeak_Media_chat", N, "", Z, "", "", 1); // TS Media's own repository
        // Steam
        row("https://store.steampowered.com/app/730/CounterStrike_2/", S, "steam", C, "730", "app", 2);
        row("https://store.steampowered.com/app/730", S, "steam", C, "730", "app", 2);
        row("https://steamcommunity.com/sharedfiles/filedetails/?id=3070284539", S, "steam", C, "3070284539", "file", 2);
        row("https://steamcommunity.com/sharedfiles/filedetails/?id=abc", W, "", C, "", "", 2);
        row("https://steamcommunity.com/id/gaben", S, "steam", C, "gaben", "page", 2);
        row("https://s.team/a/730", S, "steam", C, "730", "short", 3);
        // Wikipedia
        row("https://en.wikipedia.org/wiki/TeamSpeak", S, "wikipedia", C, "TeamSpeak", "en", 2);
        row("https://en.m.wikipedia.org/wiki/TeamSpeak", S, "wikipedia", C, "TeamSpeak", "en", 2);
        row("https://simple.wikipedia.org/wiki/TeamSpeak", S, "wikipedia", C, "TeamSpeak", "simple", 2);
        row("https://zh-min-nan.wikipedia.org/wiki/X", S, "wikipedia", C, "X", "zh-min-nan", 2);
        row("https://w.wiki/3", S, "wikipedia", C, "", "short", 3);
        row("https://en.wikipedia.org/wiki/Special:Random", N, "", Z, "", "", 1);
        row("https://wikipedia.org/wiki/TeamSpeak", W, "", C, "", "", 2);
        row("https://en.wikipedia.org.evil.example.net/wiki/TeamSpeak", W, "", C, "", "", 2);
        // Telegram, Discord
        row("https://t.me/durov/552", S, "telegram", C, "durov/552", "post", 2);
        row("https://t.me/s/durov/552", S, "telegram", C, "durov/552", "post", 2);
        row("https://t.me/durov", S, "telegram", C, "durov", "channel", 2);
        row("https://t.me/c/123/456", N, "", Z, "", "", 1);
        row("https://t.me/+AbCdEf", N, "", Z, "", "", 1);
        row("https://t.me/joinchat/AbCdEf", N, "", Z, "", "", 1);
        row("https://discord.gg/discord-developers", S, "discord", C, "discord-developers", "invite", 2);
        row("https://discord.com/invite/minecraft", S, "discord", C, "minecraft", "invite", 2);
        row("https://discord.com/channels/1/2", N, "", Z, "", "", 1);
        // YouTube's other forms
        row("https://www.youtube.com/live/jfKfPfyJRdk", S, "youtube-extra", C, "jfKfPfyJRdk", "live", 2);
        row("https://www.youtube.com/embed/jfKfPfyJRdk", S, "youtube-extra", C, "jfKfPfyJRdk", "embed", 2);
        row("https://youtube.com/clip/UgkxAbCdEfGhIjKlMnOpQrStUv", S, "youtube-extra", C, "UgkxAbCdEfGhIjKlMnOpQrStUv", "clip", 2);
        row("https://www.youtube.com/watch?v=jfKfPfyJRdk", LinkClass::YouTube, "", Kind::YouTube, "", "", 1);
        row("https://www.youtube.com/shorts/jfKfPfyJRdk", LinkClass::YouTube, "", Kind::YouTube, "", "", 1);
        row("https://youtube.com.evil.example.net/live/jfKfPfyJRdk", W, "", C, "", "", 2);
        // Exclude rows
        row("https://www.tiktok.com/@scout2015/video/6718335390845095173", N, "", Z, "", "", 1);
        row("https://vm.tiktok.com/ZMabc/", N, "", Z, "", "", 1);
        row("https://www.imdb.com/title/tt0111161/", N, "", Z, "", "", 1);
        row("https://www.amazon.de/dp/B000000000", N, "", Z, "", "", 1);
        row("https://amzn.to/3abc", N, "", Z, "", "", 1);
        row("https://www.google.com/maps/place/Eiffel+Tower/@48.8583701,2.2944813,17z", N, "", Z, "", "", 1);
        row("https://maps.google.de/maps?q=x", N, "", Z, "", "", 1);
        row("https://maps.app.goo.gl/AbCd", N, "", Z, "", "", 1);
        row("https://gfycat.com/", N, "", Z, "", "", 1);
        row("https://www.redgifs.com/watch/x", N, "", Z, "", "", 1);
        row("https://www.google.com/search?q=x", W, "", C, "", "", 2);
        // Look-alikes and international names
        row("https://xn--yutube-wqf.com/watch?v=dQw4w9WgXcQ", W, "", C, "", "", 2);
        row("https://giphy.com.evil.example.net/gifs/8FhXc8w45aN32", W, "", C, "", "", 2);
        row("https://thumbs.gfycat.com/x.gif", LinkClass::Direct, "", G, "", "", 1); // 2.4.0's rules come first
        row("https://evil.example.net/tenor.com/view/x-gif-123", W, "", C, "", "", 2);
        row("https://tenorr.com/view/x-gif-123", W, "", C, "", "", 2);
    }
    void providerMatches()
    {
        QFETCH(QString, href);
        QFETCH(int, cls);
        QFETCH(QString, provider);
        QFETCH(int, kind);
        QFETCH(QString, itemId);
        QFETCH(QString, sub);
        QFETCH(int, requests);
        const Target t = classify(href);
        QCOMPARE(int(t.cls), cls);
        QCOMPARE(int(t.kind), kind);
        if (t.isNewClass()) {
            QCOMPARE(t.provider, provider);
            QCOMPARE(t.itemId, itemId);
            QCOMPARE(t.sub, sub);
            QCOMPARE(t.expectedRequests, requests);
            QVERIFY(isKey(t.key));
            QCOMPARE(t.key, keyOf(a("page:") + t.page.toString(QUrl::FullyEncoded)));
            QCOMPARE(t.page.scheme(), a("https"));
            QVERIFY(!t.page.hasFragment());
            QVERIFY(net::isAllowedUrl(t.fetchUrl));
        }
        QCOMPARE(classifyCached(href).key, t.key);
    }

    void excludeRows()
    {
        // Every Exclude row: no preview, no request; the host is not a Web link either.
        for (const char* href : {"https://gfycat.com/x", "https://thumbs.gfycat.com/x", "https://redgifs.com/watch/x", "https://api.redgifs.com/v2/x",
                                 "https://tiktok.com/@a/video/1", "https://m.imdb.com/title/tt1/", "https://smile.amazon.com/x", "https://a.co/d/x",
                                 "https://amzn.eu/d/x", "https://amzn.asia/d/x", "https://google.com/maps", "https://www.google.co.uk/maps/@1,2,3z",
                                 "https://goo.gl/maps/AbC", "https://reddit.com/r/x/s/AbCd12", "https://twitch.tv/videos/1", "https://telegram.me/joinchat/x",
                                 "https://www.discord.com/channels/@me", "https://t.me/share/url?url=x", "https://t.me/addstickers/pack"})
            QVERIFY2(!classify(a(href)).isValid(), href);
        // goo.gl outside /maps/ and google.com outside /maps are plain pages.
        QCOMPARE(classify(a("https://goo.gl/other")).cls, LinkClass::Web);
    }

    void telegramService_data()
    {
        QTest::addColumn<QString>("segment");
        for (const char* s : {"login", "share", "proxy", "socks", "addstickers", "addemoji", "addtheme", "iv", "setlanguage", "confirmphone", "bg", "LOGIN", "Share"})
            QTest::newRow(s) << a(s);
    }
    void telegramService()
    {
        QFETCH(QString, segment);
        QVERIFY(!classify(a("https://t.me/") + segment).isValid());
        QVERIFY(!classify(a("https://t.me/") + segment + a("/123")).isValid());
        QVERIFY(!classify(a("https://www.t.me/") + segment).isValid());
    }

    void githubDownloads_data()
    {
        QTest::addColumn<QString>("href");
        QTest::newRow("release asset") << a("https://github.com/o/r/releases/download/v1/setup.exe");
        QTest::newRow("release zip") << a("https://github.com/o/r/releases/download/v1/x");
        QTest::newRow("archive") << a("https://github.com/o/r/archive/refs/heads/main.zip");
        QTest::newRow("archive tag") << a("https://github.com/o/r/archive/v1");
        QTest::newRow("raw") << a("https://github.com/o/r/raw/main/README");
        QTest::newRow("zipball") << a("https://github.com/o/r/zipball/main");
        QTest::newRow("tarball") << a("https://github.com/o/r/tarball/main");
        QTest::newRow("suites") << a("https://github.com/o/r/suites/1/artifacts/2");
        QTest::newRow("attachments") << a("https://github.com/o/r/attachments/x");
    }
    void githubDownloads()
    {
        QFETCH(QString, href);
        QVERIFY(!classify(href).isValid());
        QCOMPARE(classify(a("https://github.com/o/r/releases")).cls, LinkClass::Site);
        QCOMPARE(classify(a("https://github.com/o/r/releases/tag/v1")).cls, LinkClass::Site);
    }

    void providerPageActionSegments()
    {
        // A provider row that reads the link's own page never asks for a file or an action address.
        QVERIFY(!classify(a("https://github.com/o/r/blob/main/setup.exe")).isValid());
        QVERIFY(!classify(a("https://github.com/o/r/blob/main/doc.pdf")).isValid());
        QVERIFY(!classify(a("https://github.com/o/login/x")).isValid());
        QVERIFY(!classify(a("https://store.steampowered.com/app/730/download")).isValid());
        QVERIFY(!classify(a("https://staff.tumblr.com/post/123456789/confirm")).isValid());
        // Rows whose addresses the table builds (oEmbed, JSON, Derive) don't read the link's page: their shapes are
        // exact already.
        QVERIFY(classify(a("https://github.com/o/r/blob/main/README.md")).isValid());
    }

    void wikipediaDotTitles()
    {
        QVERIFY(!classify(a("https://en.wikipedia.org/wiki/%2E")).isValid());
        QVERIFY(!classify(a("https://en.wikipedia.org/wiki/%2E%2E")).isValid());
        QVERIFY(!classify(a("https://en.wikipedia.org/wiki/Special:Search")).isValid());
        QCOMPARE(classify(a("https://en.wikipedia.org/wiki/...")).cls, LinkClass::Site); // a real article ("...")
    }

    void simpleWikipedia()
    {
        const Target t = classify(a("https://simple.wikipedia.org/wiki/TeamSpeak"));
        QCOMPARE(t.cls, LinkClass::Site);
        QCOMPARE(t.provider, a("wikipedia"));
        QCOMPARE(t.sub, a("simple"));
        QCOMPARE(providers::firstRequestOf(t).host, a("simple.wikipedia.org"));
        const Target m = classify(a("https://fa.m.wikipedia.org/wiki/TeamSpeak"));
        QCOMPARE(providers::firstRequestOf(m).host, a("fa.wikipedia.org"));
    }

    void sTeamRedirect()
    {
        const Target t = classify(a("https://s.team/a/730"));
        QCOMPARE(t.cls, LinkClass::Site);
        QCOMPARE(t.provider, a("steam"));
        QCOMPARE(t.sub, a("short"));
        QCOMPARE(t.expectedRequests, 3);
        QVERIFY(providers::pageHopAllowed(a("steam"), QUrl(a("https://s.team/a/730"))));
        QVERIFY(providers::pageHopAllowed(a("steam"), QUrl(a("https://store.steampowered.com/app/730/"))));
        QVERIFY(!classify(a("https://s.team/chat/x")).isNewClass() || classify(a("https://s.team/chat/x")).cls == LinkClass::Web);
    }

    void youtubeExtra()
    {
        for (const char* href : {"https://www.youtube.com/live/jfKfPfyJRdk", "https://m.youtube.com/embed/jfKfPfyJRdk/",
                                 "https://www.youtube.com/clip/UgkxAbCdEfGhIjKlMnOpQrStUvWx"}) {
            const Target t = classify(a(href));
            QCOMPARE(t.cls, LinkClass::Site);
            QCOMPARE(t.provider, a("youtube-extra"));
            QVERIFY(!t.isLegacy()); // sent as LPX, never LP
        }
        QCOMPARE(classify(a("https://www.youtube.com/watch?v=jfKfPfyJRdk")).cls, LinkClass::YouTube);
        QCOMPARE(classify(a("https://youtu.be/jfKfPfyJRdk")).cls, LinkClass::YouTube);
        QCOMPARE(classify(a("https://www.youtube.com/shorts/jfKfPfyJRdk")).cls, LinkClass::YouTube);
        QCOMPARE(classify(a("https://www.youtube.com/playlist?list=PL1")).cls, LinkClass::Web);
    }

    void mediaFileRow()
    {
        const Target t = classify(a("http://x.example.com/cat.png"));
        QCOMPARE(t.cls, LinkClass::MediaPage);
        QCOMPARE(t.provider, a("media-file"));
        QCOMPARE(t.kind, Kind::Image);
        QCOMPARE(t.expectedRequests, 1);
        QCOMPARE(classify(a("http://x.example.com/a.GIF")).kind, Kind::Gif);
        QCOMPARE(classify(a("http://x.example.com/a.webm?x=1")).kind, Kind::Video);
        QCOMPARE(classify(a("http://www.gstatic.com/webp/gallery/1.jpg")).provider, a("media-file"));
        QCOMPARE(providers::firstRequestOf(t).lane, HostGate::Lane::Media);
        QVERIFY(providers::mediaHopAllowed(a("media-file"), QUrl(a("https://anything.example.org/x.png"))));
        // https media files stay Direct (2.4.0).
        QCOMPARE(classify(a("https://x.example.com/cat.png")).cls, LinkClass::Direct);
    }

    void joinerHrefs()
    {
        const QString raw     = persianWikiHref();
        const Target  fromRaw = classify(raw);
        QCOMPARE(fromRaw.cls, LinkClass::Site);
        QCOMPARE(fromRaw.provider, a("wikipedia"));
        QCOMPARE(fromRaw.key, a("557d5a7e4b885fb9215174f7839222ac"));
        QCOMPARE(fromRaw.key, classify(a("https://fa.wikipedia.org/wiki/%D8%AA%DB%8C%D9%85%E2%80%8C%D8%A7%D8%B3%D9%BE%DB%8C%DA%A9")).key);
        // In the host: never.
        QVERIFY(!classify(a("https://fa") + QChar(0x200C) + a("x.example.com/a")).isValid());
        // In the fragment: never.
        QVERIFY(!classify(a("https://example.com/a#b") + QChar(0x200C) + a("c")).isValid());
        // A media file with a joiner in its path: media-file, never a 2.4.0 Direct link.
        const Target media = classify(a("https://example.com/a") + QChar(0x200C) + a("b.png"));
        QCOMPARE(media.cls, LinkClass::MediaPage);
        QCOMPARE(media.provider, a("media-file"));
        QCOMPARE(media.page.toString(QUrl::FullyEncoded), a("https://example.com/a%E2%80%8Cb.png"));
        // Other zero-width and bidi characters stay refused.
        QVERIFY(!classify(a("https://example.com/a") + QChar(0x200B) + a("b")).isValid());
        QVERIFY(!classify(a("https://example.com/a") + QChar(0x202E) + a("b")).isValid());
        // A YouTube link with a joiner is no 2.4.0 link.
        QVERIFY(!classify(a("https://www.youtube.com/watch?v=dQw4w9WgXcQ&x=a") + QChar(0x200D) + a("b")).isLegacy());
    }

    void httpUpgrade()
    {
        const Target plain  = classify(a("http://www.teamspeak.com/en/"));
        const Target secure = classify(a("https://www.teamspeak.com/en/"));
        QCOMPARE(plain.cls, LinkClass::Web);
        QCOMPARE(plain.key, secure.key);
        QCOMPARE(plain.page, QUrl(a("https://www.teamspeak.com/en/")));
        QCOMPARE(plain.fetchUrl.scheme(), a("https"));
        QVERIFY(!classify(a("http://www.teamspeak.com:8080/en/")).isValid());
        QVERIFY(!classify(a("http://www.teamspeak.com:443/en/")).isValid());
        QVERIFY(!classify(a("https://www.teamspeak.com:80/en/")).isValid());
        QVERIFY(!classify(a("ftp://www.teamspeak.com/en/")).isValid());
        QVERIFY(!classify(a("https://user@www.teamspeak.com/en/")).isValid());
    }

    // ---- web links that never get a card ----------------------------------------------------------------------

    void actionLike_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<bool>("web");
        const auto stays = [](const char* href) { QTest::newRow(href) << a(href) << true; };
        const auto none  = [](const char* href) { QTest::newRow(href) << a(href) << false; };
        stays("https://www.bbc.com/news/articles/cm2d6kk0wzw4o");
        stays("https://docs.python.org/3/library/json.html");
        stays("https://www.example.com/news/russia-ukraine-war-latest-2024-11-09");
        stays("https://www.example.com/the-best-tech-gadgets-of-ces-2024-so-far");
        stays("https://medium.com/@someone/a-long-article-title-1a2b3c4d5e6f");
        stays("https://example.com/p/2024/11/09/a-title");
        stays("https://example.com/Donaudampfschifffahrtsgesellschaftskapitaen");
        stays("https://example.com/search?q=token+price");
        stays("https://example.com/?id=5&page=2");
        stays("https://example.com/a%E2%80%8Cb");
        none("https://example.com/verify?token=abc");
        none("https://example.com/unsubscribe/123");
        none("https://example.com/d/3f9a1c2e4b5d6a7f8e9d0c1b2a3f4e5d6c7b8a9f");
        none("https://example.com/s/Ab3dE6gH9jK2mN5pQ8rS1tU4");
        none("https://example.com/report.pdf");
        none("https://example.com/login");
        none("https://example.com/account/Sign-In");
        none("https://example.com/oauth2/authorize?x=1");
        none("https://example.com/x?access_token=1");
        none("https://example.com/x?API_KEY=1");
        none("https://example.com/x?sig=1&a=b");
        none("https://example.com/x?state=1&code=2");
        none("https://example.com/files/downloads/a");
        none("https://example.com/dl/abc");
        none("https://example.com/a/0123456789abcdef0123456789abcdef");
    }
    void actionLike()
    {
        QFETCH(QString, href);
        QFETCH(bool, web);
        const Target t = classify(href);
        QCOMPARE(t.cls == LinkClass::Web, web);
        if (!web)
            QVERIFY(!t.isValid());
        QCOMPARE(classify(a("https://github.com/torvalds/linux")).cls, LinkClass::Site); // provider rows are exempt from the token rules
    }

    void nonHtmlExtensions()
    {
        for (const char* ext : {"pdf", "zip", "rar", "7z", "exe", "msi", "dmg", "apk", "ipa", "iso", "img", "tar", "gz", "tgz", "bz2", "xz", "zst",
                                "mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt",
                                "ods", "odp", "rtf", "txt", "csv", "tsv", "json", "xml", "svg", "svgz", "bin", "torrent", "jar", "deb", "rpm",
                                "appimage", "ts", "m3u8", "mpd", "avi", "mkv", "wmv", "flv", "PDF", "Zip"}) {
            const QString href = a("https://example.com/files/file.") + a(ext);
            QVERIFY2(!classify(href).isValid(), qPrintable(href));
            QVERIFY(hasNonHtmlExtension(QUrl(href)));
        }
        for (const char* ok : {"https://example.com/a.html", "https://example.com/a.php", "https://example.com/a.aspx", "https://example.com/a.htm",
                               "https://example.com/v1.2/notes", "https://example.com/"})
            QCOMPARE(classify(a(ok)).cls, LinkClass::Web);
    }

    void trimLinkEnd_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("paren kept") << a("https://en.wikipedia.org/wiki/Foo_(bar)") << a("https://en.wikipedia.org/wiki/Foo_(bar)");
        QTest::newRow("paren dot") << a("https://x.com/a).") << a("https://x.com/a");
        QTest::newRow("dot") << a("https://x.com/a.") << a("https://x.com/a");
        QTest::newRow("bang") << a("https://x.com/a!") << a("https://x.com/a");
        QTest::newRow("quote") << a("https://x.com/a'") << a("https://x.com/a");
        QTest::newRow("comma") << a("https://x.com/a,") << a("https://x.com/a");
        QTest::newRow("many") << a("https://x.com/a?!.,;:") << a("https://x.com/a");
        QTest::newRow("balanced twice") << a("https://x.com/((x))") << a("https://x.com/((x))");
        QTest::newRow("paren in middle") << a("https://x.com/(a)b)") << a("https://x.com/(a)b");
        QTest::newRow("paren then dot kept") << a("https://x.com/Foo_(bar).") << a("https://x.com/Foo_(bar)");
        QTest::newRow("nothing") << a("https://x.com/a") << a("https://x.com/a");
        QTest::newRow("empty") << QString() << QString();
    }
    void trimLinkEnd()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(linkpreview::trimLinkEnd(in), out);
        QCOMPARE(linkpreview::trimLinkEnd(out), out);
    }

    void selectLinks_data()
    {
        QTest::addColumn<QStringList>("hrefs");
        QTest::addColumn<QStringList>("chosen");
        const QString yt1 = a("https://youtu.be/aaaaaaaaaaa"), yt2 = a("https://youtu.be/bbbbbbbbbbb"), yt3 = a("https://youtu.be/ccccccccccc"),
                      yt4 = a("https://youtu.be/ddddddddddd");
        const QString w1 = a("https://example.com/1"), w2 = a("https://example.com/2"), w3 = a("https://example.com/3"), w4 = a("https://example.com/4");
        const QString png = a("https://example.com/a.png"), vimeo = a("https://vimeo.com/76979871");
        QTest::newRow("legacy first") << QStringList{w1, w2, yt1, w3} << QStringList{w1, w2, yt1};
        QTest::newRow("three legacy win") << QStringList{w1, yt1, w2, yt2, yt3} << QStringList{yt1, yt2, yt3};
        QTest::newRow("four legacy") << QStringList{yt1, yt2, yt3, yt4} << QStringList{yt1, yt2, yt3};
        QTest::newRow("web only") << QStringList{w1, w2, w3, w4} << QStringList{w1, w2, w3};
        QTest::newRow("mixed order kept") << QStringList{vimeo, png, w1, w2} << QStringList{vimeo, png, w1};
        QTest::newRow("two legacy one web") << QStringList{w1, w2, png, yt1} << QStringList{w1, png, yt1};
        QTest::newRow("empty") << QStringList{} << QStringList{};
    }
    void selectLinks()
    {
        QFETCH(QStringList, hrefs);
        QFETCH(QStringList, chosen);
        QVector<Target> in;
        for (const QString& href : hrefs)
            in << classify(href);
        QStringList keys;
        for (const Target& t : linkpreview::selectLinks(in))
            keys << t.key;
        QStringList expected;
        for (const QString& href : chosen)
            expected << classify(href).key;
        QCOMPARE(keys, expected);
    }

    void trackingParams_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("fetch");
        QTest::newRow("utm") << a("https://example.com/a?utm_source=x&b=1&UTM_Medium=y") << a("https://example.com/a?b=1");
        QTest::newRow("fbclid only") << a("https://example.com/a?fbclid=abc") << a("https://example.com/a");
        QTest::newRow("several") << a("https://example.com/a?gclid=1&x=%2F&igshid=2&ref_src=t&mc_eid=9") << a("https://example.com/a?x=%2F");
        QTest::newRow("spotify si") << a("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT?si=abcdef0123456789")
                                    << a("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT");
        QTest::newRow("si elsewhere kept") << a("https://example.com/a?si=1") << a("https://example.com/a?si=1");
        QTest::newRow("order kept") << a("https://example.com/a?z=1&utm_x=2&a=3") << a("https://example.com/a?z=1&a=3");
    }
    void trackingParams()
    {
        QFETCH(QString, href);
        QFETCH(QString, fetch);
        const Target t = classify(href);
        QVERIFY(t.isNewClass());
        QCOMPARE(t.fetchUrl.toString(QUrl::FullyEncoded), fetch);
        QCOMPARE(t.page, normalizedPage(QUrl(href))); // the key and the open address keep everything
        QCOMPARE(t.key, keyOf(a("page:") + normalizedPage(QUrl(href)).toString(QUrl::FullyEncoded)));
    }

    void tenorMediaId()
    {
        QCOMPARE(providers::tenorMediaId(11594218182993649802ULL), a("oObwAjW2IIo"));
        QCOMPARE(providers::tenorMediaId(19787475173016375ULL), a("AEZMmpSwgzc"));
        QCOMPARE(providers::tenorMediaId(0), QString());
        QCOMPARE(providers::tenorMediaId(1).size(), 11);
    }

    void inactiveRowFallsToWeb()
    {
        QCOMPARE(classify(a("https://vimeo.com/76979871")).cls, LinkClass::Site);
        providers::setInactiveForTests({a("vimeo"), a("tenor")});
        QVERIFY(!providers::isActive(a("vimeo")));
        QCOMPARE(classify(a("https://vimeo.com/76979871")).cls, LinkClass::Web);
        QCOMPARE(classify(a("https://tenor.com/view/x-gif-11594218182993649802")).cls, LinkClass::Web);
        // The key never depends on the class.
        QCOMPARE(classify(a("https://vimeo.com/76979871")).key, keyOf(a("page:https://vimeo.com/76979871")));
        providers::setInactiveForTests({});
        QVERIFY(providers::isActive(a("vimeo")));
    }

    // Live test 2026-11-10: cdn-images.dzcdn.net answered 403 to every client. The author switched Deezer off (no card
    // attempt); its rows and plan stay (switched on again for tests: tst_linkpage_net deezerCard).
    void deezerSwitchedOff()
    {
        QVERIFY(!providers::isActive(a("deezer")));
        for (const char* href : {"https://www.deezer.com/track/14408104", "https://deezer.com/fr/album/302127", "https://link.deezer.com/s/30AbCdEfGh"}) {
            const Target t = classify(a(href));
            QCOMPARE(t.cls, LinkClass::None); // no preview, no request (never a Web card asking the same servers)
            QCOMPARE(t.kind, Kind::None);
        }
        QCOMPARE(classify(a("https://www.deezer.com/en/")).cls, LinkClass::Web); // not a row's link: a web page, as before
        QVERIFY(providers::hasHopLists(a("deezer")));
        providers::setActiveForTests({a("deezer")});
        QVERIFY(providers::isActive(a("deezer")));
        const Target on = classify(a("https://www.deezer.com/track/14408104"));
        QCOMPARE(on.cls, LinkClass::Site);
        QCOMPARE(on.provider, a("deezer"));
        QCOMPARE(classify(a("https://link.deezer.com/s/30AbCdEfGh")).sub, a("short"));
        providers::setActiveForTests({});
        QCOMPARE(classify(a("https://www.deezer.com/track/14408104")).cls, LinkClass::None);
        // Only Deezer: another switched-off row still falls to Web (D39).
        providers::setInactiveForTests({a("dailymotion")});
        QCOMPARE(classify(a("https://www.dailymotion.com/video/xbv3lcm")).cls, LinkClass::Web);
    }

    // Klipy's GIF pages: the wall's fallback request and what its oEmbed answer may name (author's decision 2026-11-11).
    void klipyWallFallback()
    {
        const Target gif = classify(a("https://klipy.com/gifs/andy-samberg-awkward?utm_source=x"));
        QCOMPARE(gif.provider, a("klipy"));
        QCOMPARE(gif.sub, a("gifs"));
        const providers::FirstRequest fallback = providers::wallFallbackOf(gif);
        QCOMPARE(fallback.host, a("api.klipy.com"));
        QCOMPARE(fallback.lane, HostGate::Lane::Page);
        QCOMPARE(fallback.firstSegment, a("api"));
        QCOMPARE(providers::firstRequestOf(gif).firstSegment, a("gifs")); // the page first, as before
        // The link rebuilt from its slug: no query, no www.
        QCOMPARE(providers::klipyOEmbedUrl(gif.itemId).toString(QUrl::FullyEncoded),
                 a("https://api.klipy.com/api/v1/web/gifs/embed/oembed?format=json&url=https%3A%2F%2Fklipy.com%2Fgifs%2Fandy-samberg-awkward"));
        QCOMPARE(providers::wallFallbackOf(classify(a("https://www.klipy.com/gifs/andy-samberg-awkward"))).host, a("api.klipy.com"));
        QVERIFY(providers::pageHopAllowed(a("klipy"), providers::klipyOEmbedUrl(gif.itemId)));
        // Only the GIF pages: clips, stickers and other sites have none.
        for (const char* href : {"https://klipy.com/clips/angry-nod", "https://klipy.com/stickers/yes-excited-yes-sticker", "https://tenor.com/view/x-gif-11594218182993649802",
                                 "https://www.example.com/gifs/x", "https://klipy.com/gifs/"})
            QVERIFY2(providers::wallFallbackOf(classify(a(href))).host.isEmpty(), href);
    }

    void klipyOEmbedMedia_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("expected"); // empty: no GIF
        const auto answer = [](const char* thumbnail) {
            // The shape of api.klipy.com's answer (2026-11-11), with this thumbnail_url.
            return QByteArray("{\"type\":\"video\",\"version\":\"1.0\",\"title\":\"Andy Samberg Awkward\",\"author_name\":\"KLIPY\",\"provider_name\":\"KLIPY\","
                              "\"cache_age\":60,\"thumbnail_url\":\"")
                   + thumbnail
                   + "\",\"thumbnail_width\":392,\"thumbnail_height\":498,\"width\":600,\"height\":600,"
                     "\"html\":\"<iframe src=\\\"https:\\/\\/klipy.com\\/gifs\\/andy-samberg-awkward\\/player\\\"><\\/iframe>\"}";
        };
        QTest::newRow("broken 2026-11-11") << answer("https:\\/\\/static2.klipy.com\\/") << QString();
        QTest::newRow("root without slash") << answer("https://static2.klipy.com") << QString();
        QTest::newRow("only slashes") << answer("https://static2.klipy.com//") << QString();
        QTest::newRow("fixed gif") << answer("https:\\/\\/static2.klipy.com\\/ii\\/4e7bea9f\\/3c\\/8a\\/andy.gif") << a("https://static2.klipy.com/ii/4e7bea9f/3c/8a/andy.gif");
        QTest::newRow("fixed webp static") << answer("https://static.klipy.com/ii/x/andy.webp") << a("https://static.klipy.com/ii/x/andy.webp");
        QTest::newRow("static7") << answer("https://static7.klipy.com/a.gif") << a("https://static7.klipy.com/a.gif");
        QTest::newRow("query kept") << answer("https://static2.klipy.com/a.gif?v=2") << a("https://static2.klipy.com/a.gif?v=2");
        QTest::newRow("http never upgraded") << answer("http://static2.klipy.com/a.gif") << QString();
        QTest::newRow("protocol relative") << answer("//static2.klipy.com/a.gif") << QString();
        QTest::newRow("relative") << answer("/a.gif") << QString();
        QTest::newRow("other klipy host") << answer("https://klipy.com/gifs/andy-samberg-awkward/player") << QString();
        QTest::newRow("api host") << answer("https://api.klipy.com/a.gif") << QString();
        QTest::newRow("static letters") << answer("https://static-cdn.klipy.com/a.gif") << QString();
        QTest::newRow("look-alike") << answer("https://static2.klipy.com.evil.example.net/a.gif") << QString();
        QTest::newRow("other site") << answer("https://media.tenor.com/a.gif") << QString();
        QTest::newRow("user info") << answer("https://user@static2.klipy.com/a.gif") << QString();
        QTest::newRow("port") << answer("https://static2.klipy.com:8443/a.gif") << QString();
        QTest::newRow("svg") << answer("https://static2.klipy.com/a.svg") << QString();
        QTest::newRow("bidi") << answer("https://static2.klipy.com/a\\u202egif.png") << QString();
        QTest::newRow("empty") << answer("") << QString();
        QTest::newRow("no thumbnail") << QByteArray("{\"type\":\"video\",\"title\":\"x\"}") << QString();
        QTest::newRow("thumbnail not a string") << QByteArray("{\"thumbnail_url\":42}") << QString();
        QTest::newRow("array") << QByteArray("[\"https://static2.klipy.com/a.gif\"]") << QString();
        QTest::newRow("not json") << QByteArray("<html><title>Just a moment...</title></html>") << QString();
        QTest::newRow("empty body") << QByteArray() << QString();
    }
    void klipyOEmbedMedia()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, expected);
        const QUrl url = providers::klipyOEmbedMedia(json);
        if (expected.isEmpty()) {
            QVERIFY2(!url.isValid(), qPrintable(url.toString()));
        } else {
            QCOMPARE(url.toString(QUrl::FullyEncoded), expected);
            QVERIFY(providers::mediaHopAllowed(a("klipy"), url));
        }
    }

    void brands()
    {
        for (const char* id : {"vimeo", "soundcloud", "spotify", "applemusic", "deezer", "dailymotion", "medal", "twitch", "kick", "bluesky", "x", "github",
                               "steam", "wikipedia", "telegram", "discord", "reddit", "youtube-extra", "tumblr"})
            QVERIFY2(providers::brandOf(a(id)).name, id);
        QVERIFY(!providers::brandOf(a("tenor")).name);
        QVERIFY(!providers::brandOf(a("unknown")).name);
        QCOMPARE(QString::fromLatin1(providers::brandOf(a("youtube-extra")).name), a("YouTube"));
    }

    void brandsReadable()
    {
        const QColor light = previewpaint::palette(false).background;
        const QColor dark  = previewpaint::palette(true).background;
        for (const char* id : {"vimeo", "soundcloud", "spotify", "applemusic", "deezer", "dailymotion", "medal", "twitch", "kick", "bluesky", "x", "github",
                               "steam", "wikipedia", "telegram", "discord", "reddit", "youtube-extra", "tumblr"}) {
            const providers::Brand brand = providers::brandOf(a(id));
            const QColor           onLight = ui::readableOn(QColor::fromRgb(brand.light), {light}, 3.0);
            const QColor           onDark  = ui::readableOn(QColor::fromRgb(brand.dark), {dark}, 3.0);
            QVERIFY2(ui::contrastRatio(onLight, light) >= 3.0, id);
            QVERIFY2(ui::contrastRatio(onDark, dark) >= 3.0, id);
        }
    }

    void hopAllowlistsComplete()
    {
        for (const QString& id : providers::ids()) {
            if (id == QLatin1String("media-file") || !providers::isActive(id))
                continue;
            const Target probe = [&id]() {
                Target t;
                t.provider = id;
                return t;
            }();
            Q_UNUSED(probe)
            // Exclude rows make no request: they need no lists.
            static const QStringList kExcluded = {a("gfycat"), a("redgifs"), a("tiktok"), a("imdb"), a("amazon"), a("googlemaps"), a("reddit-share"),
                                                  a("twitch-vod"), a("telegram-private"), a("telegram-service"), a("discord-channels")};
            if (kExcluded.contains(id))
                continue;
            QVERIFY2(providers::hasHopLists(id), qPrintable(id));
        }
        QVERIFY(!providers::pageHopAllowed(a("no-such-row"), QUrl(a("https://example.com/"))));
        QVERIFY(providers::pageHopAllowed(QString(), QUrl(a("https://example.com/")))); // the Web class: any allowed host
    }

    void hopLists()
    {
        QVERIFY(providers::mediaHopAllowed(a("klipy"), QUrl(a("https://static3.klipy.com/x.mp4"))));
        QVERIFY(providers::mediaHopAllowed(a("klipy"), QUrl(a("https://static.klipy.com/x.mp4"))));
        QVERIFY(!providers::mediaHopAllowed(a("klipy"), QUrl(a("https://static.klipy.com.evil.example/x.mp4"))));
        QVERIFY(!providers::pageHopAllowed(a("klipy"), QUrl(a("https://static.klipy.com/x"))));
        QVERIFY(providers::mediaHopAllowed(a("streamable"), QUrl(a("https://cdn-cf-east.streamable.com/video/x.mp4"))));
        QVERIFY(!providers::mediaHopAllowed(a("streamable"), QUrl(a("https://streamable.com.evil.example/x.mp4"))));
        QVERIFY(!providers::pageHopAllowed(a("streamable"), QUrl(a("https://streamable.com/x")))); // no page requests
        QVERIFY(providers::pageHopAllowed(a("pinterest"), QUrl(a("https://www.pinterest.de/pin/1/"))));
        QVERIFY(providers::pageHopAllowed(a("wikipedia"), QUrl(a("https://de.wikipedia.org/wiki/X"))));
        QVERIFY(providers::pageHopAllowed(a("tumblr"), QUrl(a("https://someblog.tumblr.com/post/1"))));
        QVERIFY(!providers::pageHopAllowed(a("tumblr"), QUrl(a("https://api.tumblr.com/post/1"))));
        QVERIFY(providers::mediaHopAllowed(a("tumblr"), QUrl(a("https://64.media.tumblr.com/x.jpg"))));
        QVERIFY(providers::mediaHopAllowed(a("vimeo"), QUrl(a("https://i.vimeocdn.com/video/1.jpg"))));
        QVERIFY(!providers::mediaHopAllowed(a("vimeo"), QUrl(a("https://vimeo.com.attacker.example/x.jpg"))));
        QVERIFY(!providers::pageHopAllowed(a("vimeo"), QUrl(a("https://vimeo.com.attacker.example/api/oembed.json"))));
        QVERIFY(providers::mediaHopAllowed(a("soundcloud"), QUrl(a("https://i1.sndcdn.com/a.jpg"))));
        QVERIFY(providers::mediaHopAllowed(a("spotify"), QUrl(a("https://image-cdn-ak.spotifycdn.com/image/x"))));
        QVERIFY(providers::mediaHopAllowed(a("telegram"), QUrl(a("https://cdn4.telesco.pe/file/x.jpg"))));
        QVERIFY(providers::mediaHopAllowed(a("dailymotion"), QUrl(a("https://s1.dmcdn.net/v/x/x720"))));
        QVERIFY(providers::mediaHopAllowed(a("twitch"), QUrl(a("https://static-cdn.jtvnw.net/x.jpg"))));
        QVERIFY(providers::mediaHopAllowed(a("dropbox"), QUrl(a("https://uc1234.dl.dropboxusercontent.com/x"))));
        QVERIFY(providers::mediaHopAllowed(a("media-file"), QUrl(a("https://any.example.net/x.png"))));
    }

    void firstRequestOf_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("lane");
        QTest::addColumn<QString>("segment");
        const int P = int(HostGate::Lane::Page), D = int(HostGate::Lane::Media);
        const auto row = [](const char* href, const char* host, int lane, const char* segment) { QTest::newRow(href) << a(href) << a(host) << lane << a(segment); };
        row("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "www.youtube.com", P, "oembed");
        row("https://example.com/a.png", "example.com", D, "a.png");
        row("https://tenor.com/bTSmr.gif", "tenor.com", P, "btsmr.gif");
        row("https://tenor.com/view/cat-gif-11594218182993649802", "media.tenor.com", D, "");
        row("https://tenor.com/view/cat-gif-25670993", "tenor.com", P, "oembed");
        row("https://giphy.com/gifs/x-8FhXc8w45aN32", "i.giphy.com", D, "");
        row("https://gph.is/2KH8MQ0", "gph.is", P, "2kh8mq0");
        row("https://klipy.com/gifs/andy-samberg-awkward", "klipy.com", P, "gifs");
        row("https://klipy.com/clips/angry-nod", "klipy.com", P, "clips");
        row("https://imgur.com/a/bcOmNQt", "imgur.com", P, "a");
        row("https://i.imgur.com/Rum0zSz.gifv", "i.imgur.com", D, "");
        row("https://v.redd.it/if97zvl1wgxh1", "v.redd.it", P, "if97zvl1wgxh1");
        row("https://www.reddit.com/r/gifs/comments/ftwv49", "www.reddit.com", P, "comments");
        row("https://vimeo.com/76979871", "vimeo.com", P, "api");
        row("https://open.spotify.com/intl-de/track/4cOdK2wGLETKBW3PvgPWqT", "open.spotify.com", P, "track");
        row("https://twitter.com/jack/status/20", "x.com", P, "jack");
        row("https://discord.gg/minecraft", "discord.com", P, "api");
        row("https://en.wikipedia.org/wiki/TeamSpeak", "en.wikipedia.org", P, "api");
        row("https://www.teamspeak.com/en/", "www.teamspeak.com", P, "en");
        row("http://x.example.com/cat.png", "x.example.com", D, "cat.png");
        row("https://streamable.com/1hzf2p", "api-f.streamable.com", D, "api");
    }
    void firstRequestOf()
    {
        QFETCH(QString, href);
        QFETCH(QString, host);
        QFETCH(int, lane);
        QFETCH(QString, segment);
        const providers::FirstRequest first = providers::firstRequestOf(classify(href));
        QCOMPARE(first.host, host);
        QCOMPARE(int(first.lane), lane);
        QCOMPARE(first.firstSegment, segment);
    }

    void siteNameMatches_data()
    {
        QTest::addColumn<QString>("siteName");
        QTest::addColumn<QString>("host");
        QTest::addColumn<bool>("matches");
        QTest::newRow("guardian") << a("The Guardian") << a("www.theguardian.com") << true;
        QTest::newRow("guardian word") << a("Guardian") << a("guardian.co.uk") << true;
        QTest::newRow("paypal pp.lc") << a("PayPal Support") << a("pp.lc") << false;
        QTest::newRow("youtube elsewhere") << a("YouTube") << a("evil.example.net") << false;
        QTest::newRow("bbc") << a("BBC") << a("www.bbc.co.uk") << true;
        QTest::newRow("bbc news") << a("BBC News") << a("www.bbc.com") << false;
        QTest::newRow("discord nitro") << a("Discord Nitro") << a("discord-nitro.gift") << true;
        QTest::newRow("github") << a("GitHub") << a("github.com") << true;
        QTest::newRow("mid word no") << a("Theguardianx") << a("theguardian.com") << false;
        QTest::newRow("heise") << a("heise online") << a("www.heise.de") << true;
        QTest::newRow("empty") << QString() << a("example.com") << false;
    }
    void siteNameMatches()
    {
        QFETCH(QString, siteName);
        QFETCH(QString, host);
        QFETCH(bool, matches);
        QCOMPARE(providers::siteNameMatches(siteName, host), matches);
    }

    // ---- netpolicy (2.5) ----------------------------------------------------------------------------------------

    void registrableDomain_data()
    {
        QTest::addColumn<QString>("host");
        QTest::addColumn<QString>("domain");
        QTest::newRow("plain") << a("www.example.com") << a("example.com");
        QTest::newRow("two labels") << a("example.com") << a("example.com");
        QTest::newRow("co.uk") << a("www.bbc.co.uk") << a("bbc.co.uk");
        QTest::newRow("co.uk deep") << a("a.b.bbc.co.uk") << a("bbc.co.uk");
        QTest::newRow("github.io") << a("user.github.io") << a("user.github.io");
        QTest::newRow("github.io itself") << a("github.io") << a("github.io");
        QTest::newRow("s3") << a("bucket.s3.amazonaws.com") << a("bucket.s3.amazonaws.com");
        QTest::newRow("s3 itself") << a("s3.amazonaws.com") << a("s3.amazonaws.com");
        QTest::newRow("cdn") << a("i.imgur.com") << a("imgur.com");
        QTest::newRow("com.au") << a("www.abc.net.au") << a("abc.net.au");
        QTest::newRow("ir") << a("www.digikala.com") << a("digikala.com");
        QTest::newRow("co.ir") << a("shop.example.co.ir") << a("example.co.ir");
        QTest::newRow("trailing dot") << a("www.example.com.") << a("example.com");
        QTest::newRow("upper") << a("WWW.Example.COM") << a("example.com");
        QTest::newRow("discord cdn") << a("cdn.discordapp.com") << a("discordapp.com");
    }
    void registrableDomain()
    {
        QFETCH(QString, host);
        QFETCH(QString, domain);
        QCOMPARE(net::registrableDomain(host), domain);
    }

    void resolveExtracted_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("expected"); // empty: refused
        const auto row = [](const char* name, const QString& raw, const char* expected) { QTest::newRow(name) << raw << a(expected); };
        row("loopback", a("https://127.0.0.1/x.png"), "");
        row("router.lan", a("https://router.lan/x.png"), "");
        row("ipv6", a("https://[::1]/x.png"), "");
        row("decimal", a("https://2130706433/x.png"), "");
        row("hex", a("https://0x7f.1/x.png"), "");
        row("single label", a("https://intranet/x.png"), "");
        row("file", a("file://server/share/x.png"), "");
        row("data svg", a("data:image/svg+xml;base64,PHN2Zz48L3N2Zz4="), "");
        row("javascript", a("javascript:alert(1)"), "");
        row("blob", a("blob:https://example.com/x"), "");
        row("ftp", a("ftp://example.com/x.png"), "");
        row("userinfo", a("https://user:pass@x.com/"), "");
        row("at in authority", a("//x.com@evil.example.com/a.png"), "");
        row("backslash", a("https://x.com\\@evil.example.com/a.png"), "");
        row("port", a("https://cdn.example.com:8443/x.png"), "");
        row("http port 81", a("http://cdn.example.com:81/x.png"), "");
        row("protocol relative", a("//cdn.example.com/x.png"), "https://cdn.example.com/x.png");
        row("http upgraded", a("http://x.com/a.jpg"), "https://x.com/a.jpg");
        row("http port 80", a("http://x.com:80/a.jpg"), "https://x.com/a.jpg");
        row("relative", a("/rel/x.png"), "https://base.example.org/rel/x.png");
        row("relative dot", a("../x.png"), "https://base.example.org/x.png");
        row("fragment dropped", a("https://x.com/a.jpg#frag"), "https://x.com/a.jpg");
        row("svg", a("https://x.com/x.svg"), "");
        row("svgz upper", a("https://x.com/x.SVGZ?a=1"), "");
        row("tab cr lf", a(" https://x.com/a\t\r\nb.jpg "), "https://x.com/ab.jpg");
        row("bidi", a("https://x.com/a") + QChar(0x202E) + a("b.jpg"), "");
        row("zero width", a("https://x.com/a") + QChar(0x200B) + a("b.jpg"), "");
        row("too long", a("https://x.com/") + QString(2049, QLatin1Char('a')), "");
        row("empty", QString(), "");
        row("local suffix name", a("https://printer.local/x.png"), "");
    }
    void resolveExtracted()
    {
        QFETCH(QString, raw);
        QFETCH(QString, expected);
        const QUrl url = net::resolveExtracted(QUrl(a("https://base.example.org/dir/page.html")), raw);
        if (expected.isEmpty())
            QVERIFY2(!url.isValid() || url.isEmpty(), qPrintable(url.toString()));
        else
            QCOMPARE(url.toString(QUrl::FullyEncoded), expected);
    }

    void parseRetryAfter_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<qint64>("ms");
        QTest::newRow("120") << QByteArray("120") << qint64(120000);
        QTest::newRow("date") << QByteArray("Wed, 21 Oct 2026 07:28:00 GMT") << qint64(90000);
        QTest::newRow("past date") << QByteArray("Wed, 21 Oct 2026 07:20:00 GMT") << qint64(0);
        QTest::newRow("negative") << QByteArray("-5") << qint64(-1);
        QTest::newRow("abc") << QByteArray("abc") << qint64(-1);
        QTest::newRow("99999") << QByteArray("99999") << qint64(99999000);
        QTest::newRow("zero") << QByteArray("0") << qint64(0);
        QTest::newRow("spaces") << QByteArray(" 30 ") << qint64(30000);
        QTest::newRow("ten digits") << QByteArray("1234567890") << qint64(-1);
        QTest::newRow("empty") << QByteArray() << qint64(-1);
    }
    void parseRetryAfter()
    {
        QFETCH(QByteArray, value);
        QFETCH(qint64, ms);
        // "now" is 2026-10-21 07:26:30 UTC
        const qint64 now = QDateTime(QDate(2026, 10, 21), QTime(7, 26, 30), Qt::UTC).toMSecsSinceEpoch();
        QCOMPARE(net::parseRetryAfter(value, now), ms);
    }

    void parseRateReset_data()
    {
        QTest::addColumn<QByteArray>("resetAfter");
        QTest::addColumn<QByteArray>("reset");
        QTest::addColumn<qint64>("ms");
        QTest::newRow("reddit") << QByteArray() << QByteArray("43") << qint64(43000);
        QTest::newRow("unix") << QByteArray() << QByteArray("1762700000.5") << qint64(10500);
        QTest::newRow("reset-after wins") << QByteArray("1.25") << QByteArray("43") << qint64(1250);
        QTest::newRow("minus one") << QByteArray() << QByteArray("-1") << qint64(-1);
        QTest::newRow("abc") << QByteArray() << QByteArray("abc") << qint64(-1);
        QTest::newRow("86401") << QByteArray() << QByteArray("86401") << qint64(-1);
        QTest::newRow("unix past") << QByteArray() << QByteArray("1762600000") << qint64(-1);
        QTest::newRow("absent") << QByteArray() << QByteArray() << qint64(-1);
        QTest::newRow("bad after, good reset") << QByteArray("x") << QByteArray("5") << qint64(5000);
    }
    void parseRateReset()
    {
        QFETCH(QByteArray, resetAfter);
        QFETCH(QByteArray, reset);
        QFETCH(qint64, ms);
        QCOMPARE(net::parseRateReset(resetAfter, reset, 1762699990000LL), ms);
    }

    void expiredSignedLink()
    {
        const qint64 now = 0x67000000;
        QVERIFY(net::expiredSignedLink(QUrl(a("https://cdn.discordapp.com/attachments/1/2/a.png?ex=66000000&is=1&hm=2")), now));
        QVERIFY(!net::expiredSignedLink(QUrl(a("https://cdn.discordapp.com/attachments/1/2/a.png?ex=68000000&is=1")), now));
        QVERIFY(!net::expiredSignedLink(QUrl(a("https://cdn.discordapp.com/attachments/1/2/a.png?ex=66ffffd0")), now)); // within 60 s
        QVERIFY(net::expiredSignedLink(QUrl(a("https://media.discordapp.net/attachments/1/2/a.png?ex=1")), now));
        QVERIFY(!net::expiredSignedLink(QUrl(a("https://example.com/a.png?ex=1")), now));
        QVERIFY(!net::expiredSignedLink(QUrl(a("https://cdn.discordapp.com/a.png")), now));
        QVERIFY(!net::expiredSignedLink(QUrl(a("https://cdn.discordapp.com/a.png?ex=zz")), now));
    }

    void localSuffixGuard()
    {
        const QStringList suffixes = {a("corp.example.com"), a("ad.contoso.org")};
        QVERIFY(net::underLocalSuffix(a("wiki.corp.example.com"), suffixes));
        QVERIFY(net::underLocalSuffix(a("corp.example.com"), suffixes));
        QVERIFY(net::underLocalSuffix(a("X.AD.Contoso.org"), suffixes));
        QVERIFY(!net::underLocalSuffix(a("notcorp.example.com"), suffixes));
        QVERIFY(!net::underLocalSuffix(a("example.com"), suffixes));
        QVERIFY(!net::underLocalSuffix(a("www.example.com"), {}));
        // The PC's own list: readable, never a whole top-level domain.
        for (const QString& suffix : net::localDnsSuffixes())
            QVERIFY2(suffix.contains(QLatin1Char('.')), qPrintable(suffix));
    }

    void userAgent()
    {
        const QStringList parts = QString::fromLatin1(TSMEDIA_VERSION).split(QLatin1Char('.'));
        const QByteArray  ua    = net::userAgent();
        QCOMPARE(ua, QByteArray("Mozilla/5.0 (compatible; TSMediaChat/") + (parts.value(0) + QLatin1Char('.') + parts.value(1)).toLatin1()
                         + QByteArray("; +https://github.com/Metihttp/Teamspeak_Media_chat)"));
        QVERIFY(!ua.contains("Windows"));
    }

    // ---- speed ----------------------------------------------------------------------------------------------------

    void classifyFourThousandBlocks()
    {
        // 8000 hrefs of mixed providers, as the chat classifies them for a long history (4000 blocks, 2 links each).
        const char* const kinds[] = {"https://tenor.com/view/cat-gif-%1", "https://giphy.com/gifs/x-8FhXc8w45a%1", "https://www.reddit.com/r/gifs/comments/a%1",
                                     "https://vimeo.com/7697%1", "https://example.com/news/article-%1", "https://www.youtube.com/watch?v=dQw4w9W%1",
                                     "https://cdn.example.net/p/%1.png", "https://en.wikipedia.org/wiki/Topic_%1", "https://github.com/user%1/repo",
                                     "https://x.com/someone/status/%1"};
        QStringList hrefs;
        for (int i = 0; i < 8000; ++i)
            hrefs << QString::fromLatin1(kinds[i % 10]).arg(1000 + i);
        QElapsedTimer timer;
        timer.start();
        int valid = 0;
        for (const QString& href : qAsConst(hrefs))
            valid += classify(href).isValid() ? 1 : 0;
        const qint64 cold = timer.elapsed();
        // The cache holds kClassifyCacheSize hrefs: a chat's 4000 anchors stay in it.
        const QStringList chat = hrefs.mid(0, 4000);
        for (const QString& href : chat)
            classifyCached(href);
        timer.restart();
        for (int round = 0; round < 2; ++round) {
            for (const QString& href : chat)
                classifyCached(href);
        }
        const qint64 warm = timer.elapsed();
        qInfo("classifyFourThousandBlocks: 8000 hrefs cold %lld ms; 8000 cached lookups (4000 hrefs twice) %lld ms; %d previewable", cold, warm, valid);
        QVERIFY(valid > 6000);
#ifdef NDEBUG
        QVERIFY2(cold < 250, qPrintable(QString::number(cold)));
        QVERIFY2(warm < 40, qPrintable(QString::number(warm)));
#endif
    }
};

TSMEDIA_REGISTER_TEST(TestProviders)

#include "tst_providers.moc"
