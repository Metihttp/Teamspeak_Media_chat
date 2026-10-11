// 2.3 poll: the poll message (compose and read back), ballots and their hash chains, the tally, the
// plugin-command messages (hostile input), the store (polls.json), PollHub with a fake transport (your vote,
// retries, late joiners, anonymity: nothing about a sender is kept), the chat document (TeamSpeak-like, both
// looks) and the card's layout and hit tests, and the New poll window.

#include <QAbstractButton>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtMath>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

#include "chatemoji.h" // copying from the chat
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "medialink.h"
#include "pollart.h"
#include "polldialog.h"
#include "polldoc.h"
#include "pollhub.h"
#include "polls.h"
#include "replydoc.h"
#include "testmain.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

const QString kKey = QStringLiteral("3f9a1c2e5b6d7e80");

polls::Poll samplePoll(int options = 3, bool multiple = false)
{
    polls::Poll p;
    p.key       = kKey;
    p.question  = QStringLiteral("Where do we eat?");
    p.multiple  = multiple;
    p.channelId = 12;
    for (int i = 0; i < options; ++i)
        p.options << QStringLiteral("Option %1").arg(i + 1);
    return p;
}

// What TeamSpeak shows of a BBCode message: backslash escapes gone, [URL=x]y[/URL] -> y (the href aside).
QString displayed(const QString& bbcode, QString* href)
{
    QString text = bbcode;
    const int open = text.lastIndexOf(QStringLiteral("[URL="));
    const int mid  = text.indexOf(QLatin1Char(']'), open);
    const int end  = text.indexOf(QStringLiteral("[/URL]"));
    if (href)
        *href = text.mid(open + 5, mid - open - 5);
    text = text.left(open);
    text.replace(QStringLiteral("\\["), QStringLiteral("["));
    text.replace(QStringLiteral("\\]"), QStringLiteral("]"));
    Q_UNUSED(end)
    return text;
}

class FakeEnv : public PollHub::Env
{
  public:
    struct Sent {
        quint64             sch;
        proto::Message      message;
        bool                background;
        QString             coalesce;
        peers::Sender::Done done;
    };
    qint64              now     = 1000;
    quint64             channel = 12;
    bool                connected = true;
    bool                others    = true;
    bool                isBlocked = false;
    bool                hidden    = false; // presence off (Env::quiet)
    QHash<quint16, quint64> where; // client -> channel
    QVector<Sent>       sent;

    qint64  nowMs() const override { return now; }
    qint64  wallMs() const override { return 1700000000000 + now; }
    bool    isConnected(quint64) const override { return connected; }
    quint64 ownChannel(quint64) const override { return connected ? channel : 0; }
    quint64 channelOf(quint64, quint16 client) const override { return where.value(client); }
    bool    isQueryClient(quint64, quint16 client) const override { return client == 99; }
    bool    othersHaveTsMedia(quint64) const override { return others; }
    bool    blocked(quint64) const override { return isBlocked; }
    bool    quiet(quint64) const override { return hidden; }
    int     randomBetween(int low, int) override { return low; }
    void    send(quint64 sch, const proto::Message& message, bool background, const QString& coalesce, peers::Sender::Done done) override
    {
        sent.append({sch, message, background, coalesce, std::move(done)});
    }
};

// "2, 1, 0" (QVector's == trips a deprecated checked iterator with Qt 5.15 and a current MSVC).
QString numbers(const QVector<int>& list)
{
    QStringList parts;
    for (const int n : list)
        parts << QString::number(n);
    return parts.join(QStringLiteral(", "));
}

proto::Message roundTrip(const proto::Message& m)
{
    const QByteArray wire = proto::serialize(m);
    std::optional<proto::Message> back = proto::parse(wire);
    return back ? *back : proto::Message();
}

QString pollHtml(const tschat::Skin& s, const polls::Poll& poll, const QString& question = QString())
{
    QString body = QStringLiteral("\U0001F4CA Poll: ") + tschat::escaped(question.isEmpty() ? poll.question : question);
    for (int i = 0; i < poll.options.size(); ++i)
        body += QStringLiteral("<br>%1. %2").arg(i + 1).arg(tschat::escaped(poll.options.at(i)));
    body += QStringLiteral("<br>") + tschat::linkHtml(s, QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat#") + polls::markerFragment(poll),
                                                      QStringLiteral("Vote with TS Media chat"));
    return body;
}

} // namespace

class TestPolls : public QObject
{
    Q_OBJECT

  private slots:
    // ---- the message ------------------------------------------------------------------------------------
    void keysAndMarkers()
    {
        for (int i = 0; i < 50; ++i)
            QVERIFY(polls::isKey(polls::newKey()));
        QVERIFY(!polls::isKey(QStringLiteral("0000000000000000")));
        QVERIFY(!polls::isKey(QStringLiteral("3F9A1C2E5B6D7E80")));
        QVERIFY(!polls::isKey(QStringLiteral("3f9a1c2e5b6d7e8")));
        const polls::Poll poll = samplePoll(10, true);
        QCOMPARE(polls::markerFragment(poll), QStringLiteral("tsmpoll1-3f9a1c2e5b6d7e80-10m-12"));
        const polls::Marker m = polls::parseMarker(QStringLiteral("https://example.com/x#") + polls::markerFragment(poll));
        QVERIFY(m.valid());
        QCOMPARE(m.key, kKey);
        QCOMPARE(m.count, 10);
        QVERIFY(m.multiple);
        QCOMPARE(m.channelId, quint64(12));
        // Hostile or foreign markers.
        for (const char* bad : {"https://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-11s-12", "https://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-1s-12",
                                "https://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-3s-0", "https://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-3s-12x",
                                "https://example.com/#tsmpoll1-0000000000000000-3s-12", "ts3file://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-3s-12",
                                "https://example.com/#tsmpoll2-3f9a1c2e5b6d7e80-3s-12", "https://example.com/#tsmpoll1-3f9a1c2e5b6d7e80-3x-12",
                                "https://example.com/tsmpoll1-3f9a1c2e5b6d7e80-3s-12"})
            QVERIFY2(!polls::parseMarker(QString::fromLatin1(bad)).valid(), bad);
    }

