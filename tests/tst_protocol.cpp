// Unit tests for the 2.2 plugin-to-plugin protocol: the tsm1 codec (hostile inputs), the PluginLink
// transport, the PeerDirectory presence state machine on a fake clock, the presence texts, the
// ReactionStore merge rules and reactions.json, and the reaction row layout and colours.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

#include "floodgovernor.h"
#include "peerprotocol.h"
#include "peers.h"
#include "pluginlink.h"
#include "reactionart.h"
#include "reactions.h"
#include "testmain.h"
#include "uiutil.h"

namespace {

const QString kKeyA = QStringLiteral("3f9a1c2e4b5d6e7f8a9b");
const QString kKeyB = QStringLiteral("0a1b2c3d4e5f60718293");
const QString kKeyC = QStringLiteral("aaaaaaaaaaaaaaaaaaaa");
const QString kUidSara = QStringLiteral("q0Xn6Sara0000000000000000000=");
const QString kUidReza = QStringLiteral("q0Xn6Reza0000000000000000000=");
const QString kUidMe   = QStringLiteral("q0Xn6Mine0000000000000000000=");

// Element by element: QVector's operator== instantiates MSVC's deprecated checked iterators (C4996).
template <typename T>
bool sameItems(const QVector<T>& list, std::initializer_list<T> expected)
{
    if (list.size() != static_cast<int>(expected.size()))
        return false;
    int i = 0;
    for (const T& value : expected) {
        if (!(list.at(i++) == value))
            return false;
    }
    return true;
}

QString keyNumber(int n)
{
    return QStringLiteral("%1").arg(n, 20, 16, QLatin1Char('0'));
}

// ---- fake TeamSpeak for the directory ------------------------------------------------------------

struct FakeClient {
    QString uid;
    QString name;
    quint64 channel = 0;
    bool    query   = false;
};

class FakeEnv : public peers::Env
{
  public:
    qint64                        now    = 100000;
    quint16                       own    = 1;
    int                           jitter = 300;
    QHash<quint16, FakeClient>    clients;

    FakeEnv() { clients.insert(own, {kUidMe, QStringLiteral("Me"), 10, false}); }

    void add(quint16 id, const QString& uid, const QString& name, quint64 channel, bool query = false) { clients.insert(id, {uid, name, channel, query}); }

    qint64           nowMs() const override { return now; }
    peers::ClientId  ownClientId(quint64) const override { return own; }
    quint64          ownChannel(quint64) const override { return clients.value(own).channel; }
    quint64          channelOf(quint64, peers::ClientId c) const override { return clients.value(c).channel; }
    QString          uid(quint64, peers::ClientId c) const override { return clients.value(c).uid; }
    QString          nickname(quint64, peers::ClientId c) const override { return clients.value(c).name; }
    bool             isQueryClient(quint64, peers::ClientId c) const override { return clients.value(c).query; }
    int              randomBetween(int low, int high) override { return qBound(low, jitter, high); }
    QVector<peers::ClientId> channelClients(quint64, quint64 channel) const override
    {
        QVector<peers::ClientId> list;
        for (auto it = clients.constBegin(); it != clients.constEnd(); ++it) {
            if (it->channel == channel)
                list.append(it.key());
        }
        std::sort(list.begin(), list.end());
        return list;
    }
};

class FakeSender : public peers::Sender
{
  public:
    struct Sent {
        quint64        sch = 0;
        proto::Message message;
        peers::Target  target;
    };
    QVector<Sent>     sent;
    bool              isBlocked = false;
    peers::SendResult result    = peers::SendResult::Ok;

    void send(quint64 sch, const proto::Message& message, const peers::Target& target, Done done) override
    {
        sent.append({sch, message, target});
        if (done)
            done(result);
    }
    bool blocked(quint64) const override { return isBlocked; }

    int count(const char* type) const
    {
        int n = 0;
        for (const Sent& s : sent)
            n += s.message.type == type ? 1 : 0;
        return n;
    }
    const Sent* last(const char* type) const
    {
        for (int i = sent.size() - 1; i >= 0; --i) {
            if (sent.at(i).message.type == type)
                return &sent.at(i);
        }
        return nullptr;
    }
};

// Runs the directory's clock forward to now + ms, waking it whenever it asked to be.
void advance(FakeEnv& env, peers::PeerDirectory& dir, qint64 ms)
{
    const qint64 target = env.now + ms;
    for (int guard = 0; guard < 1000; ++guard) {
        const qint64 next = dir.nextWakeMs();
        if (next < 0 || next > target)
            break;
        env.now = qMax(env.now, next);
        dir.tick();
    }
    env.now = target;
    dir.tick();
}

proto::Message hello(bool rr = true, const char* caps = "p,r")
{
    proto::Message m = proto::makeHello(QStringLiteral("2.2.0"), QString::fromLatin1(caps).split(QLatin1Char(',')), rr);
    return m;
}

constexpr int kModeClient  = 1;
constexpr int kModeChannel = 2;
constexpr int kModeServer  = 3;

// ---- fake TeamSpeak + Core for the link ----------------------------------------------------------

class FakeBackend : public PluginLink::Backend
{
  public:
    struct Command {
        quint64       sch;
        QByteArray    payload;
        peers::Target target;
        QString       rc;
    };
    qint64             now = 50000;
    FloodGovernor      flood;
    bool               connected = true;
    QSet<quint16>      visible{2, 3, 4, 5};
    QVector<Command>   commands;
    int                floodChanges = 0;
    int                nextCode     = 0;

    qint64         nowMs() const override { return now; }
    FloodGovernor* governor(quint64) override { return connected ? &flood : nullptr; }
    void           floodStateChanged(quint64) override { ++floodChanges; }
    QString        newReturnCode() override { return QStringLiteral("rc-test-%1").arg(++nextCode); }
    void sendCommand(quint64 sch, const QByteArray& payload, const peers::Target& target, const QString& rc) override { commands.append({sch, payload, target, rc}); }
    bool    isConnected(quint64) const override { return connected; }
    bool    clientVisible(quint64, quint16 client) const override { return visible.contains(client); }
    quint16 ownClientId(quint64) const override { return 1; }
};

// The S0 numbers (FloodGovernor's defaults): from rest 24 plugin commands back to back, then one a second.
FloodGovernor::Limits roomyLimits()
{
    return FloodGovernor::Limits();
}

qreal fakeMeasure(const QString& text, bool bold)
{
    return text.size() * (bold ? 8.0 : 7.0);
}

// 2.2 emoji: the emoji of a v1 reaction.
int E(int index)
{
    return proto::legacyReactionEmoji(index);
}

// A view of v1 reactions (index, count) in their display order.
ReactionView viewWith(std::initializer_list<std::pair<int, int>> counts, int mine = -1)
{
    QVector<std::pair<int, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end());
    ReactionView v;
    for (const auto& c : sorted) {
        ReactionView::Entry e;
        e.reaction = E(c.first);
        e.count    = c.second;
        e.mine     = c.first == mine;
        v.entries.append(e);
    }
    return v;
}

} // namespace

class TestProtocol : public QObject
{
    Q_OBJECT

  private slots:
    // codec
    void parseValidMessages();
    void parseRejectsHostileInput_data();
    void parseRejectsHostileInput();
    void looksLikeOursChecks();
    void helloFields();
    void reactValidation_data();
    void reactValidation();
    void syncValidation();
    void serializeRoundTrip();
    void serializeRefusesBadMessages();
    void reactPacking();
    void reactionCodes();

    // transport
    void linkSendsAndAnswers();
    void linkTimeoutIsNotSent();
    void linkFloodPausesAndRetriesPresence();
    void linkFiltersClientTargets();
    void linkMergesAnswersAndCoalesces();
    void linkBlocksOnPermissionError();
    void linkSpendsPluginBucket();
    void linkInboundRules();
    void linkRetryHint();

    // presence
    void helloAfterSettle();
    void channelHoppingSaysHelloOnce();
    void answersAndTimeouts();
    void joinerGetsLongerTimeout();
    void answersMergeAndRateLimit();
    void byeAndDisconnect();
    void presenceOffSendsNothing();
    void partnerCheck();
    void queryClientsAndClonesExcluded();
    void syncDueNeedsReactionPeer();
    void blockedHidesPresence();
    void floodedHelloIsRetried();
    void presenceOffDropsHeldHello();
    void presenceTexts();
    void presenceNames();

    // reactions
    void storeMergeRules();
    void storeReactorCap();
    void storeOrphans();
    void storeViewAndSync();
    void storePersistence();
    void storeHostileFile();
    void storeLimitsOnLoad();
    void storeSaveFitsLoadLimit();

    // reaction row
    void rowLayout();
    void rowZones();
    void rowColours();
};

// ---- codec ----------------------------------------------------------------------------------------

void TestProtocol::parseValidMessages()
{
    proto::ParseResult r;
    auto m = proto::parse("tsm1 HELLO pv=1 v=2.2.0 caps=p,r rr=1", &r);
    QVERIFY(m);
    QCOMPARE(r, proto::ParseResult::Ok);
    QCOMPARE(m->type, QByteArray("HELLO"));
    QCOMPARE(m->value("v"), QByteArray("2.2.0"));
    QCOMPARE(m->fields.size(), 4);

    // Fields in any order, empty values.
    m = proto::parse("tsm1 R i=3f9a1c2e4b5d6e7f8a9b: s=p");
    QVERIFY(m);
    QCOMPARE(m->value("s"), QByteArray("p"));

    QVERIFY(proto::parse("tsm1 BYE"));
    QVERIFY(proto::parse("tsm1 SYNC s=c m=3f9a1c2e4b5d6e7f8a9b,0a1b2c3d4e5f60718293"));
    // Unknown types and fields parse; the typed readers ignore what they don't know.
    m = proto::parse("tsm1 FUTURE x=1 yy=a_b-c.d:e");
    QVERIFY(m);
    QVERIFY(!proto::readHello(*m));
    QVERIFY(!proto::readReact(*m));
    m = proto::parse("tsm1 HELLO pv=1 future=yes");
    QVERIFY(m && proto::readHello(*m));
}

