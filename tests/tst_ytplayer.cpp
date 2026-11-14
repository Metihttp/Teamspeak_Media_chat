// 2.5 YouTube player (design2.md 3, 4.1 Y-T1..Y-T4, Y-T6, Y-T7): the page and its grammar, the card's states (the Consent
// state drawn and hit-tested like the others), the state machine with a fake backend (Consent once, the creation and
// ready timers, notes and which are remembered, the engine retry, one player at a time, the time bookkeeping, Lost, Esc,
// pausing rules, the switch), the anchored window's geometry and visibility rules (behind its gate), unload, the
// settings and the source rules. Offscreen, no network, no WebView2, no TeamSpeak.

#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QFontInfo>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QtTest>

#include <cwchar>
#include <memory>

#include "linkpreview/linkcard.h"
#include "previewpaint.h"
#include "previewscale.h"
#include "privacysection.h"
#include "settings.h"
#include "testmain.h"
#include "proxy/records.h"
#include "tsmp.h"
#include "tsmpmsg.h"
#include "tsmpnet.h"
#include "ytplayer/ytbackend.h"
#include "ytplayer/ytpage.h"
#include "ytplayer/ytplayer.h"

namespace {

QString a(const char* text)
{
    return QString::fromLatin1(text);
}

// What the fake backend saw (shared with the test after the player took the backend).
struct FakeState {
    int                  loads = 0, shutdowns = 0, releases = 0, next = 0;
    bool                 failLoad = false, refuseOpen = false;
    QVector<PlayerOpen>  opens;
    QStringList          places;   // "id visible x,y wxh clip x,y wxh"
    QStringList          commands; // "id:command"
    QVector<int>         closes;
    QVector<PlayerEvent> queue;
    // 2.6 chat proxy: every setNetwork (its kind and port) in order with the opens ("net:<kind>" / "open")
    QStringList          calls;
    int                  refuseNetwork = 0; // > 0: setNetwork answers -1 that many times
    void push(int player, int kind, int value = 0, const QString& text = QString())
    {
        PlayerEvent e;
        e.player = player;
        e.kind   = kind;
        e.value  = value;
        e.text   = text;
        queue.append(e);
    }
    bool sent(int id, const char* command) const { return commands.contains(QString::number(id) + QLatin1Char(':') + QLatin1String(command)); }
};

class FakeBackend : public PlayerBackend
{
  public:
    explicit FakeBackend(std::shared_ptr<FakeState> s)
        : st(std::move(s))
    {
    }
    bool load(QString* why) override
    {
        ++st->loads;
        if (st->failLoad) {
            if (why)
                *why = a("load 5");
            return false;
        }
        return true;
    }
    int open(const PlayerOpen& o) override
    {
        st->opens.append(o);
        st->calls << a("open");
        return st->refuseOpen ? 0 : ++st->next;
    }
    int setNetwork(const proxy::PlayerNetwork& net) override
    {
        static const char* const kinds[] = {"blocked", "system", "direct", "bridge"};
        st->calls << a("net:") + a(kinds[static_cast<int>(net.kind)]) + (net.bridgePort ? a(":") + QString::number(net.bridgePort) : QString());
        if (st->refuseNetwork > 0) {
            --st->refuseNetwork;
            return -1;
        }
        return net.kind == proxy::PlayerNetwork::Kind::Blocked ? -1 : 0;
    }
    void place(int id, const QRect& rect, const QRect& clip, double, bool visible) override
    {
        st->places << QString::fromLatin1("%1 %2 %3,%4 %5x%6 clip %7,%8 %9x%10")
                          .arg(id)
                          .arg(visible ? 1 : 0)
                          .arg(rect.x())
                          .arg(rect.y())
                          .arg(rect.width())
                          .arg(rect.height())
                          .arg(clip.x())
                          .arg(clip.y())
                          .arg(clip.width())
                          .arg(clip.height());
    }
    void command(int id, const QString& c) override { st->commands << QString::number(id) + QLatin1Char(':') + c; }
    void close(int id) override { st->closes << id; }
    QVector<PlayerEvent> poll() override
    {
        const QVector<PlayerEvent> out = st->queue;
        st->queue.clear();
        return out;
    }
    void    releaseEngine() override { ++st->releases; }
    void    shutdown() override { ++st->shutdowns; }
    QString runtimeVersion() override { return a("fake"); }

    std::shared_ptr<FakeState> st;
};

// The chat side: spots by slot id, what was opened, redrawn, paused.
struct FakeHost {
    QHash<QString, YouTubePlayer::Spot> spots;
    QStringList                         changed;
    QList<QUrl>                         opened;
    int                                 pauses = 0;
    bool                                muted  = false;
    QStringList                         scrolled;
    // 2.6 chat proxy: the network the player is told (the Windows settings by default), the bridge's player records,
    // what route health heard, the feedback lines.
    proxy::PlayerNetwork                net = []() {
        proxy::PlayerNetwork n;
        n.kind       = proxy::PlayerNetwork::Kind::System;
        n.blockedBy  = proxy::Failure::None;
        n.generation = 1;
        return n;
    }();
    QVector<proxy::Record>              records;
    QVector<quint64>                    recordsAsked;
    QVector<proxy::RouteReport>         reports;
    QVector<quint64>                    oks;
    QStringList                         feedback;
    QTextBrowser*                       browser = nullptr; // spot.shown follows its visibility when set

    YouTubePlayer::Host make()
    {
        YouTubePlayer::Host h;
        h.locate = [this](const QString& slot, YouTubePlayer::Spot* spot) {
            const auto it = spots.constFind(slot);
            if (it == spots.constEnd())
                return false;
            *spot = it.value();
            if (browser)
                spot->shown = spot->shown && browser->isVisible();
            return true;
        };
        h.changed        = [this](const QString& slot) { changed << slot; };
        h.openUrl        = [this](const QUrl& url) { opened << url; };
        h.pauseOthers    = [this] { ++pauses; };
        h.muted          = [this] { return muted; };
        h.scrollIntoView = [this](const QString& slot) {
            scrolled << slot;
            auto it = spots.find(slot);
            if (it != spots.end())
                it->thumb.moveTop(it->view.top() + 10); // scrolled so the whole thumbnail shows
        };
        h.network       = [this] { return net; };
        h.playerRecords = [this](quint64 since) {
            recordsAsked << since;
            return records;
        };
        h.report   = [this](const proxy::RouteReport& r) { reports << r; };
        h.reportOk = [this](quint64 generation) { oks << generation; };
        h.feedback = [this](const QString& slot, const QString& text) { feedback << slot + QLatin1Char(' ') + text; };
        return h;
    }
};

YouTubePlayer::Spot spotAt(const QRect& thumb, const QRect& view = QRect(0, 0, 1000, 800), double s = 1.0)
{
    YouTubePlayer::Spot spot;
    spot.thumb    = thumb;
    spot.view     = view;
    spot.owner    = QRect(0, 0, 1600, 1000);
    spot.thumbDu  = QSizeF(thumb.width() / s, thumb.height() / s);
    spot.s        = s;
    spot.dpr      = 1.0;
    spot.sysScale = 1.0;
    spot.title    = a("A video");
    return spot;
}

QString runtimeReader(ytbackend::Hive hive)
{
    return hive == ytbackend::Hive::LocalMachine32 ? a("157.0.4341.42") : QString();
}

const QString kVideo = a("M7lc1UVf-VE");
const QUrl    kLink  = QUrl(a("https://youtu.be/M7lc1UVf-VE?t=90"));

} // namespace

class TestYouTubePlayer : public QObject
{
    Q_OBJECT

  private:
    std::shared_ptr<FakeState>     st;
    FakeHost                       host;
    std::unique_ptr<YouTubePlayer> player;
    QTemporaryDir                  tmp;

    void make()
    {
        st     = std::make_shared<FakeState>();
        host   = FakeHost();
        player = std::make_unique<YouTubePlayer>(host.make(), std::make_unique<FakeBackend>(st));
        player->setUserDataFolderForTests(tmp.path() + a("/webview2"));
        host.spots.insert(a("s1"), spotAt(QRect(100, 100, 362, 204)));
        host.spots.insert(a("s2"), spotAt(QRect(100, 400, 362, 204)));
    }
    // The 50 ms poll timer has taken every queued event (a late timer tick never makes a test flaky).
    void poll()
    {
        QTest::qWait(60);
        for (int i = 0; i < 200 && !st->queue.isEmpty(); ++i)
            QTest::qWait(10);
    }
    // An owned single-shot timer fires now (its interval kept for its next start). 2.6: waited for by its own signal, not a
    // fixed 30 ms: late in the whole suite Windows may serve a 1 ms timer with a 15.6 ms tick (USER timer fallback), and
    // two ticks overran the fixed wait.
    void fire(const char* timer)
    {
        QTimer* t = player->timerForTests(timer);
        QVERIFY(t);
        const int  interval = t->interval();
        QSignalSpy fired(t, &QTimer::timeout);
        t->start(1);
        QVERIFY(fired.count() > 0 || fired.wait(5000));
        if (!t->isActive())
            t->setInterval(interval);
        else
            QFAIL("the timer runs again");
    }
    YouTubePlayer::State stateOf(const char* slot) const { return player->cardOf(QString::fromLatin1(slot), kVideo).state; }
    // Play on a slot up to Ready (the notice answered).
    int playToReady(const char* slot = "s1")
    {
        Settings::instance().youTubeNoticeShown = true;
        player->play(QString::fromLatin1(slot), kVideo, kLink);
        const int id = st->next;
        st->push(id, TSMP_EV_CREATED);
        poll();
        st->push(id, TSMP_EV_READY);
        poll();
        return id;
    }

  private slots:
    void initTestCase()
    {
        QVERIFY(tmp.isValid());
        ytbackend::setRegistryReaderForTests(runtimeReader);
    }
    void cleanupTestCase() { ytbackend::setRegistryReaderForTests(nullptr); }
    void init()
    {
        Settings::instance() = Settings();
        make();
    }
    void cleanup()
    {
        player.reset();
        Settings::instance() = Settings();
    }

