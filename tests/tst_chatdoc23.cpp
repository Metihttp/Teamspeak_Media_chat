// 2.3 chat documents with every layer of TS Media at once: HD emoji (jumbo too), reply lines, poll cards,
// text reaction rows and the chat layout (Cozy with bubbles, Compact, Classic), on TeamSpeak-like chats
// (tests/tschat.h, made-up people only). A message's reaction key stays the same through all of it, rows
// never change what other modules read (jumbo emoji, copying, replies), TeamSpeak's next line never takes
// a row's format, the plugin going gives TeamSpeak's document back exactly, and a layout switch at the
// chat's end with poll cards leaves it at its end (live test 2.3).

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QtTest>

#include <functional>

#include "chatemoji.h"
#include "chatscroll.h"
#include "emojidata.h"
#include "emojiformat.h"
#include "emojirender.h"
#include "layoutart.h"
#include "layoutbubbles.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "polldoc.h"
#include "polls.h"
#include "replies.h"
#include "replydoc.h"
#include "textreact.h"
#include "textreactdoc.h"
#include "testmain.h"
#include "tschat.h"

namespace {

using layoutart::Mode;

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kMe    = QStringLiteral("q0Xn6Mine000000000000000000=");

QFont chatFont()
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPointSizeF(9.0);
    return f;
}

void setup(QTextDocument& doc, int width = 520)
{
    doc.setDefaultFont(chatFont());
    doc.setTextWidth(width);
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
}

layoutdoc::Env envFor(Mode mode, bool bubbles, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
    env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("d");
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = kMe;
    env.replyHeader  = [](int, replyart::Header* h) {
        h->nick    = QStringLiteral("Alice");
        h->snippet = QStringLiteral("Who is in for tonight?");
        h->found   = true;
        return true;
    };
    return env;
}

// ---- TeamSpeak's document, exactly (as tst_chatlayout compares it) --------------------------------------

struct Snapshot {
    QString              text;
    QVector<QVariantMap> chars;
    QVector<QVariantMap> blocks;
    QVector<bool>        visible;
};

QVariantMap propsOf(const QTextFormat& f, QTextDocument* doc = nullptr)
{
    QVariantMap               out;
    const QMap<int, QVariant> props = f.properties();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (it.key() == QTextFormat::ObjectIndex && doc) {
            QTextObject* o = doc->object(it.value().toInt());
            out.insert(QStringLiteral("object"), o ? QVariant(propsOf(o->format())) : QVariant(QStringLiteral("none")));
            continue;
        }
        out.insert(QString::number(it.key()), it.value());
    }
    return out;
}

Snapshot snapshot(QTextDocument& doc)
{
    Snapshot s;
    s.text = doc.toPlainText();
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        s.blocks.append(propsOf(b.blockFormat(), &doc));
        s.visible.append(b.isVisible());
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            for (int i = 0; i < f.length(); ++i)
                s.chars.append(propsOf(f.charFormat(), &doc));
        }
        s.chars.append(propsOf(b.charFormat(), &doc)); // the block's own (its paragraph mark's) format
    }
    return s;
}

QString compare(const Snapshot& a, const Snapshot& b)
{
    if (a.text != b.text)
        return QStringLiteral("text differs:\n%1\n--- vs ---\n%2").arg(a.text, b.text);
    if (a.blocks.size() != b.blocks.size())
        return QStringLiteral("block count %1 vs %2").arg(a.blocks.size()).arg(b.blocks.size());
    for (int i = 0; i < a.blocks.size(); ++i) {
        if (a.blocks.at(i) != b.blocks.at(i))
            return QStringLiteral("block %1 format differs (%2 vs %3 properties)").arg(i).arg(a.blocks.at(i).size()).arg(b.blocks.at(i).size());
        if (a.visible.at(i) != b.visible.at(i))
            return QStringLiteral("block %1 visibility differs").arg(i);
    }
    if (a.chars.size() != b.chars.size())
        return QStringLiteral("char count %1 vs %2").arg(a.chars.size()).arg(b.chars.size());
    for (int i = 0; i < a.chars.size(); ++i) {
        if (a.chars.at(i) == b.chars.at(i))
            continue;
        QStringList keys = a.chars.at(i).keys() + b.chars.at(i).keys();
        keys.removeDuplicates();
        QStringList diff;
        for (const QString& k : keys) {
            if (a.chars.at(i).value(k) != b.chars.at(i).value(k)) {
                QString va, vb;
                QDebug(&va) << a.chars.at(i).value(k);
                QDebug(&vb) << b.chars.at(i).value(k);
                diff << k + QLatin1Char('=') + va + QStringLiteral(" vs ") + vb;
            }
        }
        return QStringLiteral("char %1 format differs in %2").arg(i).arg(diff.join(QLatin1Char(',')));
    }
    return {};
}