    void composeAndReadBack()
    {
        polls::Poll poll = samplePoll(3);
        poll.question    = QStringLiteral("[b]Bold?[/b] or \\[URL=x]  plain  ");
        poll.options[1]  = QStringLiteral("Sushi [/URL]");
        const QString text = polls::composeMessage(poll, QStringLiteral("https://example.com/page#old"));
        QVERIFY(text.startsWith(QStringLiteral("\U0001F4CA Poll: ")));
        QVERIFY(!text.contains(QStringLiteral("[b]")));           // escaped: \[b]
        QVERIFY(text.contains(QStringLiteral("\\[b\\]Bold?")));
        QVERIFY(text.contains(QStringLiteral("\n2. Sushi \\[/URL\\]")));
        QVERIFY(text.contains(QStringLiteral("[URL=https://example.com/page#tsmpoll1-3f9a1c2e5b6d7e80-3s-12]Vote with TS Media chat[/URL]")));
        QCOMPARE(text.count(QRegularExpression(QStringLiteral("(?<!\\\\)\\[URL="))), 1); // the only link (the others are escaped)
        QVERIFY(escapedMessageSize(text) < kMaxMessageBytes);
        // A page that isn't a plain web address: the default page instead.
        QVERIFY(polls::composeMessage(poll, QStringLiteral("javascript:alert(1)")).contains(QStringLiteral("[URL=https://github.com/Metihttp/Teamspeak_Media_chat#")));
        QVERIFY(polls::composeMessage(poll, QStringLiteral("https://example.com/a]b")).contains(QStringLiteral("[URL=https://github.com/Metihttp/")));
        // Read back from what the chat shows.
        QString                          href;
        const QString                    shown = displayed(text, &href);
        const std::optional<polls::Poll> back  = polls::parseText(shown, polls::parseMarker(href));
        QVERIFY(back);
        QCOMPARE(back->question, QStringLiteral("[b]Bold?[/b] or \\[URL=x] plain"));
        QCOMPARE(back->options.at(1), QStringLiteral("Sushi [/URL]"));
        QCOMPARE(back->channelId, quint64(12));
        // The text must match the marker: another number of options, a missing number, no question.
        polls::Marker four = polls::parseMarker(href);
        four.count         = 4;
        QVERIFY(!polls::parseText(shown, four));
        QString swapped = shown;
        swapped.replace(QStringLiteral("\n2. "), QStringLiteral("\n3. "));
        QVERIFY(!polls::parseText(swapped, polls::parseMarker(href)));
        QVERIFY(!polls::parseText(QStringLiteral("Poll:\n1. a\n2. b\n3. c"), polls::parseMarker(href)));
        QVERIFY(!polls::parseText(QStringLiteral("Hello\n1. a\n2. b\n3. c"), polls::parseMarker(href)));
        QVERIFY(polls::parseText(QStringLiteral("Poll: q\u20281. a\u20282. b\u20283) c\u2028"), polls::parseMarker(href)));
        // Long and non-Latin texts fit one message (10 options of 100 three-byte characters).
        polls::Poll big = samplePoll(10);
        big.question    = QString(200, QChar(0x4e2d));
        for (QString& o : big.options)
            o = QString(100, QChar(0x4e2d));
        QVERIFY(escapedMessageSize(polls::composeMessage(big, QString())) <= kMaxMessageBytes);
    }

    void cleanLines()
    {
        QCOMPARE(polls::cleanLine(QStringLiteral("  a\tb\n\nc  "), 50), QStringLiteral("a b c"));
        QCOMPARE(polls::cleanLine(QStringLiteral("x\u202Ey\u2066z\u200B!\u0007"), 50), QStringLiteral("xyz!"));
        QCOMPARE(polls::cleanLine(QStringLiteral("abc"), 2), QStringLiteral("ab"));
        const QString emoji = QStringLiteral("a\U0001F600");
        QCOMPARE(polls::cleanLine(emoji, 2), QStringLiteral("a")); // never half an emoji
        QCOMPARE(polls::cleanLine(emoji, 3), emoji);
        QCOMPARE(polls::cleanLine(QString(QChar(0xd800)) + QStringLiteral("ok"), 10), QStringLiteral("ok")); // a lone half
    }

    // ---- ballots ------------------------------------------------------------------------------------------
    void hashChains()
    {
        const QByteArray secret = polls::newSecret();
        QCOMPARE(secret.size(), 16);
        const polls::Ballot first = polls::makeBallot(kKey, secret, 1, 0x1);
        const polls::Ballot third = polls::makeBallot(kKey, secret, 3, 0x2);
        QCOMPARE(first.id, third.id);
        QCOMPARE(first.id, polls::chainAt(kKey, secret, polls::kChainLength));
        polls::Ballot idOnly;
        idOnly.id = first.id;
        QVERIFY(polls::follows(kKey, idOnly, first));
        QVERIFY(polls::follows(kKey, idOnly, third)); // first seen at a later step
        QVERIFY(polls::follows(kKey, first, third));   // two steps on
        QVERIFY(!polls::follows(kKey, third, first));  // never back
        QVERIFY(!polls::follows(kKey, first, first));  // the same step twice
        // Someone else can't make the next state: a proof that doesn't hash to the known one.
        polls::Ballot forged = third;
        forged.step          = 4;
        forged.proof         = polls::newSecret();
        QVERIFY(!polls::follows(kKey, third, forged));
        forged.proof = third.proof; // a known proof at a higher step
        QVERIFY(!polls::follows(kKey, third, forged));
        // Ballots are bound to their poll.
        QVERIFY(!polls::follows(QStringLiteral("aaaaaaaaaaaaaaaa"), idOnly, first));
        // The wire form, canonical only.
        const QByteArray text = polls::entryText(third);
        const std::optional<polls::Ballot> back = polls::parseEntry(text);
        QVERIFY(back);
        QCOMPARE(back->id, third.id);
        QCOMPARE(back->proof, third.proof);
        QCOMPARE(back->step, 3);
        QCOMPARE(back->mask, quint16(2));
        QVERIFY(!polls::parseEntry(text + "z"));
        QVERIFY(!polls::parseEntry(QByteArray(text).replace(".3.", ".03.")));
        QVERIFY(!polls::parseEntry(QByteArray(text).replace(".3.", ".0.")));
        QVERIFY(!polls::parseEntry(QByteArray(text).replace(".3.", ".121.")));
        QVERIFY(!polls::parseEntry(text.left(text.size() - 1) + "400")); // options beyond 10
        QVERIFY(!polls::parseEntry(text.left(text.size() - 1) + "A"));   // upper-case hex
        QVERIFY(!polls::parseEntry("a.b.c.d"));
        QVERIFY(!polls::parseEntry(QByteArray(200, 'A')));
    }

    void tallies()
    {
        QVector<polls::Ballot> ballots;
        auto add = [&ballots](quint16 mask) {
            polls::Ballot b = polls::makeBallot(kKey, polls::newSecret(), 1, mask);
            ballots.append(b);
        };
        add(1);
        add(1);
        add(2);
        add(0);     // taken back
        add(3);     // two options: not in a one-answer poll
        add(0x10);  // an option the poll doesn't have
        const polls::Tally single = polls::tally(ballots, 3, false);
        QCOMPARE(single.voters, 3);
        QCOMPARE(numbers(single.counts), QStringLiteral("2, 1, 0"));
        const QVector<int> pct = polls::percentages(single, false);
        QCOMPARE(numbers(pct), QStringLiteral("67, 33, 0"));
        const polls::Tally multi = polls::tally(ballots, 3, true);
        QCOMPARE(multi.voters, 4);
        QCOMPARE(numbers(multi.counts), QStringLiteral("3, 2, 0"));
        QCOMPARE(numbers(polls::percentages(multi, true)), QStringLiteral("75, 50, 0"));
        // One-answer percentages always add up to 100.
        polls::Tally thirds;
        thirds.counts = {1, 1, 1};
        thirds.voters = 3;
        const QVector<int> t = polls::percentages(thirds, false);
        QCOMPARE(t.at(0) + t.at(1) + t.at(2), 100);
        QCOMPARE(numbers(polls::percentages(polls::Tally{{0, 0}, 0}, false)), QStringLiteral("0, 0"));
    }

