// 2.5 link previews, the chat's rules and cards (pure: linkstate, autoload, linkcard): what a slot of each link class
// shows (the quiet web class, cards, LPX, nothing, busy sites), the waits, caps and budgets per link, the intake model,
// why a slot is held, the OverAutoLimit fix, the Embed card's exact layout, zones and texts, the strips of the new
// kinds, and the colours. Nothing here touches the network or TeamSpeak.

#include <QFontMetricsF>
#include <QImage>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>

#include <cmath>

#include "inlinemedia.h"
#include "linkpreview/autoload.h"
#include "loopplayers.h"
#include "linkpreview/carddata.h"
#include "linkpreview/fetcher.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/linkstate.h"
#include "linkpreview/linkurl.h"
#include "linkpreview/providers.h"
#include "linkpreview/senderpipeline.h"
#include "tschat.h"
#include "serversettings.h"
#include "settings.h"
#include "testmain.h"
#include "uiutil.h"

using linkpreview::CardData;
using linkpreview::CardLayout;
using linkpreview::Kind;
using linkpreview::LinkClass;
using linkpreview::Target;
using linkcard::Look;
using linkstate::Local;

namespace {

Target target(const char* url)
{
    return linkpreview::classify(QString::fromLatin1(url));
}

Target webTarget()
{
    return target("https://www.example.com/news/a-story");
}

Target siteTarget()
{
    return target("https://vimeo.com/76979871");
}

Target mediaTarget()
{
    return target("https://giphy.com/gifs/teacher-anyone-ferris-bueller-8FhXc8w45aN32");
}

Target directTarget()
{
    return target("https://example.com/cat.png");
}

// design2 2: a Nextcloud share (the shape row "nextcloud").
Target shareTarget()
{
    return target("https://cloud.example.org/s/Ab3dE6gH9jK2mN5");
}

PreviewStyle styleOf(bool dark, int width, qreal dpr = 1.0, bool bubble = false)
{
    PreviewStyle s;
    s.dark     = dark;
    s.maxWidth = width;
    s.dpr      = dpr;
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(13);
    s.font = f;
    if (bubble)
        s.bubbles = {dark ? QColor(0x38, 0x3a, 0x40) : QColor(0xe8, 0xec, 0xf2)};
    return s;
}

linkcard::Card embed(CardLayout layout = CardLayout::Large, bool site = false)
{
    linkcard::Card c;
    c.look                 = Look::Embed;
    c.kind                 = Kind::Card;
    c.cls                  = site ? LinkClass::Site : LinkClass::Web;
    c.provider             = site ? QStringLiteral("vimeo") : QString();
    c.host                 = site ? QStringLiteral("vimeo.com") : QStringLiteral("www.example.com");
    c.data.site            = site ? QString() : QStringLiteral("Example News");
    c.data.author          = site ? QStringLiteral("A channel") : QString();
    c.data.title           = QStringLiteral("A story about something that happened today, told in a title long enough to wrap");
    c.data.description     = QStringLiteral("What happened, in a sentence or two, long enough to need more than one line in a card of this width.");
    c.data.layout          = layout;
    c.data.pictureWidth    = 1200;
    c.data.pictureHeight   = 630;
    return c;
}

} // namespace

class TestLinkUi25 : public QObject
{
    Q_OBJECT

  private slots:
    // ---- linkstate --------------------------------------------------------------------------------------------
    void decideTable_data()
    {
        QTest::addColumn<int>("cls");      // 0 web, 1 site, 2 media page, 3 direct
        QTest::addColumn<int>("local");    // Local
        QTest::addColumn<int>("localKind"); // Kind of what it turned out to be
        QTest::addColumn<bool>("clicked");
        QTest::addColumn<int>("server");   // 0 none, 1 LPX card, 2 LPX loop with file, 3 LP picture
        QTest::addColumn<int>("wait");     // linkstate::Wait
        QTest::addColumn<bool>("webCards");
        QTest::addColumn<bool>("senderNothing");
        QTest::addColumn<int>("look");     // linkcard::Look
        QTest::addColumn<int>("underlying");
        QTest::addColumn<bool>("media");
        const auto row = [](const char* name, int cls, Local local, Kind kind, bool clicked, int server, linkstate::Wait wait, bool web, bool nothing,
                            Look look, Look underlying, bool media) {
            QTest::newRow(name) << cls << static_cast<int>(local) << static_cast<int>(kind) << clicked << server << static_cast<int>(wait) << web << nothing
                                << static_cast<int>(look) << static_cast<int>(underlying) << media;
        };
        using W = linkstate::Wait;
        // The quiet web class.
        row("web idle: absent, click underneath", 0, Local::Idle, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Click, false);
        row("web waits for the sender: absent", 0, Local::Idle, Kind::None, false, 0, W::Sender, true, false, Look::Absent, Look::Loading, false);
        row("web own: absent", 0, Local::Idle, Kind::None, false, 0, W::Own, true, false, Look::Absent, Look::Loading, false);
        row("web auto due: absent", 0, Local::Idle, Kind::None, false, 0, W::Auto, true, false, Look::Absent, Look::Loading, false);
        row("web loading: absent", 0, Local::Loading, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Loading, false);
        row("web failed: absent", 0, Local::Failed, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Error, false);
        row("web cooling: absent", 0, Local::Cooling, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Loading, false);
        row("web clicked loading: strip", 0, Local::Loading, Kind::None, true, 0, W::None, true, false, Look::Loading, Look::Loading, false);
        row("web clicked failed: error", 0, Local::Failed, Kind::None, true, 0, W::None, true, false, Look::Error, Look::Error, false);
        row("web loaded card: embed", 0, Local::Loaded, Kind::Card, false, 0, W::None, true, false, Look::Embed, Look::Embed, false);
        row("web nothing: absent", 0, Local::Nothing, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Absent, false);
        row("web nothing after a click: error", 0, Local::Nothing, Kind::None, true, 0, W::None, true, false, Look::Error, Look::Error, false);
        row("web LPX card: embed", 0, Local::Idle, Kind::None, false, 1, W::None, true, false, Look::Embed, Look::Embed, false);
        row("web LPX beats nothing (R16)", 0, Local::Nothing, Kind::None, false, 1, W::None, true, false, Look::Embed, Look::Embed, false);
        row("web cards off: absent always", 0, Local::Loaded, Kind::Card, false, 1, W::None, false, false, Look::Absent, Look::Absent, false);
        row("web sender nothing: absent", 0, Local::Idle, Kind::None, false, 0, W::None, true, true, Look::Absent, Look::Absent, false);
        row("web media known: click strip", 0, Local::OverAutoLimit, Kind::Image, false, 0, W::None, true, false, Look::Click, Look::Click, false);
        // Provider classes: strips like 2.4.
        row("site idle: click", 1, Local::Idle, Kind::None, false, 0, W::None, true, false, Look::Click, Look::Click, false);
        row("site waits: loading", 1, Local::Idle, Kind::None, false, 0, W::Sender, true, false, Look::Loading, Look::Loading, false);
        row("site cooling: busy strip", 1, Local::Cooling, Kind::None, false, 0, W::None, true, false, Look::Loading, Look::Loading, false);
        row("site loaded: embed", 1, Local::Loaded, Kind::Card, false, 0, W::None, true, false, Look::Embed, Look::Embed, false);
        row("site nothing: absent", 1, Local::Nothing, Kind::None, false, 0, W::None, true, false, Look::Absent, Look::Absent, false);
        row("site sender nothing: absent", 1, Local::Idle, Kind::None, false, 0, W::None, true, true, Look::Absent, Look::Absent, false);
        row("site cards off: still shown", 1, Local::Idle, Kind::None, false, 0, W::None, false, false, Look::Click, Look::Click, false);
        row("media page LPX loop: media", 2, Local::Idle, Kind::None, false, 2, W::None, true, false, Look::Click, Look::Click, true);
        row("media page loaded loop: media", 2, Local::Loaded, Kind::Loop, false, 0, W::None, true, false, Look::Click, Look::Click, true);
        row("direct LP picture: media", 3, Local::Idle, Kind::None, false, 3, W::None, true, false, Look::Click, Look::Click, true);
        row("direct failed: error", 3, Local::Failed, Kind::None, false, 0, W::None, true, false, Look::Error, Look::Error, false);
    }
    void decideTable()
    {
        QFETCH(int, cls);
        QFETCH(int, local);
        QFETCH(int, localKind);
        QFETCH(bool, clicked);
        QFETCH(int, server);
        QFETCH(int, wait);
        QFETCH(bool, webCards);
        QFETCH(bool, senderNothing);
        QFETCH(int, look);
        QFETCH(int, underlying);
        QFETCH(bool, media);
        const Target t = cls == 0 ? webTarget() : cls == 1 ? siteTarget() : cls == 2 ? mediaTarget() : directTarget();
        linkstate::LocalState l;
        l.state   = static_cast<Local>(local);
        l.kind    = static_cast<Kind>(localKind);
        l.clicked = clicked;
        if (l.state == Local::Loaded) {
            if (l.kind == Kind::Card)
                l.card.title = QStringLiteral("A title");
            else
                l.mediaKey = QStringLiteral("local-media-key");
        }
        linkstate::ServerState s;
        if (server == 1) {
            s.announced  = true;
            s.extended   = true;
            s.kind       = Kind::Card;
            s.card.title = QStringLiteral("Sender's card");
        } else if (server == 2) {
            s.announced = true;
            s.extended  = true;
            s.kind      = Kind::Loop;
            s.hasMedia  = true;
            s.mainKey   = QStringLiteral("server-key");
            s.state     = MediaState::Ready;
        } else if (server == 3) {
            s.announced = true;
            s.mainKey   = QStringLiteral("server-key");
            s.state     = MediaState::Ready;
        }
        linkstate::Context ctx;
        ctx.webCards      = webCards;
        ctx.senderNothing = senderNothing;
        const linkstate::Decision d = linkstate::decide(t, false, l, s, static_cast<linkstate::Wait>(wait), ctx);
        QCOMPARE(d.media, media);
        if (!media) {
            QCOMPARE(static_cast<int>(d.look), look);
            QCOMPARE(static_cast<int>(d.underlying), underlying);
        }
        // Hidden is always the pill, whatever the class.
        QCOMPARE(linkstate::decide(t, true, l, s, static_cast<linkstate::Wait>(wait), ctx).look, Look::Hidden);
    }