// ---- the chat ----------------------------------------------------------------------------------------------

polls::Poll samplePoll()
{
    polls::Poll p;
    p.key       = QStringLiteral("3f9a1c2e5b6d7e80");
    p.question  = QStringLiteral("Which map tonight? \U0001F355");
    p.options   = QStringList{QStringLiteral("Harbor"), QStringLiteral("Desert Town"), QStringLiteral("Night Market")};
    p.channelId = 12;
    return p;
}

QString pollHtml(const tschat::Skin& s, const polls::Poll& poll)
{
    QString body = QStringLiteral("\U0001F4CA Poll: ") + tschat::escaped(poll.question);
    for (int i = 0; i < poll.options.size(); ++i)
        body += QStringLiteral("<br>%1. %2").arg(i + 1).arg(tschat::escaped(poll.options.at(i)));
    body += QStringLiteral("<br>") + tschat::linkHtml(s, QStringLiteral("https://github.com/Metihttp/Teamspeak_Media_chat#") + polls::markerFragment(poll),
                                                      QStringLiteral("Vote with TS Media chat"));
    return body;
}

// TeamSpeak's rendering of a reply's quote line (BBCode: italic, the author linked), then the reply.
QString replyHtml(const QString& replyText)
{
    replies::Original alice;
    alice.nick     = QStringLiteral("Alice");
    alice.uid      = kAlice;
    alice.clientId = 17;
    alice.minutes  = 20 * 60 + 1;
    alice.text     = QStringLiteral("Who is in for tonight? :)");
    QString quote  = replies::quoteLine(alice);
    quote.replace(QStringLiteral("[i]"), QStringLiteral("<i>")).replace(QStringLiteral("[/i]"), QStringLiteral("</i>"));
    quote.replace(QRegularExpression(QStringLiteral("\\[URL=([^\\]]+)\\]")), QStringLiteral("<a href=\"\\1\">")).replace(QStringLiteral("[/URL]"), QStringLiteral("</a>"));
    return quote + QStringLiteral("<br>") + tschat::escaped(replyText);
}

// Yesterday's history (its marker), then today: system lines, text, HD and jumbo emoji, a reply, a poll,
// a link, two lines, TS Media's own line. Ends with a message (it gets a row; TeamSpeak appends after it).
void fillCombined(QTextDocument& doc)
{
    setup(doc);
    const tschat::Skin s = tschat::darkSkin();
    tschat::append(&doc, tschat::historyHtml(s, QStringLiteral("2026-10-09 20:00:00")));
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("20:00:01"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:01:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who is in for tonight? :)")));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:01:30"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("Pizza \U0001F355 tonight?")));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:02:00"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("\U0001F602\U0001F602\U0001F602")));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:03:00"), 3, kMe, QStringLiteral("Sam"), replyHtml(QStringLiteral("count me in")), true));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:04:00"), 17, kAlice, QStringLiteral("Alice"), pollHtml(s, samplePoll())));
    tschat::append(&doc, tschat::printHtml(s, QStringLiteral("2 files sent.")));
    tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:00:00"), 17, kAlice, QStringLiteral("Alice"), tschat::linkHtml(s, QStringLiteral("https://example.com/map-notes"))));
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:01:00"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("line one<br>line two")));
}