void TestProtocol::parseRejectsHostileInput_data()
{
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<int>("result");
    const int malformed = static_cast<int>(proto::ParseResult::Malformed);
    QTest::newRow("empty") << QByteArray() << static_cast<int>(proto::ParseResult::Empty);
    QTest::newRow("too long") << (QByteArray("tsm1 HELLO x=") + QByteArray(2040, 'a')) << static_cast<int>(proto::ParseResult::TooLong);
    QTest::newRow("other magic") << QByteArray("xyz1 HELLO") << static_cast<int>(proto::ParseResult::NotOurs);
    QTest::newRow("plain text") << QByteArray("hello there") << static_cast<int>(proto::ParseResult::NotOurs);
    QTest::newRow("magic prefix only") << QByteArray("tsm1x HELLO") << static_cast<int>(proto::ParseResult::NotOurs);
    QTest::newRow("newer version") << QByteArray("tsm2 HELLO pv=2") << static_cast<int>(proto::ParseResult::NewerVersion);
    QTest::newRow("leading space") << QByteArray(" tsm1 BYE") << static_cast<int>(proto::ParseResult::NotOurs);
    QTest::newRow("trailing space") << QByteArray("tsm1 BYE ") << malformed;
    QTest::newRow("double space") << QByteArray("tsm1  BYE") << malformed;
    QTest::newRow("tab") << QByteArray("tsm1\tBYE") << static_cast<int>(proto::ParseResult::NotOurs);
    QTest::newRow("tab inside") << QByteArray("tsm1 HELLO pv=1\trr=1") << malformed;
    QTest::newRow("control byte") << QByteArray("tsm1 HELLO v=\x01") << malformed;
    QTest::newRow("NUL") << QByteArray("tsm1 HELLO v=a\0b", 16) << malformed;
    QTest::newRow("newline") << QByteArray("tsm1 HELLO\nv=1") << malformed;
    QTest::newRow("non-ascii") << QByteArray("tsm1 HELLO v=\xd0\xb6") << malformed;
    QTest::newRow("no type") << QByteArray("tsm1") << malformed;
    QTest::newRow("lower type") << QByteArray("tsm1 hello") << malformed;
    QTest::newRow("long type") << QByteArray("tsm1 ABCDEFGHI") << malformed;
    QTest::newRow("type digits") << QByteArray("tsm1 R2") << malformed;
    QTest::newRow("no equals") << QByteArray("tsm1 HELLO pv") << malformed;
    QTest::newRow("empty key") << QByteArray("tsm1 HELLO =1") << malformed;
    QTest::newRow("upper key") << QByteArray("tsm1 HELLO PV=1") << malformed;
    QTest::newRow("long key") << QByteArray("tsm1 HELLO abcdefghi=1") << malformed;
    QTest::newRow("duplicate key") << QByteArray("tsm1 HELLO pv=1 pv=2") << malformed;
    QTest::newRow("pipe") << QByteArray("tsm1 HELLO v=a|b") << malformed;
    QTest::newRow("backslash") << QByteArray("tsm1 HELLO v=a\\sb") << malformed;
    QTest::newRow("quote") << QByteArray("tsm1 HELLO v=\"x\"") << malformed;
    QTest::newRow("equals in value") << QByteArray("tsm1 HELLO v=a=b") << malformed;
    QTest::newRow("slash") << QByteArray("tsm1 HELLO v=a/b") << malformed;
    QTest::newRow("bracket") << QByteArray("tsm1 HELLO v=[b]") << malformed;
    QTest::newRow("value too long") << (QByteArray("tsm1 HELLO v=") + QByteArray(513, 'a')) << malformed;
    QByteArray many = "tsm1 X";
    for (int i = 0; i < 31; ++i)
        many += " k" + QByteArray(1, static_cast<char>('a' + i % 26)) + QByteArray(i / 26 + 1, 'z') + "=1";
    QTest::newRow("too many tokens") << many << malformed;
}

void TestProtocol::parseRejectsHostileInput()
{
    QFETCH(QByteArray, raw);
    QFETCH(int, result);
    proto::ParseResult r = proto::ParseResult::Ok;
    QVERIFY(!proto::parse(raw, &r));
    QCOMPARE(static_cast<int>(r), result);
}

void TestProtocol::looksLikeOursChecks()
{
    QVERIFY(!proto::looksLikeOurs(nullptr));
    QVERIFY(!proto::looksLikeOurs(""));
    QVERIFY(!proto::looksLikeOurs("tsm"));
    QVERIFY(proto::looksLikeOurs("tsm1"));
    QVERIFY(proto::looksLikeOurs("tsm1 HELLO pv=1"));
    QVERIFY(!proto::looksLikeOurs("tsm1x HELLO"));
    QVERIFY(!proto::looksLikeOurs("tsm2 HELLO"));
    QVERIFY(!proto::looksLikeOurs("some other plugin's data"));
    const QByteArray longOne = "tsm1 HELLO v=" + QByteArray(3000, 'a');
    QVERIFY(!proto::looksLikeOurs(longOne.constData()));
    const QByteArray edge = "tsm1 X v=" + QByteArray(proto::kMaxRecvBytes - 9, 'a');
    QCOMPARE(edge.size(), proto::kMaxRecvBytes);
    QVERIFY(proto::looksLikeOurs(edge.constData()));
}

void TestProtocol::helloFields()
{
    auto read = [](const char* raw) { return proto::readHello(*proto::parse(raw)); };
    auto h    = read("tsm1 HELLO pv=1 v=2.2.0 caps=p,r rr=1");
    QVERIFY(h);
    QVERIFY(!h->hi);
    QVERIFY(h->replyRequested);
    QCOMPARE(h->version, QStringLiteral("2.2.0"));
    QVERIFY(h->hasCap("p") && h->hasCap("r"));

    QVERIFY(!read("tsm1 HELLO v=2.2.0"));          // pv is required
    QVERIFY(!read("tsm1 HELLO pv=2"));             // no common protocol
    QVERIFY(!read("tsm1 HELLO pv=12"));            // one digit each
    QVERIFY(!read("tsm1 HELLO pv=1,"));            // empty entry
    QVERIFY(read("tsm1 HELLO pv=2,1"));            // a newer client that still speaks 1
    QVERIFY(read("tsm1 HELLO pv=1 v=1.2.3.4.5")->version.isEmpty()); // malformed version: not shown
    QVERIFY(read("tsm1 HELLO pv=1 v=9999.1")->version.isEmpty());
    // Malformed capabilities are dropped one by one; unknown ones are kept (and ignored by users).
    h = read("tsm1 HELLO pv=1 caps=p,,toolong,R,zz,r");
    QCOMPARE(h->caps, QStringList({QStringLiteral("p"), QStringLiteral("zz"), QStringLiteral("r")}));
    // rr means nothing on an answer.
    h = read("tsm1 HI pv=1 rr=1");
    QVERIFY(h && h->hi && !h->replyRequested);
    QVERIFY(!read("tsm1 HELLO pv=1 rr=yes")->replyRequested);
}

void TestProtocol::reactValidation_data()
{
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<bool>("valid");
    QTest::newRow("one") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:up.heart") << true;
    QTest::newRow("removal") << QByteArray("tsm1 R s=p i=3f9a1c2e4b5d6e7f8a9b:") << true;
    QTest::newRow("sync answer") << QByteArray("tsm1 R s=c y=1 i=3f9a1c2e4b5d6e7f8a9b:fire,0a1b2c3d4e5f60718293:lol") << true;
    QTest::newRow("unknown code kept out") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:up.party") << true;
    QTest::newRow("no scope") << QByteArray("tsm1 R i=3f9a1c2e4b5d6e7f8a9b:up") << false;
    QTest::newRow("server scope") << QByteArray("tsm1 R s=s i=3f9a1c2e4b5d6e7f8a9b:up") << false;
    QTest::newRow("no items") << QByteArray("tsm1 R s=c") << false;
    QTest::newRow("empty items") << QByteArray("tsm1 R s=c i=") << false;
    QTest::newRow("no colon") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b") << false;
    QTest::newRow("upper hex") << QByteArray("tsm1 R s=c i=3F9A1C2E4B5D6E7F8A9B:up") << false;
    QTest::newRow("short key") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9:up") << false;
    QTest::newRow("long key") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b0:up") << false;
    QTest::newRow("non-hex") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9g:up") << false;
    QTest::newRow("empty code") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:up..heart") << false;
    QTest::newRow("upper code") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:UP") << false;
    QTest::newRow("long code") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:abcdefghi") << false;
    QTest::newRow("double colon") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:up:heart") << false;
    QTest::newRow("same key twice") << QByteArray("tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:up,3f9a1c2e4b5d6e7f8a9b:") << false;
    QByteArray many = "tsm1 R s=c i=";
    for (int i = 0; i < 21; ++i)
        many += (i ? "," : "") + keyNumber(i).toLatin1() + ":up";
    QTest::newRow("21 items") << many << false;
    QByteArray codes = "tsm1 R s=c i=3f9a1c2e4b5d6e7f8a9b:";
    for (int i = 0; i < 17; ++i)
        codes += (i ? "." : "") + QByteArray("up");
    QTest::newRow("17 codes") << codes << false;
}

void TestProtocol::reactValidation()
{
    QFETCH(QByteArray, raw);
    QFETCH(bool, valid);
    const auto m = proto::parse(raw);
    QVERIFY(m);
    const auto r = proto::readReact(*m);
    QCOMPARE(bool(r), valid);
    if (!r)
        return;
    QVERIFY(!r->items.isEmpty());
    if (QTest::currentDataTag() == QByteArray("one")) {
        QCOMPARE(r->scope, proto::Scope::Channel);
        QCOMPARE(r->items.first().mask, quint8((1 << proto::ThumbsUp) | (1 << proto::Heart)));
    } else if (QTest::currentDataTag() == QByteArray("removal")) {
        QCOMPARE(r->scope, proto::Scope::Private);
        QCOMPARE(r->items.first().mask, quint8(0));
    } else if (QTest::currentDataTag() == QByteArray("sync answer")) {
        QVERIFY(r->syncAnswer);
        QCOMPARE(r->items.size(), 2);
    } else if (QTest::currentDataTag() == QByteArray("unknown code kept out")) {
        QCOMPARE(r->items.first().mask, quint8(1 << proto::ThumbsUp));
    }
}

