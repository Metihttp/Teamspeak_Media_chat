// Unit tests for 2.5 link previews, Nextcloud and ownCloud 10 public shares (design2.md 2.2-2.7, N-T1..N-T4, N-T6):
// the shape rows of classify(), shares::parse and every derived address, the ownCloud file-name rule, the share
// page's markers on minimal synthesized pages (never copies of real pages: each mirrors the input structure the
// research saw), the initial-state block's start and end, the decision table row by row, and the card text.

#include <QtTest>

#include "linkpreview/linkurl.h"
#include "linkpreview/pagemeta.h"
#include "linkpreview/providers.h"
#include "linkpreview/shares.h"
#include "medialink.h"
#include "sharefixtures.h"
#include "testmain.h"

using namespace linkpreview;
using shares::Action;
using shares::FileType;
using namespace sharefixtures;

namespace {

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

QString u8(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

QString u(std::initializer_list<uint> codes)
{
    QString out;
    for (const uint code : codes)
        out += QString::fromUcs4(&code, 1);
    return out;
}

shares::Markers markersOf(const QByteArray& html)
{
    return shares::readMarkers(html, nullptr);
}

shares::PageFacts factsOf(const QByteArray& html, const QString& finalUrl = QString(), qint64 videoCap = 50LL * 1024 * 1024)
{
    shares::PageFacts f;
    f.finalUrl = QUrl(finalUrl.isEmpty() ? a("https://cloud.example.org/s/") + a(kToken) : finalUrl);
    f.link     = shares::parse(f.finalUrl);
    if (!f.link.isValid())
        f.link = shares::parse(QUrl(a("https://cloud.example.org/s/") + a(kToken)));
    f.markers  = markersOf(html);
    f.videoCap = videoCap;
    return f;
}

} // namespace

class TestShares : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup() { providers::setInactiveForTests({}); }

    // ---- N-T1: classify ---------------------------------------------------------------------------------------------