// Every message's reaction key as a client reads it (the days from the chat's day lines, or the ones given;
// UTC for the test).
QMap<int, QString> keysOf(QTextDocument& doc, QVector<QDate> dates = {})
{
    QMap<int, QString> keys;
    const QDate        today = QDate(2026, 10, 10);
    if (dates.isEmpty())
        dates = layoutdoc::datesOf(&doc, today);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const textreactdoc::Identity id = textreactdoc::identify(b);
        if (!id.eligible())
            continue;
        const QDate day = textreactdoc::dayOf(dates.value(b.blockNumber()), id.seconds, today, 23 * 3600);
        keys.insert(b.blockNumber(), textreactdoc::localKeyOf(id, day, 0));
    }
    return keys;
}

QString describe(const QMap<int, QString>& keys)
{
    QStringList out;
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it)
        out << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
    return out.join(QLatin1Char(' '));
}

// Rows on every eligible message (as ChatTextReactions puts them), each with its key.
int addRows(QTextDocument& doc, const QMap<int, QString>& keys, QSizeF size = QSizeF(96, 26))
{
    int n = 0;
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it)
        n += textreactdoc::insertRow(&doc, it.key(), textreactdoc::objectPrefix() + QStringLiteral("d-%1").arg(it.key()), it.value(), size) ? 1 : 0;
    return n;
}

bool cardAll(QTextDocument& doc)
{
    bool all = true;
    int  n   = 0;
    for (const polldoc::Found& f : polldoc::scan(&doc)) {
        if (!f.collapsed)
            all = polldoc::collapse(&doc, f, polldoc::objectPrefix() + QStringLiteral("d%1").arg(++n), QSizeF(260, 180)) && all;
    }
    polldoc::normalizeAll(&doc);
    return all && n > 0;
}

int collapseReplies(QTextDocument& doc)
{
    int n = 0;
    for (const replydoc::Message& m : replydoc::scan(&doc)) {
        if (m.hasQuote && !m.restyled)
            n += replydoc::collapse(&doc, m, replydoc::objectPrefix() + QStringLiteral("d.%1").arg(m.block), QSizeF(200, 14)) ? 1 : 0;
    }
    return n;
}

bool isJumbo(const QTextBlock& block)
{
    int large = 0, small = 0;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextCharFormat f = it.fragment().charFormat();
        if (!emojiformat::isHd(f))
            continue;
        (f.boolProperty(emojiformat::kJumbo) ? large : small) += it.fragment().length();
    }
    return large > 0 && small == 0;
}

bool hasRowFormat(const QTextFormat& f)
{
    return f.hasProperty(textreactdoc::kRowObject) || f.hasProperty(textreactdoc::kRowKey) || f.hasProperty(textreactdoc::kRowSeparator)
           || (f.isImageFormat() && f.toImageFormat().name().startsWith(textreactdoc::objectPrefix()));
}

// A line TeamSpeak just appended, as ChatLayout styles it (processBlock): a copy of our formats is
// sanitized first, then the line gets its look.
void layoutNewLine(QTextDocument& doc, const layoutdoc::Env& env, int n)
{
    QTextBlock b = doc.findBlockByNumber(n);
    if (layoutdoc::hasOurs(b) || b.blockFormat().hasProperty(layoutformat::kBlockTag))
        layoutdoc::sanitize(&doc, n);
    b                        = doc.findBlockByNumber(n);
    const layoutdoc::Row row = layoutdoc::read(b);
    if (row.type == layoutdoc::RowType::Unknown || row.styled)
        return;
    const QVector<QDate> dates = layoutdoc::datesOf(&doc, env.today);
    layoutdoc::apply(&doc, n, layoutdoc::plan(env, b, row, layoutdoc::previousOf(b), dates.value(n), 0, 900 + n), env);
}

} // namespace

class TestChatDoc23 : public QObject
{
    Q_OBJECT

