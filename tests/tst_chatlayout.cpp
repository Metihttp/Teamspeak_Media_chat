// Chat redesign: TS Media's chat layout on TeamSpeak-like chat documents (tests/tschat.h). Classification
// (the server tab's and private chats' status lines without "*** " too), byte-exact round trips in Cozy and
// Compact (with reply lines and HD emoji in any order), grouping, the geometry of the hanging indent, formats
// TeamSpeak copies from ours, copying, the one header parser, dates, dividers and history markers, lines that
// just arrived against history being loaded, the reader's place across a switch and an opened run (on a
// QTextBrowser), and the contrast of every colour pair.

#include <QAbstractTextDocumentLayout>
#include <cmath>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QUrl>
#include <QtTest>

#include "albums.h"
#include "chatemoji.h"
#include "chatscroll.h"
#include "emojidata.h"
#include "emojiformat.h"
#include "emojirender.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "replies.h"
#include "replydoc.h"
#include "settings.h"
#include "testmain.h"
#include "tschat.h"
#include "uiutil.h"

namespace {

using layoutdoc::RowType;
using layoutart::Mode;

const QString kUidAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kUidBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");

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

layoutdoc::Env envFor(Mode mode, bool dark = true, int width = 512)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false);
    env.colors       = layoutart::colorsFor(dark, dark ? QColor(0x2f, 0x31, 0x36) : QColor(Qt::white), dark ? QColor(0xdc, 0xdd, 0xde) : QColor(Qt::black),
                                            dark ? QColor(0x1c, 0xb0, 0xf4) : QColor(0x1c, 0x82, 0xcc));
    env.dpr          = 1.0;
    env.contentWidth = width;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("t");
    return env;
}

// Every character with its format, and every block's format: what TeamSpeak's document is.
struct Snapshot {
    QString                text;
    QVector<QVariantMap>   chars;
    QVector<QVariantMap>   blocks;
    QVector<bool>          visible;
};

// The properties; an object index (TeamSpeak's HTML pictures have a text object of their own) as what that
// object is, since a picture given back gets an equal new object.
QVariantMap propsOf(const QTextFormat& f, QTextDocument* doc = nullptr)
{
    QVariantMap out;
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
        s.chars.append(propsOf(b.charFormat(), &doc));
    }
    return s;
}

// The first difference, for the failure message.
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
        if (a.chars.at(i) != b.chars.at(i)) {
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
    }
    return {};
}

// A dark-skin sample chat plus the other kinds of lines.
void fillChat(QTextDocument& doc, const tschat::Skin& s = tschat::darkSkin())
{
    setup(doc);
    tschat::fillSampleChat(&doc, s);
    tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("line one<br>line two<br>line three")));
    tschat::append(&doc, tschat::printHtml(s, QStringLiteral("2 files sent.")));
    tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("20:15:00"), tschat::clientHtml(s, 18, kUidBob, QStringLiteral("Bob")) + QStringLiteral(" disconnected (leaving)")));
}

int blockWith(QTextDocument& doc, const QString& text)
{
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (b.text().contains(text))
            return b.blockNumber();
    }
    return -1;
}

// Every run of events folded as ChatLayout folds them (expanded: the first rows' tags of the open ones).
void collapseAll(QTextDocument& doc, const layoutdoc::Env& env, const QSet<int>& expanded = {})
{
    int serial = 1000;
    for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(&doc, 0, doc.blockCount() - 1, expanded))
        layoutdoc::applyRun(&doc, run, env, &serial);
}

// What read() says of every block, for comparing a document before and after styling.
QStringList rowsOf(QTextDocument& doc)
{
    QStringList out;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const layoutdoc::Row r = layoutdoc::read(b);
        out << QStringLiteral("%1:%2|%3|%4|%5|%6|%7")
                   .arg(b.blockNumber())
                   .arg(static_cast<int>(r.type))
                   .arg(static_cast<int>(r.kind))
                   .arg(r.keepFormatting ? 1 : 0)
                   .arg(r.time, r.date.toString(Qt::ISODate), r.nick);
    }
    return out;
}

// A chat with a run of status events near its end, then a few messages: what the live test scrolled in.
void fillWithRun(QTextDocument& doc, int before, int events, int after)
{
    setup(doc);
    const tschat::Skin s = tschat::darkSkin();
    for (int i = 0; i < before; ++i)
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:%1:00").arg(i % 60, 2, 10, QLatin1Char('0')), 17, i % 2 ? kUidAlice : kUidBob,
                                                  i % 2 ? QStringLiteral("Alice") : QStringLiteral("Bob"), QStringLiteral("message %1").arg(i)));
    for (int i = 0; i < events; ++i)
        tschat::append(&doc, tschat::statusHtml(s, QStringLiteral("21:00:%1").arg(i, 2, 10, QLatin1Char('0')),
                                                 QStringLiteral("You switched from channel ") + tschat::channelHtml(s, i % 2 ? 4 : 1, i % 2 ? QStringLiteral("Two") : QStringLiteral("One"))
                                                     + QStringLiteral(" to ") + tschat::channelHtml(s, i % 2 ? 1 : 4, i % 2 ? QStringLiteral("One") : QStringLiteral("Two"))));
    for (int i = 0; i < after; ++i)
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:10:%1").arg(i, 2, 10, QLatin1Char('0')), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("after %1").arg(i)));
}

int countKind(QTextDocument& doc, layoutformat::Kind kind)
{
    int n = 0;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const layoutformat::Lead lead = layoutformat::leadOf(b);
        n += lead.styled && lead.kind == kind ? 1 : 0;
    }
    return n;
}

} // namespace

class TestChatLayout : public QObject
{
    Q_OBJECT