    // ---- messages -------------------------------------------------------------------------------------------
    void wireMessages()
    {
        QVector<polls::Ballot> ballots;
        for (int i = 0; i < 20; ++i)
            ballots.append(polls::makeBallot(kKey, polls::newSecret(), 1 + i % 5, static_cast<quint16>(1u << (i % 10))));
        const QVector<proto::Message> votes = polls::makeVotes(kKey, ballots, true);
        QCOMPARE(votes.size(), 3);
        int total = 0;
        for (const proto::Message& m : votes) {
            const QByteArray wire = proto::serialize(m);
            QVERIFY2(!wire.isEmpty(), "within the protocol's limits");
            const std::optional<polls::Votes> read = polls::readVotes(roundTrip(m));
            QVERIFY(read);
            QVERIFY(read->answer);
            QCOMPARE(read->key, kKey);
            total += read->ballots.size();
        }
        QCOMPARE(total, 20);
        // Hostile: a bad key, too many entries, only junk.
        proto::Message bad = votes.first();
        bad.set("q", "XYZ");
        QVERIFY(!polls::readVotes(bad));
        proto::Message junk;
        junk.type = "PV";
        junk.set("q", kKey.toLatin1());
        junk.set("v", "a.b.c.d,e.f.g.h");
        QVERIFY(!polls::readVotes(junk));
        QByteArray many;
        for (int i = 0; i < 9; ++i)
            many += (many.isEmpty() ? QByteArray() : QByteArray(",")) + polls::entryText(ballots.at(i));
        junk.set("v", many);
        QVERIFY(!polls::readVotes(junk));
        // Queries.
        QVector<polls::Digest> digests;
        for (int i = 0; i < 10; ++i)
            digests.append({QStringLiteral("%1").arg(i + 1, 16, 16, QLatin1Char('0')), i, QByteArray("abcdEF-_")});
        const proto::Message query = polls::makeQuery(digests);
        QVERIFY(!proto::serialize(query).isEmpty());
        const std::optional<QVector<polls::Digest>> back = polls::readQuery(roundTrip(query));
        QVERIFY(back);
        QCOMPARE(back->size(), polls::kMaxQueried);
        QCOMPARE(back->first(), digests.first());
        proto::Message wrong;
        wrong.type = "PQ";
        wrong.set("q", "3f9a1c2e5b6d7e80:5:short,zz:1:abcdefgh");
        QVERIFY(!polls::readQuery(wrong));
    }

    // ---- the store ---------------------------------------------------------------------------------------------
    void storeMergeAndJson()
    {
        polls::Store        store;
        const QByteArray    secret = polls::newSecret();
        const polls::Ballot one    = polls::makeBallot(kKey, secret, 1, 1);
        const polls::Ballot two    = polls::makeBallot(kKey, secret, 2, 2);
        QCOMPARE(store.merge(kKey, two, 1), polls::Store::Merge::Added);
        QCOMPARE(store.merge(kKey, one, 2), polls::Store::Merge::Ignored); // older
        polls::Ballot forged = two;
        forged.step          = 3;
        QCOMPARE(store.merge(kKey, forged, 3), polls::Store::Merge::Ignored);
        QCOMPARE(store.merge(kKey, polls::makeBallot(kKey, secret, 5, 0), 4), polls::Store::Merge::Updated);
        QCOMPARE(store.tally(kKey, 3, false).voters, 0); // taken back
        // Your own ballot.
        std::optional<polls::Ballot> mine = store.castOwn(kKey, 4, 5);
        QVERIFY(mine);
        QCOMPARE(mine->step, 1);
        QCOMPARE(store.tally(kKey, 3, false).voters, 1);
        mine = store.castOwn(kKey, 1, 6);
        QCOMPARE(mine->step, 2);
        QCOMPARE(store.ballots(kKey).size(), 2);
        store.confirmOwn(kKey, 2, 1);
        QCOMPARE(store.own(kKey)->confirmed, quint16(1));
        store.confirmOwn(kKey, 1, 4); // an older confirmation changes nothing
        QCOMPARE(store.own(kKey)->confirmed, quint16(1));
        // JSON round trip.
        const QJsonObject json = store.toJson();
        polls::Store      copy;
        QVERIFY(copy.fromJson(json));
        QCOMPARE(copy.digest(kKey), store.digest(kKey));
        QCOMPARE(copy.own(kKey)->secret, store.own(kKey)->secret);
        QCOMPARE(copy.own(kKey)->step, 2);
        // The next own state follows the stored one (the chain goes on after a restart).
        const std::optional<polls::Ballot> next = copy.castOwn(kKey, 2, 7);
        QVERIFY(polls::follows(kKey, store.ballot(kKey, next->id), *next));
        QVERIFY(!copy.fromJson(QJsonObject{{QStringLiteral("format"), QStringLiteral("other")}}));
        // Malformed entries are left out, the rest kept.
        QJsonObject hostile = json;
        hostile.insert(QStringLiteral("polls"), QJsonArray{QJsonObject{{QStringLiteral("k"), QStringLiteral("nothex")}},
                                                           QJsonObject{{QStringLiteral("k"), kKey}, {QStringLiteral("b"), QJsonArray{QStringLiteral("x"), 5}}}});
        polls::Store h;
        QVERIFY(h.fromJson(hostile));
        QCOMPARE(h.pollCount(), 1);
        QCOMPARE(h.ballots(kKey).size(), 0);
        // File round trip.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("polls.json"));
        QVERIFY(store.save(path));
        polls::Store loaded;
        QVERIFY(loaded.load(path));
        QCOMPARE(loaded.digest(kKey), store.digest(kKey));
    }

    void storeLimits()
    {
        polls::Store store;
        for (int i = 0; i < polls::kMaxBallots + 20; ++i)
            store.merge(kKey, polls::makeBallot(kKey, polls::newSecret(), polls::kChainLength, 1), i);
        QCOMPARE(store.ballots(kKey).size(), polls::kMaxBallots);
        for (int i = 0; i < polls::kMaxPolls + 5; ++i)
            store.castOwn(QStringLiteral("%1").arg(i + 1, 16, 16, QLatin1Char('0')), 1, 10000 + i);
        QCOMPARE(store.pollCount(), polls::kMaxPolls);
        // The oldest went first (kKey was active at the start).
        QVERIFY(!store.has(kKey));
    }

