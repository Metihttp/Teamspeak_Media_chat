// 2.2 emoji: unit tests for any-emoji reactions (src/peerprotocol.* e=, src/reactions.* sets and order,
// reactions.json "x"), the picker's preferences (src/emojiprefs.*) and the chat document part of
// src/chatemoji.* (replacing, jumbo, what stays untouched, restoring, copying) on a TeamSpeak-like document.

#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QtTest>

#include <initializer_list>

#include "chatemoji.h"
#include "emojidata.h"
#include "emojiprefs.h"
#include "emojirender.h"
#include "peerprotocol.h"
#include "reactions.h"
#include "settings.h"
#include "testmain.h"

namespace {

const QString kKeyA = QStringLiteral("0123456789abcdef0123");
const QString kKeyB = QStringLiteral("fedcba9876543210fedc");
const QString kSara = QStringLiteral("SaraUidAAAAAAAAAAAAAAAAAAAA=");
const QString kReza = QStringLiteral("RezaUidAAAAAAAAAAAAAAAAAAAA=");
const QString kMe   = QStringLiteral("MeUidAAAAAAAAAAAAAAAAAAAAAA=");

int id(const char* code)
{
    return emoji::fromWireCode(QByteArray(code));
}

int E(int index)
{
    return proto::legacyReactionEmoji(index);
}

// Sets compared as text ("12,7"): QVector<int>'s == trips a deprecation warning with Qt 5.15 and MSVC.
QString S(const QVector<int>& ids)
{
    QStringList out;
    for (int i : ids)
        out << QString::number(i);
    return out.join(QLatin1Char(','));
}

// A message the way TeamSpeak 3.6.2 builds it (S0 h): icon, "<time>", nickname link, ": text".
void message(QTextDocument* doc, const QString& nick, const QString& text)
{
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (!doc->isEmpty())
        c.insertBlock();
    QTextImageFormat icon;
    icon.setName(QStringLiteral("iconpath:MESSAGE_INCOMING?size=13x13"));
    c.insertImage(icon);
    c.insertText(QStringLiteral("<20:02:13> "), QTextCharFormat());
    QTextCharFormat link;
    link.setAnchor(true);
    link.setAnchorHref(QStringLiteral("client://17/KpNqZMq7js/JajRo+3zOFPViX1E=~") + nick);
    c.insertText(QLatin1Char('"') + nick + QLatin1Char('"'), link);
    c.insertText(QStringLiteral(": "), QTextCharFormat());
    for (const QString& part : text.split(QLatin1Char('|'))) {
        if (part.startsWith(QLatin1String("emoticons:"))) {
            QTextImageFormat emo;
            emo.setName(part);
            c.insertImage(emo);
        } else {
            c.insertText(part, QTextCharFormat());
        }
    }
}

QString formatsOf(QTextDocument* doc)
{
    QString out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            out += f.text() + QLatin1Char('|') + (f.charFormat().isImageFormat() ? f.charFormat().toImageFormat().name() : QString()) + QLatin1Char('|')
                   + f.charFormat().anchorHref() + QLatin1Char('\n');
        }
    }
    return out;
}

// The size of the HD emoji at position.
qreal emojiWidthAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat().isImageFormat() ? c.charFormat().toImageFormat().width() : -1;
}

} // namespace

class TestEmojiReact : public QObject
{
    Q_OBJECT

  private slots:
    // ---- the protocol ---------------------------------------------------------------------------------
    void legacyMapping()
    {
        const char* const codes[] = {"1f44d", "2764-fe0f", "1f602", "1f62e", "1f622", "1f525"};
        for (int i = 0; i < proto::kReactionCount; ++i) {
            QCOMPARE(E(i), id(codes[i]));
            QCOMPARE(proto::legacyReactionIndex(E(i)), i);
        }
        QCOMPARE(proto::legacyReactionEmoji(-1), -1);
        QCOMPARE(proto::legacyReactionEmoji(6), -1);
        QCOMPARE(proto::legacyReactionIndex(id("1f923")), -1);
        QCOMPARE(proto::legacyMask(proto::setFromMask(proto::kAllReactions)), proto::kAllReactions);
        QCOMPARE(S(proto::setFromMask(0x05)), S((proto::ReactionSet{E(0), E(2)})));
    }