void TestProtocol::syncValidation()
{
    auto read = [](const QByteArray& raw) { return proto::readSync(*proto::parse(raw)); };
    auto s    = read("tsm1 SYNC s=c m=3f9a1c2e4b5d6e7f8a9b,0a1b2c3d4e5f60718293,3f9a1c2e4b5d6e7f8a9b");
    QVERIFY(s);
    QCOMPARE(s->keys.size(), 2); // duplicates once
    QVERIFY(!read("tsm1 SYNC m=3f9a1c2e4b5d6e7f8a9b"));
    QVERIFY(!read("tsm1 SYNC s=c"));
    QVERIFY(!read("tsm1 SYNC s=c m=nothex"));
    QByteArray many = "tsm1 SYNC s=c m=";
    for (int i = 0; i < 21; ++i)
        many += (i ? "," : "") + keyNumber(i).toLatin1();
    QVERIFY(!read(many));
    // makeSync keeps the first 20 distinct valid keys.
    QStringList keys{QStringLiteral("bad"), kKeyA, kKeyA};
    for (int i = 0; i < 30; ++i)
        keys << keyNumber(i);
    s = proto::readSync(*proto::parse(proto::serialize(proto::makeSync(proto::Scope::Channel, keys))));
    QVERIFY(s);
    QCOMPARE(s->keys.size(), proto::kMaxItems);
    QCOMPARE(s->keys.first(), kKeyA);
}

void TestProtocol::serializeRoundTrip()
{
    const QVector<proto::Message> messages = {
        proto::makeHello(QStringLiteral("2.2.0"), {QStringLiteral("p"), QStringLiteral("r")}, true),
        proto::makeHi(QStringLiteral("2.2.0"), {QStringLiteral("p")}),
        proto::makeBye(),
        proto::makeSync(proto::Scope::Channel, {kKeyA, kKeyB}),
    };
    for (const proto::Message& m : messages) {
        const QByteArray wire = proto::serialize(m);
        QVERIFY(!wire.isEmpty());
        QVERIFY(wire.startsWith("tsm1 "));
        const auto back = proto::parse(wire);
        QVERIFY(back);
        QCOMPARE(back->type, m.type);
        QCOMPARE(back->fields.size(), m.fields.size());
        for (int i = 0; i < m.fields.size(); ++i) {
            QCOMPARE(back->fields.at(i).first, m.fields.at(i).first);
            QCOMPARE(back->fields.at(i).second, m.fields.at(i).second);
        }
    }
    QCOMPARE(proto::serialize(proto::makeHello(QStringLiteral("2.2.0"), {QStringLiteral("p"), QStringLiteral("r")}, true)),
             QByteArray("tsm1 HELLO pv=1 v=2.2.0 caps=p,r rr=1"));
    QCOMPARE(proto::serialize(proto::makeBye()), QByteArray("tsm1 BYE"));
    // A version that doesn't fit the grammar is left out, not sent broken.
    QCOMPARE(proto::serialize(proto::makeHi(QStringLiteral("2.2.0-beta 1"), {})), QByteArray("tsm1 HI pv=1"));
}

void TestProtocol::serializeRefusesBadMessages()
{
    proto::Message m;
    m.type = "hello";
    QVERIFY(proto::serialize(m).isEmpty());
    m.type = "HELLO";
    m.set("v", "a b");
    QVERIFY(proto::serialize(m).isEmpty());
    m.fields.clear();
    m.fields.append({"v", "1"});
    m.fields.append({"v", "2"});
    QVERIFY(proto::serialize(m).isEmpty());
    m.fields.clear();
    m.set("v", QByteArray(512, 'a'));
    m.set("w", QByteArray(512, 'a'));
    QVERIFY(proto::serialize(m).isEmpty()); // over kMaxSendBytes
}

void TestProtocol::reactPacking()
{
    QVector<proto::ReactItem> items;
    for (int i = 0; i < proto::kMaxItems; ++i)
        items.append({keyNumber(i + 1), proto::kAllReactions});
    items.append({QStringLiteral("not a key"), 1});
    const QVector<proto::Message> out = proto::makeReacts(proto::Scope::Channel, true, items);
    QVERIFY(out.size() >= 2);
    int total = 0;
    for (const proto::Message& m : out) {
        const QByteArray wire = proto::serialize(m);
        QVERIFY(!wire.isEmpty());
        QVERIFY(wire.size() <= proto::kMaxSendBytes);
        QVERIFY(m.value("i").size() <= proto::kMaxValueChars);
        const auto r = proto::readReact(*proto::parse(wire));
        QVERIFY(r);
        QVERIFY(r->syncAnswer);
        for (const proto::ReactItem& item : r->items)
            QCOMPARE(item.mask, proto::kAllReactions);
        total += r->items.size();
    }
    QCOMPARE(total, proto::kMaxItems);

    // Small sets: one message.
    QVector<proto::ReactItem> small;
    for (int i = 0; i < proto::kMaxItems; ++i)
        small.append({keyNumber(i + 1), 1});
    QCOMPARE(proto::makeReacts(proto::Scope::Private, false, small).size(), 1);
    QCOMPARE(proto::serialize(proto::makeReacts(proto::Scope::Private, false, {{kKeyA, 0}}).first()), QByteArray("tsm1 R s=p i=3f9a1c2e4b5d6e7f8a9b:"));
    QVERIFY(proto::makeReacts(proto::Scope::Channel, false, {}).isEmpty());
}

void TestProtocol::reactionCodes()
{
    QCOMPARE(proto::maskToCodes(0), QByteArray());
    QCOMPARE(proto::maskToCodes(proto::kAllReactions), QByteArray("up.heart.lol.wow.sad.fire"));
    QCOMPARE(proto::reactionIndex("fire"), int(proto::Fire));
    QCOMPARE(proto::reactionIndex("party"), -1);
    QVERIFY(!proto::reactionCode(6));
    quint8 mask = 0xff;
    QVERIFY(proto::codesToMask("", &mask));
    QCOMPARE(mask, quint8(0));
    QVERIFY(proto::codesToMask("fire.up.fire", &mask));
    QCOMPARE(mask, quint8((1 << proto::Fire) | (1 << proto::ThumbsUp)));
    QVERIFY(!proto::codesToMask(".", &mask));
    QVERIFY(proto::isMediaKey(kKeyA));
    QVERIFY(!proto::isMediaKey(kKeyA.toUpper()));
    QVERIFY(!proto::isMediaKey(QString(kKeyA).replace(0, 1, QChar(0x0663)))); // Arabic-Indic digit
}

// ---- transport ------------------------------------------------------------------------------------

void TestProtocol::linkSendsAndAnswers()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink link(backend);
    QVector<peers::SendResult> results;
    link.sendWith(7, proto::makeBye(), peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { results.append(r); });
    QCOMPARE(backend.commands.size(), 1);
    QCOMPARE(backend.commands.first().payload, QByteArray("tsm1 BYE"));
    QVERIFY(backend.commands.first().target.channel);
    const QString rc = backend.commands.first().rc;
    QVERIFY(PluginLink::isOwnReturnCode(rc));
    QVERIFY(results.isEmpty()); // waits for the answer
    QVERIFY(link.onServerError(7, 0, rc, QString(), false));
    QVERIFY(sameItems<peers::SendResult>(results, {peers::SendResult::Ok}));
    QVERIFY(!PluginLink::isOwnReturnCode(rc));
    QVERIFY(!link.onServerError(7, 0, QStringLiteral("someone else's"), QString(), false));
    QCOMPARE(link.counters(7).answeredOk, 1);
}

void TestProtocol::linkTimeoutIsNotSent()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink link(backend);
    QVector<peers::SendResult> results;
    link.sendWith(7, proto::makeBye(), peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { results.append(r); });
    const QString rc = backend.commands.first().rc;
    backend.now += 2999;
    link.pump();
    QVERIFY(results.isEmpty());
    backend.now += 1;
    link.pump();
    QVERIFY(sameItems<peers::SendResult>(results, {peers::SendResult::Failed}));
    QCOMPARE(link.counters(7).timeouts, 1);
    // A late answer is still ours (TeamSpeak must not print it). An Ok means the command did arrive
    // after all: its owner hears LateOk (a reaction is shown again, a SYNC's answers count).
    QVERIFY(PluginLink::isOwnReturnCode(rc));
    QVERIFY(link.onServerError(7, 0, rc, QString(), false));
    QVERIFY(sameItems<peers::SendResult>(results, {peers::SendResult::Failed, peers::SendResult::LateOk}));
    QVERIFY(!link.onServerError(7, 0, rc, QString(), false)); // once
    QCOMPARE(results.size(), 2);

    // A late error changes nothing; a late Ok after the linger time isn't ours any more.
    link.sendWith(7, proto::makeBye(), peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { results.append(r); });
    const QString second = backend.commands.last().rc;
    backend.now += 3000;
    link.pump();
    QCOMPARE(results.last(), peers::SendResult::Failed);
    QVERIFY(link.onServerError(7, 0x0a08, second, QString(), false));
    QCOMPARE(results.size(), 3);
    link.sendWith(7, proto::makeBye(), peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { results.append(r); });
    const QString third = backend.commands.last().rc;
    backend.now += 3000;
    link.pump();
    backend.now += 30000; // the linger time
    link.pump();
    QVERIFY(!link.onServerError(7, 0, third, QString(), false));
    QCOMPARE(results.size(), 4);
}