    // ---- the hub ------------------------------------------------------------------------------------------------
    void voteSendsAndRetries()
    {
        FakeEnv           env;
        PollHub           hub(env, QString());
        hub.setAutoTimer(false);
        const polls::Poll poll = samplePoll(3);
        QCOMPARE(hub.canVote(1, poll), PollHub::VoteError::None);
        QCOMPARE(hub.vote(1, poll, 0x3), PollHub::VoteError::Invalid); // two answers in a one-answer poll
        QCOMPARE(hub.vote(1, poll, 0x2), PollHub::VoteError::None);
        QCOMPARE(env.sent.size(), 1);
        QCOMPARE(env.sent.first().message.type, QByteArray("PV"));
        QVERIFY(!env.sent.first().background);
        QCOMPARE(env.sent.first().coalesce, QStringLiteral("pv:") + kKey);
        PollHub::View v = hub.view(poll);
        QCOMPARE(v.own, quint16(2));
        QCOMPARE(v.tally.voters, 1); // shown at once
        QCOMPARE(v.state, PollHub::SendState::Sending);
        // Not sent: again after 2 s and 5 s, each time a new state; then failed.
        env.sent.first().done(peers::SendResult::Failed);
        QCOMPARE(hub.nextWakeMs(), env.now + 2000);
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 2);
        const int step1 = polls::readVotes(env.sent.at(0).message)->ballots.first().step;
        const int step2 = polls::readVotes(env.sent.at(1).message)->ballots.first().step;
        QCOMPARE(step2, step1 + 1);
        env.sent.at(1).done(peers::SendResult::Flooded);
        env.now += 5000;
        hub.tick();
        QCOMPARE(env.sent.size(), 3);
        env.sent.at(2).done(peers::SendResult::Failed);
        QCOMPARE(hub.view(poll).state, PollHub::SendState::Failed);
        QCOMPARE(hub.nextWakeMs(), qint64(-1));
        // Retry: sent again; the server takes it.
        hub.retry(1, poll);
        QCOMPARE(env.sent.size(), 4);
        env.sent.at(3).done(peers::SendResult::Ok);
        QCOMPARE(hub.view(poll).state, PollHub::SendState::Idle);
        QCOMPARE(hub.store().own(kKey)->confirmed, quint16(2));
        // Retract.
        QCOMPARE(hub.vote(1, poll, 0), PollHub::VoteError::None);
        QCOMPARE(hub.view(poll).own, quint16(0));
        QCOMPARE(hub.view(poll).tally.voters, 0);
        // Elsewhere: no vote.
        env.channel = 13;
        QCOMPARE(hub.canVote(1, poll), PollHub::VoteError::WrongChannel);
        QCOMPARE(hub.vote(1, poll, 1), PollHub::VoteError::WrongChannel);
        env.channel   = 12;
        env.isBlocked = true;
        QCOMPARE(hub.canVote(1, poll), PollHub::VoteError::Blocked);
        QVERIFY(!PollHub::errorText(PollHub::VoteError::WrongChannel).isEmpty());
    }

    // Ballots only from people in our channel, nothing about them kept; one answer covers a late joiner.
    void receivingIsAnonymousAndChecked()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("polls.json"));
        FakeEnv       env;
        env.where.insert(5, 12);  // Alice, in our channel
        env.where.insert(6, 40);  // Bob, elsewhere
        env.where.insert(99, 12); // a ServerQuery client
        {
            PollHub hub(env, path);
            hub.setAutoTimer(false);
            const polls::Poll   poll = samplePoll(3);
            const polls::Ballot b    = polls::makeBallot(kKey, polls::newSecret(), 1, 1);
            const proto::Message pv  = roundTrip(polls::makeVotes(kKey, {b}, false).first());
            // A poll no chat of ours shows: its ballots aren't taken (they would only fill polls.json).
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), pv);
            QCOMPARE(hub.view(poll).tally.voters, 0);
            QVERIFY(!hub.store().has(kKey));
            hub.watch(poll);
            QVERIFY(hub.watching(kKey, 12));
            QVERIFY(!hub.watching(kKey, 40));
            hub.received(1, 6, QStringLiteral("BobUid="), QStringLiteral("Bob"), pv);
            hub.received(1, 99, QStringLiteral("QueryUid="), QStringLiteral("serveradmin"), pv);
            QCOMPARE(hub.view(poll).tally.voters, 0);
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), pv);
            QCOMPARE(hub.view(poll).tally.voters, 1);
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), pv); // the same again
            QCOMPARE(hub.view(poll).tally.voters, 1);
            // A late joiner asks; we answer once (y=1) unless someone else answered first.
            polls::Digest theirs;
            theirs.key  = kKey;
            theirs.count = 0;
            theirs.hash = polls::Store().digest(kKey).hash;
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
            QVERIFY(hub.nextWakeMs() > 0);
            env.now = hub.nextWakeMs();
            hub.tick();
            QCOMPARE(env.sent.size(), 1);
            QCOMPARE(env.sent.first().message.value("y"), QByteArray("1"));
            QVERIFY(env.sent.first().background);
            // Again right away: no second answer (20 s).
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
            env.now += 5000;
            hub.tick();
            QCOMPARE(env.sent.size(), 1);
            // Later, but another member's answer is overheard first: ours is dropped.
            env.now += 30000;
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeVotes(kKey, {b}, true).first()));
            env.now += 5000;
            hub.tick();
            QCOMPARE(env.sent.size(), 1);
            hub.save();
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains(kKey.toLatin1()));
        for (const char* identity : {"Alice", "Bob", "AliceUid", "BobUid", "serveradmin"})
            QVERIFY2(!saved.contains(identity), identity); // nothing that names a voter
        // The ballot survives a restart.
        FakeEnv  env2;
        PollHub  again(env2, path);
        QCOMPARE(again.view(samplePoll(3)).tally.voters, 1);
    }

    // Ballots only in the channel the poll was posted in (its marker), even from someone in our channel.
    void ballotsOnlyWhereThePollWasPosted()
    {
        FakeEnv env;
        env.where.insert(5, 12);
        PollHub hub(env, QString());
        hub.setAutoTimer(false);
        polls::Poll elsewhere = samplePoll(3);
        elsewhere.channelId   = 40; // posted in another channel; we are in 12
        hub.watch(elsewhere);
        const polls::Ballot b = polls::makeBallot(kKey, polls::newSecret(), 1, 1);
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeVotes(kKey, {b}, false).first()));
        QCOMPARE(hub.view(elsewhere).tally.voters, 0);
        // Nor answered for there.
        polls::Digest theirs;
        theirs.key  = kKey;
        theirs.hash = QByteArray("AAAAAAAA");
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
        QCOMPARE(hub.nextWakeMs(), qint64(-1));
        // Hostile: plenty of polls nobody showed us never push out the poll we voted on.
        hub.watch(samplePoll(3));
        QCOMPARE(hub.vote(1, samplePoll(3), 0x1), PollHub::VoteError::None);
        for (int i = 0; i < polls::kMaxPolls + 10; ++i) {
            const QString key = QStringLiteral("%1").arg(i + 1, 16, 16, QLatin1Char('0'));
            proto::Message pv = polls::makeVotes(key, {polls::makeBallot(key, polls::newSecret(), 1, 1)}, false).first();
            hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(pv));
        }
        QCOMPARE(hub.store().pollCount(), 1);
        QVERIFY(hub.store().own(kKey));
    }

    // The store keeps your own votes longest, and stays within its total size.
    void storeKeepsYourVotes()
    {
        polls::Store store;
        store.castOwn(kKey, 1, 1); // the oldest poll, but with your vote
        for (int i = 0; i < polls::kMaxPolls + 5; ++i) {
            const QString key = QStringLiteral("%1").arg(i + 1, 16, 16, QLatin1Char('0'));
            QCOMPARE(store.merge(key, polls::makeBallot(key, polls::newSecret(), 1, 1), 100 + i), polls::Store::Merge::Added);
        }
        QCOMPARE(store.pollCount(), polls::kMaxPolls);
        QVERIFY(store.own(kKey)); // the others went first
        QVERIFY(!store.has(QStringLiteral("0000000000000001")));
        // The total: a polls.json over it (an older version's) loads within it, the least recently active
        // polls gone; then a new poll's ballots push out the next oldest.
        const int  perPoll = 500;
        const int  count   = polls::kMaxStoredBallots / perPoll + 10;
        QJsonArray list;
        for (int i = 1; i <= count; ++i) {
            QJsonArray ballots;
            for (int k = 0; k < perPoll; ++k) {
                polls::Ballot b;
                b.id    = polls::newSecret();
                b.proof = polls::newSecret();
                b.step  = 1;
                b.mask  = 1;
                ballots.append(QString::fromLatin1(polls::entryText(b)));
            }
            list.append(QJsonObject{{QStringLiteral("k"), QStringLiteral("%1").arg(i, 16, 16, QLatin1Char('a'))}, {QStringLiteral("t"), 1000.0 + i}, {QStringLiteral("b"), ballots}});
        }
        polls::Store big;
        QVERIFY(big.fromJson(QJsonObject{{QStringLiteral("format"), QStringLiteral("tsmedia-polls")}, {QStringLiteral("version"), 1}, {QStringLiteral("polls"), list}}));
        QVERIFY(big.ballotCount() <= polls::kMaxStoredBallots);
        QVERIFY(big.ballotCount() > polls::kMaxStoredBallots - perPoll);
        QVERIFY(!big.has(QStringLiteral("%1").arg(1, 16, 16, QLatin1Char('a'))));
        QVERIFY(big.has(QStringLiteral("%1").arg(count, 16, 16, QLatin1Char('a'))));
        const QString last = QStringLiteral("%1").arg(count + 1, 16, 16, QLatin1Char('a'));
        for (int k = 0; k < perPoll; ++k)
            big.merge(last, polls::makeBallot(last, polls::newSecret(), 1, 1), 100000 + k);
        QVERIFY(big.ballotCount() <= polls::kMaxStoredBallots);
        QCOMPARE(big.ballots(last).size(), perPoll);
        // The count survives a JSON round trip.
        polls::Store copy;
        QVERIFY(copy.fromJson(big.toJson()));
        QCOMPARE(copy.ballotCount(), big.ballotCount());
    }

    // polls.json wasn't written after a vote (a crash): the state the others have, overheard, is your vote
    // again, and the next vote goes on from there (a step the others have is never used twice).
    void ownVoteLearnedBack()
    {
        polls::Store before;
        before.castOwn(kKey, 1, 1);
        const QJsonObject saved = before.toJson(); // written after the first vote only
        const polls::Ballot second = *before.castOwn(kKey, 2, 2);
        const polls::Ballot third  = *before.castOwn(kKey, 4, 3);
        polls::Store after;
        QVERIFY(after.fromJson(saved));
        QCOMPARE(after.own(kKey)->step, 1);
        QCOMPARE(after.merge(kKey, third, 4), polls::Store::Merge::Updated);
        QCOMPARE(after.own(kKey)->step, 3);
        QCOMPARE(after.own(kKey)->mask, quint16(4));
        QCOMPARE(after.merge(kKey, second, 5), polls::Store::Merge::Ignored); // older
        const std::optional<polls::Ballot> next = after.castOwn(kKey, 1, 6);
        QVERIFY(next);
        QCOMPARE(next->step, 4);
        QVERIFY(polls::follows(kKey, third, *next));
    }

    // Questions cost their asker one command each: the answers a connection sends are budgeted.
    void answersAreBudgeted()
    {
        FakeEnv env;
        env.where.insert(5, 12);
        PollHub::Limits limits;
        limits.answerBudget   = 4;
        limits.answerBudgetMs = 20000;
        limits.answerEveryMs  = 0;
        PollHub hub(env, QString(), limits);
        hub.setAutoTimer(false);
        QStringList keys;
        for (int i = 0; i < 6; ++i) {
            polls::Poll poll = samplePoll(3);
            poll.key         = QStringLiteral("%1").arg(i + 1, 16, 16, QLatin1Char('b'));
            keys << poll.key;
            hub.watch(poll);
            QVector<polls::Ballot> ballots;
            for (int k = 0; k < 20; ++k)
                ballots.append(polls::makeBallot(poll.key, polls::newSecret(), 1, 1));
            for (const proto::Message& m : polls::makeVotes(poll.key, ballots, false))
                hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(m));
        }
        QVector<polls::Digest> asked;
        for (const QString& key : keys) {
            polls::Digest d;
            d.key  = key;
            d.hash = QByteArray("AAAAAAAA");
            asked.append(d);
        }
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery(asked)));
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 4); // 6 polls of 3 commands each asked for; the budget is 4
        // It fills up again with time.
        env.now += 20000;
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery(asked)));
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 8);
    }

    void syncAsksOnce()
    {
        FakeEnv env;
        PollHub hub(env, QString());
        hub.setAutoTimer(false);
        hub.watch(samplePoll(3)); // a card of our channel's chat (ChatPolls::afterScan)
        hub.requestSync(1, {kKey, QStringLiteral("nonsense")});
        QCOMPARE(hub.nextWakeMs(), env.now + 1500);
        env.now += 1500;
        hub.tick();
        QCOMPARE(env.sent.size(), 1);
        QCOMPARE(env.sent.first().message.type, QByteArray("PQ"));
        const std::optional<QVector<polls::Digest>> q = polls::readQuery(env.sent.first().message);
        QVERIFY(q);
        QCOMPARE(q->size(), 1);
        hub.requestSync(1, {kKey}); // within 2 minutes: not again
        QCOMPARE(hub.nextWakeMs(), qint64(-1));
        env.now += 120000;
        env.others = false; // nobody with TS Media here: nobody asked
        hub.requestSync(1, {kKey});
        env.now += 1500;
        hub.tick();
        QCOMPARE(env.sent.size(), 1);
    }

    // Presence off ("Tell people in your channel that you have TS Media"): nothing goes out that the user
    // didn't do. No question when a poll shows, no answer to someone else's; the user's own vote does go.
    void quietWithoutPresence()
    {
        FakeEnv env;
        env.hidden = true;
        env.where.insert(5, 12);
        PollHub hub(env, QString());
        hub.setAutoTimer(false);
        const polls::Poll poll = samplePoll(3);
        hub.watch(poll);
        const polls::Ballot b = polls::makeBallot(kKey, polls::newSecret(), 1, 1);
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeVotes(kKey, {b}, false).first()));
        QCOMPARE(hub.view(poll).tally.voters, 1); // what comes in still counts
        hub.requestSync(1, {kKey});
        polls::Digest theirs;
        theirs.key  = kKey;
        theirs.hash = QByteArray("AAAAAAAA");
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 0);
        QCOMPARE(hub.vote(1, poll, 0x2), PollHub::VoteError::None);
        QCOMPARE(env.sent.size(), 1);
        QCOMPARE(env.sent.first().message.type, QByteArray("PV"));
        QVERIFY(!env.sent.first().message.has("y"));
    }

    // Answers and questions go to the channel we are in when they are sent: none for a poll of the channel
    // we left meanwhile (its ballots and its id would go to another channel).
    void answersStayInTheirChannel()
    {
        FakeEnv env;
        env.where.insert(5, 12);
        PollHub hub(env, QString());
        hub.setAutoTimer(false);
        hub.watch(samplePoll(3));
        const polls::Ballot b = polls::makeBallot(kKey, polls::newSecret(), 1, 1);
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeVotes(kKey, {b}, false).first()));
        polls::Digest theirs;
        theirs.key  = kKey;
        theirs.hash = QByteArray("AAAAAAAA");
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
        hub.requestSync(1, {kKey});
        QVERIFY(hub.nextWakeMs() > 0); // an answer and a question are due
        env.channel = 40;               // moved on before either went
        env.where.insert(5, 40);
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 0);
        // Back in the poll's channel: answered and asked again as usual.
        env.channel = 12;
        env.where.insert(5, 12);
        hub.received(1, 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), roundTrip(polls::makeQuery({theirs})));
        hub.requestSync(1, {kKey});
        env.now += 2000;
        hub.tick();
        QCOMPARE(env.sent.size(), 2);
    }

    // ---- the chat document -----------------------------------------------------------------------------------------
    void documentCollapseAndRestore()
    {
        for (const bool dark : {true, false}) {
            const tschat::Skin s = dark ? tschat::darkSkin() : tschat::lightSkin();
            QTextDocument      doc;
            tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: \"Home\"")));
            const polls::Poll poll = samplePoll(3);
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:17"), 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), pollHtml(s, poll)));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:30"), 6, QStringLiteral("BobUid="), QStringLiteral("Bob"), QStringLiteral("hi")));
            // A status line with a marker link is no poll.
            tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("16:01:00"), pollHtml(s, poll)));
            const QString before = doc.toHtml();
            const QString text   = doc.toPlainText();

            QVector<polldoc::Found> found = polldoc::scan(&doc);
            QCOMPARE(found.size(), 1);
            QCOMPARE(found.first().block, 1);
            QCOMPARE(found.first().poll.question, poll.question);
            QCOMPARE(found.first().poll.options, poll.options);
            QVERIFY(!found.first().collapsed);
            QVERIFY(polldoc::collapse(&doc, found.first(), polldoc::objectPrefix() + QStringLiteral("t1"), QSizeF(300, 200)));
            QVERIFY(!doc.findBlockByNumber(1).text().contains(QStringLiteral("Where do we eat")));
            QVERIFY(doc.findBlockByNumber(1).text().contains(QStringLiteral("Alice"))); // the header stays
            QCOMPARE(doc.blockCount(), 4);                                             // blocks are never added
            found = polldoc::scan(&doc);
            QCOMPARE(found.size(), 1);
            QVERIFY(found.first().collapsed);
            QCOMPARE(found.first().poll.options, poll.options);
            QCOMPARE(found.first().size, QSizeF(300, 200));
            polldoc::resizeObject(&doc, found.first().objectPosition, QSizeF(320, 240));
            QCOMPARE(polldoc::scan(&doc).first().size, QSizeF(320, 240));
            // The card reads as the poll's question (a reply's quote) and copies as the poll's text.
            QCOMPARE(replydoc::parseBlock(doc.findBlockByNumber(1)).text, QStringLiteral("\U0001F4CA Poll: Where do we eat?"));
            const QTextBlock pollBlock = doc.findBlockByNumber(1);
            const QString    copied    = ChatEmoji::originalText(&doc, pollBlock.position(), pollBlock.position() + pollBlock.length() - 1);
            QVERIFY2(copied.contains(QStringLiteral("\"Alice\": \U0001F4CA Poll: Where do we eat?\n1. Option 1\n2. Option 2\n3. Option 3\nVote with TS Media chat")), qPrintable(copied));
            QCOMPARE(polldoc::restoreAll(&doc), 1);
            QCOMPARE(doc.toPlainText(), text);
            QVERIFY(polldoc::objectPositions(&doc).isEmpty());
            QCOMPARE(polldoc::scan(&doc).size(), 1);
            Q_UNUSED(before)
        }
    }

    // TS Media's own chat layout (Cozy and Compact): the poll is found in a styled block, collapsed under the
    // layout or before it, and everything comes back in the unload order (the layout first, then the polls).
    void documentInTsMediaLayout()
    {
        for (const layoutart::Mode mode : {layoutart::Mode::Cozy, layoutart::Mode::Compact}) {
            for (const bool pollsFirst : {false, true}) {
                const tschat::Skin s = tschat::darkSkin();
                QTextDocument      doc;
                QFont              font(QStringLiteral("Segoe UI"));
                font.setPointSizeF(9);
                doc.setDefaultFont(font);
                doc.setTextWidth(520);
                const polls::Poll poll = samplePoll(4, true);
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:02"), 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), QStringLiteral("lunch?")));
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:17"), 6, QStringLiteral("BobUid="), QStringLiteral("Bob"), pollHtml(s, poll)));
                tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:30"), 6, QStringLiteral("BobUid="), QStringLiteral("Bob"), QStringLiteral("ok")));
                const QString text = doc.toPlainText();
                layoutdoc::Env env;
                env.mode         = mode;
                env.tokens       = layoutart::tokensFor(mode, font, false);
                env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
                env.contentWidth = 500;
                env.today        = QDate(2026, 10, 10);
                env.tag          = QStringLiteral("t");
                if (pollsFirst) {
                    const QVector<polldoc::Found> found = polldoc::scan(&doc);
                    QCOMPARE(found.size(), 1);
                    QVERIFY(polldoc::collapse(&doc, found.first(), polldoc::objectPrefix() + QStringLiteral("p1"), QSizeF(300, 180)));
                    QVERIFY(layoutdoc::styleAll(&doc, env) > 0);
                } else {
                    QVERIFY(layoutdoc::styleAll(&doc, env) > 0);
                    const QVector<polldoc::Found> found = polldoc::scan(&doc);
                    QCOMPARE(found.size(), 1);
                    QCOMPARE(found.first().poll.options.size(), 4);
                    QVERIFY(found.first().poll.multiple);
                    QVERIFY(polldoc::collapse(&doc, found.first(), polldoc::objectPrefix() + QStringLiteral("p1"), QSizeF(300, 180)));
                }
                QCOMPARE(polldoc::scan(&doc).size(), 1);
                QVERIFY(polldoc::scan(&doc).first().collapsed);
                QCOMPARE(doc.blockCount(), 3);
                // Our line break only where the card would share the head's line: a Compact head, not Cozy.
                polldoc::normalizeAll(&doc);
                QCOMPARE(polldoc::normalizeAll(&doc), 0); // settled
                auto ourBreak = [&doc] {
                    const int   object = polldoc::scan(&doc).first().objectPosition;
                    QTextCursor c(&doc);
                    c.setPosition(object - 1);
                    c.setPosition(object, QTextCursor::KeepAnchor);
                    return c.charFormat().boolProperty(polldoc::kSeparatorProperty);
                };
                QCOMPARE(ourBreak(), mode == layoutart::Mode::Compact);
                layoutdoc::restoreAll(&doc);
                polldoc::normalizeAll(&doc); // TeamSpeak's look again: the line break is back
                QVERIFY(ourBreak());
                QCOMPARE(polldoc::restoreAll(&doc), 1);
                QCOMPARE(doc.toPlainText(), text);
            }
        }
    }

    // A poll whose text doesn't match its marker (edited by hand) stays text.
    void documentRejectsMismatch()
    {
        const tschat::Skin s = tschat::darkSkin();
        QTextDocument      doc;
        polls::Poll        poll = samplePoll(3);
        QString            html = pollHtml(s, poll);
        html.replace(QStringLiteral("<br>3. Option 3"), QString());
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("16:00:17"), 5, QStringLiteral("AliceUid="), QStringLiteral("Alice"), html));
        QVERIFY(polldoc::scan(&doc).isEmpty());
        // A list that would make an invalid poll.
        QVERIFY(polldoc::pollFromList({kKey, QStringLiteral("q"), QStringLiteral("s"), QStringLiteral("12"), QStringLiteral("only one")}).key.isEmpty());
        QVERIFY(!polldoc::pollFromList(polldoc::pollToList(poll)).key.isEmpty());
    }

    // ---- the card ---------------------------------------------------------------------------------------------------
    void cardLayoutAndHits()
    {
        pollart::Style style;
        style.font = QApplication::font();
        style.font.setPointSizeF(9);
        style.maxWidth = 600;
        pollart::Card card;
        card.question = QStringLiteral("Where do we eat?");
        card.options  = QStringList{QStringLiteral("Pizza"), QStringLiteral("Sushi"), QStringLiteral("Tacos")};
        const pollart::Layout open = pollart::layout(card, style);
        QCOMPARE(open.size.width(), 400); // at most 400
        QCOMPARE(open.rows.size(), 3);
        QCOMPARE(open.actionZone, pollart::Zone::None);
        pollart::Hit hit = pollart::hitTest(card, open, open.rows.at(1).center());
        QCOMPARE(hit.zone, pollart::Zone::Option);
        QCOMPARE(hit.index, 1);
        QCOMPARE(pollart::hitTest(card, open, open.badge.center()).zone, pollart::Zone::Badge);
        // Can't vote here: no option is a button, the note says why.
        pollart::Card elsewhere = card;
        elsewhere.canVote       = false;
        elsewhere.note          = QStringLiteral("Join the channel this poll was posted in to vote.");
        const pollart::Layout away = pollart::layout(elsewhere, style);
        QVERIFY(!away.note.isEmpty());
        QCOMPARE(pollart::hitTest(elsewhere, away, away.rows.at(1).center()).zone, pollart::Zone::None);
        // Multiple answers: the Vote button, off until something is ticked.
        pollart::Card multi = card;
        multi.multiple      = true;
        pollart::Layout ml  = pollart::layout(multi, style);
        QCOMPARE(ml.actionZone, pollart::Zone::Submit);
        QCOMPARE(pollart::hitTest(multi, ml, ml.action.center()).zone, pollart::Zone::None);
        multi.selected = 0x4;
        QCOMPARE(pollart::hitTest(multi, ml, ml.action.center()).zone, pollart::Zone::Submit);
        // Results: taller, Retract vote, options no longer buttons.
        pollart::Card voted = card;
        voted.results       = true;
        voted.counts        = {72, 25, 28};
        voted.percents      = {58, 20, 22};
        voted.voters        = 125;
        voted.own           = 1;
        const pollart::Layout rl = pollart::layout(voted, style);
        QVERIFY(rl.size.height() > open.size.height());
        QCOMPARE(rl.actionZone, pollart::Zone::Retract);
        QCOMPARE(pollart::hitTest(voted, rl, rl.action.center()).zone, pollart::Zone::Retract);
        QCOMPARE(pollart::hitTest(voted, rl, rl.rows.at(0).center()).zone, pollart::Zone::None);
        voted.sending = true; // no Retract while it goes out
        QCOMPARE(pollart::layout(voted, style).actionZone, pollart::Zone::None);
        voted.sending = false;
        voted.failed  = true;
        const pollart::Layout fl = pollart::layout(voted, style);
        QCOMPARE(fl.actionZone, pollart::Zone::Retry);
        QVERIFY(!fl.note.isEmpty());
        // A narrow chat: narrower card, long texts wrap (taller), never wider than the room.
        style.maxWidth = 200;
        pollart::Card longer = card;
        longer.question      = QString(180, QLatin1Char('w')).replace(QLatin1String("wwwww"), QLatin1String("word "));
        const pollart::Layout narrow = pollart::layout(longer, style);
        QVERIFY(narrow.size.width() <= 200);
        QVERIFY(narrow.question.height() > 3 * QFontMetricsF(style.font).height());
        // Texts.
        QCOMPARE(pollart::votesText(0), QStringLiteral("No votes yet"));
        QCOMPARE(pollart::votesText(1), QStringLiteral("1 vote"));
        QCOMPARE(pollart::votesText(1250), QStringLiteral("1,250 votes"));
        QVERIFY(pollart::describe(voted).contains(QStringLiteral("(your vote)")));
        // It draws, light and dark, at 1x and 2x.
        for (const bool dark : {false, true}) {
            for (const qreal dpr : {1.0, 2.0}) {
                style.dark = dark;
                style.dpr  = dpr;
                const QImage img = pollart::render(voted, style, pollart::Pointer());
                QVERIFY(!img.isNull());
                QCOMPARE(img.width(), qCeil(pollart::layout(voted, style).size.width() * dpr));
            }
        }
    }

    // ---- the window -----------------------------------------------------------------------------------------------------
    void newPollWindow()
    {
        auto* dialog = new PollDialog(QStringLiteral("the channel \u201cHome\u201d"));
        QCOMPARE(dialog->objectName(), QStringLiteral("tsmediaPollDialog"));
        QCOMPARE(dialog->optionFields().size(), 2);
        QVERIFY(!dialog->multipleSwitch()->isChecked()); // off by default
        QCOMPARE(dialog->questionField()->maxLength(), polls::kMaxQuestionChars);
        QCOMPARE(dialog->optionFields().first()->maxLength(), polls::kMaxOptionChars);
        // Cancel and Post poll don't touch (the window's own layout has no spacing to inherit).
        dialog->layout()->activate();
        QPushButton* cancel = nullptr;
        for (QPushButton* b : dialog->findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Cancel"))
                cancel = b;
        }
        QVERIFY(cancel);
        QVERIFY(dialog->postButton()->geometry().left() - cancel->geometry().right() >= 8);
        // An option added later has the muted placeholder like the first ones.
        dialog->addOption(QString(), false);
        QCOMPARE(dialog->optionFields().last()->palette().color(QPalette::PlaceholderText), dialog->optionFields().first()->palette().color(QPalette::PlaceholderText));
        dialog->removeOption(2);
        // Nothing yet: why not, under the fields.
        QVERIFY(!dialog->tryPost());
        QVERIFY(!dialog->errorText().isEmpty());
        dialog->questionField()->setText(QStringLiteral("  Lunch?  "));
        dialog->optionFields().at(0)->setText(QStringLiteral("Pizza"));
        QVERIFY(!dialog->tryPost()); // one option
        dialog->optionFields().at(1)->setText(QStringLiteral("pizza"));
        QVERIFY(!dialog->tryPost()); // the same twice
        dialog->optionFields().at(1)->setText(QStringLiteral("Sushi"));
        for (int i = 0; i < 12; ++i)
            dialog->addOption(QStringLiteral("More %1").arg(i), false);
        QCOMPARE(dialog->optionFields().size(), polls::kMaxOptions);
        QVERIFY(!dialog->addButton()->isVisibleTo(dialog));
        dialog->removeOption(9);
        QCOMPARE(dialog->optionFields().size(), polls::kMaxOptions - 1);
        while (dialog->optionFields().size() > 2)
            dialog->removeOption(2);
        dialog->removeOption(0); // never fewer than two
        QCOMPARE(dialog->optionFields().size(), 2);
        // Reorder: by the method and with Alt+Down in the field.
        dialog->moveOption(0, 1);
        QCOMPARE(dialog->optionFields().at(0)->text(), QStringLiteral("Sushi"));
        QKeyEvent altDown(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
        QApplication::sendEvent(dialog->optionFields().at(0), &altDown);
        QCOMPARE(dialog->optionFields().at(0)->text(), QStringLiteral("Pizza"));
        // Enter in the last option adds one; Backspace in an empty one removes it.
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(dialog->optionFields().at(1), &enter);
        QCOMPARE(dialog->optionFields().size(), 3);
        QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(dialog->optionFields().at(2), &back);
        QCOMPARE(dialog->optionFields().size(), 2);
        dialog->multipleSwitch()->setChecked(true);
        // The validator (the message's size) is asked last.
        dialog->setValidator([](const PollDialog::Result& r) { return r.options.size() == 2 ? QStringLiteral("Too long") : QString(); });
        QVERIFY(!dialog->tryPost());
        QCOMPARE(dialog->errorText(), QStringLiteral("Too long"));
        dialog->setValidator(nullptr);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false); // read after it was accepted
        QVERIFY(dialog->tryPost());
        const PollDialog::Result r = dialog->result();
        QCOMPARE(r.question, QStringLiteral("Lunch?"));
        QCOMPARE(r.options, (QStringList{QStringLiteral("Pizza"), QStringLiteral("Sushi")}));
        QVERIFY(r.multiple);
        delete dialog;
    }

    // 2.3 ux review: the Vote button's text reads (4.5:1) on its fill, at rest, hovered and pressed, in light
    // and dark, plain and on a bubble (dark skins: white on the light fill was 3.2:1, 2.1:1 on a bubble), and
    // that fill is what the card paints.
    void voteButtonReadable()
    {
        QStringList low;
        for (const bool dark : {false, true}) {
            const layoutart::Colors colors = layoutart::colorsFor(dark, dark ? QColor(0x36, 0x39, 0x3e) : QColor(Qt::white), dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black),
                                                                  dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
            for (const bool bubble : {false, true}) {
                pollart::Style style;
                style.dark = dark;
                style.font = QApplication::font();
                style.font.setPointSizeF(9);
                style.base = colors.base;
                style.text = colors.text;
                style.dpr  = 2.0;
                if (bubble)
                    style.bubbles = layoutart::bubbleSurfaces(colors);
                pollart::Card card;
                card.question = QStringLiteral("Which days?");
                card.options  = QStringList{QStringLiteral("Friday"), QStringLiteral("Saturday")};
                card.multiple = true;
                card.selected = 0x1;
                for (const int state : {0, 1, 2}) {
                    pollart::Pointer pointer;
                    if (state == 1)
                        pointer.hover = pollart::Zone::Submit;
                    if (state == 2)
                        pointer.pressed = pollart::Zone::Submit;
                    const pollart::ButtonColors button = pollart::voteButtonColors(style, state == 1, state == 2);
                    const QString what = QStringLiteral("%1 %2 state %3").arg(dark ? "dark" : "light", bubble ? "bubble" : "plain").arg(state);
                    const double ratio = ui::contrastRatio(button.text, button.fill);
                    if (ratio < 4.5)
                        low << QStringLiteral("%1: %2").arg(what).arg(ratio);
                    // The card paints that fill (left of the text, where the pill is full height).
                    pollart::Layout l;
                    const QImage    img   = pollart::render(card, style, pointer, &l);
                    const QPointF   at    = QPointF(l.action.left() + l.action.height() / 2.0, l.action.center().y()) * 2.0;
                    const QColor    shown = ui::flatten(img.pixelColor(at.toPoint()), bubble ? style.bubbles.first() : colors.base);
                    if (qAbs(shown.red() - button.fill.red()) > 2 || qAbs(shown.green() - button.fill.green()) > 2 || qAbs(shown.blue() - button.fill.blue()) > 2)
                        low << QStringLiteral("%1: painted %2, not %3").arg(what, shown.name(), button.fill.name());
                }
            }
        }
        QVERIFY2(low.isEmpty(), qPrintable(low.join(QStringLiteral("; "))));
    }

    // 2.3 ux review: in TeamSpeak's skins (they colour windows by style sheet, not by palette) the New poll
    // window takes the skin's colours, and it is as tall as its content (fields not squeezed), also after an
    // option is added, as far as the screen has room.
    void newPollWindowFollowsTheSkin()
    {
        QStringList problems;
        for (const bool dark : {false, true}) {
            qApp->setStyleSheet(dark ? QString::fromLatin1("QMainWindow, QWidget, QDialog { background: #36393E; color: #dcddde; }") : QString());
            auto* dialog = new PollDialog(QStringLiteral("the channel \u201cHome\u201d"));
            dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            dialog->setAttribute(Qt::WA_DontShowOnScreen);
            dialog->show();
            QCoreApplication::processEvents();
            const QString skin = dark ? QStringLiteral("dark skin: ") : QStringLiteral("light skin: ");
            const int     room = dialog->screen()->availableGeometry().height() - 48;
            const auto    fits = [&](const char* when) {
                const int content = dialog->layout()->totalHeightForWidth(dialog->width());
                if (dialog->height() < qMin(content, room))
                    problems << skin + QStringLiteral("%1: height %2 < %3").arg(QString::fromLatin1(when)).arg(dialog->height()).arg(qMin(content, room));
                if (content > room)
                    return; // a small screen: squeezed, nothing else to do
                QList<QLineEdit*> fields = dialog->optionFields();
                fields << dialog->questionField();
                for (QLineEdit* field : qAsConst(fields)) {
                    if (field->height() < field->sizeHint().height())
                        problems << skin + QStringLiteral("%1: field %2 < %3").arg(QString::fromLatin1(when)).arg(field->height()).arg(field->sizeHint().height());
                }
            };
            if ((dialog->palette().color(QPalette::Window).lightness() < 128) != dark)
                problems << skin + QStringLiteral("window %1").arg(dialog->palette().color(QPalette::Window).name());
            fits("opened");
            // The options' rows show the window's colour (no band of the skin's grey behind them).
            const QImage shot  = dialog->grab().toImage();
            const QPoint gap   = dialog->optionFields().at(1)->mapTo(dialog, QPoint(dialog->optionFields().at(1)->width() / 2, -3)); // between two rows
            const QColor shown = shot.pixelColor(gap * shot.devicePixelRatio());
            if (ui::contrastRatio(shown, dialog->palette().color(QPalette::Window)) >= 1.1)
                problems << skin + QStringLiteral("%1 behind the options on %2").arg(shown.name(), dialog->palette().color(QPalette::Window).name());
            const int before = dialog->height();
            dialog->addOption(QStringLiteral("Tacos"), false);
            QCoreApplication::processEvents();
            if (dialog->height() <= before && before < room)
                problems << skin + QStringLiteral("didn't grow for an option (%1)").arg(before);
            fits("an option added");
            // Each remove button says which option it removes (screen readers).
            QStringList removeNames;
            for (QAbstractButton* b : dialog->findChildren<QAbstractButton*>()) {
                if (b->toolTip() == QStringLiteral("Remove this option"))
                    removeNames << b->accessibleName();
            }
            removeNames.sort();
            if (removeNames != QStringList{QStringLiteral("Remove option 1"), QStringLiteral("Remove option 2"), QStringLiteral("Remove option 3")})
                problems << skin + removeNames.join(QStringLiteral(", "));
            delete dialog;
        }
        qApp->setStyleSheet(QString());
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));
    }
};

TSMEDIA_REGISTER_TEST(TestPolls)
#include "tst_polls.moc"
