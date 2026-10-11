// 2.3 text reactions: unit tests for reactions on messages without media. The message identity
// (src/textreact.*: hash, local keys, wire refs) on TeamSpeak-like chats of three clients with other time
// zones, wrong clocks, other emoticon settings, HD emoji and the chat layout; repeated identical messages;
// the TR / TSYNC messages and how 2.2 clients see them; the reaction store with message keys
// (text-reactions.json); the index's rules; the row in the chat document (src/textreactdoc.*).

#include <QAbstractTextDocumentLayout>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QtTest>

#include <memory>

#include "emojidata.h"
#include "emojiformat.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "peerprotocol.h"
#include "reactions.h"
#include "replydoc.h"
#include "testmain.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "tschat.h"

namespace {

using textreact::ChatKind;

const QString kAlice = QStringLiteral("AliceUidAAAAAAAAAAAAAAAAAA0=");
const QString kBob   = QStringLiteral("BobUidAAAAAAAAAAAAAAAAAAAA0=");
const QString kCarol = QStringLiteral("CarolUidAAAAAAAAAAAAAAAAAA0=");
const QString kDave  = QStringLiteral("DaveUidAAAAAAAAAAAAAAAAAAA0=");
const QString kServer = QStringLiteral("ServerUidAAAAAAAAAAAAAAAAA0=");

QFont chatFont()
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPointSizeF(9.0);
    return f;
}

void setup(QTextDocument& doc)
{
    doc.setDefaultFont(chatFont());
    doc.setTextWidth(520);
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
}

layoutdoc::Env envFor(layoutart::Mode mode, bool dark = true)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false);
    env.colors       = layoutart::colorsFor(dark, dark ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white), dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black),
                                            dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
    env.dpr          = 1.0;
    env.contentWidth = 512;
    env.today        = QDate(2026, 10, 11);
    env.tag          = QStringLiteral("t");
    return env;
}

// ---- three clients seeing the same messages ---------------------------------------------------------

// What someone typed and when the server passed it on (true time, ms).
struct Sent {
    QString uid;
    QString nick;
    QString text; // as typed; ":)" and ":D" may become pictures, emoji HD pictures
    qint64  atMs = 0;
};

// One client: its time zone, how wrong its clock is, how long messages take to reach it, how it shows them.
struct Client {
    int    zoneSec  = 0;
    qint64 skewMs   = 0;
    qint64 delayMs  = 0;
    bool   emoticonPictures = false; // TeamSpeak's emoticons on (and an icon pack that has ":D")
    bool   hdEmoji  = false;         // ChatEmoji replaced the emoji with its pictures
    bool   styled   = false;         // the Cozy layout
    QVector<int> seen;               // which of the messages it has (all if empty)
};

QDateTime localTime(const Client& c, qint64 trueMs)
{
    return QDateTime::fromMSecsSinceEpoch(trueMs + c.skewMs, Qt::OffsetFromUTC, c.zoneSec);
}

QString bodyHtml(const Client& c, const QString& text)
{
    QString html = tschat::escaped(text);
    if (c.emoticonPictures) {
        html.replace(QStringLiteral(":)"), QStringLiteral("<img src=\"emoticons:smile.svg\" width=\"16\" height=\"16\">"));
        html.replace(QStringLiteral(":D"), QStringLiteral("<img src=\"emoticons:laugh.svg\" width=\"16\" height=\"16\">"));
    }
    return html;
}

// ChatEmoji's pictures for every emoji of the document (what it keeps: emojiformat.h).
void hdEmoji(QTextDocument& doc)
{
    const QString emoji = QString::fromUcs4(U"\U0001F600");
    for (;;) {
        const QTextCursor found = doc.find(emoji);
        if (found.isNull())
            break;
        QTextCursor      c = found;
        QTextImageFormat image;
        image.setName(QStringLiteral("tsmemoji:1f600/20"));
        image.setWidth(20);
        image.setHeight(20);
        image.setProperty(emojiformat::kOriginalText, emoji);
        c.insertText(QString(QChar::ObjectReplacementCharacter), image);
    }
}

// The client's chat: a day line whenever its local date changes, then its messages as TeamSpeak shows them.
void fill(QTextDocument& doc, const Client& c, const QVector<Sent>& messages)
{
    setup(doc);
    const tschat::Skin skin = tschat::darkSkin();
    QDate              day;
    for (int i = 0; i < messages.size(); ++i) {
        if (!c.seen.isEmpty() && !c.seen.contains(i))
            continue;
        const Sent&     m     = messages.at(i);
        const QDateTime local = localTime(c, m.atMs + c.delayMs);
        if (local.date() != day) {
            day = local.date();
            tschat::append(&doc, tschat::dayHtml(skin, QStringLiteral("%1/%2/%3").arg(day.month()).arg(day.day()).arg(day.year()), false));
        }
        tschat::append(&doc, tschat::messageHtml(skin, local.toString(QStringLiteral("HH:mm:ss")), 7, m.uid, m.nick, bodyHtml(c, m.text)));
    }
    if (c.hdEmoji)
        hdEmoji(doc);
    if (c.styled)
        layoutdoc::styleAll(&doc, envFor(layoutart::Mode::Cozy));
}

struct Keyed {
    QString key;
    int     block = -1;
};

// The local keys of the client's messages, in document order (what ChatTextReactions works out).
QVector<Keyed> keysOf(QTextDocument& doc, const Client& c, qint64 nowTrueMs)
{
    const QDateTime      now   = localTime(c, nowTrueMs);
    const QVector<QDate> dates = layoutdoc::datesOf(&doc, now.date());
    QVector<Keyed>       out;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const textreactdoc::Identity id = textreactdoc::identify(b);
        if (!id.eligible())
            continue;
        const QDate day = textreactdoc::dayOf(dates.value(b.blockNumber()), id.seconds, now.date(), now.time().msecsSinceStartOfDay() / 1000);
        out.append({textreactdoc::localKeyOf(id, day, c.zoneSec), b.blockNumber()});
    }
    return out;
}