void TestProtocol::linkFloodPausesAndRetriesPresence()
{
    FakeBackend backend;
    FloodGovernor::Limits limits = roomyLimits();
    limits.commandFloodPauseMs   = 1000; // the pause without a hint: here the server's hint decides
    backend.flood                = FloodGovernor(limits);
    PluginLink                 link(backend);
    QVector<peers::SendResult> presence, reaction;
    link.send(7, proto::makeHello(QStringLiteral("2.2.0"), {QStringLiteral("p")}, true), peers::Target::toChannel(),
              [&](peers::SendResult r) { presence.append(r); });
    link.sendWith(7, proto::makeReacts(proto::Scope::Channel, false, {{kKeyA, 1}}).first(), peers::Target::toChannel(), PluginLink::Priority::Reaction,
                  [&](peers::SendResult r) { reaction.append(r); });
    QCOMPARE(backend.commands.size(), 2);
    QVERIFY(backend.commands.at(0).payload.startsWith("tsm1 HELLO")); // presence first
    const QString helloRc = backend.commands.at(0).rc;
    const QString reactRc = backend.commands.at(1).rc;

    QVERIFY(link.onServerError(7, 0x020c, helloRc, QStringLiteral("retry in 1500 ms"), false));
    QVERIFY(link.onServerError(7, 0x020c, reactRc, QStringLiteral("retry in 1500ms"), false));
    QCOMPARE(backend.floodChanges, 2);
    QVERIFY(sameItems<peers::SendResult>(reaction, {peers::SendResult::Flooded})); // reactions are reverted by their owner
    QVERIFY(presence.isEmpty());                                                 // presence goes again once
    QCOMPARE(backend.commands.size(), 2);
    backend.now += 1700;
    link.pump();
    QCOMPARE(backend.commands.size(), 2); // hint + 250 ms not over yet
    backend.now += 100;
    link.pump();
    QCOMPARE(backend.commands.size(), 3);
    QVERIFY(backend.commands.last().payload.startsWith("tsm1 HELLO"));
    QVERIFY(link.onServerError(7, 0x020c, backend.commands.last().rc, QString(), false));
    QVERIFY(sameItems<peers::SendResult>(presence, {peers::SendResult::Flooded})); // only one retry
    QVERIFY(link.counters(7).floodBackoffs >= 3);
}

void TestProtocol::linkFiltersClientTargets()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink                 link(backend);
    QVector<peers::SendResult> results;
    link.send(7, proto::makeHi(QStringLiteral("2.2.0"), {}), peers::Target::toClients({2, 9, 3}), [&](peers::SendResult r) { results.append(r); });
    QCOMPARE(backend.commands.size(), 1);
    QVERIFY(sameItems<quint16>(backend.commands.first().target.clients, {2, 3})); // 9 has left
    // Nobody left to send to: not sent.
    link.send(7, proto::makeHi(QStringLiteral("2.2.0"), {}), peers::Target::toClients({9}), [&](peers::SendResult r) { results.append(r); });
    QCOMPARE(backend.commands.size(), 1);
    QVERIFY(sameItems<peers::SendResult>(results, {peers::SendResult::Failed}));
    // Someone left between the check and the server: the rest get it again, once.
    backend.visible.remove(3);
    QVERIFY(link.onServerError(7, 0x0200, backend.commands.first().rc, QString(), false));
    QCOMPARE(backend.commands.size(), 2);
    QVERIFY(sameItems<quint16>(backend.commands.last().target.clients, {2}));
}

void TestProtocol::linkMergesAnswersAndCoalesces()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    for (int i = 0; i < 24; ++i)
        backend.flood.commandSent(backend.now); // the plugin bucket is full: everything waits in the queue
    PluginLink link(backend);
    const proto::Message hi = proto::makeHi(QStringLiteral("2.2.0"), {QStringLiteral("p")});
    link.send(7, hi, peers::Target::toClients({2}), {});
    link.send(7, hi, peers::Target::toClients({3, 2}), {});
    link.send(7, hi, peers::Target::toClients({4}), {});

    QVector<peers::SendResult> first, second;
    const auto r1 = proto::makeReacts(proto::Scope::Channel, false, {{kKeyA, 1}}).first();
    const auto r2 = proto::makeReacts(proto::Scope::Channel, false, {{kKeyA, 3}}).first();
    link.sendWith(7, r1, peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { first.append(r); }, QStringLiteral("R:a"));
    link.sendWith(7, r2, peers::Target::toChannel(), PluginLink::Priority::Reaction, [&](peers::SendResult r) { second.append(r); }, QStringLiteral("R:a"));
    QVERIFY(sameItems<peers::SendResult>(first, {peers::SendResult::Superseded}));
    QVERIFY(backend.commands.isEmpty());

    backend.now += 2000; // room for two commands
    link.pump();
    QCOMPARE(backend.commands.size(), 2); // one HI for everyone, one R with the latest state
    QVERIFY(sameItems<quint16>(backend.commands.at(0).target.clients, {2, 3, 4}));
    QCOMPARE(backend.commands.at(1).payload, proto::serialize(r2));
    link.onServerError(7, 0, backend.commands.at(1).rc, QString(), false);
    QVERIFY(sameItems<peers::SendResult>(second, {peers::SendResult::Ok}));
}

void TestProtocol::linkBlocksOnPermissionError()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink link(backend);
    QSignalSpy blockedSpy(&link, &PluginLink::blockedChanged);
    QVector<peers::SendResult> results;
    link.send(7, proto::makeHello(QStringLiteral("2.2.0"), {}, true), peers::Target::toChannel(), [&](peers::SendResult r) { results.append(r); });
    QVERIFY(link.onServerError(7, 0x0a08, backend.commands.first().rc, QString(), true));
    QVERIFY(sameItems<peers::SendResult>(results, {peers::SendResult::Blocked}));
    QVERIFY(link.blocked(7));
    QCOMPARE(blockedSpy.count(), 1);
    link.send(7, proto::makeBye(), peers::Target::toChannel(), [&](peers::SendResult r) { results.append(r); });
    QCOMPARE(results.last(), peers::SendResult::Blocked);
    QCOMPARE(backend.commands.size(), 1);
    QVERIFY(!link.blocked(8)); // other connections are not affected
    // Any error on our channel HELLO blocks too; a new connection starts fresh.
    link.connectionLost(7);
    QVERIFY(!link.blocked(7));
    link.send(8, proto::makeHello(QStringLiteral("2.2.0"), {}, true), peers::Target::toChannel(), {});
    QVERIFY(link.onServerError(8, 0x0605, backend.commands.last().rc, QString(), false));
    QVERIFY(link.blocked(8));
}

void TestProtocol::linkSpendsPluginBucket()
{
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink link(backend);
    // S0: plugin commands have their own server counter, so chat posts waiting don't hold them.
    backend.flood.setPendingPosts(2);
    link.send(7, proto::makeBye(), peers::Target::toChannel(), {});
    QCOMPARE(backend.commands.size(), 1);

    // The plugin bucket: 24 back to back from rest, then one a second.
    backend.flood = FloodGovernor(roomyLimits());
    backend.commands.clear();
    for (int i = 0; i < 30; ++i)
        link.sendWith(7, proto::makeSync(proto::Scope::Channel, {keyNumber(i)}), peers::Target::toChannel(), PluginLink::Priority::Background, {});
    QCOMPARE(backend.commands.size(), 24);
    backend.now += 999;
    link.pump();
    QCOMPARE(backend.commands.size(), 24);
    backend.now += 1;
    link.pump();
    QCOMPARE(backend.commands.size(), 25);
}

void TestProtocol::linkInboundRules()
{
    qRegisterMetaType<proto::Message>();
    FakeBackend backend;
    PluginLink  link(backend);
    QSignalSpy  spy(&link, &PluginLink::received);
    link.onCommand(7, 1, kUidMe, QStringLiteral("Me"), "tsm1 BYE"); // own echo
    link.onCommand(7, 2, QString(), QStringLiteral("x"), "tsm1 BYE"); // no identity
    link.onCommand(7, 0, kUidSara, QStringLiteral("Sara"), "tsm1 BYE");
    QCOMPARE(spy.count(), 0);
    link.onCommand(7, 2, kUidSara, QStringLiteral("Sara"), "tsm1 BYE");
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(1).value<quint16>(), quint16(2));
    QCOMPARE(spy.first().at(2).toString(), kUidSara);
    link.onCommand(7, 2, kUidSara, QStringLiteral("Sara"), "tsm1 BYE x");
    link.onCommand(7, 2, kUidSara, QStringLiteral("Sara"), "tsm1 " + QByteArray(3000, 'A'));
    QCOMPARE(link.counters(7).rejected, 2);
    // At most 20 per person per 30 s (the malformed one counted too); others are not affected.
    for (int i = 0; i < 30; ++i)
        link.onCommand(7, 2, kUidSara, QStringLiteral("Sara"), "tsm1 BYE");
    QCOMPARE(spy.count(), 1 + 18);
    QCOMPARE(link.counters(7).rateLimited, 12);
    link.onCommand(7, 3, kUidReza, QStringLiteral("Reza"), "tsm1 BYE");
    QCOMPARE(spy.count(), 20);
    backend.now += 30000;
    link.onCommand(7, 2, kUidSara, QStringLiteral("Sara"), "tsm1 BYE");
    QCOMPARE(spy.count(), 21);
}

void TestProtocol::linkRetryHint()
{
    // The link reads the hint through the governor (FloodGovernor::retryHintMs, capped at 2 minutes).
    QCOMPARE(FloodGovernor::retryHintMs(QStringLiteral("retry in 5687 ms")), 5687);
    QCOMPARE(FloodGovernor::retryHintMs(QStringLiteral("please Retry in 1233ms")), 1233);
    QCOMPARE(FloodGovernor::retryHintMs(QString()), -1);
    QCOMPARE(FloodGovernor::retryHintMs(QStringLiteral("retry in soon")), -1);
    QCOMPARE(FloodGovernor::retryHintMs(QStringLiteral("retry in 9999999 ms")), 120000);

    // A flood answer pauses the plugin bucket for hint + 250 ms, then exactly one command fits.
    FakeBackend backend;
    backend.flood = FloodGovernor(roomyLimits());
    PluginLink link(backend);
    link.send(7, proto::makeBye(), peers::Target::toChannel(), {});
    QVERIFY(link.onServerError(7, 0x020c, backend.commands.first().rc, QStringLiteral("retry in 1233 ms"), false));
    QCOMPARE(backend.flood.counters().lastPauseMs, 1233 + 250);
    QVERIFY(!backend.flood.commandReady(backend.now + 1482));
    QVERIFY(backend.flood.commandReady(backend.now + 1483));
}

