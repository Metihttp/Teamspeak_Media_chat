// Unit tests for 2.5 link previews: reading a page's meta safely (src/linkpreview/pagemeta.*): the tokenizer, the
// charsets, the entities, which tag wins, walls and boilerplate, the theme colour, the sensitive signals, the
// feed and manifest readers; and the new JSON readers of content.* (generic oEmbed, Wikipedia, Discord,
// Pixeldrain) and stripTags. All fixtures are synthesized (tests/linkfixtures.*).

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextCodec>
#include <QtTest>

#include "linkfixtures.h"
#include "linkpreview/content.h"
#include "linkpreview/netpolicy.h"
#include "linkpreview/pagemeta.h"
#include "linkpreview/providers.h"
#include "linkpreview/textclean.h"
#include "testmain.h"

using namespace linkpreview;
using namespace linkfixtures;

namespace {

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

// The whole chain as the Fetcher runs it: the decoder, the prepared bytes, the head-mode scan.
pagemeta::Meta read(const QByteArray& body, const QByteArray& contentType = "text/html", bool bodyMode = false)
{
    const pagemeta::Decoder decoder = pagemeta::decoderFor(body, contentType);
    return pagemeta::scan(pagemeta::prepared(body, decoder), decoder.codec, bodyMode);
}

QByteArray html(const QByteArray& head, const QByteArray& body = "x")
{
    return "<html><head>" + head + "</head><body>" + body + "</body></html>";
}

QByteArray utf16le(const QString& text)
{
    QByteArray out("\xFF\xFE", 2);
    for (const QChar ch : text) {
        out += static_cast<char>(ch.unicode() & 0xFF);
        out += static_cast<char>(ch.unicode() >> 8);
    }
    return out;
}

bool fieldsWithinCaps(const pagemeta::Meta& m)
{
    const QString title = pagemeta::titleOf(m), description = pagemeta::descriptionOf(m), site = pagemeta::siteNameOf(m);
    return title.size() <= kCardTitleChars && description.size() <= kCardDescriptionChars && site.size() <= kCardSiteChars
           && textclean::clean(title, kCardTitleChars) == title && textclean::clean(description, kCardDescriptionChars) == description
           && textclean::clean(site, kCardSiteChars) == site && m.ogImages.size() <= 4 && m.twitterImages.size() <= 2 && m.ogVideos.size() <= 4
           && m.title.size() <= 4096 && m.ogTitle.size() <= 4096;
}

} // namespace

class TestPageMeta : public QObject
{
    Q_OBJECT

  private slots:
    // ---- charsets -------------------------------------------------------------------------------------------------