    void lpxFitsPerClass()
    {
        for (const Kind k : {Kind::Image, Kind::Gif, Kind::Video, Kind::Loop, Kind::Card}) {
            QVERIFY(linkstate::lpxFits(k, webTarget(), true));
            QVERIFY(!linkstate::lpxFits(k, webTarget(), false)); // R17
            QVERIFY(linkstate::lpxFits(k, siteTarget(), false));
            QVERIFY(linkstate::lpxFits(k, mediaTarget(), true)); // R18: every new class takes every LPX kind
            QVERIFY(!linkstate::lpxFits(k, directTarget(), true)); // a 2.4 link never takes an LPX
        }
        QVERIFY(!linkstate::lpxFits(Kind::YouTube, webTarget(), true));
        // An LP only under a 2.4 link.
        QVERIFY(linkstate::lpFits(Kind::Image, directTarget()));
        QVERIFY(linkstate::lpFits(Kind::Gif, directTarget()));
        QVERIFY(!linkstate::lpFits(Kind::Image, mediaTarget()));
        QVERIFY(!linkstate::lpFits(Kind::Image, webTarget()));
    }

    // ---- 2.5: Nextcloud and ownCloud shares (design2 2.7, 2.10, 2.11; N-T6, N-T7) -------------------------------

    void shareCardsFollowWebCards()
    {
        const Target share = shareTarget();
        QCOMPARE(share.cls, LinkClass::MediaPage);
        QCOMPARE(share.provider, QStringLiteral("nextcloud"));
        // Its media always; its card only while cards for other web pages are on.
        for (const Kind k : {Kind::Image, Kind::Gif, Kind::Video, Kind::Loop}) {
            QVERIFY(linkstate::lpxFits(k, share, false));
            QVERIFY(linkstate::lpxFits(k, share, true));
        }
        QVERIFY(linkstate::lpxFits(Kind::Card, share, true));
        QVERIFY(!linkstate::lpxFits(Kind::Card, share, false));
        QVERIFY(linkstate::lpxFits(Kind::Card, mediaTarget(), false)); // other media pages: unchanged
        QVERIFY(linkpreview::providers::cardsFollowWebRules(share.cls, share.provider));
        QVERIFY(!linkpreview::providers::cardsFollowWebRules(LinkClass::MediaPage, QStringLiteral("tumblr")));

        // A share card loaded here: Embed with web cards on, nothing (no slot) with them off.
        linkstate::LocalState loaded;
        loaded.state      = Local::Loaded;
        loaded.kind       = Kind::Card;
        loaded.card.title = QStringLiteral("report.pdf");
        linkstate::Context on, off;
        off.webCards = false;
        QCOMPARE(linkstate::decide(share, false, loaded, linkstate::ServerState(), linkstate::Wait::None, on).look, Look::Embed);
        const linkstate::Decision hidden = linkstate::decide(share, false, loaded, linkstate::ServerState(), linkstate::Wait::None, off);
        QCOMPARE(hidden.look, Look::Absent);
        QCOMPARE(hidden.underlying, Look::Absent);
        QCOMPARE(linkstate::holdReason(hidden, loaded, off, true, true, true, linkpreview::HostGate::Verdict::Go), linkstate::HoldReason::Setting);
        // Its media, and its strips while it loads, whatever that setting says (the MediaPage rules).
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        QCOMPARE(linkstate::decide(share, false, loading, linkstate::ServerState(), linkstate::Wait::None, off).look, Look::Loading);
        linkstate::LocalState media;
        media.state    = Local::Loaded;
        media.kind     = Kind::Video;
        media.mediaKey = QStringLiteral("k");
        QVERIFY(linkstate::decide(share, false, media, linkstate::ServerState(), linkstate::Wait::None, off).media);
        // The sender's share card under web cards off: no slot, never loaded here by itself.
        linkstate::Context nothing = off;
        nothing.senderNothing      = true;
        QCOMPARE(linkstate::decide(share, false, linkstate::LocalState(), linkstate::ServerState(), linkstate::Wait::Sender, nothing).look, Look::Absent);
    }

    void shareLoadsFollowWebCardsSwitch()
    {
        // Review fix: a share link's load holds Caps::webCards as it started (with it off it ends at another web page,
        // with it on it loads that page). A running one whose value isn't the switch any more starts again
        // (ChatLinkPreviews::restartWebCardsLoads); nothing else does (a Web link's load stops with it off, R21).
        QVERIFY(linkstate::loadFollowsWebCards(shareTarget()));
        for (const Target& t : {webTarget(), siteTarget(), mediaTarget(), directTarget(), target("https://youtu.be/M7lc1UVf-VE")})
            QVERIFY2(!linkstate::loadFollowsWebCards(t), qPrintable(t.page.toString()));
        linkstate::LocalState fresh; // a load records the switch it started with: on by default
        QVERIFY(fresh.webCards);
        linkstate::LocalState loading;
        loading.state    = Local::Loading;
        loading.webCards = true;
        QVERIFY(linkstate::restartForWebCards(loading, shareTarget(), false)); // on -> off: it would load another site's page
        QVERIFY(!linkstate::restartForWebCards(loading, shareTarget(), true));
        loading.webCards = false;
        QVERIFY(linkstate::restartForWebCards(loading, shareTarget(), true)); // off -> on: it would end with kWebOff
        QVERIFY(!linkstate::restartForWebCards(loading, shareTarget(), false));
        QVERIFY(!linkstate::restartForWebCards(loading, webTarget(), true));
        QVERIFY(!linkstate::restartForWebCards(loading, mediaTarget(), true));
        for (const Local done : {Local::Idle, Local::Loaded, Local::Nothing, Local::Failed, Local::Cooling, Local::OverAutoLimit}) {
            linkstate::LocalState l = loading;
            l.state                 = done;
            QVERIFY(!linkstate::restartForWebCards(l, shareTarget(), true));
        }
    }

    void shareCardLook()
    {
        // The page's theme colour and a matching site name follow the web card's rules; a share card has no site line.
        CardData data;
        data.title    = QStringLiteral("report.pdf");
        data.site     = QStringLiteral("Evil Brand");
        data.color    = qRgb(0x00, 0x67, 0x9e);
        data.hasColor = true;
        const Target share = shareTarget();
        CardData     shown = linkstate::shownCardData(data, share.cls, share.host, false, share.provider);
        QVERIFY(shown.hasColor);
        QVERIFY(shown.site.isEmpty());
        QVERIFY(!linkstate::shownCardData(data, share.cls, share.host, true, share.provider).hasColor); // another website
        QVERIFY(!linkstate::shownCardData(data, mediaTarget().cls, mediaTarget().host, false, mediaTarget().provider).hasColor);
        // Strips say "Shared file" (no brand); the bar wears the page's colour like a web card's.
        linkcard::Card card;
        card.cls      = share.cls;
        card.provider = share.provider;
        card.kind     = Kind::Image;
        card.host     = share.host;
        card.look     = Look::Click;
        QCOMPARE(linkcard::stripLabel(card), QStringLiteral("Shared file"));
        QCOMPARE(linkcard::statusText(card), QStringLiteral("Shared file"));
        QVERIFY(linkcard::toolTip(card, linkcard::Zone::Load).contains(QStringLiteral("cloud.example.org, which sees your IP address")));
        card.look = Look::Embed;
        card.data = shown;
        linkcard::Card web = card;
        web.cls           = LinkClass::Web;
        web.provider.clear();
        const PreviewStyle style = styleOf(false, 400);
        QCOMPARE(linkcard::barColour(card, style), linkcard::barColour(web, style));
        card.data.hasColor = false;
        QVERIFY(linkcard::barColour(card, style) != linkcard::barColour(web, style)); // neutral grey without a colour
    }

    void shareCaps()
    {
        using namespace linkpreview::autoload;
        Settings s;
        s.autoDownloadImages  = true;
        s.autoDownloadMaxMB   = 15;
        s.videoAutoDownloadMB = 40;
        const Target share = shareTarget();
        // "lx" covers the share rows: no new cap.
        QCOMPARE(QString::fromLatin1(capFor(share.cls)), QStringLiteral("lx"));
        const linkpreview::Caps autoShare = autoCaps(share, s, QString());
        QCOMPARE(autoShare.media, 15LL * 1024 * 1024);
        QCOMPARE(autoShare.video, 40LL * 1024 * 1024); // videoAutoDownloadMB, as for other link videos
        QCOMPARE(mediaCapOf(autoShare, Kind::Video), 40LL * 1024 * 1024);
        QCOMPARE(mediaCapOf(autoShare, Kind::Loop), 40LL * 1024 * 1024);
        QCOMPARE(mediaCapOf(autoShare, Kind::Image), 15LL * 1024 * 1024);
        QCOMPARE(overLimitCap(share, Kind::Video, s, QString()), 40LL * 1024 * 1024);
        QCOMPARE(clickCaps(share).video, 50LL * 1024 * 1024);
        QCOMPARE(clickCaps(share).media, 25LL * 1024 * 1024);
        // Other links keep one media cap; a share loads with web cards off too (only its card waits for that switch).
        QCOMPARE(autoCaps(mediaTarget(), s, QString()).video, qint64(-1));
        QCOMPARE(mediaCapOf(autoCaps(mediaTarget(), s, QString()), Kind::Video), autoCaps(mediaTarget(), s, QString()).media);
        QVERIFY(autoShare.webCards);
        s.showWebCards = false;
        QCOMPARE(autoCaps(share, s, QString()).media, 15LL * 1024 * 1024);
        QVERIFY(!autoCaps(share, s, QString()).webCards); // ... but never another web page it turns out to be (kWebOff)
        QCOMPARE(autoCaps(webTarget(), s, QString()).page, qint64(0));
        // Data saver: no automatic load at all ("Load preview").
        s.dataSaver = true;
        QCOMPARE(autoCap(share, s, QString()), qint64(0));
        // The waits: a share waits for an "lx" sender's preview like every media page, never for a sender without it.
        SenderQueue queue;
        QCOMPARE(queue.add(QStringLiteral("s"), 1000, share, true), waitSec(share));
        QCOMPARE(queue.add(QStringLiteral("t"), 1000, share, false), 0);
    }