// ---- presence -------------------------------------------------------------------------------------

void TestProtocol::helloAfterSettle()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.setLocal({QStringLiteral("2.2.0"), true, true});
    dir.connected(1);
    advance(env, dir, 799);
    QCOMPARE(sender.count("HELLO"), 0);
    advance(env, dir, 1);
    QCOMPARE(sender.count("HELLO"), 1);
    const auto* h = sender.last("HELLO");
    QVERIFY(h->target.channel);
    QCOMPARE(h->message.value("rr"), QByteArray("1"));
    QCOMPARE(h->message.value("caps"), QByteArray("p,r"));
    advance(env, dir, 10000);
    QCOMPARE(sender.count("HELLO"), 1);
}

void TestProtocol::channelHoppingSaysHelloOnce()
{
    FakeEnv    env;
    FakeSender sender;
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 300);
    env.clients[1].channel = 11;
    dir.clientMoved(1, 1, 10, 11);
    advance(env, dir, 300);
    env.clients[1].channel = 12;
    dir.clientMoved(1, 1, 11, 12);
    advance(env, dir, 300);
    QCOMPARE(sender.count("HELLO"), 0);
    advance(env, dir, 600);
    QCOMPARE(sender.count("HELLO"), 1);
    QCOMPARE(dir.channelOf(1), quint64(12));
    // Another hop right after: at most one HELLO per 3 s.
    env.clients[1].channel = 13;
    dir.clientMoved(1, 1, 12, 13);
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HELLO"), 1);
    advance(env, dir, 2100);
    QCOMPARE(sender.count("HELLO"), 2);
}

void TestProtocol::answersAndTimeouts()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    env.add(3, kUidReza, QStringLiteral("Reza"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    QSignalSpy changed(&dir, &peers::PeerDirectory::presenceChanged);
    dir.connected(1);
    auto s = dir.summary(1, kModeChannel, 0);
    QCOMPARE(s.kind, peers::PresenceSummary::Kind::Channel);
    QCOMPARE(s.checking.size(), 2);
    advance(env, dir, 800);
    dir.received(1, 2, kUidSara, QStringLiteral("Sara"), *proto::parse("tsm1 HI pv=1 v=2.2.0 caps=p,r"));
    s = dir.summary(1, kModeChannel, 0);
    QCOMPARE(s.has, QStringList{QStringLiteral("Sara")});
    QCOMPARE(s.checking, QStringList{QStringLiteral("Reza")});
    QVERIFY(changed.count() > 0);
    advance(env, dir, 3999);
    QCOMPARE(dir.summary(1, kModeChannel, 0).checking.size(), 1);
    advance(env, dir, 1);
    s = dir.summary(1, kModeChannel, 0);
    QCOMPARE(s.without, QStringList{QStringLiteral("Reza")});
    QCOMPARE(dir.stateOf(1, kUidReza), peers::PeerState::Without);
    // A late answer still counts.
    dir.received(1, 3, kUidReza, QStringLiteral("Reza"), *proto::parse("tsm1 HI pv=1 v=2.2.1"));
    s = dir.summary(1, kModeChannel, 0);
    QCOMPARE(s.has.size(), 2);
    QCOMPARE(dir.versions(1).value(QStringLiteral("2.2.1")), 1);
    QVERIFY(dir.hasCapability(1, kUidSara, "r"));
    QVERIFY(!dir.hasCapability(1, kUidReza, "r"));
    QCOMPARE(sender.count("HI"), 0); // answers are never answered
}

void TestProtocol::joinerGetsLongerTimeout()
{
    FakeEnv    env;
    FakeSender sender;
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 5000);
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    dir.clientMoved(1, 2, 0, 10);
    QCOMPARE(dir.summary(1, kModeChannel, 0).checking.size(), 1);
    advance(env, dir, 5999);
    QCOMPARE(dir.summary(1, kModeChannel, 0).checking.size(), 1);
    advance(env, dir, 1);
    QCOMPARE(dir.summary(1, kModeChannel, 0).without.size(), 1);

    // The usual case: the joiner's own HELLO arrives first, and we answer only them.
    env.add(3, kUidReza, QStringLiteral("Reza"), 10);
    dir.clientMoved(1, 3, 0, 10);
    advance(env, dir, 900);
    dir.received(1, 3, kUidReza, QStringLiteral("Reza"), hello());
    QCOMPARE(dir.summary(1, kModeChannel, 0).has, QStringList{QStringLiteral("Reza")});
    QCOMPARE(sender.count("HI"), 0);
    advance(env, dir, 300); // jitter
    QCOMPARE(sender.count("HI"), 1);
    QVERIFY(sameItems<peers::ClientId>(sender.last("HI")->target.clients, {3}));
    // Leaving removes them from the count.
    env.clients[3].channel = 20;
    dir.clientMoved(1, 3, 10, 20);
    QCOMPARE(dir.summary(1, kModeChannel, 0).others(), 1);
}

void TestProtocol::answersMergeAndRateLimit()
{
    FakeEnv    env;
    FakeSender sender;
    env.jitter = 900; // the latest answer time: everyone below arrives within it
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 5000);
    for (int i = 0; i < 5; ++i) {
        const quint16 id  = static_cast<quint16>(20 + i);
        const QString uid = QStringLiteral("uid%1aaaaaaaaaaaaaaaaaaaaaaa=").arg(i);
        env.add(id, uid, QStringLiteral("P%1").arg(i), 10);
        dir.clientMoved(1, id, 0, 10);
        dir.received(1, id, uid, QStringLiteral("P%1").arg(i), hello());
        advance(env, dir, 100);
    }
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HI"), 1); // five joiners, one answer
    QCOMPARE(sender.last("HI")->target.clients.size(), 5);

    // The same person asking again within 20 s gets no second answer...
    dir.received(1, 20, QStringLiteral("uid0aaaaaaaaaaaaaaaaaaaaaaa="), QStringLiteral("P0"), hello());
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HI"), 1);
    // ... unless they reconnected (another client id).
    env.add(30, QStringLiteral("uid0aaaaaaaaaaaaaaaaaaaaaaa="), QStringLiteral("P0"), 10);
    dir.received(1, 30, QStringLiteral("uid0aaaaaaaaaaaaaaaaaaaaaaa="), QStringLiteral("P0"), hello());
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HI"), 2);
    // Someone outside the channel (a private-chat ping): once a minute.
    env.add(40, kUidSara, QStringLiteral("Sara"), 99);
    dir.received(1, 40, kUidSara, QStringLiteral("Sara"), hello());
    advance(env, dir, 1000);
    dir.received(1, 40, kUidSara, QStringLiteral("Sara"), hello());
    advance(env, dir, 59000); // a minute after the answer
    QCOMPARE(sender.count("HI"), 3);
    dir.received(1, 40, kUidSara, QStringLiteral("Sara"), hello());
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HI"), 4);
    // No reply requested: no answer.
    dir.received(1, 21, QStringLiteral("uid1aaaaaaaaaaaaaaaaaaaaaaa="), QStringLiteral("P1"), hello(false));
    advance(env, dir, 60000);
    QCOMPARE(sender.count("HI"), 4);
}

void TestProtocol::byeAndDisconnect()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 900);
    dir.received(1, 2, kUidSara, QStringLiteral("Sara"), *proto::parse("tsm1 HI pv=1"));
    QCOMPARE(dir.summary(1, kModeChannel, 0).has.size(), 1);
    dir.received(1, 2, kUidSara, QStringLiteral("Sara"), proto::makeBye());
    QCOMPARE(dir.summary(1, kModeChannel, 0).without.size(), 1);
    dir.disconnected(1);
    QCOMPARE(dir.summary(1, kModeChannel, 0).kind, peers::PresenceSummary::Kind::Hidden);
    QVERIFY(dir.connections().isEmpty());
    QCOMPARE(dir.nextWakeMs(), qint64(-1));
}

void TestProtocol::presenceOffSendsNothing()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.setLocal({QStringLiteral("2.2.0"), false, true});
    dir.connected(1);
    advance(env, dir, 10000);
    dir.received(1, 2, kUidSara, QStringLiteral("Sara"), hello());
    advance(env, dir, 10000);
    QVERIFY(sender.sent.isEmpty());
    QCOMPARE(dir.summary(1, kModeChannel, 0).kind, peers::PresenceSummary::Kind::Hidden);
    QCOMPARE(dir.summary(1, kModeServer, 0).kind, peers::PresenceSummary::Kind::Server); // static, no lookup

    // Switched on: HELLO; switched off again: one BYE.
    dir.setLocal({QStringLiteral("2.2.0"), true, true});
    advance(env, dir, 1000);
    QCOMPARE(sender.count("HELLO"), 1);
    dir.setLocal({QStringLiteral("2.2.0"), false, true});
    QCOMPARE(sender.count("BYE"), 1);
    advance(env, dir, 60000);
    QCOMPARE(sender.sent.size(), 2);
}

void TestProtocol::partnerCheck()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(5, kUidSara, QStringLiteral("Sara"), 77);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 1000);
    const int before = sender.count("HELLO");
    auto      s      = dir.summary(1, kModeClient, 5);
    QCOMPARE(s.kind, peers::PresenceSummary::Kind::Private);
    QCOMPARE(s.partnerState, peers::PeerState::Checking);
    QCOMPARE(sender.count("HELLO"), before + 1);
    QVERIFY(sameItems<peers::ClientId>(sender.last("HELLO")->target.clients, {5}));
    dir.summary(1, kModeClient, 5);
    QCOMPARE(sender.count("HELLO"), before + 1);
    advance(env, dir, 4000);
    QCOMPARE(dir.summary(1, kModeClient, 5).partnerState, peers::PeerState::Without);
    advance(env, dir, 50000);
    dir.summary(1, kModeClient, 5);
    QCOMPARE(sender.count("HELLO"), before + 1); // once a minute
    advance(env, dir, 10000);
    dir.summary(1, kModeClient, 5);
    QCOMPARE(sender.count("HELLO"), before + 2);
    dir.received(1, 5, kUidSara, QStringLiteral("Sara"), *proto::parse("tsm1 HI pv=1"));
    QCOMPARE(dir.summary(1, kModeClient, 5).partnerState, peers::PeerState::Has);
    // Unknown partner: no line.
    QCOMPARE(dir.summary(1, kModeClient, 0).kind, peers::PresenceSummary::Kind::Hidden);
    QCOMPARE(dir.summary(1, kModeClient, 99).kind, peers::PresenceSummary::Kind::Hidden);
}