    void classifyVectors_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<bool>("share");
        QTest::addColumn<QString>("sub");
        const QString t = a(kToken);
        QTest::newRow("pretty 15") << a("https://cloud.example.org/s/") + t << true << a("page");
        QTest::newRow("index.php 12") << a("https://cloud.example.org/index.php/s/Ab3dE6gH9jK2") << true << a("page");
        QTest::newRow("index.php custom") << a("https://cloud.example.org/index.php/s/my-custom-share") << true << a("page");
        QTest::newRow("prefix") << a("https://cloud.example.org/nextcloud/index.php/s/") + t << true << a("page");
        QTest::newRow("two prefix segments") << a("https://cloud.example.org/a/b/index.php/s/") + t << true << a("page");
        QTest::newRow("three prefix segments") << a("https://cloud.example.org/a/b/c/index.php/s/") + t << false << QString();
        QTest::newRow("pretty 14") << a("https://cloud.example.org/s/Ab3dE6gH9jK2mN") << false << QString();
        QTest::newRow("pretty 16") << a("https://cloud.example.org/s/Ab3dE6gH9jK2mN5p") << false << QString();
        QTest::newRow("trailing slash") << a("https://cloud.example.org/s/") + t + a("/") << true << a("page");
        QTest::newRow("file id") << a("https://cloud.example.org/s/") + t + a("/123") << true << a("page");
        QTest::newRow("download") << a("https://cloud.example.org/s/") + t + a("/download") << true << a("download");
        QTest::newRow("index download") << a("https://cloud.example.org/owncloud/index.php/s/") + t + a("/download") << true << a("download");
        QTest::newRow("preview") << a("https://cloud.example.org/s/") + t + a("/preview") << true << a("preview");
        QTest::newRow("nc publicpreview") << a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + t + a("?x=1024&y=1024&a=1") << true
                                         << a("pp");
        QTest::newRow("nc publicpreview pretty") << a("https://cloud.example.org/apps/files_sharing/publicpreview/") + t << true << a("pp");
        QTest::newRow("oc publicpreview") << a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?file=/a.png&t=") + t + a("&x=200&y=200")
                                         << true << a("pp");
        QTest::newRow("oc publicpreview two t") << a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?t=") + t + a("&t=") + t << false
                                               << QString();
        QTest::newRow("oc publicpreview no t") << a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?file=/a.png") << false << QString();
        QTest::newRow("dav zip") << a("https://cloud.example.org/public.php/dav/files/") + t + a("/?accept=zip") << true << a("dav");
        QTest::newRow("dav") << a("https://cloud.example.org/cloud/public.php/dav/files/") + t << true << a("dav");
        QTest::newRow("telegram s") << a("https://t.me/s/abcdefghijklmno") << false << QString();
        QTest::newRow("reddit share") << a("https://www.reddit.com/r/x/s/") + t << false << QString();
        QTest::newRow("deezer short") << a("https://link.deezer.com/s/") + t << false << QString();
        QTest::newRow("github owns its host") << a("https://github.com/s/") + t << false << QString();
        QTest::newRow("download pdf") << a("https://cloud.example.org/s/") + t + a("/download/a.pdf") << false << QString();
        QTest::newRow("long pretty") << a("https://example.com/s/Ab3dE6gH9jK2mN5pQ8rS1tU4") << false << QString();
        QTest::newRow("http") << a("http://cloud.example.org/s/") + t << true << a("page");
        QTest::newRow("%2F in prefix") << a("https://cloud.example.org/a%2Fb/s/") + t << false << QString();
        QTest::newRow("dot dot prefix") << a("https://cloud.example.org/../index.php/s/") + t << false << QString();
        QTest::newRow("dot prefix") << a("https://cloud.example.org/a/./s/") + t << false << QString();
        QTest::newRow("dots prefix") << a("https://cloud.example.org/.../s/") + t << false << QString();
        QTest::newRow("a.b prefix") << a("https://cloud.example.org/a.b/s/") + t << true << a("page");
        QTest::newRow("token with dash pretty") << a("https://cloud.example.org/s/Ab3dE6gH9-K2mN5") << false << QString();
        QTest::newRow("index.php short token") << a("https://cloud.example.org/index.php/s/Ab3dE") << false << QString();
        QTest::newRow("lan name") << a("https://nextcloud.local/s/") + t << false << QString();
        QTest::newRow("ip literal") << a("https://192.168.1.10/s/") + t << false << QString();
    }

    void classifyVectors()
    {
        QFETCH(QString, href);
        QFETCH(bool, share);
        QFETCH(QString, sub);
        const Target t = classify(href);
        QCOMPARE(t.provider == QLatin1String("nextcloud"), share);
        if (!share)
            return;
        QCOMPARE(t.cls, LinkClass::MediaPage);
        QCOMPARE(t.kind, Kind::Image);
        QCOMPARE(t.sub, sub);
        QCOMPARE(t.itemId, shares::parse(t.page).token);
        QCOMPARE(t.expectedRequests, sub == QLatin1String("page") || sub == QLatin1String("download") ? 2 : 1);
        QVERIFY(providers::isActive(a("nextcloud")));
        QVERIFY(providers::hasHopLists(a("nextcloud")));
    }

    void classifyOtherClasses()
    {
        const QString t = a(kToken);
        // 2.4.0's file link of a share keeps its class and key.
        const Target gif = classify(a("https://cloud.example.org/s/") + t + a("/download/a.gif"));
        QCOMPARE(gif.cls, LinkClass::Direct);
        QCOMPARE(gif.key, keyOf(a("https://cloud.example.org/s/") + t + a("/download/a.gif")));
        QCOMPARE(classify(a("https://cloud.example.org/s/") + t + a("/download/a.pdf")).cls, LinkClass::None);
        QCOMPARE(classify(a("https://example.com/s/Ab3dE6gH9jK2mN5pQ8rS1tU4")).cls, LinkClass::None);
        // Three prefix segments, 14 or 16 letters: an ordinary web page.
        QCOMPARE(classify(a("https://cloud.example.org/a/b/c/index.php/s/") + t).cls, LinkClass::Web);
        QCOMPARE(classify(a("https://cloud.example.org/s/Ab3dE6gH9jK2mN")).cls, LinkClass::Web);
        // http and https: one key (page v1 of the https form); the query and the file id stay in the key.
        const Target https = classify(a("https://cloud.example.org/s/") + t);
        QCOMPARE(classify(a("http://cloud.example.org/s/") + t).key, https.key);
        QCOMPARE(https.key, keyOf(a("page:https://cloud.example.org/s/") + t));
        QVERIFY(classify(a("https://cloud.example.org/s/") + t + a("/123")).key != https.key);
        QVERIFY(classify(a("https://cloud.example.org/s/") + t + a("?dir=x")).key != https.key);
        // Release gate: switched off, its links are Web links again (D39).
        providers::setInactiveForTests({a("nextcloud")});
        QVERIFY(!providers::isActive(a("nextcloud")));
        QCOMPARE(classify(a("https://cloud.example.org/s/") + t).cls, LinkClass::Web);
        QCOMPARE(classify(a("https://cloud.example.org/s/") + t + a("/download")).cls, LinkClass::None); // a download: no card ever
    }

    void firstRequests()
    {
        const QString t = a(kToken);
        const providers::FirstRequest page = providers::firstRequestOf(classify(a("https://cloud.example.org/nextcloud/index.php/s/") + t + a("/download")));
        QCOMPARE(page.host, a("cloud.example.org"));
        QCOMPARE(page.lane, HostGate::Lane::Page);
        QCOMPARE(page.firstSegment, a("nextcloud"));
        const providers::FirstRequest preview = providers::firstRequestOf(classify(a("https://cloud.example.org/s/") + t + a("/preview")));
        QCOMPARE(preview.lane, HostGate::Lane::Media);
        QCOMPARE(preview.firstSegment, a("s"));
        const providers::FirstRequest dav = providers::firstRequestOf(classify(a("https://cloud.example.org/public.php/dav/files/") + t));
        QCOMPARE(dav.lane, HostGate::Lane::Media);
        QCOMPARE(dav.host, a("cloud.example.org"));
    }

    void boundHopLists()
    {
        const QUrl same(a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/x"));
        const QUrl other(a("https://evil.example.net/x.png"));
        // Unbound: never a host. Bound: the share's host only.
        QVERIFY(!providers::mediaHopAllowed(a("nextcloud"), same));
        QVERIFY(!providers::pageHopAllowed(a("nextcloud"), same));
        QVERIFY(providers::mediaHopAllowed(a("nextcloud"), same, a("cloud.example.org")));
        QVERIFY(providers::pageHopAllowed(a("nextcloud"), same, a("cloud.example.org")));
        QVERIFY(!providers::mediaHopAllowed(a("nextcloud"), other, a("cloud.example.org")));
        QVERIFY(!providers::mediaHopAllowed(a("nextcloud"), same, QString()));
        // Every other id ignores the bound host.
        QVERIFY(providers::mediaHopAllowed(a("imgur"), QUrl(a("https://i.imgur.com/x.png")), a("cloud.example.org")));
        QVERIFY(!providers::mediaHopAllowed(a("imgur"), same, a("cloud.example.org")));
        QVERIFY(providers::ids().contains(a("nextcloud")));
        QVERIFY(!providers::brandOf(a("nextcloud")).name); // no brand: the host is shown
    }

    // ---- N-T2: parse and the derived addresses ---------------------------------------------------------------------

    void parseAndAddresses()
    {
        const QString t = a(kToken);
        shares::ShareLink s = shares::parse(QUrl(a("http://Cloud.Example.org/nextcloud/index.php/s/Ab3dE6gH9jK2/123?dir=x")));
        QCOMPARE(s.form, shares::Form::Page);
        QCOMPARE(s.host, a("cloud.example.org"));
        QCOMPARE(s.prefix, a("/nextcloud"));
        QVERIFY(s.indexPhp);
        QCOMPARE(s.token, a("Ab3dE6gH9jK2"));
        QCOMPARE(shares::pageUrl(s).toString(QUrl::FullyEncoded), a("https://cloud.example.org/nextcloud/index.php/s/Ab3dE6gH9jK2"));
        QCOMPARE(shares::downloadUrl(s).toString(QUrl::FullyEncoded), a("https://cloud.example.org/nextcloud/index.php/s/Ab3dE6gH9jK2/download"));
        QCOMPARE(shares::previewUrl(s).toString(QUrl::FullyEncoded), a("https://cloud.example.org/nextcloud/index.php/s/Ab3dE6gH9jK2/preview"));
        QCOMPARE(shares::publicPreviewUrl(s, 2048).toString(QUrl::FullyEncoded),
                 a("https://cloud.example.org/nextcloud/index.php/apps/files_sharing/publicpreview/Ab3dE6gH9jK2?x=2048&y=2048&a=1"));

        s = shares::parse(QUrl(a("https://cloud.example.org/s/") + t + a("/")));
        QCOMPARE(s.prefix, QString());
        QVERIFY(!s.indexPhp);
        QCOMPARE(shares::pageUrl(s).toString(QUrl::FullyEncoded), a("https://cloud.example.org/s/") + t); // no slash
        QCOMPARE(shares::publicPreviewUrl(s, 1024).toString(QUrl::FullyEncoded),
                 a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + t + a("?x=1024&y=1024&a=1"));
        QCOMPARE(shares::ownCloudPreviewUrl(s, a("a&b.png"), 1024).toString(QUrl::FullyEncoded),
                 a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?file=/a%26b.png&t=") + t + a("&x=1024&y=1024&a=1"));
        QVERIFY(!shares::ownCloudPreviewUrl(s, a("../x.png"), 1024).isValid());

        for (const char* form : {"/download", "/preview"}) {
            const shares::ShareLink f = shares::parse(QUrl(a("https://cloud.example.org/s/") + t + a(form)));
            QCOMPARE(shares::pageUrl(f).toString(QUrl::FullyEncoded), a("https://cloud.example.org/s/") + t);
        }
        const shares::ShareLink pp = shares::parse(QUrl(a("https://cloud.example.org/x/apps/files_sharing/publicpreview?file=/a.png&t=") + t));
        QCOMPARE(pp.form, shares::Form::PublicPreview);
        QCOMPARE(pp.prefix, a("/x"));
        QCOMPARE(pp.token, t);
        const shares::ShareLink dav = shares::parse(QUrl(a("https://cloud.example.org/public.php/dav/files/") + t + a("/?accept=zip")));
        QCOMPARE(dav.form, shares::Form::Dav);
        QCOMPARE(shares::subOf(dav.form), a("dav"));
        QCOMPARE(shares::expectedRequestsOf(dav.form), 1);
    }

    void fileNameRule_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("encoded");
        QTest::newRow("amp") << a("a&b.png") << a("a%26b.png");
        QTest::newRow("hash") << a("a#b.png") << a("a%23b.png");
        QTest::newRow("percent") << a("100%.png") << a("100%25.png");
        QTest::newRow("plus") << a("a+b.png") << a("a%2Bb.png");
        QTest::newRow("space") << a("a b.png") << a("a%20b.png");
        QTest::newRow("equals question") << a("a=b?.png") << a("a%3Db%3F.png");
        QTest::newRow("unreserved") << a("A-z_0.9~x.png") << a("A-z_0.9~x.png");
        QTest::newRow("rtl override") << u({'g', 'n', 'p', 0x202E, '.', 'x'}) << a("gnp%E2%80%AE.x");
        QTest::newRow("arabic") << u({0x0635, 0x0648, 0x0631, 0x0629, '.', 'p', 'n', 'g'}) << a("%D8%B5%D9%88%D8%B1%D8%A9.png");
        QTest::newRow("control") << u({'a', 0x07, '.', 'p', 'n', 'g'}) << QString();
        QTest::newRow("del") << u({'a', 0x7F, '.', 'p', 'n', 'g'}) << QString();
        QTest::newRow("c1") << u({'a', 0x85, '.', 'p', 'n', 'g'}) << QString();
        QTest::newRow("nul") << u({'a', 0x00, 'b'}) << QString();
        QTest::newRow("slash") << a("a/b.png") << QString();
        QTest::newRow("backslash") << a("a\\b.png") << QString();
        QTest::newRow("dot") << a(".") << QString();
        QTest::newRow("dot dot") << a("..") << QString();
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("255 bytes") << QString(251, QLatin1Char('a')) + a(".png") << QString(251, QLatin1Char('a')) + a(".png");
        QTest::newRow("256 bytes") << QString(252, QLatin1Char('a')) + a(".png") << QString();
        QTest::newRow("256 bytes of UTF-8") << QString(128, QChar(0x0635)) << QString();
        QTest::newRow("lone surrogate") << u({'a'}) + QChar(0xD800) << QString();
    }

    void fileNameRule()
    {
        QFETCH(QString, name);
        QFETCH(QString, encoded);
        QCOMPARE(shares::encodeFileName(name), encoded);
    }

    void downloadHops()
    {
        const QString           t     = a(kToken);
        const shares::ShareLink share = shares::parse(QUrl(a("https://cloud.example.org/nc/s/") + t));
        QVERIFY(shares::downloadHopAllowed(share, shares::downloadUrl(share)));
        QVERIFY(shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/index.php/s/") + t + a("/download"))));
        QVERIFY(shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/public.php/dav/files/") + t)));
        QVERIFY(shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/public.php/dav/files/") + t + a("/"))));
        QVERIFY(shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/public.php/dav/files/") + t + a("/?accept=zip"))));
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/public.php/dav/files/") + t + a("/?accept=zip&x=1"))));
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/public.php/dav/files/") + t)));      // another prefix
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cdn.example.org/nc/public.php/dav/files/") + t)));      // another host
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/public.php/dav/files/Other123456789"))));
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/s/") + t + a("/authenticate/downloadshare"))));
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("http://cloud.example.org/nc/s/") + t + a("/download"))));
        QVERIFY(!shares::downloadHopAllowed(share, QUrl(a("https://cloud.example.org/nc/s/") + t + a("/download?x=1"))));
    }

    void baseAfterRedirects()
    {
        const QString           t    = a(kToken);
        const shares::ShareLink link = shares::parse(QUrl(a("https://cloud.example.org/s/") + t));
        // Pretty to index.php, and a path move on the same host: the final prefix and index.php.
        shares::ShareLink base = shares::baseAfter(link, QUrl(a("https://cloud.example.org/nextcloud/index.php/s/") + t));
        QCOMPARE(base.prefix, a("/nextcloud"));
        QVERIFY(base.indexPhp);
        QCOMPARE(shares::downloadUrl(base).toString(QUrl::FullyEncoded), a("https://cloud.example.org/nextcloud/index.php/s/") + t + a("/download"));
        // Another token, or no share page: the link's own.
        base = shares::baseAfter(link, QUrl(a("https://cloud.example.org/x/index.php/s/Other123456789")));
        QCOMPARE(base.prefix, QString());
        QVERIFY(!base.indexPhp);
        base = shares::baseAfter(link, QUrl(a("https://cloud.example.org/index.php/login")));
        QCOMPARE(base.prefix, QString());
    }

    void previewAddresses()
    {
        const QString           t     = a(kToken);
        const shares::ShareLink share = shares::parse(QUrl(a("https://cloud.example.org/index.php/s/") + t));
        QVERIFY(shares::isPreviewAddress(QUrl(a("https://cloud.example.org/s/") + t + a("/preview")), share));
        QVERIFY(shares::isPreviewAddress(QUrl(a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/") + t + a("?file=/a.pdf&x=256&y=256")),
                                         share));
        QVERIFY(shares::isPreviewAddress(QUrl(a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview?file=/a.png&t=") + t + a("&x=200")),
                                         share));
        QVERIFY(!shares::isPreviewAddress(QUrl(a("https://cloud.example.org/index.php/apps/theming/icon?v=1")), share));
        QVERIFY(!shares::isPreviewAddress(QUrl(a("https://cloud.example.org/core/img/favicon-fb.png")), share));
        QVERIFY(!shares::isPreviewAddress(QUrl(a("https://cloud.example.org/index.php/apps/files_sharing/publicpreview/Other123456789")), share));
        QVERIFY(!shares::isPreviewAddress(QUrl(a("https://other.example.org/s/") + t + a("/preview")), share));
        QVERIFY(!shares::isPreviewAddress(QUrl(), share));
    }

    // ---- N-T3: the markers ------------------------------------------------------------------------------------------

    void nc35Image()
    {
        const QByteArray html = NcPage().build();
        const shares::Markers m = markersOf(html);
        QVERIFY(m.blockStarted);
        QVERIFY(m.blockEnded);
        QVERIFY(m.hasStateToken);
        QCOMPARE(m.stateToken, a(kToken));
        QCOMPARE(m.view, a("public-file-share"));
        QCOMPARE(m.stateName, a("top_shunt.jpg"));
        QCOMPARE(m.isFileRequest, 0);
        QCOMPARE(m.permissions, qint64(1));
        QVERIFY(m.menuSeen);
        QVERIFY(m.menuDownload);
        QCOMPARE(m.menuSize, qint64(1757588));
        QCOMPARE(m.ogImageType, a("image/jpeg"));
        QCOMPARE(m.ogUrls, QStringList{a("https://cloud.example.org/s/") + a(kToken)});
        QVERIFY(!m.downloadLimit);
        QVERIFY(shares::markersComplete(m));
        const shares::Decision d = shares::decide(factsOf(html));
        QCOMPARE(d.action, Action::Picture);
        QCOMPARE(d.type, FileType::Picture);
        QCOMPARE(d.size, qint64(1757588));
        QVERIFY(!d.ownCloud);
        QVERIFY(shares::isWebPicture(m));
    }

    void completeOnlyAfterTheBlock()
    {
        const NcPage     page;
        const QByteArray upToLastState = page.head() + page.block();
        shares::Markers  m             = markersOf(upToLastState);
        QVERIFY(m.blockStarted);
        QVERIFY(!m.blockEnded);
        QVERIFY(!shares::markersComplete(m)); // a limit could still follow
        QVERIFY(shares::insideBlock(m));
        m = markersOf(upToLastState + "\t</div>");
        QVERIFY(m.blockEnded);
        QVERIFY(shares::markersComplete(m));
        // A following <div> ends it too.
        NcPage divEnd;
        divEnd.blockEnd = "<div id=\"next\">";
        QVERIFY(shares::markersComplete(markersOf(divEnd.head() + divEnd.block() + divEnd.blockEnd)));
        // White space and a comment between the inputs: the block goes on.
        NcPage spaced;
        spaced.betweenInputs = "\n\n   <!-- a comment <div> -->\n\t";
        spaced.limitAt       = 2;
        m                    = markersOf(spaced.build());
        QVERIFY(m.downloadLimit);
        QVERIFY(m.blockEnded);
        // Cut before anything of the block: not complete, not inside it.
        m = markersOf(page.head());
        QVERIFY(!m.blockStarted);
        QVERIFY(!shares::markersComplete(m));
        QVERIFY(!shares::insideBlock(m));
    }

    void downloadLimitAnywhere_data()
    {
        QTest::addColumn<int>("at");
        QTest::newRow("before the menu") << 0;
        QTest::newRow("right after the menu") << 1;
        QTest::newRow("after the menu and 20 states") << 2;
    }

    void downloadLimitAnywhere()
    {
        QFETCH(int, at);
        NcPage page;
        page.limitAt              = at;
        const QByteArray all      = page.build();
        const QByteArray noEnd    = page.head() + page.block();
        QVERIFY(!shares::markersComplete(markersOf(noEnd)));
        const shares::Markers m = markersOf(all);
        QVERIFY(m.downloadLimit);
        QVERIFY(shares::markersComplete(m));
        const shares::Decision d = shares::decide(factsOf(all));
        QCOMPARE(d.action, Action::Card);
        QVERIFY(d.noFiles);
        QCOMPARE(d.reason, a("limit"));
        // Every prefix of the page that is complete already holds the limit (the read never stops before it).
        for (int cut = 1024; cut < all.size(); cut += 1024) {
            const shares::Markers part = markersOf(all.left(cut));
            if (shares::markersComplete(part))
                QVERIFY2(part.downloadLimit, qPrintable(QString::number(cut)));
        }
    }

    void incompleteInsideTheBlock()
    {
        // The page's cap inside the block: card only.
        NcPage           page;
        const QByteArray cut = page.head() + page.block().left(page.block().size() - 40);
        shares::PageFacts f  = factsOf(cut);
        QVERIFY(shares::insideBlock(f.markers));
        shares::Decision d = shares::decide(f);
        QCOMPARE(d.action, Action::Card);
        QVERIFY(d.noFiles);
        QCOMPARE(d.reason, a("incomplete"));
        // The tokenizer's tag limit inside the block.
        NcPage many;
        many.after          = 5000;
        const QByteArray big = many.build();
        f                    = factsOf(big);
        QVERIFY(f.markers.tagLimit);
        QVERIFY(shares::insideBlock(f.markers));
        d = shares::decide(f);
        QCOMPARE(d.action, Action::Card);
        QVERIFY(d.noFiles);
    }

    void nc34Video()
    {
        NcPage page;
        page.name        = "Recording_1920x1080.mp4";
        page.ogImage     = "https://cloud.example.org/index.php/apps/theming/icon?v=3d4c0f38";
        page.ogImageType = QByteArray();
        page.ogVideoType = "video/mp4";
        page.size        = 603869811;
        const shares::Decision big = shares::decide(factsOf(page.build(), QString(), 25LL * 1024 * 1024));
        QCOMPARE(big.action, Action::TooLarge);
        QCOMPARE(big.size, qint64(603869811));
        page.size                    = 4 * 1024 * 1024;
        const shares::Decision small = shares::decide(factsOf(page.build(), QString(), 25LL * 1024 * 1024));
        QCOMPARE(small.action, Action::Video);
        // Data saver (a cap of 0): too large whatever the size.
        QCOMPARE(shares::decide(factsOf(page.build(), QString(), 0)).action, Action::TooLarge);
    }

    void nc31Folder()
    {
        NcPage page;
        page.name        = "LEDGAR";
        page.view        = "public-share";
        page.ogImageType = QByteArray();
        page.ogImage     = "https://cloud.example.org/index.php/apps/theming/icon?v=1";
        page.size        = 3249811888LL;
        const shares::Decision d = shares::decide(factsOf(page.build()));
        QCOMPARE(d.action, Action::Card);
        QCOMPARE(d.type, FileType::Folder);
        QCOMPARE(d.size, qint64(3249811888LL));
        QCOMPARE(shares::cardDescription(d.type, d.size, d.viewOnly), u8("Folder · 3.0 GB"));
    }

    void nc26OldTemplate()
    {
        const QByteArray      html = ncOldTemplate(true);
        const shares::Markers m    = markersOf(html);
        QVERIFY(m.blockStarted);
        QVERIFY(m.blockEnded);
        QVERIFY(!m.hasStateToken);
        QCOMPARE(m.templateToken, a(kToken));
        QCOMPARE(m.mimetype, a("video/mp4"));
        QCOMPARE(m.filesize, qint64(748121190));
        QVERIFY(m.hideDownload);
        QVERIFY(shares::markersComplete(m));
        // Complete only once the template's inputs came: the block's end alone isn't enough.
        const int at = html.indexOf("name=\"sharingToken\"");
        QVERIFY(!shares::markersComplete(markersOf(html.left(at))));
        const shares::Decision d = shares::decide(factsOf(html, a("https://cloud.example.org/index.php/s/") + a(kToken)));
        QCOMPARE(d.action, Action::Card);
        QVERIFY(d.viewOnly);
        QCOMPARE(d.type, FileType::Video);
        QCOMPARE(shares::cardDescription(d.type, d.size, d.viewOnly), u8("Video · 713 MB · View only"));
        // The same share with downloads allowed: the video (over a 25 MB cap: too large).
        QCOMPARE(shares::decide(factsOf(ncOldTemplate(false), a("https://cloud.example.org/index.php/s/") + a(kToken), 25LL * 1024 * 1024)).action,
                 Action::TooLarge);
    }

    void markersBeyondTheCap()
    {
        // NC 26 with the text app: the template inputs after 512 KiB. A read cut at the cap holds no token: unconfirmed.
        const QByteArray html = ncOldTemplate(false, 600 * 1024);
        const QByteArray read = html.left(512 * 1024);
        const shares::Markers m = markersOf(read);
        QVERIFY(!m.hasTemplateToken);
        QVERIFY(shares::insideBlock(m));
        QCOMPARE(shares::decide(factsOf(read, a("https://cloud.example.org/index.php/s/") + a(kToken))).action, Action::Unconfirmed);
        QCOMPARE(shares::decide(factsOf(html, a("https://cloud.example.org/index.php/s/") + a(kToken))).action, Action::TooLarge); // confirmed: 713 MB
    }

    void ownCloudThemed()
    {
        const QByteArray      html = ownCloudPage();
        const shares::Markers m    = markersOf(html);
        QVERIFY(!m.blockStarted);
        QCOMPARE(m.ogUrls.size(), 2);
        QCOMPARE(m.templateName, a("logo & mark.png"));
        QVERIFY(shares::markersComplete(m));
        const shares::Decision d = shares::decide(factsOf(html, a("https://cloud.example.org/index.php/s/") + a(kToken)));
        QCOMPARE(d.action, Action::Picture);
        QVERIFY(d.ownCloud);
        QCOMPARE(d.name, a("logo & mark.png"));
        QCOMPARE(d.size, qint64(32812));
        // Without the share's own og:url (only the theme's): not proven.
        QByteArray noShareUrl = html;
        noShareUrl.replace(QByteArray("<meta property=\"og:url\" content=\"https://cloud.example.org/index.php/s/") + kToken + "\"/>", "");
        QCOMPARE(shares::decide(factsOf(noShareUrl, a("https://cloud.example.org/index.php/s/") + a(kToken))).action, Action::Unconfirmed);
    }

    void passwordPage()
    {
        const QByteArray html = "<!DOCTYPE html><html><head><title>Cloud</title></head><body>"
                                + state("core-publicShareAuth", "{\"showPasswordReset\":false,\"shareType\":3}") + "<form></form></body></html>";
        QVERIFY(markersOf(html).passwordForm);
        shares::Decision d = shares::decide(factsOf(html));
        QCOMPARE(d.action, Action::Nothing);
        QCOMPARE(d.reason, a("password"));
        d = shares::decide(factsOf("<html><body></body></html>", a("https://cloud.example.org/s/") + a(kToken) + a("/authenticate/showShare")));
        QCOMPARE(d.reason, a("password"));
        d = shares::decide(factsOf("<html><body></body></html>", a("https://cloud.example.org/index.php/s/") + a(kToken) + a("/authenticate")));
        QCOMPARE(d.reason, a("password"));
    }

    void notAShare()
    {
        // An SPA (oCIS, Immich-like) at /s/<15>: nothing proves a share.
        const QByteArray spa = "<!DOCTYPE html><html><head><title>App</title><meta property=\"og:title\" content=\"App\"></head><body><div id=\"app\"></div>"
                               "<script src=\"/js/app.js\"></script></body></html>";
        QCOMPARE(shares::decide(factsOf(spa)).action, Action::Unconfirmed);
        // base64 that isn't JSON, a token in another case, a sharingToken of another share.
        NcPage notJson;
        QByteArray html = notJson.build();
        html.replace(QByteArray("\"" + QByteArray(kToken) + "\"").toBase64(), QByteArray("not json at all").toBase64());
        QVERIFY(!markersOf(html).hasStateToken);
        QCOMPARE(shares::decide(factsOf(html)).action, Action::Unconfirmed);
        NcPage otherCase;
        otherCase.token = QByteArray(kToken).toLower();
        QCOMPARE(shares::decide(factsOf(otherCase.build())).action, Action::Unconfirmed);
        // og:url on another host.
        NcPage elsewhere;
        elsewhere.host = "mirror.example.net";
        QCOMPARE(shares::decide(factsOf(elsewhere.build())).action, Action::Unconfirmed);
    }

    void stateLimits()
    {
        // JSON over 64 KiB: ignored (as if absent).
        NcPage big;
        big.name = QByteArray(70 * 1024, 'x') + ".jpg";
        const shares::Markers m = markersOf(big.build());
        QVERIFY(!m.hasStateName);
        QVERIFY(m.hasStateToken);
        // Up to 64 KiB: read.
        NcPage fits;
        fits.name = QByteArray(60 * 1024, 'x') + ".jpg";
        QVERIFY(markersOf(fits.build()).hasStateName);
        // Typed reads: a number where a string belongs is ignored; the menu's size only as decimal digits.
        const QByteArray typed = "<html><head></head><body>" + state("files_sharing-sharingToken", "12345") + state("files_sharing-view", "[\"x\"]")
                                 + state("core-public-page-menu", "[{\"id\":\"download\",\"details\":\"12e3\"}]")
                                 + state("files_sharing-sharePermissions", "1.5") + "</body></html>";
        const shares::Markers t = markersOf(typed);
        QVERIFY(!t.hasStateToken);
        QVERIFY(!t.hasView);
        QVERIFY(t.menuDownload);
        QCOMPARE(t.menuSize, qint64(-1));
        QCOMPARE(t.permissions, qint64(-1));
        // Scalars wrapped for Qt 5's parser can't smuggle a second value.
        const shares::Markers two = markersOf("<html><body>" + state("files_sharing-sharingToken", "\"a\",\"b\"") + "</body></html>");
        QVERIFY(!two.hasStateToken);
    }

    void adversarialInputs()
    {
        const QByteArray token = state("files_sharing-sharingToken", "\"" + QByteArray(kToken) + "\"");
        for (const char* wrap : {"<!-- %1 -->", "<script>%1</script>", "<template>%1</template>", "<textarea>%1</textarea>", "<noscript>%1</noscript>"}) {
            QByteArray html = "<html><head></head><body>" + QByteArray(wrap).replace("%1", token) + "</body></html>";
            const shares::Markers m = markersOf(html);
            QVERIFY2(!m.hasStateToken, wrap);
            QVERIFY2(!m.blockStarted, wrap);
        }
        // A duplicate sharingToken: the first wins.
        const QByteArray dup = "<html><body>" + state("files_sharing-sharingToken", "\"first123456789\"") + token + "</body></html>";
        QCOMPARE(markersOf(dup).stateToken, a("first123456789"));
        // id and name disagree: the id wins; the name counts only on an input without an id.
        QByteArray idWins = "<html><body><input type=\"hidden\" id=\"other\" name=\"sharingToken\" value=\"" + QByteArray(kToken) + "\">"
                            "<input type=\"hidden\" id=\"filename\" name=\"x\" value=\"a.png\"><input type=\"hidden\" name=\"mimetype\" value=\"image/png\"></body></html>";
        shares::Markers m = markersOf(idWins);
        QVERIFY(!m.hasTemplateToken);
        QCOMPARE(m.templateName, a("a.png"));
        QCOMPARE(m.mimetype, a("image/png"));
        // An initial state through a name only: never.
        m = markersOf("<html><body><input type=\"hidden\" name=\"initial-state-files_sharing-sharingToken\" value=\"" + QByteArray("\"x\"").toBase64()
                      + "\"></body></html>");
        QVERIFY(!m.hasStateToken);
        // Before <body>: ignored, and it doesn't start the block.
        m = markersOf("<html><head>" + token + "</head><body><p>x</p></body></html>");
        QVERIFY(!m.hasStateToken);
        QVERIFY(!m.blockStarted);
        // Not hidden: not a marker.
        m = markersOf("<html><body><input type=\"text\" id=\"sharingToken\" value=\"" + QByteArray(kToken) + "\"></body></html>");
        QVERIFY(!m.hasTemplateToken);
        // Entities in a template value are decoded once.
        m = markersOf("<html><body><input type=\"hidden\" id=\"filename\" value=\"a &amp;amp; b.png\"></body></html>");
        QCOMPARE(m.templateName, a("a &amp; b.png"));
    }

    // ---- N-T4: the decision, row by row ------------------------------------------------------------------------------

    void decisionRows()
    {
        shares::PageFacts f = factsOf(NcPage().build());
        // 1. 404 / 410
        f.status = 404;
        QCOMPARE(shares::decide(f).action, Action::NotFound);
        f.status = 410;
        QCOMPARE(shares::decide(f).action, Action::NotFound);
        f.status = 200;
        // 2. the password form (also on a page that otherwise proves the share)
        f.markers.passwordForm = true;
        QCOMPARE(shares::decide(f).reason, a("password"));
        f.markers.passwordForm = false;
        // 3. a login segment
        shares::PageFacts login = f;
        login.finalUrl          = QUrl(a("https://cloud.example.org/index.php/login?redirect_url=x"));
        QCOMPARE(shares::decide(login).action, Action::Nothing);
        QCOMPARE(shares::decide(login).reason, a("login"));
        // 4. not confirmed: either proof missing
        shares::PageFacts noUrl = f;
        noUrl.markers.ogUrls.clear();
        QCOMPARE(shares::decide(noUrl).action, Action::Unconfirmed);
        shares::PageFacts noToken = f;
        noToken.markers.hasStateToken = false;
        QCOMPARE(shares::decide(noToken).action, Action::Unconfirmed);
        // 5. a file drop
        shares::PageFacts drop = f;
        drop.markers.isFileRequest = 1;
        QCOMPARE(shares::decide(drop).reason, a("drop"));
        drop.markers.isFileRequest = 0;
        drop.markers.permissions   = 4; // create only
        QCOMPARE(shares::decide(drop).action, Action::Nothing);
        drop.markers.permissions = 17; // read + share
        QCOMPARE(shares::decide(drop).action, Action::Picture);
        // 6. a download limit (before the folder and view-only rows)
        shares::PageFacts limit = f;
        limit.markers.downloadLimit = true;
        limit.markers.view          = a("public-share");
        QCOMPARE(shares::decide(limit).reason, a("limit"));
        // 7. a folder
        shares::PageFacts folder = f;
        folder.markers.view      = a("public-share");
        QCOMPARE(shares::decide(folder).type, FileType::Folder);
        QCOMPARE(shares::decide(folder).action, Action::Card);
        // 8. download hidden (NC 31+: no download entry in the menu)
        shares::PageFacts hidden = f;
        hidden.markers.menuDownload = false;
        hidden.markers.menuSize     = -1;
        const shares::Decision viewOnly = shares::decide(hidden);
        QCOMPARE(viewOnly.action, Action::Card);
        QVERIFY(viewOnly.viewOnly);
        QCOMPARE(viewOnly.type, FileType::Picture);
        QCOMPARE(shares::cardDescription(viewOnly.type, viewOnly.size, viewOnly.viewOnly), u8("Picture · View only"));
        // 9-11. picture, GIF, video
        shares::PageFacts gif = f;
        gif.markers.ogImageType = a("image/gif");
        QCOMPARE(shares::decide(gif).action, Action::Gif);
        shares::PageFacts video = f;
        video.markers.ogImageType.clear();
        video.markers.ogVideoType = a("video/webm");
        QCOMPARE(shares::decide(video).action, Action::Video);
        video.markers.menuSize = 60LL * 1024 * 1024;
        QCOMPARE(shares::decide(video).action, Action::TooLarge);
        video.markers.menuDownload = false; // no size known (and hidden): view only first
        QCOMPARE(shares::decide(video).action, Action::Card);
        // 12. anything else
        shares::PageFacts pdf = f;
        pdf.markers.ogImageType.clear();
        pdf.markers.stateName = a("02_mcmc.pdf");
        const shares::Decision card = shares::decide(pdf);
        QCOMPARE(card.action, Action::Card);
        QCOMPARE(card.type, FileType::Pdf);
        QCOMPARE(shares::cardDescription(card.type, 2249191, false), u8("PDF document · 2.1 MB"));
    }

    void typesByMimeAndName_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("type");
        QTest::newRow("jpeg") << a("image/jpeg") << a("x.bin") << int(FileType::Picture);
        QTest::newRow("heic name") << QString() << a("IMG_1.HEIC") << int(FileType::Picture);
        QTest::newRow("gif name") << QString() << a("a.gif") << int(FileType::Gif);
        QTest::newRow("mov") << a("video/quicktime") << QString() << int(FileType::Video);
        QTest::newRow("m4v name") << QString() << a("clip.m4v") << int(FileType::Video);
        QTest::newRow("svg") << a("image/svg+xml") << a("a.svg") << int(FileType::File);
        QTest::newRow("folder") << a("httpd/unix-directory") << a("dir") << int(FileType::Folder);
        QTest::newRow("docx") << QString() << a("a.docx") << int(FileType::Document);
        QTest::newRow("ods") << QString() << a("a.ods") << int(FileType::Spreadsheet);
        QTest::newRow("csv mime") << a("text/csv") << QString() << int(FileType::Spreadsheet);
        QTest::newRow("pptx") << QString() << a("a.pptx") << int(FileType::Presentation);
        QTest::newRow("tar.gz") << QString() << a("a.tar.gz") << int(FileType::Archive);
        QTest::newRow("zst") << QString() << a("a.zst") << int(FileType::Archive);
        QTest::newRow("opus") << QString() << a("a.opus") << int(FileType::Audio);
        QTest::newRow("audio mime") << a("audio/flac") << QString() << int(FileType::Audio);
        QTest::newRow("md") << QString() << a("README.md") << int(FileType::Text);
        QTest::newRow("unknown") << a("application/octet-stream") << a("model.pth") << int(FileType::File);
        QTest::newRow("no extension") << QString() << a("README") << int(FileType::File);
        QTest::newRow("unknown mime, known name") << a("application/x-unknown") << a("a.pdf") << int(FileType::Pdf);
    }

    void typesByMimeAndName()
    {
        QFETCH(QString, mime);
        QFETCH(QString, name);
        QFETCH(int, type);
        shares::Markers m;
        m.mimetype        = mime;
        m.hasMimetype     = !mime.isEmpty();
        m.templateName    = name;
        m.hasTemplateName = true;
        QCOMPARE(int(shares::typeOf(m)), type);
    }

    void webPictures()
    {
        shares::Markers m;
        m.stateName    = a("a.HEIC");
        m.hasStateName = true;
        QVERIFY(!shares::isWebPicture(m));
        m.stateName = a("a.JPG");
        QVERIFY(shares::isWebPicture(m));
        m.ogImageType = a("image/tiff"); // the type wins over the name
        QVERIFY(!shares::isWebPicture(m));
        m.ogImageType = a("image/webp");
        QVERIFY(shares::isWebPicture(m));
    }

    // ---- N-T6: the card text ----------------------------------------------------------------------------------------

    void cardText()
    {
        QCOMPARE(shares::cardDescription(FileType::Folder, 626383444, false), u8("Folder · ") + formatSize(626383444));
        QCOMPARE(shares::cardDescription(FileType::Archive, -1, false), a("Archive"));
        QCOMPARE(shares::cardDescription(FileType::File, 0, true), u8("File · 0 B · View only"));
        QCOMPARE(shares::cardDescription(FileType::Text, 1536, false), u8("Text file · 1.5 KB"));
        QCOMPARE(shares::typeLabel(FileType::Spreadsheet), a("Spreadsheet"));
        QCOMPARE(shares::typeLabel(FileType::Presentation), a("Presentation"));
        QCOMPARE(shares::typeLabel(FileType::Document), a("Document"));
        QCOMPARE(shares::typeLabel(FileType::Audio), a("Audio"));
        QCOMPARE(shares::typeLabel(FileType::Gif), a("GIF"));
        QCOMPARE(shares::typeLabel(FileType::Video), a("Video"));
    }
};

TSMEDIA_REGISTER_TEST(TestShares)

#include "tst_shares.moc"