  private slots:
    // Every layer on, in the order the plugin runs them, a layout switch through all three, the next lines
    // TeamSpeak appends, then the plugin going (rows, layout, cards, reply lines, emoji): every message keeps
    // its key and TeamSpeak gets its document back exactly.
    void keysAndRestoreThroughEveryLayer()
    {
        if (!emoji::hasColor())
            QSKIP("no colour emoji engine here");
        QTextDocument plain, doc;
        fillCombined(plain);
        fillCombined(doc);
        const QMap<int, QString> keys = keysOf(doc);
        QCOMPARE(keys.size(), 7); // Alice, the pizza, the jumbo, the reply, the poll, the link, the two lines
        const auto same = [&](const char* when) {
            const QMap<int, QString> now = keysOf(doc);
            for (auto it = keys.constBegin(); it != keys.constEnd(); ++it)
                QVERIFY2(now.value(it.key()) == it.value(), qPrintable(QStringLiteral("%1: %2\nwas %3").arg(QString::fromLatin1(when), describe(now), describe(keys))));
        };

        ChatEmoji::processDocument(&doc, 1.0);
        QVERIFY(ChatEmoji::countEmoji(&doc) >= 4);
        same("hd emoji");
        QCOMPARE(collapseReplies(doc), 1);
        same("reply line");
        QVERIFY(cardAll(doc));
        same("poll card");
        QCOMPARE(addRows(doc, keys), keys.size());
        same("rows");

        layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
        polldoc::normalizeAll(&doc);
        ChatEmoji::processDocument(&doc, 1.0);
        same("cozy with bubbles");
        // The rows sit in their bubbles.
        doc.documentLayout()->documentSize();
        for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
            const QTextBlock          b    = doc.findBlockByNumber(it.key());
            const layoutbubbles::Item item = layoutbubbles::itemOf(b, envFor(Mode::Cozy, true));
            QCOMPARE(item.kind, layoutbubbles::Kind::Bubble);
            QVERIFY2(item.rect.contains(textreactdoc::rowRect(b)), qPrintable(QStringLiteral("block %1").arg(it.key())));
            QCOMPARE(textreactdoc::rowOf(b).key, it.value());
        }

        // TeamSpeak's next line, right after a row: a message of its own, no row of ours.
        const tschat::Skin s    = tschat::darkSkin();
        const QString      next = tschat::messageHtml(s, QStringLiteral("09:02:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("see you there"));
        tschat::append(&plain, next);
        tschat::append(&doc, next);
        const QTextBlock appended = doc.lastBlock();
        QVERIFY(!textreactdoc::rowOf(appended).exists());
        QVERIFY(textreactdoc::identify(appended).eligible());
        layoutNewLine(doc, envFor(Mode::Cozy, true), appended.blockNumber());
        ChatEmoji::processDocument(&doc, 1.0);
        same("a line appended");
        QVERIFY(!textreactdoc::rowOf(doc.lastBlock()).exists());

        for (const Mode mode : {Mode::Compact, Mode::Classic, Mode::Cozy}) {
            layoutdoc::restoreAll(&doc);
            polldoc::normalizeAll(&doc);
            if (mode != Mode::Classic) {
                layoutdoc::styleAll(&doc, envFor(mode, false));
                polldoc::normalizeAll(&doc);
                ChatEmoji::processDocument(&doc, 1.0);
            }
            same(mode == Mode::Compact ? "compact" : mode == Mode::Classic ? "classic" : "cozy without bubbles");
        }

        // The plugin goes (ChatIntegration's order): rows, layout, cards, reply lines, HD emoji. Nothing of a
        // row stays, also where TeamSpeak's next line copied the row picture's format.
        QCOMPARE(textreactdoc::removeAll(&doc), keys.size());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            QVERIFY2(!hasRowFormat(b.charFormat()), qPrintable(QStringLiteral("block %1's own format").arg(b.blockNumber())));
            for (auto it = b.begin(); !it.atEnd(); ++it)
                QVERIFY(!hasRowFormat(it.fragment().charFormat()));
        }
        layoutdoc::restoreAll(&doc);
        polldoc::restoreAll(&doc);
        replydoc::restoreAll(&doc);
        ChatEmoji::restore(&doc);
        const QString diff = compare(snapshot(plain), snapshot(doc));
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    // A message of only emoji stays large when it gets reactions: the row (our line break and picture) is
    // not part of the message when HD emoji read it again (the next line TeamSpeak appends, a zoom, ...).
    void jumboEmojiKeepTheirSizeWithARow()
    {
        if (!emoji::hasColor())
            QSKIP("no colour emoji engine here");
        for (const bool styled : {false, true}) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:02:00"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("\U0001F602\U0001F602\U0001F602")));
            if (styled)
                layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
            ChatEmoji::processDocument(&doc, 1.0);
            QVERIFY(isJumbo(doc.firstBlock()));
            const QMap<int, QString> keys = keysOf(doc);
            QCOMPARE(keys.size(), 1);
            QCOMPARE(addRows(doc, keys), 1);
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:02:10"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("haha")));
            ChatEmoji::processDocument(&doc, 1.0); // what the next line makes ChatEmoji do for the block before it
            QVERIFY2(isJumbo(doc.firstBlock()), styled ? "cozy" : "classic");
            QCOMPARE(keysOf(doc).value(0), keys.value(0));
            // And without the row it is still judged the same way.
            textreactdoc::removeAll(&doc);
            ChatEmoji::processDocument(&doc, 1.0);
            QVERIFY(isJumbo(doc.firstBlock()));
        }
    }