    void capNamesRule()
    {
        // design2 2.10: the parsers keep at most 8 caps of 1 to 4 lower-case letters. PeerDirectory::caps() never names
        // more than 8 (2.5.0: p r l lx lw; the 8th is reserved for one extension cap), each of that shape.
        QFile peers(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/peers.cpp"));
        QVERIFY(peers.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(peers.readAll());
        const int     start  = source.indexOf(QStringLiteral("QStringList PeerDirectory::caps() const"));
        QVERIFY(start >= 0);
        const int end = source.indexOf(QStringLiteral("\n}\n"), start);
        QVERIFY(end > start);
        const QString body = source.mid(start, end - start);
        static const QRegularExpression literal(QStringLiteral("fromLatin1\\(\"([^\"]*)\"\\)"));
        static const QRegularExpression shape(QStringLiteral("^[a-z]{1,4}$"));
        QStringList names;
        for (auto it = literal.globalMatch(body); it.hasNext();) {
            const QString name = it.next().captured(1);
            QVERIFY2(shape.match(name).hasMatch(), qPrintable(name));
            QVERIFY2(!names.contains(name), qPrintable(name));
            names << name;
        }
        QCOMPARE(names, (QStringList{QStringLiteral("p"), QStringLiteral("r"), QStringLiteral("l"), QStringLiteral("lx"), QStringLiteral("lw")}));
        QVERIFY(names.size() <= 7); // the 8th slot stays free for an extension cap
        QVERIFY(!body.contains(QStringLiteral("QStringLiteral"))); // the list goes into queued messages
    }

    void coolingRounds()
    {
        using linkstate::LocalState;
        const qint64 now = 1000000;
        LocalState   l;
        // A click: the busy line at once.
        LocalState clicked = linkstate::afterBusy(l, 30000, false, false, false, now);
        QCOMPARE(clicked.state, Local::Failed);
        QCOMPARE(clicked.error, QStringLiteral("The site is busy right now. Try again later."));
        // Automatic: two hard rounds, then the error.
        l = linkstate::afterBusy(l, 30000, false, false, true, now);
        QCOMPARE(l.state, Local::Cooling);
        QCOMPARE(l.retryAtMs, now + 30000);
        QCOMPARE(l.coolRetries, 1);
        QVERIFY(!linkstate::coolingDue(l, now + 29999));
        QVERIFY(linkstate::coolingDue(l, now + 30000));
        // Soft rounds and gate waits don't count.
        l = linkstate::afterBusy(l, 43000, true, false, true, now);
        QCOMPARE(l.coolRetries, 1);
        QVERIFY(l.softCooling);
        l = linkstate::afterBusy(l, 0, false, true, true, now);
        QCOMPARE(l.coolRetries, 1);
        QCOMPARE(l.retryAtMs, now + linkstate::kGateBusyRetryMs); // a gate wait without a time: 10 s
        l = linkstate::afterBusy(l, 30000, false, false, true, now);
        QCOMPARE(l.state, Local::Cooling);
        QCOMPARE(l.coolRetries, 2);
        l = linkstate::afterBusy(l, 30000, false, false, true, now);
        QCOMPARE(l.state, Local::Failed);
        // Ten minutes in all, whatever the rounds: Imgur's 600 s once, then the error.
        LocalState imgur = linkstate::afterBusy(LocalState(), 600000, false, false, true, now);
        QCOMPARE(imgur.state, Local::Cooling);
        imgur = linkstate::afterBusy(imgur, 1000, true, false, true, now);
        QCOMPARE(imgur.state, Local::Failed);
    }

    void overAutoLimitClearsWhenRaised()
    {
        linkstate::LocalState l;
        l.state  = Local::OverAutoLimit;
        l.size   = 30 * 1024 * 1024;
        l.capHit = 15 * 1024 * 1024;
        QVERIFY(linkstate::stillOverLimit(l, 15 * 1024 * 1024));
        QVERIFY(!linkstate::stillOverLimit(l, 50 * 1024 * 1024)); // raised above its size: a normal candidate again
        QVERIFY(linkstate::stillOverLimit(l, 0));                 // data saver: still over
        // Size unknown: only a cap above the one it hit.
        l.size = -1;
        QVERIFY(linkstate::stillOverLimit(l, 15 * 1024 * 1024));
        QVERIFY(!linkstate::stillOverLimit(l, 20 * 1024 * 1024));
        // Not OverAutoLimit: never.
        l.state = Local::Idle;
        QVERIFY(!linkstate::stillOverLimit(l, 1));
    }

    void holdReasons()
    {
        using linkstate::HoldReason;
        using Verdict = linkpreview::HostGate::Verdict;
        linkstate::Decision d;
        d.look = d.underlying = Look::Absent;
        linkstate::LocalState l;
        linkstate::Context    ctx;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::None);
        QCOMPARE(linkstate::holdReason(d, l, ctx, false, true, true, Verdict::Go), HoldReason::Offscreen);
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, false, true, Verdict::Go), HoldReason::Turn);
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, false, Verdict::Go), HoldReason::Budget);
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Cooling), HoldReason::Gate);
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Walled), HoldReason::Gate);
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go, false), HoldReason::Cap);
        ctx.autoOn = false;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Setting);
        ctx.autoOn        = true;
        ctx.senderNothing = true;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::SenderNothing);
        ctx.senderNothing = false;
        l.state           = Local::Nothing;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Nothing);
        l.state = Local::Failed;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Failed);
        l.state = Local::Cooling;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Gate);
        l.state = Local::Idle;
        d.wait  = linkstate::Wait::Sender;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Waiting);
        d.look = Look::Hidden;
        QCOMPARE(linkstate::holdReason(d, l, ctx, true, true, true, Verdict::Go), HoldReason::Hidden);
        QCOMPARE(QString::fromLatin1(linkstate::holdReasonName(HoldReason::SenderNothing)), QStringLiteral("sender-nothing"));
    }

    void errorTextsNeutral()
    {
        QCOMPARE(linkstate::errorText(QString::fromLatin1(linkpreview::errors::kRateLimited)), QStringLiteral("The site is busy right now. Try again later."));
        QCOMPARE(linkstate::errorText(QString::fromLatin1(linkpreview::errors::kGateBusy)), QStringLiteral("The site is busy right now. Try again later."));
        QCOMPARE(linkstate::errorText(QString::fromLatin1(linkpreview::errors::kUnsupported)), QStringLiteral("This picture can't be shown here"));
        for (const char* nothing : {linkpreview::errors::kNothing, linkpreview::errors::kWalled, linkpreview::errors::kExpired})
            QVERIFY(linkstate::isNothingError(QString::fromLatin1(nothing)));
        QVERIFY(!linkstate::isNothingError(QString::fromLatin1(linkpreview::errors::kNotFound)));
        QVERIFY(linkstate::errorText(QString::fromLatin1(linkpreview::errors::kTls)).isEmpty());
    }

    void webLinkLoopAndSensitive()
    {
        const MediaLink loop = linkstate::webLink(mediaTarget(), QStringLiteral("video/mp4"), 1234, 480, 270, 4000, true, true);
        QVERIFY(loop.loop);
        QVERIFY(loop.spoiler);
        QVERIFY(loop.fileName.endsWith(QLatin1String(".mp4")));
        // A loop never goes into a link: the round trip drops it.
        MediaLink parsed = MediaLink::parse(loop.toUrl());
        QVERIFY(!parsed.loop);
        // Opening a page link: the https form.
        QCOMPARE(linkstate::webAddress(QStringLiteral("http://www.example.com/news/a-story"), linkpreview::classify(QStringLiteral("http://www.example.com/news/a-story")))
                     .scheme(),
                 QStringLiteral("https"));
    }

    // ---- autoload ---------------------------------------------------------------------------------------------
    void waitSecPerTarget()
    {
        using namespace linkpreview::autoload;
        QCOMPARE(waitSec(target("https://www.youtube.com/watch?v=dQw4w9WgXcQ")), 30);
        QCOMPARE(waitSec(directTarget()), 45);
        QCOMPARE(waitSec(target("https://example.com/clip.mp4")), 120);
        QCOMPARE(waitSec(mediaTarget()), 60);
        QCOMPARE(waitSec(target("https://streamable.com/1hzf2p")), 120); // a media page's video
        // A Nextcloud share may be a video (expected kind Image): its sender's preview gets a video's wait.
        const linkpreview::Target share = target("https://cloud.example.org/s/Ab3dE6gH9jK2mN5");
        QCOMPARE(share.provider, QStringLiteral("nextcloud"));
        QCOMPARE(waitSec(share), 120);
        QCOMPARE(waitSec(siteTarget()), 40);
        QCOMPARE(waitSec(webTarget()), 40);
        QCOMPARE(maxWaitSec(webTarget()), 160);
        QCOMPARE(maxWaitSec(mediaTarget()), 180);
        QCOMPARE(QString::fromLatin1(capFor(LinkClass::Direct)), QStringLiteral("l"));
        QCOMPARE(QString::fromLatin1(capFor(LinkClass::YouTube)), QStringLiteral("l"));
        QCOMPARE(QString::fromLatin1(capFor(LinkClass::MediaPage)), QStringLiteral("lx"));
        QCOMPARE(QString::fromLatin1(capFor(LinkClass::Site)), QStringLiteral("lx"));
        QCOMPARE(QString::fromLatin1(capFor(LinkClass::Web)), QStringLiteral("lw"));
    }

    void autoCapsPerTarget()
    {
        using namespace linkpreview::autoload;
        Settings s;
        s.autoDownloadImages  = true;
        s.autoDownloadMaxMB   = 15;
        s.videoAutoDownloadMB = 0;
        const qint64 mb = 1024 * 1024;
        // Cards: picture 2 MB, page 512 KB; their media follows the picture rule.
        linkpreview::Caps web = autoCaps(webTarget(), s, QString());
        QCOMPARE(web.picture, 2 * mb);
        QCOMPARE(web.page, qint64(512 * 1024));
        QCOMPARE(web.media, 15 * mb);
        QVERIFY(!web.userAsked);
        QCOMPARE(autoCap(webTarget(), s, QString()), 2 * mb); // a card's cap: its picture
        // A loop like a GIF, a media page like a picture.
        QCOMPARE(autoCaps(mediaTarget(), s, QString()).media, 15 * mb);
        QCOMPARE(autoCap(mediaTarget(), s, QString()), 15 * mb);
        // YouTube always 2 MB.
        QCOMPARE(autoCap(target("https://youtu.be/dQw4w9WgXcQ"), s, QString()), 2 * mb);
        // Data saver: cards smaller, no media at all.
        s.dataSaver = true;
        web         = autoCaps(webTarget(), s, QString());
        QCOMPARE(web.picture, qint64(512 * 1024));
        QCOMPARE(web.page, qint64(128 * 1024));
        QCOMPARE(web.media, qint64(0));
        QCOMPARE(autoCap(webTarget(), s, QString()), qint64(512 * 1024)); // cards still load
        QCOMPARE(autoCap(mediaTarget(), s, QString()), qint64(0));        // media pages wait for a click
        QCOMPARE(autoCap(siteTarget(), s, QString()), qint64(512 * 1024));
        QCOMPARE(autoCap(target("https://youtu.be/dQw4w9WgXcQ"), s, QString()), 2 * mb);
        s.dataSaver = false;
        // Automatic picture downloads off: no media; cards still.
        s.autoDownloadImages = false;
        QCOMPARE(autoCap(mediaTarget(), s, QString()), qint64(0));
        QCOMPARE(autoCaps(webTarget(), s, QString()).media, qint64(0));
        QCOMPARE(autoCap(webTarget(), s, QString()), 2 * mb);
        s.autoDownloadImages = true;
        // Web cards off: nothing at all for a web page; a site card still.
        s.showWebCards = false;
        QCOMPARE(autoCap(webTarget(), s, QString()), qint64(0));
        QCOMPARE(autoCaps(webTarget(), s, QString()).page, qint64(0));
        QCOMPARE(autoCap(siteTarget(), s, QString()), 2 * mb);
    }

    void overLimitCapOfPageMedia()
    {
        // Review fix (8.3): a web page that answered with media over the automatic limit is judged against the media cap
        // that refused it, never the card's picture cap.
        using namespace linkpreview::autoload;
        Settings s;
        s.autoDownloadImages  = true;
        s.autoDownloadMaxMB   = 15;
        const qint64 mb       = 1024 * 1024;
        QCOMPARE(overLimitCap(webTarget(), Kind::Image, s, QString()), 15 * mb);
        QCOMPARE(overLimitCap(webTarget(), Kind::Video, s, QString()), 15 * mb);
        QCOMPARE(overLimitCap(webTarget(), Kind::None, s, QString()), 2 * mb); // a card's own picture
        QCOMPARE(overLimitCap(mediaTarget(), Kind::Loop, s, QString()), 15 * mb);
        // A "Picture · 3 MB" entry of a web page with data saver: still over (the card cap of 512 KB isn't it; 0 is).
        linkstate::LocalState l;
        l.state  = Local::OverAutoLimit;
        l.kind   = Kind::Image;
        l.size   = 3 * mb;
        l.capHit = 0;
        s.dataSaver = true;
        QVERIFY(linkstate::stillOverLimit(l, overLimitCap(webTarget(), l.kind, s, QString())));
        // Automatic picture downloads switched on after it was refused with them off: it loads by itself now (the
        // card's picture cap didn't change, the media cap did).
        s.dataSaver          = false;
        s.autoDownloadImages = false;
        QCOMPARE(overLimitCap(webTarget(), Kind::Image, s, QString()), qint64(0));
        s.autoDownloadImages = true;
        QVERIFY(!linkstate::stillOverLimit(l, overLimitCap(webTarget(), l.kind, s, QString())));
        QVERIFY(isMediaKind(Kind::Loop) && isMediaKind(Kind::Gif) && !isMediaKind(Kind::Card) && !isMediaKind(Kind::YouTube));
    }

    void askPartnerAgainStillWorks()
    {
        // Review fix: 2.4.0's "ask the private partner again" (caps said no, heard over a minute ago) still runs for
        // every class: a link of a sender whose caps say no has no wait in the queue model (budget 0), which must not
        // end the question.
        using namespace linkpreview::autoload;
        Input in;
        in.autoOn     = true;
        in.chat       = Chat::Private;
        in.nowSec     = 100000;
        in.timeSec    = in.nowSec - 10;
        in.budgetSec  = 0; // SenderQueue::add for a sender whose caps say no
        in.cap        = SenderCap::No;
        in.senderHere = true;
        QVERIFY(worthAsking(in, webTarget()));
        QVERIFY(worthAsking(in, siteTarget()));
        QVERIFY(worthAsking(in, directTarget()));
        // An old message: not worth a question.
        in.timeSec = in.nowSec - 600;
        QVERIFY(!worthAsking(in, webTarget()));
        // The sender left, or said it won't make this one.
        in.timeSec    = in.nowSec - 10;
        in.senderHere = false;
        QVERIFY(!worthAsking(in, webTarget()));
        in.senderHere = true;
        in.skipped    = true;
        QVERIFY(!worthAsking(in, webTarget()));
    }

    void walledSiteIsNothing()
    {
        // Review fix: every link of a site behind a bot check is Nothing (no slot), like the load that met the wall;
        // never the retryable error a click can't fix.
        using Verdict = linkpreview::HostGate::Verdict;
        const Target klipy = target("https://klipy.com/gifs/andy-samberg-awkward");
        QCOMPARE(klipy.cls, LinkClass::MediaPage);
        const linkstate::LocalState walled = linkstate::underGate(linkstate::LocalState(), Verdict::Walled, 1000, 5000);
        QCOMPARE(walled.state, Local::Nothing);
        QCOMPARE(linkstate::decide(klipy, false, walled, linkstate::ServerState(), linkstate::Wait::None, linkstate::Context()).look, Look::Absent);
        linkstate::LocalState clicked = walled;
        clicked.clicked               = true;
        const linkstate::Decision d   = linkstate::decide(klipy, false, clicked, linkstate::ServerState(), linkstate::Wait::None, linkstate::Context());
        QCOMPARE(d.look, Look::Error);
        QVERIFY(d.nothing);
        const linkstate::LocalState cooling = linkstate::underGate(linkstate::LocalState(), Verdict::Cooling, 30000, 5000);
        QCOMPARE(cooling.state, Local::Cooling);
        QCOMPARE(cooling.retryAtMs, qint64(35000));
        linkstate::LocalState loaded;
        loaded.state = Local::Loaded;
        QCOMPARE(linkstate::underGate(loaded, Verdict::Walled, 0, 0).state, Local::Loaded);
        // Live test fix: Load link previews on a walled link answers at once with "No preview for this page" (Open in
        // browser, no Retry), not the plain error; a cooling site still gets the busy line.
        const linkstate::LocalState clickWall = linkstate::clickedUnderGate(Verdict::Walled);
        QVERIFY(clickWall.clicked);
        const linkstate::Decision dw = linkstate::decide(klipy, false, clickWall, linkstate::ServerState(), linkstate::Wait::None, linkstate::Context());
        QCOMPARE(dw.look, Look::Error);
        QVERIFY(dw.nothing);
        linkcard::Card strip;
        strip.look    = dw.look;
        strip.nothing = dw.nothing;
        QCOMPARE(linkcard::statusText(strip), QStringLiteral("No preview for this page"));
        const linkstate::LocalState clickBusy = linkstate::clickedUnderGate(Verdict::Cooling);
        QCOMPARE(clickBusy.state, Local::Failed);
        QVERIFY(clickBusy.clicked);
        QCOMPARE(clickBusy.error, linkstate::errorText(QStringLiteral("rateLimited")));
        QVERIFY(!linkstate::decide(klipy, false, clickBusy, linkstate::ServerState(), linkstate::Wait::None, linkstate::Context()).nothing);
    }

    void storedPreviewIsLoadingNotWaiting()
    {
        // Live test fix: an LPX whose media is still coming from the TeamSpeak server (paced, one a second) read
        // "Waiting for the sender's preview…" in history, also on the sender's own messages. It is loading.
        linkstate::Decision d;
        d.look = Look::Loading;
        d.wait = linkstate::Wait::Sender;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::Sender);
        linkcard::Card card;
        card.look    = Look::Loading;
        card.waiting = linkstate::waitingShown(d);
        QCOMPARE(linkcard::statusText(card), QString::fromUtf8("Waiting for the sender's preview\xe2\x80\xa6"));
        d.fromServer = true;
        d.media      = true;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::None);
        card.waiting  = linkstate::waitingShown(d);
        card.progress = -1.0;
        QCOMPARE(linkcard::statusText(card), QString::fromUtf8("Loading preview\xe2\x80\xa6"));
        // The other waits read as before.
        d.fromServer = false;
        d.wait       = linkstate::Wait::Own;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::Own);
        d.wait = linkstate::Wait::Checking;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::Checking);
        d.wait = linkstate::Wait::Host;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::Host);
        d.wait = linkstate::Wait::Auto;
        QCOMPARE(linkstate::waitingShown(d), linkcard::Waiting::None);
    }

    void clickCapsPerTarget()
    {
        using namespace linkpreview::autoload;
        const qint64 mb = 1024 * 1024;
        QCOMPARE(clickCaps(target("https://youtu.be/dQw4w9WgXcQ")).media, 2 * mb);
        QCOMPARE(clickCaps(directTarget()).media, 25 * mb);
        QCOMPARE(clickCaps(mediaTarget()).media, 25 * mb);
        QCOMPARE(clickCaps(target("https://example.com/clip.mp4")).media, 50 * mb);
        const linkpreview::Caps web = clickCaps(webTarget());
        QVERIFY(web.userAsked);
        QCOMPARE(web.picture, 2 * mb);
        QCOMPARE(web.page, qint64(512 * 1024));
    }

    void senderBudgetTakesN()
    {
        using linkpreview::autoload::SenderBudget;
        SenderBudget budget;
        const qint64 now = 1000;
        QVERIFY(budget.has(QStringLiteral("a"), now, 3));
        QVERIFY(budget.take(QStringLiteral("a"), now, 3)); // a card with its picture: 2-3 requests
        QVERIFY(budget.take(QStringLiteral("a"), now, 3));
        QVERIFY(!budget.has(QStringLiteral("a"), now, 1)); // burst 6 used up
        QVERIFY(!budget.take(QStringLiteral("a"), now, 1));
        QVERIFY(budget.has(QStringLiteral("a"), now + 20000, 2)); // one per 10 s
        QVERIFY(!budget.has(QStringLiteral("a"), now + 20000, 3));
        QVERIFY(!budget.take(QStringLiteral("b"), now, 7)); // more than the burst: never
    }

    void senderQueueIntakeModel()
    {
        using linkpreview::autoload::SenderQueue;
        const Target web = webTarget();
        {
            SenderQueue q;
            // Six links within 57 s: each waits; the seventh was refused at the sender's intake: 0.
            for (int i = 0; i < 6; ++i)
                QVERIFY(q.add(QStringLiteral("s"), 1000 + i * 9, web, true) > 0);
            QCOMPARE(q.add(QStringLiteral("s"), 1000 + 56, web, true), 0);
            // Another sender has their own.
            QVERIFY(q.add(QStringLiteral("t"), 1056, web, true) > 0);
        }
        {
            SenderQueue q;
            // Six within 58 s: the seventh at 58 s is outside the window: it waits.
            for (int i = 0; i < 6; ++i)
                q.add(QStringLiteral("s"), 1000 + (i == 0 ? 0 : 50 + i), web, true);
            QVERIFY(q.add(QStringLiteral("s"), 1058, web, true) > 0);
        }
        {
            SenderQueue q;
            // Links the sender doesn't make (their cap doesn't cover the class) don't count and don't wait.
            for (int i = 0; i < 10; ++i)
                QCOMPARE(q.add(QStringLiteral("s"), 1000 + i, web, false), 0);
            QVERIFY(q.add(QStringLiteral("s"), 1010, web, true) > 0);
        }
        {
            // Review fix: a 2.5 sender's plugin has an allowance for the 2.4 classes and one for the new ones: six page
            // links in a minute never make a YouTube or file link look refused (and the other way round).
            SenderQueue q;
            for (int i = 0; i < 6; ++i)
                QVERIFY(q.add(QStringLiteral("s"), 1000 + i, web, true) > 0);
            QCOMPARE(q.add(QStringLiteral("s"), 1007, web, true), 0);
            QVERIFY(q.add(QStringLiteral("s"), 1008, directTarget(), true) > 0);
            for (int i = 0; i < 5; ++i)
                QVERIFY(q.add(QStringLiteral("s"), 1009 + i, target("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), true) > 0);
            QCOMPARE(q.add(QStringLiteral("s"), 1020, directTarget(), true), 0);
        }
        {
            SenderQueue q;
            // Refused links are not recorded: after the window the sender's next link waits as usual.
            for (int i = 0; i < 6; ++i)
                q.add(QStringLiteral("s"), 1000, web, true);
            QCOMPARE(q.add(QStringLiteral("s"), 1001, web, true), 0);
            QCOMPARE(q.add(QStringLiteral("s"), 1002, web, true), 0);
            QVERIFY(q.add(QStringLiteral("s"), 1000 + 57, web, true) > 0);
        }
    }

    void steadyPosterNeverRefused()
    {
        using linkpreview::autoload::SenderQueue;
        const Target web = webTarget();
        // One link every 10 s for 10 minutes: never modelled as refused.
        {
            SenderQueue q;
            for (int i = 0; i < 60; ++i)
                QVERIFY2(q.add(QStringLiteral("s"), 1000 + i * 10, web, true) > 0, qPrintable(QString::number(i)));
        }
        // One every 9.5 s with receive times rounded to whole seconds: never either.
        {
            SenderQueue q;
            for (int i = 0; i < 64; ++i) {
                const qint64 t = 1000 + static_cast<qint64>(std::floor(i * 9.5));
                QVERIFY2(q.add(QStringLiteral("s"), t, web, true) > 0, qPrintable(QString::number(i)));
            }
        }
    }

    // ---- linkcard: the Embed card -----------------------------------------------------------------------------
    void embedLayout_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<int>("width");
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<bool>("bubble");
        for (const CardLayout lay : {CardLayout::Large, CardLayout::Thumb, CardLayout::None}) {
            for (const int width : {160, 240, 279, 280, 300, 400}) {
                for (const qreal dpr : {1.0, 2.0}) {
                    for (const bool bubble : {false, true})
                        QTest::newRow(qPrintable(QStringLiteral("%1 %2 x%3 %4").arg(linkpreview::layoutName(lay)).arg(width).arg(dpr).arg(bubble ? "bubble" : "chat")))
                            << static_cast<int>(lay) << width << dpr << bubble;
                }
            }
        }
    }
    void embedLayout()
    {
        QFETCH(int, layout);
        QFETCH(int, width);
        QFETCH(qreal, dpr);
        QFETCH(bool, bubble);
        const PreviewStyle      style = styleOf(false, width, dpr, bubble);
        linkcard::Card          card  = embed(static_cast<CardLayout>(layout));
        const linkcard::Layout  l     = linkcard::layout(card, style);
        QVERIFY(l.size.width() <= qMax(160, width));
        QVERIFY(l.size.width() <= linkcard::kEmbedWidth);
        QVERIFY(l.size.height() > 0);
        // Everything inside the card, the host's footer last.
        const QRectF bounds(QPointF(0, 0), QSizeF(l.size));
        for (const QRectF& r : {l.bar, l.site, l.title, l.description, l.thumb, l.footer, l.hide}) {
            if (!r.isEmpty())
                QVERIFY2(bounds.adjusted(-0.5, -2.5, 0.5, 0.5).contains(r), qPrintable(QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())));
        }
        QVERIFY(l.footer.top() >= l.description.bottom());
        if (!l.thumb.isEmpty()) {
            QVERIFY(l.thumb.height() <= linkcard::kEmbedMaxPicture);
            if (l.thumbSide) {
                QCOMPARE(l.thumb.width(), qreal(linkcard::kEmbedThumb));
                QVERIFY(l.title.right() <= l.thumb.left());
            } else {
                QVERIFY(l.thumb.top() >= l.description.bottom());
            }
        }
        if (static_cast<CardLayout>(layout) == CardLayout::None)
            QVERIFY(l.thumb.isEmpty());
        // The size never changes with the pointer, the picture arriving, or a ring.
        linkcard::Card loaded = card;
        QImage         pic(1200, 630, QImage::Format_RGB32);
        pic.fill(Qt::darkCyan);
        loaded.picture        = pic;
        linkcard::Card waiting = card;
        waiting.pictureLoading = true;
        QCOMPARE(linkcard::layout(loaded, style).size, l.size);
        QCOMPARE(linkcard::layout(waiting, style).size, l.size);
        linkcard::Pointer over;
        over.over  = true;
        over.hover = linkcard::Zone::Open;
        linkcard::Layout drawn;
        const QImage     image = linkcard::render(loaded, style, over, &drawn);
        QCOMPARE(drawn.size, l.size);
        QCOMPARE(image.size(), QSize(qRound(l.size.width() * dpr), qRound(l.size.height() * dpr)));
        // Narrow: at most two lines of text.
        if (l.size.width() < linkcard::kEmbedNarrow)
            QVERIFY(l.descriptionLines.size() <= 2);
        else
            QVERIFY(l.descriptionLines.size() <= 3);
        QVERIFY(l.titleLines.size() <= 2);
    }

    void embedSecondCardThumb()
    {
        const PreviewStyle style = styleOf(false, 400);
        linkcard::Card     first = embed(CardLayout::Large);
        linkcard::Card     second = first;
        second.slotIndex          = 1;
        const linkcard::Layout a = linkcard::layout(first, style);
        const linkcard::Layout b = linkcard::layout(second, style);
        QCOMPARE(a.pictureLayout, CardLayout::Large);
        QCOMPARE(b.pictureLayout, CardLayout::Thumb);
        QVERIFY(b.thumbSide);
        QVERIFY(b.size.height() < a.size.height());
        linkcard::Card third = first;
        third.slotIndex      = 2;
        QCOMPARE(linkcard::layout(third, style).size, b.size);
    }

    void embedZones()
    {
        const PreviewStyle style = styleOf(false, 400);
        linkcard::Card     card  = embed(CardLayout::Large);
        linkcard::Layout   l     = linkcard::layout(card, style);
        QCOMPARE(linkcard::primaryZone(card), linkcard::Zone::Open);
        QVERIFY(linkcard::isActionable(card, linkcard::Zone::Body));
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Open);
        QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), true), linkcard::Zone::Hide);
        QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), false), linkcard::Zone::Open);
        // Covered while sensitive: the picture reveals it; elsewhere it opens.
        card.data.sensitive = true;
        l                   = linkcard::layout(card, style);
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Reveal);
        QCOMPARE(linkcard::zoneAt(card, l, l.title.center(), true), linkcard::Zone::Open);
        card.revealed = true;
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Open);
        // Absent: nothing at all.
        linkcard::Card absent;
        absent.look = Look::Absent;
        QVERIFY(linkcard::layout(absent, style).size.isEmpty());
        QVERIFY(linkcard::render(absent, style, linkcard::Pointer()).isNull());
    }

    void embedTexts()
    {
        linkcard::Card card = embed(CardLayout::Large);
        card.data.title     = QStringLiteral("<img src=\"//127.0.0.1/x.png\"> A title");
        card.fromSender     = true;
        const QString tip   = linkcard::toolTip(card, linkcard::Zone::Open);
        QVERIFY(tip.startsWith(card.data.title));
        QVERIFY(tip.contains(QStringLiteral("Opens www.example.com in your browser")));
        QVERIFY(tip.contains(QStringLiteral("Preview made by the sender's TS Media")));
        const QString html = linkcard::toolTipHtml(card, linkcard::Zone::Open);
        QVERIFY(!html.contains(QLatin1String("<img")));
        QVERIFY(html.contains(QLatin1String("&lt;img")));
        // A video page: "Plays on ..."; a redirect: the link's own host (what a click opens), then where it went.
        card.data.play    = true;
        card.host         = QStringLiteral("bit.example.com");
        card.redirectedTo = QStringLiteral("other.example.net");
        const QString moved = linkcard::toolTip(card, linkcard::Zone::Body);
        QVERIFY(moved.contains(QStringLiteral("Plays on bit.example.com in your browser")));
        QVERIFY(moved.contains(QStringLiteral("This preview comes from other.example.net")));
        card.data.sensitive = true;
        QVERIFY(linkcard::toolTip(card, linkcard::Zone::Body).contains(QStringLiteral("Marked as sensitive")));
        // The text is cut to 300 characters there.
        card.data.title = QString(1000, QLatin1Char('t'));
        QVERIFY(linkcard::toolTip(card, linkcard::Zone::Body).split(QLatin1Char('\n')).first().size() <= 300);
        QCOMPARE(linkcard::statusText(card), card.data.title);
    }

    void embedRtlParagraphs()
    {
        // Review fix: a Persian title is one right-to-left paragraph; its second line that starts with a Latin brand
        // name stays right-aligned (drawn in the paragraph's direction, not re-guessed per line).
        const PreviewStyle style = styleOf(false, 240);
        linkcard::Card     c     = embed(CardLayout::None);
        c.data.site.clear();
        c.data.description.clear();
        linkcard::Layout l;
        bool             found = false;
        for (int n = 1; n < 40 && !found; ++n) {
            QStringList words;
            for (int i = 0; i < n; ++i)
                words << QStringLiteral("\u06a9\u0644\u0645\u0647");
            c.data.title = words.join(QLatin1Char(' ')) + QStringLiteral(" Samsung S24");
            l            = linkcard::layout(c, style);
            found        = l.titleLines.size() == 2 && l.titleLines.at(1).startsWith(QStringLiteral("Samsung"));
        }
        QVERIFY(found);
        // One paragraph, one direction: the line that starts with "Samsung" is drawn right to left (right-aligned)
        // with the rest of the title, never guessed again from its own first letter.
        QVERIFY(l.titleRtl);
        QVERIFY(!QString(l.titleLines.at(1)).isRightToLeft());
        QCOMPARE(l.titleRects.size(), 2);
        // Its first line is drawn as narrow as it was wrapped: right-aligned, it ends before the hide button.
        QVERIFY2(l.titleRects.at(0).right() <= l.hide.left(), qPrintable(QString::number(l.titleRects.at(0).right())));
        QCOMPARE(l.titleRects.at(1).right(), l.title.right());
        QCOMPARE(linkcard::zoneAt(c, l, QPointF(l.titleRects.at(0).right() - 2, l.titleRects.at(0).center().y()), true), linkcard::Zone::Open);
        // The same for a description that is the card's first text, at 400 px.
        c.data.title.clear();
        c.data.description = QStringLiteral("\u0641\u0631\u0648\u0634\u06af\u0627\u0647 \u0627\u06cc\u0646\u062a\u0631\u0646\u062a\u06cc \u0628\u0627 \u0628\u0647\u062a\u0631\u06cc\u0646 \u0642\u06cc\u0645\u062a ") + QStringLiteral("Samsung Galaxy S24 Ultra");
        l                  = linkcard::layout(c, styleOf(false, 400));
        QVERIFY(l.descriptionRtl);
        QVERIFY(!l.descriptionRects.isEmpty());
        QVERIFY(l.descriptionRects.at(0).right() <= l.hide.left());
        // An English title keeps its own direction; a site line its own.
        c = embed(CardLayout::None);
        l = linkcard::layout(c, style);
        QVERIFY(!l.titleRtl);
        QVERIFY(!l.siteRtl);
        QCOMPARE(l.titleRects.size(), l.titleLines.size());
    }

    void embedTipWraps()
    {
        // Review fix: an Embed card's tool tip wraps its page text (no one-line tip wider than the screen) and keeps
        // its host lines whole; its text is cut between whole characters with "…".
        static const uint kFamily[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467}; // one emoji (a ZWJ sequence)
        const QString     family    = QString::fromUcs4(kFamily, 5);
        linkcard::Card    c         = embed(CardLayout::Large);
        c.data.description          = QString(296, QLatin1Char('d')) + family + QString(50, QLatin1Char('e'));
        const QString tip  = linkcard::toolTip(c, linkcard::Zone::Open);
        const QStringList lines = tip.split(QLatin1Char('\n'));
        QVERIFY(lines.at(1).size() <= 300);
        QVERIFY(lines.at(1).endsWith(QChar(0x2026)));
        for (int i = 0; i < lines.at(1).size(); ++i) {
            const QChar ch = lines.at(1).at(i);
            if (ch.isHighSurrogate())
                QVERIFY(i + 1 < lines.at(1).size() && lines.at(1).at(i + 1).isLowSurrogate());
            if (ch.isLowSurrogate())
                QVERIFY(i > 0 && lines.at(1).at(i - 1).isHighSurrogate());
        }
        QVERIFY(!lines.at(1).contains(QChar(0x200d)) || lines.at(1).contains(family)); // whole or not at all
        const QString html = linkcard::toolTipHtml(c, linkcard::Zone::Open);
        QVERIFY(!html.contains(QLatin1String("white-space:pre")));
        QVERIFY(html.contains(QLatin1String("<nobr>Opens www.example.com in your browser</nobr>")));
        // Strips keep their one-line tips.
        linkcard::Card strip;
        strip.look = Look::Click;
        strip.host = QStringLiteral("www.example.com");
        QVERIFY(linkcard::toolTipHtml(strip, linkcard::Zone::Load).contains(QLatin1String("white-space:pre")));
    }

    void loopOnlyWhenHonest()
    {
        // Review fix: an LPX k=a plays like a GIF (picture rule, autoplay) only when it says what an honest sender's
        // loop is: 1 ms..60 s long, its file within the GIF cap (8 MB). Anything else is a video: the video rule.
        linkpreview::Announcement a;
        a.extended    = true;
        a.kind        = Kind::Loop;
        a.urlKey      = QStringLiteral("8eb4311921f7b1964785a11a4cc1f418");
        a.serverUid   = QStringLiteral("server");
        a.channelId   = 7;
        a.previewPath = QStringLiteral("/tsmedia/previews/3f9a1c2e.jpg");
        a.previewSha  = QString(64, QLatin1Char('a'));
        a.previewSize = 1234;
        a.mediaPath   = QStringLiteral("/tsmedia/link_3f9a1c2e.mp4");
        a.mediaSha    = QString(64, QLatin1Char('b'));
        a.mediaSize   = 4 * 1024 * 1024;
        a.durationMs  = 5000;
        QVERIFY(linkpreview::loopTrusted(a));
        QVERIFY(linkpreview::linkFor(a).loop);
        a.mediaSize = 100LL * 1024 * 1024; // any video a client calls a loop
        QVERIFY(!linkpreview::loopTrusted(a));
        QVERIFY(!linkpreview::linkFor(a).loop);
        a.mediaSize  = 4 * 1024 * 1024;
        a.durationMs = 0; // no length said
        QVERIFY(!linkpreview::linkFor(a).loop);
        a.durationMs = 10 * 60 * 1000; // ten minutes
        QVERIFY(!linkpreview::linkFor(a).loop);
        a.durationMs = 60000;
        QVERIFY(linkpreview::linkFor(a).loop);
        a.kind = Kind::Video;
        QVERIFY(!linkpreview::linkFor(a).loop);
    }

    void cardPictureGoneForgotten()
    {
        // Review fix: a card loaded here whose picture left cache/links (pruned after other loads) is forgotten, so it
        // loads again by the usual rules instead of keeping an empty picture box.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.path() + QStringLiteral("/0123-thumb.png");
        QVERIFY(QImage(8, 8, QImage::Format_RGB32).save(file, "PNG"));
        linkstate::LocalState l;
        l.state            = Local::Loaded;
        l.kind             = Kind::Card;
        l.card.title       = QStringLiteral("T");
        l.card.pictureFile = file;
        QVERIFY(!linkstate::localFilesGone(l));
        QVERIFY(QFile::remove(file));
        QVERIFY(linkstate::localFilesGone(l));
        l.card.pictureFile.clear(); // a text card has nothing to lose
        QVERIFY(!linkstate::localFilesGone(l));
        l.thumbnailFile = file; // YouTube: as 2.4.0
        QVERIFY(linkstate::localFilesGone(l));
        l.state = Local::Failed;
        QVERIFY(!linkstate::localFilesGone(l));
    }

    void senderCardPictureFailedForGood()
    {
        // Review fix: a sender's card picture that can never download here (gone, a permission, a channel password)
        // draws a text card, never an empty box; a card that is only that picture is no sender's preview at all.
        linkstate::ServerState s;
        s.announced        = true;
        s.extended         = true;
        s.kind             = Kind::Card;
        s.mainKey          = QStringLiteral("k");
        s.card.title       = QStringLiteral("T");
        s.card.description = QStringLiteral("D");
        s.card.layout      = CardLayout::Large;
        s.card.play        = true;
        s.state            = MediaState::Failed;
        s.error            = MediaError::Permission;
        QVERIFY(linkstate::serverUsable(s));
        QCOMPARE(linkstate::serverCardData(s).layout, CardLayout::None);
        QVERIFY(!linkstate::serverCardData(s).play);
        s.card.description.clear();
        QVERIFY(!linkstate::serverUsable(s));
        // A failure that may pass (connection, quota) keeps the box (Core tries again).
        s.error = MediaError::NotConnected;
        QVERIFY(linkstate::serverUsable(s));
        QCOMPARE(linkstate::serverCardData(s).layout, CardLayout::Large);
        // Still coming, or no picture announced.
        s.state = MediaState::Downloading;
        QCOMPARE(linkstate::serverCardData(s).layout, CardLayout::Large);
        s.mainKey.clear();
        QCOMPARE(linkstate::serverCardData(s).layout, CardLayout::None);
        // The sensitive flag of the LPX covers it.
        s.sensitive = true;
        QVERIFY(linkstate::serverCardData(s).sensitive);
    }

    void loopToolTipLikeGif()
    {
        // Review fix: a GIF-like loop's tip is a GIF's: its state while it isn't there, nothing once it is; never a
        // video's autoplay text.
        MediaEntry e;
        e.kind  = MediaKind::Video;
        e.state = MediaState::Idle;
        QCOMPARE(loopToolTipDetail(e), QStringLiteral("Click to download"));
        e.state    = MediaState::Downloading;
        e.progress = 0.45;
        QVERIFY(loopToolTipDetail(e).startsWith(QStringLiteral("Downloading")));
        QVERIFY(!loopToolTipDetail(e).contains(QStringLiteral("autoplay")));
        e.state = MediaState::Ready;
        QVERIFY(loopToolTipDetail(e).isEmpty());
    }

    void signatureCoversNewFields()
    {
        // Review fix: the chat draws a card again when anything it is drawn from changes (it compares signatures).
        const PreviewStyle      style = styleOf(false, 400);
        const linkcard::Pointer none;
        // A still "Site busy" strip: a new picture when its minutes change, not every second.
        linkcard::Card busy;
        busy.look      = Look::Loading;
        busy.waiting   = linkcard::Waiting::Host;
        busy.host      = QStringLiteral("i.imgur.com");
        busy.retryInMs = 600000;
        const QString tenMinutes = linkcard::signature(busy, style, none);
        busy.retryInMs           = 599000;
        QCOMPARE(linkcard::signature(busy, style, none), tenMinutes); // still "10 min"
        busy.retryInMs = 540000;
        QVERIFY(linkcard::signature(busy, style, none) != tenMinutes); // "9 min"
        busy.retryInMs           = 43000;
        const QString fortyThree = linkcard::signature(busy, style, none);
        busy.retryInMs           = 42000;
        QVERIFY(linkcard::signature(busy, style, none) != fortyThree); // seconds (also with animations off)
        // An Embed card: its picture arriving, its cover revealed, its text, a redirect.
        linkcard::Card c       = embed(CardLayout::Large);
        c.pictureLoading       = true;
        const QString waiting  = linkcard::signature(c, style, none);
        c.pictureLoading       = false;
        c.picture              = QImage(40, 21, QImage::Format_RGB32);
        const QString arrived  = linkcard::signature(c, style, none);
        QVERIFY(arrived != waiting);
        c.data.sensitive       = true;
        const QString covered  = linkcard::signature(c, style, none);
        c.revealed             = true;
        QVERIFY(linkcard::signature(c, style, none) != covered);
        const QString shown    = linkcard::signature(c, style, none);
        c.data.description     = QStringLiteral("Another text");
        QVERIFY(linkcard::signature(c, style, none) != shown);
        const QString text     = linkcard::signature(c, style, none);
        c.redirectedTo         = QStringLiteral("other.example.net");
        QVERIFY(linkcard::signature(c, style, none) != text);
        linkcard::Card nothing;
        nothing.look           = Look::Error;
        const QString error    = linkcard::signature(nothing, style, none);
        nothing.nothing        = true;
        QVERIFY(linkcard::signature(nothing, style, none) != error);
    }

    void crossSiteCardShowsLinkHost()
    {
        // Review fix (D18): a page that redirected the plugin to another website (a cloaking redirect to a well-known
        // site) never hides the link's own host. The footer names the link's host first and whole; the other website
        // only after it; never that website's site name or colour; and only a load the user started shows it.
        const QFont         foot = [] {
            QFont f(QStringLiteral("Segoe UI"));
            f.setPixelSize(11);
            return f;
        }();
        const QFontMetricsF fm(foot);
        linkcard::Card      c = embed(CardLayout::None);
        c.host                = QStringLiteral("evil.example");
        c.redirectedTo        = QStringLiteral("www.paypal.com");
        const QString wide    = linkcard::footerText(c, fm, 400);
        QCOMPARE(wide, QStringLiteral("evil.example ") + QChar(0x2192) + QStringLiteral(" www.paypal.com"));
        // Narrow: the other website goes first (or loses its start), the link's own host stays whole.
        const qreal ownWidth = fm.horizontalAdvance(c.host);
        for (qreal w = ownWidth; w < fm.horizontalAdvance(wide) + 1; w += 3) {
            const QString text = linkcard::footerText(c, fm, w);
            QVERIFY2(text.startsWith(c.host), qPrintable(text));
            QVERIFY2(fm.horizontalAdvance(text) <= w + 0.5, qPrintable(text));
        }
        QVERIFY(!linkcard::footerText(c, fm, ownWidth + 20).contains(QStringLiteral("paypal")));
        c.redirectedTo.clear();
        QCOMPARE(linkcard::footerText(c, fm, 400), c.host);
        // Its data: no site name, no colour from the other website; a card on its own site keeps both.
        CardData data;
        data.title    = QStringLiteral("PayPal");
        data.site     = QStringLiteral("PayPal");
        data.hasColor = true;
        data.color    = qRgb(0x2e, 0x7d, 0x32);
        const CardData other = linkstate::shownCardData(data, LinkClass::Web, QStringLiteral("evil.example"), true);
        QVERIFY(other.site.isEmpty());
        QVERIFY(!other.hasColor);
        const CardData own = linkstate::shownCardData(data, LinkClass::Web, QStringLiteral("www.paypal.com"), false);
        QCOMPARE(own.site, QStringLiteral("PayPal"));
        QVERIFY(own.hasColor);
        QVERIFY(linkstate::shownCardData(data, LinkClass::Web, QStringLiteral("evil.example"), false).site.isEmpty()); // no match
        QVERIFY(!linkstate::shownCardData(data, LinkClass::Site, QStringLiteral("vimeo.com"), false).hasColor);
        // Shown only for a load the user started or for our own message; an automatic or cached load: nothing.
        QVERIFY(!linkstate::crossSiteCardShown(QStringLiteral("www.paypal.com"), false, false));
        QVERIFY(linkstate::crossSiteCardShown(QStringLiteral("www.paypal.com"), true, false));
        QVERIFY(linkstate::crossSiteCardShown(QStringLiteral("www.paypal.com"), false, true));
        QVERIFY(linkstate::crossSiteCardShown(QString(), false, false));
    }

    // Live test fix: our own cross-site card (golang.org -> go.dev) disappeared after a restart: the cache answered for
    // the slot as for anyone's (Nothing), and our pipeline's record, the only "own" path, was gone with the restart.
    void ownCrossSiteCardAfterRestart()
    {
        const Target link = target("https://golang.org/doc/");
        QCOMPARE(link.cls, LinkClass::Web);
        CardData card;
        card.title       = QStringLiteral("Documentation - The Go Programming Language");
        card.description = QStringLiteral("The Go documentation.");
        linkstate::LocalState loading;
        loading.state = Local::Loading;
        const linkstate::ServerState none;
        linkstate::Context           sent; // our own message: we sent LPXNO r=x for it
        sent.senderNothing = true;
        // Our own message (a cached load after the restart): the card, with the host it went to.
        const linkstate::LocalState own = linkstate::afterCard(loading, card, QStringLiteral("go.dev"), true);
        QCOMPARE(own.state, Local::Loaded);
        QCOMPARE(own.kind, Kind::Card);
        QVERIFY(!own.heldCrossSite);
        QCOMPARE(linkstate::decide(link, false, own, none, linkstate::Wait::None, sent).look, Look::Embed);
        // Anyone else's slot first (an automatic or cached load): Nothing, but held; our own slot then shows it.
        const linkstate::LocalState held = linkstate::afterCard(loading, card, QStringLiteral("go.dev"), false);
        QCOMPARE(held.state, Local::Nothing);
        QVERIFY(held.heldCrossSite);
        QVERIFY(held.card.isEmpty());
        QCOMPARE(linkstate::decide(link, false, held, none, linkstate::Wait::None, linkstate::Context()).look, Look::Absent);
        QVERIFY(linkstate::ownShowsHeldCard(held, true));
        QVERIFY(!linkstate::ownShowsHeldCard(held, false));
        // A click shows it to anyone; a card on its own site needs nothing of this.
        linkstate::LocalState clicked = loading;
        clicked.clicked               = true;
        QCOMPARE(linkstate::afterCard(clicked, card, QStringLiteral("go.dev"), false).state, Local::Loaded);
        const linkstate::LocalState same = linkstate::afterCard(loading, card, QString(), false);
        QCOMPARE(same.state, Local::Loaded);
        QVERIFY(!linkstate::ownShowsHeldCard(same, true));
        // No card at all: Nothing, never held (nothing to read again).
        const linkstate::LocalState empty = linkstate::afterCard(loading, CardData(), QStringLiteral("go.dev"), true);
        QCOMPARE(empty.state, Local::Nothing);
        QVERIFY(!empty.heldCrossSite);
        QVERIFY(!linkstate::ownShowsHeldCard(empty, true));
        // A plain "nothing" (a wall, a page without a card) is never read again either.
        linkstate::LocalState nothing;
        nothing.state = Local::Nothing;
        QVERIFY(!linkstate::ownShowsHeldCard(nothing, true));
    }

    void stripLabelsNewKinds()
    {
        linkcard::Card c;
        c.look = Look::Click;
        c.host = QStringLiteral("giphy.com");
        c.cls  = LinkClass::MediaPage;
        c.kind = Kind::Loop;
        QCOMPARE(linkcard::stripLabel(c), QStringLiteral("GIF"));
        QCOMPARE(linkcard::kindLabel(Kind::Loop), QStringLiteral("GIF"));
        c.kind = Kind::Image;
        QCOMPARE(linkcard::stripLabel(c), QStringLiteral("Picture"));
        c.cls      = LinkClass::Site;
        c.kind     = Kind::Card;
        c.provider = QStringLiteral("vimeo");
        QCOMPARE(linkcard::stripLabel(c), QStringLiteral("Vimeo"));
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Vimeo"));
        QVERIFY(linkcard::toolTip(c, linkcard::Zone::Load).contains(QStringLiteral("and to the site its picture comes from")));
        c.cls = LinkClass::Web;
        c.provider.clear();
        QCOMPARE(linkcard::stripLabel(c), QStringLiteral("Web page"));
        c.cls  = LinkClass::MediaPage;
        c.kind = Kind::Gif;
        QVERIFY(linkcard::toolTip(c, linkcard::Zone::Load).contains(QStringLiteral("giphy.com's servers")));
        // A click that found nothing: "No preview for this page", no Retry (Open in browser).
        linkcard::Card none;
        none.look    = Look::Error;
        none.nothing = true;
        none.host    = QStringLiteral("www.example.com");
        QCOMPARE(linkcard::statusText(none), QStringLiteral("No preview for this page"));
        QCOMPARE(linkcard::primaryZone(none), linkcard::Zone::Open);
        // Review fix: its menu's bold item is what a click does (Open in browser), never Retry.
        QCOMPARE(linkcard::menuDefault(none), linkcard::Zone::Open);
        none.nothing = false;
        QCOMPARE(linkcard::menuDefault(none), linkcard::Zone::Retry);
        none.look = Look::Click;
        QCOMPARE(linkcard::menuDefault(none), linkcard::Zone::Load);
        none.look = Look::Hidden;
        QCOMPARE(linkcard::menuDefault(none), linkcard::Zone::Show);
        none.look = Look::Loading;
        QCOMPARE(linkcard::menuDefault(none), linkcard::Zone::Open);
        QCOMPARE(linkcard::menuDefault(embed()), linkcard::Zone::Open);
    }

    void waitingHostTexts()
    {
        linkcard::Card c;
        c.look      = Look::Loading;
        c.waiting   = linkcard::Waiting::Host;
        c.host      = QStringLiteral("i.imgur.com");
        c.busySite  = QStringLiteral("imgur.com");
        c.retryInMs = 43000;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Site busy · retrying in 43 s"));
        QVERIFY(linkcard::toolTip(c, linkcard::Zone::Body).startsWith(QStringLiteral("imgur.com asked to wait")));
        c.retryInMs = 600000;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Site busy · try again in 10 min"));
        c.retryInMs = 61000;
        QCOMPARE(linkcard::statusText(c), QStringLiteral("Site busy · try again in 2 min")); // minutes rounded up
        // Over a minute it is still: two renders far apart in time are the same picture (no ring turning).
        PreviewStyle style = styleOf(false, 340);
        style.animate      = true;
        c.kind             = Kind::Card;
        c.cls              = LinkClass::Web;
        c.retryInMs        = 300000;
        const QImage a     = linkcard::render(c, style, linkcard::Pointer());
        QTest::qWait(120);
        const QImage b = linkcard::render(c, style, linkcard::Pointer());
        QCOMPARE(a, b);
    }

    void hostRunUnaffectedByRtlTitle()
    {
        // The footer's host is drawn the same whatever the title's direction (its own left-to-right run).
        const PreviewStyle style = styleOf(false, 400);
        linkcard::Card     ltr   = embed(CardLayout::None);
        linkcard::Card     rtl   = ltr;
        rtl.data.title           = QStringLiteral("\u0645\u06cc\u200c\u062e\u0648\u0627\u0647\u0645 \u06cc\u06a9 \u062a\u06cc\u062a\u0631 ") + QChar(0x202b)
                         + QStringLiteral("\u0628\u0644\u0646\u062f"); // an unbalanced embedding: clipped, never past its box
        rtl.data.description     = QStringLiteral("\u0645\u062a\u0646 \u06a9\u0648\u062a\u0627\u0647");
        rtl.data.site.clear();
        ltr.data.site.clear();
        ltr.data.description = rtl.data.description;
        ltr.data.title       = QStringLiteral("A plain title");
        linkcard::Layout la, lb;
        const QImage     a = linkcard::render(ltr, style, linkcard::Pointer(), &la);
        const QImage     b = linkcard::render(rtl, style, linkcard::Pointer(), &lb);
        QCOMPARE(la.footer.height(), lb.footer.height());
        const QRect footA = QRectF(la.footer).toAlignedRect();
        const QRect footB = QRectF(lb.footer).toAlignedRect();
        QCOMPARE(a.copy(footA), b.copy(footB));
        // The Persian title keeps its joiner and is clipped to its box.
        QVERIFY(lb.titleLines.join(QString()).contains(QChar(0x200c)));
    }

    void colorPairsPass()
    {
        for (const bool dark : {false, true}) {
            for (const PreviewColorPair& pair : linkcard::colorPairs(dark)) {
                const double ratio = ui::contrastRatio(pair.foreground, pair.background);
                QVERIFY2(ratio + 0.005 >= pair.minimum, qPrintable(QStringLiteral("%1 (%2): %3 < %4").arg(pair.name).arg(dark ? "dark" : "light").arg(ratio).arg(pair.minimum)));
            }
        }
        // Every brand line and bar is there.
        int brands = 0;
        for (const PreviewColorPair& pair : linkcard::colorPairs(false))
            brands += pair.name.startsWith(QLatin1String("embed brand line ")) ? 1 : 0;
        QVERIFY(brands >= 15);
    }

    // ---- the loop pool (InlineMediaController's) ----------------------------------------------------------------
    void loopModePolicy()
    {
        using loopplayers::Slot;
        QCOMPARE(InlineMediaController::kMaxLoopPlayers, 4);
        // Five visible loops, autoplay: four get players, the fifth shows its poster.
        QVector<Slot> loops;
        for (int i = 0; i < 5; ++i)
            loops.append({QStringLiteral("l%1").arg(i), false, true, false, 0});
        loopplayers::Plan p = loopplayers::plan(loops, true, 4);
        QCOMPARE(p.start.size(), 4);
        QVERIFY(p.evict.isEmpty());
        // Without autoplay only the hovered one plays.
        loops[2].hovered = true;
        p = loopplayers::plan(loops, false, 4);
        QCOMPARE(p.start, QStringList({QStringLiteral("l2")}));
        loops[2].hovered = false;
        // An invisible loop's player goes before any visible one's.
        for (int i = 0; i < 4; ++i)
            loops[i].hasPlayer = true;
        loops[0].visible  = false;
        loops[0].lastUsed = 9; // recently used, but out of view
        loops[1].lastUsed = 1;
        p = loopplayers::plan(loops, true, 4);
        QCOMPARE(p.evict, QStringList({QStringLiteral("l0")}));
        QCOMPARE(p.start, QStringList({QStringLiteral("l4")}));
        QVERIFY(!p.play.contains(QStringLiteral("l0")) && !p.pause.contains(QStringLiteral("l0")));
        // All four players on visible loops: a fifth visible one waits (poster), never takes one...
        loops[0].visible = true;
        p                = loopplayers::plan(loops, true, 4);
        QVERIFY(p.start.isEmpty() && p.evict.isEmpty());
        QCOMPARE(p.play.size(), 4);
        // ...unless it is hovered: then the visible one used longest ago gives its player.
        loops[4].hovered = true;
        p                = loopplayers::plan(loops, true, 4);
        QCOMPARE(p.evict, QStringList({QStringLiteral("l2")})); // lastUsed 0 (l1 is 1, l0 is 9, l3 is 0 but later)
        QCOMPARE(p.start, QStringList({QStringLiteral("l4")}));
        // Out of view: paused, kept (while no loop in view needs its player).
        loops[4].hovered = false;
        loops[4].visible = false;
        loops[1].visible = false;
        p                = loopplayers::plan(loops, true, 4);
        QVERIFY(p.pause.contains(QStringLiteral("l1")));
        QVERIFY(p.evict.isEmpty());
        // A loop coming into view takes the paused one's player.
        loops[4].visible = true;
        p                = loopplayers::plan(loops, true, 4);
        QCOMPARE(p.evict, QStringList({QStringLiteral("l1")}));
    }

    void loopPoolNoChurn()
    {
        using loopplayers::Slot;
        // Six visible loops, 100 repaints: four players made in all, never one destroyed.
        QVector<Slot> loops;
        for (int i = 0; i < 6; ++i)
            loops.append({QStringLiteral("l%1").arg(i), false, true, false, 0});
        int created = 0, destroyed = 0;
        for (int round = 0; round < 100; ++round) {
            const loopplayers::Plan p = loopplayers::plan(loops, true, InlineMediaController::kMaxLoopPlayers);
            created += p.start.size();
            destroyed += p.evict.size();
            for (Slot& s : loops) {
                if (p.start.contains(s.key))
                    s.hasPlayer = true;
                if (p.evict.contains(s.key))
                    s.hasPlayer = false;
            }
        }
        QCOMPARE(created, 4);
        QCOMPARE(destroyed, 0);
    }

    // ---- the sender and every receiver pick the same links (D35) -------------------------------------------------
    void senderSelectionMatchesReceivers_data()
    {
        QTest::addColumn<QString>("bbcode");
        QTest::addColumn<QStringList>("anchors"); // the hrefs TeamSpeak makes of it on receivers
        const QString persian = QString::fromUtf8("https://fa.wikipedia.org/wiki/\xd8\xaa\xdb\x8c\xd9\x85\xe2\x80\x8c\xd8\xa7\xd8\xb3\xd9\xbe\xdb\x8c\xda\xa9");
        QTest::newRow("raw ZWNJ in a Persian path") << QStringLiteral("[URL]%1[/URL]").arg(persian) << QStringList{persian};
        QTest::newRow("Foo_(bar)") << QStringLiteral("see [URL]https://en.wikipedia.org/wiki/Foo_(bar)[/URL]")
                                   << QStringList{QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)")};
        QTest::newRow("trailing dot, bang, quote") << QStringLiteral("[URL]https://www.example.com/a.[/URL] [URL]https://www.example.net/b![/URL] [URL]https://www.example.org/c'[/URL]")
                                                   << QStringList{QStringLiteral("https://www.example.com/a."), QStringLiteral("https://www.example.net/b!"),
                                                                  QStringLiteral("https://www.example.org/c'")};
        QTest::newRow("www. without a scheme") << QStringLiteral("[URL]www.example.org/news/today[/URL]") << QStringList{QStringLiteral("http://www.example.org/news/today")};
        // Live test: TeamSpeak links an address without [URL] only at the start or after white space (seen on a
        // receiver for each of these from a ServerQuery sender).
        QTest::newRow("bare addresses TeamSpeak leaves as text")
            << QStringLiteral("see (https://www.example.com/p1) and https://www.example.com/p2 x:https://www.example.com/p3 \"https://www.example.com/p4\" "
                              "\\[URL]https://www.example.com/p5\\[/URL] [b]x[/b]https://www.example.com/p6\thttps://www.example.com/p7")
            << QStringList{QStringLiteral("https://www.example.com/p2"), QStringLiteral("https://www.example.com/p7")};
        QTest::newRow("bare address first") << QStringLiteral("https://www.example.com/first") << QStringList{QStringLiteral("https://www.example.com/first")};
        QTest::newRow("3 files and 2 pages") << QStringLiteral("https://www.example.com/p1 https://a.example.com/1.png https://www.example.com/p2 https://a.example.com/2.png https://a.example.com/3.gif")
                                             << QStringList{QStringLiteral("https://www.example.com/p1"), QStringLiteral("https://a.example.com/1.png"),
                                                            QStringLiteral("https://www.example.com/p2"), QStringLiteral("https://a.example.com/2.png"),
                                                            QStringLiteral("https://a.example.com/3.gif")};
        QString     many;
        QStringList manyAnchors;
        for (int i = 0; i < 32; ++i) {
            many += QStringLiteral("https://t%1.example.com/x ").arg(i);
            manyAnchors << QStringLiteral("https://t%1.example.com/x").arg(i);
        }
        many += QStringLiteral("https://a.example.com/late.png");
        manyAnchors << QStringLiteral("https://a.example.com/late.png");
        QTest::newRow("a file after the 32nd link") << many << manyAnchors;
    }
    void senderSelectionMatchesReceivers()
    {
        QFETCH(QString, bbcode);
        QFETCH(QStringList, anchors);
        // The sender: its own BBCode.
        const QVector<Target> sent = linkpreview::SenderPipeline::selectedLinks(bbcode);
        // A receiver: TeamSpeak's anchors in its chat document.
        const tschat::Skin skin = tschat::lightSkin();
        QTextDocument      doc;
        doc.setTextWidth(600);
        QString body;
        for (const QString& href : anchors)
            body += tschat::linkHtml(skin, href) + QStringLiteral(" ");
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("20:01:00"), 17, QStringLiteral("q0Xn6Alice+00000/000000000="), QStringLiteral("Alice"), body));
        const QVector<linkdoc::Link> seen = linkdoc::findLinks(&doc);
        QStringList a, b;
        for (const Target& t : sent)
            a << t.key;
        for (const linkdoc::Link& l : seen)
            b << l.target.key;
        QCOMPARE(b.join(QLatin1Char(',')), a.join(QLatin1Char(',')));
        QVERIFY(!a.isEmpty());
        QVERIFY(a.size() <= linkpreview::kMaxSlots);
        // The index of each selected link is its place among them.
        for (int i = 0; i < seen.size(); ++i)
            QCOMPARE(seen.at(i).index, i);
        if (QTest::currentDataTag() == QLatin1String("a file after the 32nd link")) {
            for (const Target& t : sent)
                QVERIFY(!t.url.toString().contains(QLatin1String("late.png"))); // never looked at, on either side
        }
        if (QTest::currentDataTag() == QLatin1String("3 files and 2 pages")) {
            for (const Target& t : sent)
                QVERIFY(t.isLegacy()); // the files first
        }
        // The quiet slot of a link is found where its line is.
        const QRectF line = linkdoc::lineRect(&doc, seen.first().end - 1);
        QVERIFY(!line.isEmpty());
    }

    void brandLineStyle()
    {
        // A site's brand line is DemiBold in its colour; a web page's site line regular in the muted colour; a web page
        // can't wear a brand's bar colour (pagemeta refuses it, the card falls back to the neutral bar).
        const PreviewStyle style = styleOf(false, 400);
        linkcard::Card     site  = embed(CardLayout::Large, true);
        linkcard::Card     web   = embed(CardLayout::Large, false);
        QCOMPARE(linkcard::layout(site, style).siteText, QStringLiteral("Vimeo"));
        QCOMPARE(linkcard::layout(web, style).siteText, QStringLiteral("Example News"));
        const QColor brand = linkcard::brandLineColour(site, style);
        QVERIFY(brand != linkcard::brandLineColour(web, style));
        QVERIFY(ui::contrastRatio(brand, QColor(Qt::white)) >= 4.5);
        web.data.hasColor = false;
        QCOMPARE(linkcard::barColour(web, style), QColor(0xc9, 0xcd, 0xd3));
        web.data.hasColor = true;
        web.data.color    = qRgb(0x2e, 0x7d, 0x32);
        QVERIFY(ui::contrastRatio(linkcard::barColour(web, style), QColor(Qt::white)) >= 3.0);
        // The bar of a site: its brand (readable 3:1).
        QVERIFY(ui::contrastRatio(linkcard::barColour(site, style), QColor(Qt::white)) >= 3.0);
    }
};

TSMEDIA_REGISTER_TEST(TestLinkUi25)
#include "tst_linkui25.moc"
