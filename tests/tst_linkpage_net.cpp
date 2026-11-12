// Unit tests for 2.5 link previews, the Fetcher's page plans end to end through a scripted transport
// (tests/linkfixtures.h FakeHttp, never the internet): every provider plan, the Web plan, the request rules (user
// agent, identity encoding, tracking parameters, extracted addresses, hop lists), HostGate inside the Fetcher (the
// first request queued on the GUI side, waits outside the deadline, cooling results), the v2 cache and the logs.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QTimer>
#include <QtTest>

#include <atomic>

#include "linkfixtures.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/hostgate.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/netpolicy.h"
#include "linkpreview/providers.h"
#include "medialink.h"
#include "sharefixtures.h"
#include "testmain.h"

using namespace linkpreview;
using namespace linkfixtures;

namespace {

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

QString enc(const QString& text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

Reply html(const QByteArray& body, int status = 200)
{
    Reply r;
    r.status      = status;
    r.contentType = "text/html; charset=utf-8";
    r.body        = body;
    return r;
}

Reply json(const QByteArray& body, int status = 200)
{
    Reply r;
    r.status      = status;
    r.contentType = "application/json";
    r.body        = body;
    return r;
}

Reply media(const QByteArray& type, const QByteArray& body)
{
    Reply r;
    r.contentType = type;
    r.body        = body;
    return r;
}

Reply status(int code, const QByteArray& type = "text/plain")
{
    Reply r;
    r.status      = code;
    r.contentType = type;
    r.body        = "x";
    return r;
}

Reply redirect(const QString& location, int code = 302)
{
    Reply r;
    r.status   = code;
    r.location = location;
    return r;
}

Caps clickCaps()
{
    Caps caps;
    caps.media     = 25 * 1024 * 1024;
    caps.picture   = 2 * 1024 * 1024;
    caps.page      = 512 * 1024;
    caps.userAsked = true;
    return caps;
}

// A gate whose clock jumps an hour at every look: never spaces, cools or breaks (for the plan tests).
std::shared_ptr<HostGate> permissiveGate()
{
    auto             clock = std::make_shared<std::atomic<qint64>>(0);
    HostGate::Limits limits;
    limits.pageRunning = limits.mediaRunning = 1000;
    return std::make_shared<HostGate>([clock] { return clock->fetch_add(3600000) + 3600000; }, limits);
}

struct ProbeFacts {
    bool   hasAudio   = false;
    qint64 durationMs = 5000;
};

class Harness
{
  public:
    explicit Harness(Fetcher::Options options = Fetcher::Options(), bool permissive = true)
    {
        options.totalTimeoutMs = options.totalTimeoutMs == net::kTotalTimeoutMs ? 15000 : options.totalTimeoutMs;
        if (permissive && !options.gate)
            options.gate = permissiveGate();
        auto facts    = m_facts;
        options.probe = [facts](const QString&, VideoProbe* out) {
            out->ok         = true;
            out->durationMs = facts->durationMs;
            out->width      = 320;
            out->height     = 240;
            out->hasAudio   = facts->hasAudio;
            out->poster     = QImage(32, 24, QImage::Format_RGB32);
            out->poster.fill(Qt::darkGray);
            return true;
        };
        if (!options.localSuffixes)
            options.localSuffixes = [] { return QStringList{QStringLiteral("corp.example.net")}; };
        fetcher.reset(new Fetcher(http, options));
        fetcher->setLogSink([this](const LogLine& line) { logs << line.text(); });
    }

    Result load(const QString& href, const Caps& caps = clickCaps(), int timeoutMs = 20000)
    {
        return loadTarget(classify(href), caps, timeoutMs);
    }
    Result loadTarget(const Target& target, const Caps& caps = clickCaps(), int timeoutMs = 20000)
    {
        QSignalSpy spy(fetcher.get(), &Fetcher::finished);
        fetcher->load(target, caps, dir.path());
        if (!spy.wait(timeoutMs) && spy.isEmpty())
            return Result();
        return spy.first().at(1).value<Result>();
    }
    QStringList files() const { return QDir(dir.path()).entryList(QDir::Files, QDir::Name); }
    void        setAudio(bool hasAudio, qint64 durationMs = 5000)
    {
        m_facts->hasAudio   = hasAudio;
        m_facts->durationMs = durationMs;
    }

    QTemporaryDir             dir;
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::unique_ptr<Fetcher>  fetcher;
    QStringList               logs;

  private:
    std::shared_ptr<ProbeFacts> m_facts = std::make_shared<ProbeFacts>();
};

QByteArray mp4()
{
    return mp4Header();
}

// A Nextcloud share's addresses (design2 2.5), as the transport sees them.
QString sharePage(const char* path = "/s/")
{
    return QString::fromLatin1("https://") + QString::fromLatin1(sharefixtures::kHost) + QString::fromLatin1(path) + QString::fromLatin1(sharefixtures::kToken);
}

QString sharePreview2048()
{
    return QString::fromLatin1("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + QString::fromLatin1(sharefixtures::kToken)
           + QString::fromLatin1("?x=2048&y=2048&a=1");
}

// Klipy's oEmbed address for a GIF page's slug, as the transport sees it (its exact form: tst_providers).
QString klipyOEmbedAddress(const char* slug)
{
    return providers::klipyOEmbedUrl(QString::fromLatin1(slug)).toString(QUrl::FullyEncoded);
}

} // namespace

class TestLinkPageNet : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup() { providers::setActiveForTests({}); }

    // ---- media sites -----------------------------------------------------------------------------------------------

    void tenorDerive()
    {
        Harness h;
        h.http->respond(a("https://media.tenor.com/oObwAjW2IIoAAAPo/tenor.mp4"), media("video/mp4", mp4()));
        const Result r = h.load(a("https://tenor.com/view/cat-cute-cat-angry-tantrum-hmph-gif-11594218182993649802"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        QCOMPARE(r.requests, 1);
        QCOMPARE(h.http->urls(), QStringList{a("https://media.tenor.com/oObwAjW2IIoAAAPo/tenor.mp4")});
        QVERIFY(QFileInfo(r.localFile).fileName().startsWith(classify(a("https://tenor.com/view/cat-cute-cat-angry-tantrum-hmph-gif-11594218182993649802")).key));
        QVERIFY(QFile::exists(r.thumbnailFile)); // the poster
    }

    void tenorSmallIdOEmbed()
    {
        Harness       h;
        const QString page = a("https://tenor.com/view/blahaj-go-spinny-blahaj-blahaj-spin-spin-shark-spin-gif-25670993");
        h.http->respond(a("https://tenor.com/oembed?url=") + enc(page), json(tenorOEmbed(a("AbCdEfGhIjK"))));
        h.http->respond(a("https://media.tenor.com/AbCdEfGhIjKAAAPo/tenor.mp4"), media("video/mp4", mp4()));
        const Result r = h.load(page);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        QCOMPARE(r.requests, 2);
    }

    void tenorShortGif()
    {
        Harness h;
        h.http->respond(a("https://tenor.com/bTSmr.gif"), redirect(a("https://tenor.com/view/cat-gif-11594218182993649802"), 301));
        h.http->respond(a("https://media.tenor.com/oObwAjW2IIoAAAAd/tenor.gif"), media("image/gif", gif(3)));
        const Target t = classify(a("https://tenor.com/bTSmr.gif"));
        QCOMPARE(t.cls, LinkClass::Direct);
        const Result r = h.loadTarget(t);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
        QCOMPARE(QFileInfo(r.localFile).fileName(), t.key + a(".gif"));
        QVERIFY(!h.http->urls().contains(a("https://tenor.com/view/cat-gif-11594218182993649802"))); // never the page itself
        Result cached;
        QVERIFY(Fetcher::cached(t, h.dir.path(), &cached)); // 2.4.0's record
        QCOMPARE(cached.localFile, r.localFile);
    }

    void giphyForms()
    {
        Harness h;
        h.http->respond(a("https://i.giphy.com/8FhXc8w45aN32.mp4"), media("video/mp4", mp4()));
        Result r = h.load(a("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        h.http->respond(a("https://i.giphy.com/abc123DEF.webp"), media("image/webp", webpLossy()));
        r = h.load(a("https://giphy.com/stickers/happy-abc123DEF"));
        if (QImageReader::supportedImageFormats().contains("webp")) {
            QVERIFY2(r.ok, qPrintable(r.error));
            QVERIFY(r.kind == Kind::Image || r.kind == Kind::Gif);
        }
        // Clips: the 480p MP4; a 404 (Giphy sends one with a video/mp4 body: the status decides) falls back.
        Reply missing = media("video/mp4", mp4());
        missing.status = 404;
        h.http->respond(a("https://media.giphy.com/media/abcdefGHIJ12/giphy480p.mp4"), missing);
        h.http->respond(a("https://i.giphy.com/abcdefGHIJ12.mp4"), media("video/mp4", mp4()));
        h.setAudio(true);
        r = h.load(a("https://giphy.com/clips/show-abcdefGHIJ12"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Video);
        QVERIFY(r.hasAudio);
    }

    void gphisRedirect()
    {
        Harness h;
        h.http->respond(a("https://gph.is/2KH8MQ0"), redirect(a("https://giphy.com/gifs/x-8FhXc8w45aN32")));
        h.http->respond(a("https://i.giphy.com/8FhXc8w45aN32.mp4"), media("video/mp4", mp4()));
        const Result r = h.load(a("https://gph.is/2KH8MQ0"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.requests, 2);
        QCOMPARE(h.http->urls(), QStringList({a("https://gph.is/2KH8MQ0"), a("https://i.giphy.com/8FhXc8w45aN32.mp4")}));
        // A redirect that ends on a 200 page (no Giphy link): nothing.
        h.http->respond(a("https://gph.is/AbCd1"), redirect(a("https://giphy.com/")));
        h.http->respond(a("https://giphy.com/"), html(webCard(a("GIPHY"), a("d"), QString())));
        QCOMPARE(h.load(a("https://gph.is/AbCd1")).error, a(errors::kNothing));
    }

    void imgurGifvAnd400()
    {
        Harness h;
        h.http->respond(a("https://i.imgur.com/Rum0zSz.mp4"), status(400));
        h.http->respond(a("https://i.imgur.com/Rum0zSz.jpeg"), media("image/jpeg", jpeg(40, 30)));
        const Result r = h.load(a("https://i.imgur.com/Rum0zSz.gifv"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(r.width, 40);
    }

    void imgurAlbumPick()
    {
        Harness h;
        h.http->respond(a("https://imgur.com/a/bcOmNQt"), html(imgurAlbum(true)));
        h.http->respond(a("https://i.imgur.com/AbCdE12.mp4"), media("video/mp4", mp4()));
        Result r = h.load(a("https://imgur.com/a/bcOmNQt"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        // og:video typed video/mp4 but ending .gif: the path decides (twitter:player:stream .mp4 first).
        h.http->respond(a("https://imgur.com/gallery/x-XyZ9876"), html(imgurPostMismatch()));
        h.http->respond(a("https://i.imgur.com/XyZ9876.mp4"), media("video/mp4", mp4()));
        r = h.load(a("https://imgur.com/gallery/x-XyZ9876"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!h.http->urls().contains(a("https://i.imgur.com/XyZ9876.gif")));
        // A picture cover: og:image without its query.
        h.http->respond(a("https://imgur.com/a/AbCdE12"), html(imgurAlbum(false)));
        h.http->respond(a("https://i.imgur.com/AbCdE12.jpeg"), media("image/jpeg", jpeg(20, 20)));
        r = h.load(a("https://imgur.com/a/AbCdE12"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        // A shell with only og:site_name: nothing.
        h.http->respond(a("https://imgur.com/a/ShElL12"), html(imgurShell()));
        QCOMPARE(h.load(a("https://imgur.com/a/ShElL12")).error, a(errors::kNothing));
    }

    void imgurRemovedPng()
    {
        Harness h;
        h.http->respond(a("https://i.imgur.com/Rum0zSz.mp4"), redirect(a("https://i.imgur.com/removed.png")));
        const Result r = h.load(a("https://imgur.com/Rum0zSz"));
        QCOMPARE(r.error, a(errors::kNotFound));
        QVERIFY(!h.http->urls().contains(a("https://i.imgur.com/removed.png")));
        // Direct imgur links too.
        h.http->respond(a("https://i.imgur.com/AbCdE12.png"), redirect(a("https://i.imgur.com/removed.png")));
        QCOMPARE(h.load(a("https://i.imgur.com/AbCdE12.png")).error, a(errors::kNotFound));
    }

    void klipyClip()
    {
        Harness h;
        h.http->respond(a("https://klipy.com/clips/angry-nod"), html(linkfixtures::klipyClip()));
        h.http->respond(a("https://static.klipy.com/clips/angry-nod/clip.mp4"), media("video/mp4", mp4()));
        Result r = h.load(a("https://klipy.com/clips/angry-nod"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        QVERIFY(!h.http->urls().contains(a("https://klipy.com/embed/angry-nod"))); // twitter:player:stream is a player: never
        h.http->respond(a("https://klipy.com/stickers/yes-excited-yes-sticker"), html(klipySticker()));
        h.http->respond(a("https://static3.klipy.com/stickers/yes/sticker.gif"), media("image/gif", gif(3)));
        r = h.load(a("https://klipy.com/stickers/yes-excited-yes-sticker"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
    }

    // Author's decision 2026-11-11: a walled Klipy GIF page asks Klipy's public oEmbed (no key). Its answer of that day
    // names no GIF ("https://static2.klipy.com/"): nothing, quietly, like the wall alone; the page and the oEmbed are
    // both remembered for 30 minutes, so the next GIF page makes no request at all.
    void klipyWalledMemo()
    {
        Harness       h(Fetcher::Options(), false); // the real gate (real time)
        const QString page   = a("https://klipy.com/gifs/andy-samberg-awkward");
        const QString oembed = klipyOEmbedAddress("andy-samberg-awkward");
        Reply         wall   = html(klipyChallenge(), 403);
        wall.challenge       = true;
        h.http->respond(page, wall);
        h.http->respond(oembed, json(klipyOEmbed("https:\\/\\/static2.klipy.com\\/")));
        const Result first = h.load(page);
        QCOMPARE(first.error, a(errors::kWalled));
        QCOMPARE(first.requests, 2);
        QCOMPARE(h.http->urls(), QStringList({page, oembed}));
        QVERIFY(!h.http->urls().contains(a("https://static2.klipy.com/"))); // the root is never asked
        // The oEmbed is a JSON request like any other: no cookies, the honest user agent, identity encoding.
        const FakeHttp::Request asked = h.http->requests().at(1);
        QCOMPARE(asked.accept, QByteArray("application/json"));
        QCOMPARE(asked.acceptEncoding, QByteArray("identity"));
        QVERIFY(asked.userAgent.contains("TSMediaChat/"));
        // Another GIF page: the page and the oEmbed are both walled for 30 minutes, no request (a click neither).
        QCOMPARE(h.load(a("https://klipy.com/gifs/other-gif")).error, a(errors::kWalled));
        QCOMPARE(h.load(page).error, a(errors::kWalled));
        QCOMPARE(h.http->count(), 2);
        QVERIFY(h.fetcher->gate()->describe().join(QLatin1Char('\n')).contains(a("health klipy.com walled /api")));
        // The link's own record: "nothing" (walled), kept 30 minutes.
        Result cached;
        QVERIFY(Fetcher::cached(classify(page), h.dir.path(), &cached));
        QVERIFY(!cached.ok);
        QCOMPARE(cached.error, a(errors::kWalled));
        // Klipy's clips still work.
        h.http->respond(a("https://klipy.com/clips/angry-nod"), html(linkfixtures::klipyClip()));
        h.http->respond(a("https://static.klipy.com/clips/angry-nod/clip.mp4"), media("video/mp4", mp4()));
        const Result r = h.load(a("https://klipy.com/clips/angry-nod"));
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // The day Klipy's oEmbed names the GIF: the GIF itself (spaced by the gate after the page), and while the page stays
    // walled the next GIF page goes straight to the oEmbed, never asking the page again.
    void klipyWalledOEmbedGif()
    {
        Harness       h(Fetcher::Options(), false); // the real gate (real time)
        const QString page    = a("https://klipy.com/gifs/andy-samberg-awkward");
        const QString oembed  = klipyOEmbedAddress("andy-samberg-awkward");
        const QString file    = a("https://static2.klipy.com/ii/4e7bea9f/3c/8a/andy-samberg-awkward.gif");
        Reply         wall    = html(klipyChallenge(), 403);
        wall.challenge        = true;
        h.http->respond(page, wall);
        h.http->respond(oembed, json(klipyOEmbed("https:\\/\\/static2.klipy.com\\/ii\\/4e7bea9f\\/3c\\/8a\\/andy-samberg-awkward.gif")));
        h.http->respond(file, media("image/gif", gif(3)));
        QElapsedTimer timer;
        timer.start();
        Result r = h.load(page);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
        QCOMPARE(r.requests, 3);
        QCOMPARE(h.http->urls(), QStringList({page, oembed, file}));
        QVERIFY2(timer.elapsed() >= 900, "the oEmbed is spaced after the page like every request to klipy.com");
        // The next GIF page: the oEmbed and its GIF, never the walled page.
        const QString other     = a("https://www.klipy.com/gifs/other-gif");
        const QString oembed2   = klipyOEmbedAddress("other-gif");
        const QString file2     = a("https://static.klipy.com/ii/other.gif");
        h.http->respond(other, wall);
        h.http->respond(oembed2, json(klipyOEmbed("https://static.klipy.com/ii/other.gif")));
        h.http->respond(file2, media("image/gif", gif(1)));
        r = h.load(other);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.kind == Kind::Gif || r.kind == Kind::Image); // a one-frame GIF may be a still, by its bytes
        QCOMPARE(h.http->urls().mid(3), QStringList({oembed2, file2}));
    }

    // The GIF the oEmbed names is loaded under the media rules (bytes, hops, caps); anything but a busy site is the
    // wall's quiet "nothing"; only an answer that came through walls the oEmbed itself.
    void klipyOEmbedRules()
    {
        Harness       h; // a gate that never holds (its memos end at once)
        Reply         wall = html(klipyChallenge(), 403);
        wall.challenge     = true;
        const auto    load = [&h, &wall](const char* slug, const Reply& answer) {
            const QString page = a("https://klipy.com/gifs/") + a(slug);
            h.http->respond(page, wall);
            h.http->respond(klipyOEmbedAddress(slug), answer);
            return h.load(page);
        };
        // A page that answers 200 with the challenge's own page: walled too (pagemeta), the oEmbed asked.
        h.http->respond(a("https://klipy.com/gifs/two-hundred"), html(klipyChallenge()));
        h.http->respond(klipyOEmbedAddress("two-hundred"), json(klipyOEmbed("https://static2.klipy.com/")));
        QCOMPARE(h.load(a("https://klipy.com/gifs/two-hundred")).error, a(errors::kWalled));
        QVERIFY(h.http->urls().contains(klipyOEmbedAddress("two-hundred")));
        // Not a GIF by its bytes: the media rules' answer, never shown.
        h.http->respond(a("https://static2.klipy.com/ii/fake.gif"), media("image/gif", "<html><body>not a gif</body></html>"));
        Result r = load("fake", json(klipyOEmbed("https://static2.klipy.com/ii/fake.gif")));
        QVERIFY(!r.ok);
        QCOMPARE(r.error, a(errors::kBadType));
        // A redirect off Klipy's static hosts: refused at that hop, no request to it.
        h.http->respond(a("https://static2.klipy.com/ii/moved.gif"), redirect(a("https://evil.example.net/x.gif")));
        r = load("moved", json(klipyOEmbed("https://static2.klipy.com/ii/moved.gif")));
        QVERIFY(!r.ok);
        QVERIFY(!h.http->urls().contains(a("https://evil.example.net/x.gif")));
        // Off the list as written: no request for it.
        r = load("elsewhere", json(klipyOEmbed("https://media.tenor.com/x/tenor.gif")));
        QCOMPARE(r.error, a(errors::kWalled));
        QVERIFY(!h.http->urls().contains(a("https://media.tenor.com/x/tenor.gif")));
        // Errors of the oEmbed: the wall's quiet nothing; a busy site its own answer (tried again when it allows).
        QCOMPARE(load("gone", status(404)).error, a(errors::kWalled));
        QCOMPARE(load("broken", status(500)).error, a(errors::kWalled));
        QCOMPARE(load("html", html(klipyChallenge())).error, a(errors::kWalled));
        Reply busy        = status(429);
        busy.retryAfterMs = 30000;
        QCOMPARE(load("busy", busy).error, a(errors::kRateLimited));
        // Klipy's clips and stickers never ask the oEmbed.
        const int before = h.http->count();
        h.http->respond(a("https://klipy.com/stickers/walled-sticker"), wall);
        QCOMPARE(h.load(a("https://klipy.com/stickers/walled-sticker")).error, a(errors::kWalled));
        QCOMPARE(h.http->count(), before + 1);
    }

    void vredditVideoOnly()
    {
        Harness h;
        Reply   mpd = media("application/dash+xml", vredditMpd(false, true));
        h.http->respond(a("https://v.redd.it/if97zvl1wgxh1/DASHPlaylist.mpd"), mpd);
        h.http->respond(a("https://v.redd.it/if97zvl1wgxh1/CMAF_480.mp4"), media("video/mp4", mp4()));
        const Result r = h.load(a("https://v.redd.it/if97zvl1wgxh1"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        QVERIFY(!h.http->urls().contains(a("https://v.redd.it/if97zvl1wgxh1"))); // never the bare address
    }

    void vredditAudioNothing()
    {
        Harness h;
        h.http->respond(a("https://v.redd.it/if97zvl1wgxh1/DASHPlaylist.mpd"), media("application/dash+xml", vredditMpd(true, true)));
        const Result r = h.load(a("https://v.redd.it/if97zvl1wgxh1"));
        QCOMPARE(r.error, a(errors::kNothing));
        QCOMPARE(h.http->count(), 1);
    }

    void redditRssNestedMedia()
    {
        Harness h;
        h.http->respond(a("https://www.reddit.com/comments/ftwv49/.rss?limit=1"), media("application/atom+xml; charset=UTF-8", redditRss(RedditLink::IRedd)));
        h.http->respond(a("https://i.redd.it/cx6f6vybmhq41.gif"), media("image/gif", gif(3)));
        const Result r = h.load(a("https://www.reddit.com/r/gifs/comments/ftwv49"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
        QCOMPARE(QFileInfo(r.localFile).fileName(), classify(a("https://www.reddit.com/r/gifs/comments/ftwv49")).key + a(".gif")); // the link's key
    }

    void redditRssCard()
    {
        Harness h;
        h.http->respond(a("https://www.reddit.com/comments/ftwv49/.rss?limit=1"), media("application/atom+xml", redditRss(RedditLink::External)));
        h.http->respond(a("https://preview.redd.it/cx6f6vybmhq41.gif?width=320&crop=smart&s=fcd5"), media("image/png", png(640, 360)));
        const Result r = h.load(a("https://redd.it/ftwv49"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.site, a("Reddit"));
        QCOMPARE(r.card.author, a("r/gifs") + QString::fromUtf8(" \xC2\xB7 ") + a("u/someone"));
        QCOMPARE(r.card.title, a("Dog & ball ") + QChar(0x2014) + a(" a classic"));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(!h.http->urls().contains(a("https://www.example.org/news/a-story"))); // an outside link is never fetched
    }

    void redditNestedFallbackCard()
    {
        Harness h;
        h.http->respond(a("https://www.reddit.com/comments/ftwv49/.rss?limit=1"), media("application/atom+xml", redditRss(RedditLink::VRedd)));
        h.http->respond(a("https://v.redd.it/if97zvl1wgxh1/DASHPlaylist.mpd"), media("application/dash+xml", vredditMpd(true, true)));
        h.http->respond(a("https://preview.redd.it/cx6f6vybmhq41.gif?width=320&crop=smart&s=fcd5"), media("image/png", png(320, 200)));
        const Result r = h.load(a("https://www.reddit.com/comments/ftwv49"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.layout, CardLayout::Thumb);
    }

    void redditTextPost()
    {
        // A text post (no picture, no link): its own text is the card's description.
        Harness    h;
        QByteArray feed = "<feed xmlns=\"http://www.w3.org/2005/Atom\"><entry><author><name>/u/someone</name></author>"
                          "<category term=\"IAmA\" label=\"r/IAmA\"/><content type=\"html\">&lt;!-- SC_OFF --&gt;&lt;div class=&quot;md&quot;&gt;"
                          "&lt;p&gt;Hello &amp;amp; welcome to the AMA.&lt;/p&gt;&lt;/div&gt;&lt;!-- SC_ON --&gt; submitted by "
                          "&lt;a href=&quot;https://www.reddit.com/user/someone&quot;&gt; /u/someone &lt;/a&gt;</content>"
                          "<title>I am a person, ask me anything</title></entry></feed>";
        h.http->respond(a("https://www.reddit.com/comments/z1c9z/.rss?limit=1"), media("application/atom+xml", feed));
        const Result r = h.load(a("https://www.reddit.com/r/IAmA/comments/z1c9z/i_am_a_person/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("I am a person, ask me anything"));
        QCOMPARE(r.card.description, a("Hello & welcome to the AMA."));
        QCOMPARE(r.card.layout, CardLayout::None);
        QCOMPARE(h.http->count(), 1);
    }

    void redirectOffListNothing()
    {
        // A short link to something its row doesn't preview: nothing, and no request to it.
        Harness h;
        h.http->respond(a("https://w.wiki/3"), redirect(a("https://www.wikimedia.org/"), 301));
        const Result r = h.load(a("https://w.wiki/3"));
        QCOMPARE(r.error, a(errors::kNothing));
        QVERIFY(r.hopRefused.isEmpty());
        QCOMPARE(h.http->urls(), QStringList{a("https://w.wiki/3")});
    }

    void redditSpacing6sAndHeaders()
    {
        Harness h(Fetcher::Options(), false);
        Reply   feed = media("application/atom+xml", redditRss(RedditLink::External));
        feed.rateRemaining = 0.0;
        feed.rateResetMs   = 43000;
        h.http->respond(a("https://www.reddit.com/comments/ftwv49/.rss?limit=1"), feed);
        h.http->respond(a("https://preview.redd.it/cx6f6vybmhq41.gif?width=320&crop=smart&s=fcd5"), media("image/png", png(320, 200)));
        QVERIFY(h.load(a("https://www.reddit.com/comments/ftwv49")).ok);
        const int    before = h.http->count();
        const Result second = h.load(a("https://www.reddit.com/comments/abc12"));
        QCOMPARE(second.error, a(errors::kRateLimited));
        QVERIFY(second.softCooling);
        QVERIFY(second.retryAfterMs > 30000 && second.retryAfterMs <= 43000);
        QCOMPARE(h.http->count(), before); // no request: never a 429
        QCOMPARE(providers::pageSpacingMs(spacingGroup(a("www.reddit.com"))), 6000);
    }

    void streamableProtocolRelative()
    {
        Harness h;
        h.http->respond(a("https://api-f.streamable.com/api/v1/videos/1hzf2p/mp4"), redirect(a("//cdn-cf-east.streamable.com/video/mp4/1hzf2p.mp4?token=x")));
        h.http->respond(a("https://cdn-cf-east.streamable.com/video/mp4/1hzf2p.mp4?token=x"), media("video/mp4", mp4()));
        h.setAudio(true);
        const Result r = h.load(a("https://streamable.com/1hzf2p"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Video);
        // A signed address on another host: refused by the hop list.
        h.http->respond(a("https://api-f.streamable.com/api/v1/videos/2abcde/mp4"), redirect(a("https://evil.example.net/x.mp4")));
        const Result off = h.load(a("https://streamable.com/2abcde"));
        QCOMPARE(off.error, a(errors::kRedirect));
        QCOMPARE(off.hopRefused, a("evil.example.net"));
    }

    void imgflip404Fallback()
    {
        Harness h;
        h.http->respond(a("https://i.imgflip.com/b3nmqx.gif"), media("image/gif", gif(2)));
        const Result r = h.load(a("https://imgflip.com/gif/b3nmqx"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
        QCOMPARE(h.http->urls(), QStringList({a("https://i.imgflip.com/b3nmqx.mp4"), a("https://i.imgflip.com/b3nmqx.gif")}));
    }

    void ninegagFallback()
    {
        Harness h;
        h.http->respond(a("https://img-9gag-fun.9cache.com/photo/aBZRrjx_700b.jpg"), media("image/jpeg", jpeg(30, 30)));
        const Result r = h.load(a("https://9gag.com/gag/aBZRrjx"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(h.http->urls().first(), a("https://img-9gag-fun.9cache.com/photo/aBZRrjx_460sv.mp4"));
    }

    void pinterest736Fallback()
    {
        Harness       h;
        const QString pin = a("https://www.pinterest.com/pin/1008173066603825557/");
        h.http->respond(a("https://www.pinterest.com/oembed.json?url=") + enc(pin), json(pinterestOEmbed()));
        h.http->respond(a("https://i.pinimg.com/236x/ab/cd/ef/abcdef.jpg"), media("image/jpeg", jpeg(236, 354)));
        const Result r = h.load(a("https://pinterest.com/pin/some-title--1008173066603825557/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->urls().value(1), a("https://i.pinimg.com/736x/ab/cd/ef/abcdef.jpg"));
        QCOMPARE(h.http->urls().value(2), a("https://i.pinimg.com/236x/ab/cd/ef/abcdef.jpg"));
    }

    void pinItRedirect()
    {
        Harness       h;
        const QString pin = a("https://www.pinterest.com/pin/1008173066603825557/");
        h.http->respond(a("https://pin.it/AbCdEf"), redirect(a("https://api.pinterest.com/url_shortener/AbCdEf/redirect/")));
        h.http->respond(a("https://api.pinterest.com/url_shortener/AbCdEf/redirect/"), redirect(pin));
        h.http->respond(a("https://www.pinterest.com/oembed.json?url=") + enc(pin), json(pinterestOEmbed()));
        h.http->respond(a("https://i.pinimg.com/736x/ab/cd/ef/abcdef.jpg"), media("image/jpeg", jpeg(73, 60)));
        const Result r = h.load(a("https://pin.it/AbCdEf"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!h.http->urls().contains(pin)); // the re-matched link's plan, never its page
        // The Pinterest app's links end on the pin's /sent/ form: the same pin, its share parameters never sent on.
        const QString sent = a("https://www.pinterest.com/pin/1008173066603825557/sent/?sender=1&invite_code=x");
        h.http->respond(a("https://pin.it/Hvt8hgT"), redirect(a("https://api.pinterest.com/url_shortener/Hvt8hgT/redirect/"), 308));
        h.http->respond(a("https://api.pinterest.com/url_shortener/Hvt8hgT/redirect/"), redirect(sent));
        const int before = h.http->count();
        const Result app = h.load(a("https://pin.it/Hvt8hgT"));
        QVERIFY2(app.ok, qPrintable(app.error));
        QCOMPARE(app.kind, Kind::Image);
        QVERIFY(!h.http->urls().contains(sent));
        QCOMPARE(h.http->urls().mid(before, 2),
                 QStringList({a("https://pin.it/Hvt8hgT"), a("https://api.pinterest.com/url_shortener/Hvt8hgT/redirect/")}));
        // An unknown code ends on the home page: nothing.
        h.http->respond(a("https://pin.it/Unknown"), redirect(a("https://www.pinterest.com/")));
        h.http->respond(a("https://www.pinterest.com/"), html(webCard(a("Pinterest"), a("d"), QString())));
        QCOMPARE(h.load(a("https://pin.it/Unknown")).error, a(errors::kNothing));
    }

    void gyazoPhotoVideo()
    {
        Harness       h;
        const QString id = a("0123456789abcdef0123456789abcdef");
        h.http->respond(a("https://api.gyazo.com/api/oembed?url=") + enc(a("https://gyazo.com/") + id), json(gyazoOEmbed(false)));
        h.http->respond(a("https://i.gyazo.com/") + id + a(".png"), media("image/png", png(80, 60)));
        Result r = h.load(a("https://gyazo.com/") + id);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        const QString vid = a("fedcba9876543210fedcba9876543210");
        h.http->respond(a("https://api.gyazo.com/api/oembed?url=") + enc(a("https://gyazo.com/") + vid), json(gyazoOEmbed(true)));
        h.http->respond(a("https://i.gyazo.com/") + vid + a(".mp4"), media("video/mp4", mp4()));
        r = h.load(a("https://gyazo.com/") + vid);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
    }

    void gyazo503()
    {
        Harness       h(Fetcher::Options(), false);
        const QString id = a("0123456789abcdef0123456789abcdef");
        h.http->respond(a("https://api.gyazo.com/api/oembed?url=") + enc(a("https://gyazo.com/") + id), status(503));
        QCOMPARE(h.load(a("https://gyazo.com/") + id).error, a(errors::kNothing));
        QCOMPARE(h.fetcher->gate()->state(a("api.gyazo.com"), HostGate::Lane::Page, QString(), nullptr) == HostGate::Verdict::Broken, false);
    }

    void pixeldrainInfoGate()
    {
        Harness h;
        h.http->respond(a("https://pixeldrain.com/api/file/AbCd12/info"), json(pixeldrainInfo(a("image/png"), 1234)));
        h.http->respond(a("https://pixeldrain.com/api/file/AbCd12"), media("application/octet-stream", png(10, 10)));
        Result r = h.load(a("https://pixeldrain.com/u/AbCd12"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        h.http->respond(a("https://pixeldrain.com/api/file/ZiPzip/info"), json(pixeldrainInfo(a("application/zip"), 1234)));
        QCOMPARE(h.load(a("https://pixeldrain.com/u/ZiPzip")).error, a(errors::kNothing));
        h.http->respond(a("https://pixeldrain.com/api/file/BiGbig/info"), json(pixeldrainInfo(a("video/mp4"), 900LL * 1024 * 1024)));
        r = h.load(a("https://pixeldrain.com/u/BiGbig"));
        QCOMPARE(r.error, a(errors::kTooLarge));
        QCOMPARE(r.size, 900LL * 1024 * 1024);
        QVERIFY(!h.http->urls().contains(a("https://pixeldrain.com/api/file/BiGbig")));
    }

    void tumblrVideoImageCard()
    {
        Harness h;
        h.http->respond(a("https://someblog.tumblr.com/post/123456789"), html(tumblrPost(0)));
        h.http->respond(a("https://va.media.tumblr.com/tumblr_abc.mp4"), media("video/mp4", mp4()));
        Result r = h.load(a("https://someblog.tumblr.com/post/123456789"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Loop);
        h.http->respond(a("https://someblog.tumblr.com/post/223456789"), html(tumblrPost(1)));
        h.http->respond(a("https://64.media.tumblr.com/abc/s1280x1920/photo.jpg"), media("image/jpeg", jpeg(30, 40)));
        r = h.load(a("https://someblog.tumblr.com/post/223456789"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        h.http->respond(a("https://someblog.tumblr.com/post/323456789"), html(tumblrPost(2)));
        r = h.load(a("https://someblog.tumblr.com/post/323456789"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.site, a("Tumblr"));
        QCOMPARE(r.card.description, a("Words of the post"));
    }

    // ---- site cards -------------------------------------------------------------------------------------------

    void vimeoCard()
    {
        Harness h;
        h.http->respond(a("https://vimeo.com/api/oembed.json?url=") + enc(a("https://vimeo.com/76979871")) + a("&width=640"), json(vimeoOEmbed()));
        h.http->respond(a("https://i.vimeocdn.com/video/452001751-8216e1ed.jpg?region=us"), media("image/jpeg", jpeg(640, 360)));
        const Result r = h.load(a("https://vimeo.com/76979871"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.site, a("Vimeo"));
        QCOMPARE(r.card.title, a("The New Vimeo Player (You Know, For Videos)"));
        QCOMPARE(r.card.author, a("Vimeo Staff"));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(r.card.play);
        QCOMPARE(r.card.durationMs, qint64(62000));
        QCOMPARE(r.card.pictureWidth, 640);
        QVERIFY(QFile::exists(r.card.pictureFile));
        QVERIFY(r.title.isEmpty() && r.author.isEmpty()); // YouTube's fields stay YouTube's
        QCOMPARE(r.requests, 2);
        // The unlisted form keeps its hash.
        h.http->respond(a("https://vimeo.com/api/oembed.json?url=") + enc(a("https://vimeo.com/76979871/8272103f6e")) + a("&width=640"), json(vimeoOEmbed()));
        QVERIFY(h.load(a("https://vimeo.com/76979871/8272103f6e")).ok);
    }

    void soundcloudCardStripped()
    {
        Harness h;
        h.http->respond(a("https://soundcloud.com/oembed?format=json&url=") + enc(a("https://soundcloud.com/forss/flickermood")), json(soundcloudOEmbed()));
        h.http->respond(a("https://i1.sndcdn.com/artworks-000067273316-smsiqx-t500x500.jpg"), media("image/jpeg", jpeg(500, 500)));
        const Result r = h.load(a("https://soundcloud.com/forss/flickermood"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.description, a("From the Soulhack album, recently featured & more here ."));
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        QVERIFY(!h.http->urls().contains(a("https://example.com/x"))); // links inside the description are text
    }

    void spotifyNoIntl()
    {
        Harness h;
        h.http->respond(a("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT"), html(spotifyTrack()));
        h.http->respond(a("https://i.scdn.co/image/ab67616d0000b273baf89eb11ec7c657805d2da0"), media("image/jpeg", jpeg(64, 64)));
        const Result r = h.load(a("https://open.spotify.com/intl-de/track/4cOdK2wGLETKBW3PvgPWqT?si=abcdef0123456789"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("Never Gonna Give You Up"));
        QCOMPARE(r.card.description, a("Rick Astley ") + QChar(0xB7) + a(" Whenever You Need Somebody ") + QChar(0xB7) + a(" Song ") + QChar(0xB7) + a(" 1987"));
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        QCOMPARE(h.http->urls().first(), a("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT")); // no intl part, no si
    }

    void appleRewrite600()
    {
        Harness       h;
        const QString page = a("https://music.apple.com/us/album/never-gonna-give-you-up/1559885420?i=1559885421");
        h.http->respond(a("https://music.apple.com/api/oembed?url=") + enc(page), json(appleOEmbed()));
        h.http->respond(a("https://is1-ssl.mzstatic.com/image/thumb/Music/v4/ab/cd/ef/source/600x600bb.jpg"), media("image/jpeg", jpeg(600, 600)));
        const Result r = h.load(page);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.author, a("Rick Astley"));
        QCOMPARE(r.card.pictureWidth, 600);
    }

    void deezerCard()
    {
        // Deezer is switched off (its covers answer 403 to every client); the plan is kept and tested switched on.
        providers::setActiveForTests({a("deezer")});
        Harness h;
        h.http->respond(a("https://api.deezer.com/oembed?url=") + enc(a("https://www.deezer.com/track/14408104")), json(deezerOEmbed()));
        h.http->respond(a("https://cdn-images.dzcdn.net/images/cover/abc/500x500-000000-80-0-0.jpg"), media("image/jpeg", jpeg(500, 500)));
        const Result r = h.load(a("https://www.deezer.com/fr/track/14408104"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.site, a("Deezer"));
        QCOMPARE(r.card.author, a("Daft Punk"));
    }

    void dailymotionOEmbedThumb()
    {
        Harness h;
        h.http->respond(a("https://www.dailymotion.com/services/oembed?url=") + enc(a("https://www.dailymotion.com/video/xbv3lcm")), json(dailymotionOEmbed()));
        h.http->respond(a("https://s1.dmcdn.net/v/TOKEN/x720"), media("image/jpeg", jpeg(405, 720)));
        const Result r = h.load(a("https://dai.ly/xbv3lcm"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.pictureHeight, 720);
        QVERIFY(r.card.play);
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.contains(a("/thumbnail/video/")) && !url.startsWith(a("https://dai.ly")), qPrintable(url));
    }

    void medalCardOnly()
    {
        Harness       h;
        const QString page = a("https://medal.tv/games/valorant/clips/nNUxTEDYDM4ts-dIt");
        h.http->respond(a("https://medal.tv/api/oembed?url=") + enc(page), json(medalOEmbed()));
        h.http->respond(a("https://cdn.medal.tv/123/thumbnail-1280.jpg?signature=abc&expires=9"), media("image/jpeg", jpeg(128, 72)));
        const Result r = h.load(page);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(h.http->count(), 2); // never the clip file
    }

    void youtubeExtraLpxOnly()
    {
        Harness h;
        h.http->respond(a("https://www.youtube.com/oembed?format=json&url=") + enc(a("https://www.youtube.com/watch?v=jfKfPfyJRdk")), json(youtubeOEmbed()));
        h.http->respond(a("https://i.ytimg.com/vi/jfKfPfyJRdk/hqdefault.jpg"), media("image/jpeg", jpeg(480, 360)));
        const Target t = classify(a("https://www.youtube.com/live/jfKfPfyJRdk"));
        QVERIFY(!t.isLegacy());
        const Result r = h.loadTarget(t);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.site, a("YouTube"));
        QVERIFY(r.card.play);
        // The embed form answers 404 for its own address: the watch address is asked; a 404 there: nothing.
        h.http->respond(a("https://www.youtube.com/oembed?format=json&url=") + enc(a("https://www.youtube.com/watch?v=AAAAAAAAAAA")), status(404));
        QCOMPARE(h.load(a("https://www.youtube.com/embed/AAAAAAAAAAA")).error, a(errors::kNothing));
    }

    void sTeamRedirectStore()
    {
        Harness h;
        h.http->respond(a("https://s.team/a/730"), redirect(a("https://store.steampowered.com/app/730/")));
        h.http->respond(a("https://store.steampowered.com/app/730/"), html(steamStore()));
        h.http->respond(a("https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/730/header.jpg?t=1"), media("image/jpeg", jpeg(460, 215)));
        const Result r = h.load(a("https://s.team/a/730"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.site, a("Steam"));
        QCOMPARE(r.card.title, a("Counter-Strike 2 on Steam"));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QCOMPARE(r.requests, 3);
    }

    void twitchShellRetries()
    {
        Fetcher::Options options;
        options.twitchRetryMs = 20;
        Harness h(options);
        h.http->respondSequence(a("https://clips.twitch.tv/IgnorantBelovedKathyCopyThis-eRU45Tis0o2zS27d"),
                                {html(twitchShell()), html(twitchShell()), html(twitchClip())});
        h.http->respond(a("https://clips-media-assets2.twitch.tv/abc/AT-cm%7Cabc-preview-480x272.jpg"), media("image/jpeg", jpeg(480, 272)));
        const Result r = h.load(a("https://clips.twitch.tv/IgnorantBelovedKathyCopyThis-eRU45Tis0o2zS27d"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.durationMs, qint64(28000));
        QVERIFY(r.card.play);
        QVERIFY(h.http->urls().contains(a("https://clips-media-assets2.twitch.tv/abc/AT-cm%7Cabc-preview-480x272.jpg")));
        h.http->respond(a("https://clips.twitch.tv/AlwaysTheShellClip"), html(twitchShell()));
        const int before = h.http->count();
        QCOMPARE(h.load(a("https://clips.twitch.tv/AlwaysTheShellClip")).error, a(errors::kNothing));
        QCOMPARE(h.http->count() - before, 3); // at most two retries
    }

    void twitchVodExcluded()
    {
        Harness h;
        QVERIFY(!classify(a("https://www.twitch.tv/videos/2912355720")).isValid());
        QCOMPARE(h.load(a("https://www.twitch.tv/videos/2912355720")).error, a(errors::kNotPreviewable));
        QCOMPARE(h.http->count(), 0);
    }

    void kickClipBodyScan()
    {
        Harness h;
        h.http->respond(a("https://kick.com/xqc/clips/clip_01H811MXG4FBR62FXPE1AXABDH"), html(kickClip()));
        h.http->respond(a("https://clips.kick.com/clips/clip_01H811MXG4FBR62FXPE1AXABDH/thumb.jpg"), media("image/jpeg", jpeg(640, 360)));
        const Result r = h.load(a("https://kick.com/xqc/clips/clip_01H811MXG4FBR62FXPE1AXABDH"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("xQc clip: the moment"));
        QVERIFY(r.card.play);
        // With data saver's 128 KiB page read the tags (260 KB in) are never reached: nothing.
        Caps saver   = clickCaps();
        saver.page   = 128 * 1024;
        Harness h2;
        h2.http->respond(a("https://kick.com/xqc/clips/clip_01H811MXG4FBR62FXPE1AXABDH"), html(kickClip()));
        QCOMPARE(h2.load(a("https://kick.com/xqc/clips/clip_01H811MXG4FBR62FXPE1AXABDH"), saver).error, a(errors::kNothing));
    }

    void blueskyOptOut()
    {
        Harness h;
        h.http->respond(a("https://bsky.app/profile/bsky.app/post/3mzjjovisxk2h"), html(blueskyPost(BlueskyKind::OptOut)));
        QCOMPARE(h.load(a("https://bsky.app/profile/bsky.app/post/3mzjjovisxk2h")).error, a(errors::kNothing));
        QCOMPARE(h.http->count(), 1);
    }

    void blueskyImageVideoText()
    {
        Harness h;
        h.http->respond(a("https://bsky.app/profile/bsky.app/post/aaaaaaaaaa"), html(blueskyPost(BlueskyKind::Image)));
        h.http->respond(a("https://cdn.bsky.app/img/feed_fullsize/plain/did:plc:z72/bafk@jpeg"), media("image/jpeg", jpeg(800, 600)));
        Result r = h.load(a("https://bsky.app/profile/bsky.app/post/aaaaaaaaaa"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(!h.http->urls().contains(a("https://cdn.bsky.app/img/feed_thumbnail/plain/did:plc:z72/bafk@jpeg"))); // the full size first
        h.http->respond(a("https://bsky.app/profile/bsky.app/post/bbbbbbbbbb"), html(blueskyPost(BlueskyKind::Video)));
        h.http->respond(a("https://video.bsky.app/watch/did/bafk/thumbnail.jpg"), media("image/jpeg", jpeg(640, 360)));
        r = h.load(a("https://bsky.app/profile/bsky.app/post/bbbbbbbbbb"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.play);
        QVERIFY(!h.http->urls().contains(a("https://video.bsky.app/watch/did/bafk/playlist.m3u8"))); // HLS never played
        h.http->respond(a("https://bsky.app/profile/bsky.app/post/cccccccccc"), html(blueskyPost(BlueskyKind::Text)));
        h.http->respond(a("https://cdn.bsky.app/img/avatar/plain/did:plc:z72/bafk@jpeg"), media("image/jpeg", jpeg(100, 100)));
        r = h.load(a("https://bsky.app/profile/bsky.app/post/cccccccccc"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        QCOMPARE(r.card.description, a("Just words in this post"));
    }

    void xCard()
    {
        Harness h;
        h.http->respond(a("https://x.com/jack/status/20"), html(xStatus(true)));
        h.http->respond(a("https://pbs.twimg.com/media/AbCdEf.jpg:large"), media("image/jpeg", jpeg(600, 400)));
        const Result r = h.load(a("https://twitter.com/jack/status/20"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.site, a("X"));
        QCOMPARE(r.card.title, a("jack on X"));
        QCOMPARE(r.card.description, a("\"just setting up my twttr\""));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(!r.card.hasColor); // a brand card never wears a page's colour
        h.http->respond(a("https://x.com/jack/status/21"), html(webCard(QString(), a("no title"), QString())));
        QCOMPARE(h.load(a("https://x.com/jack/status/21")).error, a(errors::kNothing));
    }

    void githubLateMeta()
    {
        Harness h;
        h.http->respond(a("https://github.com/torvalds/linux"), html(githubRepo()));
        h.http->respond(a("https://opengraph.githubassets.com/abc/torvalds/linux"), media("image/png", png(1200, 600)));
        const Result r = h.load(a("https://github.com/torvalds/linux"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("GitHub - torvalds/linux: Linux kernel source tree"));
        QCOMPARE(r.card.layout, CardLayout::Large);
    }

    void steamScreenshotMedia()
    {
        Harness h;
        h.http->respond(a("https://steamcommunity.com/sharedfiles/filedetails/?id=3070284539"), html(steamScreenshot()));
        h.http->respond(a("https://images.steamusercontent.com/ugc/123/ABC/"), media("image/jpeg", jpeg(64, 36)));
        const Result r = h.load(a("https://steamcommunity.com/sharedfiles/filedetails/?id=3070284539"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
    }

    void steamAgecheckNothing()
    {
        Harness h;
        h.http->respond(a("https://store.steampowered.com/app/271590/"), redirect(a("https://store.steampowered.com/agecheck/app/271590/")));
        h.http->respond(a("https://store.steampowered.com/agecheck/app/271590/"), html(steamAgecheck()));
        QCOMPARE(h.load(a("https://store.steampowered.com/app/271590/")).error, a(errors::kNothing));
    }

    void wikipediaSummaryRtl()
    {
        Harness       h;
        QString       title;
        for (const uint code : {0x062A, 0x06CC, 0x0645, 0x200C, 0x0627, 0x0633, 0x067E, 0x06CC, 0x06A9})
            title += QString::fromUcs4(&code, 1);
        h.http->respond(a("https://fa.wikipedia.org/api/rest_v1/page/summary/") + enc(title), json(wikiSummary(true)));
        h.http->respond(a("https://upload.wikimedia.org/wikipedia/fa/thumb/a/ab/x.png/320px-x.png"), media("image/png", png(320, 240)));
        const Result r = h.load(a("https://fa.wikipedia.org/wiki/") + title);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.title.contains(QChar(0x200C))); // the ZWNJ between letters stays
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        QCOMPARE(r.card.site, a("Wikipedia"));
    }

    void wikiShortLink()
    {
        Harness h;
        h.http->respond(a("https://w.wiki/3"), redirect(a("https://en.wikipedia.org/wiki/TeamSpeak"), 301));
        h.http->respond(a("https://en.wikipedia.org/api/rest_v1/page/summary/TeamSpeak"), json(wikiSummary(false)));
        h.http->respond(a("https://upload.wikimedia.org/wikipedia/commons/thumb/a/ab/TS.png/640px-TS.png"), media("image/png", png(640, 360)));
        const Result r = h.load(a("https://w.wiki/3"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(!h.http->urls().contains(a("https://en.wikipedia.org/wiki/TeamSpeak"))); // the summary, never the page
    }

    void telegramPost()
    {
        Harness h;
        h.http->respond(a("https://t.me/durov/552"), html(linkfixtures::telegramPost()));
        h.http->respond(a("https://cdn4.telesco.pe/file/AbCdEf.jpg"), media("image/jpeg", jpeg(800, 450)));
        const Result r = h.load(a("https://t.me/s/durov/552"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.site, a("Telegram"));
        QCOMPARE(r.card.layout, CardLayout::Large);
    }

    void discordInviteCounts()
    {
        Harness h;
        h.http->respond(a("https://discord.com/api/v10/invites/minecraft?with_counts=true"), json(discordInvite(true, false)));
        h.http->respond(a("https://cdn.discordapp.com/splashes/302094807046684672/0123456789abcdef0123456789abcdef.jpg?size=512"),
                        media("image/jpeg", jpeg(512, 288)));
        Result r = h.load(a("https://discord.com/invite/minecraft"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("MINECRAFT"));
        QCOMPARE(r.card.author, a("1,234,567 members") + QString::fromUtf8(" \xC2\xB7 ") + a("89,012 online"));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(!r.card.sensitive);
        h.http->respond(a("https://discord.com/api/v10/invites/nsfwserver?with_counts=true"), json(discordInvite(false, true)));
        h.http->respond(a("https://cdn.discordapp.com/icons/302094807046684672/a_fedcba9876543210fedcba9876543210.png?size=256"),
                        media("image/png", png(256, 256)));
        r = h.load(a("https://discord.gg/nsfwserver"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.sensitive);
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        h.http->respond(a("https://discord.com/api/v10/invites/gone?with_counts=true"), json("{\"message\":\"Unknown Invite\"}", 404));
        QCOMPARE(h.load(a("https://discord.gg/gone")).error, a(errors::kNothing));
    }

    void discordExpiredNoRequest()
    {
        Fetcher::Options options;
        options.nowSec = [] { return qint64(0x67000000); };
        Harness h(options);
        const Result r = h.load(a("https://cdn.discordapp.com/attachments/1/2/a.png?ex=66000000&is=1&hm=2"));
        QCOMPARE(r.error, a(errors::kExpired));
        QCOMPARE(h.http->count(), 0);
    }

    void dropboxRawRewrite()
    {
        Harness h;
        h.http->respond(a("https://www.dropbox.com/s/abcdefghij12/cat.png?rlkey=k&raw=1"), redirect(a("https://uc123.dl.dropboxusercontent.com/cd/0/inline/x")));
        h.http->respond(a("https://uc123.dl.dropboxusercontent.com/cd/0/inline/x"), media("image/png", png(12, 12)));
        const Target t = classify(a("https://www.dropbox.com/s/abcdefghij12/cat.png?dl=0&rlkey=k"));
        QCOMPARE(t.provider, a("dropbox"));
        const Result r = h.loadTarget(t);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
    }

    // ---- the Web class ------------------------------------------------------------------------------------------

    void webGenericCard()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/story/1"),
                        html(webCard(a("A story"), a("What happened"), a("https://img.example.net/s.jpg"),
                                     a("<meta property=\"og:site_name\" content=\"Example News\"><meta name=\"theme-color\" content=\"#2E7D32\">"))));
        h.http->respond(a("https://img.example.net/s.jpg"), media("image/jpeg", jpeg(1200, 630)));
        const Result r = h.load(a("https://news.example.net/story/1"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.title, a("A story"));
        QCOMPARE(r.card.description, a("What happened"));
        QCOMPARE(r.card.site, a("Example News"));
        QVERIFY(r.card.hasColor);
        QCOMPARE(r.card.color, QRgb(0xFF2E7D32));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(r.finalHost.isEmpty());
        QCOMPARE(r.requests, 2);
    }

    void webRelativeImage()
    {
        Harness h;
        h.http->respond(a("https://www.teamspeak.com/en/"), html(teamspeakHome()));
        h.http->respond(a("https://www.teamspeak.com/assets/images/og-image.png"), media("image/png", png(300, 200)));
        const Result r = h.load(a("http://www.teamspeak.com/en/")); // http: fetched as https
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.site, a("TeamSpeak"));
        QCOMPARE(r.card.layout, CardLayout::Thumb);
        QCOMPARE(h.http->urls().first(), a("https://www.teamspeak.com/en/"));
    }

    void webSvgFallsBackToTwitterImage()
    {
        Harness h;
        h.http->respond(a("https://community.teamspeak.com/t/teamspeak-6-0-0-beta5-1/65709"), html(discourseSvg()));
        h.http->respond(a("https://community.teamspeak.com/uploads/default/original/1X/card.png"), media("image/png", png(200, 200)));
        const Result r = h.load(a("https://community.teamspeak.com/t/teamspeak-6-0-0-beta5-1/65709"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.card.pictureFile.isEmpty());
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.endsWith(a(".svg")), qPrintable(url));
    }

    void webOctetStreamPng()
    {
        Harness h;
        h.http->respond(a("https://files.example.net/download-me"), media("application/octet-stream", png(50, 40)));
        const Result r = h.load(a("https://files.example.net/download-me"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(r.width, 50);
    }

    void webShiftJis()
    {
        if (!QTextCodec::codecForName("Shift_JIS"))
            QSKIP("no Shift_JIS codec in this Qt");
        Harness h;
        Reply   page = html(shiftJisPage());
        page.contentType = "text/html";
        h.http->respond(a("https://kakaku.example.net/"), page);
        const Result r = h.load(a("https://kakaku.example.net/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, linkfixtures::text({0x4FA1, 0x683C, 0x6BD4, 0x8F03}));
    }

    void webLateMetaCnn()
    {
        Harness h;
        h.http->respond(a("https://edition.cnn.com/"), html(cnnLike()));
        h.http->respond(a("https://media.cnn.com/api/v1/images/x.jpg"), media("image/jpeg", jpeg(1200, 675)));
        const Result r = h.load(a("https://edition.cnn.com/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.title, a("CNN International - Breaking News, US News, World News and Video"));
        // Data saver: the page read stops at 128 KiB, before the late og tags: the title alone is no card.
        Caps saver  = clickCaps();
        saver.page  = 128 * 1024;
        Harness h2;
        h2.http->respond(a("https://edition.cnn.com/"), html(cnnLike()));
        QCOMPARE(h2.load(a("https://edition.cnn.com/"), saver).error, a(errors::kNothing));
        QVERIFY(h2.http->bytesRead() <= 128 * 1024);
    }

    void web404NoCard()
    {
        Harness h;
        h.http->respond(a("https://mastodon.social/@Gargron/1"), html(mastodon404WithOg(), 404));
        QCOMPARE(h.load(a("https://mastodon.social/@Gargron/1")).error, a(errors::kNotFound));
        QCOMPARE(h.http->count(), 1);
    }

    void webWalls_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("challenge");
        QTest::newRow("cf 403") << cfChallenge403() << 403 << true;
        QTest::newRow("cf 200 header") << spotifyTrack() << 200 << true;
        QTest::newRow("client challenge 200") << clientChallenge200() << 200 << false;
        QTest::newRow("just a moment 503") << cfChallenge403() << 503 << true;
    }
    void webWalls()
    {
        QFETCH(QByteArray, body);
        QFETCH(int, status);
        QFETCH(bool, challenge);
        Harness h;
        Reply   reply  = html(body, status);
        reply.challenge = challenge;
        h.http->respond(a("https://walled.example.net/a/b"), reply);
        QCOMPARE(h.load(a("https://walled.example.net/a/b")).error, a(errors::kWalled));
    }

    void webConsentRedirect()
    {
        Harness h;
        h.http->respond(a("https://www.golem.de/"), redirect(a("https://www.golem.de/sonstiges/zustimmung/auswahl.html")));
        h.http->respond(a("https://www.golem.de/sonstiges/zustimmung/auswahl.html"), html(consentPage()));
        QCOMPARE(h.load(a("https://www.golem.de/")).error, a(errors::kWalled));
    }

    void webCrossSiteFinalHost()
    {
        Harness h;
        h.http->respond(a("https://bit.ly/abc"), redirect(a("https://www.example.org/article"), 301));
        h.http->respond(a("https://www.example.org/article"),
                        html(webCard(a("Article"), a("Text"), QString(),
                                     a("<meta property=\"og:site_name\" content=\"Example\"><meta name=\"theme-color\" content=\"#2E7D32\">"))));
        const Result r = h.load(a("https://bit.ly/abc"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.finalHost, a("www.example.org"));
        // Review fix: the other website's own name and colour never dress up the link's card.
        QVERIFY(r.card.site.isEmpty());
        QVERIFY(!r.card.hasColor);
        // A redirect within the site's own domain: no final host.
        h.http->respond(a("https://example.org/old"), redirect(a("https://www.example.org/article")));
        QVERIFY(h.load(a("https://example.org/old")).finalHost.isEmpty());
    }

    // Live test fix: our own message's card from another website (golang.org -> go.dev) disappeared after a restart. The
    // cache keeps the card with the host it went to; read back for our own message (the chat's tryCache / showHeldCard
    // after a restart) it is a card, for anyone else's automatic or cached load it is held back, never forgotten.
    void webCrossSiteCachedForOwnMessage()
    {
        Harness       h;
        const QString link = a("https://golang.org/doc/");
        h.http->respond(link, redirect(a("https://go.dev/doc/"), 301));
        h.http->respond(a("https://go.dev/doc/"), html(webCard(a("Documentation - The Go Programming Language"), a("The Go documentation."), QString())));
        const Result loaded = h.load(link);
        QVERIFY2(loaded.ok, qPrintable(loaded.error));
        QCOMPARE(loaded.finalHost, a("go.dev"));
        // After a restart: no request, the record as written.
        const Target t = classify(link);
        Result       cached;
        QVERIFY(Fetcher::cached(t, h.dir.path(), &cached));
        QVERIFY(cached.ok);
        QCOMPARE(cached.kind, Kind::Card);
        QCOMPARE(cached.finalHost, a("go.dev"));
        linkstate::LocalState loading;
        loading.state                    = linkstate::Local::Loading;
        const linkstate::LocalState own  = linkstate::afterCard(loading, cached.card, cached.finalHost, true);
        QCOMPARE(own.state, linkstate::Local::Loaded);
        QCOMPARE(own.finalHost, a("go.dev"));
        QCOMPARE(own.card.title, loaded.card.title);
        const linkstate::LocalState held = linkstate::afterCard(loading, cached.card, cached.finalHost, false);
        QCOMPARE(held.state, linkstate::Local::Nothing);
        QVERIFY(held.heldCrossSite);
        QVERIFY(linkstate::ownShowsHeldCard(held, true));   // our own message's slot reads it again
        QVERIFY(!linkstate::ownShowsHeldCard(held, false)); // anyone else's: nothing
        QCOMPARE(h.http->count(), 2);
    }

    void webImageByContentType()
    {
        Harness h;
        h.http->respond(a("https://pics.example.net/photo"), media("image/png", png(33, 22)));
        const Result r = h.load(a("https://pics.example.net/photo"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(r.width, 33);
        h.http->respond(a("https://pics.example.net/anim"), media("image/gif", gif(3)));
        QCOMPARE(h.load(a("https://pics.example.net/anim")).kind, Kind::Gif);
        h.http->respond(a("https://pics.example.net/clip"), media("video/mp4", mp4()));
        const Result video = h.load(a("https://pics.example.net/clip"));
        QCOMPARE(video.kind, Kind::Video); // never Loop for a Web link
    }

    void webAudioZeroBytes()
    {
        Harness h;
        Reply   audio = media("audio/mpeg", QByteArray(100000, 'a'));
        h.http->respond(a("https://radio.example.net/live"), audio);
        QCOMPARE(h.load(a("https://radio.example.net/live")).error, a(errors::kBadType));
        QCOMPARE(h.http->bytesRead(), qint64(0));
    }

    void webEncodedNoCard()
    {
        Harness h;
        Reply   gz = html(webCard(a("T"), a("D"), QString()));
        gz.contentEncoding = "gzip";
        h.http->respond(a("https://gz.example.net/"), gz);
        QCOMPARE(h.load(a("https://gz.example.net/")).error, a(errors::kBadType));
        QCOMPARE(h.http->bytesRead(), qint64(0));
        for (const FakeHttp::Request& request : h.http->requests())
            QCOMPARE(request.acceptEncoding, QByteArray("identity"));
    }

    void webEndlessBodyCapped()
    {
        Harness h;
        QByteArray body = "<html><head><title>T</title><meta name=\"description\" content=\"D\">" + QByteArray(2 * 1024 * 1024, 'x');
        h.http->respond(a("https://endless.example.net/"), html(body));
        const Result r = h.load(a("https://endless.example.net/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(h.http->bytesRead() <= 512 * 1024);
    }

    void webStopsAtHeadEnd()
    {
        Harness h;
        const QByteArray page = webCard(a("Early"), a("head"), QString()) + QByteArray(1024 * 1024, 'b');
        h.http->respond(a("https://early.example.net/"), html(page));
        const Result r = h.load(a("https://early.example.net/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY2(h.http->bytesRead() < 64 * 1024, qPrintable(QString::number(h.http->bytesRead())));
    }

    void webSlowloris()
    {
        Fetcher::Options options;
        options.pageMinBytesAfterMs = 300;
        Harness h(options);
        Reply   slow = html(webCard(a("Slow"), a("drip"), QString()) + QByteArray(64 * 1024, 's'));
        slow.chunk  = 50;
        slow.dripMs = 60;
        h.http->respond(a("https://slow.example.net/"), slow);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(h.load(a("https://slow.example.net/")).error, a(errors::kTimeout));
        QVERIFY(timer.elapsed() < 5000);
    }

    void webTitleOnlyNothing()
    {
        Harness h;
        h.http->respond(a("https://doc.qt.io/qt-5/qnetworkaccessmanager.html"), html(titleOnly()));
        QCOMPARE(h.load(a("https://doc.qt.io/qt-5/qnetworkaccessmanager.html")).error, a(errors::kNothing));
    }

    void webTwoPictureTries()
    {
        Harness h;
        const QString extra = a("<meta property=\"og:image\" content=\"https://img.example.net/2.jpg\"><meta property=\"og:image\" content=\"https://img.example.net/3.jpg\">"
                                "<meta name=\"twitter:image\" content=\"https://img.example.net/4.jpg\">");
        h.http->respond(a("https://pics.example.net/page"), html(webCard(a("T"), a("D"), a("https://img.example.net/1.jpg"), extra)));
        const Result r = h.load(a("https://pics.example.net/page"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.pictureFile.isEmpty());
        QCOMPARE(r.card.layout, CardLayout::None);
        QCOMPARE(h.http->count(), 3); // the page and two picture tries
    }

    void webPictureActionLikeRefused()
    {
        Harness h;
        h.http->respond(a("https://page.example.net/a"),
                        html(webCard(a("T"), a("D"), a("https://victim.example.net/unsubscribe?token=x"),
                                     a("<meta name=\"twitter:image\" content=\"https://img.example.net/ok.png\">"))));
        h.http->respond(a("https://img.example.net/ok.png"), media("image/png", png(100, 100)));
        const Result r = h.load(a("https://page.example.net/a"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.card.pictureFile.isEmpty());
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.contains(a("victim")), qPrintable(url));
    }

    // ---- review fixes (2.5.0) -------------------------------------------------------------------------------------

    void webRedirectHopsActionChecked()
    {
        // The action and token rules hold on every hop of a Web page's and a Web picture's request, not only on the
        // address the message or the page wrote: a redirect to an unsubscribe or token address is never followed.
        Harness h;
        h.http->respond(a("https://short.example.net/x"), redirect(a("https://victim.example.net/unsubscribe?token=abc")));
        h.http->respond(a("https://victim.example.net/unsubscribe?token=abc"), html(webCard(a("Bye"), a("Unsubscribed"), QString())));
        const Result page = h.load(a("https://short.example.net/x"));
        QVERIFY(!page.ok);
        QCOMPARE(page.error, a(errors::kRedirect));
        // A non-HTML file behind a redirect: not requested either.
        h.http->respond(a("https://short.example.net/doc"), redirect(a("https://files.example.net/report.pdf")));
        QVERIFY(!h.load(a("https://short.example.net/doc")).ok);
        // A page's picture that redirects to a vote address: that picture is skipped, the next candidate is used.
        h.http->respond(a("https://page.example.net/p"),
                        html(webCard(a("T"), a("D"), a("https://img.example.net/counter.png"),
                                     a("<meta name=\"twitter:image\" content=\"https://img.example.net/ok.png\">"))));
        h.http->respond(a("https://img.example.net/counter.png"), redirect(a("https://victim.example.net/vote?token=1")));
        h.http->respond(a("https://victim.example.net/vote?token=1"), media("image/png", png(10, 10)));
        h.http->respond(a("https://img.example.net/ok.png"), media("image/png", png(100, 100)));
        const Result card = h.load(a("https://page.example.net/p"));
        QVERIFY2(card.ok, qPrintable(card.error));
        QVERIFY(!card.card.pictureFile.isEmpty());
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.contains(a("victim")) && !url.contains(a("report.pdf")), qPrintable(url));
    }

    void pageClockExcludesGateWait()
    {
        // A later hop's wait for its site (another load's page request runs there) is not charged to the page clock
        // or to the slow-drip check: the page loads once its turn comes.
        Fetcher::Options options;
        options.pageTimeoutMs       = 700;
        options.pageMinBytesAfterMs = 300;
        options.maxGateWaitMs       = 5000;
        Harness h(options, false);
        h.http->respond(a("https://short.example.net/x"), redirect(a("https://www.held.example.org/story")));
        h.http->respond(a("https://www.held.example.org/story"), html(webCard(a("T"), a("D"), QString())));
        HostGate::Ticket busy;
        qint64           ms = 0;
        QCOMPARE(h.fetcher->gate()->acquire(a("other.held.example.org"), HostGate::Lane::Page, QString(), false, &ms, &busy), HostGate::Verdict::Go);
        QTimer release;
        release.setSingleShot(true);
        connect(&release, &QTimer::timeout, this, [&] { h.fetcher->gate()->release(busy, HostGate::Outcome::Ok); });
        release.start(1100); // longer than the page clock (700 ms) and the drip check (300 ms)
        const Result r = h.load(a("https://short.example.net/x"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(h.fetcher->gate()->state(a("held.example.org"), HostGate::Lane::Page, QString(), &ms) != HostGate::Verdict::Broken);
    }

    void completePageIsNotADrip()
    {
        // A small page whose last bytes arrive after the drip check's time is complete: never a time-out.
        Fetcher::Options options;
        options.pageMinBytesAfterMs = 300;
        Harness h(options);
        Reply small  = html(webCard(a("Small"), a("complete"), QString()));
        small.chunk  = small.body.size() / 2 + 1;
        small.dripMs = 200;
        h.http->respond(a("https://small.example.net/"), small);
        const Result r = h.load(a("https://small.example.net/"));
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    void webMediaNotUnderPageClock()
    {
        // A Web link that answers with media is read under the media rules, not the page clock (15 s, slow drip).
        Fetcher::Options options;
        options.pageTimeoutMs       = 300;
        options.pageMinBytesAfterMs = 150;
        Harness h(options);
        Reply slow  = media("image/png", png(40, 30) + QByteArray(40 * 1024, '\0'));
        slow.chunk  = 4096;
        slow.dripMs = 60; // about 600 ms in all, under 16 KiB after 150 ms
        h.http->respond(a("https://files.example.net/picture"), slow);
        const Result r = h.load(a("https://files.example.net/picture"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        // Sent as octet-stream: its first bytes make it media, and the rest is media too.
        Reply octet       = slow;
        octet.contentType = "application/octet-stream";
        h.http->respond(a("https://files.example.net/blob"), octet);
        const Result sniffed = h.load(a("https://files.example.net/blob"));
        QVERIFY2(sniffed.ok, qPrintable(sniffed.error));
        QCOMPARE(sniffed.kind, Kind::Image);
    }

    void cachedLinkIgnoresGate()
    {
        // A link the cache answers needs no request: never held or refused by its site's gate (spacing, cooling).
        Harness h(Fetcher::Options(), false);
        h.http->respond(a("https://cool.example.net/story"), html(webCard(a("Cached"), a("D"), QString())));
        QVERIFY(h.load(a("https://cool.example.net/story")).ok);
        const int     requests = h.http->count();
        QElapsedTimer timer;
        timer.start();
        Result again = h.load(a("https://cool.example.net/story")); // within the site's 1 s page spacing
        QVERIFY2(again.ok, qPrintable(again.error));
        QVERIFY2(timer.elapsed() < 700, qPrintable(QString::number(timer.elapsed())));
        // The site now cools (a 429 elsewhere): the cached card still comes, without a request.
        QTest::qWait(300);
        HostGate::Ticket t;
        qint64           ms = 0;
        QCOMPARE(h.fetcher->gate()->acquire(a("img.cool.example.net"), HostGate::Lane::Media, QString(), false, &ms, &t), HostGate::Verdict::Go);
        h.fetcher->gate()->release(t, HostGate::Outcome::RateLimited, 60000);
        QCOMPARE(h.fetcher->gate()->state(a("cool.example.net"), HostGate::Lane::Page, QString(), &ms), HostGate::Verdict::Cooling);
        again = h.load(a("https://cool.example.net/story"));
        QVERIFY2(again.ok, qPrintable(again.error));
        QCOMPARE(again.card.title, a("Cached"));
        QCOMPARE(h.http->count(), requests);
        // A link that isn't cached still waits for the site.
        QCOMPARE(h.load(a("https://cool.example.net/other")).error, a(errors::kRateLimited));
        QCOMPARE(h.http->count(), requests);
    }

    void pageMediaSensitive()
    {
        // D24: media taken from a page that declares adult content is marked like the page's card (and cached so).
        Harness    h;
        QByteArray adult = tumblrPost(1);
        adult.replace("<head>", "<head><meta name=\"rating\" content=\"adult\">");
        h.http->respond(a("https://someblog.tumblr.com/post/223456789"), html(adult));
        h.http->respond(a("https://64.media.tumblr.com/abc/s1280x1920/photo.jpg"), media("image/jpeg", jpeg(30, 40)));
        const Result r = h.load(a("https://someblog.tumblr.com/post/223456789"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QVERIFY(r.card.sensitive);
        Result cached;
        QVERIFY(Fetcher::cached(classify(a("https://someblog.tumblr.com/post/223456789")), h.dir.path(), &cached));
        QVERIFY(cached.card.sensitive);
        // og:restrictions:age 18 on a Klipy clip page: the video too.
        QByteArray clip = linkfixtures::klipyClip();
        clip.replace("<head>", "<head><meta property=\"og:restrictions:age\" content=\"18+\">");
        h.http->respond(a("https://klipy.com/clips/angry-nod"), html(clip));
        h.http->respond(a("https://static.klipy.com/clips/angry-nod/clip.mp4"), media("video/mp4", mp4()));
        const Result video = h.load(a("https://klipy.com/clips/angry-nod"));
        QVERIFY2(video.ok, qPrintable(video.error));
        QVERIFY(video.card.sensitive);
        // Without a flag: not marked.
        h.http->respond(a("https://someblog.tumblr.com/post/323456780"), html(tumblrPost(1)));
        const Result plain = h.load(a("https://someblog.tumblr.com/post/323456780"));
        QVERIFY2(plain.ok, qPrintable(plain.error));
        QVERIFY(!plain.card.sensitive);
    }

    void cardPictureMadeSmall()
    {
        // Review fix: a card's picture larger than a card ever draws it (800 px on a 2x screen) is made smaller here,
        // on the worker, once: the chat never decodes a large picture on the GUI thread, and pictures of 8 to 40 MP
        // (which the chat wouldn't decode) are shown too.
        Harness h;
        h.http->respond(a("https://big.example.net/a"), html(webCard(a("Big"), a("D"), a("https://img.example.net/wide.png"))));
        h.http->respond(a("https://img.example.net/wide.png"), media("image/png", png(2000, 1000)));
        const Result r = h.load(a("https://big.example.net/a"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.card.pictureWidth, 800);
        QCOMPARE(r.card.pictureHeight, 400);
        QCOMPARE(QImageReader(r.card.pictureFile).size(), QSize(800, 400));
        QCOMPARE(r.card.layout, CardLayout::Large);
        Result cached;
        QVERIFY(Fetcher::cached(classify(a("https://big.example.net/a")), h.dir.path(), &cached));
        QCOMPARE(cached.card.pictureFile, r.card.pictureFile);
        // About 9 megapixels (above what the chat decodes): made small, so it is drawn.
        h.http->respond(a("https://big.example.net/b"), html(webCard(a("Huge"), a("D"), a("https://img.example.net/huge.png"))));
        h.http->respond(a("https://img.example.net/huge.png"), media("image/png", png(3000, 3000)));
        const Result huge = h.load(a("https://big.example.net/b"));
        QVERIFY2(huge.ok, qPrintable(huge.error));
        QCOMPARE(QImageReader(huge.card.pictureFile).size(), QSize(800, 800));
        // A small one stays as it came.
        h.http->respond(a("https://big.example.net/c"), html(webCard(a("Small"), a("D"), a("https://img.example.net/small.png"))));
        const QByteArray small = png(300, 200);
        h.http->respond(a("https://img.example.net/small.png"), media("image/png", small));
        const Result kept = h.load(a("https://big.example.net/c"));
        QVERIFY2(kept.ok, qPrintable(kept.error));
        QFile file(kept.card.pictureFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), small);
        file.close();
        // Live test fix: a JPEG and a PNG with alpha, whose smaller copy takes the original's own name. The reader
        // held the original open, so the copy couldn't replace it; the card named a file that was gone.
        struct Big {
            const char* page;
            const char* picture;
            QByteArray  bytes;
            const char* mime;
            QSize       want;
        };
        const Big bigs[] = {
            {"https://big.example.net/jpeg", "https://img.example.net/og.jpg", jpeg(1200, 630), "image/jpeg", QSize(800, 420)},
            {"https://big.example.net/alpha", "https://img.example.net/og.png", png(1146, 600, true), "image/png", QSize(800, 418)},
            {"https://big.example.net/medal", "https://img.example.net/thumb.jpg", jpeg(854, 480), "image/jpeg", QSize(800, 449)},
            {"https://big.example.net/opaque", "https://img.example.net/opaque.png", png(2000, 1000), "image/png", QSize(800, 400)}, // the copy is a JPEG
        };
        for (const Big& b : bigs) {
            h.http->respond(a(b.page), html(webCard(a("Big"), QString(), a(b.picture))));
            h.http->respond(a(b.picture), media(b.mime, b.bytes));
            const Result big = h.load(a(b.page));
            QVERIFY2(big.ok, qPrintable(QString::fromLatin1(b.page) + QLatin1Char(' ') + big.error));
            QVERIFY2(QFileInfo::exists(big.card.pictureFile), b.page);
            QCOMPARE(QImageReader(big.card.pictureFile).size(), b.want);
            QCOMPARE(QSize(big.card.pictureWidth, big.card.pictureHeight), b.want);
            // Nothing else of that load is left beside it (no original, no part file).
            const QString stem = QFileInfo(big.card.pictureFile).fileName().section(QLatin1Char('-'), 0, 0);
            const QStringList files = QDir(h.dir.path()).entryList({stem + QLatin1String("*")}, QDir::Files);
            QCOMPARE(files.size(), 2); // <key>.json and <key>-thumb.<ext>
        }
    }

    void redditDirectOnlyKnownHosts()
    {
        // A Reddit link post's own file link is loaded only from Reddit's (or a known media site's) servers: a host
        // the post's author picked is never contacted; the post's card shows instead.
        Harness    h;
        QByteArray feed = redditRss(RedditLink::IRedd);
        feed.replace("https://i.redd.it/cx6f6vybmhq41.gif", "https://tracker.example.net/x.gif");
        h.http->respond(a("https://www.reddit.com/comments/ftwv49/.rss?limit=1"), media("application/atom+xml", feed));
        h.http->respond(a("https://tracker.example.net/x.gif"), media("image/gif", gif(3)));
        h.http->respond(a("https://preview.redd.it/cx6f6vybmhq41.gif?width=320&crop=smart&s=fcd5"), media("image/png", png(640, 360)));
        const Result r = h.load(a("https://www.reddit.com/r/gifs/comments/ftwv49"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.contains(a("tracker.example.net")), qPrintable(url));
        // An i.imgur.com file in a post still shows as media (Imgur's media list).
        feed = redditRss(RedditLink::IRedd);
        feed.replace("https://i.redd.it/cx6f6vybmhq41.gif", "https://i.imgur.com/AbCdEfG.png");
        h.http->respond(a("https://www.reddit.com/comments/abc123/.rss?limit=1"), media("application/atom+xml", feed));
        h.http->respond(a("https://i.imgur.com/AbCdEfG.png"), media("image/png", png(20, 20)));
        const Result imgur = h.load(a("https://www.reddit.com/comments/abc123"));
        QVERIFY2(imgur.ok, qPrintable(imgur.error));
        QCOMPARE(imgur.kind, Kind::Image);
        // A Reddit file that redirects off Reddit's hosts: refused on that hop (no request), the card instead.
        feed = redditRss(RedditLink::IRedd);
        h.http->respond(a("https://www.reddit.com/comments/def456/.rss?limit=1"), media("application/atom+xml", feed));
        h.http->respond(a("https://i.redd.it/cx6f6vybmhq41.gif"), redirect(a("https://tracker.example.net/y.gif")));
        const Result hop = h.load(a("https://www.reddit.com/comments/def456"));
        QVERIFY2(hop.ok, qPrintable(hop.error));
        QCOMPARE(hop.kind, Kind::Card);
        QVERIFY(!h.http->urls().contains(a("https://tracker.example.net/y.gif")));
    }

    void webMediaOverCapStopsBeforeBody()
    {
        Harness h;
        h.http->respond(a("https://pics.example.net/big"), media("image/png", png(400, 300)));
        Caps saver   = clickCaps();
        saver.media  = 0;
        saver.userAsked = false;
        const Result r = h.load(a("https://pics.example.net/big"), saver);
        QCOMPARE(r.error, a(errors::kTooLarge));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(r.size, qint64(png(400, 300).size()));
        QCOMPARE(h.http->bytesRead(), qint64(0));
    }

    void dataSaverCaps()
    {
        Harness h;
        h.http->respond(a("https://heavy.example.net/"), html(webCard(a("Heavy"), a("Picture too big"), a("https://img.example.net/huge.png"))));
        Reply bigPicture = media("image/png", png(10, 10) + QByteArray(600 * 1024, '\0'));
        h.http->respond(a("https://img.example.net/huge.png"), bigPicture);
        Caps saver;
        saver.media   = 0;
        saver.picture = 512 * 1024;
        saver.page    = 128 * 1024;
        const Result r = h.load(a("https://heavy.example.net/"), saver);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.pictureFile.isEmpty()); // over 512 KiB: the card without its picture
    }

    // ---- request rules ------------------------------------------------------------------------------------------

    void requestHeaders()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/story/1"), html(webCard(a("A"), a("B"), a("https://img.example.net/s.jpg"))));
        h.http->respond(a("https://img.example.net/s.jpg"), media("image/jpeg", jpeg(10, 10)));
        h.http->respond(a("https://cdn.example.net/a.png"), media("image/png", png(4, 4)));
        QVERIFY(h.load(a("https://news.example.net/story/1")).ok);
        QVERIFY(h.load(a("https://cdn.example.net/a.png")).ok);
        QVERIFY(h.http->count() >= 3);
        for (const FakeHttp::Request& request : h.http->requests()) {
            QCOMPARE(request.userAgent, net::userAgent());
            QCOMPARE(request.acceptEncoding, QByteArray("identity"));
        }
        QVERIFY(net::userAgent().startsWith("Mozilla/5.0 (compatible; TSMediaChat/"));
    }

    void trackingStripped()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/a?b=1"), html(webCard(a("T"), a("D"), QString())));
        const QString href = a("https://news.example.net/a?utm_source=x&b=1&fbclid=y");
        const Target  t    = classify(href);
        QVERIFY(h.loadTarget(t).ok);
        QCOMPARE(h.http->urls(), QStringList{a("https://news.example.net/a?b=1")});
        QCOMPARE(t.key, keyOf(a("page:https://news.example.net/a?utm_source=x&b=1&fbclid=y")));
    }

    void shortLinkTrackingStripped()
    {
        // A short link's Redirect step asks for the link without its tracking parameters too (review fix).
        Harness       h;
        const QString pin = a("https://www.pinterest.com/pin/1008173066603825557/");
        h.http->respond(a("https://pin.it/AbCdEf"), redirect(pin));
        h.http->respond(a("https://www.pinterest.com/oembed.json?url=") + enc(pin), json(pinterestOEmbed()));
        h.http->respond(a("https://i.pinimg.com/736x/ab/cd/ef/abcdef.jpg"), media("image/jpeg", jpeg(73, 60)));
        const Target t = classify(a("https://pin.it/AbCdEf?utm_source=share&fbclid=abc"));
        QCOMPARE(t.provider, a("pinterest"));
        const Result r = h.loadTarget(t);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->urls().value(0), a("https://pin.it/AbCdEf"));
        h.http->respond(a("https://s.team/a/730"), redirect(a("https://store.steampowered.com/app/730/")));
        h.http->respond(a("https://store.steampowered.com/app/730/"), html(steamStore()));
        const int before = h.http->count();
        h.load(a("https://s.team/a/730?utm_campaign=x"));
        QCOMPARE(h.http->urls().value(before), a("https://s.team/a/730"));
    }

    void extractedUrlsNeverRequested()
    {
        Harness     h;
        QString     head;
        for (const char* hostile : {"https://127.0.0.1/x.png", "https://router.lan/x.png", "https://[::1]/x.png", "https://2130706433/x.png",
                                    "https://0x7f.1/x.png", "https://intranet/x.png", "file://server/share/x.png", "data:image/png;base64,AAAA",
                                    "javascript:alert(1)", "https://user:pass@x.example.net/x.png", "https://cdn.example.net:8443/x.png"})
            head += a("<meta property=\"og:image\" content=\"") + a(hostile) + a("\">");
        h.http->respond(a("https://page.example.net/"), html(webCard(a("T"), a("D"), QString(), head)));
        const Result r = h.load(a("https://page.example.net/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->count(), 1);
    }

    void pageRedirectRebinding()
    {
        Harness h;
        h.http->setDns(a("rebind.example.net"), a("10.0.0.1"));
        h.http->respond(a("https://page.example.net/r"), redirect(a("https://rebind.example.net/x")));
        QCOMPARE(h.load(a("https://page.example.net/r")).error, a(errors::kBlocked));
        for (int i = 0; i < 7; ++i)
            h.http->respond(a("https://loop.example.net/") + QString::number(i), redirect(a("https://loop.example.net/") + QString::number(i + 1)));
        QCOMPARE(h.load(a("https://loop.example.net/0")).error, a(errors::kRedirect));
        h.http->respond(a("https://page.example.net/v6"), redirect(a("https://[::1]/x")));
        QCOMPARE(h.load(a("https://page.example.net/v6")).error, a(errors::kRedirect));
        // Through the Windows proxy, a host under the PC's own DNS suffixes is never asked.
        h.http->setViaProxy(true);
        const int before = h.http->count();
        h.http->respond(a("https://page.example.net/intranet"), redirect(a("https://wiki.corp.example.net/")));
        QCOMPARE(h.load(a("https://page.example.net/intranet")).error, a(errors::kBlocked));
        QCOMPARE(h.http->count() - before, 1);
    }

    void providerHopAllowlist()
    {
        Harness h;
        h.http->respond(a("https://vimeo.com/api/oembed.json?url=") + enc(a("https://vimeo.com/76979871")) + a("&width=640"),
                        redirect(a("https://vimeo.com.attacker.example.net/api/oembed.json")));
        const Result r = h.load(a("https://vimeo.com/76979871"));
        QCOMPARE(r.error, a(errors::kRedirect));
        QCOMPARE(r.hopRefused, a("vimeo.com.attacker.example.net"));
        QCOMPARE(h.http->count(), 1);
        QVERIFY(h.logs.last().contains(a("hop refused vimeo.com.attacker.example.net")));
        // A picture off the provider's media list: the card without it.
        QByteArray offList = vimeoOEmbed();
        offList.replace("https://i.vimeocdn.com/video/452001751-8216e1ed.jpg?region=us", "https://evil.example.net/x.jpg");
        h.http->respond(a("https://vimeo.com/api/oembed.json?url=") + enc(a("https://vimeo.com/12345")) + a("&width=640"), json(offList));
        const Result card = h.load(a("https://vimeo.com/12345"));
        QVERIFY(!card.ok); // a Vimeo card needs a picture (no description): nothing
        QVERIFY(!h.http->urls().contains(a("https://evil.example.net/x.jpg")));
    }

    void hopAllowlistsCoverPlans()
    {
        // Every address a plan builds (first steps and their fallbacks, every answer a 404) is on its row's lists.
        // (Klipy's walled GIF pages: klipyWalledMemo; Deezer, switched off, with its plan kept: switched on here.)
        providers::setActiveForTests({a("deezer")});
        Harness           h;
        const QStringList links = {a("https://klipy.com/clips/angry-nod"), a("https://tenor.com/view/cat-gif-11594218182993649802"),
                                   a("https://tenor.com/view/cat-gif-25670993"), a("https://giphy.com/gifs/x-8FhXc8w45aN32"),
                                   a("https://giphy.com/stickers/x-abc123DEF"), a("https://giphy.com/clips/x-abcdefGHIJ12"), a("https://gph.is/2KH8MQ0"),
                                   a("https://i.imgur.com/Rum0zSz.gifv"), a("https://imgur.com/a/bcOmNQt"), a("https://v.redd.it/if97zvl1wgxh1"),
                                   a("https://www.pinterest.com/pin/1008173066603825557/"), a("https://pin.it/AbCdEf"), a("https://streamable.com/1hzf2p"),
                                   a("https://imgflip.com/gif/b3nmqx"), a("https://imgflip.com/i/b5c1yf"), a("https://9gag.com/gag/aBZRrjx"),
                                   a("https://coub.com/view/3tgo9z"), a("https://gifer.com/en/Ab1"), a("https://makeagif.com/gif/jayne-cobb-6y"),
                                   a("https://gyazo.com/0123456789abcdef0123456789abcdef"), a("https://prnt.sc/abc123"), a("https://ibb.co/AbC123x"),
                                   a("https://postimg.cc/AbC123x"), a("https://pixeldrain.com/u/AbCd12"), a("https://someblog.tumblr.com/post/123456789"),
                                   a("https://vimeo.com/76979871"), a("https://soundcloud.com/forss/flickermood"), a("https://on.soundcloud.com/AbCd1"),
                                   a("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT"),
                                   a("https://music.apple.com/us/album/x/1559885420"), a("https://www.deezer.com/track/14408104"),
                                   a("https://link.deezer.com/s/30AbCdEfGh"), a("https://dai.ly/xbv3lcm"), a("https://medal.tv/clips/nNUxTEDYDM4ts"),
                                   a("https://clips.twitch.tv/IgnorantBelovedKathyCopyThis"), a("https://www.twitch.tv/shroud"), a("https://kick.com/xqc"),
                                   a("https://bsky.app/profile/bsky.app/post/3mzjjovisxk2h"), a("https://twitter.com/jack/status/20"),
                                   a("https://github.com/torvalds/linux"), a("https://store.steampowered.com/app/730/"), a("https://s.team/a/730"),
                                   a("https://en.wikipedia.org/wiki/TeamSpeak"), a("https://w.wiki/3"), a("https://t.me/durov/552"),
                                   a("https://discord.gg/minecraft"), a("https://www.reddit.com/r/gifs/comments/ftwv49"),
                                   a("https://www.youtube.com/live/jfKfPfyJRdk"), a("https://www.youtube.com/clip/UgkxAbCdEfGhIjKlMnOpQrStUv")};
        Fetcher::Options options;
        options.twitchRetryMs = 1;
        for (const QString& href : links) {
            const Target t = classify(href);
            QVERIFY2(t.isNewClass() && !t.provider.isEmpty(), qPrintable(href));
            const int before = h.http->count();
            h.load(href);
            const QVector<FakeHttp::Request> requests = h.http->requests();
            QVERIFY2(requests.size() > before, qPrintable(href));
            for (int i = before; i < requests.size(); ++i) {
                const QUrl url(requests.at(i).url);
                const bool page = providers::pageHopAllowed(t.provider, url);
                const bool med  = providers::mediaHopAllowed(t.provider, url);
                QVERIFY2(page || med, qPrintable(href + a(" -> ") + url.toString()));
            }
        }
    }

    // ---- HostGate in the Fetcher -----------------------------------------------------------------------------------

    void rateLimitedResult()
    {
        Harness h(Fetcher::Options(), false);
        Reply   busy = status(429, "text/html");
        busy.retryAfterMs = 120000;
        h.http->respond(a("https://busy.example.net/a.png"), busy);
        const Result r = h.load(a("https://busy.example.net/a.png"));
        QCOMPARE(r.error, a(errors::kRateLimited));
        QCOMPARE(r.retryAfterMs, qint64(120000));
        QVERIFY(!r.softCooling);
        // The next load of that site finishes without a request.
        const Result next = h.load(a("https://cdn.busy.example.net/b.png"));
        QCOMPARE(next.error, a(errors::kRateLimited));
        QVERIFY(next.retryAfterMs > 100000);
        QCOMPARE(h.http->count(), 1);
    }

    void gateFirstRequestQueued()
    {
        Fetcher::Options options;
        options.maxConcurrent = 3;
        Harness h(options, false);
        for (int i = 0; i < 3; ++i) {
            Reply slow = html(webCard(a("Slow"), a("D"), QString()));
            slow.delayMs = 200;
            h.http->respond(a("https://slow.example.net/") + QString::number(i), slow);
        }
        h.http->respond(a("https://other.example.org/"), html(webCard(a("Other"), a("D"), QString())));
        QSignalSpy finished(h.fetcher.get(), &Fetcher::finished);
        for (int i = 0; i < 3; ++i)
            h.fetcher->load(classify(a("https://slow.example.net/") + QString::number(i)), clickCaps(), h.dir.path());
        h.fetcher->load(classify(a("https://other.example.org/")), clickCaps(), h.dir.path());
        // One slow page runs; the two others wait in the queue (no thread held); the other site starts at once.
        QCOMPARE(h.fetcher->runningThreads(), 2);
        QCOMPARE(h.fetcher->queuedLoads(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 10000);
        const QStringList urls = h.http->urls();
        QVERIFY(urls.indexOf(a("https://other.example.org/")) <= 1);
        for (int i = 0; i < 4; ++i)
            QVERIFY(finished.at(i).at(1).value<Result>().ok);
    }

    void waitedOutIsCoolingNotFailure()
    {
        Fetcher::Options options;
        options.maxGateWaitMs = 300;
        Harness h(options, false);
        h.http->respond(a("https://page.example.net/w"), html(webCard(a("T"), a("D"), a("https://pics.blocked.example.org/p.png"))));
        h.http->respond(a("https://pics.blocked.example.org/p.png"), media("image/png", png(10, 10)));
        // Two media requests to that site's group are running: the picture hop must wait, longer than allowed.
        HostGate::Ticket t1, t2;
        qint64           ms = 0;
        QCOMPARE(h.fetcher->gate()->acquire(a("cdn.blocked.example.org"), HostGate::Lane::Media, QString(), false, &ms, &t1), HostGate::Verdict::Go);
        QCOMPARE(h.fetcher->gate()->acquire(a("img.blocked.example.org"), HostGate::Lane::Media, QString(), false, &ms, &t2), HostGate::Verdict::Wait);
        // (the spacing says wait: the second ticket comes a moment later)
        QTest::qWait(300);
        QCOMPARE(h.fetcher->gate()->acquire(a("img.blocked.example.org"), HostGate::Lane::Media, QString(), false, &ms, &t2), HostGate::Verdict::Go);
        const Result r = h.load(a("https://page.example.net/w"));
        QCOMPARE(r.error, a(errors::kGateBusy));
        QVERIFY(r.retryAfterMs > 0);
        h.fetcher->gate()->release(t1, HostGate::Outcome::Ok);
        h.fetcher->gate()->release(t2, HostGate::Outcome::Ok);
        QVERIFY(h.fetcher->gate()->state(a("blocked.example.org"), HostGate::Lane::Media, QString(), &ms) != HostGate::Verdict::Broken);
        QVERIFY(!h.http->urls().contains(a("https://pics.blocked.example.org/p.png")));
    }

    void gateWaitNotChargedToDeadline()
    {
        Fetcher::Options options;
        options.totalTimeoutMs = 800;
        options.maxGateWaitMs  = 5000;
        Harness h(options, false);
        h.http->respond(a("https://page.example.net/d"), html(webCard(a("T"), a("D"), a("https://pics.held.example.org/p.png"))));
        h.http->respond(a("https://pics.held.example.org/p.png"), media("image/png", png(10, 10)));
        HostGate::Ticket t1, t2;
        qint64           ms = 0;
        h.fetcher->gate()->acquire(a("a.held.example.org"), HostGate::Lane::Media, QString(), false, &ms, &t1);
        QTest::qWait(300);
        h.fetcher->gate()->acquire(a("b.held.example.org"), HostGate::Lane::Media, QString(), false, &ms, &t2);
        QTimer release;
        release.setSingleShot(true);
        connect(&release, &QTimer::timeout, this, [&] {
            h.fetcher->gate()->release(t1, HostGate::Outcome::Ok);
            h.fetcher->gate()->release(t2, HostGate::Outcome::Ok);
        });
        release.start(1200); // longer than the load's own 800 ms
        QElapsedTimer timer;
        timer.start();
        const Result r = h.load(a("https://page.example.net/d"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.card.pictureFile.isEmpty());
        QVERIFY(timer.elapsed() >= 1000);
    }

    void fetcherWithoutHubGate()
    {
        auto http = std::make_shared<FakeHttp>();
        http->respond(a("https://cdn.example.net/a.png"), media("image/png", png(4, 4)));
        http->respond(a("https://cdn.example.net/b.png"), media("image/png", png(4, 4)));
        QTemporaryDir dir;
        auto*         fetcher = new Fetcher(http, Fetcher::Options());
        QVERIFY(fetcher->gate());
        QSignalSpy finished(fetcher, &Fetcher::finished);
        fetcher->load(classify(a("https://cdn.example.net/a.png")), 1 << 20, dir.path());
        fetcher->load(classify(a("https://cdn.example.net/b.png")), 1 << 20, dir.path());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
        std::weak_ptr<HostGate> gate = fetcher->gate();
        delete fetcher; // shutdown with nothing running: no crash, the private gate goes with it
        QVERIFY(gate.expired());
    }

    void loopVsVideo()
    {
        Harness h;
        h.http->respond(a("https://i.giphy.com/AbCdEf1.mp4"), media("video/mp4", mp4()));
        QCOMPARE(h.load(a("https://giphy.com/gifs/AbCdEf1")).kind, Kind::Loop);
        h.setAudio(true);
        h.http->respond(a("https://i.giphy.com/AbCdEf2.mp4"), media("video/mp4", mp4()));
        QCOMPARE(h.load(a("https://giphy.com/gifs/AbCdEf2")).kind, Kind::Video);
        h.setAudio(false, 90000);
        h.http->respond(a("https://i.giphy.com/AbCdEf3.mp4"), media("video/mp4", mp4()));
        QCOMPARE(h.load(a("https://giphy.com/gifs/AbCdEf3")).kind, Kind::Video);
        // Direct links keep 2.4.0's kind.
        h.setAudio(false);
        h.http->respond(a("https://cdn.example.net/v.mp4"), media("video/mp4", mp4()));
        QCOMPARE(h.load(a("https://cdn.example.net/v.mp4")).kind, Kind::Video);
    }

    void webpLosslessRefused()
    {
        Harness h;
        h.http->respond(a("https://i.giphy.com/abc123DEF.webp"), media("image/webp", webpLossless()));
        const Result r = h.load(a("https://giphy.com/stickers/x-abc123DEF"));
        QCOMPARE(r.error, a(errors::kUnsupported));
        for (const QString& name : h.files())
            QVERIFY2(!name.endsWith(a(".webp")), qPrintable(name));
        h.http->respond(a("https://cdn.example.net/s.webp"), media("image/webp", webpAlphLossless()));
        QCOMPARE(h.load(a("https://cdn.example.net/s.webp")).error, a(errors::kUnsupported));
    }

    void win32PixelCapForMedia()
    {
        Fetcher::Options options;
        options.maxMediaPixels = 4096LL * 4096;
        Harness h(options);
        h.http->respond(a("https://cdn.example.net/huge.png"), media("image/png", png(5000, 4000)));
        QCOMPARE(h.load(a("https://cdn.example.net/huge.png")).error, a(errors::kTooManyPixels));
    }

    // ---- the cache --------------------------------------------------------------------------------------------------

    void cacheV2()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/story/1"), html(webCard(a("A story"), a("Text"), a("https://img.example.net/s.jpg"),
                                                                            a("<meta name=\"theme-color\" content=\"#E91E63\">"))));
        h.http->respond(a("https://img.example.net/s.jpg"), media("image/jpeg", jpeg(640, 360)));
        const Target t = classify(a("https://news.example.net/story/1"));
        QVERIFY(h.loadTarget(t).ok);
        Result cached;
        QVERIFY(Fetcher::cached(t, h.dir.path(), &cached));
        QVERIFY(cached.ok);
        QCOMPARE(cached.kind, Kind::Card);
        QCOMPARE(cached.card.title, a("A story"));
        QVERIFY(cached.card.hasColor);
        QVERIFY(QFile::exists(cached.card.pictureFile));
        const int before = h.http->count();
        QVERIFY(h.loadTarget(t).ok);
        QCOMPARE(h.http->count(), before); // from the cache
        // The record is v2, never read by 2.4.0 (which asks v1 only).
        QFile file(h.dir.path() + QLatin1Char('/') + t.key + a(".json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(o.value(a("v")).toInt(), 2);
        // Text cleaned again on read, a bad colour dropped.
        QJsonObject card = o.value(a("card")).toObject();
        card.insert(a("title"), a("  dirty ") + QChar(0x202E) + a(" title "));
        card.insert(a("color"), a("#5865F2")); // a brand's colour
        o.insert(a("card"), card);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(o).toJson());
        file.close();
        QVERIFY(Fetcher::cached(t, h.dir.path(), &cached));
        QCOMPARE(cached.card.title, a("dirty title"));
        QVERIFY(!cached.card.hasColor);
        // Eight days old: fetched again.
        o.insert(a("at"), double(QDateTime::currentSecsSinceEpoch() - 8 * 24 * 3600));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(o).toJson());
        file.close();
        QVERIFY(!Fetcher::cached(t, h.dir.path(), &cached));
        // Nothing to show: remembered for 24 hours (a click asks again).
        h.http->respond(a("https://doc.example.net/title-only"), html(titleOnly()));
        const Target none = classify(a("https://doc.example.net/title-only"));
        QCOMPARE(h.loadTarget(none).error, a(errors::kNothing));
        QVERIFY(Fetcher::cached(none, h.dir.path(), &cached));
        QVERIFY(!cached.ok);
        QCOMPARE(cached.error, a(errors::kNothing));
        Caps automatic       = clickCaps();
        automatic.userAsked  = false;
        const int noneBefore = h.http->count();
        QCOMPARE(h.loadTarget(none, automatic).error, a(errors::kNothing));
        QCOMPARE(h.http->count(), noneBefore);
        QCOMPARE(h.loadTarget(none, clickCaps()).error, a(errors::kNothing));
        QCOMPARE(h.http->count(), noneBefore + 1);
        // Legacy links keep 2.4.0's v1 record.
        h.http->respond(a("https://cdn.example.net/a.png"), media("image/png", png(4, 4)));
        const Target png4 = classify(a("https://cdn.example.net/a.png"));
        QVERIFY(h.loadTarget(png4).ok);
        QFile v1(h.dir.path() + QLatin1Char('/') + png4.key + a(".json"));
        QVERIFY(v1.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(v1.readAll()).object().value(a("v")).toInt(), 1);
    }

    void cachedWalled30Min()
    {
        Harness h;
        Reply   wall = html(clientChallenge200());
        h.http->respond(a("https://walled.example.net/page/1"), wall);
        const Target t = classify(a("https://walled.example.net/page/1"));
        QCOMPARE(h.loadTarget(t).error, a(errors::kWalled));
        Result cached;
        QVERIFY(Fetcher::cached(t, h.dir.path(), &cached));
        QCOMPARE(cached.error, a(errors::kWalled));
        QFile       file(h.dir.path() + QLatin1Char('/') + t.key + a(".json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        o.insert(a("at"), double(QDateTime::currentSecsSinceEpoch() - 31 * 60));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(o).toJson());
        file.close();
        QVERIFY(!Fetcher::cached(t, h.dir.path(), &cached));
    }

    void cacheIsolation()
    {
        // Two pages naming one picture: each card is its own (by the link's key), never shared by the picture's address.
        Harness h;
        h.http->respond(a("https://one.example.net/"), html(webCard(a("One"), a("D1"), a("https://img.example.net/same.png"))));
        h.http->respond(a("https://two.example.net/"), html(webCard(a("Two"), a("D2"), a("https://img.example.net/same.png"))));
        h.http->respond(a("https://img.example.net/same.png"), media("image/png", png(50, 50)));
        const Result one = h.load(a("https://one.example.net/"));
        const Result two = h.load(a("https://two.example.net/"));
        QVERIFY(one.ok && two.ok);
        QCOMPARE(one.card.title, a("One"));
        QCOMPARE(two.card.title, a("Two"));
        QVERIFY(one.card.pictureFile != two.card.pictureFile);
        QVERIFY(QFileInfo(one.card.pictureFile).fileName().startsWith(classify(a("https://one.example.net/")).key));
        QCOMPARE(h.http->urls().count(a("https://img.example.net/same.png")), 2); // one picture request per card
    }

    void htmlNeverOnDisk()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/story/2"), html(webCard(a("T"), a("D"), a("https://img.example.net/s.jpg"))));
        h.http->respond(a("https://img.example.net/s.jpg"), media("image/jpeg", jpeg(64, 64)));
        const Target t = classify(a("https://news.example.net/story/2"));
        QVERIFY(h.loadTarget(t).ok);
        for (const QString& name : h.files())
            QVERIFY2(name == t.key + a(".json") || name.startsWith(t.key + a("-thumb.")), qPrintable(name));
    }

    // ---- Nextcloud and ownCloud shares (design2 2.3-2.8, N-T5) -------------------------------------------------------

    void sharePictureThroughPreview()
    {
        Harness h;
        h.http->respond(sharePage(), html(sharefixtures::NcPage().build()));
        h.http->respond(sharePreview2048(), media("image/jpeg", jpeg(640, 480)));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(r.requests, 2);
        QCOMPARE(h.http->urls(), (QStringList{sharePage(), sharePreview2048()})); // one preview request after the page, never the file
        QCOMPARE(h.http->requests().first().accept, QByteArray("text/html,application/xhtml+xml;q=0.9,*/*;q=0.1"));
        // The log names the server, never the share's token (anyone with it can open the share).
        QVERIFY(!h.logs.isEmpty());
        for (const QString& line : h.logs) {
            QVERIFY2(!line.contains(a(sharefixtures::kToken)), qPrintable(line));
            QVERIFY2(line.contains(a("https://cloud.example.org/")), qPrintable(line));
        }
        // The same link again: the cache answers, no request.
        const Result again = h.load(sharePage());
        QVERIFY(again.ok);
        QCOMPARE(h.http->count(), 2);
    }

    void sharePreviewFallbacks()
    {
        Harness h;
        h.http->respond(sharePage(), html(sharefixtures::NcPage().build()));
        h.http->respond(sharePreview2048(), status(404));
        h.http->respond(sharePage() + a("/preview"), media("image/jpeg", jpeg(320, 240)));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->urls(), (QStringList{sharePage(), sharePreview2048(), sharePage() + a("/preview")}));
        // Both previews gone: a JPEG within the cap loads from /download (303 to the DAV address on the same host).
        Harness d;
        d.http->respond(sharePage(), html(sharefixtures::NcPage().build()));
        d.http->respond(sharePreview2048(), status(404));
        d.http->respond(sharePage() + a("/preview"), status(400));
        d.http->respond(sharePage() + a("/download"), redirect(a("/public.php/dav/files/") + a(sharefixtures::kToken), 303));
        d.http->respond(a("https://cloud.example.org/public.php/dav/files/") + a(sharefixtures::kToken), media("image/jpeg", jpeg(64, 64)));
        const Result file = d.load(sharePage());
        QVERIFY2(file.ok, qPrintable(file.error));
        QCOMPARE(d.http->count(), 5);
        // HEIC: never the original.
        Harness heic;
        sharefixtures::NcPage page;
        page.name        = "IMG_1.heic";
        page.ogImageType = "image/heic";
        heic.http->respond(sharePage(), html(page.build()));
        heic.http->respond(sharePreview2048(), status(404));
        heic.http->respond(sharePage() + a("/preview"), status(404));
        QVERIFY(!heic.load(sharePage()).ok);
        QCOMPARE(heic.http->count(), 3);
    }

    void shareGifDownloadToDav()
    {
        Harness h;
        sharefixtures::NcPage page;
        page.name        = "cat.gif";
        page.ogImageType = "image/gif";
        page.size        = 4000;
        h.http->respond(sharePage(), html(page.build()));
        h.http->respond(sharePage() + a("/download"), redirect(a("https://cloud.example.org/public.php/dav/files/") + a(sharefixtures::kToken) + a("/?accept=zip"), 303));
        h.http->respond(a("https://cloud.example.org/public.php/dav/files/") + a(sharefixtures::kToken) + a("/?accept=zip"), media("image/gif", gif(3)));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Gif);
        QCOMPARE(h.http->count(), 3);
        QCOMPARE(h.http->requests().at(1).accept, QByteArray("image/gif,image/*;q=0.8"));
    }

    void shareDownloadRedirectRefused_data()
    {
        QTest::addColumn<QString>("location");
        QTest::newRow("another host") << a("https://cdn.example.net/public.php/dav/files/") + a(sharefixtures::kToken);
        QTest::newRow("another path") << a("/remote.php/webdav/cat.gif");
        QTest::newRow("another token") << a("/public.php/dav/files/Other123456789");
        QTest::newRow("a private address") << a("https://10.0.0.1/x.gif");
    }

    void shareDownloadRedirectRefused()
    {
        QFETCH(QString, location);
        Harness h;
        sharefixtures::NcPage page;
        page.name        = "cat.gif";
        page.ogImageType = "image/gif";
        h.http->respond(sharePage(), html(page.build()));
        h.http->respond(sharePage() + a("/download"), redirect(location, 303));
        const Result r = h.load(sharePage());
        QVERIFY(!r.ok);
        QCOMPARE(r.error, a(errors::kRedirect));
        QCOMPARE(h.http->count(), 2); // the page and /download: never the place it pointed to
    }

    void shareVideoTooLargeNoRequest()
    {
        Harness h;
        sharefixtures::NcPage page;
        page.name        = "Recording.mp4";
        page.ogImageType = QByteArray();
        page.ogImage     = "https://cloud.example.org/index.php/apps/theming/icon?v=3d4c0f38";
        page.ogVideoType = "video/mp4";
        page.size        = 603869811;
        h.http->respond(sharePage(), html(page.build()));
        const Result r = h.load(sharePage());
        QVERIFY(!r.ok);
        QCOMPARE(r.error, a(errors::kTooLarge));
        QCOMPARE(r.size, qint64(603869811));
        QCOMPARE(r.kind, Kind::Video);
        QCOMPARE(h.http->urls(), QStringList{sharePage()}); // no media request, never the theming icon
        // Within the cap (caps.video when set): the video through /download, a Loop or Video by its probe.
        Harness v;
        page.size = 3 * 1024 * 1024;
        v.http->respond(sharePage(), html(page.build()));
        v.http->respond(sharePage() + a("/download"), media("video/mp4", mp4()));
        Caps caps  = clickCaps();
        caps.media = 1024 * 1024; // a picture's cap
        caps.video = 50 * 1024 * 1024;
        v.setAudio(true, 9000);
        const Result video = v.load(sharePage(), caps);
        QVERIFY2(video.ok, qPrintable(video.error));
        QCOMPARE(video.kind, Kind::Video);
        QCOMPARE(v.http->requests().at(1).accept, QByteArray("video/mp4,video/webm,video/quicktime,video/*;q=0.8"));
    }

    void shareVideoOgVideoNeverRequested()
    {
        // og:video names a private address: never requested; the derived /download is.
        Harness h;
        sharefixtures::NcPage page;
        page.name        = "clip.mp4";
        page.ogImageType = QByteArray();
        page.ogVideoType = "video/mp4";
        page.size        = 1000;
        QByteArray body  = page.build();
        body.replace(QByteArray("<meta property=\"og:video\" content=\"https://cloud.example.org/s/") + sharefixtures::kToken + "/download\"/>",
                     "<meta property=\"og:video\" content=\"http://10.0.0.1/x.mp4\"/>");
        QVERIFY(body.contains("10.0.0.1"));
        h.http->respond(sharePage(), html(body));
        h.http->respond(sharePage() + a("/download"), media("video/mp4", mp4()));
        QVERIFY(h.load(sharePage()).ok);
        for (const QString& url : h.http->urls())
            QVERIFY2(!url.contains(a("10.0.0.1")), qPrintable(url));
    }

    void shareHiddenDownloadCard()
    {
        Harness h;
        sharefixtures::NcPage page;
        page.menuDownload = false;
        h.http->respond(sharePage(), html(page.build()));
        const QString picture = a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + a(sharefixtures::kToken) + a("?x=1024&y=1024&a=1");
        h.http->respond(picture, media("image/jpeg", jpeg(800, 600)));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.title, a("top_shunt.jpg"));
        QCOMPARE(r.card.description, QString::fromUtf8("Picture \xC2\xB7 View only"));
        QVERIFY(r.card.site.isEmpty());
        QVERIFY(r.card.hasColor); // the page's theme-color under the Web rules
        QCOMPARE(r.card.color, qRgb(0x00, 0x67, 0x9e));
        QCOMPARE(r.card.layout, CardLayout::Large);
        QVERIFY(QFile::exists(r.card.pictureFile));
        QCOMPARE(h.http->urls(), (QStringList{sharePage(), picture})); // never /download
    }

    void shareDownloadLimitNoRequests_data()
    {
        QTest::addColumn<int>("at");
        QTest::newRow("before the menu") << 0;
        QTest::newRow("after the menu") << 1;
        QTest::newRow("after 20 more states") << 2;
    }

    void shareDownloadLimitNoRequests()
    {
        QFETCH(int, at);
        Harness h;
        sharefixtures::NcPage page;
        page.limitAt = at;
        h.http->respond(sharePage(), html(page.build()));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QVERIFY(r.card.pictureFile.isEmpty());
        QCOMPARE(h.http->urls(), QStringList{sharePage()}); // zero requests after the page
    }

    void shareIncompleteReadNoRequests()
    {
        // The block runs past the page cap (16 KiB here): a card, nothing else.
        Harness h;
        sharefixtures::NcPage page;
        page.after = 400;
        h.http->respond(sharePage(), html(page.build()));
        Caps caps = clickCaps();
        caps.page = 16 * 1024;
        const Result r = h.load(sharePage(), caps);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QVERIFY(r.card.pictureFile.isEmpty());
        QCOMPARE(h.http->count(), 1);
    }

    void shareEarlyStop()
    {
        // The read ends at the first checkpoint after the block's end: 300 KB of page after it are never read.
        Harness h;
        sharefixtures::NcPage page;
        page.blockEnd += QByteArray(300 * 1024, ' ');
        h.http->respond(sharePage(), html(page.build()));
        h.http->respond(sharePreview2048(), media("image/png", png(64, 64)));
        QVERIFY(h.load(sharePage()).ok);
        QVERIFY2(h.http->bytesRead() < 100 * 1024, qPrintable(QString::number(h.http->bytesRead())));
    }

    void sharePasswordAndGone()
    {
        Harness h;
        h.http->respond(sharePage(), redirect(a("/s/") + a(sharefixtures::kToken) + a("/authenticate/showShare"), 303));
        h.http->respond(sharePage() + a("/authenticate/showShare"), html("<!DOCTYPE html><html><head><title>Cloud</title></head><body><form></form></body></html>"));
        const Result r = h.load(sharePage());
        QVERIFY(!r.ok);
        QCOMPARE(r.error, a(errors::kNothing));
        QCOMPARE(h.http->count(), 2); // the page and its same-host redirect to the form; nothing else
        QCOMPARE(r.requests, 1);
        Harness gone;
        gone.http->respond(sharePage(), status(404, "text/html"));
        QCOMPARE(gone.load(sharePage()).error, a(errors::kNotFound));
    }

    void shareRedirectToAnotherHostIsWeb()
    {
        // A shortener or a moved instance: the share plan ends; the Web plan's rules on the final page (its card and
        // host), no media request.
        Harness h;
        h.http->respond(sharePage(), redirect(a("https://moved.example.net/s/") + a(sharefixtures::kToken), 302));
        h.http->respond(a("https://moved.example.net/s/") + a(sharefixtures::kToken), html(webCard(a("Landing"), a("A page"), QString())));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.title, a("Landing"));
        QCOMPARE(r.finalHost, a("moved.example.net"));
        QCOMPARE(h.http->count(), 2); // the share page and the Web page it went to
        // Even when the page there proves a share: a Web card, never a share request (no derived preview, no download)
        // on the other host, and its og:image (that share's own preview) isn't requested either: a read of it would use
        // up a download of a limited share (the Web picture rule, shares::isShareFileAddress).
        Harness moved;
        sharefixtures::NcPage there;
        there.host = "moved.example.net";
        moved.http->respond(sharePage(), redirect(a("https://moved.example.net/s/") + a(sharefixtures::kToken), 302));
        moved.http->respond(a("https://moved.example.net/s/") + a(sharefixtures::kToken), html(there.build()));
        moved.http->respond(a("https://moved.example.net/s/") + a(sharefixtures::kToken) + a("/preview"), media("image/jpeg", jpeg(640, 480)));
        const Result web = moved.load(sharePage());
        QVERIFY2(web.ok, qPrintable(web.error));
        QCOMPARE(web.kind, Kind::Card);
        QCOMPARE(web.card.title, a("top_shunt.jpg")); // og:title by the Web rules
        QVERIFY(web.card.pictureFile.isEmpty());
        QCOMPARE(web.finalHost, a("moved.example.net"));
        QCOMPARE(moved.http->urls(), (QStringList{sharePage(), a("https://moved.example.net/s/") + a(sharefixtures::kToken)}));
        // Live pass: a page there that is gone shows nothing, as for any Web link (never the share's "file isn't on the
        // website any more" strip); a server error keeps its own (transient) handling.
        Harness dead;
        dead.http->respond(sharePage(), redirect(a("https://go.example.net/s/") + a(sharefixtures::kToken), 301));
        dead.http->respond(a("https://go.example.net/s/") + a(sharefixtures::kToken), status(404, "text/plain"));
        const Result gone = dead.load(sharePage());
        QVERIFY(!gone.ok);
        QCOMPARE(gone.error, a(errors::kNothing));
        QVERIFY(linkstate::isNothingError(gone.error));
        QCOMPARE(dead.http->count(), 2);
        Harness down;
        down.http->respond(sharePage(), redirect(a("https://go.example.net/s/") + a(sharefixtures::kToken), 301));
        down.http->respond(a("https://go.example.net/s/") + a(sharefixtures::kToken), status(500, "text/html"));
        QCOMPARE(down.load(sharePage()).error, a(errors::kServerError));
        // A redirect to a private address: no request to it.
        Harness lan;
        lan.http->respond(sharePage(), redirect(a("https://192.168.1.10/s/") + a(sharefixtures::kToken), 302));
        QVERIFY(!lan.load(sharePage()).ok);
        QCOMPARE(lan.http->count(), 1);
    }

    void shareSameHostPathMove()
    {
        // /owncloud -> /nextcloud on the same host: the plan goes on with the final prefix.
        Harness h;
        sharefixtures::NcPage page;
        page.path = QByteArray("/nextcloud/index.php/s/") + sharefixtures::kToken;
        h.http->respond(sharePage("/owncloud/index.php/s/"), redirect(a("/nextcloud/index.php/s/") + a(sharefixtures::kToken), 302));
        h.http->respond(sharePage("/nextcloud/index.php/s/"), html(page.build()));
        const QString preview = a("https://cloud.example.org/nextcloud/index.php/apps/files_sharing/publicpreview/") + a(sharefixtures::kToken)
                                + a("?x=2048&y=2048&a=1");
        h.http->respond(preview, media("image/jpeg", jpeg(64, 64)));
        const Result r = h.load(sharePage("/owncloud/index.php/s/"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(h.http->urls().last(), preview);
    }

    void shareUnconfirmedIsWebCard()
    {
        // Not proven a share (no token marker): the generic web card rules on the same bytes; with no card, nothing.
        Harness h;
        h.http->respond(sharePage(), html(webCard(a("Something else"), a("Not a share"), a("https://img.example.net/x.jpg"))));
        h.http->respond(a("https://img.example.net/x.jpg"), media("image/jpeg", jpeg(500, 300)));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.title, a("Something else"));
        QCOMPARE(h.http->count(), 2); // the page and its picture under the Web rules (another host is fine there)
        Harness none;
        none.http->respond(sharePage(), html("<!DOCTYPE html><html><head><title>App</title></head><body><div id=\"app\"></div></body></html>"));
        QCOMPARE(none.load(sharePage()).error, a(errors::kNothing));
    }

    void shareUnconfirmedNeverLoadsSharePreviews()
    {
        // A real share page that can't be confirmed (its markers past the cap, NC <= 30 with a large page) whose og:image
        // is the share's own preview: never requested (any file read, a preview too, uses up a download of a limited
        // share, design2 2.4); a text card from the same bytes.
        const QString preview = a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + a(sharefixtures::kToken)
                                + a("?x=1920&y=1080&a=1");
        for (const QString& image : {preview, sharePage() + a("/preview"), sharePage() + a("/download"),
                                     a("https://cloud.example.org/public.php/dav/files/") + a(sharefixtures::kToken) + a("/x.jpg")}) {
            Harness h;
            h.http->respond(sharePage(), html(webCard(a("Holiday"), a("A shared picture"), image)));
            h.http->respond(image, media("image/jpeg", jpeg(640, 480)));
            const Result r = h.load(sharePage());
            QVERIFY2(r.ok, qPrintable(r.error + QLatin1Char(' ') + image));
            QCOMPARE(r.kind, Kind::Card);
            QVERIFY(r.card.pictureFile.isEmpty());
            QCOMPARE(h.http->count(), 1);
        }
        // The same rule for any web page's picture (a custom share name is a Web link: its page shows the preview).
        Harness web;
        const QString custom = a("https://cloud.example.org/s/holiday-pictures-2026");
        web.http->respond(custom, html(webCard(a("Holiday"), a("A shared picture"), preview)));
        web.http->respond(preview, media("image/jpeg", jpeg(640, 480)));
        const Result r = web.load(custom);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.card.pictureFile.isEmpty());
        QCOMPARE(web.http->count(), 1);
    }

    void shareWebCardsOff()
    {
        // Show cards for other web pages off (Caps::webCards): a share link that turns out to be another web page ends
        // there with kWebOff after the share page itself (design2 2.3/2.7: "exactly as a Web link"), never remembered.
        Caps off     = clickCaps();
        off.webCards = false;
        // (a) A redirect to another website (a shortener, an app's link): no request to it, its host for the sender (r=x).
        Harness away;
        away.http->respond(sharePage(), redirect(a("https://tracker.example.net/landing"), 302));
        away.http->respond(a("https://tracker.example.net/landing"), html(webCard(a("Landing"), a("A page"), a("https://img.example.com/x.jpg"))));
        away.http->respond(a("https://img.example.com/x.jpg"), media("image/jpeg", jpeg(500, 300)));
        Result r = away.load(sharePage(), off);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, a(errors::kWebOff));
        QCOMPARE(r.finalHost, a("tracker.example.net"));
        QCOMPARE(away.http->urls(), QStringList{sharePage()});
        QVERIFY(linkstate::isNothingError(r.error)); // no slot, no error strip
        // Another host of the same website: the same, without a final host (the sender says a plain LPXNO).
        Harness same;
        same.http->respond(sharePage(), redirect(a("https://www.example.org/landing"), 302));
        r = same.load(sharePage(), off);
        QCOMPARE(r.error, a(errors::kWebOff));
        QVERIFY(r.finalHost.isEmpty());
        QCOMPARE(same.http->count(), 1);
        // (b) A page that isn't proven a share: no picture it names.
        Harness page;
        page.http->respond(sharePage(), html(webCard(a("Something else"), a("Not a share"), a("https://img.example.net/x.jpg"))));
        page.http->respond(a("https://img.example.net/x.jpg"), media("image/jpeg", jpeg(500, 300)));
        r = page.load(sharePage(), off);
        QCOMPARE(r.error, a(errors::kWebOff));
        QCOMPARE(page.http->count(), 1);
        // Never remembered: with the switch on again the same cache folder loads it at once.
        r = page.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(page.http->count(), 3);
        // A proven share's media loads with the switch off as always (the switch is for text cards, design2 2.7).
        Harness shown;
        sharefixtures::NcPage share;
        shown.http->respond(sharePage(), html(share.build()));
        shown.http->respond(sharePreview2048(), media("image/jpeg", jpeg(640, 480)));
        r = shown.load(sharePage(), off);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(shown.http->urls(), (QStringList{sharePage(), sharePreview2048()}));
    }

    void shareFolderAndPdfCards()
    {
        Harness h;
        sharefixtures::NcPage folder;
        folder.name        = "Team pictures";
        folder.view        = "public-share";
        folder.ogImageType = QByteArray();
        folder.ogImage     = "https://cloud.example.org/apps/theming/icon?v=8126b454";
        folder.size        = 626383444;
        h.http->respond(sharePage(), html(folder.build()));
        const Result r = h.load(sharePage());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Card);
        QCOMPARE(r.card.description, QString::fromUtf8("Folder \xC2\xB7 ") + formatSize(626383444));
        QVERIFY(r.card.pictureFile.isEmpty()); // the theming icon is never loaded
        QCOMPARE(h.http->count(), 1);

        Harness p;
        sharefixtures::NcPage pdf;
        pdf.path        = QByteArray("/index.php/s/") + sharefixtures::kToken;
        pdf.name        = "02_mcmc.pdf";
        pdf.ogImageType = QByteArray();
        pdf.ogImage     = QByteArray("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + sharefixtures::kToken
                      + "?file=/Shared/02_mcmc.pdf&amp;x=256&amp;y=256";
        pdf.size        = 2249191;
        p.http->respond(sharePage("/index.php/s/"), html(pdf.build()));
        const QString picture = a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + a(sharefixtures::kToken) + a("?x=1024&y=1024&a=1");
        p.http->respond(picture, media("image/png", png(256, 362)));
        const Result card = p.load(sharePage("/index.php/s/"));
        QVERIFY2(card.ok, qPrintable(card.error));
        QCOMPARE(card.card.description, QString::fromUtf8("PDF document \xC2\xB7 2.1 MB"));
        QCOMPARE(card.card.layout, CardLayout::Thumb);
        QCOMPARE(p.http->urls(), (QStringList{sharePage("/index.php/s/"), picture}));
        // Cleaned names: controls and bidi marks go.
        Harness rtl;
        sharefixtures::NcPage named = pdf;
        named.name = QByteArray("a\xE2\x80\xAE" "fdp.exe\x07" ".pdf");
        rtl.http->respond(sharePage("/index.php/s/"), html(named.build()));
        rtl.http->respond(picture, media("image/png", png(64, 64)));
        const Result cleaned = rtl.load(sharePage("/index.php/s/"));
        QVERIFY2(cleaned.ok, qPrintable(cleaned.error));
        QCOMPARE(cleaned.card.title, a("afdp.exe.pdf"));
    }

    void shareOwnCloudPicture()
    {
        Harness h;
        const QString page = sharePage("/index.php/s/");
        h.http->respond(page, html(sharefixtures::ownCloudPage("logo & mark.png")));
        const QString preview = a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?file=/logo%20%26%20mark.png&t=")
                                + a(sharefixtures::kToken) + a("&x=2048&y=2048&a=1");
        h.http->respond(preview, media("image/png", png(120, 80)));
        const Result r = h.load(page);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.kind, Kind::Image);
        QCOMPARE(h.http->urls(), (QStringList{page, preview}));
        // Its preview gone: the PNG itself (200 on /download).
        Harness d;
        d.http->respond(page, html(sharefixtures::ownCloudPage("logo & mark.png")));
        d.http->respond(preview, status(404));
        d.http->respond(page + a("/download"), media("image/png", png(120, 80)));
        QVERIFY(d.load(page).ok);
        QCOMPARE(d.http->urls().last(), page + a("/download"));
    }

    void shareLinkForms()
    {
        // preview, publicpreview and DAV links: one media request for the link itself, same host only.
        Harness h;
        h.http->respond(sharePage() + a("/preview"), media("image/jpeg", jpeg(64, 64)));
        const Result preview = h.load(sharePage() + a("/preview"));
        QVERIFY2(preview.ok, qPrintable(preview.error));
        QCOMPARE(h.http->count(), 1);
        const QString pp = a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + a(sharefixtures::kToken) + a("?x=1024&y=1024&a=1");
        h.http->respond(pp, redirect(a("https://cdn.example.net/x.jpg"), 302));
        QCOMPARE(h.load(pp).error, a(errors::kRedirect));
        const QString dav = a("https://cloud.example.org/public.php/dav/files/") + a(sharefixtures::kToken) + a("/?accept=zip");
        h.http->respond(dav, media("application/zip", "PK\x03\x04 a folder"));
        QCOMPARE(h.load(dav).error, a(errors::kBadType));
        Harness v;
        v.http->respond(dav, media("video/mp4", mp4()));
        Caps caps  = clickCaps();
        caps.video = 50 * 1024 * 1024;
        const Result video = v.load(dav, caps);
        QVERIFY2(video.ok, qPrintable(video.error));
        QVERIFY(video.kind == Kind::Video || video.kind == Kind::Loop);
    }

    void logsHaveNoText()
    {
        Harness h;
        h.http->respond(a("https://news.example.net/story/3?secret=1"), html(webCard(a("Secret Title"), a("Private words"), a("https://img.example.net/pic-name.jpg"))));
        h.http->respond(a("https://img.example.net/pic-name.jpg"), media("image/jpeg", jpeg(64, 64)));
        QVERIFY(h.load(a("https://news.example.net/story/3?secret=1")).ok);
        QVERIFY(!h.logs.isEmpty());
        for (const QString& line : h.logs) {
            QVERIFY2(!line.contains(a("Secret")) && !line.contains(a("Private")) && !line.contains(a("pic-name")) && !line.contains(a("secret=")),
                     qPrintable(line));
            QVERIFY(line.contains(a("card, ")));
        }
    }
};

TSMEDIA_REGISTER_TEST(TestLinkPageNet)

#include "tst_linkpage_net.moc"