    // TeamSpeak's next line copies the format of the character before it (a new block at the end): after a
    // row, our picture's. A line it puts in as HTML and one it types as plain text after a row stay
    // TeamSpeak's: never read as a row, never taken out with one, and given back as TeamSpeak made them.
    void linesAfterARowStayTeamSpeaks()
    {
        const tschat::Skin s = tschat::darkSkin();
        for (const bool styled : {false, true}) {
            for (const bool typed : {true, false}) {
                const QString tag = QStringLiteral("%1 %2").arg(styled ? QStringLiteral("cozy") : QStringLiteral("classic"), typed ? QStringLiteral("typed") : QStringLiteral("html"));
                QTextDocument plain, doc;
                for (QTextDocument* d : {&plain, &doc}) {
                    setup(*d);
                    tschat::append(d, tschat::messageHtml(s, QStringLiteral("20:01:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who is in for ") + tschat::linkHtml(s, QStringLiteral("https://example.com/tonight"))));
                }
                if (styled)
                    layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
                QCOMPARE(addRows(doc, keysOf(doc)), 1);
                for (QTextDocument* d : {&plain, &doc}) {
                    if (typed) {
                        QTextCursor c(d);
                        c.movePosition(QTextCursor::End);
                        c.insertBlock();
                        c.insertText(QStringLiteral("typed after"));
                    } else {
                        tschat::append(d, tschat::messageHtml(s, QStringLiteral("20:01:10"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("me too")));
                    }
                }
                const QTextBlock added = doc.lastBlock();
                QVERIFY2(!textreactdoc::rowOf(added).exists(), qPrintable(tag));
                QCOMPARE(textreactdoc::rowOf(doc.firstBlock()).key, keysOf(doc).value(0));
                if (styled)
                    layoutNewLine(doc, envFor(Mode::Cozy, true), added.blockNumber());
                QCOMPARE(textreactdoc::removeAll(&doc), 1);
                for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                    QVERIFY2(!hasRowFormat(b.charFormat()), qPrintable(tag));
                    for (auto it = b.begin(); !it.atEnd(); ++it)
                        QVERIFY2(!hasRowFormat(it.fragment().charFormat()), qPrintable(tag));
                }
                layoutdoc::restoreAll(&doc);
                QCOMPARE(doc.lastBlock().text(), plain.lastBlock().text());
                const QString diff = compare(snapshot(plain), snapshot(doc));
                QVERIFY2(diff.isEmpty(), qPrintable(tag + QStringLiteral(": ") + diff));
            }
        }
    }