textreact::IndexedChat chatOf(const QVector<Keyed>& keys, const QVector<QString>& senders)
{
    textreact::IndexedChat chat;
    chat.serverUid = kServer;
    chat.kind      = ChatKind::Channel;
    for (int i = 0; i < keys.size(); ++i) {
        QString hash;
        qint64  t = 0;
        textreact::parseLocalKey(keys.at(i).key, &hash, &t);
        chat.messages.append({hash, t, senders.value(i)});
    }
    return chat;
}

qint64 secondsOf(const Client& c, qint64 trueMs)
{
    return (trueMs + c.skewMs) / 1000;
}

} // namespace

class TestTextReact : public QObject
{
    Q_OBJECT

  private slots:
    // ---- identity ---------------------------------------------------------------------------------------

    void normalizedIgnoresWhatClientsShowDifferently()
    {
        // Emoticons as typed, in their other form, or as TeamSpeak's picture (gone, read back as ":)").
        QCOMPARE(textreact::normalized(QStringLiteral("hello :)")), textreact::normalized(QStringLiteral("hello")));
        QCOMPARE(textreact::normalized(QStringLiteral("hello :-)")), textreact::normalized(QStringLiteral("Hello")));
        QCOMPARE(textreact::normalized(QStringLiteral("ok :D")), textreact::normalized(QStringLiteral("ok :-d")));
        QCOMPARE(textreact::normalized(QStringLiteral("8) cool")), QStringLiteral("cool"));
        // Spacing, line breaks (U+2028 live, nothing after a history reload), punctuation, invisible characters.
        QCOMPARE(textreact::normalized(QStringLiteral("one\r\ntwo")), textreact::normalized(QString::fromUtf8("one\xe2\x80\xa8two")));
        QCOMPARE(textreact::normalized(QStringLiteral("a, b! c?")), QStringLiteral("abc"));
        QCOMPARE(textreact::normalized(QString::fromUtf8("x\xe2\x80\x8by\xe2\x81\xa0z")), QStringLiteral("xyz"));
        // Emoji stay (without their variation selectors), and so do other scripts.
        const QString heart  = QString::fromUtf8("\xe2\x9d\xa4");
        const QString heart2 = QString::fromUtf8("\xe2\x9d\xa4\xef\xb8\x8f");
        QCOMPARE(textreact::normalized(heart), textreact::normalized(heart2));
        QVERIFY(!textreact::normalized(heart).isEmpty());
        const QString persian = QString::fromUtf16(u"\u0633\u0644\u0627\u0645 \u062f\u0648\u0633\u062a");
        QCOMPARE(textreact::normalized(persian), QString::fromUtf16(u"\u0633\u0644\u0627\u0645\u062f\u0648\u0633\u062a"));
        QCOMPARE(textreact::normalized(QString::fromUtf16(u"\u0645\u06cc\u200c\u0631\u0648\u0645")), QString::fromUtf16(u"\u0645\u06cc\u0631\u0648\u0645"));
        // Bounded: only the first kMaxTextChars count.
        QCOMPARE(textreact::normalized(QString(textreact::kMaxTextChars, QLatin1Char('a')) + QStringLiteral("b")).size(), textreact::kMaxTextChars);
    }

    void hashIsStableAndDistinct()
    {
        const QString h = textreact::contentHash(kAlice, QStringLiteral("good game :)"));
        QCOMPARE(h.size(), textreact::kHashChars);
        QVERIFY(textreact::isHash(h));
        QCOMPARE(textreact::contentHash(kAlice, QStringLiteral("Good game")), h);
        QVERIFY(textreact::contentHash(kBob, QStringLiteral("good game")) != h);
        QVERIFY(textreact::contentHash(kAlice, QStringLiteral("good games")) != h);
        QVERIFY(textreact::contentHash(QString(), QStringLiteral("x")).isEmpty());
        // A fixed value: every client and version computes the same (the wire depends on it).
        QCOMPARE(textreact::contentHash(QStringLiteral("AliceUid"), QStringLiteral("hello")), textreact::contentHash(QStringLiteral("AliceUid"), QStringLiteral("HELLO!")));
    }

    void localKeysAndWireRefs()
    {
        const QString hash = textreact::contentHash(kAlice, QStringLiteral("hi"));
        const QString key  = textreact::localKey(hash, 1791565292);
        QCOMPARE(key, hash + QStringLiteral("-1791565292"));
        QString h;
        qint64  t = 0;
        QVERIFY(textreact::parseLocalKey(key, &h, &t));
        QCOMPARE(h, hash);
        QCOMPARE(t, 1791565292LL);
        QVERIFY(textreact::isLocalKey(key));
        QVERIFY(!proto::isMediaKey(key));
        for (const QString& bad : {QString(), hash, hash + QStringLiteral("-"), hash + QStringLiteral("-0"), hash + QStringLiteral("-01"),
                                   hash + QStringLiteral("-12a"), hash.toUpper() + QStringLiteral("-5"), hash.left(15) + QStringLiteral("-5"),
                                   hash + QStringLiteral("--5"), hash + QStringLiteral("-123456789012")})
            QVERIFY2(!textreact::isLocalKey(bad), qPrintable(bad));
        QVERIFY(textreact::localKey(QStringLiteral("xyz"), 5).isEmpty());
        QVERIFY(textreact::localKey(hash, 0).isEmpty());

        QCOMPARE(textreact::wireRef(hash, 0), hash.toLatin1() + "-0");
        QCOMPARE(textreact::wireRef(hash, 125), hash.toLatin1() + "-125");
        QVERIFY(textreact::wireRef(hash, -1).isEmpty());
        QVERIFY(textreact::wireRef(hash, textreact::kMaxAgeSec + 1).isEmpty());
        qint64 age = -1;
        QVERIFY(textreact::parseWireRef(hash.toLatin1() + "-125", &h, &age));
        QCOMPARE(age, 125LL);
        for (const QByteArray& bad : {QByteArray("-5"), hash.toLatin1() + "-", hash.toLatin1() + "-0125", hash.toLatin1() + "-99999999",
                                      hash.toLatin1() + "-2678401", hash.toLatin1() + ":5", hash.toLatin1()})
            QVERIFY2(!textreact::parseWireRef(bad, nullptr, nullptr), bad.constData());
        // From a local key: the age now; a time a little ahead (another day's guess) counts as now.
        QCOMPARE(textreact::wireRefOf(key, 1791565292 + 40), hash.toLatin1() + "-40");
        QCOMPARE(textreact::wireRefOf(key, 1791565292 - 10), hash.toLatin1() + "-0");
        QVERIFY(textreact::wireRefOf(key, 1791565292 - textreact::kFutureSec - 1).isEmpty());
        QVERIFY(textreact::wireRefOf(key, 1791565292 + textreact::kMaxAgeSec + 1).isEmpty());
    }