void TestProtocol::queryClientsAndClonesExcluded()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    env.add(3, kUidSara, QStringLiteral("Sara (laptop)"), 10); // the same person twice
    env.add(4, kUidMe, QStringLiteral("Me too"), 10);          // our own clone
    env.add(5, QStringLiteral("serveradmin"), QStringLiteral("bot"), 10, true);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    QCOMPARE(dir.summary(1, kModeChannel, 0).others(), 1);
    advance(env, dir, 900);
    dir.received(1, 3, kUidSara, QStringLiteral("Sara (laptop)"), *proto::parse("tsm1 HI pv=1"));
    const auto s = dir.summary(1, kModeChannel, 0);
    QCOMPARE(s.others(), 1);
    QCOMPARE(s.has.size(), 1);
}

void TestProtocol::syncDueNeedsReactionPeer()
{
    FakeEnv    env;
    FakeSender sender;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    QSignalSpy sync(&dir, &peers::PeerDirectory::syncDue);
    dir.connected(1);
    advance(env, dir, 1500);
    dir.received(1, 2, kUidSara, QStringLiteral("Sara"), *proto::parse("tsm1 HI pv=1 caps=p,r"));
    advance(env, dir, 499);
    QCOMPARE(sync.count(), 0);
    advance(env, dir, 1);
    QCOMPARE(sync.count(), 1);
    advance(env, dir, 20000);
    QCOMPARE(sync.count(), 1);

    // Without anyone supporting reactions: no SYNC.
    env.clients[1].channel = 11;
    dir.clientMoved(1, 1, 10, 11);
    advance(env, dir, 20000);
    QCOMPARE(sync.count(), 1);
}

void TestProtocol::blockedHidesPresence()
{
    FakeEnv    env;
    FakeSender sender;
    sender.isBlocked = true;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 10000);
    QVERIFY(sender.sent.isEmpty());
    QCOMPARE(dir.summary(1, kModeChannel, 0).kind, peers::PresenceSummary::Kind::Hidden);
    QVERIFY(peers::presenceText(dir.summary(1, kModeChannel, 0)).isEmpty());
}

void TestProtocol::floodedHelloIsRetried()
{
    FakeEnv    env;
    FakeSender sender;
    sender.result = peers::SendResult::Flooded;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    peers::PeerDirectory dir(env, sender);
    dir.setAutoTimer(false);
    dir.connected(1);
    advance(env, dir, 800);
    QCOMPARE(sender.count("HELLO"), 1);
    advance(env, dir, 5000);
    QCOMPARE(sender.count("HELLO"), 2);
    QCOMPARE(dir.summary(1, kModeChannel, 0).checking.size(), 1); // nobody was asked yet
    advance(env, dir, 5000);
    QCOMPARE(sender.count("HELLO"), 3);
    advance(env, dir, 4000); // gave up: those who couldn't be asked count as without
    QCOMPARE(sender.count("HELLO"), 3);
    QCOMPARE(dir.summary(1, kModeChannel, 0).without.size(), 1);
}

void TestProtocol::presenceOffDropsHeldHello()
{
    // The real transport between the directory and a fake server. A HELLO it still holds back (a flood
    // pause) never goes out once presence is switched off; one already on its way is taken back with a
    // BYE when its Ok comes; one whose Ok comes after its timeout counts as announced after all.
    FakeEnv     env;
    FakeBackend backend;
    env.add(2, kUidSara, QStringLiteral("Sara"), 10);
    PluginLink           link(backend);
    peers::PeerDirectory dir(env, link);
    dir.setAutoTimer(false);
    const auto commands = [&backend](const char* type) {
        int n = 0;
        for (const FakeBackend::Command& c : qAsConst(backend.commands)) {
            const QByteArray head = QByteArray("tsm1 ") + type;
            n += c.payload == head || c.payload.startsWith(head + ' ') ? 1 : 0;
        }
        return n;
    };

    backend.flood.commandFlooded(backend.now, 5000);
    dir.setLocal({QStringLiteral("2.2.0"), true, true});
    dir.connected(1);
    advance(env, dir, 1000);
    QCOMPARE(commands("HELLO"), 0); // held back by the pause
    dir.setLocal({QStringLiteral("2.2.0"), false, true});
    backend.now += 6000;
    link.pump();
    QVERIFY(backend.commands.isEmpty()); // neither the HELLO nor a BYE: nobody was told anything

    backend.flood = FloodGovernor(roomyLimits());
    dir.setLocal({QStringLiteral("2.2.0"), true, true});
    advance(env, dir, 1000);
    QCOMPARE(commands("HELLO"), 1);
    const QString rc = backend.commands.last().rc;
    dir.setLocal({QStringLiteral("2.2.0"), false, true});
    QCOMPARE(commands("BYE"), 0); // not announced yet
    QVERIFY(link.onServerError(1, 0, rc, QString(), false));
    QCOMPARE(commands("BYE"), 1); // it arrived: taken back

    dir.setLocal({QStringLiteral("2.2.0"), true, true});
    advance(env, dir, 1000);
    QCOMPARE(commands("HELLO"), 2);
    const QString late = backend.commands.last().rc;
    backend.now += 3000;
    link.pump();                                               // no answer in time: Failed
    QVERIFY(link.onServerError(1, 0, late, QString(), false)); // then its Ok after all
    dir.setLocal({QStringLiteral("2.2.0"), false, true});
    QCOMPARE(commands("BYE"), 2);
}

void TestProtocol::presenceTexts()
{
    using K = peers::PresenceSummary::Kind;
    peers::PresenceSummary s;
    QVERIFY(peers::presenceText(s).isEmpty());
    s.kind = K::Server;
    QCOMPARE(peers::presenceText(s), QStringLiteral("People without TS Media get a download link."));

    s.kind = K::Channel;
    QCOMPARE(peers::presenceText(s), QStringLiteral("You're the only one in this channel. No one else will get this message."));
    s.checking = QStringList{QStringLiteral("A"), QStringLiteral("B")};
    QCOMPARE(peers::presenceText(s), QStringLiteral("Checking who here has TS Media…"));
    s.checking = QStringList{QStringLiteral("B")};
    s.has      = QStringList{QStringLiteral("A")};
    QCOMPARE(peers::presenceText(s), QStringLiteral("So far 1 of 2 people here will see it in the chat."));
    s.checking.clear();
    QCOMPARE(peers::presenceText(s), QStringLiteral("A will see it in the chat."));
    s.has.clear();
    s.without = QStringList{QStringLiteral("B")};
    QCOMPARE(peers::presenceText(s), QStringLiteral("B doesn't seem to have TS Media, so they'll get a download link."));
    s.without = QStringList{QStringLiteral("B"), QStringLiteral("C")};
    QCOMPARE(peers::presenceText(s), QStringLiteral("Nobody else here seems to have TS Media, so they'll get a download link."));
    s.has = QStringList{QStringLiteral("A"), QStringLiteral("D"), QStringLiteral("E")};
    QCOMPARE(peers::presenceText(s), QStringLiteral("3 of 5 people here will see it in the chat. The others get a download link."));
    s.without.clear();
    QCOMPARE(peers::presenceText(s), QStringLiteral("All 3 people here will see it in the chat."));

    s.without = QStringList{QStringLiteral("Bob")};
    s.checking = QStringList{QStringLiteral("Eve")};
    for (int i = 0; i < 12; ++i)
        s.has << QStringLiteral("N%1").arg(i);
    const QStringList details = peers::presenceDetails(s);
    QCOMPARE(details.size(), 4);
    QVERIFY(details.at(0).startsWith(QStringLiteral("Will see it in the chat: A, D, E, N0")));
    QVERIFY(details.at(0).endsWith(QStringLiteral(" and 5 more")));
    QCOMPARE(details.at(1), QStringLiteral("Will get a link: Bob"));
    QCOMPARE(details.at(2), QStringLiteral("Still checking: Eve"));
    QCOMPARE(details.at(3), QStringLiteral("People on TS Media 2.1 or older count as getting a link."));

    peers::PresenceSummary p;
    p.kind    = K::Private;
    p.partner = QStringLiteral("Sara");
    p.partnerState = peers::PeerState::Checking;
    QCOMPARE(peers::presenceText(p), QStringLiteral("Checking whether Sara has TS Media…"));
    p.partnerState = peers::PeerState::Has;
    QCOMPARE(peers::presenceText(p), QStringLiteral("Sara has TS Media and will see it in the chat."));
    p.partnerState = peers::PeerState::Without;
    QCOMPARE(peers::presenceText(p), QStringLiteral("Sara doesn't seem to have TS Media, so they'll get a download link."));
    p.partner.clear();
    QVERIFY(peers::presenceText(p).isEmpty());
}

void TestProtocol::presenceNames()
{
    QCOMPARE(peers::presenceName(QStringLiteral("Sara")), QStringLiteral("Sara"));
    QCOMPARE(peers::presenceName(QStringLiteral("evil") + QChar(0x202e) + QStringLiteral("txt.exe")), QStringLiteral("eviltxt.exe"));
    QCOMPARE(peers::presenceName(QStringLiteral("a\nb\tc")), QStringLiteral("abc"));
    QCOMPARE(peers::presenceName(QString(QChar(0x200f))), QStringLiteral("Someone"));
    const QString longName = peers::presenceName(QString(40, QLatin1Char('x')));
    QCOMPARE(longName.size(), 32);
    QVERIFY(longName.endsWith(QChar(0x2026)));
}