    // TeamSpeak's line limit drops the first lines once a chat is full, and with them the history marker
    // above yesterday's messages. Their rows move with them, and with the days the last reading kept they
    // keep their day, so their keys (and reactions) stay when they are read again (a restyle, a switch).
    void lineLimitKeepsDaysAndRows()
    {
        const tschat::Skin s     = tschat::darkSkin();
        const QDate        today = QDate(2026, 10, 10);
        QTextDocument      doc;
        setup(doc);
        tschat::append(&doc, tschat::historyHtml(s, QStringLiteral("2026-10-09 20:00:00")));
        for (int i = 0; i < 3; ++i)
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:0%1:00").arg(i), 17 + i % 2, i % 2 ? kBob : kAlice, i % 2 ? QStringLiteral("Bob") : QStringLiteral("Alice"),
                                                      QStringLiteral("yesterday %1").arg(i)));
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
        for (int i = 0; i < 2; ++i)
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:0%1:00").arg(i), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("today %1").arg(i)));
        const QMap<int, QString> keys = keysOf(doc);
        QCOMPARE(keys.size(), 5);
        QCOMPARE(addRows(doc, keys), 5);
        const auto signatures = [&doc] {
            QVector<quint64> out;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
                out << ((static_cast<quint64>(qHash(b.text())) << 1) | 1);
            return out;
        };
        QVector<quint64> before = signatures();
        QVector<QDate>   days   = layoutdoc::datesOf(&doc, today);
        doc.setMaximumBlockCount(doc.blockCount());

        for (int drop = 1; drop <= 2; ++drop) {
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:1%1:00").arg(drop), 18, kBob, QStringLiteral("Bob"), QStringLiteral("now %1").arg(drop)));
            QCOMPARE(doc.blockCount(), before.size());
            QVERIFY(doc.firstBlock().text().contains(drop == 1 ? QStringLiteral("yesterday 0") : QStringLiteral("yesterday 1")));
            // The rows moved with their messages.
            for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
                if (it.key() - drop >= 0)
                    QCOMPARE(textreactdoc::rowOf(doc.findBlockByNumber(it.key() - drop)).key, it.value());
            }
            // Read from scratch, yesterday's messages are today's now (no day line above them is left)...
            QVERIFY(keysOf(doc).value(0) != keys.value(drop));
            // ...the days of the last reading give them theirs.
            const QVector<quint64> now = signatures();
            const int              up  = textreact::movedUp(textreact::carryOver(before, now));
            QCOMPARE(up, 1);
            days = textreact::keepDays(layoutdoc::datesOf(&doc, today), days, up);
            const QMap<int, QString> kept = keysOf(doc, days);
            for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
                if (it.key() - drop >= 0)
                    QCOMPARE(kept.value(it.key() - drop), it.value());
            }
            before = now;
        }
        // Nothing moved: the same days. A cleared chat (nothing found again): nothing kept.
        QCOMPARE(textreact::movedUp(textreact::carryOver(before, before)), 0);
        QCOMPARE(textreact::movedUp(textreact::carryOver(before, QVector<quint64>{7, 8, 9})), -1);
        const auto iso = [](const QVector<QDate>& dates) {
            QStringList out;
            for (const QDate& d : dates)
                out << d.toString(Qt::ISODate);
            return out;
        };
        QCOMPARE(iso(textreact::keepDays(QVector<QDate>(3), days, -1)), iso(QVector<QDate>(3)));
        QVector<QDate> earlier(3);
        earlier[0] = earlier[1] = QDate(2026, 10, 9);
        QCOMPARE(iso(textreact::keepDays(QVector<QDate>(2), earlier, 1)), (QStringList{QStringLiteral("2026-10-09"), QString()}));
    }

    // Copying (Ctrl+C, Copy text) gives the same text with rows as without them, in every layout, also for
    // poll cards (the poll as TeamSpeak shows it).
    void copyTextIgnoresRows()
    {
        for (const Mode mode : {Mode::Classic, Mode::Cozy, Mode::Compact}) {
            QTextDocument with, without;
            fillCombined(with);
            fillCombined(without);
            for (QTextDocument* d : {&with, &without}) {
                QVERIFY(cardAll(*d));
                if (mode != Mode::Classic) {
                    layoutdoc::styleAll(d, envFor(mode, true));
                    polldoc::normalizeAll(d);
                }
            }
            QCOMPARE(addRows(with, keysOf(with)), keysOf(without).size());
            const QString a = ChatEmoji::originalText(&with, 0, with.characterCount() - 1);
            const QString b = ChatEmoji::originalText(&without, 0, without.characterCount() - 1);
            QCOMPARE(a, b);
            QVERIFY(a.contains(QStringLiteral("2. Desert Town")));
            QVERIFY(!a.contains(QChar::ObjectReplacementCharacter));
            QVERIFY(!a.contains(QChar::LineSeparator));
            // One message with its row, from its text to the block's end: no line break at the end.
            const int        n  = 2;
            const QTextBlock b2 = with.findBlockByNumber(n);
            QVERIFY(textreactdoc::rowOf(b2).exists());
            const QString one = ChatEmoji::originalText(&with, replydoc::parseBlock(b2).textStart, b2.position() + b2.length() - 1);
            QCOMPARE(one, QStringLiteral("Who is in for tonight? :)"));
        }
    }

    // Ctrl+C over a message with a row: TeamSpeak's own copy would put our line break and picture on the
    // clipboard ("\n" and U+FFFC after the text); a selection with a row is copied as its text, like one
    // with HD emoji or a poll card. (The offscreen platform's clipboard is the test's own.)
    void ctrlCOverARowCopiesTheText()
    {
        for (const bool withRow : {true, false}) {
            QTextBrowser browser;
            QTextDocument* doc = browser.document();
            setup(*doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(doc, tschat::messageHtml(s, QStringLiteral("20:01:00"), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("Who is in for tonight")));
            tschat::append(doc, tschat::messageHtml(s, QStringLiteral("20:01:10"), 18, kBob, QStringLiteral("Bob"), QStringLiteral("me")));
            if (withRow)
                QCOMPARE(addRows(*doc, keysOf(*doc)), 2);
            ChatEmoji chat;
            chat.attach(&browser);
            // Alice's text to the end of her line (with its row, when it has one).
            const QTextBlock first = doc->firstBlock();
            QTextCursor      selection(doc);
            selection.setPosition(replydoc::parseBlock(first).textStart);
            selection.setPosition(first.position() + first.length() - 1, QTextCursor::KeepAnchor);
            browser.setTextCursor(selection);
            QGuiApplication::clipboard()->clear();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
            QApplication::sendEvent(&browser, &press);
            const QString copied = QGuiApplication::clipboard()->text();
            QVERIFY2(copied == QStringLiteral("Who is in for tonight"), qPrintable(QStringLiteral("row %1: [%2]").arg(withRow).arg(copied)));
            QGuiApplication::clipboard()->clear();
        }
    }

    // A long chat (4000 messages in Cozy with bubbles, rows on every tenth): reading every message for its
    // key, the days and a full give-back stay cheap (ChatTextReactions reads in 6 ms steps; this is all of it).
    void longChatReadsQuickly()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
        for (int i = 0; i < 4000; ++i) {
            const QString time = QStringLiteral("%1:%2:%3").arg(8 + i / 3600, 2, 10, QLatin1Char('0')).arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
            tschat::append(&doc, tschat::messageHtml(s, time, 17 + i % 3, i % 2 ? kBob : kAlice, i % 2 ? QStringLiteral("Bob") : QStringLiteral("Alice"),
                                                      i % 7 ? QStringLiteral("message %1 about the map").arg(i) : tschat::linkHtml(s, QStringLiteral("https://example.com/%1").arg(i))));
        }
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
        QElapsedTimer clock;
        clock.start();
        const QMap<int, QString> keys = keysOf(doc);
        const qint64             read = clock.elapsed();
        QCOMPARE(keys.size(), 4000);
        QMap<int, QString> some;
        for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
            if (it.key() % 10 == 0)
                some.insert(it.key(), it.value());
        }
        QCOMPARE(addRows(doc, some), some.size());
        clock.restart();
        QCOMPARE(keysOf(doc), keys);
        const qint64 again = clock.elapsed();
        clock.restart();
        QCOMPARE(textreactdoc::removeAll(&doc), some.size());
        const qint64 back = clock.elapsed();
        qInfo("4000 messages: read %lld ms, with rows %lld ms, rows out %lld ms", read, again, back);
        QVERIFY2(read < 3000 && again < 3000 && back < 3000, "reading a long chat got slow");
    }

    // What replies read of a message (its text for snippets and matching) is the same with a row.
    void repliesReadTheSameWithRows()
    {
        for (const Mode mode : {Mode::Classic, Mode::Cozy, Mode::Compact}) {
            QTextDocument with, without;
            fillCombined(with);
            fillCombined(without);
            for (QTextDocument* d : {&with, &without}) {
                collapseReplies(*d);
                QVERIFY(cardAll(*d));
                if (mode != Mode::Classic) {
                    layoutdoc::styleAll(d, envFor(mode, true));
                    polldoc::normalizeAll(d);
                }
            }
            addRows(with, keysOf(with));
            const QVector<replydoc::Message> a = replydoc::scan(&with);
            const QVector<replydoc::Message> b = replydoc::scan(&without);
            QCOMPARE(a.size(), b.size());
            for (int i = 0; i < a.size(); ++i) {
                QCOMPARE(a.at(i).block, b.at(i).block);
                QCOMPARE(a.at(i).text, b.at(i).text);
                QCOMPARE(a.at(i).hasQuote, b.at(i).hasQuote);
                QCOMPARE(a.at(i).mediaLabel, b.at(i).mediaLabel);
                QCOMPARE(replies::snippetSource(a.at(i).text, a.at(i).mediaLabel), replies::snippetSource(b.at(i).text, b.at(i).mediaLabel));
            }
        }
    }

    // Live test 2.3: a chat at its end with poll cards, switched Cozy -> Classic -> Cozy. The layout keeps the
    // reader's place around its own edit, then ChatPolls moves the cards' line breaks for the new look
    // (ChatLayout::documentEdited): that edit changes the chat's height too, so it keeps the place as well,
    // or the chat ends about a line above its end. Here the same steps on a QTextBrowser.
    void layoutSwitchWithPollCardsKeepsTheEnd()
    {
        QTextBrowser browser;
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(560, 240);
        QTextDocument* doc = browser.document();
        fillCombined(*doc);
        const tschat::Skin s = tschat::darkSkin();
        polls::Poll second = samplePoll();
        second.key         = QStringLiteral("5e1b2c3d4f6a7b80");
        second.question    = QStringLiteral("Coffee or tea?");
        tschat::append(doc, tschat::messageHtml(s, QStringLiteral("09:02:00"), 17, kAlice, QStringLiteral("Alice"), pollHtml(s, second)));
        const layoutdoc::Env cozy = envFor(Mode::Cozy, true);
        layoutdoc::styleAll(doc, cozy);
        {
            layoutformat::MutatingScope mutating; // ChatPolls::afterScan: one card after the other
            int                         made = 0;
            for (bool more = true; more;) {
                more = false;
                for (const polldoc::Found& f : polldoc::scan(doc)) {
                    if (!f.collapsed) {
                        QVERIFY(polldoc::collapse(doc, f, polldoc::objectPrefix() + QStringLiteral("e%1").arg(++made), QSizeF(260, 180)));
                        more = true;
                        break;
                    }
                }
            }
            QCOMPARE(made, 2);
            polldoc::normalizeAll(doc);
        }
        browser.show();
        QCoreApplication::processEvents();
        chatscroll::syncRange(&browser);
        QScrollBar* bar = browser.verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum());

        // A step of ChatLayout (its place kept), then the cards' line breaks with the place kept.
        int        edits = 0;
        const auto step  = [&](const std::function<void()>& edit) {
            const chatscroll::Place layoutPlace = chatscroll::capture(&browser);
            QVERIFY(layoutPlace.bottom);
            edit();
            chatscroll::restore(&browser, layoutPlace);
            QVERIFY(chatscroll::atBottom(&browser));
            const chatscroll::Place place = chatscroll::capture(&browser);
            {
                layoutformat::MutatingScope mutating;
                edits += polldoc::normalizeAll(doc);
            }
            chatscroll::restore(&browser, place);
            QVERIFY2(chatscroll::atBottom(&browser), qPrintable(QStringLiteral("%1 of %2").arg(bar->value()).arg(bar->maximum())));
        };
        const layoutdoc::Env compact = envFor(Mode::Compact, false);
        step([&] { layoutdoc::restoreAll(doc); layoutdoc::styleAll(doc, compact); }); // Cozy -> Compact
        step([&] { layoutdoc::restoreAll(doc); });                                     // -> TeamSpeak classic
        step([&] { layoutdoc::styleAll(doc, cozy); });                                 // -> Cozy again
        QVERIFY(edits > 0); // the cards' line breaks did follow the looks
        layoutdoc::restoreAll(doc);
    }
};

TSMEDIA_REGISTER_TEST(TestChatDoc23)
#include "tst_chatdoc23.moc"