    void charsets_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("contentType");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QByteArray>("codec");
        QTest::newRow("utf-7 literal") << html("<meta charset=\"utf-7\"><title>+AGEAYgBj-</title>") << QByteArray("text/html") << a("+AGEAYgBj-")
                                       << QByteArray("UTF-8");
        QTest::newRow("iso-2022-jp is utf-8") << html("<title>plain</title>") << QByteArray("text/html; charset=iso-2022-jp") << a("plain")
                                              << QByteArray("UTF-8");
        QTest::newRow("bom beats header") << (QByteArray("\xEF\xBB\xBF", 3) + html("<title>caf\xC3\xA9</title>")) << QByteArray("text/html; charset=windows-1252")
                                          << text({'c', 'a', 'f', 0xE9}) << QByteArray("UTF-8");
        QTest::newRow("windows-1252 quotes") << html("<title>\x93Hi\x94</title>") << QByteArray("text/html; charset=windows-1252")
                                             << text({0x201C, 'H', 'i', 0x201D}) << QByteArray("windows-1252");
        QTest::newRow("latin1 is 1252") << html("<title>\x93Hi\x94</title>") << QByteArray("text/html;charset=\"ISO-8859-1\"")
                                        << text({0x201C, 'H', 'i', 0x201D}) << QByteArray("windows-1252");
        QTest::newRow("meta after 8 KiB ignored")
            << ("<html><head><!--" + QByteArray(9000, 'x') + "--><meta charset=\"shift_jis\"><title>caf\xC3\xA9</title></head></html>") << QByteArray("text/html")
            << text({'c', 'a', 'f', 0xE9}) << QByteArray("UTF-8");
        QTest::newRow("invalid utf-8 is 1252") << html("<title>caf\xE9</title>") << QByteArray("text/html") << text({'c', 'a', 'f', 0xE9})
                                               << QByteArray("windows-1252");
        QTest::newRow("http-equiv meta") << html("<meta http-equiv=\"content-type\" content=\"text/html; charset=windows-1252\"><title>\x93x\x94</title>")
                                         << QByteArray("text/html") << text({0x201C, 'x', 0x201D}) << QByteArray("windows-1252");
        QTest::newRow("utf-16 bom") << utf16le(a("<html><head><title>wide</title></head></html>")) << QByteArray("text/html") << a("wide") << QByteArray("UTF-8");
        QTest::newRow("utf-16 named is utf-8") << html("<title>x</title>") << QByteArray("text/html; charset=utf-16") << a("x") << QByteArray("UTF-8");
    }
    void charsets()
    {
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, contentType);
        QFETCH(QString, title);
        QFETCH(QByteArray, codec);
        const pagemeta::Decoder decoder = pagemeta::decoderFor(body, contentType);
        QCOMPARE(decoder.name, codec);
        QVERIFY(decoder.codec);
        QCOMPARE(pagemeta::titleOf(read(body, contentType)), title);
    }

    void shiftJisFromMeta()
    {
        if (!QTextCodec::codecForName("Shift_JIS"))
            QSKIP("This Qt has no Shift_JIS codec (the plugin then reads UTF-8)");
        const pagemeta::Decoder decoder = pagemeta::decoderFor(shiftJisPage(), "text/html");
        QCOMPARE(decoder.name, QByteArray("Shift_JIS"));
        QCOMPARE(pagemeta::titleOf(read(shiftJisPage())), text({0x4FA1, 0x683C, 0x6BD4, 0x8F03}));
    }

    void replacementCharsDropAField()
    {
        // Declared UTF-8, a title of broken bytes (more than 10% U+FFFD): dropped; the clean description stays.
        const QByteArray body = html("<meta charset=\"utf-8\"><title>\xFF\xFE\xFD\xFC ab</title><meta name=\"description\" content=\"fine text\">");
        const pagemeta::Meta m = read(body, "text/html; charset=utf-8");
        QVERIFY(m.title.isEmpty());
        QCOMPARE(pagemeta::descriptionOf(m), a("fine text"));
        QVERIFY(pagemeta::codecNameFor("cp1252") == "windows-1252");
        QVERIFY(pagemeta::codecNameFor("sjis") == "Shift_JIS");
        QVERIFY(pagemeta::codecNameFor("x-user-defined").isEmpty());
        QVERIFY(pagemeta::codecNameFor("iso-8859-12").isEmpty());
        QVERIFY(pagemeta::codecNameFor("utf-32").isEmpty());
    }

    // ---- entities -------------------------------------------------------------------------------------------------

    void entities_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("amp") << a("a &amp; b") << a("a & b");
        QTest::newRow("lt gt quot apos") << a("&lt;b&gt; &quot;x&quot; &apos;y&apos;") << a("<b> \"x\" 'y'");
        QTest::newRow("dec") << a("&#8212;") << text({0x2014});
        QTest::newRow("hex") << a("&#x2014;&#X41;") << text({0x2014, 'A'});
        QTest::newRow("zero") << a("&#0;") << text({0xFFFD});
        QTest::newRow("surrogate") << a("&#xD800;") << text({0xFFFD});
        QTest::newRow("too high") << a("&#x110000;") << text({0xFFFD});
        QTest::newRow("c1 as 1252") << a("&#150;&#x93;") << text({0x2013, 0x201C});
        QTest::newRow("legacy no semicolon") << a("&amp &lt &gt &quot &nbsp") << text({'&', ' ', '<', ' ', '>', ' ', '"', ' ', 0xA0});
        QTest::newRow("named needs semicolon") << a("&eacute x") << a("&eacute x");
        QTest::newRow("eacute") << a("caf&eacute;") << text({'c', 'a', 'f', 0xE9});
        QTest::newRow("case sensitive") << a("&Eacute;&EACUTE;") << (text({0xC9}) + a("&EACUTE;"));
        QTest::newRow("unknown") << a("&foo; &bar") << a("&foo; &bar");
        QTest::newRow("nine digits") << a("&#123456789;") << a("&#123456789;");
        QTest::newRow("empty numeric") << a("&#; &#x;") << a("&#; &#x;");
        QTest::newRow("hellip nbsp") << a("a&hellip;&nbsp;b") << text({'a', 0x2026, 0xA0, 'b'});
        QTest::newRow("once only") << a("&amp;lt;") << a("&lt;");
        QTest::newRow("middot") << a("x &#183; y &middot; z") << text({'x', ' ', 0xB7, ' ', 'y', ' ', 0xB7, ' ', 'z'});
    }
    void entities()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(pagemeta::decodeEntities(in), out);
    }

    // ---- which tag wins ---------------------------------------------------------------------------------------------

    void precedence()
    {
        const pagemeta::Meta all = read(html("<title>T</title><meta name=\"twitter:title\" content=\"TW\"><meta property=\"og:title\" content=\"OG\">"
                                             "<meta name=\"description\" content=\"D\"><meta name=\"twitter:description\" content=\"TD\">"
                                             "<meta property=\"og:description\" content=\"OD\">"));
        QCOMPARE(pagemeta::titleOf(all), a("OG"));
        QCOMPARE(pagemeta::descriptionOf(all), a("OD"));
        const pagemeta::Meta twitter = read(html("<title>T</title><meta name=\"twitter:title\" content=\"TW\"><meta name=\"description\" content=\"D\">"
                                                 "<meta name=\"twitter:description\" content=\"TD\">"));
        QCOMPARE(pagemeta::titleOf(twitter), a("TW"));
        QCOMPARE(pagemeta::descriptionOf(twitter), a("TD"));
        const pagemeta::Meta plain = read(html("<title>  T  \n</title><meta name=\"description\" content=\"D\">"));
        QCOMPARE(pagemeta::titleOf(plain), a("T"));
        QCOMPARE(pagemeta::descriptionOf(plain), a("D"));
        // An og:title of only white space falls through to the next source.
        const pagemeta::Meta blank = read(html("<meta property=\"og:title\" content=\"   \"><title>T</title>"));
        QCOMPARE(pagemeta::titleOf(blank), a("T"));
    }

    void firstWins()
    {
        const pagemeta::Meta m = read(html("<meta property=\"og:title\" content=\"one\"><meta property=\"og:title\" content=\"two\">"
                                           "<meta property=\"og:image\" content=\"https://x.com/1.png\"><meta property=\"og:image:secure_url\" content=\"https://x.com/1.png\">"
                                           "<meta property=\"og:image\" content=\"https://x.com/2.png\"><meta property=\"og:image:url\" content=\"https://x.com/3.png\">"
                                           "<meta property=\"og:image\" content=\"https://x.com/4.png\"><meta property=\"og:image\" content=\"https://x.com/5.png\">"
                                           "<meta name=\"twitter:image\" content=\"https://x.com/t1.png\"><meta name=\"twitter:image:src\" content=\"https://x.com/t2.png\">"
                                           "<meta name=\"twitter:image\" content=\"https://x.com/t3.png\"><title>a</title><title>b</title>"));
        QCOMPARE(m.ogTitle, a("one"));
        QCOMPARE(m.ogImages, QStringList({a("https://x.com/1.png"), a("https://x.com/2.png"), a("https://x.com/3.png"), a("https://x.com/4.png")}));
        QCOMPARE(m.twitterImages, QStringList({a("https://x.com/t1.png"), a("https://x.com/t2.png")}));
        QCOMPARE(m.title, a("a"));
        QCOMPARE(pagemeta::pictureCandidates(m).size(), 4);
        // og:video:type pairs with the last video before it.
        const pagemeta::Meta v = read(html("<meta property=\"og:video\" content=\"https://x.com/a.m3u8\"><meta property=\"og:video:type\" content=\"application/x-mpegURL\">"
                                           "<meta property=\"og:video:secure_url\" content=\"https://x.com/b.mp4\"><meta property=\"og:video:type\" content=\"video/mp4\">"));
        QCOMPARE(v.ogVideos.size(), 2);
        QCOMPARE(v.ogVideos.at(0).type, a("application/x-mpegurl"));
        QCOMPARE(v.ogVideos.at(1).url, a("https://x.com/b.mp4"));
        QCOMPARE(v.ogVideos.at(1).type, a("video/mp4"));
    }

    void nameOrProperty()
    {
        const pagemeta::Meta m = read(html("<meta name=\"og:title\" content=\"by name\"><meta property=\"twitter:card\" content=\"summary\">"
                                           "<meta property=\"\" name=\"description\" content=\"empty property\"><meta property=\"og:description\">"));
        QCOMPARE(m.ogTitle, a("by name"));
        QCOMPARE(m.twitterCard, a("summary"));
        QCOMPARE(m.description, a("empty property"));
        QVERIFY(m.ogDescription.isEmpty()); // no content: ignored
    }

    void upperCaseTags()
    {
        const pagemeta::Meta m = read("<HTML><HEAD><TITLE>Upper</TITLE><META PROPERTY=\"OG:TITLE\" CONTENT=\"Big\"><Meta Name='Description' Content=Unquoted></HEAD>");
        QCOMPARE(m.title, a("Upper"));
        QCOMPARE(m.ogTitle, a("Big"));
        QCOMPARE(m.description, a("Unquoted"));
        QCOMPARE(m.stop, pagemeta::Meta::Stop::HeadEnd);
    }

    void commentsScriptsIgnored()
    {
        const pagemeta::Meta m = read(html("<!-- <meta property=\"og:title\" content=\"comment\"> -->"
                                           "<script>var s = '<meta property=\"og:title\" content=\"script\">'; </script>"
                                           "<noscript><meta property=\"og:title\" content=\"noscript\"></noscript>"
                                           "<style>/* <meta property=og:title content=style> */</style>"
                                           "<template><meta property=\"og:title\" content=\"template\"></template>"
                                           "<![CDATA[ <meta property=\"og:title\" content=\"cdata\"> ]]>"
                                           "<meta property=\"og:title\" content=\"real\">"));
        QCOMPARE(m.ogTitle, a("real"));
        // </SCRIPT> in another case ends a script too.
        const pagemeta::Meta upper = read(html("<script>x</SCRIPT><meta property=\"og:title\" content=\"after\">"));
        QCOMPARE(upper.ogTitle, a("after"));
    }

    void unterminated_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("ogTitle");
        QTest::newRow("comment") << QByteArray("<meta property=\"og:title\" content=\"a\"><!-- never ends <meta property=\"og:description\" content=\"b\">") << a("a");
        QTest::newRow("script") << QByteArray("<meta property=\"og:title\" content=\"a\"><script> never ends") << a("a");
        QTest::newRow("quote") << QByteArray("<meta property=\"og:title\" content=\"a\"><meta property=\"og:description\" content=\"b") << a("a");
        QTest::newRow("tag") << QByteArray("<meta property=\"og:title\" content=\"a\"><meta property") << a("a");
        QTest::newRow("title") << QByteArray("<meta property=\"og:title\" content=\"a\"><title>never ends") << a("a");
        QTest::newRow("cdata") << QByteArray("<meta property=\"og:title\" content=\"a\"><![CDATA[ x") << a("a");
        QTest::newRow("bang") << QByteArray("<meta property=\"og:title\" content=\"a\"><!doctype") << a("a");
    }
    void unterminated()
    {
        QFETCH(QByteArray, body);
        QFETCH(QString, ogTitle);
        const pagemeta::Meta m = read(body);
        QCOMPARE(m.ogTitle, ogTitle);
        QCOMPARE(m.stop, pagemeta::Meta::Stop::End);
    }

    void pathological_data()
    {
        QTest::addColumn<QByteArray>("body");
        const int cap = 512 * 1024;
        QTest::newRow("all lt") << QByteArray(cap, '<');
        QByteArray metas;
        while (metas.size() < cap - 6)
            metas += "<meta ";
        QTest::newRow("meta x N") << metas;
        QTest::newRow("unclosed comment") << ("<html><head><!--" + QByteArray(cap - 20, 'c'));
        QTest::newRow("unclosed script") << ("<html><head><script>" + QByteArray(cap - 30, 's'));
        QTest::newRow("huge attribute") << ("<head><meta property=\"og:title\" content=\"" + QByteArray(500000, 'a') + "\"><title>t</title></head>");
        QByteArray many = "<head><meta ";
        for (int i = 0; i < 10000; ++i)
            many += "a" + QByteArray::number(i) + "=\"x\" ";
        many += "property=\"og:title\" content=\"late\"><title>t</title></head>";
        QTest::newRow("10000 attributes") << many;
        QByteArray amps = "<head><title>";
        for (int i = 0; i < 100000; ++i)
            amps += "&#";
        amps += "</title></head>";
        QTest::newRow("100000 ampersands") << amps;
        QByteArray tags = "<html><head>";
        while (tags.size() < cap - 4)
            tags += "<b>";
        QTest::newRow("tags") << tags;
        QTest::newRow("nul bytes") << ("<head><title>a" + QByteArray(1000, '\0') + "b</title></head>");
    }
    void pathological()
    {
        QFETCH(QByteArray, body);
        QVERIFY(body.size() <= 512 * 1024 + 64);
        QElapsedTimer timer;
        timer.start();
        const pagemeta::Meta m = read(body);
        const qint64         ms = timer.elapsed();
        QVERIFY(fieldsWithinCaps(m));
#ifdef NDEBUG
        QVERIFY2(ms < 50, qPrintable(QString::number(ms)));
#else
        Q_UNUSED(ms)
#endif
        QVERIFY(m.tags <= 4096);
    }

    void seededFuzz()
    {
        // 20 000 mutations in all, round robin over the fixtures (the big ones on their first 32 KiB): byte flips,
        // splices, truncations. Never a crash, every field within its cap and clean.
        const QVector<QByteArray> fixtures = {spotifyTrack(), xStatus(true), xStatus(false), teamspeakHome(), discourseSvg(), shiftJisPage(),
                                              klipyClip(), klipySticker(), blueskyPost(BlueskyKind::Image).right(4096), twitchClip(), telegramPost(),
                                              steamStore(), mastodon404WithOg(), titleOnly(), imgurAlbum(true), imgurPostMismatch(), tumblrPost(0),
                                              githubRepo().left(32 * 1024), cnnLike().left(32 * 1024), redditRss(RedditLink::IRedd), vredditMpd(true, true)};
        QRandomGenerator random(20251109);
        for (int round = 0; round < 20000; ++round) {
            QByteArray data = fixtures.at(round % fixtures.size());
            const int  kind = static_cast<int>(random.bounded(4));
            if (kind == 0) { // byte flips
                for (int i = 0; i < 8 && !data.isEmpty(); ++i)
                    data[static_cast<int>(random.bounded(data.size()))] = static_cast<char>(random.bounded(256));
            } else if (kind == 1 && data.size() > 2) { // a splice of another fixture
                const QByteArray& other = fixtures.at(static_cast<int>(random.bounded(fixtures.size())));
                const int         at    = static_cast<int>(random.bounded(data.size()));
                const int         from  = static_cast<int>(random.bounded(qMax(1, other.size())));
                data                    = data.left(at) + other.mid(from, 200) + data.mid(at);
            } else if (kind == 2) { // a truncation
                data.truncate(static_cast<int>(random.bounded(data.size() + 1)));
            } else { // characters that matter to the tokenizer
                static const char kSpecial[] = "<>\"'=/!-&;#";
                for (int i = 0; i < 6 && !data.isEmpty(); ++i)
                    data[static_cast<int>(random.bounded(data.size()))] = kSpecial[random.bounded(static_cast<int>(sizeof(kSpecial) - 1))];
            }
            const pagemeta::Meta m = read(data, round % 3 == 0 ? QByteArray("text/html; charset=windows-1252") : QByteArray("text/html"), round % 7 == 0);
            if (!fieldsWithinCaps(m))
                QFAIL(qPrintable(QString::number(round)));
            pagemeta::wallOf(m, QUrl(a("https://example.com/a/b")), QUrl(a("https://example.com/a/b")), false);
            const pagemeta::FeedEntry entry = pagemeta::readFeed(data);
            QVERIFY(entry.content.size() <= 64 * 1024 + 16);
            pagemeta::pickManifestVideo(pagemeta::readManifest(data));
        }
    }

    // ---- events, body mode ---------------------------------------------------------------------------------------

    void headEvents()
    {
        QCOMPARE(read(html("<title>x</title>")).stop, pagemeta::Meta::Stop::HeadEnd);
        QCOMPARE(read("<html><title>x</title><body><meta property=\"og:title\" content=\"in body\">").stop, pagemeta::Meta::Stop::BodyStart);
        QVERIFY(read("<html><title>x</title><body><meta property=\"og:title\" content=\"in body\">").ogTitle.isEmpty());
        const pagemeta::Meta cut = pagemeta::scanPage("<html><head><title>x</title><meta property=\"og:title\" content=\"y\">", nullptr, false, true);
        QCOMPARE(cut.stop, pagemeta::Meta::Stop::Cap);
        QCOMPARE(cut.ogTitle, a("y"));
        QCOMPARE(pagemeta::scanPage("<html><head><title>x</title>", nullptr, false, false).stop, pagemeta::Meta::Stop::End);
        QByteArray tags = "<html><head>";
        for (int i = 0; i < 5000; ++i)
            tags += "<i>";
        tags += "<meta property=\"og:title\" content=\"late\">";
        const pagemeta::Meta many = read(tags);
        QCOMPARE(many.stop, pagemeta::Meta::Stop::Tags);
        QVERIFY(many.ogTitle.isEmpty());
        QVERIFY(pagemeta::headDone(read(html("x")), false));
        QVERIFY(!pagemeta::headDone(read(html("x")), true));
    }

    void bodyModeKick()
    {
        const QByteArray     page = kickClip();
        const pagemeta::Meta head = read(page);
        QCOMPARE(head.stop, pagemeta::Meta::Stop::HeadEnd);
        QVERIFY(head.ogImages.isEmpty());
        QVERIFY(head.bytesScanned < 16 * 1024);
        const pagemeta::Meta body = read(page, "text/html", true);
        QCOMPARE(body.ogImages.value(0), a("https://clips.kick.com/clips/clip_01H811MXG4FBR62FXPE1AXABDH/thumb.jpg"));
        QCOMPARE(body.ogTitle, a("xQc clip: the moment"));
        QVERIFY(body.bytesScanned > 260 * 1024);
        // A late head is read to its end (CNN-like: og:title after 240 KB, og:site_name after 420 KB).
        const pagemeta::Meta cnn = read(cnnLike());
        QCOMPARE(cnn.ogTitle, a("CNN International - Breaking News, US News, World News and Video"));
        QCOMPARE(cnn.ogSiteName, a("CNN"));
        const pagemeta::Meta github = read(githubRepo());
        QCOMPARE(github.ogTitle, a("GitHub - torvalds/linux: Linux kernel source tree"));
        QCOMPARE(github.twitterCard, a("summary_large_image"));
    }

    // ---- theme colour -------------------------------------------------------------------------------------------

    void themeColor_data()
    {
        QTest::addColumn<QByteArray>("head");
        QTest::addColumn<bool>("has");
        QTest::addColumn<QString>("colour");
        const auto row = [](const char* name, const QByteArray& head, bool has, const char* colour) { QTest::newRow(name) << head << has << a(colour); };
        row("green", "<meta name=\"theme-color\" content=\"#2E7D32\">", true, "#ff2e7d32");
        row("short", "<meta name=\"theme-color\" content=\"#e1e\">", true, "#ffee11ee");
        row("pink", "<meta name=\"theme-color\" content=\"#E91E63\">", true, "#ffe91e63");
        row("indigo near wikipedia", "<meta name=\"theme-color\" content=\"#3F51B5\">", true, "#ff3f51b5");
        row("facebook blue near bluesky", "<meta name=\"theme-color\" content=\"#1877F2\">", true, "#ff1877f2");
        row("white", "<meta name=\"theme-color\" content=\"#ffffff\">", false, "");
        row("black", "<meta name=\"theme-color\" content=\"#000\">", false, "");
        row("discord brand", "<meta name=\"theme-color\" content=\"#5865F2\">", false, "");
        row("discord neighbour", "<meta name=\"theme-color\" content=\"#5866F2\">", false, "");
        row("spotify brand", "<meta name=\"theme-color\" content=\"#1DB954\">", false, "");
        row("named colour", "<meta name=\"theme-color\" content=\"red\">", false, "");
        row("five digits", "<meta name=\"theme-color\" content=\"#12345\">", false, "");
        row("bad digit", "<meta name=\"theme-color\" content=\"#12345g\">", false, "");
        row("empty", "<meta name=\"theme-color\" content=\"\">", false, "");
        row("media light wins without plain", "<meta name=\"theme-color\" media=\"(prefers-color-scheme: dark)\" content=\"#2E7D32\">"
                                              "<meta name=\"theme-color\" media=\"(prefers-color-scheme: light)\" content=\"#E91E63\">",
            true, "#ffe91e63");
        row("plain wins", "<meta name=\"theme-color\" media=\"(prefers-color-scheme: light)\" content=\"#E91E63\"><meta name=\"theme-color\" content=\"#2E7D32\">",
            true, "#ff2e7d32");
    }
    void themeColor()
    {
        QFETCH(QByteArray, head);
        QFETCH(bool, has);
        QFETCH(QString, colour);
        QRgb       rgb = 0;
        const bool got = pagemeta::themeColourOf(read(html(head)), &rgb);
        QCOMPARE(got, has);
        if (has)
            QCOMPARE(QColor::fromRgba(rgb).name(QColor::HexArgb), colour);
    }

    void brandLookAlikeColour()
    {
        for (const QRgb brand : providers::brandColours())
            QVERIFY(!pagemeta::isAcceptableThemeColour(brand));
        QRgb near = 0;
        QVERIFY(pagemeta::parseHexColour(a("#5866F2"), &near));
        QVERIFY(!pagemeta::isAcceptableThemeColour(near));
        for (const char* kept : {"#2E7D32", "#E91E63", "#3F51B5", "#1877F2"}) {
            QRgb c = 0;
            QVERIFY(pagemeta::parseHexColour(a(kept), &c));
            QVERIFY2(pagemeta::isAcceptableThemeColour(c), kept);
        }
        QRgb indigo = 0, wiki = 0, fb = 0, bsky = 0;
        pagemeta::parseHexColour(a("#3F51B5"), &indigo);
        pagemeta::parseHexColour(a("#3366CC"), &wiki);
        pagemeta::parseHexColour(a("#1877F2"), &fb);
        pagemeta::parseHexColour(a("#1185FE"), &bsky);
        qInfo("deltaE #3F51B5 vs Wikipedia %.2f, #1877F2 vs Bluesky %.2f", pagemeta::deltaE(indigo, wiki), pagemeta::deltaE(fb, bsky));
        QVERIFY(pagemeta::deltaE(indigo, wiki) >= 6);
        QVERIFY(pagemeta::deltaE(fb, bsky) >= 6);
    }

    // ---- walls, boilerplate ---------------------------------------------------------------------------------------

    void walls_data()
    {
        QTest::addColumn<QByteArray>("head");
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("final");
        QTest::addColumn<int>("verdict");
        const int  card = int(pagemeta::Verdict::Card), walled = int(pagemeta::Verdict::Walled);
        const auto row  = [](const char* name, const QByteArray& head, const char* link, const char* final, int verdict) {
            QTest::newRow(name) << head << a(link) << a(final) << verdict;
        };
        // Must stay cards
        row("error handling rust", "<title>Error handling - Rust By Example</title><meta name=\"description\" content=\"d\">",
            "https://doc.rust-lang.org/rust-by-example/error.html", "https://doc.rust-lang.org/rust-by-example/error.html", card);
        row("login with github docs", "<title>Login with GitHub | Docs</title>", "https://docs.example.com/guides/login-github",
            "https://docs.example.com/guides/login-github", card);
        row("sign in with apple guide", "<title>Sign in with Apple: a guide</title>", "https://blog.example.com/apple", "https://blog.example.com/apple", card);
        row("access denied film", "<title>Access Denied (2024 film)</title><meta property=\"og:title\" content=\"Access Denied (2024 film)\">",
            "https://films.example.com/access-denied", "https://films.example.com/access-denied", card);
        row("og title differs", "<title>Login</title><meta property=\"og:title\" content=\"A real article\">", "https://news.example.com/a",
            "https://news.example.com/a", card);
        row("link was a consent page", "<title>Our consent policy</title>", "https://example.com/consent", "https://example.com/consent", card);
        // Must be walls
        row("error", "<title>Error</title>", "https://example.com/page", "https://example.com/page", walled);
        row("login dash site", "<title>Login - Example</title>", "https://example.com/members", "https://example.com/members", walled);
        row("site pipe sign in", "<title>Example | Sign in</title>", "https://example.com/x", "https://example.com/x", walled);
        row("access denied", "<title>Access Denied</title>", "https://shop.example.com/item", "https://shop.example.com/item", walled);
        row("just a moment", "<title>Just a moment...</title>", "https://klipy.com/gifs/x", "https://klipy.com/gifs/x", walled);
        row("attention required", "<title>Attention Required! | Cloudflare</title>", "https://example.com/x", "https://example.com/x", walled);
        row("client challenge", "<title>Client Challenge</title>", "https://example.com/x", "https://example.com/x", walled);
        row("ddos guard", "<title>DDoS-Guard</title>", "https://example.com/x", "https://example.com/x", walled);
        row("redirected to login", "<title>Welcome</title>", "https://example.com/post/1", "https://example.com/login?next=/post/1", walled);
        row("consent host", "<title>Before you continue to YouTube</title>", "https://www.youtube.com/@TeamSpeak", "https://consent.youtube.com/m?x", walled);
        row("agecheck", "<title>Steam</title>", "https://store.steampowered.com/app/1/", "https://store.steampowered.com/agecheck/app/1/", walled);
        row("golem consent", "<meta name=\"robots\" content=\"noindex,nofollow\"><title>Golem</title>", "https://www.golem.de/",
            "https://www.golem.de/sonstiges/zustimmung/auswahl.html", walled);
    }
    void walls()
    {
        QFETCH(QByteArray, head);
        QFETCH(QString, link);
        QFETCH(QString, final);
        QFETCH(int, verdict);
        QCOMPARE(int(pagemeta::wallOf(read(html(head)), QUrl(link), QUrl(final), false)), verdict);
    }

    void challengeHeaderWalls()
    {
        QCOMPARE(pagemeta::wallOf(read(spotifyTrack()), QUrl(a("https://x.com/a")), QUrl(a("https://x.com/a")), true), pagemeta::Verdict::Walled);
    }

    void boilerplate_data()
    {
        QTest::addColumn<QByteArray>("page");
        QTest::addColumn<QString>("link");
        QTest::addColumn<int>("verdict");
        const int card = int(pagemeta::Verdict::Card), nothing = int(pagemeta::Verdict::Nothing);
        QTest::newRow("twitch vod shell") << twitchVodBoilerplate() << a("https://www.twitch.tv/videos/2912355720") << nothing;
        QTest::newRow("twitch home") << twitchVodBoilerplate() << a("https://www.twitch.tv/") << card;
        QTest::newRow("myteamspeak addons") << html("<title>myTeamSpeak</title>") << a("https://www.myteamspeak.com/addons") << nothing;
        QTest::newRow("teamspeak home") << teamspeakHome() << a("https://www.teamspeak.com/en/") << card;
        QTest::newRow("mastodon 404") << mastodon404WithOg() << a("https://mastodon.social/@Gargron/117577463015511048") << nothing;
        QTest::newRow("query is deep") << html("<title>Example</title>") << a("https://example.com/?p=1") << nothing;
        QTest::newRow("short single segment") << html("<title>Example</title>") << a("https://example.com/en") << card;
        QTest::newRow("instagram") << html("<meta property=\"og:site_name\" content=\"Instagram\"><title>Instagram</title>")
                                   << a("https://www.instagram.com/p/BsOGulcndj-/") << nothing;
    }
    void boilerplate()
    {
        QFETCH(QByteArray, page);
        QFETCH(QString, link);
        QFETCH(int, verdict);
        QCOMPARE(int(pagemeta::wallOf(read(page), QUrl(link), QUrl(link), false)), verdict);
    }

    void sensitiveSignals()
    {
        QVERIFY(pagemeta::isSensitive(read(html("<meta name=\"rating\" content=\"adult\">"))));
        QVERIFY(pagemeta::isSensitive(read(html("<meta name=\"RATING\" content=\"RTA-5042-1996-1400-1577-RTA\">"))));
        QVERIFY(pagemeta::isSensitive(read(html("<meta property=\"og:restrictions:age\" content=\"18\">"))));
        QVERIFY(pagemeta::isSensitive(read(html("<meta property=\"og:restrictions:age\" content=\"18+\">"))));
        QVERIFY(pagemeta::isSensitive(read(html("<meta property=\"og:restrictions:age\" content=\"21+\">"))));
        QVERIFY(!pagemeta::isSensitive(read(html("<meta property=\"og:restrictions:age\" content=\"13+\">"))));
        QVERIFY(!pagemeta::isSensitive(read(html("<meta name=\"rating\" content=\"general\">"))));
        QVERIFY(!pagemeta::isSensitive(read(html("<title>adult education</title>"))));
    }

    void layoutRules()
    {
        QCOMPARE(pagemeta::layoutFor(a("summary_large_image"), true, 100, 100), CardLayout::Large);
        QCOMPARE(pagemeta::layoutFor(a("player"), true, 100, 100), CardLayout::Large);
        QCOMPARE(pagemeta::layoutFor(a("summary"), true, 1200, 630), CardLayout::Thumb);
        QCOMPARE(pagemeta::layoutFor(QString(), true, 1200, 630), CardLayout::Large);
        QCOMPARE(pagemeta::layoutFor(QString(), true, 399, 200), CardLayout::Thumb);
        QCOMPARE(pagemeta::layoutFor(QString(), true, 600, 600), CardLayout::Thumb);
        QCOMPARE(pagemeta::layoutFor(a("summary_large_image"), false, 0, 0), CardLayout::None);
        QCOMPARE(pagemeta::durationMsOf(read(twitchClip())), qint64(28000));
    }

    // ---- xml-lite ----------------------------------------------------------------------------------------------------

    void xmlLiteRss()
    {
        const pagemeta::FeedEntry entry = pagemeta::readFeed(redditRss(RedditLink::IRedd));
        QVERIFY(entry.found);
        QCOMPARE(entry.title, text({'D', 'o', 'g', ' ', '&', ' ', 'b', 'a', 'l', 'l', ' ', 0x2014, ' ', 'a', ' ', 'c', 'l', 'a', 's', 's', 'i', 'c'}));
        QCOMPARE(entry.category, a("r/gifs"));
        QCOMPARE(entry.author, a("/u/someone"));
        QCOMPARE(entry.thumbnail, a("https://preview.redd.it/cx6f6vybmhq41.gif?width=320&crop=smart&s=fcd5"));
        QCOMPARE(pagemeta::linkOfContent(entry.content), a("https://i.redd.it/cx6f6vybmhq41.gif"));
        QCOMPARE(pagemeta::linkOfContent(pagemeta::readFeed(redditRss(RedditLink::VRedd)).content), a("https://v.redd.it/if97zvl1wgxh1"));
        QCOMPARE(pagemeta::linkOfContent(pagemeta::readFeed(redditRss(RedditLink::External)).content), a("https://www.example.org/news/a-story"));
        QVERIFY(!pagemeta::readFeed("<feed><title>no entries</title></feed>").found);
        QVERIFY(pagemeta::linkOfContent(a("<a href=\"https://x.com\">[comments]</a>")).isEmpty());
    }

    void xmlLiteMpd()
    {
        const pagemeta::Manifest silent = pagemeta::readManifest(vredditMpd(false, true));
        QVERIFY(!silent.hasAudio);
        QCOMPARE(silent.videos.size(), 5);
        QCOMPARE(pagemeta::pickManifestVideo(silent), a("CMAF_480.mp4"));
        QCOMPARE(pagemeta::pickManifestVideo(silent, 200), a("CMAF_220.mp4")); // none small enough: the smallest
        const pagemeta::Manifest sound = pagemeta::readManifest(vredditMpd(true, true));
        QVERIFY(sound.hasAudio);
        QCOMPARE(pagemeta::pickManifestVideo(pagemeta::readManifest(vredditMpd(false, false))), a("DASH_480"));
        // A BaseURL that is anything but a plain name is never used.
        const pagemeta::Manifest hostile = pagemeta::readManifest(
            "<MPD><AdaptationSet contentType=\"video\"><Representation height=\"480\"><BaseURL>https://evil.example.com/x.mp4</BaseURL></Representation>"
            "<Representation height=\"360\"><BaseURL>../x.mp4</BaseURL></Representation></AdaptationSet></MPD>");
        QVERIFY(pagemeta::pickManifestVideo(hostile).isEmpty());
        // Audio named on the representation only.
        QVERIFY(pagemeta::readManifest("<MPD><AdaptationSet><Representation mimeType=\"audio/mp4\"><BaseURL>a.mp4</BaseURL></Representation></AdaptationSet></MPD>")
                    .hasAudio);
    }

    // ---- JSON ---------------------------------------------------------------------------------------------------------

    void oembedGeneric_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QString>("error");
        QTest::newRow("vimeo") << vimeoOEmbed() << true << QString();
        QTest::newRow("html iframe ignored") << QByteArray("{\"type\":\"rich\",\"title\":\"t\",\"html\":\"<iframe src=\\\"https://evil\\\"></iframe>\"}") << true << QString();
        QTest::newRow("private thumbnail kept as text") << QByteArray("{\"type\":\"video\",\"title\":\"t\",\"thumbnail_url\":\"https://10.0.0.1/x.jpg\"}") << true << QString();
        QTest::newRow("too large") << ("{\"title\":\"" + QByteArray(70 * 1024, 'a') + "\"}") << false << a("tooLarge");
        QTest::newRow("array") << QByteArray("[{\"title\":\"t\"}]") << false << a("wrongType");
        QTest::newRow("xml") << QByteArray("<?xml version=\"1.0\"?><oembed><title>t</title></oembed>") << false << a("badJson");
        QTest::newRow("broken") << QByteArray("{\"title\":") << false << a("badJson");
        QTest::newRow("empty") << QByteArray("  ") << false << a("badJson");
        QTest::newRow("wrong types") << QByteArray("{\"type\":7,\"title\":[\"t\"],\"author_name\":{},\"thumbnail_width\":\"x\",\"duration\":-5}") << true << QString();
    }
    void oembedGeneric()
    {
        QFETCH(QByteArray, body);
        QFETCH(bool, ok);
        QFETCH(QString, error);
        content::OEmbedData data;
        QString            why;
        QCOMPARE(content::parseOEmbedData(body, &data, &why), ok);
        if (!ok) {
            QCOMPARE(why, error);
            return;
        }
        QVERIFY(data.title.size() <= 4096);
        if (QTest::currentDataTag() == QLatin1String("vimeo")) {
            QCOMPARE(data.type, a("video"));
            QCOMPARE(data.title, a("The New Vimeo Player (You Know, For Videos)"));
            QCOMPARE(data.authorName, a("Vimeo Staff"));
            QCOMPARE(data.thumbnailUrl, a("https://i.vimeocdn.com/video/452001751-8216e1ed.jpg?region=us"));
            QCOMPARE(data.thumbnailWidth, 640);
            QCOMPARE(data.durationMs, qint64(62000));
        } else if (QTest::currentDataTag() == QLatin1String("private thumbnail kept as text")) {
            QVERIFY(!net::resolveExtracted(QUrl(a("https://vimeo.com/api/oembed.json")), data.thumbnailUrl).isValid());
        } else if (QTest::currentDataTag() == QLatin1String("wrong types")) {
            QVERIFY(data.type.isEmpty() && data.title.isEmpty() && data.authorName.isEmpty());
            QCOMPARE(data.thumbnailWidth, 0);
            QCOMPARE(data.durationMs, qint64(0));
        }
        // Photo answers carry url; others never.
        content::OEmbedData gyazo;
        QVERIFY(content::parseOEmbedData(gyazoOEmbed(false), &gyazo, nullptr));
        QCOMPARE(gyazo.url, a("https://i.gyazo.com/0123456789abcdef0123456789abcdef.png"));
        QVERIFY(content::parseOEmbedData("{\"type\":\"video\",\"url\":\"https://x.com/a.mp4\"}", &gyazo, nullptr));
        QVERIFY(gyazo.url.isEmpty());
    }

    void typedJson()
    {
        content::WikiSummary wiki;
        QVERIFY(content::parseWikiSummary(wikiSummary(false), &wiki, nullptr));
        QCOMPARE(wiki.title, a("TeamSpeak"));
        QVERIFY(wiki.extract.startsWith(a("TeamSpeak is")));
        QCOMPARE(wiki.thumbnailWidth, 640);
        QVERIFY(content::parseWikiSummary(wikiSummary(true), &wiki, nullptr));
        QVERIFY(wiki.title.contains(QChar(0x200C)));
        QCOMPARE(textclean::clean(wiki.title, 200), wiki.title); // the ZWNJ between letters stays

        content::DiscordInvite invite;
        QVERIFY(content::parseDiscordInvite(discordInvite(true, false), &invite, nullptr));
        QCOMPARE(invite.guildId, a("302094807046684672"));
        QCOMPARE(invite.name, a("MINECRAFT"));
        QCOMPARE(invite.splash, a("0123456789abcdef0123456789abcdef"));
        QCOMPARE(invite.icon, a("a_fedcba9876543210fedcba9876543210"));
        QCOMPARE(invite.members, qint64(1234567));
        QCOMPARE(invite.online, qint64(89012));
        QVERIFY(!invite.nsfw);
        QVERIFY(content::parseDiscordInvite(discordInvite(false, true), &invite, nullptr));
        QVERIFY(invite.splash.isEmpty());
        QVERIFY(invite.nsfw);
        QVERIFY(content::parseDiscordInvite("{\"guild\":{\"id\":\"12\",\"splash\":\"../x\",\"icon\":\"ABCDEF\"}}", &invite, nullptr));
        QVERIFY(invite.guildId.isEmpty() && invite.splash.isEmpty() && invite.icon.isEmpty());

        content::PixeldrainInfo info;
        QVERIFY(content::parsePixeldrainInfo(pixeldrainInfo(a("image/PNG"), 12345), &info, nullptr));
        QCOMPARE(info.mimeType, a("image/png"));
        QCOMPARE(info.size, qint64(12345));
        QVERIFY(content::parsePixeldrainInfo("{\"size\":-1}", &info, nullptr));
        QCOMPARE(info.size, qint64(-1));
    }

    void stripTags()
    {
        content::OEmbedData sc;
        QVERIFY(content::parseOEmbedData(soundcloudOEmbed(), &sc, nullptr));
        const QString plain = textclean::clean(content::stripTags(sc.description), 350);
        QCOMPARE(plain, a("From the Soulhack album, recently featured & more here ."));
        QCOMPARE(content::stripTags(a("&lt;script&gt;alert(1)&lt;/script&gt;x")), a(" alert(1) x"));
        QCOMPARE(content::stripTags(a("a <b>bold</b> <unclosed")), a("a  bold  "));
    }
};

TSMEDIA_REGISTER_TEST(TestPageMeta)

#include "tst_pagemeta.moc"