    // ---- Y-T1 the page ----
    void ids()
    {
        QVERIFY(ytpage::validId(a("M7lc1UVf-VE")));
        QVERIFY(ytpage::validId(a("abc_DEF-123")));
        QVERIFY(!ytpage::validId(a("M7lc1UVf-V")));
        QVERIFY(!ytpage::validId(a("M7lc1UVf-VEx")));
        QVERIFY(!ytpage::validId(a("M7lc1UVf+VE")));
        QVERIFY(!ytpage::validId(QString::fromUtf8("M7lc1UVf-V\xC3\xA9")));
        QVERIFY(!ytpage::validId(QString()));
    }
    void startTimes()
    {
        QCOMPARE(ytpage::parseStart(a("90")), 90);
        QCOMPARE(ytpage::parseStart(a("90s")), 90);
        QCOMPARE(ytpage::parseStart(a("1m30s")), 90);
        QCOMPARE(ytpage::parseStart(a("1h")), 3600);
        QCOMPARE(ytpage::parseStart(a("1h2m3s")), 3723);
        QCOMPARE(ytpage::parseStart(a("2m")), 120);
        QCOMPARE(ytpage::parseStart(a("86400")), 86400);
        QCOMPARE(ytpage::parseStart(a("86401")), 0);
        QCOMPARE(ytpage::parseStart(a("25h")), 0);
        QCOMPARE(ytpage::parseStart(a("abc")), 0);
        QCOMPARE(ytpage::parseStart(a("-5")), 0);
        QCOMPARE(ytpage::parseStart(a("1m30")), 0);
        QCOMPARE(ytpage::parseStart(QString()), 0);
        QCOMPARE(ytpage::startOf(QUrl(a("https://youtu.be/M7lc1UVf-VE?t=1m30s"))), 90);
        QCOMPARE(ytpage::startOf(QUrl(a("https://www.youtube.com/watch?v=M7lc1UVf-VE&start=42"))), 42);
        QCOMPARE(ytpage::startOf(QUrl(a("https://www.youtube.com/watch?v=M7lc1UVf-VE#t=10"))), 10);
        QCOMPARE(ytpage::startOf(QUrl(a("https://www.youtube.com/watch?v=M7lc1UVf-VE"))), 0);
    }
    void pageGolden()
    {
        const QString expected = a(
            "<!doctype html><html><head><meta charset=\"utf-8\">\n"
            "<meta name=\"referrer\" content=\"strict-origin-when-cross-origin\">\n"
            "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline' https://www.youtube.com; frame-src "
            "https://www.youtube-nocookie.com; style-src 'unsafe-inline'\">\n"
            "<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}iframe{border:0;width:100%;height:100%}</style>\n"
            "</head><body>\n"
            "<iframe id=\"p\" allow=\"autoplay; encrypted-media\"\n"
            "  src=\"https://www.youtube-nocookie.com/embed/M7lc1UVf-VE?enablejsapi=1&origin=https%3A%2F%2Fio.github.metihttp.tsmediachat&autoplay=1&rel=0&fs=0"
            "&start=90\"></iframe>\n"
            "<script>\n"
            "var p = null;\n"
            "function tell(s) { window.chrome.webview.postMessage(s); }\n"
            "function now() { if (p && p.getCurrentTime) tell('time:' + Math.floor(p.getCurrentTime())); }\n"
            "function onYouTubeIframeAPIReady() {\n"
            "  p = new YT.Player('p', { events: {\n"
            "    onReady: function () { tell('ready'); },\n"
            "    onStateChange: function (e) { tell('state:' + e.data); now(); },\n"
            "    onError: function (e) { tell('error:' + e.data); } } });\n"
            "  setInterval(function () { if (p.getPlayerState && p.getPlayerState() === 1) now(); }, 5000);\n"
            "}\n"
            "window.chrome.webview.addEventListener('message', function (e) {\n"
            "  var c = String(e.data);\n"
            "  if (!p || !p.pauseVideo) return;\n"
            "  if (c === 'pause') p.pauseVideo(); else if (c === 'play') p.playVideo();\n"
            "  else if (c === 'mute') p.mute(); else if (c === 'unmute') p.unMute();\n"
            "  else if (/^volume:\\d{1,3}$/.test(c)) p.setVolume(Math.min(100, parseInt(c.slice(7), 10)));\n"
            "});\n"
            "</script>\n"
            "<script src=\"https://www.youtube.com/iframe_api\"></script>\n"
            "</body></html>\n");
        const QString page = ytpage::pageHtml(kVideo, 90, true);
        QCOMPARE(page, expected);
        QCOMPARE(ytpage::pageHtml(kVideo, 90, false), expected); // the same document for both windows
        QVERIFY(page.contains(a("fs=0")));
        QVERIFY(!page.contains(a("allowfullscreen")) && !page.contains(a("fullscreen;")) && !page.contains(a("picture-in-picture")));
        QVERIFY(page.contains(a("}, 5000);"))); // the periodic time report
        QVERIFY(ytpage::pageHtml(a("bad id"), 0, true).isEmpty());
        QVERIFY(ytpage::pageHtml(kVideo, 999999, true).contains(a("&start=86400\"")));
        QVERIFY(ytpage::pageHtml(kVideo, -3, true).contains(a("&start=0\"")));
        QVERIFY(!page.contains(a("mute=1")));
        // TS Media's session mute: YouTube starts muted (the commands come only after Ready; review fix). Only that
        // parameter differs.
        QString muted = expected;
        muted.replace(a("&start=90\""), a("&start=90&mute=1\""));
        QCOMPARE(ytpage::pageHtml(kVideo, 90, true, QString(), true), muted);
        // Test mode (player_smoke): the fake API from our origin, which the CSP allows then.
        const QString test = ytpage::pageHtml(kVideo, 0, true, a("https://io.github.metihttp.tsmediachat/test_api.js"));
        QVERIFY(test.contains(a("script-src 'unsafe-inline' https://www.youtube.com https://io.github.metihttp.tsmediachat;")));
        QVERIFY(test.contains(a("<script src=\"https://io.github.metihttp.tsmediachat/test_api.js\"></script>")));
        QVERIFY(!test.contains(a("iframe_api")));
        QVERIFY(ytpage::spikePage().contains(a("postMessage('ready')")));
    }
    void messages()
    {
        QCOMPARE(ytpage::parseMessage(a("ready")).kind, 1);
        QCOMPARE(ytpage::parseMessage(a("state:1")).kind, 2);
        QCOMPARE(ytpage::parseMessage(a("state:-1")).value, -1);
        QCOMPARE(ytpage::parseMessage(a("state:100")).kind, 0);
        QCOMPARE(ytpage::parseMessage(a("error:150")).value, 150);
        QCOMPARE(ytpage::parseMessage(a("error:12345")).kind, 0);
        QCOMPARE(ytpage::parseMessage(a("time:123456")).value, 123456);
        QCOMPARE(ytpage::parseMessage(a("time:1234567")).kind, 0);
        QCOMPARE(ytpage::parseMessage(a("time:")).kind, 0);
        for (const char* bad : {"READY", "ready ", " ready", "state:1;", "state:+1", "error:-1", "time:1.5", "", "eval:x"})
            QCOMPARE(ytpage::parseMessage(a(bad)).kind, 0);
        QCOMPARE(ytpage::parseMessage(QString(100, QLatin1Char('a'))).kind, 0);
        QCOMPARE(ytpage::parseMessage(QString::fromUtf8("ready\xE2\x80\x8B")).kind, 0);
        QCOMPARE(ytpage::parseMessage(QString::fromUtf8("state:\xD9\xA1")).kind, 0); // an Arabic digit
        QCOMPARE(ytpage::parseMessage(a("test:pause")).kind, 0);
        QCOMPARE(ytpage::parseMessage(a("test:pause"), true).kind, 5);
        QCOMPARE(ytpage::parseMessage(a("test:<script>"), true).kind, 0);
        for (const char* ok : {"pause", "play", "mute", "unmute", "volume:0", "volume:100"})
            QVERIFY2(ytpage::validCommand(a(ok)), ok);
        for (const char* bad : {"volume:101", "volume:-1", "volume:", "volume:1000", "Pause", "eval", "front", "time", ""})
            QVERIFY2(!ytpage::validCommand(a(bad)), bad);
    }
    void geometryRules()
    {
        QCOMPARE(ytpage::cssScale(1.0, 1.0), 1.0);
        QCOMPARE(ytpage::cssScale(1.5, 2.0), 3.0);
        QVERIFY(ytpage::canPlayInPlace(true, QSizeF(362, 204), 1.0, 1.0));
        QVERIFY(!ytpage::canPlayInPlace(true, QSizeF(362, 204), 0.85, 1.0));
        QVERIFY(ytpage::canPlayInPlace(true, QSizeF(362, 204), 1.45, 1.5));
        QVERIFY(!ytpage::canPlayInPlace(true, QSizeF(300, 169), 1.0, 1.0));
        QVERIFY(!ytpage::canPlayInPlace(false, QSizeF(362, 204), 1.0, 1.0));
        QVERIFY(!ytpage::kAnchoredPlayer); // the release gate stays closed until Y-L4..Y-L8 pass (Y-L15)
        QCOMPARE(ytpage::visibleFraction(QRect(0, 0, 100, 100), QRect(0, 0, 100, 100)), 1.0);
        QCOMPARE(ytpage::visibleFraction(QRect(0, 0, 100, 100), QRect(0, 50, 100, 100)), 0.5);
        QCOMPARE(ytpage::visibleFraction(QRect(0, 0, 100, 100), QRect(200, 200, 10, 10)), 0.0);
        QCOMPARE(ytpage::visibleFraction(QRect(), QRect(0, 0, 10, 10)), 0.0);
        QCOMPARE(ytpage::popupSizeDu(QString()), QSize(640, 360));
        QCOMPARE(ytpage::popupSizeDu(a("800x450")), QSize(800, 450));
        QCOMPARE(ytpage::popupSizeDu(a("100x50")), QSize(356, 200));
        QCOMPARE(ytpage::popupSizeDu(a("garbage")), QSize(640, 360));
        QCOMPARE(ytpage::popupSizeText(QSize(700, 400)), a("700x400"));
        QCOMPARE(ytpage::popupRect(QSize(640, 360), 1.0, 1.0, QRect(0, 0, 1600, 1000)), QRect(480, 320, 640, 360));
        QCOMPARE(ytpage::popupRect(QSize(640, 360), 1.5, 2.0, QRect(0, 0, 3000, 2000)).size(), QSize(1920, 1080));
        QCOMPARE(ytpage::sizeDuFromDevice(a("1280x720"), 2.0, 1.0), QSize(640, 360));
        QVERIFY(!ytpage::sizeDuFromDevice(a("x"), 1.0, 1.0).isValid());
    }
    void notesAndTexts()
    {
        QCOMPARE(ytpage::noteFor(101), ytpage::Note::OnlyOnYouTube);
        QCOMPARE(ytpage::noteFor(150), ytpage::Note::OnlyOnYouTube);
        QCOMPARE(ytpage::noteFor(100), ytpage::Note::Unavailable);
        for (const int e : {2, 5, 153, -1, -2, 9999})
            QCOMPARE(ytpage::noteFor(e), ytpage::Note::CantPlay);
        for (const int e : {100, 101, 150})
            QVERIFY(ytpage::rememberedError(e));
        for (const int e : {2, 5, 153, -1})
            QVERIFY(!ytpage::rememberedError(e));
        QCOMPARE(ytpage::noteText(ytpage::Note::OnlyOnYouTube), a("This video can only be watched on YouTube"));
        QCOMPARE(ytpage::noteText(ytpage::Note::Unavailable), a("Video unavailable"));
        QCOMPARE(ytpage::noteText(ytpage::Note::CantPlay), a("Can't play this video here"));
        QCOMPARE(ytpage::timeText(65000), a("1:05"));
        QCOMPARE(ytpage::timeText(3723000), a("1:02:03"));
        QCOMPARE(ytpage::watchUrl(kVideo, 42), QUrl(a("https://www.youtube.com/watch?v=M7lc1UVf-VE&t=42s")));
        QCOMPARE(ytpage::watchUrl(kVideo, 0), QUrl(a("https://www.youtube.com/watch?v=M7lc1UVf-VE")));
        QCOMPARE(ytpage::termsUrl(), QUrl(a("https://www.youtube.com/t/terms")));
        QCOMPARE(ytpage::privacyUrl(), QUrl(a("https://policies.google.com/privacy")));
        QVERIFY(ytpage::consentNotice().contains(a("privacy-enhanced mode")));
        QVERIFY(ytpage::consentNotice().contains(a("Microsoft")));
    }

