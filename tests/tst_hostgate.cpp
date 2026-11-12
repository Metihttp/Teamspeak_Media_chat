// Unit tests for 2.5 link previews: HostGate (src/linkpreview/hostgate.*): spacing and bursts per spacing group and
// lane, cooldowns per answering domain (Retry-After, x-ratelimit), the breaker, walled memos, bounded keys, stale
// tickets, the shared gate's lifetime. A virtual clock: nothing here sleeps.

#include <QtTest>

#include "linkpreview/hostgate.h"
#include "linkpreview/providers.h"
#include "testmain.h"

using namespace linkpreview;
using Lane    = HostGate::Lane;
using Verdict = HostGate::Verdict;
using Outcome = HostGate::Outcome;

namespace {

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

struct Clock {
    qint64 now = 1000000;
    std::function<qint64()> fn()
    {
        return [this] { return now; };
    }
};

// Go now (and the ticket), or the verdict.
Verdict take(HostGate& gate, const char* host, Lane lane, HostGate::Ticket* ticket = nullptr, qint64* ms = nullptr, const char* segment = "",
             bool userAsked = false)
{
    qint64           wait = 0;
    HostGate::Ticket t;
    const Verdict    v = gate.acquire(a(host), lane, a(segment), userAsked, &wait, &t);
    if (ticket)
        *ticket = t;
    if (ms)
        *ms = wait;
    return v;
}

} // namespace

class TestHostGate : public QObject
{
    Q_OBJECT

  private slots:
    void spacingAndBurst()
    {
        Clock    clock;
        HostGate gate(clock.fn());
        const qint64 start = clock.now;
        QVector<qint64> starts;
        // Five page requests to one group, each done at once: they start at 0, 1, 2, 3 s and the fifth at 10 s.
        while (starts.size() < 5) {
            HostGate::Ticket ticket;
            qint64           ms = 0;
            const Verdict    v  = take(gate, "www.example.com", Lane::Page, &ticket, &ms);
            if (v == Verdict::Go) {
                starts << clock.now - start;
                gate.release(ticket, Outcome::Ok);
                continue;
            }
            QCOMPARE(v, Verdict::Wait);
            QVERIFY(ms > 0 && ms <= 10000);
            clock.now += ms;
        }
        QCOMPARE(starts, QVector<qint64>({0, 1000, 2000, 3000, 10000}));
        // Another host of the same registrable domain is the same group.
        QCOMPARE(take(gate, "cdn.example.com", Lane::Page), Verdict::Wait);
    }