// ---- reactions ------------------------------------------------------------------------------------

void TestProtocol::storeMergeRules()
{
    ReactionStore store;
    QSignalSpy    spy(&store, &ReactionStore::changed);
    const qint64  now = 1760000000000;
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0x01, now), ReactionStore::Apply::Applied);
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0x01, now), ReactionStore::Apply::Unchanged); // idempotent
    QCOMPARE(spy.count(), 1);
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0x06, now), ReactionStore::Apply::Applied); // replaces
    QCOMPARE(store.maskOf(kKeyA, kUidSara), quint8(0x06));
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0xff, now), ReactionStore::Apply::Applied); // unknown bits cut
    QCOMPARE(store.maskOf(kKeyA, kUidSara), proto::kAllReactions);
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0, now), ReactionStore::Apply::Applied); // removed
    QVERIFY(!store.hasReactions(kKeyA));
    QCOMPARE(store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0, now), ReactionStore::Apply::Unchanged);
    QCOMPARE(store.applyRemote(QString(), QStringLiteral("nope"), kUidSara, QStringLiteral("Sara"), 1, now), ReactionStore::Apply::Rejected);
    QCOMPARE(store.applyRemote(QString(), kKeyA, QStringLiteral("bad uid!"), QStringLiteral("x"), 1, now), ReactionStore::Apply::Rejected);
    QCOMPARE(store.applyRemote(QString(), kKeyA, QString(), QStringLiteral("x"), 1, now), ReactionStore::Apply::Rejected);

    // Own optimistic set, then a revert to the committed mask.
    store.setOwn(kKeyB, kUidMe, QStringLiteral("Me"), 0x01, now);
    store.setOwn(kKeyB, kUidMe, QStringLiteral("Me"), 0x03, now);
    QCOMPARE(store.view(kKeyB, kUidMe).ownMask(), 0x03);
    store.setOwn(kKeyB, kUidMe, QStringLiteral("Me"), 0x01, now);
    QCOMPARE(store.view(kKeyB, kUidMe).ownMask(), 0x01);
    // Names are cleaned up and capped.
    store.applyRemote(QString(), kKeyB, kUidReza, QStringLiteral("Re\nza") + QString(100, QLatin1Char('z')), 0x01, now);
    const QString name = store.view(kKeyB, kUidMe).entry(E(0)).others.first();
    QVERIFY(name.startsWith(QStringLiteral("Reza")));
    QCOMPARE(name.size(), 64);
}

void TestProtocol::storeReactorCap()
{
    ReactionStore::Limits limits;
    limits.maxReactors = 3;
    ReactionStore store(limits);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(store.applyRemote(QString(), kKeyA, QStringLiteral("uid%1").arg(i), QStringLiteral("P"), 1, 0), ReactionStore::Apply::Applied);
    QCOMPARE(store.applyRemote(QString(), kKeyA, QStringLiteral("uid9"), QStringLiteral("P"), 1, 0), ReactionStore::Apply::Rejected);
    QCOMPARE(store.applyRemote(QString(), kKeyA, QStringLiteral("uid1"), QStringLiteral("P"), 3, 0), ReactionStore::Apply::Applied); // existing ones may change
    QCOMPARE(store.reactorCount(kKeyA), 3);
    store.applyRemote(QString(), kKeyA, QStringLiteral("uid0"), QStringLiteral("P"), 0, 0);
    QCOMPARE(store.applyRemote(QString(), kKeyA, QStringLiteral("uid9"), QStringLiteral("P"), 1, 0), ReactionStore::Apply::Applied);
}

void TestProtocol::storeOrphans()
{
    ReactionStore::Limits limits;
    limits.maxOrphans = 3;
    ReactionStore store(limits);
    QSet<QString> known;
    store.setKnownKey([&](const QString& key, const QString& server) { return known.contains(key) && server == QStringLiteral("srv"); });
    QCOMPARE(store.applyRemote(QStringLiteral("srv"), kKeyA, kUidSara, QStringLiteral("Sara"), 1, 1000), ReactionStore::Apply::Orphaned);
    QCOMPARE(store.applyRemote(QStringLiteral("srv"), kKeyA, kUidSara, QStringLiteral("Sara"), 2, 2000), ReactionStore::Apply::Orphaned); // replaces
    QCOMPARE(store.orphanCount(), 1);
    QVERIFY(!store.hasReactions(kKeyA));
    known.insert(kKeyA);
    store.resolveOrphans(kKeyA, 5000);
    QCOMPARE(store.maskOf(kKeyA, kUidSara), quint8(2));
    QCOMPARE(store.orphanCount(), 0);

    // Expired after 15 s.
    store.applyRemote(QStringLiteral("srv"), kKeyB, kUidSara, QStringLiteral("Sara"), 1, 10000);
    known.insert(kKeyB);
    store.resolveOrphans(kKeyB, 25001);
    QVERIFY(!store.hasReactions(kKeyB));
    // Another server's media with the same key stays out.
    store.applyRemote(QStringLiteral("other"), kKeyB, kUidReza, QStringLiteral("Reza"), 1, 30000);
    store.resolveOrphans(kKeyB, 30001);
    QVERIFY(!store.hasReactions(kKeyB));
    // Bounded: the oldest goes.
    for (int i = 0; i < 5; ++i)
        store.applyRemote(QStringLiteral("srv"), keyNumber(i + 100), kUidSara, QStringLiteral("Sara"), 1, 40000);
    QCOMPARE(store.orphanCount(), 3);
}

void TestProtocol::storeViewAndSync()
{
    ReactionStore store;
    store.applyRemote(QString(), kKeyA, kUidReza, QStringLiteral("Reza"), (1 << proto::ThumbsUp) | (1 << proto::Fire), 2000000);
    store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 1 << proto::ThumbsUp, 1000000);
    store.setOwn(kKeyA, kUidMe, QStringLiteral("Me"), 1 << proto::ThumbsUp, 3000000);
    const ReactionView v = store.view(kKeyA, kUidMe);
    QCOMPARE(v.entry(E(proto::ThumbsUp)).count, 3);
    QVERIFY(v.entry(E(proto::ThumbsUp)).mine);
    QCOMPARE(v.entry(E(proto::ThumbsUp)).others, (QStringList{QStringLiteral("Sara"), QStringLiteral("Reza")})); // earliest first
    QCOMPARE(v.entry(E(proto::Fire)).count, 1);
    QVERIFY(!v.entry(E(proto::Fire)).mine);
    QCOMPARE(v.entry(E(proto::Heart)).count, 0);
    QVERIFY(!v.isEmpty());
    QVERIFY(store.view(kKeyC, kUidMe).isEmpty());

    store.setOwn(kKeyB, kUidMe, QStringLiteral("Me"), 1 << proto::Sad, 0);
    store.applyRemote(QString(), kKeyC, kUidSara, QStringLiteral("Sara"), 1, 0);
    // Only our own, only requested, only non-empty: nobody relays anyone else's.
    const auto answer = store.syncAnswer({kKeyA, kKeyC, kKeyB, kKeyA}, kUidMe);
    QCOMPARE(answer.size(), 2);
    QCOMPARE(answer.at(0).key, kKeyA);
    QCOMPARE(answer.at(1).key, kKeyB);
    QCOMPARE(answer.at(1).mask, quint8(1 << proto::Sad));
    QVERIFY(store.syncAnswer({kKeyA}, QString()).isEmpty());
}

void TestProtocol::storePersistence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("reactions.json"));
    const qint64  now  = 1760010000;
    {
        ReactionStore store;
        store.applyRemote(QString(), kKeyA, kUidSara, QStringLiteral("Sara"), 0x03, now * 1000);
        store.setOwn(kKeyA, kUidMe, QStringLiteral("Me"), 0x20, now * 1000);
        store.applyRemote(QString(), kKeyB, kUidReza, QStringLiteral("Reza"), 0x01, now * 1000);
        QVERIFY(store.save(path, now));
    }
    ReactionStore loaded;
    QVERIFY(loaded.load(path, now + 60));
    QCOMPARE(loaded.maskOf(kKeyA, kUidSara), quint8(0x03));
    QCOMPARE(loaded.maskOf(kKeyA, kUidMe), quint8(0x20));
    QCOMPARE(loaded.maskOf(kKeyB, kUidReza), quint8(0x01));
    QCOMPARE(loaded.view(kKeyA, kUidMe).entry(E(0)).others, QStringList{QStringLiteral("Sara")});

    // 31 days later everything is too old.
    ReactionStore later;
    QVERIFY(later.load(path, now + 31LL * 86400));
    QVERIFY(later.keys().isEmpty());

    // A broken file is moved aside and the store starts empty.
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{ not json");
    file.close();
    ReactionStore broken;
    QVERIFY(!broken.load(path, now));
    QVERIFY(!QFile::exists(path));
    QVERIFY(QFile::exists(path + QStringLiteral(".bad")));
    QVERIFY(broken.keys().isEmpty());
    QVERIFY(!broken.load(dir.filePath(QStringLiteral("missing.json")), now));
}

void TestProtocol::storeHostileFile()
{
    const qint64 now = 1760010000;
    // Wrong types, bad keys, bad uids, bad codes, future times: each skipped on its own.
    const QByteArray json = QByteArray(R"({"v":1,"media":{)")
                            + R"("3f9a1c2e4b5d6e7f8a9b":{"seen":1760010000,"by":{"good=":{"n":"Sara","e":"up.party","t":1760010000},)"
                              R"("bad uid":{"n":"x","e":"up","t":1},"q=":{"n":"x","e":"UP","t":1},"w=":{"n":5,"e":"heart","t":"x"},"e=":{"n":"x","e":"","t":1},"s=":"str"}},)"
                              R"("NOTAKEY0000000000000":{"seen":1760010000,"by":{"a=":{"e":"up"}}},)"
                              R"("0a1b2c3d4e5f60718293":{"seen":"yesterday","by":{"a=":{"e":"up"}}},)"
                              R"("aaaaaaaaaaaaaaaaaaaa":{"seen":1760010000,"by":[]},)"
                              R"("bbbbbbbbbbbbbbbbbbbb":[1,2,3]}})";
    ReactionStore store;
    QVERIFY(store.fromJson(json, now));
    QCOMPARE(store.keys(), QStringList{kKeyA});
    QCOMPARE(store.maskOf(kKeyA, QStringLiteral("good=")), quint8(1));
    QCOMPARE(store.maskOf(kKeyA, QStringLiteral("w=")), quint8(1 << proto::Heart)); // a bad name or time is replaced, not trusted
    QCOMPARE(store.reactorCount(kKeyA), 2);

    QVERIFY(!store.fromJson(R"({"v":2,"media":{}})", now));
    QVERIFY(!store.fromJson(R"([1,2])", now));
    QVERIFY(!store.fromJson(R"({"v":1,"media":[]})", now));
    QVERIFY(!store.fromJson(QByteArray(), now));
}