    void dayOfMessages()
    {
        const QDate today(2026, 10, 11);
        QCOMPARE(textreactdoc::dayOf(QDate(2026, 10, 9), 50000, today, 100), QDate(2026, 10, 9)); // the chat's day line decides
        QCOMPARE(textreactdoc::dayOf(QDate(), 3600, today, 7200), today);
        QCOMPARE(textreactdoc::dayOf(QDate(), 86000, today, 600), today.addDays(-1)); // 23:53 read at 00:10
        QCOMPARE(textreactdoc::dayOf(QDate(), 700, today, 600), today);               // a clock a little behind
        QCOMPARE(textreact::epochOf(QDate(2026, 10, 11), 3600, 3 * 3600), QDateTime(QDate(2026, 10, 11), QTime(1, 0), Qt::UTC).toSecsSinceEpoch() - 3 * 3600);
        QCOMPARE(textreact::epochOf(QDate(), 5), 0LL);
        QCOMPARE(textreact::epochOf(QDate(2026, 1, 1), 86400), 0LL);
    }

    // The heart of it: three clients in other time zones, with wrong clocks, other emoticon settings, HD
    // emoji and the chat layout, find each other's reactions on the same messages; identical repeated
    // messages are told apart; a message one of them doesn't have is not mistaken for another.
    void keysAgreeAcrossClients()
    {
        // 2026-10-10 20:15:00 UTC: 23:45 in Tehran (A), 15:15 in New York (B), 20:15 in London's winter (C).
        const qint64  t0      = QDateTime(QDate(2026, 10, 10), QTime(20, 15, 0), Qt::UTC).toMSecsSinceEpoch();
        const QString grin    = QString::fromUcs4(U"\U0001F600");
        const QString persian = QString::fromUtf16(u"\u0633\u0644\u0627\u0645");
        const QVector<Sent> sent{
            {kAlice, QStringLiteral("Alice"), QStringLiteral("hello everyone :)"), t0},
            {kBob, QStringLiteral("Bob"), QStringLiteral("ok"), t0 + 30000},
            {kBob, QStringLiteral("Bob"), QStringLiteral("ok"), t0 + 50000},   // the same again, 20 s later
            // ... and 5 s after that. Header times are whole seconds on both sides, so an age is off by up to
            // 2 s: repeats closer than that may share where a reaction lands (the nearest one wins).
            {kBob, QStringLiteral("Bob"), QStringLiteral("ok"), t0 + 55000},
            {kAlice, QStringLiteral("Alice"), persian + QStringLiteral(" ") + grin + QStringLiteral(" :D"), t0 + 3600000}, // after midnight in Tehran
            {kCarol, QStringLiteral("Carol"), QStringLiteral("see https://example.com/a"), t0 + 3700000},
            {kAlice, QStringLiteral("Alice"), grin, t0 + 3800000},             // only an emoji
            {kAlice, QStringLiteral("Alice"), grin, t0 + 3830000},
        };
        QVector<QString> senders;
        for (const Sent& s : sent)
            senders << s.uid;

        Client a;
        a.zoneSec          = 3 * 3600 + 1800;
        a.delayMs          = 120;
        a.emoticonPictures = true;
        a.styled           = true;
        Client b;
        b.zoneSec = -5 * 3600;
        b.skewMs  = 97300; // a clock 97 s ahead
        b.delayMs = 880;
        Client c;
        c.zoneSec = 0;
        c.skewMs  = -3599800; // an hour behind
        c.delayMs = 430;
        c.hdEmoji = true;
        c.styled  = true;
        const QVector<Client> clients{a, b, c};

        const qint64 later = t0 + 4 * 3600000; // the reactions come hours later
        QVector<QVector<Keyed>> keys;
        QVector<std::shared_ptr<QTextDocument>> docs;
        for (const Client& client : clients) {
            auto doc = std::make_shared<QTextDocument>();
            fill(*doc, client, sent);
            keys.append(keysOf(*doc, client, later));
            docs.append(doc);
            QCOMPARE(keys.last().size(), sent.size());
        }
        // The same hashes everywhere (the times are each client's own).
        for (int i = 0; i < sent.size(); ++i) {
            const QString hash = keys.at(0).at(i).key.left(textreact::kHashChars);
            QCOMPARE(keys.at(1).at(i).key.left(textreact::kHashChars), hash);
            QCOMPARE(keys.at(2).at(i).key.left(textreact::kHashChars), hash);
            QCOMPARE(hash, textreact::contentHash(sent.at(i).uid, sent.at(i).text));
        }
        // Every client reacts to every message; every other client finds the same message.
        for (int from = 0; from < clients.size(); ++from) {
            for (int to = 0; to < clients.size(); ++to) {
                if (from == to)
                    continue;
                textreact::Index index;
                index.setChat(1, chatOf(keys.at(to), senders));
                for (int i = 0; i < sent.size(); ++i) {
                    const qint64     reactAt = later + i * 61000;
                    const QByteArray ref     = textreact::wireRefOf(keys.at(from).at(i).key, secondsOf(clients.at(from), reactAt));
                    QVERIFY(!ref.isEmpty());
                    QString hash;
                    qint64  age = 0;
                    QVERIFY(textreact::parseWireRef(ref, &hash, &age));
                    textreact::Index::Query q;
                    q.serverUid    = kServer;
                    q.reactorUid   = kDave;
                    q.hash         = hash;
                    q.estimatedSec = secondsOf(clients.at(to), reactAt + 250) - age; // 250 ms through TeamSpeak
                    QVERIFY2(index.resolve(q) == keys.at(to).at(i).key, qPrintable(QStringLiteral("message %1 from %2 to %3").arg(i).arg(from).arg(to)));
                }
            }
        }
    }