    // ---- the card's states (linkcard) ----
    void cardStates()
    {
        linkcard::Card card;
        card.look     = linkcard::Look::YouTube;
        card.kind     = linkpreview::Kind::YouTube;
        card.cls      = linkpreview::LinkClass::YouTube;
        card.host     = a("youtube.com");
        card.title    = a("A video with a title");
        card.author   = a("A channel");
        card.playable = true;
        for (const int width : {160, 280, 400}) {
            PreviewStyle style;
            style.maxWidth = width;
            card.player    = linkcard::Player::None;
            const linkcard::Layout none = linkcard::layout(card, style);
            QCOMPARE(none.size.width(), width);
            // Sizes never depend on state.
            for (const linkcard::Player p : {linkcard::Player::Consent, linkcard::Player::Starting, linkcard::Player::Open, linkcard::Player::Popup,
                                             linkcard::Player::Hidden, linkcard::Player::Note}) {
                card.player = p;
                card.note   = a("Video unavailable");
                QCOMPARE(linkcard::layout(card, style).size, none.size);
                QVERIFY(!linkcard::render(card, style, linkcard::Pointer()).isNull());
            }
            // Consent: its parts inside the card, the notice from 280 du, the pills apart.
            card.player              = linkcard::Player::Consent;
            const linkcard::Layout l = linkcard::layout(card, style);
            const QRectF           box(QPointF(0, 0), QSizeF(l.size));
            for (const QRectF& r : {l.question, l.terms, l.privacy, l.consentPlay, l.consentBrowser})
                QVERIFY2(box.contains(r.center()) && r.bottom() <= box.bottom() + 0.5 && r.top() >= 0, qPrintable(QString::number(width)));
            QVERIFY(!l.consentPlay.isEmpty() && !l.consentBrowser.isEmpty());
            QVERIFY(!l.consentPlay.intersects(l.consentBrowser));
            QCOMPARE(l.noticeLines.isEmpty(), width < 280);
            QCOMPARE(linkcard::zoneAt(card, l, l.consentPlay.center(), true), linkcard::Zone::ConsentPlay);
            QCOMPARE(linkcard::zoneAt(card, l, l.consentBrowser.center(), true), linkcard::Zone::ConsentBrowser);
            if (l.terms.width() > 4 && l.privacy.width() > 4) { // offscreen without fonts the texts have no width
                QCOMPARE(linkcard::zoneAt(card, l, l.terms.center(), true), linkcard::Zone::Terms);
                QCOMPARE(linkcard::zoneAt(card, l, l.privacy.center(), true), linkcard::Zone::Privacy);
            }
            QCOMPARE(linkcard::zoneAt(card, l, QPointF(2, 2), true), linkcard::Zone::Body); // the rest does nothing
            QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), true), linkcard::Zone::Body); // no hide button on it
            QVERIFY(!linkcard::isActionable(card, linkcard::Zone::Body));
            QVERIFY(linkcard::toolTip(card, linkcard::Zone::Body).contains(a("YouTube Terms of Service")));
            QVERIFY(linkcard::toolTipHtml(card, linkcard::Zone::Body).startsWith(a("<p>"))); // the notice wraps
            // The note's reason (up to three lines) and its pill, and the popup's pill, are never cut, at 160 du too (review
            // finding: "This video can only ...", "Watch on ...", "Playing in a wi...").
            const QChar         ellipsis(0x2026);
            const QFont         noteFont = previewpaint::font(style, -1, true);
            const QFontMetricsF fm(noteFont);
            // Offscreen without Windows' fonts Qt measures with a wide stand-in: the "never cut" checks need real metrics
            // (Segoe UI); the live run's gallery covers them where they don't hold (Y-L checks).
            const bool realFont = fm.horizontalAdvance(a("abcdefghij")) < 10 * 0.7 * QFontInfo(noteFont).pixelSize();
            for (const QString& note : {ytpage::noteText(ytpage::Note::OnlyOnYouTube), ytpage::noteText(ytpage::Note::Unavailable),
                                        ytpage::noteText(ytpage::Note::CantPlay)}) {
                card.player                 = linkcard::Player::Note;
                card.note                   = note;
                const linkcard::Layout   nl = linkcard::layout(card, style);
                const linkcard::NoteText nt = linkcard::noteTextFor(card, nl, style);
                QVERIFY(!nt.lines.isEmpty() && nt.lines.size() <= 3);
                QVERIFY(nt.lines.size() * fm.height() + 10 + 26 <= nl.thumb.height() + 0.5); // the block fits the thumbnail
                if (realFont)
                    QVERIFY2(!nt.lines.join(QLatin1Char(' ')).contains(ellipsis), qPrintable(QString::number(width) + QLatin1Char(' ') + note));
                if (width >= 400)
                    QCOMPARE(nt.lines.join(QLatin1Char(' ')), note); // wide cards say it whole
                QVERIFY(nt.pillWidth <= nl.thumb.width() - 16 + 0.01);
                if (realFont)
                    QVERIFY2(fm.horizontalAdvance(nt.pill) + (nt.pillIcon ? 20 : 0) + 24 <= nl.thumb.width() - 16 + 0.5, qPrintable(nt.pill));
                QVERIFY(!linkcard::render(card, style, linkcard::Pointer()).isNull());
            }
            QCOMPARE(ytpage::shortNoteText(ytpage::noteText(ytpage::Note::OnlyOnYouTube)), a("Only on YouTube"));
            card.player                = linkcard::Player::Popup;
            const linkcard::Layout pl  = linkcard::layout(card, style);
            const QString          pill = linkcard::popupPillText(pl, style);
            QVERIFY(QFontMetricsF(previewpaint::font(style, -1, true)).horizontalAdvance(pill) + 24 <= pl.thumb.width() - 16 + 0.5 || pill == a("In a window"));
            if (width >= 400)
                QCOMPARE(pill, a("Playing in a window"));
        }
        PreviewStyle style;
        card.player              = linkcard::Player::None;
        const linkcard::Layout l = linkcard::layout(card, style);
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Play);
        QCOMPARE(linkcard::zoneAt(card, l, l.footer.center(), true), linkcard::Zone::Open);
        // 2.5.0 plays in a window of its own (the release gate): the texts say so (review finding).
        QCOMPARE(linkcard::toolTip(card, linkcard::Zone::Play), ytpage::kAnchoredPlayer ? a("Play here") : a("Play in a window"));
        if (!ytpage::kAnchoredPlayer) {
            QVERIFY(!ytpage::consentNotice().contains(a("right in the chat")));
            QVERIFY(ytpage::consentNotice().contains(a("in a window of its own")));
            QCOMPARE(ytpage::consentQuestion(), a("Play this video in a window?"));
            QCOMPARE(linkcard::toolTip(card, linkcard::Zone::ConsentPlay), a("Play this video in a window with YouTube's own player"));
            QVERIFY(PrivacySection::playYouTubeHint().contains(a("in a window of its own over TeamSpeak")));
            QVERIFY(!PrivacySection::playYouTubeHint().contains(a("over the card")));
        }
        card.playable = false; // the switch off, no runtime: the whole card opens the browser (2.4)
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Open);
        card.playable = true;
        card.player   = linkcard::Player::Starting;
        QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), false), linkcard::Zone::Stop); // the (x) is always there
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Body);
        QVERIFY(!linkcard::isActionable(card, linkcard::Zone::Body));
        QCOMPARE(linkcard::toolTip(card, linkcard::Zone::Stop), a("Stop video"));
        card.player = linkcard::Player::Popup;
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Play); // brings the window to the front
        card.player = linkcard::Player::Hidden;
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Play); // resume
        QCOMPARE(linkcard::toolTip(card, linkcard::Zone::Play), a("Resume the video"));
        card.player = linkcard::Player::Note;
        card.note   = a("Video unavailable");
        QCOMPARE(linkcard::zoneAt(card, l, l.thumb.center(), true), linkcard::Zone::Open); // Watch on YouTube
        QCOMPARE(linkcard::zoneAt(card, l, l.hide.center(), true), linkcard::Zone::Hide);
        QVERIFY(linkcard::toolTip(card, linkcard::Zone::Open).contains(a("Video unavailable")));
        // The signature follows the state (the card is drawn again when it changes).
        linkcard::Card other = card;
        other.player         = linkcard::Player::None;
        QVERIFY(linkcard::signature(card, style, {}) != linkcard::signature(other, style, {}));
        linkcard::Card timed = other;
        timed.resumeMs       = 42000;
        QVERIFY(linkcard::signature(timed, style, {}) != linkcard::signature(other, style, {}));
        QVERIFY(!linkcard::playerOpen(other));
        other.player = linkcard::Player::Hidden;
        QVERIFY(linkcard::playerOpen(other));
    }

    // ---- Y-T2 the state machine ----
    void consentOnce()
    {
        QVERIFY(player->playable());
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Consent);
        QCOMPARE(st->loads, 0); // nothing is loaded before the notice is answered
        QVERIFY(st->opens.isEmpty());
        QVERIFY(host.changed.contains(a("s1")));
        player->consentBrowser(a("s1"));
        QCOMPARE(host.opened.size(), 1);
        QCOMPARE(host.opened.first(), kLink);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        QVERIFY(!Settings::instance().youTubeNoticeShown);
        player->play(a("s1"), kVideo, kLink); // asked again
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Consent);
        player->play(a("s2"), kVideo, kLink); // another card's Play ends it there
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        QCOMPARE(stateOf("s2"), YouTubePlayer::State::Consent);
        player->consentPlay(a("s1")); // not the Consent card: nothing
        QVERIFY(st->opens.isEmpty());
        player->consentPlay(a("s2"));
        QVERIFY(Settings::instance().youTubeNoticeShown);
        QCOMPARE(st->loads, 1);
        QCOMPARE(st->opens.size(), 1);
        QCOMPARE(stateOf("s2"), YouTubePlayer::State::Starting);
        QVERIFY(st->opens.first().popup); // the gate is closed: the popup player
        QCOMPARE(st->opens.first().page, ytpage::pageHtml(kVideo, 90, true)); // the link's t=90
        QVERIFY(st->opens.first().title.endsWith(a("A video")));
        QCOMPARE(host.pauses, 1); // TS Media's players paused
    }
    void consentSlotDeleted()
    {
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Consent);
        host.spots.remove(a("s1")); // the message left the document (MaxLines, a kick, a closed tab)
        player->relayout();
        player->consentPlay(a("s1"));
        QVERIFY(st->opens.isEmpty());
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
    }
    void createdThenReady()
    {
        Settings::instance().youTubeNoticeShown = true;
        Settings::instance().videoVolume        = 35;
        host.muted                              = true;
        player->play(a("s1"), kVideo, kLink);
        const int id = st->next;
        QVERIFY(player->timerForTests("creation")->isActive());
        QCOMPARE(player->timerForTests("creation")->interval(), 20000);
        QVERIFY(!player->timerForTests("ready")->isActive()); // YouTube's load is timed from Created, not from open
        QVERIFY(player->timerForTests("poll")->isActive());
        st->push(id, TSMP_EV_CREATED);
        poll();
        QVERIFY(!player->timerForTests("creation")->isActive());
        QVERIFY(player->timerForTests("ready")->isActive());
        QCOMPARE(player->timerForTests("ready")->interval(), 15000);
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 1 "))); // the window shows
        st->push(id, TSMP_EV_READY);
        poll();
        QVERIFY(!player->timerForTests("ready")->isActive());
        QVERIFY(st->sent(id, "volume:35"));
        QVERIFY(st->sent(id, "mute"));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        player->play(a("s1"), kVideo, kLink); // the card's thumbnail: the window to the front
        QVERIFY(st->sent(id, "front"));
        QCOMPARE(st->opens.size(), 1);
    }
    void creationTimeoutRetries()
    {
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        const int first = st->next;
        fire("creation"); // no Created within 20 s: closed, one retry after 1 s
        QVERIFY(st->closes.contains(first));
        QVERIFY(player->timerForTests("retry")->isActive());
        QCOMPARE(player->timerForTests("retry")->interval(), 1000);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
        fire("retry");
        QCOMPARE(st->opens.size(), 2);
        const int second = st->next;
        st->push(second, TSMP_EV_CREATED);
        st->push(second, TSMP_EV_READY);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        QVERIFY(host.opened.isEmpty());
    }
    void engineFailures()
    {
        Settings::instance().youTubeNoticeShown = true;
        for (int round = 1; round <= 3; ++round) {
            player->play(a("s1"), kVideo, kLink);
            st->push(st->next, TSMP_EV_FAILED, 0x05, a("environment"));
            poll();
            QVERIFY2(player->timerForTests("retry")->isActive(), qPrintable(player->describe() + a(" round ") + QString::number(round))); // one retry
            fire("retry");
            st->push(st->next, TSMP_EV_FAILED, 0x05, a("controller"));
            poll();
            // The second failure: the browser for this click, no note (not the video's fault).
            QCOMPARE(host.opened.size(), round);
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
            QCOMPARE(player->engineFailuresForTests(), round);
            QCOMPARE(player->unavailableForTests(), round == 3);
        }
        // Three in a session: unavailable; Play opens the browser at once.
        const int opens = st->opens.size();
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->opens.size(), opens);
        QCOMPARE(host.opened.size(), 4);
        QVERIFY(!player->playable());
    }
    void readyTimeoutNote()
    {
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        const int id = st->next;
        st->push(id, TSMP_EV_CREATED);
        poll();
        fire("ready"); // no Ready within 15 s of Created
        QVERIFY(st->closes.contains(id));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play this video here"));
        // Not remembered: another card of the video plays; the note goes when the card is drawn from scratch.
        player->play(a("s2"), kVideo, kLink);
        QCOMPARE(st->opens.size(), 2);
        player->forgetShownNotes();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
    }
    void errorNotes_data()
    {
        QTest::addColumn<int>("error");
        QTest::addColumn<QString>("note");
        QTest::addColumn<bool>("remembered");
        QTest::newRow("101") << 101 << a("This video can only be watched on YouTube") << true;
        QTest::newRow("150") << 150 << a("This video can only be watched on YouTube") << true;
        QTest::newRow("100") << 100 << a("Video unavailable") << true;
        QTest::newRow("2") << 2 << a("Can't play this video here") << false;
        QTest::newRow("5") << 5 << a("Can't play this video here") << false;
        QTest::newRow("153") << 153 << a("Can't play this video here") << false;
    }
    void errorNotes()
    {
        QFETCH(int, error);
        QFETCH(QString, note);
        QFETCH(bool, remembered);
        const int id = playToReady();
        st->push(id, TSMP_EV_ERROR, error);
        poll();
        QVERIFY(st->closes.contains(id));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, note);
        // The note card: its click opens the browser.
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(host.opened.size(), 1);
        // Another card of the same video: remembered errors open the browser at once, the others play.
        const int opens = st->opens.size();
        player->play(a("s2"), kVideo, kLink);
        QCOMPARE(st->opens.size(), remembered ? opens : opens + 1);
        QCOMPARE(host.opened.size(), remembered ? 2 : 1);
        if (remembered) {
            player->tryAgain(a("s1"), kVideo, kLink); // the Note card's menu forgets it and plays
            QCOMPARE(st->opens.size(), opens + 1);
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
        }
    }
    void failedProcessNote()
    {
        const int id = playToReady();
        st->push(id, TSMP_EV_FAILED, 1, a("process"));
        poll();
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play this video here"));
        player->play(a("s2"), kVideo, kLink);
        QCOMPARE(st->opens.size(), 2); // not remembered
    }
    void secondPlayKeepsTime()
    {
        const int a1 = playToReady("s1");
        st->push(a1, TSMP_EV_STATE, 1);
        st->push(a1, TSMP_EV_TIME, 42);
        poll();
        QCOMPARE(host.pauses, 2); // Play, and State(1) (the user may have resumed in the player)
        player->play(a("s2"), a("abcdefghijk"), QUrl(a("https://youtu.be/abcdefghijk")));
        QVERIFY(st->closes.contains(a1));
        QCOMPARE(player->cardOf(a("s1"), kVideo).resumeMs, qint64(42000));
        QCOMPARE(player->watchUrlOf(a("s1"), kVideo, kLink), ytpage::watchUrl(kVideo, 42));
        for (const QString& c : qAsConst(st->commands))
            QVERIFY2(!c.contains(a("time")), qPrintable(c)); // never asked: the page reports it
        // The same video again starts where it stopped.
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->opens.last().page, ytpage::pageHtml(kVideo, 42, true));
    }
    void lostEscAndPauses()
    {
        int id = playToReady();
        st->push(id, TSMP_EV_TIME, 12);
        st->push(id, TSMP_EV_LOST);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        QCOMPARE(player->cardOf(a("s1"), kVideo).resumeMs, qint64(12000));
        id = playToReady();
        player->playbackStarted(); // a TS Media video or voice message
        QVERIFY(st->sent(id, "pause"));
        st->push(id, TSMP_EV_KEY, 0x1B);
        poll();
        QVERIFY(st->closes.contains(id));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        id = playToReady();
        st->push(id, TSMP_EV_OPENURL, 0, a("https://www.youtube.com/watch?v=M7lc1UVf-VE"));
        st->push(id, TSMP_EV_OPENURL, 0, a("http://example.com/"));
        poll();
        QCOMPARE(host.opened.size(), 1);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup); // the player stays
        player->stop(a("s1"));
        QVERIFY(st->closes.contains(id));
    }
    void closedByUserKeepsSize()
    {
        const int id = playToReady();
        st->push(id, TSMP_EV_CLOSEDBYUSER, 0, a("800x500"));
        poll();
        QCOMPARE(Settings::instance().youTubeWindowSize, a("800x500"));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->opens.last().rect.size(), QSize(800, 500));
    }
    void switchOffAndFolder()
    {
        const QString folder = tmp.path() + a("/webview2");
        QVERIFY(QDir().mkpath(folder + a("/EBWebView")));
        const int id = playToReady();
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        QVERIFY(st->closes.contains(id));
        QCOMPARE(st->releases, 1);      // released now, not in 60 s
        QVERIFY(QDir(folder).exists()); // its processes may still use it
        QVERIFY(player->timerForTests("poll")->isActive());
        st->push(0, TSMP_EV_ENGINEEXITED);
        poll();
        QVERIFY(!QDir(folder).exists()); // deleted after EngineExited
        QVERIFY(!player->timerForTests("poll")->isActive());
        // Off: Play opens the browser, nothing is loaded.
        const int loads = st->loads;
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->loads, loads);
        QCOMPARE(host.opened.size(), 1);
    }
    void switchOffFolderGoesAtNextStart()
    {
        // Switched off, then TeamSpeak exits (or the plugin unloads) before EngineExited: the folder stays. The next
        // start with the switch off deletes it (setUpYouTube calls settingsChanged; design2 3.6).
        const QString folder = tmp.path() + a("/webview2");
        QVERIFY(QDir().mkpath(folder + a("/EBWebView")));
        playToReady();
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        player->shutdown(); // before EngineExited
        QVERIFY(QDir(folder).exists());
        auto          st2 = std::make_shared<FakeState>();
        YouTubePlayer next(host.make(), std::make_unique<FakeBackend>(st2));
        next.setUserDataFolderForTests(folder);
        next.settingsChanged(); // what setUpYouTube does at start
        QVERIFY(!QDir(folder).exists());
        QCOMPARE(st2->loads, 0); // nothing loaded for it
        // The elevated variant goes the same way; any other folder never.
        const QString elevated = tmp.path() + a("/webview2-elevated");
        QVERIFY(QDir().mkpath(elevated + a("/EBWebView")));
        next.setUserDataFolderForTests(elevated);
        next.settingsChanged();
        QVERIFY(!QDir(elevated).exists());
        const QString other = tmp.path() + a("/other");
        QVERIFY(QDir().mkpath(other));
        next.setUserDataFolderForTests(other);
        next.settingsChanged();
        QVERIFY(QDir(other).exists());
        next.shutdown();
    }
    void userDataFolders()
    {
        // WebView2's guidance for elevated hosts (author decision 4): an elevated TeamSpeak has a folder of its own.
        QCOMPARE(ytbackend::userDataFolderFor(a("C:\\Users\\u\\AppData\\Local"), false), a("C:/Users/u/AppData/Local/TSMedia/webview2"));
        QCOMPARE(ytbackend::userDataFolderFor(a("C:\\Users\\u\\AppData\\Local"), true), a("C:/Users/u/AppData/Local/TSMedia/webview2-elevated"));
        QVERIFY(ytbackend::userDataFolderFor(QString(), true).isEmpty());
        QCOMPARE(ytbackend::userDataFolder().endsWith(a("/webview2-elevated")), ytbackend::elevatedToken());
        // Review fix: switched off, both go (the folder first, then the other elevation's); any other name never.
        QCOMPARE(ytbackend::userDataFamily(a("C:/L/TSMedia/webview2")), QStringList({a("C:/L/TSMedia/webview2"), a("C:/L/TSMedia/webview2-elevated")}));
        QCOMPARE(ytbackend::userDataFamily(a("C:\\L\\TSMedia\\webview2-elevated")),
                 QStringList({a("C:/L/TSMedia/webview2-elevated"), a("C:/L/TSMedia/webview2")}));
        QCOMPARE(ytbackend::userDataFamily(a("C:/L/TSMedia/WebView2")).size(), 2);
        for (const char* other : {"C:/L/TSMedia/mywebview2", "C:/L/TSMedia/webview2/x", "C:/L/TSMedia/webview2-elevatedx", "/webview2", "", "C:/L/other"})
            QVERIFY2(ytbackend::userDataFamily(a(other)).isEmpty(), other);
    }
    void switchOffDeletesBothElevations()
    {
        // Review fix: an elevated TeamSpeak's folder (or a normal one's, seen from an elevated TeamSpeak) goes too, at the
        // switch and at every start with it off; one a browser process still runs on is never deleted in part.
        const QString normal   = tmp.path() + a("/webview2");
        const QString elevated = tmp.path() + a("/webview2-elevated");
        QVERIFY(QDir().mkpath(normal + a("/EBWebView/Default")));
        QVERIFY(QDir().mkpath(elevated + a("/EBWebView/Default")));
        // Chromium's lock file, held as its browser process holds it (no delete sharing, deleted when closed).
        const std::wstring lock = QDir::toNativeSeparators(elevated + a("/EBWebView/lockfile")).toStdWString();
        HANDLE held = CreateFileW(lock.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        QVERIFY(held != INVALID_HANDLE_VALUE);
        QVERIFY(ytbackend::userDataInUse(elevated));
        QVERIFY(!ytbackend::userDataInUse(normal));
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged(); // nothing ever loaded: at once (what a start with the switch off does)
        QVERIFY(!QDir(normal).exists());
        QVERIFY(QDir(elevated + a("/EBWebView/Default")).exists()); // in use: left whole
        CloseHandle(held);                                         // its browser process exited
        QVERIFY(!QFileInfo::exists(QString::fromStdWString(lock)));
        player->settingsChanged();
        QVERIFY(!QDir(elevated).exists());
        // A lock file nobody holds (a crashed run's) doesn't keep a folder.
        QVERIFY(QDir().mkpath(normal + a("/EBWebView")));
        QFile stale(normal + a("/EBWebView/lockfile"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        QVERIFY(!ytbackend::userDataInUse(normal));
        player->settingsChanged();
        QVERIFY(!QDir(normal).exists());
    }
    void switchOnAgainBeforeExitKeepsFolder_data()
    {
        QTest::addColumn<int>("answer"); // the old engine's EngineExited value; -1: none (the 20 s fallback)
        QTest::newRow("exited") << 0;
        QTest::newRow("not heard in 10 s") << 1;
        QTest::newRow("no answer") << -1;
    }
    void switchOnAgainBeforeExitKeepsFolder()
    {
        // Review fix: switched off (the engine released, the folder to go after its answer), on again and Play before
        // that answer: the helper makes the new environment on the same folder once the old browser exited (or after
        // 10 s). That answer, or the 20 s fallback, must not delete the folder under the new player.
        QFETCH(int, answer);
        const QString folder = tmp.path() + a("/webview2");
        QVERIFY(QDir().mkpath(folder + a("/EBWebView/Default")));
        const int first = playToReady();
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        QVERIFY(st->closes.contains(first));
        QCOMPARE(st->releases, 1);
        Settings::instance().playYouTubeInChat = true;
        player->settingsChanged();
        player->play(a("s2"), kVideo, kLink);
        const int next = st->next;
        QVERIFY(next > first);
        if (answer >= 0) {
            st->push(0, TSMP_EV_ENGINEEXITED, answer);
        } else {
            player->setReleaseWaitForTests(0);
        }
        st->push(next, TSMP_EV_CREATED);
        st->push(next, TSMP_EV_READY);
        poll();
        QTRY_VERIFY_WITH_TIMEOUT(!player->describe().contains(a("engine=releasing")), 2000);
        player->setReleaseWaitForTests(20000);
        QVERIFY(QDir(folder + a("/EBWebView/Default")).exists());
        QCOMPARE(stateOf("s2"), YouTubePlayer::State::Popup);
        QCOMPARE(st->releases, 1);
        // Closed and switched off later: it goes then (released, after its answer), as always.
        player->stop(a("s2"));
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        QCOMPARE(st->releases, 2);
        QVERIFY(QDir(folder).exists());
        st->push(0, TSMP_EV_ENGINEEXITED, 0);
        poll();
        QVERIFY(!QDir(folder).exists());
    }
    void switchOffAgainAfterAPlayReleasesFirst()
    {
        // Off, on, a Play that closes again, off again, all before the first release's answer: the player's engine may
        // be a new environment on the folder. That answer releases it first; the folder goes after the next answer.
        const QString folder = tmp.path() + a("/webview2");
        QVERIFY(QDir().mkpath(folder + a("/EBWebView/Default")));
        playToReady();
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        QCOMPARE(st->releases, 1);
        Settings::instance().playYouTubeInChat = true;
        player->settingsChanged();
        player->play(a("s2"), kVideo, kLink);
        player->stop(a("s2"));
        Settings::instance().playYouTubeInChat = false;
        player->settingsChanged();
        QCOMPARE(st->releases, 1); // the first release still waits for its answer
        st->push(0, TSMP_EV_ENGINEEXITED, 0);
        poll();
        QCOMPARE(st->releases, 2);
        QVERIFY(QDir(folder).exists());
        QVERIFY(player->timerForTests("poll")->isActive());
        st->push(0, TSMP_EV_ENGINEEXITED, 2);
        poll();
        QVERIFY(!QDir(folder).exists());
        QVERIFY(!player->timerForTests("poll")->isActive());
    }
    void releaseWithoutAnswerStopsPolling()
    {
        // The engine released, but no EngineExited ever comes (an old helper): the poll doesn't run for the session.
        const int id = playToReady();
        player->stop(a("s1"));
        QVERIFY(st->closes.contains(id));
        fire("engine");
        QCOMPARE(st->releases, 1);
        QVERIFY(player->timerForTests("poll")->isActive());
        player->setReleaseWaitForTests(0);
        // The next 50 ms poll gives up (a fixed 60 ms window was flaky: Windows' coarse timers can fire later).
        QTRY_VERIFY_WITH_TIMEOUT(!player->timerForTests("poll")->isActive(), 2000);
        QVERIFY(player->describe().contains(a("engine=on")) || player->describe().contains(a("engine=idle")));
    }
    void sessionMuteStartsMuted()
    {
        // "Start videos muted" / TS Media's session mute: YouTube starts muted (mute=1), before any command can arrive.
        host.muted                              = true;
        player                                  = std::make_unique<YouTubePlayer>(host.make(), std::make_unique<FakeBackend>(st));
        player->setUserDataFolderForTests(tmp.path() + a("/webview2"));
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(st->opens.last().page.contains(a("&mute=1\"")));
        const int id = st->next;
        st->push(id, TSMP_EV_CREATED);
        st->push(id, TSMP_EV_READY);
        poll();
        QVERIFY(st->sent(id, "mute"));
        player->stop(a("s1"));
        host.muted = false;
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(!st->opens.last().page.contains(a("mute=1")));
    }
    void popupMadeWhileMinimized()
    {
        // TeamSpeak minimized while the popup is still being made: Windows doesn't hide what wasn't shown, so the
        // popup isn't shown at Created and the page is paused (before the volume lifts the view's mute); restored,
        // the popup shows, still paused (review fix, 3.8 "TeamSpeak minimized: pause").
        QTextBrowser browser;
        browser.setPlainText(a("line\n").repeated(20));
        browser.show();
        YouTubePlayer::Spot spot = spotAt(QRect(100, 100, 362, 204));
        spot.viewport            = browser.viewport();
        host.spots.insert(a("s1"), spot);
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(st->opens.last().popup);
        const int id = st->next;
        browser.setWindowState(Qt::WindowMinimized);
        QVERIFY(browser.isMinimized());
        st->push(id, TSMP_EV_CREATED);
        poll();
        QVERIFY2(st->places.last().startsWith(QString::number(id) + a(" 0 ")), qPrintable(st->places.last()));
        st->commands.clear();
        st->push(id, TSMP_EV_READY);
        poll();
        // Paused, and no volume yet: the view's own mute stays while TeamSpeak is minimized (live run: YouTube's
        // autoplay can start after a pause sent at Ready).
        const QRegularExpression volumeOf(QString::number(id) + a(":volume:\\d+"));
        QVERIFY(st->sent(id, "pause"));
        QCOMPARE(st->commands.indexOf(volumeOf), -1);
        // The autoplay starts anyway: paused again at once (never played in the background).
        st->commands.clear();
        st->push(id, TSMP_EV_STATE, 1);
        poll();
        QVERIFY(st->sent(id, "pause"));
        QCOMPARE(st->commands.indexOf(volumeOf), -1);
        const int places = st->places.size();
        st->commands.clear();
        browser.setWindowState(Qt::WindowNoState);
        QCOMPARE(st->places.size(), places + 1);
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 1 ")));
        // Back: shown, still paused, and only now the volume (which lifts the view's mute).
        const int pause  = st->commands.indexOf(QString::number(id) + a(":pause"));
        const int volume = st->commands.indexOf(volumeOf);
        QVERIFY2(pause >= 0 && volume > pause, qPrintable(st->commands.join(a(" "))));
        QVERIFY(!st->sent(id, "play"));
        // Minimized after Created but before Ready (Windows hides the shown popup itself): the same rule.
        player->stop(a("s1"));
        player->play(a("s1"), kVideo, kLink);
        const int third = st->next;
        st->push(third, TSMP_EV_CREATED);
        poll();
        QVERIFY(st->places.last().startsWith(QString::number(third) + a(" 1 ")));
        browser.setWindowState(Qt::WindowMinimized);
        QVERIFY(browser.isMinimized());
        st->commands.clear();
        st->push(third, TSMP_EV_READY);
        poll();
        const QRegularExpression volumeOf3(QString::number(third) + a(":volume:\\d+"));
        QVERIFY(st->sent(third, "pause"));
        QCOMPARE(st->commands.indexOf(volumeOf3), -1);
        st->commands.clear();
        browser.setWindowState(Qt::WindowNoState);
        QVERIFY(st->commands.indexOf(volumeOf3) > st->commands.indexOf(QString::number(third) + a(":pause")));
        QVERIFY(!st->sent(third, "play"));
        // Restored before Created: it starts as any Play (shown, not paused).
        player->stop(a("s1"));
        player->play(a("s1"), kVideo, kLink);
        const int second = st->next;
        browser.setWindowState(Qt::WindowMinimized);
        browser.setWindowState(Qt::WindowNoState);
        QTest::qWait(30); // the platform's own state events too (the minimize's pause goes nowhere before Created)
        QVERIFY(!browser.isMinimized());
        st->commands.clear();
        st->push(second, TSMP_EV_CREATED);
        st->push(second, TSMP_EV_READY);
        poll();
        QVERIFY(st->places.last().startsWith(QString::number(second) + a(" 1 ")));
        QVERIFY(!st->sent(second, "pause"));
        QVERIFY(st->commands.indexOf(QRegularExpression(QString::number(second) + a(":volume:\\d+"))) >= 0);
        // Minimized and restored between Created and Ready: it starts as any Play too.
        player->stop(a("s1"));
        player->play(a("s1"), kVideo, kLink);
        const int fourth = st->next;
        st->push(fourth, TSMP_EV_CREATED);
        poll();
        browser.setWindowState(Qt::WindowMinimized);
        browser.setWindowState(Qt::WindowNoState);
        QTest::qWait(30);
        st->commands.clear();
        st->push(fourth, TSMP_EV_READY);
        poll();
        QVERIFY(!st->sent(fourth, "pause"));
        QVERIFY(st->commands.indexOf(QRegularExpression(QString::number(fourth) + a(":volume:\\d+"))) >= 0);
        // Playing while TeamSpeak is visible: this rule pauses nothing.
        st->commands.clear();
        st->push(fourth, TSMP_EV_STATE, 1);
        poll();
        QVERIFY(!st->sent(fourth, "pause"));
    }
    void popupFitsTheMonitor()
    {
        // The popup's window lies inside its monitor's work area (title bar on screen; at most 90% of it), its aspect
        // kept (tsmpmsg::fitPopup, review fix).
        const tsmpmsg::Box frame{8, 31, 8, 8};
        const tsmpmsg::Box work{0, 0, 1920, 1040};
        // 640x360 du at s 3 (1920x1080 px) centred on TeamSpeak: shrunk to fit, 16:9, inside.
        const tsmpmsg::Box big = tsmpmsg::fitPopup(tsmpmsg::Box{0, -20, 1920, 1060}, frame, work);
        QVERIFY(big.right - big.left <= 1920 * 0.9 && big.bottom - big.top + 39 <= 1040 * 0.9 + 1);
        QVERIFY(qAbs(double(big.right - big.left) / (big.bottom - big.top) - 16.0 / 9.0) < 0.02);
        QVERIFY(big.left - frame.left >= 0 && big.top - frame.top >= 0 && big.right + frame.right <= 1920 && big.bottom + frame.bottom <= 1040);
        // A window that fits is only moved inside (here: off the top), never resized.
        const tsmpmsg::Box moved = tsmpmsg::fitPopup(tsmpmsg::Box{100, -100, 740, 260}, frame, work);
        QCOMPARE(moved.right - moved.left, 640L);
        QCOMPARE(moved.bottom - moved.top, 360L);
        QCOMPARE(moved.top, 31L);
        // A second monitor on the left (negative coordinates).
        const tsmpmsg::Box left = tsmpmsg::fitPopup(tsmpmsg::Box{-2000, 100, -1360, 460}, frame, tsmpmsg::Box{-1920, 0, 0, 1040});
        QCOMPARE(left.left, -1912L);
        // An empty work area: unchanged.
        const tsmpmsg::Box same = tsmpmsg::fitPopup(tsmpmsg::Box{1, 2, 3, 4}, frame, tsmpmsg::Box{});
        QCOMPARE(same.right, 3L);
    }
    void viewMuteRule()
    {
        // The helper's web view starts muted; only the plugin's volume or an unmute lifts it (never pause, play, mute).
        for (const wchar_t* c : {L"volume:0", L"volume:70", L"volume:100", L"unmute"})
            QVERIFY(tsmpmsg::liftsViewMute(c, wcslen(c)));
        for (const wchar_t* c : {L"pause", L"play", L"mute", L"volume:101", L"volume:", L"front", L"unmute "})
            QVERIFY(!tsmpmsg::liftsViewMute(c, wcslen(c)));
    }
    void previewsOffCloses()
    {
        const int id = playToReady();
        Settings::instance().showLinkPreviews = false;
        player->settingsChanged();
        QVERIFY(st->closes.contains(id));
        QCOMPARE(st->releases, 0);
    }
    void messageRemovedCloses()
    {
        const int id = playToReady();
        host.spots.remove(a("s1"));
        player->relayout();
        QVERIFY(st->closes.contains(id));
    }
    void releaseThenPlay()
    {
        const int id = playToReady();
        player->stop(a("s1"));
        QVERIFY(st->closes.contains(id));
        QVERIFY(player->timerForTests("engine")->isActive());
        QCOMPARE(player->timerForTests("engine")->interval(), 60000);
        fire("engine");
        QCOMPARE(st->releases, 1);
        QVERIFY(player->timerForTests("poll")->isActive()); // until EngineExited
        // A Play during the pending release: the helper queues it until EngineExited (the fake: Created after it).
        player->play(a("s1"), kVideo, kLink);
        const int next = st->next;
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
        st->push(0, TSMP_EV_ENGINEEXITED);
        st->push(next, TSMP_EV_CREATED);
        st->push(next, TSMP_EV_READY);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        QVERIFY(!player->timerForTests("engine")->isActive());
    }
    void loadFailureUnavailable()
    {
        Settings::instance().youTubeNoticeShown = true;
        st->failLoad                            = true;
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(host.opened.size(), 1);
        QVERIFY(st->opens.isEmpty());
        QVERIFY(!player->playable());
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->loads, 1); // never tried again this session
        QCOMPARE(host.opened.size(), 2);
    }
    void notPlayable()
    {
        ytbackend::setRegistryReaderForTests([](ytbackend::Hive) { return QString(); }); // no WebView2 Runtime
        QVERIFY(!player->playable());
        QVERIFY(!player->cardOf(a("s1"), kVideo).playable);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->loads, 0);
        QCOMPARE(host.opened.size(), 1);
        ytbackend::setRegistryReaderForTests(runtimeReader);
        YouTubePlayer none(host.make(), nullptr); // no helper built in
        QVERIFY(!none.playable());
    }

    // ---- Y-T3 the anchored window (behind its gate) ----
    void anchoredGeometry()
    {
        QTextBrowser browser;
        browser.resize(500, 400);
        browser.setPlainText(a("line\n").repeated(200));
        browser.show();
        host.browser = &browser;
        player->setAnchoredGateForTests(true);
        Settings::instance().youTubeNoticeShown = true;
        // A thumbnail partly out of the view: scrolled into view first.
        YouTubePlayer::Spot spot = spotAt(QRect(100, 700, 362, 204), QRect(0, 0, 1000, 800));
        spot.viewport            = browser.viewport();
        host.spots.insert(a("s1"), spot);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(host.scrolled, QStringList{a("s1")});
        QCOMPARE(st->opens.size(), 1);
        const PlayerOpen o = st->opens.first();
        QVERIFY(!o.popup);
        QCOMPARE(o.rect, QRect(100, 10, 362, 204));
        QCOMPARE(o.clip, o.rect);
        QCOMPARE(o.cssScale, 1.0);
        const int id = st->next;
        st->push(id, TSMP_EV_CREATED);
        st->push(id, TSMP_EV_READY);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Open);
        // Below half visible: paused and hidden; the 60 s timer runs.
        host.spots[a("s1")].thumb.moveTop(-150);
        player->relayout();
        QVERIFY(st->sent(id, "pause"));
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 0 ")));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Hidden);
        QVERIFY(player->timerForTests("hidden")->isActive());
        QCOMPARE(player->timerForTests("hidden")->interval(), 60000);
        // Back above half: shown again, still paused (no automatic resume), clipped to the chat.
        st->commands.clear();
        host.spots[a("s1")].thumb.moveTop(-50);
        player->relayout();
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 1 ")));
        QCOMPARE(player->lastClipForTests(), QRect(100, 0, 362, 154));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Open);
        QVERIFY(!st->sent(id, "play"));
        QVERIFY(!player->timerForTests("hidden")->isActive());
        // An active scroll: hidden while it lasts (no pause above 0.5), back 150 ms after the last change.
        st->commands.clear();
        QScrollBar* bar = browser.verticalScrollBar();
        QVERIFY(bar->maximum() > 40);
        bar->setValue(bar->value() + 30);
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 0 ")));
        QVERIFY(!st->sent(id, "pause"));
        QVERIFY(player->timerForTests("settle")->isActive());
        QCOMPARE(player->timerForTests("settle")->interval(), 150);
        fire("settle");
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 1 ")));
        // The chat's tab hidden: paused and hidden; shown again: back, still paused.
        browser.hide();
        QVERIFY(st->sent(id, "pause"));
        QVERIFY(st->places.last().startsWith(QString::number(id) + a(" 0 ")));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Hidden);
        browser.show();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Open);
        // Scrolled away again, then hidden for 60 s: closed.
        host.spots[a("s1")].thumb.moveTop(-150);
        player->relayout();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Hidden);
        fire("hidden");
        QVERIFY(st->closes.contains(id));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        host.browser = nullptr;
    }
    void anchoredConditions()
    {
        Settings::instance().youTubeNoticeShown = true;
        player->setAnchoredGateForTests(true);
        // Too small (a narrow chat, Small): the popup.
        host.spots.insert(a("s1"), spotAt(QRect(100, 100, 300, 169)));
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(st->opens.last().popup);
        player->stop(a("s1"));
        // "Play in a window" always opens the popup.
        host.spots.insert(a("s1"), spotAt(QRect(100, 100, 362, 204)));
        player->play(a("s1"), kVideo, kLink, true);
        QVERIFY(st->opens.last().popup);
        player->stop(a("s1"));
        // s 2 on a 2x screen: the CSS scale keeps the thumbnail's 362 du as CSS px.
        YouTubePlayer::Spot big = spotAt(QRect(100, 100, 1448, 816), QRect(0, 0, 3000, 2000), 2.0);
        big.dpr                 = 2.0;
        big.sysScale            = 2.0;
        big.thumbDu             = QSizeF(362, 204);
        host.spots.insert(a("s1"), big);
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(!st->opens.last().popup);
        QCOMPARE(st->opens.last().cssScale, 4.0);
        player->stop(a("s1"));
        // A narrow chat fits its previews down to s 0.5; the popup, a window of its own, keeps the chat's scale without
        // that fit (live run: 800x450 du came out as a 400x225 px window with YouTube's page at 50%).
        YouTubePlayer::Spot narrow = spotAt(QRect(100, 100, 150, 84), QRect(0, 0, 160, 800), 0.5);
        narrow.popupS              = 1.0;
        host.spots.insert(a("s1"), narrow);
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(st->opens.last().popup);
        QCOMPARE(st->opens.last().cssScale, 1.0);
        QCOMPARE(st->opens.last().rect.size(), QSize(640, 360));
        player->stop(a("s1"));
        // Small (or a larger text size) still counts: popupS is the preset times the chat's text size.
        narrow.popupS = 0.85;
        host.spots.insert(a("s1"), narrow);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->opens.last().cssScale, 0.85);
        QCOMPARE(st->opens.last().rect.size(), QSize(544, 306));
        player->stop(a("s1"));
        QCOMPARE(previewscale::fontScale20(11, 100), 20);
        QCOMPARE(previewscale::computeScale(11, 100, 160, 4, 0, 1.0).s20, 14); // the chat itself is fitted (0.70)
    }

    // ---- Y-T4 unload ----
    void unload()
    {
        QTextBrowser browser;
        browser.setPlainText(a("line\n").repeated(200));
        browser.show();
        host.browser = &browser;
        player->setAnchoredGateForTests(true);
        Settings::instance().youTubeNoticeShown = true;
        YouTubePlayer::Spot spot                = spotAt(QRect(100, 100, 362, 204));
        spot.viewport                           = browser.viewport();
        host.spots.insert(a("s1"), spot);
        player->play(a("s1"), kVideo, kLink);
        st->push(st->next, TSMP_EV_CREATED);
        poll();
        player->shutdown();
        player->shutdown();
        QCOMPARE(st->shutdowns, 1);
        for (const char* name : {"poll", "creation", "ready", "hidden", "settle", "retry", "engine"})
            QVERIFY2(!player->timerForTests(name)->isActive(), name);
        // An event after shutdown reaches nothing.
        const int places = st->places.size(), commands = st->commands.size(), opens = st->opens.size();
        browser.hide();
        browser.show();
        browser.verticalScrollBar()->setValue(5);
        player->play(a("s1"), kVideo, kLink);
        player->playbackStarted();
        player->relayout();
        player->settingsChanged();
        QCOMPARE(st->places.size(), places);
        QCOMPARE(st->commands.size(), commands);
        QCOMPARE(st->opens.size(), opens);
        QVERIFY(!player->playable());
        host.browser = nullptr;
        // A reload: a new player and backend; init again at its first Play.
        auto          st2 = std::make_shared<FakeState>();
        YouTubePlayer again(host.make(), std::make_unique<FakeBackend>(st2));
        again.setUserDataFolderForTests(tmp.path() + a("/webview2"));
        host.spots.insert(a("s1"), spotAt(QRect(100, 100, 362, 204)));
        again.play(a("s1"), kVideo, kLink);
        QCOMPARE(st2->loads, 1);
        QCOMPARE(st2->opens.size(), 1);
        again.shutdown();
        QCOMPARE(st2->shutdowns, 1);
    }

    // ---- Y-T7 settings ----
    void settings()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(a("settings.ini"));
        Settings      s;
        QVERIFY(s.playYouTubeInChat);
        QVERIFY(!s.youTubeNoticeShown);
        QVERIFY(s.youTubeWindowSize.isEmpty());
        s.playYouTubeInChat  = false;
        s.youTubeNoticeShown = true;
        s.youTubeWindowSize  = a("800x450");
        s.save(file);
        Settings t;
        t.load(file);
        QVERIFY(!t.playYouTubeInChat);
        QVERIFY(t.youTubeNoticeShown);
        QCOMPARE(t.youTubeWindowSize, a("800x450"));
        // Restore defaults never resets the notice (no section stores it); the box goes back on.
        PrivacySection section;
        section.load(t);
        section.restoreDefaults(Settings());
        Settings target = t;
        section.store(target, t);
        QVERIFY(target.youTubeNoticeShown);
        QVERIFY(target.playYouTubeInChat);
        // The box greys out with Show previews off (its tick kept).
        auto* box = section.findChild<QCheckBox*>(a("tsmediaPlayYouTube"));
        QVERIFY(box);
        QCheckBox* show = nullptr;
        for (QCheckBox* c : section.findChildren<QCheckBox*>()) {
            if (c->text().contains(a("other websites")))
                show = c;
        }
        QVERIFY(show);
        section.load(Settings());
        QVERIFY(box->isEnabled() && box->isChecked());
        show->setChecked(false);
        QVERIFY(!box->isEnabled());
        QVERIFY(box->isChecked());
        // The hint: this test build has no helper resource.
        QVERIFY(PrivacySection::playYouTubeHint().contains(a("privacy-enhanced mode")));
        QVERIFY(PrivacySection::playYouTubeHint().contains(a("has no YouTube player")));
    }

    // ---- 2.6 chat proxy (design 9.3 tests 2 and 3) ----
    void tsmpNet()
    {
        const std::wstring common = L"--autoplay-policy=no-user-gesture-required --force-webrtc-ip-handling-policy=disable_non_proxied_udp";
        QVERIFY(tsmpnet::argsFor(TSMP_NET_SYSTEM, 0) == common);
        QVERIFY(tsmpnet::argsFor(TSMP_NET_DIRECT, 0) == common + L" --no-proxy-server");
        QVERIFY(tsmpnet::argsFor(TSMP_NET_BRIDGE, 47123) == common + L" --proxy-server=http://127.0.0.1:47123");
        QVERIFY(tsmpnet::argsFor(TSMP_NET_UNSET, 0).empty());
        QVERIFY(tsmpnet::argsFor(7, 0).empty());
        QVERIFY(tsmpnet::argsFor(TSMP_NET_BRIDGE, 80).empty()); // never a privileged port
        // The WebRTC switch in every family (S10); never a bypass list or direct:// (fail closed).
        for (const int mode : {TSMP_NET_SYSTEM, TSMP_NET_DIRECT, TSMP_NET_BRIDGE}) {
            const QString args = QString::fromStdWString(tsmpnet::argsFor(mode, 47123));
            QVERIFY(args.contains(a("--force-webrtc-ip-handling-policy=disable_non_proxied_udp")));
            QVERIFY(!args.contains(a("bypass")));
            QVERIFY(!args.contains(a("direct://")));
        }
        // The bridge's argument has nothing but the port's digits after the fixed text.
        const QString bridgeArgs = QString::fromStdWString(tsmpnet::argsFor(TSMP_NET_BRIDGE, 65535));
        QVERIFY(bridgeArgs.endsWith(a(" --proxy-server=http://127.0.0.1:65535")));
        tsmpnet::Net net;
        QVERIFY(!tsmpnet::openAllowed(net)); // UNSET (S9)
        QVERIFY(tsmpnet::validNetwork(net));
        net.mode = TSMP_NET_SYSTEM;
        QVERIFY(tsmpnet::openAllowed(net));
        net.mode       = TSMP_NET_BRIDGE;
        net.bridgePort = 47123;
        net.realm      = L"tsmedia-0123456789abcdef";
        net.user       = L"tsm-p";
        net.token      = std::wstring(43, L'A');
        QVERIFY(tsmpnet::validNetwork(net));
        QVERIFY(tsmpnet::openAllowed(net));
        for (const auto& bad : {std::make_pair(0, 0), std::make_pair(1, 0), std::make_pair(2, 0), std::make_pair(3, 0)}) {
            tsmpnet::Net b = net;
            if (bad.first == 0)
                b.bridgePort = 1023;
            if (bad.first == 1)
                b.realm = L"tsmedia-0123456789ABCDEF"; // upper case
            if (bad.first == 2)
                b.user = L"tsm-f1";
            if (bad.first == 3)
                b.token = L"short";
            QVERIFY(!tsmpnet::validNetwork(b));
            QVERIFY(!tsmpnet::openAllowed(b));
        }
        tsmpnet::Net unknown;
        unknown.mode = 9;
        QVERIFY(!tsmpnet::validNetwork(unknown));
        // The bridge's login only (I2a finding: get_Uri "http://127.0.0.1:<port>/", get_Challenge "Basic realm=...").
        const wchar_t* challenge = L"Basic realm=\"tsmedia-0123456789abcdef\"";
        QVERIFY(tsmpnet::answersChallenge(net, L"http://127.0.0.1:47123/", challenge));
        QVERIFY(tsmpnet::answersChallenge(net, L"http://127.0.0.1:47123", challenge));
        QVERIFY(!tsmpnet::answersChallenge(net, L"http://127.0.0.1:47124/", challenge));
        QVERIFY(!tsmpnet::answersChallenge(net, L"https://127.0.0.1:47123/", challenge));
        QVERIFY(!tsmpnet::answersChallenge(net, L"http://localhost:47123/", challenge));
        QVERIFY(!tsmpnet::answersChallenge(net, L"http://127.0.0.1:47123/", L"Basic realm=\"tsmedia-ffffffffffffffff\""));
        QVERIFY(!tsmpnet::answersChallenge(net, L"https://www.youtube.com/", L"Basic realm=\"site\"")); // a website's 401
        QVERIFY(!tsmpnet::answersChallenge(net, nullptr, challenge));
        tsmpnet::Net system = net;
        system.mode         = TSMP_NET_SYSTEM;
        QVERIFY(!tsmpnet::answersChallenge(system, L"http://127.0.0.1:47123/", challenge));
        // Overrides (S9).
        QVERIFY(!tsmpnet::overrideMentionsProxy({}));
        QVERIFY(!tsmpnet::overrideMentionsProxy({L"", L"--disable-gpu"}));
        QVERIFY(tsmpnet::overrideMentionsProxy({L"--proxy-server=x"}));
        QVERIFY(tsmpnet::overrideMentionsProxy({L"--disable-gpu", L"--PROXY-bypass-list=*"})); // the second hive
        QVERIFY(tsmpnet::overrideMentionsProxy({L"", L"", L"--no-Proxy-server"}));
        // Notes.
        QCOMPARE(ytpage::noteText(ytpage::Note::Proxy), a("Can't play through the proxy"));
        QCOMPARE(ytpage::shortNoteText(ytpage::noteText(ytpage::Note::Proxy)), a("Proxy not working"));
    }
    proxy::PlayerNetwork bridgeNet(quint64 generation = 2, quint16 port = 47123)
    {
        proxy::PlayerNetwork n;
        n.kind       = proxy::PlayerNetwork::Kind::Bridge;
        n.blockedBy  = proxy::Failure::None;
        n.bridgePort = port;
        n.realm      = "tsmedia-0123456789abcdef";
        n.token      = QByteArray(43, 'A');
        n.generation = generation;
        return n;
    }
    proxy::PlayerNetwork kindNet(proxy::PlayerNetwork::Kind kind, proxy::Failure blockedBy = proxy::Failure::None)
    {
        proxy::PlayerNetwork n;
        n.kind       = kind;
        n.blockedBy  = blockedBy;
        n.generation = 3;
        return n;
    }
    void networkBeforeEveryOpen()
    {
        // Play: the network right before the open (S9).
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->calls, QStringList({a("net:system"), a("open")}));
        // The 1 s engine retry.
        st->push(st->next, TSMP_EV_FAILED, 0x05, a("environment"));
        poll();
        fire("retry");
        QCOMPARE(st->calls.mid(2), QStringList({a("net:system"), a("open")}));
        // The "busy" retry.
        st->push(st->next, TSMP_EV_FAILED, int(0x8007139F), a("busy"));
        poll();
        fire("busy");
        QCOMPARE(st->calls.mid(4), QStringList({a("net:system"), a("open")}));
        // Try again after a note, through the bridge.
        st->push(st->next, TSMP_EV_ERROR, 5);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
        host.net = bridgeNet();
        player->networkChanged(); // nothing open: the engine goes now (it was made for the Windows settings)
        QCOMPARE(st->releases, 1);
        player->tryAgain(a("s1"), kVideo, kLink);
        QCOMPARE(st->calls.mid(6), QStringList({a("net:bridge:47123"), a("open")}));
        // The popup (forced) takes the same open.
        player->stop(a("s1"));
        player->play(a("s2"), kVideo, kLink, true);
        QCOMPARE(st->calls.mid(8), QStringList({a("net:bridge:47123"), a("open")}));
        QCOMPARE(player->engineFailuresForTests(), 0);
    }
    void blockedRouteIsProxyNote()
    {
        Settings::instance().youTubeNoticeShown = true;
        // The chat proxy is refused (its password can't be read): no open, the proxy note, no engine failure (S9).
        host.net = kindNet(proxy::PlayerNetwork::Kind::Blocked, proxy::Failure::PasswordUnreadable);
        player->play(a("s1"), kVideo, kLink);
        QVERIFY(st->opens.isEmpty());
        QVERIFY(st->calls.isEmpty());
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play through the proxy"));
        QVERIFY(host.opened.isEmpty()); // never the browser behind the user's back
        QCOMPARE(player->engineFailuresForTests(), 0);
        QVERIFY(!player->hasPlayer());
        // Windows mode with a manual SOCKS-only setting (S13): the same.
        host.net = kindNet(proxy::PlayerNetwork::Kind::Blocked, proxy::Failure::SocksOnlyWindows);
        player->play(a("s2"), kVideo, kLink);
        QVERIFY(st->opens.isEmpty());
        QCOMPARE(player->cardOf(a("s2"), kVideo).note, a("Can't play through the proxy"));
        // The helper refuses the network: the same, nothing opened.
        host.net          = kindNet(proxy::PlayerNetwork::Kind::System);
        st->refuseNetwork = 1;
        player->tryAgain(a("s1"), kVideo, kLink);
        QCOMPARE(st->calls, QStringList({a("net:system")}));
        QVERIFY(st->opens.isEmpty());
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play through the proxy"));
        QCOMPARE(player->engineFailuresForTests(), 0);
        QVERIFY(player->playable());
        // No network hook at all: fail closed.
        YouTubePlayer::Host bare = host.make();
        bare.network             = nullptr;
        auto          st2        = std::make_shared<FakeState>();
        YouTubePlayer other(bare, std::make_unique<FakeBackend>(st2));
        other.setUserDataFolderForTests(tmp.path() + a("/webview2-bare"));
        other.play(a("s1"), kVideo, kLink);
        QVERIFY(st2->opens.isEmpty());
        QCOMPARE(other.cardOf(a("s1"), kVideo).note, a("Can't play through the proxy"));
        other.shutdown();
    }
    void customEnteredOrLeftClosesPlayer()
    {
        for (const bool entering : {true, false}) {
            make();
            host.net     = entering ? kindNet(proxy::PlayerNetwork::Kind::System) : bridgeNet();
            const int id = playToReady();
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
            host.net = entering ? bridgeNet() : kindNet(proxy::PlayerNetwork::Kind::Direct);
            player->networkChanged();
            QVERIFY(st->closes.contains(id));
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::None); // back to its thumbnail
            QCOMPARE(host.feedback, QStringList({a("s1 Closed: the chat proxy changed. Press Play again.")}));
            QCOMPARE(st->releases, 1); // released now, not in 60 s
            QVERIFY(!player->timerForTests("engine")->isActive());
            QVERIFY(!player->engineLiveForTests());
            // The next Play opens on the new route at once (a new environment: no waiting).
            player->play(a("s1"), kVideo, kLink);
            QCOMPARE(st->calls.last(), a("open"));
            QCOMPARE(st->calls.at(st->calls.size() - 2), entering ? a("net:bridge:47123") : a("net:direct"));
            QCOMPARE(player->engineFailuresForTests(), 0);
        }
    }
    void windowsDirectAtNextOpen()
    {
        // Windows <-> direct with a player open: it stays (F15); the next open releases the engine first and opens after
        // EngineExited, not counted as an engine failure (F7).
        const int id = playToReady();
        host.net     = kindNet(proxy::PlayerNetwork::Kind::Direct);
        player->networkChanged();
        QVERIFY(!st->closes.contains(id));
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        QCOMPARE(st->releases, 0);
        QVERIFY(host.feedback.isEmpty());
        player->stop(a("s1"));
        QCOMPARE(st->opens.size(), 1);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->releases, 1);
        QCOMPARE(st->opens.size(), 1); // waits for EngineExited
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
        QVERIFY(player->timerForTests("poll")->isActive());
        st->push(0, TSMP_EV_ENGINEEXITED);
        poll();
        QCOMPARE(st->opens.size(), 2);
        QCOMPARE(st->calls.mid(st->calls.size() - 2), QStringList({a("net:direct"), a("open")}));
        st->push(st->next, TSMP_EV_CREATED);
        st->push(st->next, TSMP_EV_READY);
        poll();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        QCOMPARE(player->engineFailuresForTests(), 0);
        // Back to the Windows settings: the same once more (no answer in 20 s: the open goes on anyway).
        player->stop(a("s1"));
        host.net = kindNet(proxy::PlayerNetwork::Kind::System);
        player->setReleaseWaitForTests(50);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->releases, 2);
        QTRY_COMPARE_WITH_TIMEOUT(st->opens.size(), 3, 3000);
        QCOMPARE(st->calls.at(st->calls.size() - 2), a("net:system"));
    }
    void customToCustomKeepsPlaying()
    {
        host.net     = bridgeNet(2);
        const int id = playToReady();
        host.net     = bridgeNet(3); // another endpoint or login: the same bridge port, realm and token
        player->networkChanged();
        QVERIFY(!st->closes.contains(id));
        QCOMPARE(st->releases, 0);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Popup);
        // A refused custom route is still "custom": the player closes only when custom is entered or left.
        host.net = kindNet(proxy::PlayerNetwork::Kind::Blocked, proxy::Failure::BridgeUnavailable);
        player->networkChanged();
        QVERIFY(!st->closes.contains(id));
    }
    void networkFailedReopensOnce()
    {
        for (const bool custom : {false, true}) {
            make();
            Settings::instance().youTubeNoticeShown = true;
            host.net                                = custom ? bridgeNet() : kindNet(proxy::PlayerNetwork::Kind::System);
            player->play(a("s1"), kVideo, kLink);
            st->push(st->next, TSMP_EV_FAILED, 0, a("network")); // the helper's environment has other arguments
            poll();
            QCOMPARE(st->releases, 1);
            QCOMPARE(st->opens.size(), 1);
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
            st->push(0, TSMP_EV_ENGINEEXITED);
            poll();
            QCOMPARE(st->opens.size(), 2); // one re-open
            st->push(st->next, TSMP_EV_FAILED, 0, a("network"));
            poll();
            QCOMPARE(st->opens.size(), 2);
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
            QCOMPARE(player->cardOf(a("s1"), kVideo).note, custom ? a("Can't play through the proxy") : a("Can't play this video here"));
            QCOMPARE(player->engineFailuresForTests(), 0);
            QVERIFY(host.opened.isEmpty());
        }
        // An override that mentions a proxy (S9): the proxy note at once.
        make();
        Settings::instance().youTubeNoticeShown = true;
        host.net                                = bridgeNet();
        player->play(a("s1"), kVideo, kLink);
        st->push(st->next, TSMP_EV_FAILED, 0, a("network-override"));
        poll();
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play through the proxy"));
        QCOMPARE(player->engineFailuresForTests(), 0);
    }
    void busyRetries()
    {
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        for (int round = 1; round <= 3; ++round) {
            st->push(st->next, TSMP_EV_FAILED, int(0x8007139F), a("busy"));
            poll();
            QVERIFY(player->timerForTests("busy")->isActive());
            QCOMPARE(player->timerForTests("busy")->interval(), 3000);
            QCOMPARE(stateOf("s1"), YouTubePlayer::State::Starting);
            QVERIFY(player->hasPlayer());
            fire("busy");
            QCOMPARE(st->opens.size(), round + 1);
        }
        st->push(st->next, TSMP_EV_FAILED, int(0x8007139F), a("busy"));
        poll();
        QVERIFY(!player->timerForTests("busy")->isActive());
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play this video here"));
        QCOMPARE(player->engineFailuresForTests(), 0); // never toward the disable
        QVERIFY(!player->unavailableForTests());
        QVERIFY(host.opened.isEmpty());
    }
    void noReadyThroughBridge()
    {
        Settings::instance().youTubeNoticeShown = true;
        host.net = bridgeNet(5);
        player->play(a("s1"), kVideo, kLink);
        st->push(st->next, TSMP_EV_CREATED);
        poll();
        proxy::Record r;
        r.client  = 'p';
        r.host    = a("www.youtube.com");
        r.step    = proxy::Step::Connect;
        r.failure = proxy::Failure::Unreachable;
        host.records << r;
        proxy::Record fine;
        fine.client = 'p';
        fine.host   = a("i.ytimg.com");
        fine.step   = proxy::Step::Done;
        host.records.prepend(fine);
        fire("ready");
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play through the proxy"));
        QCOMPARE(host.recordsAsked.size(), 1);
        QVERIFY(host.recordsAsked.first() > 0); // since the open
        QCOMPARE(host.reports.size(), 1);
        QCOMPARE(host.reports.first().failure, proxy::Failure::Unreachable);
        QCOMPARE(host.reports.first().step, proxy::Step::Connect);
        QVERIFY(host.reports.first().decisive);
        QCOMPARE(host.reports.first().generation, quint64(5));
        QCOMPARE(host.reports.first().via, proxy::Via::Bridge);
        QCOMPARE(host.reports.first().host, a("www.youtube.com"));
        // No route-level record: 2.5's note, no report.
        host.records.clear();
        host.records << fine;
        player->tryAgain(a("s2"), kVideo, kLink);
        st->push(st->next, TSMP_EV_CREATED);
        poll();
        fire("ready");
        QCOMPARE(player->cardOf(a("s2"), kVideo).note, a("Can't play this video here"));
        QCOMPARE(host.reports.size(), 1);
        // Windows mode: the records are never asked.
        make();
        Settings::instance().youTubeNoticeShown = true;
        player->play(a("s1"), kVideo, kLink);
        st->push(st->next, TSMP_EV_CREATED);
        poll();
        fire("ready");
        QVERIFY(host.recordsAsked.isEmpty());
        QCOMPARE(player->cardOf(a("s1"), kVideo).note, a("Can't play this video here"));
    }
    void readyReportsOk()
    {
        host.net = bridgeNet(7);
        playToReady();
        QCOMPARE(host.oks, QVector<quint64>({7}));
        make();
        playToReady(); // Windows settings: nothing for route health
        QVERIFY(host.oks.isEmpty());
    }
    void proxyNoteGoesOnRouteChange()
    {
        Settings::instance().youTubeNoticeShown = true;
        host.net = kindNet(proxy::PlayerNetwork::Kind::Blocked, proxy::Failure::PasswordUnreadable);
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::Note);
        host.net = kindNet(proxy::PlayerNetwork::Kind::System);
        host.changed.clear();
        player->networkChanged();
        QCOMPARE(stateOf("s1"), YouTubePlayer::State::None);
        QVERIFY(host.changed.contains(a("s1")));
        player->play(a("s1"), kVideo, kLink);
        QCOMPARE(st->opens.size(), 1);
    }

    // ---- Y-T6 source rules ----
    void sourceRules()
    {
        const auto read = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        const QString root = QString::fromLatin1(TSMEDIA_SOURCE_DIR);
        // No WebView2 header outside player/.
        QDirIterator it(root + a("/src"), {a("*.cpp"), a("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        int          files = 0;
        while (it.hasNext()) {
            const QString path = it.next();
            ++files;
            const QString text = read(path);
            QVERIFY2(!text.contains(a("#include <WebView2")) && !text.contains(a("#include \"WebView2")), qPrintable(path));
        }
        QVERIFY(files > 50);
        const QString helper = read(root + a("/player/tsmp.cpp"));
        QVERIFY(!helper.isEmpty());
        // Every add_ has its remove_ (the handlers go by token at close).
        static const QRegularExpression adds(a("->add_(\\w+)\\("));
        auto                            m = adds.globalMatch(helper);
        int                             n = 0;
        while (m.hasNext()) {
            const QString name = m.next().captured(1);
            ++n;
            QVERIFY2(helper.contains(a("->remove_") + name + QLatin1Char('(')), qPrintable(name));
        }
        QVERIFY(n >= 12);
        // No threads, hooks or timers of its own.
        for (const char* banned : {"CreateThread", "_beginthread", "std::thread", "SetWindowsHookEx", "SetTimer(", "QueueUserWorkItem"})
            QVERIFY2(!helper.contains(QLatin1String(banned)), banned);
        // Every SetWindowPos keeps the z-order (a move never lifts the player above TeamSpeak's menus).
        static const QRegularExpression setPos(a("SetWindowPos\\(([^;]*);"));
        auto                            p     = setPos.globalMatch(helper);
        int                             moves = 0;
        while (p.hasNext()) {
            const QString call = p.next().captured(1);
            ++moves;
            QVERIFY2(call.contains(a("SWP_NOZORDER")) && call.contains(a("SWP_NOACTIVATE")), qPrintable(call));
        }
        QVERIFY(moves >= 1);
        QVERIFY(helper.contains(a("SW_PARENTOPENING")));
        QVERIFY(helper.contains(a("WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX")));
        QVERIFY(helper.contains(a("put_IsReputationCheckingRequired(FALSE)")));
        QVERIFY(helper.contains(a("put_IsInPrivateModeEnabled(TRUE)")));
        // The plugin side: no dialog (the notice is a card state), no functor singleShot, no metatype, no QStringLiteral.
        for (const char* name : {"ytpage.cpp", "ytpage.h", "ytplayer.cpp", "ytplayer.h", "ytbackend.cpp", "ytbackend.h"}) {
            const QString text = read(root + a("/src/ytplayer/") + QString::fromLatin1(name));
            QVERIFY2(!text.isEmpty(), name);
            for (const char* banned : {"QDialog", "exec(", "QMessageBox", "QTimer::singleShot", "qRegisterMetaType", "Q_DECLARE_METATYPE", "QStringLiteral",
                                       "WebView2.h"})
                QVERIFY2(!text.contains(QLatin1String(banned)), qPrintable(QString::fromLatin1(name) + a(": ") + QString::fromLatin1(banned)));
        }
    }
};

TSMEDIA_REGISTER_TEST(TestYouTubePlayer)

#include "tst_ytplayer.moc"