    void reactionsWithAnyEmoji()
    {
        // 🤣 then 👍: i= keeps the v1 part, e= the whole set in its order.
        const proto::ReactionSet set{id("1f923"), E(proto::ThumbsUp)};
        const QVector<proto::Message> messages = proto::makeReacts(proto::Scope::Channel, false, {{kKeyA, 0, set}});
        QCOMPARE(messages.size(), 1);
        const QByteArray wire = proto::serialize(messages.first());
        QCOMPARE(wire, QByteArray("tsm1 R s=c i=") + kKeyA.toLatin1() + ":up e=" + kKeyA.toLatin1() + ":1f923.1f44d");

        // What a 2.2.0 receiver does with it: the grammar is fine and i= reads as thumbs up.
        proto::ParseResult result = proto::ParseResult::Empty;
        const std::optional<proto::Message> parsed = proto::parse(wire, &result);
        QCOMPARE(result, proto::ParseResult::Ok);
        quint8 mask = 0;
        QVERIFY(proto::codesToMask(parsed->value("i").mid(21), &mask));
        QCOMPARE(mask, quint8(1 << proto::ThumbsUp));

        // A 2.2 emoji receiver: the whole set, in order.
        const std::optional<proto::React> react = proto::readReact(*parsed);
        QVERIFY(react);
        QCOMPARE(react->items.size(), 1);
        QCOMPARE(S(react->items.first().set), S(set));
        QCOMPARE(react->items.first().mask, quint8(1 << proto::ThumbsUp));
        QCOMPARE(S(react->items.first().reactions()), S(set));

        // Only other emoji: i= says "none of the six" (a 2.2.0 receiver removes the reactor there).
        const QByteArray onlyOthers = proto::serialize(proto::makeReacts(proto::Scope::Private, false, {{kKeyB, 0, {id("1f389")}}}).first());
        QVERIFY(onlyOthers.contains(QByteArray(" i=") + kKeyB.toLatin1() + ": "));
        QVERIFY(onlyOthers.endsWith(QByteArray(" e=") + kKeyB.toLatin1() + ":1f389"));
        // Only the six: no e= at all (as 2.2.0 sent it).
        const QByteArray classic = proto::serialize(proto::makeReacts(proto::Scope::Channel, false, {{kKeyA, 0x03, {}}}).first());
        QVERIFY(!classic.contains(" e="));
        QVERIFY(classic.endsWith(":up.heart"));
    }

    void oldMessagesStillRead()
    {
        const std::optional<proto::Message> m = proto::parse(QByteArray("tsm1 R s=c i=") + kKeyA.toLatin1() + ":heart.fire");
        const std::optional<proto::React>   r = proto::readReact(*m);
        QVERIFY(r);
        QCOMPARE(S(r->items.first().set), S((proto::ReactionSet{E(proto::Heart), E(proto::Fire)})));
        // Unknown v1-style codes (a newer version's) are still left out, not refused.
        const std::optional<proto::React> newer = proto::readReact(*proto::parse(QByteArray("tsm1 R s=c i=") + kKeyA.toLatin1() + ":up.zap"));
        QCOMPARE(S(newer->items.first().set), S(proto::ReactionSet{E(proto::ThumbsUp)}));
    }

    void hostileEmojiField_data()
    {
        QTest::addColumn<QByteArray>("e");
        QTest::addColumn<bool>("used"); // false: ignored as a whole (i= counts)
        const QByteArray a = kKeyA.toLatin1();
        QTest::newRow("valid") << a + ":1f923" << true;
        QTest::newRow("unknown emoji left out") << a + ":1faff.1f923" << true;
        QTest::newRow("bad code") << a + ":zz" << false;
        QTest::newRow("upper case") << a + ":1F923" << false;
        QTest::newRow("leading zero") << a + ":01f923" << false;
        QTest::newRow("empty code") << a + ":1f923..1f44d" << false;
        QTest::newRow("too many code points") << a + ":1-2-3-4-5-6-7-8-9-a-b" << false;
        QTest::newRow("surrogate") << a + ":d83d" << false;
        QTest::newRow("no colon") << a << false;
        QTest::newRow("bad key") << QByteArray("xyz:1f923") << false;
        QTest::newRow("key twice") << a + ":1f923," + a + ":1f44d" << false;
        QByteArray many = a + ':';
        for (int i = 0; i < 17; ++i)
            many += QByteArray(i ? "." : "") + "1f600";
        QTest::newRow("17 codes") << many << false;
    }