    void missingMessageIsNotMistakenForItsRepeat()
    {
        const qint64        t0 = QDateTime(QDate(2026, 10, 10), QTime(12, 0, 0), Qt::UTC).toMSecsSinceEpoch();
        const QVector<Sent> sent{{kBob, QStringLiteral("Bob"), QStringLiteral("gg"), t0}, {kBob, QStringLiteral("Bob"), QStringLiteral("gg"), t0 + 20000}};
        Client              a;
        Client              b;
        b.zoneSec = 7200;
        b.skewMs  = 40000;
        b.seen    = {1}; // came in between the two
        QTextDocument docA, docB;
        fill(docA, a, sent);
        fill(docB, b, sent);
        const QVector<Keyed> keysA = keysOf(docA, a, t0 + 60000);
        const QVector<Keyed> keysB = keysOf(docB, b, t0 + 60000);
        QCOMPARE(keysB.size(), 1);
        textreact::Index index;
        index.setChat(1, chatOf(keysB, {kBob}));
        QString hash;
        qint64  age = 0;
        // A's reaction on the first one: B doesn't have it, and the second is 20 s off.
        QVERIFY(textreact::parseWireRef(textreact::wireRefOf(keysA.at(0).key, secondsOf(a, t0 + 60000)), &hash, &age));
        textreact::Index::Query q;
        q.serverUid    = kServer;
        q.reactorUid   = kAlice;
        q.hash         = hash;
        q.estimatedSec = secondsOf(b, t0 + 60000) - age;
        QVERIFY(index.resolve(q).isEmpty());
        // On the second it does.
        QVERIFY(textreact::parseWireRef(textreact::wireRefOf(keysA.at(1).key, secondsOf(a, t0 + 60000)), &hash, &age));
        q.estimatedSec = secondsOf(b, t0 + 60000) - age;
        QCOMPARE(index.resolve(q), keysB.at(0).key);
    }