    void lanesIndependent()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket page;
        QCOMPARE(take(gate, "example.com", Lane::Page, &page), Verdict::Go);
        // The page lane is busy; the media lane of the same group is not.
        QCOMPARE(take(gate, "example.com", Lane::Page), Verdict::Wait);
        HostGate::Ticket m1, m2;
        QCOMPARE(take(gate, "example.com", Lane::Media, &m1), Verdict::Go);
        clock.now += 250;
        QCOMPARE(take(gate, "example.com", Lane::Media, &m2), Verdict::Go);
        clock.now += 250;
        QCOMPARE(take(gate, "example.com", Lane::Media), Verdict::Wait); // two running at most
        gate.release(m1, Outcome::Ok);
        QCOMPARE(take(gate, "example.com", Lane::Media), Verdict::Go);
        QCOMPARE(gate.runningCount(), 3);
    }

    void oneRunningPerPageLane()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "example.com", Lane::Page, &t), Verdict::Go);
        clock.now += 5000;
        qint64 ms = 0;
        QCOMPARE(take(gate, "example.com", Lane::Page, nullptr, &ms), Verdict::Wait); // still running after 5 s
        QVERIFY(ms > 0);
        gate.release(t, Outcome::Ok);
        QCOMPARE(take(gate, "example.com", Lane::Page), Verdict::Go);
        // Other sites are never held up.
        QCOMPARE(take(gate, "other.example.org", Lane::Page), Verdict::Go);
    }

    void cooldown_data()
    {
        QTest::addColumn<qint64>("retryAfterMs");
        QTest::addColumn<qint64>("rateResetMs");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("retry-after 120") << qint64(120000) << qint64(-1) << qint64(120000);
        QTest::newRow("date (parsed: 90 s)") << qint64(90000) << qint64(-1) << qint64(90000);
        QTest::newRow("missing") << qint64(-1) << qint64(-1) << qint64(60000);
        QTest::newRow("zero") << qint64(0) << qint64(-1) << qint64(60000);
        QTest::newRow("99999 s") << qint64(99999000) << qint64(-1) << qint64(600000);
        QTest::newRow("half a second") << qint64(500) << qint64(-1) << qint64(1000);
        QTest::newRow("rate reset") << qint64(-1) << qint64(43000) << qint64(43000);
    }
    void cooldown()
    {
        QFETCH(qint64, retryAfterMs);
        QFETCH(qint64, rateResetMs);
        QFETCH(qint64, expected);
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "api.example.com", Lane::Page, &t), Verdict::Go);
        gate.release(t, Outcome::RateLimited, retryAfterMs, -1, rateResetMs);
        qint64 ms   = 0;
        bool   soft = true;
        QCOMPARE(gate.state(a("api.example.com"), Lane::Page, QString(), &ms, &soft), Verdict::Cooling);
        QCOMPARE(ms, expected);
        QVERIFY(!soft);
        clock.now += expected - 1;
        QCOMPARE(gate.state(a("www.example.com"), Lane::Media, QString(), &ms), Verdict::Cooling); // the whole domain
        clock.now += 1;
        QVERIFY(gate.state(a("api.example.com"), Lane::Page, QString(), &ms) != Verdict::Cooling);
    }

    void imgurMinimum()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "i.imgur.com", Lane::Media, &t), Verdict::Go);
        gate.release(t, Outcome::RateLimited, 0); // Imgur answers "Retry-After: 0"
        qint64 ms = 0;
        QCOMPARE(gate.state(a("imgur.com"), Lane::Page, QString(), &ms), Verdict::Cooling);
        QCOMPARE(ms, qint64(600000));
        QCOMPARE(providers::minCooldownMs(a("imgur.com")), 600000);
    }

    void walledPerSegment()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "klipy.com", Lane::Page, &t, nullptr, "gifs"), Verdict::Go);
        gate.release(t, Outcome::Walled);
        clock.now += 1000;
        qint64 ms = 0;
        QCOMPARE(take(gate, "klipy.com", Lane::Page, nullptr, &ms, "gifs"), Verdict::Walled);
        QCOMPARE(ms, qint64(1800000 - 1000));
        // A user's click never passes a wall.
        QCOMPARE(take(gate, "klipy.com", Lane::Page, nullptr, nullptr, "gifs", true), Verdict::Walled);
        QCOMPARE(take(gate, "www.klipy.com", Lane::Page, nullptr, nullptr, "clips"), Verdict::Go);
        clock.now += 1800000;
        QVERIFY(gate.state(a("klipy.com"), Lane::Page, a("gifs"), &ms) != Verdict::Walled);
    }

    // A 200 answer that can never give a preview (Klipy's oEmbed naming no GIF) is walled like a challenge: its domain and
    // first segment, 30 minutes, every lane and a click alike; nothing else of the domain.
    void markWalled()
    {
        Clock    clock;
        HostGate gate(clock.fn());
        gate.markWalled(a("api.klipy.com"), a("api"));
        qint64 ms = 0;
        QCOMPARE(gate.state(a("api.klipy.com"), Lane::Page, a("api"), &ms), Verdict::Walled);
        QCOMPARE(ms, qint64(1800000));
        QCOMPARE(take(gate, "api.klipy.com", Lane::Page, nullptr, nullptr, "api", true), Verdict::Walled);
        HostGate::Ticket page, media;
        QCOMPARE(take(gate, "klipy.com", Lane::Page, &page, nullptr, "gifs"), Verdict::Go); // the page's own memo is apart
        QCOMPARE(take(gate, "static2.klipy.com", Lane::Media, &media, nullptr, "ii"), Verdict::Go);
        gate.release(page, Outcome::Ok);
        gate.release(media, Outcome::Ok);
        QCOMPARE(gate.describe(), QStringList{a("health klipy.com walled /api 1800 s")});
        clock.now += 1800000;
        QVERIFY(gate.state(a("api.klipy.com"), Lane::Page, a("api"), &ms) != Verdict::Walled);
        gate.markWalled(QString(), a("x")); // no host: nothing
        QVERIFY(gate.describe().isEmpty());
    }

    void breaker()
    {
        Clock    clock;
        HostGate gate(clock.fn());
        for (int i = 0; i < 4; ++i) {
            HostGate::Ticket t;
            QCOMPARE(take(gate, "slow.example.com", Lane::Media, &t), Verdict::Go);
            gate.release(t, i % 2 ? Outcome::Timeout : Outcome::ServerError);
            clock.now += 2000;
        }
        // An Ok resets the count.
        HostGate::Ticket ok;
        QCOMPARE(take(gate, "slow.example.com", Lane::Media, &ok), Verdict::Go);
        gate.release(ok, Outcome::Ok);
        clock.now += 2000;
        for (int i = 0; i < 5; ++i) {
            HostGate::Ticket t;
            QCOMPARE(take(gate, "slow.example.com", Lane::Media, &t), Verdict::Go);
            gate.release(t, Outcome::Offline);
            clock.now += 2000;
        }
        qint64 ms = 0;
        QCOMPARE(take(gate, "slow.example.com", Lane::Media, nullptr, &ms), Verdict::Broken);
        QCOMPARE(ms, qint64(600000 - 2000));
        // A user's click passes the breaker once; an Ok closes it again.
        HostGate::Ticket click;
        QCOMPARE(take(gate, "slow.example.com", Lane::Media, &click, nullptr, "", true), Verdict::Go);
        gate.release(click, Outcome::Ok);
        clock.now += 1000;
        QCOMPARE(take(gate, "slow.example.com", Lane::Media), Verdict::Go);
    }

    void gyazo503NotCounted()
    {
        Clock    clock;
        HostGate gate(clock.fn());
        for (int i = 0; i < 8; ++i) {
            HostGate::Ticket t;
            QCOMPARE(take(gate, "api.gyazo.com", Lane::Page, &t), Verdict::Go);
            gate.release(t, Outcome::Other);
            clock.now += 10000;
        }
        QCOMPARE(take(gate, "api.gyazo.com", Lane::Page), Verdict::Go);
    }

    void boundedKeys()
    {
        Clock              clock;
        HostGate::Limits   limits;
        limits.maxKeys = 32;
        HostGate gate(clock.fn(), limits);
        for (int i = 0; i < 500; ++i) {
            HostGate::Ticket t;
            const QByteArray host = "www.site" + QByteArray::number(i) + ".com"; // every one its own group and health key
            QCOMPARE(take(gate, host.constData(), Lane::Media, &t), Verdict::Go);
            gate.release(t, i % 3 ? Outcome::Ok : Outcome::RateLimited, 1000);
            clock.now += 20;
        }
        // Bounded: describe() lists at most the kept entries (no running ones are left).
        QVERIFY(gate.describe().size() <= 2 * limits.maxKeys);
        QCOMPARE(gate.runningCount(), 0);
    }

    void staleTicketIgnored()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "example.com", Lane::Page, &t), Verdict::Go);
        gate.clear();
        gate.release(t, Outcome::RateLimited, 600000); // after clear: nothing happens
        QCOMPARE(take(gate, "example.com", Lane::Page), Verdict::Go);
        HostGate::Ticket fake;
        fake.id     = 987654;
        fake.domain = a("example.org");
        gate.release(fake, Outcome::RateLimited, 600000);
        gate.unused(fake);
        QCOMPARE(take(gate, "example.org", Lane::Page), Verdict::Go);
    }

    void unusedTicketUncounted()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket cached;
        QCOMPARE(take(gate, "example.com", Lane::Page, &cached), Verdict::Go);
        gate.unused(cached); // a cache hit: no request was made
        QCOMPARE(take(gate, "example.com", Lane::Page), Verdict::Go); // no spacing to wait for
        QCOMPARE(gate.runningCount(), 1);
    }

    void softCooldownFromHeaders()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket t;
        QCOMPARE(take(gate, "www.reddit.com", Lane::Page, &t), Verdict::Go);
        gate.release(t, Outcome::Ok, -1, 0.0, 43000); // 200 with "x-ratelimit-remaining: 0.0, x-ratelimit-reset: 43"
        qint64 ms   = 0;
        bool   soft = false;
        QCOMPARE(gate.state(a("www.reddit.com"), Lane::Page, QString(), &ms, &soft), Verdict::Cooling);
        QCOMPARE(ms, qint64(43000));
        QVERIFY(soft);
        // Not a failure: the breaker count is untouched (four timeouts later the domain is not broken).
        clock.now += 43000;
        for (int i = 0; i < 4; ++i) {
            HostGate::Ticket f;
            QCOMPARE(take(gate, "i.redd.it", Lane::Media, &f), Verdict::Go);
            gate.release(f, Outcome::Timeout);
            clock.now += 1000;
        }
        QCOMPARE(take(gate, "www.reddit.com", Lane::Page), Verdict::Go);
        // remaining 5: nothing.
        Clock            c2;
        HostGate         g2(c2.fn());
        HostGate::Ticket t2;
        QCOMPARE(take(g2, "www.reddit.com", Lane::Page, &t2), Verdict::Go);
        g2.release(t2, Outcome::Ok, -1, 5.0, 43000);
        c2.now += 6000;
        HostGate::Ticket t3;
        QCOMPARE(take(g2, "www.reddit.com", Lane::Page, &t3), Verdict::Go);
        // A 429's longer hard cooldown stays when a later answer says "remaining 0" with a short reset.
        g2.release(t3, Outcome::RateLimited, 300000);
        QCOMPARE(g2.state(a("reddit.com"), Lane::Page, QString(), &ms, &soft), Verdict::Cooling);
        QVERIFY(!soft);
        QCOMPARE(ms, qint64(300000));
        HostGate::Ticket late;
        QCOMPARE(g2.acquire(a("i.redd.it"), Lane::Media, QString(), false, &ms, &late), Verdict::Go);
        HostGate::Ticket redditLate = late;
        redditLate.domain = a("reddit.com"); // as if reddit.com had answered it
        g2.release(redditLate, Outcome::Ok, -1, 0.0, 5000);
        QCOMPARE(g2.state(a("reddit.com"), Lane::Page, QString(), &ms, &soft), Verdict::Cooling);
        QVERIFY(!soft);
        QCOMPARE(ms, qint64(300000));
    }

    void redditRemainingZeroCoolsWithout429()
    {
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket first;
        QCOMPARE(take(gate, "www.reddit.com", Lane::Page, &first), Verdict::Go);
        gate.release(first, Outcome::Ok, -1, 0.0, 43000);
        clock.now += 6000; // the reddit spacing
        bool   soft = false;
        qint64 ms   = 0;
        QCOMPARE(gate.acquire(a("www.reddit.com"), Lane::Page, QString(), false, &ms, nullptr, &soft), Verdict::Cooling);
        QVERIFY(soft);
        QCOMPARE(ms, qint64(37000));
        QCOMPARE(providers::pageSpacingMs(a("reddit")), 6000);
    }

    void gateAttributionAcrossRedirect()
    {
        // bit.ly answers a redirect (Ok), i.imgur.com answers 429: imgur.com cools, bit.ly doesn't.
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket shortener, target;
        QCOMPARE(take(gate, "bit.ly", Lane::Page, &shortener), Verdict::Go);
        gate.release(shortener, Outcome::Ok);
        QCOMPARE(take(gate, "i.imgur.com", Lane::Media, &target), Verdict::Go);
        gate.release(target, Outcome::RateLimited, 0);
        clock.now += 2000;
        QCOMPARE(take(gate, "bit.ly", Lane::Page), Verdict::Go);
        QCOMPARE(take(gate, "imgur.com", Lane::Page), Verdict::Cooling);
    }

    void apiCooldownLeavesCdn()
    {
        Clock    clock;
        HostGate gate(clock.fn());
        for (const auto& pair : {std::make_pair("discord.com", "cdn.discordapp.com"), std::make_pair("x.com", "pbs.twimg.com"),
                                 std::make_pair("www.reddit.com", "i.redd.it")}) {
            HostGate::Ticket api;
            QCOMPARE(take(gate, pair.first, Lane::Page, &api), Verdict::Go);
            gate.release(api, Outcome::RateLimited, 120000);
            QCOMPARE(take(gate, pair.second, Lane::Media), Verdict::Go);
            QCOMPARE(gate.state(a(pair.first), Lane::Page, QString(), nullptr), Verdict::Cooling);
        }
    }

    void shortenerCannotCoolTarget()
    {
        // t.co redirects to attacker.example.net, which answers 429 Retry-After 600: only attacker.example.net cools.
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket hop1, hop2;
        QCOMPARE(take(gate, "t.co", Lane::Page, &hop1), Verdict::Go);
        gate.release(hop1, Outcome::Ok);
        QCOMPARE(take(gate, "attacker.example.net", Lane::Page, &hop2), Verdict::Go);
        gate.release(hop2, Outcome::RateLimited, 600000);
        clock.now += 1000;
        QCOMPARE(take(gate, "t.co", Lane::Page), Verdict::Go);
        QCOMPARE(take(gate, "www.example.org", Lane::Page), Verdict::Go);
        QCOMPARE(gate.state(a("attacker.example.net"), Lane::Page, QString(), nullptr), Verdict::Cooling);
    }

    void groupsSpaceButDontCool()
    {
        // youtube.com and i.ytimg.com are one spacing group but two health keys.
        Clock            clock;
        HostGate         gate(clock.fn());
        HostGate::Ticket page;
        QCOMPARE(take(gate, "www.youtube.com", Lane::Page, &page), Verdict::Go);
        gate.release(page, Outcome::RateLimited, 60000);
        QCOMPARE(take(gate, "i.ytimg.com", Lane::Media), Verdict::Go);
        clock.now += 500;
        QCOMPARE(take(gate, "i.ytimg.com", Lane::Page), Verdict::Wait); // spaced with the youtube.com request
        QCOMPARE(spacingGroup(a("i.ytimg.com")), spacingGroup(a("www.youtube.com")));
        QVERIFY(healthKey(a("i.ytimg.com")) != healthKey(a("www.youtube.com")));
    }

    void spacingGroups()
    {
        QCOMPARE(spacingGroup(a("www.youtube.com")), a("youtube"));
        QCOMPARE(spacingGroup(a("youtu.be")), a("youtube"));
        QCOMPARE(spacingGroup(a("i.ytimg.com")), a("youtube"));
        QCOMPARE(spacingGroup(a("pbs.twimg.com")), a("x"));
        QCOMPARE(spacingGroup(a("twitter.com")), a("x"));
        QCOMPARE(spacingGroup(a("old.reddit.com")), a("reddit"));
        QCOMPARE(spacingGroup(a("i.redd.it")), a("reddit"));
        QCOMPARE(spacingGroup(a("gph.is")), a("giphy"));
        QCOMPARE(spacingGroup(a("cdn.discordapp.com")), a("discord"));
        QCOMPARE(spacingGroup(a("media.discordapp.net")), a("discord"));
        QCOMPARE(spacingGroup(a("static-cdn.jtvnw.net")), a("twitch"));
        QCOMPARE(spacingGroup(a("i.vimeocdn.com")), a("vimeo"));
        QCOMPARE(spacingGroup(a("www.pinterest.co.uk")), a("pinterest"));
        QCOMPARE(spacingGroup(a("i.pinimg.com")), a("pinterest"));
        QCOMPARE(spacingGroup(a("i1.sndcdn.com")), a("soundcloud"));
        QCOMPARE(spacingGroup(a("i.scdn.co")), a("spotify"));
        QCOMPARE(spacingGroup(a("s.team")), a("steam"));
        QCOMPARE(spacingGroup(a("shared.akamai.steamstatic.com")), a("steam"));
        QCOMPARE(spacingGroup(a("www.example.co.uk")), a("example.co.uk"));
        QCOMPARE(spacingGroup(a("user.github.io")), a("user.github.io"));
    }

    void spacingGroupVsHealthKey()
    {
        QCOMPARE(spacingGroup(a("discord.com")), spacingGroup(a("cdn.discordapp.com")));
        QCOMPARE(healthKey(a("discord.com")), a("discord.com"));
        QCOMPARE(healthKey(a("cdn.discordapp.com")), a("discordapp.com"));
        QCOMPARE(healthKey(a("i.imgur.com")), healthKey(a("imgur.com")));
    }

    void mediaOverrides()
    {
        // opengraph.githubassets.com answered 429 after 4 quick requests: 1 s apart, 3 per 10 s.
        Clock    clock;
        HostGate gate(clock.fn());
        int      started = 0;
        for (int i = 0; i < 4; ++i) {
            HostGate::Ticket t;
            if (take(gate, "opengraph.githubassets.com", Lane::Media, &t) == Verdict::Go) {
                ++started;
                gate.release(t, Outcome::Ok);
            }
            clock.now += 1000;
        }
        QCOMPARE(started, 3);
    }

    void sharedGateLifetime()
    {
        std::weak_ptr<HostGate> seen;
        {
            std::shared_ptr<HostGate> one = sharedGate();
            std::shared_ptr<HostGate> two = sharedGate();
            QCOMPARE(one.get(), two.get()); // both Fetchers hold the same gate
            seen = one;
            std::shared_ptr<HostGate> jobCopy = one; // a FetchJob's copy keeps it alive
            one.reset();
            two.reset();
            QVERIFY(!seen.expired());
            Q_UNUSED(jobCopy)
        }
        QVERIFY(seen.expired());
        std::shared_ptr<HostGate> next = sharedGate();
        QVERIFY(next);
        QVERIFY(seen.expired());
    }
};

TSMEDIA_REGISTER_TEST(TestHostGate)

#include "tst_hostgate.moc"