    void hostileEmojiField()
    {
        QFETCH(QByteArray, e);
        QFETCH(bool, used);
        const QByteArray wire = QByteArray("tsm1 R s=c i=") + kKeyA.toLatin1() + ":up e=" + e;
        const std::optional<proto::Message> m = proto::parse(wire);
        if (!m)
            return; // refused by the grammar already: older and newer receivers alike
        const std::optional<proto::React> r = proto::readReact(*m);
        QVERIFY(r); // a bad e= never costs the i= part
        if (used)
            QCOMPARE(S(r->items.first().set), S(proto::ReactionSet{id("1f923")}));
        else
            QCOMPARE(S(r->items.first().set), S(proto::ReactionSet{E(proto::ThumbsUp)}));
    }

    void setLimits()
    {
        proto::ReactionSet set;
        const char* const codes[] = {"1f600", "1f601", "1f602", "1f603", "1f604", "1f605", "1f606", "1f607", "1f608", "1f609", "1f60a"};
        for (const char* code : codes) {
            if (proto::canAdd(set, id(code)))
                set.append(id(code));
        }
        QCOMPARE(set.size(), proto::kMaxReactionsPerSet);
        QVERIFY(!proto::canAdd(set, id("1f60b")));
        QVERIFY(!proto::canAdd({id("1f600")}, id("1f600"))); // already in
        QVERIFY(!proto::canAdd({}, -1));
        // Long sequences: the wire text of one set stays within its budget.
        const int kiss = emoji::find(QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x92\x8B\xE2\x80\x8D\xF0\x9F\x91\xA8"));
        QVERIFY(kiss >= 0);
        proto::ReactionSet longOnes;
        for (int t = 0; t <= emoji::kToneCount && emoji::hasTones(kiss); ++t) {
            const int variant = emoji::withTone(kiss, t);
            if (proto::canAdd(longOnes, variant))
                longOnes.append(variant);
        }
        QVERIFY(proto::setToEmojiCodes(longOnes).size() <= proto::kMaxSetCodeChars);
        QCOMPARE(S(proto::cleanSet({id("1f600"), -5, id("1f600"), 99999})), S(proto::ReactionSet{id("1f600")}));
    }

    void packingWithEmoji()
    {
        // 20 media, each with ten long emoji: several messages, each within every limit, all items there.
        QVector<proto::ReactItem> items;
        for (int i = 0; i < 20; ++i) {
            QString key = QStringLiteral("%1").arg(i, 20, 16, QLatin1Char('0'));
            proto::ReactionSet set;
            for (int k = 0; k < 10; ++k)
                set.append(emoji::members(emoji::Group::SmileysPeople).at(200 + k + i));
            items.append({key, 0, set});
        }
        const QVector<proto::Message> messages = proto::makeReacts(proto::Scope::Channel, false, items);
        QVERIFY(messages.size() > 1);
        int seen = 0;
        for (const proto::Message& m : messages) {
            const QByteArray wire = proto::serialize(m);
            QVERIFY(!wire.isEmpty());
            QVERIFY(wire.size() <= proto::kMaxSendBytes);
            QVERIFY(m.value("i").size() <= proto::kMaxValueChars);
            QVERIFY(m.value("e").size() <= proto::kMaxValueChars);
            const std::optional<proto::React> r = proto::readReact(*proto::parse(wire));
            QVERIFY(r);
            for (const proto::ReactItem& item : r->items) {
                QCOMPARE(S(item.set), S(items.at(seen).set));
                ++seen;
            }
        }
        QCOMPARE(seen, items.size());
    }

    // ---- the store ------------------------------------------------------------------------------------
    void storeKeepsOrderAndSets()
    {
        ReactionStore store;
        store.applyRemote(QString(), kKeyA, kSara, QStringLiteral("Sara"), proto::ReactionSet{id("1f923"), E(0)}, 1000);
        store.applyRemote(QString(), kKeyA, kReza, QStringLiteral("Reza"), proto::ReactionSet{E(0), id("1f389")}, 2000);
        store.setOwn(kKeyA, kMe, QStringLiteral("Me"), proto::ReactionSet{id("1f389")}, 3000);
        ReactionView v = store.view(kKeyA, kMe);
        QCOMPARE(v.entries.size(), 3);
        QCOMPARE(v.entries.at(0).reaction, id("1f923")); // in the order they appeared
        QCOMPARE(v.entries.at(1).reaction, E(0));
        QCOMPARE(v.entries.at(2).reaction, id("1f389"));
        QCOMPARE(v.entry(E(0)).count, 2);
        QCOMPARE(v.entry(id("1f389")).count, 2);
        QVERIFY(v.entry(id("1f389")).mine);
        QCOMPARE(S(v.own()), S(proto::ReactionSet{id("1f389")}));
        QCOMPARE(v.ownMask(), 0);
        QCOMPARE(store.maskOf(kKeyA, kSara), quint8(1));
        QCOMPARE(store.distinctCount(kKeyA), 3);
        // Sara takes 🤣 back: it leaves the row; the others keep their places.
        store.applyRemote(QString(), kKeyA, kSara, QStringLiteral("Sara"), proto::ReactionSet{E(0)}, 4000);
        v = store.view(kKeyA, kMe);
        QCOMPARE(v.entries.size(), 2);
        QCOMPARE(v.entries.at(0).reaction, E(0));
        // A new one goes to the end.
        store.applyRemote(QString(), kKeyA, kReza, QStringLiteral("Reza"), proto::ReactionSet{E(0), id("1f389"), id("1f923")}, 5000);
        QCOMPARE(store.view(kKeyA, kMe).entries.last().reaction, id("1f923"));
        // The same set again: unchanged.
        QCOMPARE(store.applyRemote(QString(), kKeyA, kReza, QStringLiteral("Reza"), proto::ReactionSet{E(0), id("1f389"), id("1f923")}, 6000),
                 ReactionStore::Apply::Unchanged);
        // SYNC answers carry the whole set.
        const QVector<proto::ReactItem> answer = store.syncAnswer({kKeyA}, kMe);
        QCOMPARE(answer.size(), 1);
        QCOMPARE(S(answer.first().set), S(proto::ReactionSet{id("1f389")}));
    }

    void storeShowsTwentyAtMost()
    {
        ReactionStore store;
        const QVector<int> faces = emoji::members(emoji::Group::FoodDrink);
        for (int r = 0; r < 3; ++r) {
            proto::ReactionSet set;
            for (int k = 0; k < 10; ++k)
                set.append(faces.at(r * 10 + k));
            store.applyRemote(QString(), kKeyA, QStringLiteral("uid%1").arg(r), QStringLiteral("P"), set, 1000 * (r + 1));
        }
        QCOMPARE(store.distinctCount(kKeyA), 30);
        QCOMPARE(store.view(kKeyA, kMe).entries.size(), proto::kMaxDistinctReactions);
    }

    void jsonRoundTripWithEmoji()
    {
        const qint64  now = 1800000000;
        ReactionStore store;
        store.applyRemote(QString(), kKeyA, kSara, QStringLiteral("Sara"), proto::ReactionSet{id("1f923"), E(proto::Heart)}, now * 1000);
        store.applyRemote(QString(), kKeyB, kReza, QStringLiteral("Reza"), proto::ReactionSet{E(proto::Fire)}, now * 1000);
        const QByteArray json = store.toJson(now);
        QVERIFY(json.contains("\"x\":\"1f923.2764-fe0f\""));
        QVERIFY(json.contains("\"e\":\"heart\""));
        QVERIFY(!json.contains("\"x\":\"1f525\"")); // only the six: no x (as 2.2.0 wrote it)
        ReactionStore loaded;
        QVERIFY(loaded.fromJson(json, now));
        QCOMPARE(S(loaded.setOf(kKeyA, kSara)), S((proto::ReactionSet{id("1f923"), E(proto::Heart)})));
        QCOMPARE(S(loaded.setOf(kKeyB, kReza)), S(proto::ReactionSet{E(proto::Fire)}));
        // A file of 2.2.0 (codes only), and a bad x that falls back to them.
        const QByteArray old = QByteArray("{\"v\":1,\"media\":{\"") + kKeyA.toLatin1() + "\":{\"seen\":1800000000,\"by\":{\"" + kSara.toLatin1()
                               + "\":{\"n\":\"Sara\",\"e\":\"up.sad\",\"t\":1800000000},\"" + kReza.toLatin1()
                               + "\":{\"n\":\"Reza\",\"e\":\"fire\",\"x\":\"zz!\",\"t\":1800000000}}}}}";
        QVERIFY(loaded.fromJson(old, now));
        QCOMPARE(S(loaded.setOf(kKeyA, kSara)), S((proto::ReactionSet{E(proto::ThumbsUp), E(proto::Sad)})));
        QCOMPARE(S(loaded.setOf(kKeyA, kReza)), S(proto::ReactionSet{E(proto::Fire)}));
    }

    // ---- preferences --------------------------------------------------------------------------------------
    void prefsRecentAndTone()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("emoji.ini"));
        emoji::prefs::setFile(file);
        QVERIFY(!emoji::prefs::recent().isEmpty()); // a few common ones to start with
        emoji::prefs::noteUsed(id("1f389"));
        emoji::prefs::noteUsed(id("1f923"));
        emoji::prefs::noteUsed(id("1f389")); // moves to the front, once
        emoji::prefs::noteUsed(-3);          // ignored
        emoji::prefs::setTone(4);
        emoji::prefs::setTone(99); // clamped
        emoji::prefs::setFile(file); // read again
        QCOMPARE(S(emoji::prefs::recent().mid(0, 2)), S((QVector<int>{id("1f389"), id("1f923")})));
        QCOMPARE(emoji::prefs::recent().size(), 2);
        QCOMPARE(emoji::prefs::tone(), emoji::kToneCount);
        // The quick reactions: the six, then the two most recent others.
        QVector<int> quick = emoji::prefs::quickReactions();
        QCOMPARE(quick.size(), 8);
        for (int i = 0; i < proto::kReactionCount; ++i)
            QCOMPARE(quick.at(i), E(i));
        QCOMPARE(quick.at(6), id("1f389"));
        QCOMPARE(quick.at(7), id("1f923"));
        for (int i = 0; i < 50; ++i)
            emoji::prefs::noteUsed(emoji::members(emoji::Group::AnimalsNature).at(i));
        QCOMPARE(emoji::prefs::recent().size(), emoji::prefs::kMaxRecent);
        emoji::prefs::clearRecent();
        // A hand-edited file with junk in it.
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("[General]\nrecent=\"zz,1f44d,,1F44D,1f44d,1f602\"\ntone=-4\n");
        }
        emoji::prefs::setFile(file);
        QCOMPARE(S(emoji::prefs::recent()), S((QVector<int>{id("1f44d"), id("1f602")})));
        QCOMPARE(emoji::prefs::tone(), 0);
        emoji::prefs::setFile(QString());
    }

    // ---- the chat document -------------------------------------------------------------------------------
    void chatDocument()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here: the chat keeps TeamSpeak's rendering");
        Settings::instance() = Settings();
        QTextDocument doc;
        QFont         font(QStringLiteral("Segoe UI"));
        font.setPixelSize(12);
        doc.setDefaultFont(font);
        message(&doc, QStringLiteral("Sara"), QString::fromUtf8("hi \xF0\x9F\x98\x80 there \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD (c) \xC2\xA9 ok"));
        message(&doc, QStringLiteral("Reza"), QString::fromUtf8("\xF0\x9F\x98\x82\xF0\x9F\x98\x82"));
        message(&doc, QStringLiteral("Reza"), QStringLiteral("smile |emoticons:smile.svg| wink |emoticons:twinkle.svg|"));
        message(&doc, QString::fromUtf8("Mehdi \xF0\x9F\x8C\xB8"), QStringLiteral("|emoticons:laugh.svg|"));
        // A TS Media message: caption, link, the note (with an emoji of its own).
        message(&doc, QStringLiteral("Ali"), QString::fromUtf8("look \xF0\x9F\x98\x8E") + QChar(QChar::LineSeparator));
        {
            QTextCursor c(&doc);
            c.movePosition(QTextCursor::End);
            QTextCharFormat link;
            link.setAnchor(true);
            link.setAnchorHref(QStringLiteral("ts3file://127.0.0.1?port=9987&filename=a.jpg"));
            c.insertText(QStringLiteral("a.jpg"), link);
            c.insertText(QString::fromUtf8(" \xE2\x80\x94 plugin required \xF0\x9F\x94\xA5"), QTextCharFormat());
        }
        const QString before        = doc.toPlainText();
        const QString beforeFormats = formatsOf(&doc);

        ChatEmoji::processDocument(&doc, 2.0);
        // 😀, 👍🏽 | 😂😂 (jumbo) | two emoticons | one emoticon (jumbo) | 😎 = 8; © stays text, so do the note and nickname.
        QCOMPARE(ChatEmoji::countEmoji(&doc), 8);
        const QTextBlock sara = doc.findBlockByNumber(0);
        QVERIFY(sara.text().contains(QChar(0xA9)));
        QVERIFY(!sara.text().contains(QString::fromUtf8("\xF0\x9F\x98\x80")));
        const QTextBlock jumbo = doc.findBlockByNumber(1);
        QCOMPARE(emojiWidthAt(&doc, jumbo.position() + jumbo.text().indexOf(QChar::ObjectReplacementCharacter, 1)), 48.0);
        const int inlineAt = sara.position() + sara.text().indexOf(QChar::ObjectReplacementCharacter, 1);
        QVERIFY(emojiWidthAt(&doc, inlineAt) >= 12 && emojiWidthAt(&doc, inlineAt) <= 17); // 1.375 x 12 px, at most the line
        const QTextBlock oneEmoticon = doc.findBlockByNumber(3);
        QVERIFY(oneEmoticon.text().contains(QString::fromUtf8("\xF0\x9F\x8C\xB8"))); // in the nickname link: untouched
        QCOMPARE(emojiWidthAt(&doc, oneEmoticon.position() + oneEmoticon.length() - 2), 48.0);
        const QTextBlock media = doc.findBlockByNumber(4);
        QVERIFY(media.text().contains(QString::fromUtf8("\xF0\x9F\x94\xA5")));        // the note
        QVERIFY(!media.text().contains(QString::fromUtf8("\xF0\x9F\x98\x8E")));       // the caption's emoji
        // Copying gives text and codes back.
        const QTextBlock emoticons = doc.findBlockByNumber(2);
        QCOMPARE(ChatEmoji::originalText(&doc, emoticons.position(), emoticons.position() + emoticons.length() - 1),
                 QStringLiteral("<20:02:13> \"Reza\": smile :) wink ;)"));
        QVERIFY(ChatEmoji::originalText(&doc, sara.position(), sara.position() + sara.length() - 1).endsWith(QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD (c) \xC2\xA9 ok")));
        // Done twice: nothing more to do.
        ChatEmoji::processDocument(&doc, 2.0);
        QCOMPARE(ChatEmoji::countEmoji(&doc), 8);
        // Restored exactly.
        ChatEmoji::restore(&doc);
        QCOMPARE(ChatEmoji::countEmoji(&doc), 0);
        QCOMPARE(doc.toPlainText(), before);
        QCOMPARE(formatsOf(&doc), beforeFormats);
        // Jumbo off: everything inline.
        Settings::instance().jumboEmoji = false;
        ChatEmoji::processDocument(&doc, 1.0);
        QVERIFY(emojiWidthAt(&doc, doc.findBlockByNumber(1).position() + 1) < 48);
        ChatEmoji::restore(&doc);
        Settings::instance() = Settings();
    }

    void chatDocumentBounds()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QTextDocument doc;
        QString       many;
        for (int i = 0; i < 1000; ++i)
            many += QString::fromUtf8("\xF0\x9F\x98\x80");
        message(&doc, QStringLiteral("Spammer"), many);
        ChatEmoji::processDocument(&doc, 1.0);
        QVERIFY(ChatEmoji::countEmoji(&doc) <= 400); // per message
        QVERIFY(ChatEmoji::countEmoji(&doc) > 0);
        ChatEmoji::restore(&doc);
        QCOMPARE(doc.toPlainText().count(QString::fromUtf8("\xF0\x9F\x98\x80")), 1000);
    }
};

TSMEDIA_REGISTER_TEST(TestEmojiReact)

#include "tst_emojireact.moc"