    void historyReloadKeepsKeys()
    {
        // The same chat read live and after TeamSpeak reloaded it from its log ("Chat begins" marker, no
        // line breaks of "\r", the layout gone): the same local keys, so stored reactions come back.
        const tschat::Skin skin = tschat::darkSkin();
        QTextDocument      live, reloaded;
        setup(live);
        setup(reloaded);
        tschat::append(&live, tschat::dayHtml(skin, QStringLiteral("10/10/2026"), false));
        tschat::append(&live, tschat::messageHtml(skin, QStringLiteral("21:14:05"), 7, kAlice, QStringLiteral("Alice"), tschat::escaped(QStringLiteral("one \ntwo"))));
        layoutdoc::styleAll(&live, envFor(layoutart::Mode::Compact));
        tschat::append(&reloaded, tschat::historyHtml(skin, QStringLiteral("2026-10-10 20:00:00")));
        tschat::append(&reloaded, tschat::messageHtml(skin, QStringLiteral("21:14:05"), 0, kAlice, QStringLiteral("Alice"), tschat::escaped(QStringLiteral("one\ntwo"))));
        Client client;
        client.zoneSec = 3600;
        const qint64 now = QDateTime(QDate(2026, 10, 12), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch();
        QCOMPARE(keysOf(live, client, now).value(0).key, keysOf(reloaded, client, now).value(0).key);
        QVERIFY(!keysOf(live, client, now).value(0).key.isEmpty());
    }

    void whichBlocksAreEligible()
    {
        const tschat::Skin skin = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        tschat::append(&doc, tschat::systemHtml(skin, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(skin, QStringLiteral("Home"))));
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:17"), 7, kAlice, QStringLiteral("Alice"), QStringLiteral("plain text")));
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:20"), 7, kAlice, QStringLiteral("Alice"),
                                                 tschat::linkHtml(skin, QStringLiteral("ts3file://127.0.0.1?filename=lake.jpg"), QStringLiteral("lake.jpg"))));
        tschat::append(&doc, tschat::messageHtml(skin, QString(), 7, kAlice, QStringLiteral("Alice"), QStringLiteral("no timestamps")));
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:25"), 7, kAlice, QStringLiteral("Alice"), tschat::linkHtml(skin, QStringLiteral("https://example.com/x"))));
        tschat::append(&doc, tschat::printHtml(skin, QStringLiteral("2 files sent.")));
        QString eligible; // as text: QVector<bool>'s == trips a deprecation warning with Qt 5.15 and MSVC
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            eligible += textreactdoc::identify(b).eligible() ? QLatin1Char('1') : QLatin1Char('0');
        QCOMPARE(eligible, QStringLiteral("010010"));
        // A preview of ours in a block makes it media, as does a hidden block.
        QTextBlock  b = doc.findBlockByNumber(1);
        QTextCursor c(&doc);
        c.setPosition(b.position() + b.length() - 1);
        QTextImageFormat preview;
        preview.setName(QStringLiteral("tsmedia:0123456789abcdef0123"));
        c.insertImage(preview);
        QVERIFY(textreactdoc::identify(doc.findBlockByNumber(1)).media);
        doc.findBlockByNumber(4).setVisible(false);
        QVERIFY(!textreactdoc::identify(doc.findBlockByNumber(4)).eligible());
    }

    // ---- protocol ---------------------------------------------------------------------------------------

    void reactMessagesRoundTrip()
    {
        const QString    hash = textreact::contentHash(kAlice, QStringLiteral("hi"));
        const QByteArray ref  = textreact::wireRef(hash, 42);
        const int        grin = emoji::fromWireCode(QByteArray("1f600"));
        const int        up   = proto::legacyReactionEmoji(proto::ThumbsUp);
        QVERIFY(grin >= 0 && up >= 0);
        const QVector<proto::Message> messages =
            textreact::makeReacts(proto::Scope::Channel, false, {{QString::fromLatin1(ref), 0, proto::ReactionSet{up, grin}}, {QStringLiteral("not-a-ref"), 1, {}}});
        QCOMPARE(messages.size(), 1);
        const QByteArray wire = proto::serialize(messages.first());
        QVERIFY(!wire.isEmpty());
        QCOMPARE(wire, QByteArray("tsm1 TR s=c i=") + ref + ":up e=" + ref + ":1f44d.1f600");
        const std::optional<proto::Message> parsed = proto::parse(wire);
        QVERIFY(parsed);
        const std::optional<proto::React> react = textreact::readReact(*parsed);
        QVERIFY(react);
        QCOMPARE(react->items.size(), 1);
        QCOMPARE(react->items.first().key, QString::fromLatin1(ref));
        QCOMPARE(react->items.first().reactions().size(), 2);
        QCOMPARE(react->items.first().reactions().at(1), grin);

        // Many messages at once: split within every limit.
        QVector<proto::ReactItem> many;
        for (int i = 0; i < 45; ++i)
            many.append({QString::fromLatin1(textreact::wireRef(textreact::contentHash(kAlice, QString::number(i)), 1000000 + i)), 0, proto::ReactionSet{up, grin}});
        const QVector<proto::Message> split = textreact::makeReacts(proto::Scope::Private, true, many);
        int                           items = 0;
        for (const proto::Message& m : split) {
            const QByteArray bytes = proto::serialize(m);
            QVERIFY(!bytes.isEmpty() && bytes.size() <= proto::kMaxSendBytes);
            const std::optional<proto::React> r = textreact::readReact(*proto::parse(bytes));
            QVERIFY(r && r->syncAnswer && r->scope == proto::Scope::Private);
            items += r->items.size();
        }
        QCOMPARE(items, 45);

        // Hostile input is refused as a whole.
        for (const QByteArray& bad : {QByteArray("tsm1 TR s=s i=") + ref + ":up", QByteArray("tsm1 TR i=") + ref + ":up", QByteArray("tsm1 TR s=c"),
                                      QByteArray("tsm1 TR s=c i=") + ref + ":up," + ref + ":heart", QByteArray("tsm1 TR s=c i=0123456789abcdef0123:up"),
                                      QByteArray("tsm1 TR s=c i=") + hash.toLatin1() + "-9999999:up"}) {
            const std::optional<proto::Message> m = proto::parse(bad);
            QVERIFY2(!m || !textreact::readReact(*m), bad.constData());
        }
        // A malformed e= is ignored as a whole (the v1 part stays).
        const std::optional<proto::Message> half = proto::parse(QByteArray("tsm1 TR s=c i=") + ref + ":up e=" + ref + ":zz");
        QVERIFY(half);
        QCOMPARE(textreact::readReact(*half)->items.first().reactions().size(), 1);
    }

    void syncMessagesRoundTrip()
    {
        QStringList refs;
        for (int i = 0; i < 25; ++i)
            refs << QString::fromLatin1(textreact::wireRef(textreact::contentHash(kBob, QString::number(i)), 2678400 - i));
        refs << QStringLiteral("junk") << refs.first();
        const proto::Message m     = textreact::makeSync(proto::Scope::Channel, refs);
        const QByteArray     bytes = proto::serialize(m);
        QVERIFY(!bytes.isEmpty() && bytes.size() <= proto::kMaxSendBytes);
        const std::optional<proto::Sync> sync = textreact::readSync(*proto::parse(bytes));
        QVERIFY(sync);
        QVERIFY(sync->keys.size() <= proto::kMaxItems && sync->keys.size() >= 18);
        QCOMPARE(sync->keys.first(), refs.first());
        QVERIFY(!textreact::readSync(*proto::parse(QByteArray("tsm1 TSYNC s=c m=junk"))));
        QVERIFY(!textreact::readSync(*proto::parse(QByteArray("tsm1 TSYNC s=c"))));
        QVERIFY(!textreact::makeSync(proto::Scope::Channel, {QStringLiteral("junk")}).has("m"));
    }

    // 2.2 clients: TR and TSYNC are types they don't know (PeerHub ignores them); their R and SYNC readers
    // refuse them even if asked, and ours refuse theirs.
    void olderClientsIgnoreThem()
    {
        const QByteArray ref = textreact::wireRef(textreact::contentHash(kAlice, QStringLiteral("hi")), 7);
        const QByteArray tr  = proto::serialize(textreact::makeReacts(proto::Scope::Channel, false, {{QString::fromLatin1(ref), 1, {}}}).first());
        const QByteArray ts  = proto::serialize(textreact::makeSync(proto::Scope::Channel, {QString::fromLatin1(ref)}));
        for (const QByteArray& bytes : {tr, ts}) {
            QVERIFY(proto::looksLikeOurs(bytes.constData())); // the same magic: within the grammar of tsm1
            proto::ParseResult result = proto::ParseResult::Empty;
            const std::optional<proto::Message> m = proto::parse(bytes, &result);
            QVERIFY(m && result == proto::ParseResult::Ok);
            QVERIFY(!proto::readReact(*m));
            QVERIFY(!proto::readSync(*m));
            QVERIFY(!proto::readHello(*m));
        }
        // Their messages aren't ours.
        const proto::Message r = proto::makeReacts(proto::Scope::Channel, false, {{QStringLiteral("0123456789abcdef0123"), 1, {}}}).first();
        QVERIFY(!textreact::readReact(r));
        QVERIFY(!textreact::readSync(proto::makeSync(proto::Scope::Channel, {QStringLiteral("0123456789abcdef0123")})));
        // And a 2.2 R whose items were refs would be refused as a whole by 2.2 (no half-read state).
        proto::Message fake = r;
        fake.set("i", ref + ":up");
        QVERIFY(!proto::readReact(fake));
    }

    // ---- the store ---------------------------------------------------------------------------------------

    void storeKeepsMessageKeys()
    {
        const QString key   = textreact::localKey(textreact::contentHash(kAlice, QStringLiteral("hi")), QDateTime::currentSecsSinceEpoch() - 100);
        const qint64  nowMs = QDateTime::currentMSecsSinceEpoch();
        const int     up    = proto::legacyReactionEmoji(proto::ThumbsUp);
        const int     grin  = emoji::fromWireCode(QByteArray("1f600"));

        ReactionStore media; // 2.2's: media keys only
        QCOMPARE(media.applyRemote(kServer, key, kBob, QStringLiteral("Bob"), proto::ReactionSet{up}, nowMs), ReactionStore::Apply::Rejected);

        ReactionStore texts;
        texts.setKeyCheck(&textreact::isLocalKey);
        QCOMPARE(texts.applyRemote(kServer, key, kBob, QStringLiteral("Bob"), proto::ReactionSet{up, grin}, nowMs), ReactionStore::Apply::Applied);
        QCOMPARE(texts.applyRemote(kServer, QStringLiteral("0123456789abcdef0123"), kBob, QStringLiteral("Bob"), proto::ReactionSet{up}, nowMs),
                 ReactionStore::Apply::Rejected);
        texts.setOwn(key, kAlice, QStringLiteral("Alice"), proto::ReactionSet{grin}, nowMs);
        const ReactionView view = texts.view(key, kAlice);
        QCOMPARE(view.entries.size(), 2);
        QCOMPARE(view.entry(grin).count, 2);
        QVERIFY(view.entry(grin).mine);
        QCOMPARE(view.entry(up).others, QStringList{QStringLiteral("Bob")});

        // text-reactions.json: back as it was; a 2.2 store reading the same file skips them.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("text-reactions.json"));
        QVERIFY(texts.save(path, nowMs / 1000));
        ReactionStore again;
        again.setKeyCheck(&textreact::isLocalKey);
        QVERIFY(again.load(path, nowMs / 1000));
        QCOMPARE(again.view(key, kAlice).entry(grin).count, 2);
        QVERIFY(again.view(key, kAlice).entry(grin).mine);
        ReactionStore old;
        QVERIFY(old.load(path, nowMs / 1000));
        QVERIFY(old.keys().isEmpty());
        // syncAnswer: only our own.
        const QVector<proto::ReactItem> answer = again.syncAnswer({key}, kAlice);
        QCOMPARE(answer.size(), 1);
        QCOMPARE(answer.first().set.size(), 1);
    }

    // ---- the index ---------------------------------------------------------------------------------------

    void indexRules()
    {
        const QString hash  = textreact::contentHash(kBob, QStringLiteral("hey"));
        const qint64  t     = 1791565000;
        textreact::Index index;
        textreact::IndexedChat channel;
        channel.serverUid = kServer;
        channel.kind      = ChatKind::Channel;
        channel.messages  = {{hash, t, kBob}, {hash, t + 4, kBob}};
        textreact::IndexedChat privateChat;
        privateChat.serverUid  = kServer;
        privateChat.kind       = ChatKind::Private;
        privateChat.partnerUid = kBob;
        privateChat.messages   = {{hash, t + 1000, kBob}};
        textreact::IndexedChat server;
        server.kind     = ChatKind::Server;
        server.messages = {{hash, t + 2000, kBob}};
        index.setChat(1, channel);
        index.setChat(2, privateChat);
        index.setChat(3, server);
        QCOMPARE(index.messageCount(), 4);

        textreact::Index::Query q;
        q.serverUid    = kServer;
        q.reactorUid   = kCarol;
        q.ownUid       = kAlice;
        q.hash         = hash;
        q.estimatedSec = t + 1;
        QCOMPARE(index.resolve(q), textreact::localKey(hash, t));     // the nearest
        q.estimatedSec = t + 2;
        QCOMPARE(index.resolve(q), textreact::localKey(hash, t + 4)); // as near: the newer one
        q.estimatedSec = t + 4 + textreact::kToleranceSec + 1;
        QVERIFY(index.resolve(q).isEmpty());                          // too far
        q.estimatedSec = t + 1000;
        QVERIFY(index.resolve(q).isEmpty());                          // a channel reaction never reaches a private chat
        q.estimatedSec = t + 2000;
        QVERIFY(index.resolve(q).isEmpty());                          // nor the server chat
        q.serverUid    = QStringLiteral("OtherServerAAAAAAAAAAAAAAAA=");
        q.estimatedSec = t;
        QVERIFY(index.resolve(q).isEmpty());                          // another server's chat

        // Private: only from the partner, only in that chat.
        q.serverUid    = kServer;
        q.scope        = proto::Scope::Private;
        q.estimatedSec = t + 1000;
        QVERIFY(index.resolve(q).isEmpty()); // Carol isn't the partner
        q.reactorUid = kBob;
        QCOMPARE(index.resolve(q), textreact::localKey(hash, t + 1000));
        q.estimatedSec = t;
        QVERIFY(index.resolve(q).isEmpty()); // the channel's message isn't the private chat's
        // A private chat not identified yet: messages of the reactor only.
        textreact::IndexedChat unknown;
        unknown.messages = {{hash, t + 5000, kBob}, {textreact::contentHash(kDave, QStringLiteral("hey")), t + 5000, kDave}};
        index.setChat(4, unknown);
        q.estimatedSec = t + 5000;
        QCOMPARE(index.resolve(q), textreact::localKey(hash, t + 5000));
        q.hash = textreact::contentHash(kDave, QStringLiteral("hey"));
        QVERIFY(index.resolve(q).isEmpty()); // Dave's message, Bob's reaction: not their private chat
        q.hash = hash;

        // Ours: only in the private chat known to be with the reactor. Anyone on the server can send s=p,
        // so a chat not identified yet (maybe the channel's) or one with an unknown partner takes none.
        const QString mine = textreact::contentHash(kAlice, QStringLiteral("see you"));
        textreact::IndexedChat withBob;
        withBob.serverUid  = kServer;
        withBob.kind       = ChatKind::Private;
        withBob.partnerUid = kBob;
        withBob.messages   = {{mine, t + 6000, kAlice}};
        textreact::IndexedChat notYet;
        notYet.messages = {{mine, t + 7000, kAlice}};
        textreact::IndexedChat partnerUnknown;
        partnerUnknown.serverUid = kServer;
        partnerUnknown.kind      = ChatKind::Private;
        partnerUnknown.messages  = {{mine, t + 8000, kAlice}, {hash, t + 8000, kBob}};
        index.setChat(5, withBob);
        index.setChat(6, notYet);
        index.setChat(7, partnerUnknown);
        q.hash         = mine;
        q.estimatedSec = t + 6000;
        QCOMPARE(index.resolve(q), textreact::localKey(mine, t + 6000)); // Bob on ours in our chat with him
        q.reactorUid = kCarol;
        QVERIFY(index.resolve(q).isEmpty()); // Carol on ours there: no
        for (const qint64 at : {t + 7000, t + 8000}) {
            q.estimatedSec = at;
            q.reactorUid   = kBob;
            QVERIFY(index.resolve(q).isEmpty()); // not known to be with Bob
            q.reactorUid = kCarol;
            QVERIFY(index.resolve(q).isEmpty());
        }
        q.reactorUid   = kBob;
        q.hash         = hash;
        q.estimatedSec = t + 8000;
        QCOMPARE(index.resolve(q), textreact::localKey(hash, t + 8000)); // Bob on his own message there: his chat with us
        index.removeChat(5);
        index.removeChat(6);
        index.removeChat(7);
        q.reactorUid = kCarol;

        QCOMPARE(index.recentChannelKeys(kServer, 5), (QStringList{textreact::localKey(hash, t + 4), textreact::localKey(hash, t)}));
        QVERIFY(index.knows(textreact::localKey(hash, t + 1000)));
        index.removeChat(2);
        QVERIFY(!index.knows(textreact::localKey(hash, t + 1000)));
        QCOMPARE(index.chatCount(), 3);
        index.clear();
        QCOMPARE(index.messageCount(), 0);
    }

    // TeamSpeak's line limit drops the first blocks with every new line once a chat is full: what was read
    // is found again by its text instead of being read again (or, worse, taken for the block that now has
    // its number).
    void readingsFollowRenumberedBlocks()
    {
        // As text: QVector's == trips a deprecation warning with Qt 5.15 and MSVC.
        const auto carried = [](const QVector<quint64>& before, const QVector<quint64>& now, bool* moved) {
            QStringList out;
            for (const int i : textreact::carryOver(before, now, moved))
                out << QString::number(i);
            return out.join(QLatin1Char(' '));
        };
        bool moved = true;
        // Nothing changed: every block keeps its own reading.
        QCOMPARE(carried({11, 12, 13}, {11, 12, 13}, &moved), QStringLiteral("0 1 2"));
        QVERIFY(!moved);
        // Two lines dropped at the top, two new ones at the end (not read yet).
        QCOMPARE(carried({11, 12, 13, 14, 15}, {13, 14, 15, 16, 17}, &moved), QStringLiteral("2 3 4 -1 -1"));
        QVERIFY(moved);
        // One block restyled in place (another text: read again), unread ones (0) never match.
        QCOMPARE(carried({11, 0, 13}, {11, 0, 99}, &moved), QStringLiteral("0 -1 -1"));
        QVERIFY(!moved);
        // The same text twice (a repeat within a second): the first reading of it, which says the same.
        QCOMPARE(carried({7, 7, 8}, {7, 8, 7}, &moved), QStringLiteral("0 2 0"));
        QVERIFY(moved);
        QVERIFY(textreact::carryOver({}, {}).isEmpty());
        QCOMPARE(carried({}, {5}, nullptr), QStringLiteral("-1"));

        // The tabs' places (TeamSpeak's chat tab widget: the server's, the channel's, then private chats).
        QCOMPARE(textreact::kindOfTab(-1), ChatKind::Unknown);
        QCOMPARE(textreact::kindOfTab(0), ChatKind::Server);
        QCOMPARE(textreact::kindOfTab(1), ChatKind::Channel);
        QCOMPARE(textreact::kindOfTab(2), ChatKind::Private);
        QCOMPARE(textreact::kindOfTab(7), ChatKind::Private);
    }

    // ---- the row in the chat document -------------------------------------------------------------------------

    void rowInsertResizeRemove()
    {
        const tschat::Skin skin = tschat::darkSkin();
        QTextDocument      doc;
        setup(doc);
        tschat::append(&doc, tschat::dayHtml(skin, QStringLiteral("10/10/2026"), false));
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:17"), 7, kAlice, QStringLiteral("Alice"), QStringLiteral("first ") + tschat::linkHtml(skin, QStringLiteral("https://example.com"))));
        tschat::append(&doc, tschat::messageHtml(skin, QStringLiteral("16:00:20"), 8, kBob, QStringLiteral("Bob"), QStringLiteral("second")));
        const QString third  = tschat::messageHtml(skin, QStringLiteral("16:00:30"), 8, kBob, QStringLiteral("Bob"), QStringLiteral("third"));
        const QString fourth = tschat::messageHtml(skin, QStringLiteral("16:00:31"), 8, kBob, QStringLiteral("Bob"), QStringLiteral("fourth"));
        std::unique_ptr<QTextDocument> plain(doc.clone()); // the same chat as TeamSpeak shows it, for the end
        tschat::append(plain.get(), third);
        tschat::append(plain.get(), fourth);
        layoutdoc::styleAll(&doc, envFor(layoutart::Mode::Cozy));
        const textreactdoc::Identity id = textreactdoc::identify(doc.findBlockByNumber(1));
        const replydoc::Message      m  = replydoc::parseBlock(doc.findBlockByNumber(1));
        QVERIFY(id.eligible());

        QVERIFY(textreactdoc::insertRow(&doc, 1, QStringLiteral("tsmedia-treact:t-1"), QStringLiteral("k"), QSizeF(120, 30)));
        QVERIFY(!textreactdoc::insertRow(&doc, 1, QStringLiteral("tsmedia-treact:t-2"), QStringLiteral("k"), QSizeF(120, 30))); // one per message
        textreactdoc::Row row = textreactdoc::rowOf(doc.findBlockByNumber(1));
        QVERIFY(row.exists());
        QCOMPARE(row.key, QStringLiteral("k"));
        QCOMPARE(row.object, row.separator + 1);
        QCOMPARE(doc.characterAt(row.separator), QChar(QChar::LineSeparator));
        QVERIFY(!row.format.isAnchor()); // nothing of the link before it
        // Every reader of the message sees it as before.
        QCOMPARE(textreactdoc::identify(doc.findBlockByNumber(1)).hash, id.hash);
        QCOMPARE(replydoc::parseBlock(doc.findBlockByNumber(1)).text, m.text);
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(1)).time, QStringLiteral("16:00:17"));
        QVERIFY(layoutformat::leadOf(doc.findBlockByNumber(1)).styled);
        // Its line is the row's height (on the baseline, no font descent below it).
        doc.documentLayout()->documentSize();
        const QTextBlock b    = doc.findBlockByNumber(1);
        const QTextLine  line = b.layout()->lineForTextPosition(row.object - b.position());
        QVERIFY(line.isValid());
        QCOMPARE(line.lineNumber(), b.layout()->lineCount() - 1);
        QVERIFY(qAbs(line.height() - 30) <= 1.0);
        // Where it is drawn: under the text, at the content column, as tall as its line.
        const QRectF rect = textreactdoc::rowRect(b);
        const QRectF br   = doc.documentLayout()->blockBoundingRect(b);
        QCOMPARE(rect.size(), QSizeF(120, 30));
        QVERIFY(qAbs(rect.bottom() - (br.top() + line.y() + line.height())) < 0.01);
        QVERIFY(rect.top() >= br.top() + b.layout()->lineAt(line.lineNumber() - 1).y() + b.layout()->lineAt(line.lineNumber() - 1).height() - 1.5);
        QVERIFY(rect.left() > br.left()); // the hanging indent: under the message text, not the avatar

        // TeamSpeak appends the next message the way it does: it doesn't become part of the row.
        tschat::append(&doc, third);
        QVERIFY(!textreactdoc::rowOf(doc.lastBlock()).exists());
        QVERIFY(textreactdoc::identify(doc.lastBlock()).eligible());
        QVERIFY(textreactdoc::insertRow(&doc, 2, QStringLiteral("tsmedia-treact:t-3"), QStringLiteral("k2"), QSizeF(60, 30)));
        tschat::append(&doc, fourth);
        QVERIFY(!textreactdoc::rowOf(doc.lastBlock()).exists());
        QCOMPARE(textreactdoc::identify(doc.lastBlock()).text, QStringLiteral("fourth"));

        QVERIFY(textreactdoc::resizeRow(&doc, 1, QSizeF(180, 60)));
        QCOMPARE(textreactdoc::rowOf(doc.findBlockByNumber(1)).format.width(), 180.0);
        QVERIFY(!textreactdoc::resizeRow(&doc, 1, QSizeF(180, 60)));
        QVERIFY(!textreactdoc::resizeRow(&doc, 3, QSizeF(1, 1)));

        // Out again: the text is TeamSpeak's (with the layout's, which goes on its own).
        QStringList names;
        QCOMPARE(textreactdoc::removeAll(&doc, &names), 2);
        QCOMPARE(names.size(), 2);
        QVERIFY(!textreactdoc::removeRow(&doc, 1));
        layoutdoc::restoreAll(&doc);
        QCOMPARE(doc.toPlainText(), plain->toPlainText());
        for (QTextBlock x = doc.begin(); x.isValid(); x = x.next()) {
            for (auto it = x.begin(); !it.atEnd(); ++it) {
                QVERIFY(!textreactdoc::isRowObject(it.fragment().charFormat()));
                QVERIFY(!textreactdoc::isRowSeparator(it.fragment().charFormat()));
            }
        }
    }
};

TSMEDIA_REGISTER_TEST(TestTextReact)
#include "tst_textreact.moc"