  private slots:
    // ---- 1. classification ------------------------------------------------------------------------------
    void classification()
    {
        QTextDocument doc;
        fillChat(doc);
        const layoutdoc::Row first = layoutdoc::read(doc.findBlockByNumber(0));
        QCOMPARE(first.type, RowType::System);
        QCOMPARE(first.time, QStringLiteral("15:56:16"));
        QCOMPARE(first.kind, layoutart::SystemKind::Join);
        const layoutdoc::Row kai = layoutdoc::read(doc.findBlockByNumber(1));
        QCOMPARE(kai.type, RowType::Message);
        QCOMPARE(kai.nick, QStringLiteral("Kai"));
        QCOMPARE(kai.uid, QString::fromLatin1(tschat::kKaiUid));
        QCOMPARE(kai.clientId, 7);
        QCOMPARE(kai.time, QStringLiteral("16:00:17"));
        QVERIFY(kai.outgoing);
        QCOMPARE(kai.nickColor, QColor(0x1c, 0xb0, 0xf4));
        const int day = blockWith(doc, QStringLiteral("9/28/2026"));
        const layoutdoc::Row d = layoutdoc::read(doc.findBlockByNumber(day));
        QCOMPARE(d.type, RowType::Day);
        QCOMPARE(d.date, QDate(2026, 9, 28));
        const layoutdoc::Row print = layoutdoc::read(doc.findBlockByNumber(blockWith(doc, QStringLiteral("2 files sent"))));
        QCOMPARE(print.type, RowType::System);
        QVERIFY(print.ownPrint);
        const layoutdoc::Row left = layoutdoc::read(doc.findBlockByNumber(blockWith(doc, QStringLiteral("disconnected"))));
        QCOMPARE(left.type, RowType::System);
        QCOMPARE(left.kind, layoutart::SystemKind::Leave);

        // Timestamps off, a day line without its icon, a history marker, another plugin's line.
        QTextDocument more;
        setup(more);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&more, tschat::messageHtml(s, QString(), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("no time")));
        tschat::append(&more, tschat::dayHtml(s, QStringLiteral("10/9/2026"), false));
        tschat::append(&more, QStringLiteral("<span style=\"color:#707475\">*** Chat history of April 11, 2026 (4/11/2026)</span>"));
        tschat::append(&more, QStringLiteral("<span style=\"color:#00008b\">Some plugin says hi</span>"));
        tschat::append(&more, tschat::systemHtml(s, QString(), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
        const layoutdoc::Row noTime = layoutdoc::read(more.findBlockByNumber(0));
        QCOMPARE(noTime.type, RowType::Message);
        QVERIFY(noTime.time.isEmpty());
        QCOMPARE(noTime.nick, QStringLiteral("Alice"));
        QCOMPARE(layoutdoc::read(more.findBlockByNumber(1)).type, RowType::Day);
        const layoutdoc::Row history = layoutdoc::read(more.findBlockByNumber(2));
        QCOMPARE(history.type, RowType::History);
        QCOMPARE(history.date, QDate(2026, 4, 11));
        QCOMPARE(layoutdoc::read(more.findBlockByNumber(3)).type, RowType::Other);
        QCOMPARE(layoutdoc::read(more.findBlockByNumber(4)).type, RowType::System); // an icon, no time: a system row

        // Hostile: a fake header inside the text, an absurd nick, a link that only looks like a client.
        QTextDocument hostile;
        setup(hostile);
        tschat::append(&hostile, QStringLiteral("<span>&lt;12:00:00&gt; \"Mallory\": not a header</span>"));
        tschat::append(&hostile, tschat::messageHtml(s, QStringLiteral("12:00:01"), 3, QStringLiteral("x"), QString(70, QLatin1Char('A')), QStringLiteral("too long")));
        QCOMPARE(layoutdoc::read(hostile.findBlockByNumber(0)).type, RowType::Other);
        QCOMPARE(layoutdoc::read(hostile.findBlockByNumber(1)).type, RowType::Other);
    }

    // Review: a system row's kind comes from TeamSpeak's words, never from names or channels users choose.
    void systemRowsIgnoreNamesAndChannels()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s     = tschat::darkSkin();
        const auto         name  = [&](int clid, const char* nick) { return tschat::clientHtml(s, clid, kUidBob, QString::fromLatin1(nick)); };
        const auto         event = [&](const char* time, const QString& html) { tschat::append(&doc, tschat::systemHtml(s, QString::fromLatin1(time), html)); };
        event("20:00:00", name(21, "Pokemon") + QStringLiteral(" connected to channel ") + tschat::channelHtml(s, QStringLiteral("Home")));
        event("20:00:10", name(22, "Terror") + QStringLiteral(" connected to channel ") + tschat::channelHtml(s, QStringLiteral("Error lounge")));
        event("20:00:20", name(23, "Quitter") + QStringLiteral(" connected to channel ") + tschat::channelHtml(s, QStringLiteral("Kicked & banned")));
        event("20:00:30", name(24, "WelcomeBot") + QStringLiteral(" connected to channel ") + tschat::channelHtml(s, QStringLiteral("Home")));
        event("20:00:40", name(22, "Terror") + QStringLiteral(" disconnected (leaving)"));
        event("20:00:50", name(25, "Joiner") + QStringLiteral(" was kicked from the server by ") + name(26, "Admin"));
        event("20:01:00", name(25, "Bob") + QStringLiteral(" poked you: hey"));
        event("20:01:10", QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Poke me")));
        using K = layoutart::SystemKind;
        const QVector<K> want = {K::Join, K::Join, K::Join, K::Join, K::Leave, K::Danger, K::Poke, K::Join};
        const auto check = [&](const char* when) {
            for (int n = 0; n < want.size(); ++n) {
                const layoutdoc::Row row = layoutdoc::read(doc.findBlockByNumber(n));
                QCOMPARE(row.type, RowType::System);
                QVERIFY2(row.kind == want.at(n), qPrintable(QStringLiteral("%1: row %2 is kind %3").arg(QString::fromLatin1(when)).arg(n).arg(static_cast<int>(row.kind))));
                QVERIFY2(!row.keepFormatting, qPrintable(QStringLiteral("%1: row %2 keeps TeamSpeak's formatting").arg(QString::fromLatin1(when)).arg(n)));
            }
            // So joins of such names fold like any other; the real poke and kick never do.
            QVERIFY(layoutdoc::collapsible(layoutdoc::read(doc.findBlockByNumber(0))));
            QVERIFY(!layoutdoc::collapsible(layoutdoc::read(doc.findBlockByNumber(5))));
            QVERIFY(!layoutdoc::collapsible(layoutdoc::read(doc.findBlockByNumber(6))));
        };
        check("unstyled");
        const Snapshot plain = snapshot(doc);
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
        check("cozy");
        layoutdoc::restoreAll(&doc);
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        layoutdoc::styleAll(&doc, envFor(Mode::Compact));
        check("compact");
        // TeamSpeak's own words still decide when no link is involved.
        QCOMPARE(layoutdoc::systemKindOf(QStringLiteral("\"Bob\" was banned"), nullptr), K::Danger);
    }

    // Review: Qt's font cache keeps a font's family after the plugin is unloaded, so a family must never
    // be a QStringLiteral (its data is in our DLL): QString::fromLatin1, like setStyleSheet and QSettings.
    void noLiteralFontFamilies()
    {
        static const QRegularExpression literal(QStringLiteral("QFont\\s*\\(\\s*QStringLiteral|setFamil(y|ies)\\s*\\(\\s*(QStringList\\s*\\{\\s*)?QStringLiteral"));
        QDirIterator it(QStringLiteral(TSMEDIA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        int          files = 0;
        while (it.hasNext()) {
            const QString path = it.next();
            ++files;
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString                 text = QString::fromUtf8(f.readAll());
            const QRegularExpressionMatch m    = literal.match(text);
            QVERIFY2(!m.hasMatch(), qPrintable(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), m.captured(0))));
        }
        QVERIFY(files > 50);
    }

    // ---- 2. byte-exact round trips -------------------------------------------------------------------------
    void roundTripCozyAndCompact()
    {
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            for (const bool dark : {true, false}) {
                QTextDocument plain, styled;
                fillChat(plain, dark ? tschat::darkSkin() : tschat::lightSkin());
                fillChat(styled, dark ? tschat::darkSkin() : tschat::lightSkin());
                const Snapshot before = snapshot(plain);
                QCOMPARE(compare(before, snapshot(styled)), QString());
                const layoutdoc::Env env = envFor(mode, dark);
                const int            n   = layoutdoc::styleAll(&styled, env);
                QVERIFY(n >= styled.blockCount() - 1);
                layoutdoc::renderAll(&styled, env, env.today);
                QVERIFY(styled.toPlainText() != plain.toPlainText());
                QVERIFY(countKind(styled, mode == Mode::Cozy ? layoutformat::Head : layoutformat::CompactHead) >= 5);
                QVERIFY(countKind(styled, layoutformat::Continuation) >= 4);
                QCOMPARE(countKind(styled, layoutformat::Divider), 2);
                QVERIFY(layoutdoc::restoreAll(&styled) > 0);
                const QString diff = compare(before, snapshot(styled));
                QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("mode %1 dark %2: %3").arg(static_cast<int>(mode)).arg(dark).arg(diff)));
                for (QTextBlock b = styled.begin(); b.isValid(); b = b.next())
                    QVERIFY(!layoutdoc::hasOurs(b));
            }
        }
    }

    void roundTripWithReplies()
    {
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("Hello world")));
            replies::Original alice;
            alice.nick     = QStringLiteral("Alice");
            alice.uid      = kUidAlice;
            alice.clientId = 17;
            alice.minutes  = 21 * 60 + 14;
            alice.text     = QStringLiteral("Hello world");
            const QString quote = replies::quoteLine(alice);
            // TeamSpeak's rendering of the quote line's BBCode (italic, the author linked).
            QString quoteHtml = quote;
            quoteHtml.replace(QStringLiteral("[i]"), QStringLiteral("<i>")).replace(QStringLiteral("[/i]"), QStringLiteral("</i>"));
            quoteHtml.replace(QRegularExpression(QStringLiteral("\\[URL=([^\\]]+)\\]")), QStringLiteral("<a href=\"\\1\">")).replace(QStringLiteral("[/URL]"), QStringLiteral("</a>"));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:15:30"), 18, kUidBob, QStringLiteral("Bob"), quoteHtml + QStringLiteral("<br>my reply")));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:15:40"), 18, kUidBob, QStringLiteral("Bob"), QStringLiteral("and more")));
            const Snapshot plain = snapshot(doc);
            // ChatReplies first (the pass order), then the layout.
            const QVector<replydoc::Message> ms = replydoc::scan(&doc);
            QCOMPARE(ms.size(), 3);
            QVERIFY(ms.at(1).hasQuote);
            QVERIFY(replydoc::collapse(&doc, ms.at(1), replydoc::objectPrefix() + QStringLiteral("t.1"), QSizeF(200, 14)));
            const Snapshot replied = snapshot(doc);
            layoutdoc::Env env = envFor(mode);
            env.replyHeader    = [](int, replyart::Header* h) {
                h->nick    = QStringLiteral("Alice");
                h->snippet = QStringLiteral("Hello world");
                h->found   = true;
                return true;
            };
            QVERIFY(layoutdoc::styleAll(&doc, env) == 3);
            const QTextBlock reply = doc.findBlockByNumber(1);
            const layoutformat::Lead lead = layoutformat::leadOf(reply);
            QVERIFY(lead.styled);
            QVERIFY(lead.reply >= 0);
            QCOMPARE(lead.kind, mode == Mode::Cozy ? layoutformat::Head : layoutformat::CompactHead);
            // The reply is never a continuation; the message after it may join it.
            QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(2)).kind, layoutformat::Continuation);
            // One header parser: the reply reads back with its nick, its time and its text.
            const replydoc::Message m = replydoc::parseBlock(reply);
            QCOMPARE(m.nick, QStringLiteral("Bob"));
            QCOMPARE(m.minutes, 21 * 60 + 15);
            QVERIFY(m.restyled);
            QVERIFY(m.hasQuote);
            QCOMPARE(m.quote.nick, QStringLiteral("Alice"));
            QCOMPARE(m.text, QStringLiteral("my reply"));
            QCOMPARE(m.uid, kUidBob);
            QCOMPARE(m.clientId, 18);
            layoutdoc::restoreAll(&doc);
            QCOMPARE(compare(replied, snapshot(doc)), QString());
            QVERIFY(replydoc::restoreAll(&doc) > 0);
            QCOMPARE(compare(plain, snapshot(doc)), QString());
        }
    }

    void roundTripWithHdEmoji()
    {
        if (!emoji::hasColor())
            QSKIP("no colour emoji engine here");
        // HD emoji on, then the layout, then HD emoji off, then the layout off; and the other order.
        for (int order = 0; order < 2; ++order) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("21:00:00"), tschat::clientHtml(s, 18, kUidBob, QStringLiteral("Bob")) + QStringLiteral(" connected 🎮 to channel")));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("Pizza 🍕 tonight? :)")));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:30"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("😂😂😂")));
            const Snapshot plain = snapshot(doc);
            const layoutdoc::Env env = envFor(Mode::Cozy);
            if (order == 0) {
                ChatEmoji::processDocument(&doc, 1.0);
                QVERIFY(ChatEmoji::countEmoji(&doc) >= 5);
                layoutdoc::styleAll(&doc, env);
                // Jumbo works through the layout (the header is ours now).
                ChatEmoji::processDocument(&doc, 1.0);
                ChatEmoji::restore(&doc);
                QCOMPARE(ChatEmoji::countEmoji(&doc), 0);
                layoutdoc::restoreAll(&doc);
            } else {
                layoutdoc::styleAll(&doc, env);
                ChatEmoji::processDocument(&doc, 1.0);
                QVERIFY(ChatEmoji::countEmoji(&doc) >= 5);
                // A message of only emoji is still large under the layout.
                const int  last  = doc.blockCount() - 1;
                bool       jumbo = false;
                for (auto it = doc.findBlockByNumber(last).begin(); !it.atEnd(); ++it)
                    jumbo = jumbo || it.fragment().charFormat().boolProperty(emojiformat::kJumbo);
                QVERIFY(jumbo);
                layoutdoc::restoreAll(&doc);
                ChatEmoji::restore(&doc);
            }
            const QString diff = compare(plain, snapshot(doc));
            QVERIFY2(diff.isEmpty(), qPrintable(QStringLiteral("order %1: %2").arg(order).arg(diff)));
        }
    }

    // ---- 3. grouping ----------------------------------------------------------------------------------------
    void groupingTruthTable()
    {
        struct Case {
            const char* name;
            QString     uid2;
            QString     nick2;
            QString     time2;
            bool        divider;
            bool        joins;
        };
        const QVector<Case> cases = {
            {"same author within 7 min", kUidAlice, QStringLiteral("Alice"), QStringLiteral("21:21:02"), false, true},
            {"7:00 exactly", kUidAlice, QStringLiteral("Alice"), QStringLiteral("21:21:02"), false, true},
            {"7:01", kUidAlice, QStringLiteral("Alice"), QStringLiteral("21:21:03"), false, false},
            {"earlier time", kUidAlice, QStringLiteral("Alice"), QStringLiteral("21:14:01"), false, false},
            {"another uid", kUidBob, QStringLiteral("Alice"), QStringLiteral("21:14:10"), false, false},
            {"renamed", kUidAlice, QStringLiteral("Alicia"), QStringLiteral("21:14:10"), false, false},
            {"a divider between", kUidAlice, QStringLiteral("Alice"), QStringLiteral("21:14:10"), true, false},
        };
        for (const Case& c : cases) {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("one")));
            if (c.divider)
                tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026")));
            tschat::append(&doc, tschat::messageHtml(s, c.time2, 17, c.uid2, c.nick2, QStringLiteral("two")));
            layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
            const layoutformat::Kind kind = layoutformat::leadOf(doc.lastBlock()).kind;
            QVERIFY2((kind == layoutformat::Continuation) == c.joins, c.name);
        }
        // A system row between two messages of the same author breaks the group; grouping off: never.
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("one")));
        tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("21:14:05"), QStringLiteral("Bob connected")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:06"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("two")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("21:14:07"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("three")));
        layoutdoc::Env env = envFor(Mode::Cozy);
        layoutdoc::styleAll(&doc, env);
        QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(2)).kind, layoutformat::Head);
        QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(3)).kind, layoutformat::Continuation);
        layoutdoc::restoreAll(&doc);
        env.group = false;
        layoutdoc::styleAll(&doc, env);
        QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(3)).kind, layoutformat::Head);
        // An album-hidden block between is skipped.
        layoutdoc::restoreAll(&doc);
        env.group = true;
        QTextBlock hidden = doc.findBlockByNumber(1);
        hidden.setVisible(false);
        layoutdoc::styleAll(&doc, env);
        QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(2)).kind, layoutformat::Continuation);
    }

    // ---- 4. geometry ----------------------------------------------------------------------------------------
    void geometry()
    {
        QTextDocument doc;
        fillChat(doc);
        const layoutdoc::Env env = envFor(Mode::Cozy);
        QCOMPARE(env.tokens.f, 12);
        QCOMPARE(env.tokens.G, 48);
        QCOMPARE(env.tokens.avatar, 24);
        QCOMPARE(env.tokens.lineMin, 17);
        QCOMPARE(env.tokens.replyRow, 18);
        layoutdoc::styleAll(&doc, env);
        doc.documentLayout()->documentSize();
        const qreal margin = doc.documentMargin();
        Q_UNUSED(margin);
        // A head: line 0 is the head picture at x 0, its height exactly; the body starts at G.
        const QTextBlock head   = doc.findBlockByNumber(1);
        const QTextLayout* hl   = head.layout();
        QVERIFY(hl->lineCount() >= 2);
        qInfo("head line 0: height %.2f ascent %.2f descent %.2f leading %.2f; line 1 y %.2f", hl->lineAt(0).height(), hl->lineAt(0).ascent(),
              hl->lineAt(0).descent(), hl->lineAt(0).leading(), hl->lineAt(1).y());
        QCOMPARE(hl->lineAt(0).x(), 0.0);
        QVERIFY2(qAbs(hl->lineAt(0).height() - env.tokens.headRow) <= 1.0, qPrintable(QString::number(hl->lineAt(0).height())));
        QCOMPARE(hl->lineAt(1).x(), 48.0);
        // The head takes exactly its height in the flow; MinimumHeight puts the body line's extra pixel above
        // its text (so the body's first line starts at most 2 px below the picture).
        const qreal gap = hl->lineAt(1).y() - (hl->lineAt(0).y() + env.tokens.headRow);
        QVERIFY2(gap >= -0.01 && gap <= 3.01 && hl->lineAt(0).y() >= -1.01, qPrintable(QStringLiteral("line 0 y %1, line 1 y %2").arg(hl->lineAt(0).y()).arg(hl->lineAt(1).y())));
        // A continuation: the text at G on line 0.
        const QTextBlock cont = doc.findBlockByNumber(2);
        QCOMPARE(layoutformat::leadOf(cont).kind, layoutformat::Continuation);
        QCOMPARE(cont.layout()->lineAt(0).x(), 0.0);
        QCOMPARE(cont.layout()->lineAt(0).cursorToX(1), 48.0);
        // The 3-line message keeps its lines at G.
        const QTextBlock three = doc.findBlockByNumber(blockWith(doc, QStringLiteral("line two")));
        QVERIFY(three.layout()->lineCount() >= 4);
        QCOMPARE(three.layout()->lineAt(3).x(), 48.0);
        // Body lines are at least Lh (17) apart (MinimumHeight), never Proportional.
        QVERIFY2(three.layout()->lineAt(2).y() - three.layout()->lineAt(1).y() >= 17.0 - 0.01,
                 qPrintable(QString::number(three.layout()->lineAt(2).y() - three.layout()->lineAt(1).y())));
        QCOMPARE(three.blockFormat().lineHeightType(), static_cast<int>(QTextBlockFormat::MinimumHeight));

        // Compact: the head on the text's baseline (within half a pixel), L = 38 at 12 px.
        QTextDocument compact;
        fillChat(compact);
        const layoutdoc::Env cenv = envFor(Mode::Compact);
        // L = ceil(advance("00:00") in the 0.92 f font) + 10: about 38 at 12 px (the font decides the pixel).
        const int expectedL = static_cast<int>(std::ceil(QFontMetricsF(cenv.tokens.compactTime).horizontalAdvance(QStringLiteral("00:00")))) + 10;
        QCOMPARE(cenv.tokens.L, expectedL);
        // The range is Windows' Segoe UI at 12 px; the offscreen platform (-platform offscreen) measures
        // with a font engine of its own, so only the formula above is checked there.
        if (QGuiApplication::platformName() != QLatin1String("offscreen"))
            QVERIFY2(cenv.tokens.L >= 34 && cenv.tokens.L <= 42, qPrintable(QString::number(cenv.tokens.L)));
        QVERIFY(layoutart::tokensFor(Mode::Compact, chatFont(), true).L > cenv.tokens.L); // "12:59 PM" is wider
        layoutdoc::styleAll(&compact, cenv);
        compact.documentLayout()->documentSize();
        const QTextBlock ch = compact.findBlockByNumber(1);
        QCOMPARE(layoutformat::leadOf(ch).kind, layoutformat::CompactHead);
        const QTextLine line = ch.layout()->lineAt(0);
        QCOMPARE(line.x(), 0.0);
    }

    // The compact head's name is drawn on the body text's baseline (within half a pixel): where Qt really
    // paints the picture, found in the pixels.
    void compactBaseline()
    {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            QSKIP("Needs Windows' own font engine: the offscreen platform puts the text baseline elsewhere (run without -platform offscreen)");
        {
            QTextDocument doc;
            setup(doc);
            const tschat::Skin s = tschat::darkSkin();
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("18:10:05"), 18, kUidBob, QStringLiteral("Reza"), QStringLiteral("Road trip! gjpqy")));
            tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("18:11:00"), QStringLiteral("Bob connected")));
            const layoutdoc::Env env = envFor(Mode::Compact);
            layoutdoc::styleAll(&doc, env);
            for (int n = 0; n < 2; ++n) {
                const QTextBlock         b    = doc.findBlockByNumber(n);
                const layoutformat::Lead lead = layoutformat::leadOf(b);
                QVERIFY(lead.styled);
                const QTextImageFormat image = lead.format.toImageFormat();
                QImage solid(qRound(image.width()), qRound(image.height()), QImage::Format_ARGB32_Premultiplied);
                solid.fill(QColor(255, 0, 255));
                doc.addResource(QTextDocument::ImageResource, QUrl(image.name()), solid);
            }
            doc.documentLayout()->documentSize();
            QImage canvas(doc.size().toSize() + QSize(2, 2), QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::black);
            {
                QPainter p(&canvas);
                doc.drawContents(&p);
            }
            const qreal ascent = layoutart::bodyAscent(env.tokens);
            for (int n = 0; n < 2; ++n) {
                const QTextBlock b    = doc.findBlockByNumber(n);
                const QRectF     br   = doc.documentLayout()->blockBoundingRect(b);
                const QTextLine  line = b.layout()->lineAt(0);
                const int        x    = qRound(br.left() + 4);
                int              top  = -1;
                for (int y = qMax(0, qFloor(br.top())); y < canvas.height(); ++y) {
                    if (canvas.pixelColor(x, y) == QColor(255, 0, 255)) {
                        top = y;
                        break;
                    }
                }
                QVERIFY2(top >= 0, "the picture is drawn");
                const qreal drawnBaseline = top + ascent;
                const qreal textBaseline  = br.top() + line.y() + line.ascent();
                QVERIFY2(qAbs(drawnBaseline - textBaseline) <= 0.5 + 1e-6,
                         qPrintable(QStringLiteral("block %1: drawn baseline %2, text baseline %3").arg(n).arg(drawnBaseline).arg(textBaseline)));
            }
        }
    }


    void previewsAreNotStretched()
    {
        // A preview picture in a message keeps its height (MinimumHeight never makes a line taller than it).
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("18:07:10"), 14, kUidAlice, QStringLiteral("Alice"), tschat::linkHtml(s, QStringLiteral("ts3file://x"), QStringLiteral("shot.png"))));
        QTextCursor c(&doc);
        c.movePosition(QTextCursor::End);
        c.insertText(QString(QChar::LineSeparator));
        QTextImageFormat preview;
        preview.setName(QStringLiteral("tsmedia:preview"));
        preview.setWidth(240);
        preview.setHeight(153);
        c.insertImage(preview);
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
        doc.documentLayout()->documentSize();
        const QTextLayout* l = doc.firstBlock().layout();
        QVERIFY(l->lineCount() >= 3);
        QVERIFY2(qAbs(l->lineAt(2).height() - 153.0) < 4.0, qPrintable(QString::number(l->lineAt(2).height())));
    }

    // ---- 5. formats TeamSpeak copies from ours ----------------------------------------------------------------
    void inheritance()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::systemHtml(s, QStringLiteral("15:56:16"), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"))));
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
        QVERIFY(layoutformat::leadOf(doc.firstBlock()).styled);
        // TeamSpeak appends a plain line after it: a new block that copies the last one's formats.
        QTextCursor c(&doc);
        c.movePosition(QTextCursor::End);
        c.insertBlock();
        c.insertText(QStringLiteral("typed after"));
        const QTextBlock added = doc.lastBlock();
        QVERIFY(added.blockFormat().hasProperty(layoutformat::kOrigBlock)); // took ours over
        QVERIFY(layoutdoc::hasOurs(added));
        QVERIFY(layoutdoc::sanitize(&doc, added.blockNumber()));
        QVERIFY(!layoutdoc::hasOurs(doc.lastBlock()));
        // It reads as TeamSpeak would have made it: no trailer kind, no muted colour of ours.
        for (auto it = doc.lastBlock().begin(); !it.atEnd(); ++it) {
            QVERIFY(!it.fragment().charFormat().hasProperty(layoutformat::kKind));
            QVERIFY(!it.fragment().charFormat().hasProperty(layoutformat::kApplied));
        }
        layoutdoc::restoreAll(&doc);
        QVERIFY(!layoutdoc::hasOurs(doc.firstBlock()));
    }

    // ---- 6. copying --------------------------------------------------------------------------------------------
    void copyGivesTeamSpeaksText()
    {
        QTextDocument plain, styled;
        fillChat(plain);
        fillChat(styled);
        layoutdoc::styleAll(&styled, envFor(Mode::Cozy));
        const QString expected = ChatEmoji::originalText(&plain, 0, plain.characterCount() - 1);
        const QString got      = ChatEmoji::originalText(&styled, 0, styled.characterCount() - 1);
        QCOMPARE(got, expected);
        QVERIFY(got.contains(QStringLiteral("<16:00:17> \"Kai\": https://example.com/live/first-stream")));
        QVERIFY(got.contains(QStringLiteral("*** 9/28/2026")));
        QVERIFY(!got.contains(QStringLiteral("  20:15"))); // no trailer
        // One message's body.
        const int        n  = blockWith(plain, QStringLiteral("night-owl"));
        const QTextBlock pb = plain.findBlockByNumber(n);
        const QTextBlock sb = styled.findBlockByNumber(n);
        QCOMPARE(ChatEmoji::originalText(&styled, layoutformat::bodyStart(sb), sb.position() + sb.length() - 1),
                 ChatEmoji::originalText(&plain, replydoc::parseBlock(pb).textStart, pb.position() + pb.length() - 1));
        // Compact too.
        QTextDocument compact;
        fillChat(compact);
        layoutdoc::styleAll(&compact, envFor(Mode::Compact));
        QCOMPARE(ChatEmoji::originalText(&compact, 0, compact.characterCount() - 1), expected);
    }

    // ---- 7. the one header parser ------------------------------------------------------------------------------
    void parsersAgreeOnStyledDocuments()
    {
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            QTextDocument plain, styled;
            fillChat(plain);
            fillChat(styled);
            layoutdoc::styleAll(&styled, envFor(mode));
            const QVector<replydoc::Message> a = replydoc::scan(&plain);
            const QVector<replydoc::Message> b = replydoc::scan(&styled);
            QCOMPARE(b.size(), a.size());
            for (int i = 0; i < a.size(); ++i) {
                QCOMPARE(b.at(i).block, a.at(i).block);
                QCOMPARE(b.at(i).nick, a.at(i).nick);
                QCOMPARE(b.at(i).uid, a.at(i).uid);
                QCOMPARE(b.at(i).clientId, a.at(i).clientId);
                QCOMPARE(b.at(i).minutes, a.at(i).minutes);
                QCOMPARE(b.at(i).nickColor, a.at(i).nickColor);
                QCOMPARE(b.at(i).text, a.at(i).text);
                QCOMPARE(b.at(i).mediaLabel, a.at(i).mediaLabel);
            }
            // Albums read the same header (the sender's uid) from the styled document.
            for (int n = 0; n < plain.blockCount(); ++n) {
                const layoutformat::Header h = layoutformat::headerOf(styled.findBlockByNumber(n));
                if (!h.valid)
                    continue;
                const QTextBlock pb   = plain.findBlockByNumber(n);
                const albums::Header expected = albums::parseHeaderText(pb.text());
                const albums::Header got      = albums::parseHeader(h.lead, h.nickText, h.nickHref, h.after);
                QVERIFY(got.valid);
                QCOMPARE(got.nick, expected.nick);
                QCOMPARE(got.uid, albums::uidFromClientHref(h.nickHref));
            }
        }
    }

    void noOtherHeaderParsers()
    {
        // Every module reads TeamSpeak's header through replydoc::parseBlock / layoutformat (a styled
        // block has no client:// link at its start any more). The files allowed to look for one:
        const QSet<QString> allowed = {QStringLiteral("replydoc.cpp"), QStringLiteral("replies.cpp"),  QStringLiteral("layoutdoc.cpp"),
                                       QStringLiteral("layoutformat.h"), QStringLiteral("albums.cpp"), QStringLiteral("albums.h"),
                                       QStringLiteral("replies.h"),     QStringLiteral("replydoc.h"),  QStringLiteral("selftest.cpp"),
                                       QStringLiteral("chatemoji.cpp"), QStringLiteral("chatalbums.cpp")};
        QDirIterator it(QStringLiteral(TSMEDIA_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files);
        int          files = 0;
        while (it.hasNext()) {
            const QString path = it.next();
            ++files;
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(f.readAll());
            if (text.contains(QLatin1String("\"client://\"")) || text.contains(QLatin1String("(\"client://")))
                QVERIFY2(allowed.contains(QFileInfo(path).fileName()), qPrintable(QFileInfo(path).fileName()));
        }
        QVERIFY(files > 50);
        // The ones allowed check our layout first.
        for (const char* name : {"chatemoji.cpp", "chatalbums.cpp"}) {
            QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/src/") + QString::fromLatin1(name));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(QString::fromUtf8(f.readAll()).contains(QLatin1String("layoutformat::")), name);
        }
    }

    // ---- 8. dates ----------------------------------------------------------------------------------------------
    void datesAndDividers()
    {
        const QDate today(2026, 10, 10);
        QCOMPARE(layoutart::dayLabel(today, today), QStringLiteral("Today, October 10, 2026"));
        QCOMPARE(layoutart::dayLabel(today.addDays(-1), today), QStringLiteral("Yesterday, October 9, 2026"));
        QCOMPARE(layoutart::dayLabel(QDate(2026, 9, 28), today), QStringLiteral("Monday, September 28, 2026"));
        QCOMPARE(layoutart::historyLabel(QDate(2026, 4, 11)), QStringLiteral("History \u00b7 April 11, 2026"));
        QCOMPARE(layoutart::headTimeLabel(today, today, QStringLiteral("16:00:17"), false), QStringLiteral("Today at 16:00"));
        QCOMPARE(layoutart::headTimeLabel(today.addDays(-1), today, QStringLiteral("22:34:01"), false), QStringLiteral("Yesterday at 22:34"));
        QCOMPARE(layoutart::headTimeLabel(QDate(), today, QStringLiteral("16:00:17"), false), QStringLiteral("16:00"));
        QCOMPARE(layoutart::headTimeLabel(today, today, QStringLiteral("4:00:17 PM"), true), QStringLiteral("Today at 4:00 PM"));
        QCOMPARE(layoutart::fullTimeLabel(QDate(2026, 10, 10), QStringLiteral("16:00:17")), QStringLiteral("Saturday, October 10, 2026 16:00:17"));
        QCOMPARE(layoutart::parseDayText(QStringLiteral("9/28/2026")), QDate(2026, 9, 28));
        QCOMPARE(layoutart::parseDayText(QStringLiteral("28.9.2026")), QDate(2026, 9, 28));
        QCOMPARE(layoutart::parseDayText(QStringLiteral("2026-09-28")), QDate(2026, 9, 28));
        QVERIFY(!layoutart::parseDayText(QStringLiteral("You are now talking")).isValid());
        QCOMPARE(layoutart::secondsOf(QStringLiteral("16:00:17")), 16 * 3600 + 17);
        QCOMPARE(layoutart::secondsOf(QStringLiteral("4:00:17 PM")), 16 * 3600 + 17);
        QCOMPARE(layoutart::secondsOf(QStringLiteral("12:05:00 AM")), 5 * 60);
        QCOMPARE(layoutart::secondsOf(QStringLiteral("25:00:00")), -1);
        // Accepted only up to tomorrow and never before the divider above.
        QCOMPARE(layoutdoc::acceptDate(QDate(2026, 10, 11), QDate(), today), QDate(2026, 10, 11));
        QVERIFY(!layoutdoc::acceptDate(QDate(2026, 10, 12), QDate(), today).isValid());
        QVERIFY(!layoutdoc::acceptDate(QDate(2026, 9, 1), QDate(2026, 9, 28), today).isValid());
        QTextDocument doc;
        fillChat(doc);
        const QVector<QDate> dates = layoutdoc::datesOf(&doc, today);
        QVERIFY(!dates.value(1).isValid()); // before any divider: unknown
        QCOMPARE(dates.value(blockWith(doc, QStringLiteral("20:34:51"))), QDate(2026, 9, 28));
        QCOMPARE(dates.value(blockWith(doc, QStringLiteral("WARP"))), QDate(2026, 9, 29));
        // Lines TeamSpeak appends cost only themselves: extending gives what reading everything gives.
        QVector<QDate> part = dates;
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:00:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("morning")));
        layoutdoc::extendDates(&doc, today, &part, part.size() - 1); // the last line again too (TeamSpeak fills it after adding it)
        const QVector<QDate> all = layoutdoc::datesOf(&doc, today);
        QCOMPARE(part.size(), all.size());
        for (int i = 0; i < all.size(); ++i)
            QVERIFY2(part.at(i) == all.at(i), qPrintable(QString::number(i)));
        QCOMPARE(part.last(), today);
    }

    // ---- 12. contrast ---------------------------------------------------------------------------------------------
    void contrastOfEveryPair()
    {
        struct Skin {
            const char* name;
            bool        dark;
            QColor      base, text, link;
        };
        const QVector<Skin> skins = {
            {"dark", true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4)},
            {"light", false, QColor(0xff, 0xff, 0xff), QColor(0, 0, 0), QColor(0x1c, 0x82, 0xcc)},
            {"synthetic black", true, QColor(0x10, 0x10, 0x10), QColor(0xc0, 0xc0, 0xc0), QColor(0x30, 0x60, 0xd0)},
            {"synthetic grey", false, QColor(0xe8, 0xe8, 0xe8), QColor(0x30, 0x30, 0x30), QColor(0x50, 0x90, 0xe0)},
            {"synthetic blue", true, QColor(0x1e, 0x2a, 0x3a), QColor(0xd0, 0xd8, 0xe0), QColor(0x60, 0x90, 0xff)},
        };
        for (const Skin& s : skins) {
            const layoutart::Colors c = layoutart::colorsFor(s.dark, s.base, s.text, s.link);
            for (const layoutart::Pair& p : layoutart::contrastPairs(c)) {
                const double ratio = ui::contrastRatio(p.fore, p.back);
                QVERIFY2(ratio >= p.minimum - 0.005, qPrintable(QStringLiteral("%1: %2 %3 on %4: %5").arg(QString::fromLatin1(s.name), p.what, p.fore.name(), p.back.name()).arg(ratio)));
            }
        }
        // The spec's values on TeamSpeak's two skins.
        const layoutart::Colors dark = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
        QCOMPARE(dark.link.name(), QStringLiteral("#1cb0f4"));
        QCOMPARE(layoutart::hoverRow(dark).name(), QStringLiteral("#282a2e"));
        const layoutart::Colors light = layoutart::colorsFor(false, QColor(Qt::white), QColor(Qt::black), QColor(0x1c, 0x82, 0xcc));
        QCOMPARE(light.link.name(), QStringLiteral("#1872b8"));
        QCOMPARE(layoutart::hoverRow(light).name(), QStringLiteral("#f5f5f5"));
        // Initials: white keeps 4.5:1 on all eight colours.
        for (int i = 0; i < 64; ++i)
            QVERIFY(ui::contrastRatio(Qt::white, layoutart::initialsColor(QString::number(i))) >= 4.5);
        // Friend / blocked colours keep their hue at 4.5:1.
        const QColor friendly = layoutart::nameColorFor(dark, QColor(0x1c, 0xa0, 0x37), QColor(0x1c, 0xb0, 0xf4));
        QVERIFY(ui::contrastRatio(friendly, dark.base) >= 4.5);
        QVERIFY(friendly.green() > friendly.red());
        QCOMPARE(layoutart::nameColorFor(dark, QColor(0x1c, 0xb0, 0xf4), QColor(0x1c, 0xb0, 0xf4)), dark.name);
    }

    // ---- 8. midnight: "Today" becomes "Yesterday" (the pictures are drawn again) ------------------------------------
    void midnightMovesTheLabels()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("23:59:30"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("good night")));
        layoutdoc::Env env = envFor(Mode::Cozy);
        layoutdoc::styleAll(&doc, env);
        const QVector<QDate> dates = layoutdoc::datesOf(&doc, env.today);
        const QString        today = layoutdoc::signature(env, doc.findBlockByNumber(1), dates.value(1), layoutdoc::Look());
        QVERIFY(today.contains(QStringLiteral("Today at 23:59")));
        env.today                  = env.today.addDays(1); // the owned timer at midnight + 1 s refreshes the day
        const QString yesterday    = layoutdoc::signature(env, doc.findBlockByNumber(1), dates.value(1), layoutdoc::Look());
        QVERIFY(yesterday.contains(QStringLiteral("Yesterday at 23:59")));
        QVERIFY(layoutdoc::signature(env, doc.findBlockByNumber(0), dates.value(0), layoutdoc::Look()).contains(QStringLiteral("Yesterday, October 10, 2026")));
    }

    // ---- 9. mentions (P2) ----------------------------------------------------------------------------------------
    void mentions()
    {
        // As text (comparing QVectors warns with this compiler's checked iterators).
        const auto R = [](const QString& text, const QString& nick) {
            QStringList out;
            for (const QPair<int, int>& r : layoutdoc::mentionRanges(text, nick))
                out << QStringLiteral("%1-%2").arg(r.first).arg(r.second);
            return out.join(QLatin1Char(','));
        };
        QCOMPARE(R(QStringLiteral("@Mehdi are you coming?"), QStringLiteral("Mehdi")), QStringLiteral("0-6"));
        QCOMPARE(R(QStringLiteral("hey mehdi! and MEHDI"), QStringLiteral("Mehdi")), QStringLiteral("4-9,15-20"));
        QVERIFY(layoutdoc::mentionRanges(QStringLiteral("Mehdiz and xMehdi"), QStringLiteral("Mehdi")).isEmpty());
        QCOMPARE(R(QStringLiteral("@Al hi"), QStringLiteral("Al")), QStringLiteral("0-3"));
        QVERIFY(layoutdoc::mentionRanges(QStringLiteral("Al hi, also"), QStringLiteral("Al")).isEmpty()); // a word needs 3 characters
        QCOMPARE(R(QStringLiteral("ping Dex- now"), QStringLiteral("Dex-")), QStringLiteral("5-9"));

        const QString kUidMe = QStringLiteral("q0Xn6Mine0000000000000000000=");
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("17:45:37"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("@Mehdi are you coming tonight?")));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("17:45:40"), 17, kUidAlice, QStringLiteral("Alice"), tschat::linkHtml(s, QStringLiteral("https://mehdi.example/"))));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("17:46:00"), 7, kUidMe, QStringLiteral("Mehdi"), QStringLiteral("Mehdi here, yes"), true));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("17:47:00"), 17, kUidAlice, QStringLiteral("Mehdi"), QStringLiteral("no mention in my header")));
        const Snapshot plain = snapshot(doc);
        layoutdoc::Env env   = envFor(Mode::Cozy);
        env.mentions         = true;
        env.ownNick          = QStringLiteral("Mehdi");
        env.ownUid           = kUidMe;
        layoutdoc::styleAll(&doc, env);
        QVERIFY(layoutdoc::isMention(doc.findBlockByNumber(0)));
        QVERIFY(!layoutdoc::isMention(doc.findBlockByNumber(1))); // inside a link
        QVERIFY(!layoutdoc::isMention(doc.findBlockByNumber(2))); // your own message
        QVERIFY(!layoutdoc::isMention(doc.findBlockByNumber(3))); // the header is never read
        // The pill: "@Mehdi" only, in the pill's colours, DemiBold.
        const QTextBlock first = doc.findBlockByNumber(0);
        const int        body  = layoutformat::bodyStart(first);
        QTextCursor      c(&doc);
        c.setPosition(body + 1);
        c.setPosition(body + 2, QTextCursor::KeepAnchor);
        QCOMPARE(c.charFormat().background().color(), env.colors.pillBack);
        QCOMPARE(c.charFormat().fontWeight(), static_cast<int>(QFont::DemiBold));
        c.setPosition(body + 7);
        c.setPosition(body + 8, QTextCursor::KeepAnchor);
        QCOMPARE(c.charFormat().background().style(), Qt::NoBrush);
        // Mentions off: none.
        layoutdoc::restoreAll(&doc);
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        env.mentions = false;
        layoutdoc::styleAll(&doc, env);
        QVERIFY(!layoutdoc::isMention(doc.findBlockByNumber(0)));
        layoutdoc::restoreAll(&doc);
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        // The pill's text reads on the mention row (the contrast test checks the pairs).
        QVERIFY(ui::contrastRatio(env.colors.pillText, ui::flatten(env.colors.pillBack, layoutart::mentionRow(env.colors))) >= 4.5);
    }

    // ---- 10. collapsed runs of events (P2) ---------------------------------------------------------------------------
    void collapsingRuns()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s     = tschat::darkSkin();
        const QString      home  = QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("Home"));
        const auto         event = [&](const char* time, const QString& html) { tschat::append(&doc, tschat::systemHtml(s, QString::fromLatin1(time), html)); };
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:00:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("hi")));
        event("20:01:00", home); // 1-3: the same event three times ("x3")
        event("20:02:00", home);
        event("20:03:00", home);
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:04:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("hi again")));
        event("20:05:00", QStringLiteral("\"Bob\" connected to channel \"Home\""));   // 5: a run of 5
        event("20:05:10", QStringLiteral("\"Carl\" connected to channel \"Home\""));
        event("20:05:20", QStringLiteral("\"Dana\" connected to channel \"Home\""));
        event("20:05:30", QStringLiteral("\"Bob\" disconnected (leaving)"));
        event("20:05:40", QStringLiteral("\"Erin\" connected to channel \"Home\""));
        event("20:06:00", QStringLiteral("\"Bob\" poked you: hey"));               // 10: never collapsed, ends the run
        event("20:06:10", QStringLiteral("\"Fay\" connected to channel \"Home\""));
        event("20:06:20", QStringLiteral("\"Gus\" connected to channel \"Home\""));
        const Snapshot plain = snapshot(doc);
        const QString  copy  = ChatEmoji::originalText(&doc, 0, doc.characterCount() - 1);
        layoutdoc::Env env   = envFor(Mode::Cozy);
        env.collapse         = true;
        layoutdoc::styleAll(&doc, env);
        int serial = 1;
        const auto collapse = [&](const QSet<int>& expanded) {
            for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(&doc, 0, doc.blockCount() - 1, expanded))
                layoutdoc::applyRun(&doc, run, env, &serial);
        };
        collapse({});
        const auto shown = [&](int n) { return doc.findBlockByNumber(n).isVisible(); };
        const auto chips = [&](int n) {
            QStringList out;
            for (const layoutdoc::ChipRef& c : layoutdoc::chipsOf(doc.findBlockByNumber(n)))
                out << c.text;
            return out;
        };
        QVERIFY(shown(1) && !shown(2) && !shown(3));
        QCOMPARE(chips(1), QStringList{QString(QChar(0x00d7)) + QStringLiteral("3")});
        QVERIFY(shown(5) && !shown(6) && !shown(7) && !shown(8) && !shown(9));
        QCOMPARE(chips(5), QStringList{QStringLiteral("+4 more events")});
        QVERIFY(doc.findBlockByNumber(5).blockFormat().boolProperty(layoutformat::kRunHead)); // ChatLayout draws its chips
        QVERIFY(doc.findBlockByNumber(1).blockFormat().boolProperty(layoutformat::kRunHead));
        QVERIFY(!doc.findBlockByNumber(6).blockFormat().boolProperty(layoutformat::kRunHead));
        QVERIFY(shown(10) && shown(11) && shown(12)); // the poke, and a run of two after it
        QVERIFY(chips(11).isEmpty());
        // Opened by the reader: everything, and "Show fewer".
        const int tag = doc.findBlockByNumber(5).blockFormat().intProperty(layoutformat::kBlockTag);
        QVERIFY(tag > 0);
        collapse({tag});
        QVERIFY(shown(5) && shown(6) && shown(7) && shown(8) && shown(9));
        QCOMPARE(chips(5), QStringList{QStringLiteral("Show fewer")});
        QVERIFY(!shown(2)); // the duplicates stay folded into "x3"
        // A new event at the end of an open run keeps it open; closed again it counts it.
        collapse({});
        QCOMPARE(chips(5), QStringList{QStringLiteral("+4 more events")});
        // Copying still gives TeamSpeak's text (chips give nothing; hidden rows are still in the chat).
        QCOMPARE(ChatEmoji::originalText(&doc, 0, doc.characterCount() - 1), copy);
        // Restored: every row shown, no chip, byte-exact.
        layoutdoc::restoreAll(&doc);
        QCOMPARE(compare(plain, snapshot(doc)), QString());
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            QVERIFY(b.isVisible());
    }

    // ---- 10b. status lines without "*** " (the server tab, private chats: the live test's real lines) -------------
    void serverTabStatusLines()
    {
        using K = layoutart::SystemKind;
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::fillServerTab(&doc, s);
        QCOMPARE(doc.blockCount(), 13);
        struct Want {
            RowType type;
            K       kind;
            bool    keep;
        };
        const QVector<Want> want = {
            {RowType::History, K::Info, false}, // *** Log begins 2026-10-10 21:40:46
            {RowType::System, K::Info, false},  // Trying to connect to server on 127.0.0.1
            {RowType::System, K::Announce, true}, // TeamSpeak's welcome: its green and its link kept
            {RowType::System, K::Join, false},  // Connected to Server: "TeamSpeak ]I[ Server"
            {RowType::System, K::Edit, false},  // Channel "L4 Second" created by "serveradmin"
            {RowType::System, K::Join, false},  {RowType::System, K::Group, false}, // You switched ... / Channel group "Guest" ...
            {RowType::System, K::Join, false},  {RowType::System, K::Group, false},
            {RowType::System, K::Join, false},  {RowType::System, K::Group, false},
            {RowType::System, K::Join, false},  // "Nora" connected to channel "Default Channel"
            {RowType::System, K::Leave, false}, // "Nora" disconnected (leaving)
        };
        const auto check = [&](const char* when) {
            for (int n = 0; n < want.size(); ++n) {
                const layoutdoc::Row row = layoutdoc::read(doc.findBlockByNumber(n));
                QVERIFY2(row.type == want.at(n).type, qPrintable(QStringLiteral("%1: block %2 is type %3").arg(QString::fromLatin1(when)).arg(n).arg(static_cast<int>(row.type))));
                if (row.type != RowType::System)
                    continue;
                QVERIFY2(row.kind == want.at(n).kind, qPrintable(QStringLiteral("%1: block %2 is kind %3").arg(QString::fromLatin1(when)).arg(n).arg(static_cast<int>(row.kind))));
                QVERIFY2(row.keepFormatting == want.at(n).keep, qPrintable(QStringLiteral("%1: block %2 keep %3").arg(QString::fromLatin1(when)).arg(n).arg(row.keepFormatting)));
            }
        };
        check("unstyled");
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(1)).time, QStringLiteral("21:40:46"));
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(0)).date, QDate(2026, 10, 10));
        const Snapshot    plain = snapshot(doc);
        const QString     rows  = rowsOf(doc).join(QLatin1Char('\n'));
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            layoutdoc::Env env = envFor(mode);
            env.collapse       = true;
            QVERIFY(layoutdoc::styleAll(&doc, env) >= 12);
            check(mode == Mode::Cozy ? "cozy" : "compact");
            QCOMPARE(rowsOf(doc).join(QLatin1Char('\n')), rows); // read the same through our styling
            // Every status line is a system row of ours: the kind's icon in front, TeamSpeak's time kept in it.
            for (int n = 1; n < doc.blockCount(); ++n) {
                const layoutformat::Lead lead = layoutformat::leadOf(doc.findBlockByNumber(n));
                QVERIFY2(lead.styled && lead.kind == layoutformat::SystemPrefix, qPrintable(QString::number(n)));
            }
            QCOMPARE(layoutformat::leadOf(doc.findBlockByNumber(0)).kind, layoutformat::Divider);
            // Muted words, names in the name colour; TeamSpeak's welcome keeps its own green.
            QTextCursor c(&doc);
            const int   moved = layoutformat::bodyStart(doc.findBlockByNumber(5));
            c.setPosition(moved + 1);
            c.setPosition(moved + 2, QTextCursor::KeepAnchor);
            QCOMPARE(c.charFormat().foreground().color(), env.colors.muted);
            const int welcome = layoutformat::bodyStart(doc.findBlockByNumber(2));
            c.setPosition(welcome + 1);
            c.setPosition(welcome + 2, QTextCursor::KeepAnchor);
            QCOMPARE(c.charFormat().foreground().color(), QColor(0x0a, 0xa5, 0x37));
            // The run of events folds into its first row; the welcome is never folded (and ends the run before).
            collapseAll(doc, env);
            QVERIFY(doc.findBlockByNumber(1).isVisible() && doc.findBlockByNumber(2).isVisible() && doc.findBlockByNumber(3).isVisible());
            for (int n = 4; n < doc.blockCount(); ++n)
                QVERIFY2(!doc.findBlockByNumber(n).isVisible(), qPrintable(QString::number(n)));
            const QVector<layoutdoc::ChipRef> chips = layoutdoc::chipsOf(doc.findBlockByNumber(3));
            QCOMPARE(chips.size(), 1);
            QCOMPARE(chips.first().text, QStringLiteral("+9 more events"));
            QVERIFY(layoutdoc::chipsOf(doc.findBlockByNumber(1)).isEmpty());
            layoutdoc::restoreAll(&doc);
            QCOMPARE(compare(plain, snapshot(doc)), QString());
        }

        // A private chat: TeamSpeak's own line after the messages.
        QTextDocument chat;
        setup(chat);
        tschat::append(&chat, tschat::messageHtml(s, QStringLiteral("21:50:45"), 179, QStringLiteral("serveradmin"), QStringLiteral("Nora"), QStringLiteral("Private hello from Nora")));
        tschat::append(&chat, tschat::statusHtml(s, QStringLiteral("21:53:16"), QStringLiteral("Chat partner disconnected out of view.")));
        QCOMPARE(layoutdoc::read(chat.findBlockByNumber(0)).type, RowType::Message);
        const layoutdoc::Row partner = layoutdoc::read(chat.findBlockByNumber(1));
        QCOMPARE(partner.type, RowType::System);
        QCOMPARE(partner.kind, K::Leave);
        QCOMPARE(partner.time, QStringLiteral("21:53:16"));

        // A server's own welcome and host message (any words, any language): info rows that keep everything.
        QTextDocument other;
        setup(other);
        tschat::append(&other, tschat::statusHtml(s, QStringLiteral("21:40:47"), QStringLiteral("<span style=\"color:#ff5500\">Willkommen auf dem Server</span> - viel Spass"), QStringLiteral("#0aa537"), true));
        tschat::append(&other, tschat::statusHtml(s, QStringLiteral("21:40:48"), QStringLiteral("Rules: no spam, or you get kicked")));
        for (int n = 0; n < 2; ++n) {
            const layoutdoc::Row row = layoutdoc::read(other.findBlockByNumber(n));
            QCOMPARE(row.type, RowType::System);
            QCOMPARE(row.kind, K::Info);
            QVERIFY(row.keepFormatting);
            QVERIFY(!layoutdoc::collapsible(row));
        }
        const Snapshot otherPlain = snapshot(other);
        layoutdoc::styleAll(&other, envFor(Mode::Cozy));
        QTextCursor oc(&other);
        const int   orange = layoutformat::bodyStart(other.firstBlock());
        oc.setPosition(orange + 1);
        oc.setPosition(orange + 2, QTextCursor::KeepAnchor);
        QCOMPARE(oc.charFormat().foreground().color(), QColor(0xff, 0x55, 0x00));
        layoutdoc::restoreAll(&other);
        QCOMPARE(compare(otherPlain, snapshot(other)), QString());

        // Users' text is never a system row: a picture a user sends with TeamSpeak's icon name (BBCode [img]
        // gives no size), another icon, a message whose words look like a status line.
        QTextDocument hostile;
        setup(hostile);
        tschat::append(&hostile, QStringLiteral("<img src=\"iconpath:MESSAGE_INFO?size=13x13\">&lt;12:00:00&gt; ") + tschat::clientHtml(s, 3, kUidBob, QStringLiteral("Bob"))
                                     + QStringLiteral(" connected to channel ") + tschat::channelHtml(s, QStringLiteral("Home")));
        tschat::append(&hostile, QStringLiteral("<img src=\"iconpath:MESSAGE_INFOX?size=13x13\" width=\"13\" height=\"13\">&lt;12:00:00&gt; Channel group \"Guest\" was assigned"));
        tschat::append(&hostile, tschat::messageHtml(s, QStringLiteral("12:00:01"), 9, kUidAlice, QStringLiteral("Alice"), QStringLiteral("&lt;12:00:00&gt; Channel group \"Admin\" was assigned to you")));
        QCOMPARE(layoutdoc::read(hostile.findBlockByNumber(0)).type, RowType::Other);
        QCOMPARE(layoutdoc::read(hostile.findBlockByNumber(1)).type, RowType::Other);
        QCOMPARE(layoutdoc::read(hostile.findBlockByNumber(2)).type, RowType::Message);
    }

    // A status line's kind comes from the start of TeamSpeak's words; names, channels, quoted groups and
    // nicknames, reasons and poke texts never decide it.
    void statusKindsReadOnlyTeamSpeaksWords()
    {
        using K         = layoutart::SystemKind;
        const auto kind = [](const char* words, bool* keep = nullptr) { return layoutdoc::statusKindOf(QString::fromLatin1(words), keep); };
        QCOMPARE(kind("\"\" connected to channel \"\""), K::Join);
        QCOMPARE(kind("\"\" disconnected (Kicked for an error, poke me)"), K::Leave);
        QCOMPARE(kind("\"\" dropped (connection lost)"), K::Leave);
        QCOMPARE(kind("\"\" was moved from channel \"\" to \"\" by \"\""), K::Join);
        QCOMPARE(kind("\"\" appears, coming from channel \"\""), K::Join);
        QCOMPARE(kind("\"\" left, was moved to channel \"\" by \"\""), K::Leave);
        QCOMPARE(kind("\"\" was kicked from the server by \"\" (bye)"), K::Danger);
        QCOMPARE(kind("\"\" was banned for 5 minutes from the server by \"\" (spam)"), K::Danger);
        QCOMPARE(kind("\"\" is now known as \"Pokemon kicked error\""), K::Edit); // a new nickname is a name
        QCOMPARE(kind("Channel group \"Banned error\" was assigned to \"\" by \"\"."), K::Group);
        QCOMPARE(kind("Channel group \"Guest\" [Inherited from: Lobby] was assigned to \"\" by \"\"."), K::Group);
        QCOMPARE(kind("\"\" was added to server group \"Poke masters\" by \"\"."), K::Group);
        QCOMPARE(kind("Channel \"\" was deleted by \"\""), K::Edit);
        QCOMPARE(kind("Channel \"\" was moved by \"\", new parent channel is \"\""), K::Edit);
        QCOMPARE(kind("\"\" pokes you: you are banned"), K::Poke);
        QCOMPARE(kind("You poked \"\"."), K::Poke);
        QCOMPARE(kind("Chat partner disconnected out of view."), K::Leave);
        QCOMPARE(kind("insufficient client permissions (failed on i_channel_join_power)"), K::Danger);
        bool keep = false;
        QCOMPARE(kind("Welcome to TeamSpeak, check www.teamspeak.com for latest information", &keep), K::Announce);
        QVERIFY(keep);
        keep = true;
        QCOMPARE(kind("Trying to connect to server on 127.0.0.1", &keep), K::Info);
        QVERIFY(!keep);
        // Anything else (a server's welcome, another language): info, TeamSpeak's formatting kept. Known words
        // later in a line never make it one of TeamSpeak's.
        keep = false;
        QCOMPARE(kind("Willkommen! Bitte keine Werbung.", &keep), K::Info);
        QVERIFY(keep);
        keep = false;
        QCOMPARE(kind("Our rules: when \"\" connected to channel \"\" say hi", &keep), K::Info);
        QVERIFY(keep);
    }

    // ---- 8b. history markers and day lines in TeamSpeak's real formats -------------------------------------------
    void historyMarkersAndDayLines()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("08:00:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("before any marker"))); // 0
        tschat::append(&doc, tschat::historyHtml(s, QStringLiteral("2026-10-08 20:37:01")));                                                                 // 1
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("20:38:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("on the 8th")));        // 2
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/9/2026"), false));                                                                        // 3 as in the screenshots
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:00:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("on the 9th")));        // 4
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), true));                                                                        // 5 with the icon
        tschat::append(&doc, tschat::historyHtml(s, QStringLiteral("2026-10-10 04:44:02"), true));                                                           // 6 the server tab's
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:01:00"), 18, kUidBob, QStringLiteral("Bob"), QStringLiteral("today")));                 // 7
        // Timestamps off and a channel named like a date: still a system row (names never count).
        tschat::append(&doc, tschat::systemHtml(s, QString(), QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, QStringLiteral("2026-10-10 04:44:02")))); // 8
        const layoutdoc::Row chatBegins = layoutdoc::read(doc.findBlockByNumber(1));
        QCOMPARE(chatBegins.type, RowType::History);
        QCOMPARE(chatBegins.date, QDate(2026, 10, 8));
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(3)).type, RowType::Day);
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(3)).date, QDate(2026, 10, 9));
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(5)).type, RowType::Day);
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(5)).date, QDate(2026, 10, 10));
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(6)).type, RowType::History);
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(6)).date, QDate(2026, 10, 10));
        QCOMPARE(layoutdoc::read(doc.findBlockByNumber(8)).type, RowType::System);
        // The day of a line: the nearest day line or history marker above; none above: unknown.
        const layoutdoc::Env env   = envFor(Mode::Cozy);
        const QVector<QDate> dates = layoutdoc::datesOf(&doc, env.today);
        QVERIFY(!dates.value(0).isValid());
        QCOMPARE(dates.value(2), QDate(2026, 10, 8));
        QCOMPARE(dates.value(4), QDate(2026, 10, 9));
        QCOMPARE(dates.value(7), QDate(2026, 10, 10));
        const Snapshot plain = snapshot(doc);
        layoutdoc::styleAll(&doc, env);
        QCOMPARE(countKind(doc, layoutformat::Divider), 4);
        const QVector<QDate> styledDates = layoutdoc::datesOf(&doc, env.today);
        const auto           sig         = [&](int n) { return layoutdoc::signature(env, doc.findBlockByNumber(n), styledDates.value(n), layoutdoc::Look()); };
        QVERIFY2(sig(1).contains(QStringLiteral("History · October 8, 2026")), qPrintable(sig(1)));
        QVERIFY2(sig(3).contains(QStringLiteral("Yesterday, October 9, 2026")), qPrintable(sig(3)));
        QVERIFY2(sig(5).contains(QStringLiteral("Today, October 10, 2026")), qPrintable(sig(5)));
        QVERIFY2(sig(6).contains(QStringLiteral("History · October 10, 2026")), qPrintable(sig(6)));
        QVERIFY2(!sig(0).contains(QStringLiteral("Today")) && sig(0).contains(QStringLiteral("08:00")), qPrintable(sig(0)));
        QVERIFY2(!sig(2).contains(QStringLiteral("Today")) && sig(2).contains(QStringLiteral("20:38")), qPrintable(sig(2)));
        QVERIFY2(sig(7).contains(QStringLiteral("Today at 09:01")), qPrintable(sig(7)));
        layoutdoc::restoreAll(&doc);
        QCOMPARE(compare(plain, snapshot(doc)), QString());
    }

    // Lines that just arrived ("Today at", grouping without timestamps) against history being loaded.
    void linesThatJustArrived()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        for (int i = 0; i < 6; ++i)
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("09:00:0%1").arg(i), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("line %1").arg(i)));
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("08:52:01"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("from the log"))); // 6
        tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("23:59:50"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("before midnight"))); // 7
        tschat::append(&doc, tschat::messageHtml(s, QString(), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("no time"))); // 8
        const auto arrived = [&](std::initializer_list<int> blocks, int now) {
            QVector<QTextBlock> lines;
            for (const int n : blocks)
                lines.append(doc.findBlockByNumber(n));
            QStringList out;
            for (const int n : layoutdoc::arrivedNow(lines, now))
                out << QString::number(n);
            return out.join(QLatin1Char(','));
        };
        const int now = 9 * 3600 + 30; // 09:00:30
        QCOMPARE(arrived({4, 5, 5}, now), QStringLiteral("4,5"));       // one line (TeamSpeak's append touches the one before)
        QCOMPARE(arrived({3, 4, 4, 5}, now), QStringLiteral("3,4,5"));  // two in one turn
        QCOMPARE(arrived({0, 1, 1, 2, 2, 3, 3, 4}, now), QString());     // history loaded in one go
        QCOMPARE(arrived({5, 6}, now), QStringLiteral("5"));            // a line from the log, refilled one by one after a reconnect
        QCOMPARE(arrived({7}, 10), QStringLiteral("7"));                // 23:59:50 at 00:00:10: just now
        QCOMPARE(arrived({8}, now), QStringLiteral("8"));               // no time: the turn decides
    }

    // ---- 14. the reader's place across the layout's edits ----------------------------------------------------------
    void anchorSurvivesFoldedRows()
    {
        QTextDocument doc;
        fillWithRun(doc, 30, 8, 6);
        QAbstractTextDocumentLayout* layout = doc.documentLayout();
        layout->documentSize();
        const int                head  = 30; // the run's first row
        const int                third = 32;
        const qreal              y     = layout->blockBoundingRect(doc.findBlockByNumber(third)).top() + 3;
        const layoutdoc::Anchor  a     = layoutdoc::anchorAt(&doc, y);
        QCOMPARE(a.block, third);
        // A message read halfway: halfway into it again, however tall the new look makes it.
        const QRectF            ten   = layout->blockBoundingRect(doc.findBlockByNumber(10));
        const layoutdoc::Anchor m     = layoutdoc::anchorAt(&doc, ten.top() + ten.height() / 2);
        QCOMPARE(m.block, 10);
        layoutdoc::Env env = envFor(Mode::Cozy);
        env.collapse       = true;
        {
            QTextCursor batch(&doc); // one step of ChatLayout: styled and folded in one edit
            batch.beginEditBlock();
            layoutdoc::styleAll(&doc, env);
            collapseAll(doc, env);
            batch.endEditBlock();
        }
        layout->documentSize();
        QVERIFY(!doc.findBlockByNumber(third).isVisible());
        QVERIFY(doc.findBlockByNumber(head).isVisible());
        // Qt gives a hidden block no place (read as it is, the chat would go to its top): the run's row instead.
        QCOMPARE(layout->blockBoundingRect(doc.findBlockByNumber(third)).top(), 0.0);
        const qreal headTop = layout->blockBoundingRect(doc.findBlockByNumber(head)).top();
        QVERIFY(headTop > 100);
        QCOMPARE(layoutdoc::anchorY(&doc, a), headTop);
        const QRectF styledTen = layout->blockBoundingRect(doc.findBlockByNumber(10));
        QVERIFY2(qAbs(layoutdoc::anchorY(&doc, m) - (styledTen.top() + styledTen.height() / 2)) < 0.5,
                 qPrintable(QStringLiteral("%1 vs %2").arg(layoutdoc::anchorY(&doc, m)).arg(styledTen.top() + styledTen.height() / 2)));
    }

    // The live test: a layout switch (or grouping toggled) slightly above the bottom jumped to the chat's top.
    void layoutSwitchNearTheBottomKeepsThePlace()
    {
        QTextBrowser browser;
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(560, 220);
        QTextDocument* doc = browser.document();
        fillWithRun(*doc, 30, 8, 12);
        browser.show();
        QCoreApplication::processEvents();
        QScrollBar* bar = browser.verticalScrollBar();
        chatscroll::syncRange(&browser);
        QAbstractTextDocumentLayout* layout = doc->documentLayout();
        const int                    head   = 30;
        const int                    third  = 32;
        const int                    top    = qRound(layout->blockBoundingRect(doc->findBlockByNumber(third)).top()) + 2;
        // Slightly above the bottom (the chat's end on screen), as in the live test.
        QVERIFY2(top < bar->maximum() - 4 && bar->maximum() - top < browser.viewport()->height(), qPrintable(QStringLiteral("%1 of %2").arg(top).arg(bar->maximum())));
        bar->setValue(top);
        const chatscroll::Place place = chatscroll::capture(&browser);
        QVERIFY(!place.bottom);
        QCOMPARE(place.anchor.block, third);
        // TeamSpeak classic to Cozy as one step of ChatLayout: styled and folded in one edit, the place kept.
        layoutdoc::Env env = envFor(Mode::Cozy, true, 500);
        env.collapse       = true;
        {
            QTextCursor batch(doc);
            batch.beginEditBlock();
            layoutdoc::styleAll(doc, env);
            collapseAll(*doc, env);
            batch.endEditBlock();
        }
        chatscroll::restore(&browser, place);
        QVERIFY(!doc->findBlockByNumber(third).isVisible());
        const int headTop = qRound(layout->blockBoundingRect(doc->findBlockByNumber(head)).top());
        QVERIFY2(bar->value() == qMin(headTop, bar->maximum()) && bar->value() > bar->maximum() / 2,
                 qPrintable(QStringLiteral("at %1 of %2, the run at %3").arg(bar->value()).arg(bar->maximum()).arg(headTop)));
        // The range is the new document's (a value set before QTextEdit hears of it would be clamped).
        QCOMPARE(bar->maximum(), qMax(0, qRound(layout->documentSize().height()) - browser.viewport()->height()));
        // And back to TeamSpeak classic: the row on top stays on top.
        const chatscroll::Place back = chatscroll::capture(&browser);
        layoutdoc::restoreAll(doc);
        chatscroll::restore(&browser, back);
        const int backTop = qRound(layout->blockBoundingRect(doc->findBlockByNumber(back.anchor.block)).top() + back.anchor.offset);
        QVERIFY2(qAbs(bar->value() - qMin(backTop, bar->maximum())) <= 1 && bar->value() > bar->maximum() / 2,
                 qPrintable(QStringLiteral("at %1 of %2, the row at %3").arg(bar->value()).arg(bar->maximum()).arg(backTop)));
        // At the bottom: at the bottom again after a switch.
        bar->setValue(bar->maximum());
        const chatscroll::Place bottom = chatscroll::capture(&browser);
        QVERIFY(bottom.bottom);
        layoutdoc::Env compact = envFor(Mode::Compact, true, 500);
        compact.collapse       = true;
        {
            QTextCursor batch(doc);
            batch.beginEditBlock();
            layoutdoc::styleAll(doc, compact);
            collapseAll(*doc, compact);
            batch.endEditBlock();
        }
        chatscroll::restore(&browser, bottom);
        QCOMPARE(bar->value(), bar->maximum());
        layoutdoc::restoreAll(doc);
    }

    // The live test: a run opened at the bottom left the chat above it, and new lines came in out of view.
    void openingARunAtTheBottomFollowsIt()
    {
        for (const bool atEnd : {true, false}) {
            QTextBrowser browser;
            browser.setAttribute(Qt::WA_DontShowOnScreen);
            browser.resize(560, 220);
            QTextDocument* doc = browser.document();
            fillWithRun(*doc, 20, 8, atEnd ? 0 : 20);
            browser.show();
            QCoreApplication::processEvents();
            layoutdoc::Env env = envFor(Mode::Cozy, true, 500);
            env.collapse       = true;
            {
                QTextCursor batch(doc);
                batch.beginEditBlock();
                layoutdoc::styleAll(doc, env);
                collapseAll(*doc, env);
                batch.endEditBlock();
            }
            QScrollBar* bar = browser.verticalScrollBar();
            chatscroll::syncRange(&browser);
            const int head = 20;
            const int tag  = doc->findBlockByNumber(head).blockFormat().intProperty(layoutformat::kBlockTag);
            QVERIFY(tag > 0);
            QVERIFY(!doc->findBlockByNumber(head + 1).isVisible());
            // As ChatLayout::toggleRun does it.
            const auto toggle = [&](const QSet<int>& expanded) {
                const chatscroll::RowPlace place = chatscroll::captureRow(&browser, head);
                {
                    QTextCursor batch(doc);
                    batch.beginEditBlock();
                    int serial = 5000;
                    for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(doc, head, head, expanded))
                        layoutdoc::applyRun(doc, run, env, &serial);
                    batch.endEditBlock();
                }
                chatscroll::restoreRow(&browser, place);
            };
            QAbstractTextDocumentLayout* layout = doc->documentLayout();
            if (atEnd) {
                bar->setValue(bar->maximum());
                const int closedMax = bar->maximum();
                toggle({tag});
                QVERIFY(doc->findBlockByNumber(head + 7).isVisible());
                QVERIFY(bar->maximum() > closedMax);
                QCOMPARE(bar->value(), bar->maximum()); // still following the bottom
                // A new line: QTextEdit::append keeps a chat at the bottom there.
                browser.append(tschat::messageHtml(tschat::darkSkin(), QStringLiteral("21:30:00"), 18, kUidBob, QStringLiteral("Bob"), QStringLiteral("a new line")));
                QCOMPARE(bar->value(), bar->maximum());
            } else {
                // Not at the bottom: the clicked row stays where it is on screen, opened and closed.
                bar->setValue(qRound(layout->blockBoundingRect(doc->findBlockByNumber(head)).top()) - 40);
                const int before = qRound(layout->blockBoundingRect(doc->findBlockByNumber(head)).top()) - bar->value();
                QVERIFY(bar->value() < bar->maximum() - 4);
                for (const QSet<int>& expanded : {QSet<int>{tag}, QSet<int>()}) {
                    toggle(expanded);
                    QCOMPARE(doc->findBlockByNumber(head + 1).isVisible(), !expanded.isEmpty());
                    const int now = qRound(layout->blockBoundingRect(doc->findBlockByNumber(head)).top()) - bar->value();
                    QVERIFY2(qAbs(now - before) <= 1, qPrintable(QStringLiteral("%1 vs %2").arg(now).arg(before)));
                }
            }
        }
    }

    // ---- 13. unload leaves nothing of ours ------------------------------------------------------------------------------
    void unloadLeavesNothing()
    {
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            QTextDocument doc;
            fillChat(doc);
            const Snapshot plain = snapshot(doc);
            layoutdoc::Env env   = envFor(mode);
            layoutdoc::styleAll(&doc, env);
            layoutdoc::renderAll(&doc, env, env.today);
            QStringList names;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
                const layoutformat::Lead lead = layoutformat::leadOf(b);
                if (lead.styled)
                    names << lead.format.toImageFormat().name();
            }
            QVERIFY(names.size() > 10);
            QVERIFY(doc.resource(QTextDocument::ImageResource, QUrl(names.first())).isValid());
            QVERIFY(layoutdoc::giveBack(&doc) > 0);
            for (const QString& name : qAsConst(names))
                QVERIFY2(!doc.resource(QTextDocument::ImageResource, QUrl(name)).isValid() || doc.resource(QTextDocument::ImageResource, QUrl(name)).isNull(), qPrintable(name));
            QCOMPARE(compare(plain, snapshot(doc)), QString());
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
                QVERIFY(!layoutdoc::hasOurs(b));
        }
    }

    // ---- 11. performance -------------------------------------------------------------------------------------------
    void fourThousandBlocks()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        QString html;
        for (int i = 0; i < 4000; ++i) {
            const int     m    = i % 60;
            const QString time = QStringLiteral("%1:%2:%3").arg(10 + i / 600, 2, 10, QLatin1Char('0')).arg((i / 10) % 60, 2, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0'));
            if (i % 9 == 0)
                tschat::append(&doc, tschat::systemHtml(s, time, QStringLiteral("Bob connected")));
            else
                tschat::append(&doc, tschat::messageHtml(s, time, 17, i % 3 ? kUidAlice : kUidBob, i % 3 ? QStringLiteral("Alice") : QStringLiteral("Bob"),
                                                         tschat::linkHtml(s, QStringLiteral("https://example.com/%1").arg(i))));
        }
        doc.documentLayout()->documentSize();
        const Snapshot   before = snapshot(doc);
        QElapsedTimer    clock;
        clock.start();
        const int done = layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
        doc.documentLayout()->documentSize();
        const qint64 styleMs = clock.elapsed();
        QCOMPARE(done, 4000);
        clock.restart();
        layoutdoc::restoreAll(&doc);
        doc.documentLayout()->documentSize();
        const qint64 restoreMs = clock.elapsed();
        qInfo("4000 blocks: styled in %lld ms, restored in %lld ms", styleMs, restoreMs);
        QVERIFY2(styleMs < 3000, qPrintable(QString::number(styleMs)));
        QCOMPARE(compare(before, snapshot(doc)), QString());
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy));
        // A new line on a styled chat of 4000: what ChatLayout does for it (sanitize, read, plan, apply, its day).
        {
            QVector<QDate> dates = layoutdoc::datesOf(&doc, QDate(2026, 10, 10));
            tschat::append(&doc, tschat::messageHtml(s, QStringLiteral("23:59:00"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("a new one")));
            const layoutdoc::Env cozy = envFor(Mode::Cozy);
            QElapsedTimer        one;
            one.start();
            const int   n = doc.blockCount() - 1;
            QTextCursor batch(&doc);
            batch.beginEditBlock(); // as ChatLayout's step: one relayout
            layoutdoc::extendDates(&doc, QDate(2026, 10, 10), &dates, n);
            layoutdoc::sanitize(&doc, n);
            const QTextBlock      b    = doc.findBlockByNumber(n);
            const layoutdoc::Row  row  = layoutdoc::read(b);
            const layoutdoc::Plan plan = layoutdoc::plan(cozy, b, row, layoutdoc::previousOf(b), dates.value(n), 0, 99999);
            const bool            styled = layoutdoc::apply(&doc, n, plan, cozy);
            batch.endEditBlock();
            doc.documentLayout()->documentSize();
            const qint64 us = one.nsecsElapsed() / 1000;
            QVERIFY(styled);
            qInfo("a new line: %lld us", us);
            QVERIFY2(us < 1500, qPrintable(QString::number(us))); // about 0.4 ms here, layout included
        }
    }
};

TSMEDIA_REGISTER_TEST(TestChatLayout)

#include "tst_chatlayout.moc"