void TestProtocol::storeLimitsOnLoad()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("reactions.json"));
    const qint64  now  = 1760010000;
    // More media than allowed: the least recently changed are dropped.
    QJsonObject media;
    for (int i = 0; i < 10; ++i) {
        QJsonObject reactor{{QStringLiteral("n"), QStringLiteral("P")}, {QStringLiteral("e"), QStringLiteral("up")}, {QStringLiteral("t"), double(now)}};
        media.insert(keyNumber(i), QJsonObject{{QStringLiteral("seen"), double(now - 100 + i)}, {QStringLiteral("by"), QJsonObject{{QStringLiteral("u="), reactor}}}});
    }
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("v"), 1}, {QStringLiteral("media"), media}}).toJson();
    ReactionStore::Limits limits;
    limits.maxMedia = 4;
    ReactionStore store(limits);
    QVERIFY(store.fromJson(json, now));
    QStringList keys = store.keys();
    std::sort(keys.begin(), keys.end());
    QCOMPARE(keys, (QStringList{keyNumber(6), keyNumber(7), keyNumber(8), keyNumber(9)}));

    // Over the size limit: not read, moved aside.
    limits.maxFileBytes = 64;
    ReactionStore small(limits);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(json);
    file.close();
    QVERIFY(!small.load(path, now));
    QVERIFY(small.keys().isEmpty());
    QVERIFY(QFile::exists(path + QStringLiteral(".bad")));
}

void TestProtocol::storeSaveFitsLoadLimit()
{
    // A full store can be larger than the file load() accepts: save() leaves the oldest media out, so
    // the next start reads it instead of moving it aside and starting empty.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("reactions.json"));
    const qint64  now  = 1760010000;
    ReactionStore::Limits limits;
    limits.maxFileBytes = 4096;
    ReactionStore store(limits);
    for (int i = 0; i < 60; ++i) {
        for (int r = 0; r < 4; ++r) {
            const QString uid = QStringLiteral("q0Xn6%1=").arg(i * 10 + r, 22, 10, QLatin1Char('0'));
            QCOMPARE(store.applyRemote(QString(), keyNumber(i), uid, QStringLiteral("A rather long display name %1").arg(r), 0x03, (now - 3600 + i * 10) * 1000),
                     ReactionStore::Apply::Applied);
        }
    }
    QCOMPARE(store.keys().size(), 60);
    QVERIFY(store.toJson(now).size() <= limits.maxFileBytes);
    QVERIFY(store.save(path, now));
    QVERIFY(QFileInfo(path).size() <= limits.maxFileBytes);
    ReactionStore loaded(limits);
    QVERIFY(loaded.load(path, now));
    QVERIFY(!QFile::exists(path + QStringLiteral(".bad")));
    const QStringList keys = loaded.keys();
    QVERIFY(keys.size() > 0 && keys.size() < 60);
    QVERIFY(keys.contains(keyNumber(59))); // the newest are kept
    QVERIFY(!keys.contains(keyNumber(0)));
    QCOMPARE(loaded.reactorCount(keyNumber(59)), 4);
}

// ---- reaction row ---------------------------------------------------------------------------------

void TestProtocol::rowLayout()
{
    const ReactionView none;
    QVERIFY(rx::layoutRow(none, 300, 400, fakeMeasure).isEmpty());
    const rx::RowLayout kept = rx::layoutRow(none, 300, 400, fakeMeasure, true);
    QVERIFY(!kept.isEmpty());
    QVERIFY(kept.pills.isEmpty());
    QCOMPARE(kept.addPill, QRectF(0, rx::kRowTopMargin, rx::kAddPillWidth, rx::kPillHeight));

    // Fixed order (not by count), one line under a wide picture.
    const ReactionView v = viewWith({{proto::Fire, 3}, {proto::ThumbsUp, 120}, {proto::Sad, 1}}, proto::Sad);
    rx::RowLayout      row = rx::layoutRow(v, 300, 400, fakeMeasure);
    QCOMPARE(row.reactions.size(), 3);
    QCOMPARE(row.reactions.at(0), E(proto::ThumbsUp));
    QCOMPARE(row.reactions.at(1), E(proto::Sad));
    QCOMPARE(row.reactions.at(2), E(proto::Fire));
    QCOMPARE(row.size, QSize(300, rx::kRowTopMargin + rx::kPillHeight));
    for (const QRectF& r : row.pills) {
        QCOMPARE(r.top(), qreal(rx::kRowTopMargin));
        QCOMPARE(r.height(), qreal(rx::kPillHeight));
        QVERIFY(r.width() >= 40);
    }
    QCOMPARE(row.pills.at(0).width(), 6 + 16 + 4 + 21.0 + 8); // "99+"
    QVERIFY(row.pills.at(1).left() > row.pills.at(0).right());
    QCOMPARE(row.addPill.left(), row.pills.at(2).right() + rx::kPillGap);

    // A narrow picture: the row may be wider (up to the maximum)...
    const ReactionView all = viewWith({{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}});
    row                    = rx::layoutRow(all, 120, 400, fakeMeasure);
    QCOMPARE(row.size.height(), rx::kRowTopMargin + rx::kPillHeight);
    QVERIFY(row.size.width() > 120 && row.size.width() <= 400);
    // ... and wraps when the maximum is too narrow, never reordering.
    row = rx::layoutRow(all, 120, 150, fakeMeasure);
    QCOMPARE(row.size.width(), 150);
    QVERIFY(row.size.height() >= rx::kRowTopMargin + 2 * rx::kPillHeight + rx::kLineGap);
    for (int i = 1; i < row.pills.size(); ++i) {
        const QRectF a = row.pills.at(i - 1);
        const QRectF b = row.pills.at(i);
        QVERIFY(b.top() > a.top() || b.left() > a.right());
        QVERIFY(b.right() <= 150 + 0.01);
    }
    QVERIFY(row.addPill.bottom() <= row.size.height());

    QCOMPARE(rx::countText(1), QStringLiteral("1"));
    QCOMPARE(rx::countText(99), QStringLiteral("99"));
    QCOMPARE(rx::countText(100), QStringLiteral("99+"));
}

void TestProtocol::rowZones()
{
    const QSizeF        picture(300, 200);
    const ReactionView  v   = viewWith({{proto::Heart, 2}, {proto::Fire, 1}});
    const rx::RowLayout row = rx::layoutRow(v, 300, 400, fakeMeasure);
    using rx::Zone;
    QCOMPARE(rx::zoneAt(picture, row, true, QPointF(10, 10)).zone, Zone::Picture);
    const QRectF button = rx::addButtonRect(picture);
    QCOMPARE(button, QRectF(300 - 8 - 28, 8, 28, 28));
    QCOMPARE(rx::zoneAt(picture, row, true, button.center()).zone, Zone::AddButton);
    QCOMPARE(rx::zoneAt(picture, row, false, button.center()).zone, Zone::Picture); // not shown: the picture
    const rx::ZoneHit heart = rx::zoneAt(picture, row, true, row.pills.at(0).center() + QPointF(0, 200));
    QCOMPARE(heart.zone, Zone::Pill);
    QCOMPARE(heart.index, E(proto::Heart));
    QCOMPARE(rx::zoneAt(picture, row, true, row.pills.at(1).center() + QPointF(0, 200)).index, E(proto::Fire));
    QCOMPARE(rx::zoneAt(picture, row, true, row.addPill.center() + QPointF(0, 200)).zone, Zone::AddPill);
    QCOMPARE(rx::zoneAt(picture, row, true, QPointF(290, 200 + 15)).zone, Zone::Row);  // between and beside the pills
    QCOMPARE(rx::zoneAt(picture, row, true, QPointF(10, 200 + 2)).zone, Zone::Row);    // the top margin
    QCOMPARE(rx::zoneAt(picture, row, true, QPointF(10, 200 + 40)).zone, Zone::None);  // below
    QCOMPARE(rx::zoneAt(picture, rx::RowLayout(), true, QPointF(10, 210)).zone, Zone::None);
    // Tiny pictures get no button.
    QVERIFY(!rx::addButtonFits(QSizeF(50, 30)));
    QCOMPARE(rx::zoneAt(QSizeF(50, 30), rx::RowLayout(), true, QPointF(40, 15)).zone, Zone::Picture);
}

void TestProtocol::rowColours()
{
    for (const bool dark : {false, true}) {
        const QList<QColor> bases = dark ? QList<QColor>{QColor(0x2b, 0x2d, 0x31), QColor(0x36, 0x39, 0x3f), QColor(0x31, 0x33, 0x38), QColor(0x1e, 0x1f, 0x22)}
                                         : QList<QColor>{QColor(Qt::white), QColor(0xf2, 0xf3, 0xf5)};
        for (const QColor& base : bases) {
            for (const rx::ColorPair& pair : rx::colorPairs(dark, base)) {
                const double ratio = ui::contrastRatio(pair.foreground, pair.background);
                QVERIFY2(ratio + 0.005 >= pair.minimum, qPrintable(QStringLiteral("%1 on %2: %3 (%4)").arg(pair.name, base.name()).arg(ratio).arg(pair.minimum)));
            }
        }
    }
}

TSMEDIA_REGISTER_TEST(TestProtocol)

#include "tst_protocol.moc"
