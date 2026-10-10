// Unit tests for 2.2 replies: the quote line format and its parser (round trips, hostile input, giant
// emoji runs), snippets, matching a reply to its original, TeamSpeak-like chat documents (reading,
// turning quote lines into reply lines and back, the line height, 1000 of them in one edit and reading
// only the edited tail again), the reply line's drawing sizes and colours, and the quote line in front of
// a caption when files are sent as a reply.

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QtTest>

#include "albums.h"
#include "medialink.h"
#include "replies.h"
#include "replyart.h"
#include "replydoc.h"
#include "testmain.h"
#include "uiutil.h"

namespace {

const QString kUidAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kUidBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");

QString arrow()
{
    return replies::arrow();
}

replies::Original alice(const QString& text = QStringLiteral("Hello world"), int minutes = 21 * 60 + 14)
{
    replies::Original o;
    o.nick     = QStringLiteral("Alice");
    o.uid      = kUidAlice;
    o.clientId = 17;
    o.minutes  = minutes;
    o.text     = text;
    return o;
}

// One chat message the way TeamSpeak 3.6 shows it (S0 h): icon, time, a space, the quoted nickname
// linked to client://<clid>/<uid>~<nick>, ": " and the message (BBCode already turned into HTML).
QString messageHtml(const QString& time, int clid, const QString& uid, const QString& nick, const QString& bodyHtml, bool outgoing = false)
{
    return QStringLiteral("<p><img src=\"iconpath:MESSAGE_%1?size=13x13\" width=\"13\" height=\"13\">"
                          "<span style=\"color:#607d8b\">&lt;%2&gt;</span> "
                          "<a href=\"client://%3/%4~%5\" style=\"color:#1cb0f4;font-weight:bold;text-decoration:none\">\"%5\"</a>: %6</p>")
        .arg(outgoing ? QStringLiteral("OUTGOING") : QStringLiteral("INCOMING"), time)
        .arg(clid)
        .arg(uid.toHtmlEscaped(), nick.toHtmlEscaped(), bodyHtml);
}

// What TeamSpeak makes of a quote line's BBCode: [i] -> <i>, [URL=x]y[/URL] -> a link, '\n' -> <br>.
QString quoteHtml(const QString& nick, int clid, const QString& uid, const QString& time, const QString& snippet)
{
    QString html = QStringLiteral("<i>") + arrow() + QStringLiteral(" <a href=\"client://%1/%2~%3\">%3</a>").arg(clid).arg(uid.toHtmlEscaped(), nick.toHtmlEscaped());
    if (!time.isEmpty())
        html += QStringLiteral(" · ") + time;
    if (!snippet.isEmpty())
        html += QStringLiteral(": “") + snippet.toHtmlEscaped() + QStringLiteral("”");
    return html + QStringLiteral("</i>");
}

QString statusHtml()
{
    return QStringLiteral("<p><span style=\"color:#607d8b\">&lt;21:00:00&gt;</span> <a href=\"client://17/%1~Alice\">Alice</a> connected</p>").arg(kUidAlice);
}

// A document like a TeamSpeak chat: Alice says hello, a status line, Bob replies to her.
void fillChat(QTextDocument& doc)
{
    QString html = messageHtml(QStringLiteral("21:14:02"), 17, kUidAlice, QStringLiteral("Alice"), QStringLiteral("Hello world"));
    html += statusHtml();
    html += messageHtml(QStringLiteral("21:15:30"), 18, kUidBob, QStringLiteral("Bob"),
                        quoteHtml(QStringLiteral("Alice"), 17, kUidAlice, QStringLiteral("21∶14"), QStringLiteral("Hello world")) + QStringLiteral("<br>my reply"));
    doc.setHtml(html);
    doc.setTextWidth(600);
    doc.documentLayout()->documentSize();
}

QVector<QPair<QString, QString>> fragmentsOf(const QTextBlock& block)
{
    QVector<QPair<QString, QString>> out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid())
            out.append({f.text(), f.charFormat().anchorHref()});
    }
    return out;
}

MediaLink photo(const QString& fileName = QStringLiteral("alpine-lake_3f9a1c2e.jpg"))
{
    MediaLink link;
    link.host        = QStringLiteral("ts.example.com");
    link.port        = 9987;
    link.serverUid   = QStringLiteral("Wn5SbAbc+/9xQ0pRu7Zy3pCt+Ys=");
    link.channelId   = 42;
    link.path        = QStringLiteral("/tsmedia");
    link.fileName    = fileName;
    link.size        = 2458123;
    link.dateTime    = 1760000000;
    link.protocol    = MediaLink::kProtocol;
    link.width       = 4032;
    link.height      = 3024;
    return link;
}

} // namespace

class TestReplies : public QObject
{
    Q_OBJECT

  private slots:
    // ---- the quote line ----------------------------------------------------------------------------
    void quoteLineFormat()
    {
        const QString line = replies::quoteLine(alice());
        QCOMPARE(line, QStringLiteral("[i]") + arrow() + QStringLiteral(" [URL=client://17/") + kUidAlice + QStringLiteral("~Alice]Alice[/URL] · 21∶14: “Hello world”[/i]"));
        // No time (timestamps off), no client id (not on the server), no uid (no link at all).
        replies::Original o = alice();
        o.minutes           = -1;
        o.clientId          = 0;
        QCOMPARE(replies::quoteLine(o), QStringLiteral("[i]") + arrow() + QStringLiteral(" [URL=client://0/") + kUidAlice + QStringLiteral("~Alice]Alice[/URL]: “Hello world”[/i]"));
        o.uid = QString();
        QCOMPARE(replies::quoteLine(o), QStringLiteral("[i]") + arrow() + QStringLiteral(" Alice: “Hello world”[/i]"));
        // A snippet left out.
        QCOMPARE(replies::quoteLine(alice(), 0), QStringLiteral("[i]") + arrow() + QStringLiteral(" [URL=client://17/") + kUidAlice + QStringLiteral("~Alice]Alice[/URL] · 21∶14[/i]"));
    }

    void timeNeverMakesEmoticons()
    {
        // ":0" (21:05) and "8)" are TeamSpeak emoticons: the time has neither.
        for (int minutes : {21 * 60 + 5, 21 * 60 + 8, 8 * 60 + 18, 0, 23 * 60 + 59}) {
            const QString line = replies::quoteLine(alice(QStringLiteral("x"), minutes));
            QVERIFY2(!line.contains(QLatin1String(":0")) && !line.contains(QLatin1String("8)")), qPrintable(line));
        }
        QCOMPARE(replies::formatTime(21 * 60 + 5), QStringLiteral("21∶05"));
        QCOMPARE(replies::formatTime(-1), QString());
    }

    void roundTrip_data()
    {
        QTest::addColumn<QString>("nick");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("minutes");
        QTest::newRow("plain") << QStringLiteral("Alice") << QStringLiteral("Hello world") << 21 * 60 + 14;
        QTest::newRow("persian") << QStringLiteral("\u0645\u0647\u062f\u06cc") << QStringLiteral("\u0633\u0644\u0627\u0645\u060c \u062e\u0648\u0628\u06cc\u061f \u0627\u06cc\u0646 \u06cc\u06a9 \u067e\u06cc\u0627\u0645 \u0627\u0633\u062a") << 9 * 60 + 3;
        QTest::newRow("brackets") << QStringLiteral("[Admin] Bob]") << QStringLiteral("see [b]this[/b] and [URL=ts3file://x]y[/URL]") << 12 * 60;
        QTest::newRow("backslash") << QStringLiteral("back\\") << QStringLiteral("a\\[b] c\\") << 1;
        QTest::newRow("quotes") << QStringLiteral("\"Q\"") << QStringLiteral("he said “no” and left") << 600;
        QTest::newRow("tilde") << QStringLiteral("a~b~c") << QStringLiteral("~~~") << 30;
        QTest::newRow("emoji") << QStringLiteral("Ali 😀") << QStringLiteral("😀😀 nice") << 75;
        QTest::newRow("no time") << QStringLiteral("Alice") << QStringLiteral("x") << -1;
    }

    void roundTrip()
    {
        QFETCH(QString, nick);
        QFETCH(QString, text);
        QFETCH(int, minutes);
        replies::Original o = alice(text, minutes);
        o.nick              = nick;
        const QString line  = replies::quoteLine(o);
        const replies::Quote q = replies::parseQuoteBBCode(line);
        QVERIFY2(q.valid(), qPrintable(line));
        QString expectedNick = nick.trimmed();
        while (expectedNick.endsWith(QLatin1Char('\\')))
            expectedNick.chop(1); // a backslash right before "[/URL]" is left out
        QCOMPARE(q.nick, expectedNick);
        QCOMPARE(q.uid, kUidAlice);
        QCOMPARE(q.clientId, 17);
        QCOMPARE(q.minutes, minutes);
        QVERIFY(!q.media);
        // The snippet names the original (the same matching the chat does).
        QVERIFY2(replies::snippetMatches(q, replies::matchKey(text)), qPrintable(q.snippet));
        // TeamSpeak's own link: the nickname at its end, percent-encoded (it can hold "]" or "~").
        QVERIFY(line.contains(QStringLiteral("[URL=client://17/") + kUidAlice + QLatin1Char('~')));
        // Nothing in a quote line is ever a file link.
        QVERIFY(MediaLink::findInMessage(line).isEmpty());
    }

    void hostileSnippets()
    {
        // A file link in the original's text stays text in the quote line.
        const QString evil = QStringLiteral("[URL=ts3file://127.0.0.1?port=9987&channel=1&path=%2F&name=x.exe&size=1]free.exe[/URL]");
        const QString line = replies::quoteLine(alice(evil));
        QVERIFY(MediaLink::findInMessage(line).isEmpty());
        QVERIFY(!line.contains(QLatin1String("[URL=ts3file")));
        // Web addresses become "(link)"; line breaks, controls and bidi marks go.
        QCOMPARE(replies::makeSnippet(QStringLiteral("look https://example.com/x?y=1 here")), QStringLiteral("look (link) here"));
        QCOMPARE(replies::makeSnippet(QStringLiteral("www.example.com")), QStringLiteral("(link)"));
        QCOMPARE(replies::makeSnippet(QStringLiteral("a\nb c\td")), QStringLiteral("a b c d"));
        const QString rlo = QString(QChar(0x202E)); // RIGHT-TO-LEFT OVERRIDE (not in the source: C5255)
        QCOMPARE(replies::makeSnippet(QStringLiteral("x") + rlo + QStringLiteral("gnp.exe​⁠￼y")), QStringLiteral("xgnp.exey"));
        // Long text is cut near 60 characters, at a space, with an ellipsis.
        const QString cut = replies::makeSnippet(QStringLiteral("one two three four five six seven eight nine ten eleven twelve thirteen"));
        QVERIFY(cut.endsWith(QChar(0x2026)));
        QVERIFY(cut.size() <= replies::kSnippetChars + 1);
        QVERIFY(!cut.contains(QLatin1String(" …")));
        // Never half of a surrogate pair.
        const QString emoji = replies::makeSnippet(QString(70, QLatin1Char('a')).left(59) + QStringLiteral("😀😀😀"), 60);
        QVERIFY(!emoji.at(emoji.size() - 2).isHighSurrogate());
        // A media message quotes its file.
        replies::Original file = alice(QString());
        file.mediaLabel        = QStringLiteral("alpine-lake.jpg");
        const replies::Quote q = replies::parseQuoteBBCode(replies::quoteLine(file));
        QVERIFY(q.valid());
        QVERIFY(q.media);
        QCOMPARE(q.snippet, QStringLiteral("alpine-lake.jpg"));
        // A uid that isn't one: no link at all, just the name.
        replies::Original fake = alice();
        fake.uid               = QStringLiteral("abc]def[URL=x]");
        QVERIFY(!replies::quoteLine(fake).contains(QLatin1String("[URL")));
    }

    void notQuoteLines_data()
    {
        QTest::addColumn<QString>("line");
        QTest::newRow("plain") << QStringLiteral("hello there");
        QTest::newRow("arrow talk") << arrow() + QStringLiteral(" see above, it's great");
        QTest::newRow("arrow only") << arrow();
        QTest::newRow("bad time") << arrow() + QStringLiteral(" Alice · 25∶99: “x”");
        QTest::newRow("foreign link") << QStringLiteral("[i]") + arrow() + QStringLiteral(" [URL=https://evil.example]Alice[/URL]: “x”[/i]");
        QTest::newRow("file link") << QStringLiteral("[i]") + arrow() + QStringLiteral(" [URL=client://1/") + kUidAlice
                                          + QStringLiteral("~A]A[/URL]: “[URL=ts3file://h?name=a]a[/URL]”[/i]");
        QTest::newRow("too long") << arrow() + QStringLiteral(" Alice: “") + QString(700, QLatin1Char('x')) + QStringLiteral("”");
        QTest::newRow("long nick") << arrow() + QStringLiteral(" ") + QString(80, QLatin1Char('n')) + QStringLiteral(": “x”");
        QTest::newRow("trailing junk") << arrow() + QStringLiteral(" Alice: “x” and more");
    }

    void notQuoteLines()
    {
        QFETCH(QString, line);
        QVERIFY2(!replies::parseQuoteBBCode(line).valid(), qPrintable(line));
    }

    void plainQuoteWithoutLink()
    {
        // TeamSpeak might show the client link as plain text: the name is read from the text.
        const replies::Quote q = replies::parseQuoteBBCode(arrow() + QStringLiteral(" Alice · 21:14: “Hello”"));
        QVERIFY(q.valid());
        QCOMPARE(q.nick, QStringLiteral("Alice"));
        QVERIFY(q.uid.isEmpty());
        QCOMPARE(q.minutes, 21 * 60 + 14);
        QCOMPARE(q.snippet, QStringLiteral("Hello"));
    }

    void composeWithinLimit()
    {
        const QString reply = replies::composeReply(alice(), QStringLiteral("line one\r\nline two\n\n"), kMaxMessageBytes);
        QVERIFY(reply.endsWith(QStringLiteral("\nline one\nline two")));
        QVERIFY(reply.startsWith(replies::quoteLine(alice())));
        QVERIFY(replies::composeReply(alice(), QStringLiteral("   \n "), kMaxMessageBytes).isEmpty());
        // Near the limit the snippet shrinks, then goes; past it nothing is sent.
        const QString full    = replies::quoteLine(alice(QString(200, QLatin1Char('w'))));
        const int     room    = kMaxMessageBytes - escapedMessageSize(full) - 1;
        const QString longest = QString(room + 30, QLatin1Char('x'));
        const QString shrunk  = replies::composeReply(alice(QString(200, QLatin1Char('w'))), longest, kMaxMessageBytes);
        QVERIFY(!shrunk.isEmpty());
        QVERIFY(escapedMessageSize(shrunk) <= kMaxMessageBytes);
        QVERIFY(replies::parseQuoteBBCode(shrunk.section(QLatin1Char('\n'), 0, 0)).valid());
        QVERIFY(replies::composeReply(alice(), QString(kMaxMessageBytes, QLatin1Char('x')), kMaxMessageBytes).isEmpty());
        // Escaped size: spaces count double.
        QVERIFY(replies::composeReply(alice(), QString(kMaxMessageBytes / 2, QLatin1Char(' ')) + QLatin1Char('x'), kMaxMessageBytes).isEmpty());
    }

    // ---- matching ---------------------------------------------------------------------------------------
    void normalizing()
    {
        QCOMPARE(replies::normalized(QStringLiteral("Hello, World! :) 12")), QStringLiteral("helloworld12"));
        QCOMPARE(replies::normalized(QStringLiteral("\u0633\u0644\u0627\u0645\u060c \u062e\u0648\u0628\u06cc\u061f")), QStringLiteral("\u0633\u0644\u0627\u0645\u062e\u0648\u0628\u06cc"));
        replies::Quote q;
        q.nick    = QStringLiteral("A");
        q.snippet = QStringLiteral("Hello, World");
        QVERIFY(replies::snippetMatches(q, replies::matchKey(QStringLiteral("hello world"))));
        QVERIFY(!replies::snippetMatches(q, replies::matchKey(QStringLiteral("hello world and more"))));
        q.snippet = QStringLiteral("Hello, World…");
        QVERIFY(replies::snippetMatches(q, replies::matchKey(QStringLiteral("hello world and more"))));
        QVERIFY(!replies::snippetMatches(q, replies::matchKey(QStringLiteral("goodbye"))));
        // A URL in the original matches the "(link)" of the quote.
        q.snippet = replies::makeSnippet(QStringLiteral("see https://example.com/a now"));
        QVERIFY(replies::snippetMatches(q, replies::matchKey(QStringLiteral("see https://example.com/a now"))));
        q.snippet = QString();
        QVERIFY(replies::snippetMatches(q, replies::matchKey(QStringLiteral("anything"))));
    }

    void timeScores()
    {
        QCOMPARE(replies::timeScore(21 * 60 + 14, 21 * 60 + 14), 3);
        QCOMPARE(replies::timeScore(21 * 60 + 14, 21 * 60 + 13), 3);
        QCOMPARE(replies::timeScore(0, 23 * 60 + 59), 3);
        QCOMPARE(replies::timeScore(21 * 60 + 14, 18 * 60 + 44), 2); // 2:30 apart: another time zone
        QCOMPARE(replies::timeScore(21 * 60 + 14, 17 * 60 + 29), 2); // 3:45 (Nepal) and a minute
        QCOMPARE(replies::timeScore(21 * 60 + 14, 20 * 60 + 37), 0);
        QCOMPARE(replies::timeScore(-1, 600), 1);
        QCOMPARE(replies::timeScore(600, -1), 1);
    }

    void findingOriginals()
    {
        auto candidate = [](const QString& uid, const QString& nick, int minutes, const QString& text) {
            replies::Candidate c;
            c.uid     = uid;
            c.nick    = nick;
            c.minutes = minutes;
            c.source  = text;
            return c;
        };
        QVector<replies::Candidate> all;
        all << candidate(kUidAlice, QStringLiteral("Alice"), 600, QStringLiteral("lol"))                   // 0
            << candidate(kUidBob, QStringLiteral("Bob"), 601, QStringLiteral("lol"))                       // 1
            << candidate(kUidAlice, QStringLiteral("Alice"), 610, QStringLiteral("lol"))                   // 2
            << candidate(kUidAlice, QStringLiteral("Alice"), 611, replies::clip() + QStringLiteral(" lol")) // 3: a file named lol
            << candidate(QString(), QStringLiteral("Carol"), 612, QStringLiteral("hi there"))              // 4
            << candidate(kUidAlice, QStringLiteral("Alice"), 620, QStringLiteral("reply"));                // 5: the reply itself
        replies::Quote q;
        q.nick    = QStringLiteral("Alice");
        q.uid     = kUidAlice;
        q.snippet = QStringLiteral("lol");
        q.minutes = 600;
        QCOMPARE(replies::findOriginal(q, all, 5), 0); // the time picks the older one
        q.minutes = -1;
        QCOMPARE(replies::findOriginal(q, all, 5), 2); // no time: the newest
        q.minutes = 610 + 150;
        QCOMPARE(replies::findOriginal(q, all, 5), 2); // another time zone
        q.media   = true;
        QCOMPARE(replies::findOriginal(q, all, 5), 3); // the file, not the text
        q.media   = false;
        q.uid     = kUidBob; // the uid decides, not the nickname
        QCOMPARE(replies::findOriginal(q, all, 5), 1);
        q.uid     = QStringLiteral("someone else");
        QCOMPARE(replies::findOriginal(q, all, 5), -1);
        replies::Quote carol;
        carol.nick    = QStringLiteral("Carol");
        carol.uid     = kUidBob; // the candidate has no uid: the nickname decides
        carol.snippet = QStringLiteral("hi there");
        QCOMPARE(replies::findOriginal(carol, all, 5), 4);
        QCOMPARE(replies::findOriginal(carol, all, 4), -1); // only messages before the reply
        // Bounded: an original further back than kMaxSearchBack messages isn't looked for.
        QVector<replies::Candidate> many;
        many << candidate(kUidAlice, QStringLiteral("Alice"), 1, QStringLiteral("needle"));
        for (int i = 0; i < replies::kMaxSearchBack + 5; ++i)
            many << candidate(kUidBob, QStringLiteral("Bob"), 2, QStringLiteral("hay"));
        replies::Quote needle;
        needle.nick    = QStringLiteral("Alice");
        needle.uid     = kUidAlice;
        needle.snippet = QStringLiteral("needle");
        QCOMPARE(replies::findOriginal(needle, many, many.size()), -1);
        QCOMPARE(replies::findOriginal(needle, many, 10), 0);
    }

    // ---- TeamSpeak-like chat documents ---------------------------------------------------------------------
    void readingMessages()
    {
        QTextDocument doc;
        fillChat(doc);
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 2); // the status line isn't a message
        const replydoc::Message& a = messages.at(0);
        QCOMPARE(a.nick, QStringLiteral("Alice"));
        QCOMPARE(a.uid, kUidAlice);
        QCOMPARE(a.clientId, 17);
        QCOMPARE(a.minutes, 21 * 60 + 14);
        QCOMPARE(a.text, QStringLiteral("Hello world"));
        QVERIFY(!a.hasQuote);
        QVERIFY(a.nickColor.isValid());
        const replydoc::Message& b = messages.at(1);
        QCOMPARE(b.nick, QStringLiteral("Bob"));
        QVERIFY(b.hasQuote);
        QVERIFY(!b.restyled);
        QCOMPARE(b.quote.nick, QStringLiteral("Alice"));
        QCOMPARE(b.quote.uid, kUidAlice);
        QCOMPARE(b.quote.minutes, 21 * 60 + 14);
        QCOMPARE(b.quote.snippet, QStringLiteral("Hello world"));
        QCOMPARE(b.text, QStringLiteral("my reply"));
        QVERIFY(b.quoteStart > b.position && b.quoteEnd > b.quoteStart);
        QCOMPARE(doc.characterAt(b.quoteEnd - 1), QChar(QChar::LineSeparator));
    }

    void collapseAndRestore()
    {
        QTextDocument doc;
        fillChat(doc);
        const QString before = doc.toPlainText();
        QVector<replydoc::Message> messages = replydoc::scan(&doc);
        const QTextBlock           reply    = doc.findBlockByNumber(messages.at(1).block);
        const auto                 fragments = fragmentsOf(reply);
        const qreal                height    = messages.at(1).lineHeight;
        QVERIFY(height > 0);

        const QString name = replydoc::objectPrefix() + QStringLiteral("test.1");
        const QFont   font = messages.at(1).baseFormat.font().resolve(doc.defaultFont());
        QVERIFY(replydoc::collapse(&doc, messages.at(1), name, QSizeF(220, height - QFontMetricsF(font).descent())));
        doc.documentLayout()->documentSize();

        messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 2);
        const replydoc::Message& b = messages.at(1);
        QVERIFY(b.restyled);
        QCOMPARE(b.object, name);
        QVERIFY(b.hasQuote);
        QCOMPARE(b.quote.uid, kUidAlice);
        QCOMPARE(b.quote.snippet, QStringLiteral("Hello world"));
        QCOMPARE(b.nick, QStringLiteral("Bob"));
        QCOMPARE(b.uid, kUidBob);
        QCOMPARE(b.text, QStringLiteral("my reply"));
        const QString text = doc.findBlockByNumber(b.block).text();
        QCOMPARE(text.at(0), QChar(QChar::ObjectReplacementCharacter));
        QCOMPARE(text.at(1), QChar(QChar::LineSeparator));
        QVERIFY(!text.contains(arrow()));
        // The album code still reads the header (its sender id comes from there).
        QVERIFY(albums::parseHeaderText(text).valid);
        // The reply line is exactly as high as a message line: nothing moves when it replaces the quote.
        const QTextLayout* layout = doc.findBlockByNumber(b.block).layout();
        QVERIFY(layout->lineCount() >= 2);
        QVERIFY2(qAbs(layout->lineAt(0).height() - height) < 0.6, qPrintable(QStringLiteral("%1 vs %2").arg(layout->lineAt(0).height()).arg(height)));
        // Collapsing again does nothing.
        QVERIFY(!replydoc::collapse(&doc, b, name, QSizeF(10, 10)));

        // Back exactly as it was: text, links and formats.
        QCOMPARE(replydoc::objectPositions(&doc).size(), 1);
        QCOMPARE(replydoc::restoreAll(&doc), 1);
        QCOMPARE(doc.toPlainText(), before);
        const QTextBlock restored      = doc.findBlockByNumber(b.block);
        const auto       restoredParts = fragmentsOf(restored);
        QCOMPARE(restoredParts.size(), fragments.size()); // element by element (QVector's == warns in MSVC)
        for (int i = 0; i < fragments.size(); ++i) {
            QCOMPARE(restoredParts.at(i).first, fragments.at(i).first);
            QCOMPARE(restoredParts.at(i).second, fragments.at(i).second);
        }
        bool italic = false;
        for (auto it = restored.begin(); !it.atEnd(); ++it) {
            if (it.fragment().text().contains(arrow()))
                italic = it.fragment().charFormat().fontItalic();
        }
        QVERIFY(italic);
        QVERIFY(replydoc::objectPositions(&doc).isEmpty());
        QCOMPARE(replydoc::restoreAll(&doc), 0);
    }

    void staleMessagesAreLeftAlone()
    {
        QTextDocument doc;
        fillChat(doc);
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QTextCursor                      c(&doc);
        c.movePosition(QTextCursor::Start);
        c.insertText(QStringLiteral("moved"));
        QVERIFY(!replydoc::collapse(&doc, messages.at(1), replydoc::objectPrefix() + QStringLiteral("x"), QSizeF(10, 10)));
        QVERIFY(!replydoc::restore(&doc, 0));
    }

    void hostileDocuments()
    {
        QTextDocument doc;
        QString       html;
        // A "quote line" with a file link in it is no quote line (it would hide the link from plugin users).
        html += messageHtml(QStringLiteral("10:00:00"), 3, kUidBob, QStringLiteral("Mallory"),
                            QStringLiteral("<i>") + arrow() + QStringLiteral(" <a href=\"client://1/%1~Alice\">Alice</a>: “<a href=\"ts3file://h?name=a\">a</a>”</i><br>x").arg(kUidAlice));
        // A quote line typed without the plugin (plain text) is read; nothing in it does anything.
        html += messageHtml(QStringLiteral("10:00:01"), 3, kUidBob, QStringLiteral("Mallory"),
                            arrow() + QStringLiteral(" Alice · 09∶59: “fake”<br>gotcha"));
        // A nickname with HTML-ish characters, a 12-hour time.
        html += QStringLiteral("<p><img src=\"iconpath:MESSAGE_INCOMING\"><span>&lt;9:14:02 PM&gt;</span> <a href=\"client://5/%1~x\">\"&lt;b&gt;x&lt;/b&gt;\"</a>: hi</p>").arg(kUidAlice);
        // Plugin prints: no header.
        html += QStringLiteral("<p><span style=\"color:#00008b\">TS Media chat: Canceled 1 upload.</span></p>");
        doc.setHtml(html);
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 3);
        QVERIFY(!messages.at(0).hasQuote);
        QVERIFY(messages.at(1).hasQuote);
        QVERIFY(messages.at(1).quote.uid.isEmpty());
        QCOMPARE(messages.at(1).quote.nick, QStringLiteral("Alice"));
        QCOMPARE(messages.at(1).text, QStringLiteral("gotcha"));
        QCOMPARE(messages.at(2).nick, QStringLiteral("<b>x</b>"));
        QCOMPARE(messages.at(2).minutes, 21 * 60 + 14);
    }

    void mediaMessagesAndEmoticons()
    {
        QTextDocument doc;
        const QString link = photo().toUrl().toHtmlEscaped();
        QString       html = messageHtml(QStringLiteral("11:00:00"), 3, kUidAlice, QStringLiteral("Alice"),
                                         QStringLiteral("<a href=\"%1\">alpine-lake.jpg</a> <i>— TS Media chat plugin required</i>").arg(link));
        html += messageHtml(QStringLiteral("11:00:05"), 3, kUidAlice, QStringLiteral("Alice"),
                            QStringLiteral("caption here <img src=\"emoticons:smile.svg\"><br><a href=\"%1\">alpine-lake.jpg</a>").arg(link));
        doc.setHtml(html);
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).text, QString());
        QCOMPARE(messages.at(0).mediaLabel, QStringLiteral("alpine-lake.jpg"));
        QCOMPARE(replies::snippetSource(messages.at(0).text, messages.at(0).mediaLabel), replies::clip() + QStringLiteral(" alpine-lake.jpg"));
        QCOMPARE(messages.at(1).text, QStringLiteral("caption here :)")); // the emoticon read back as its text
        QCOMPARE(replydoc::emoticonText(QStringLiteral("emoticons:laugh.svg")), QStringLiteral(":D"));
        QCOMPARE(replydoc::emoticonText(QStringLiteral("iconpath:MESSAGE_INCOMING?size=13x13")), QString());
        QCOMPARE(replydoc::emoticonText(QStringLiteral("tsmedia:abc")), QString());
    }

    void longChatsAreBounded()
    {
        QTextDocument doc;
        QString       html;
        html.reserve(5200 * 200);
        for (int i = 0; i < 5200; ++i)
            html += messageHtml(QStringLiteral("10:%1:00").arg(i % 60, 2, 10, QLatin1Char('0')), 3, kUidBob, QStringLiteral("Bob"), QStringLiteral("message %1").arg(i));
        doc.setHtml(html);
        QElapsedTimer timer;
        timer.start();
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), replydoc::kMaxBlocks);
        QCOMPARE(messages.last().text, QStringLiteral("message 5199"));
        qInfo("scan of the newest %d of 5200 messages: %lld ms", replydoc::kMaxBlocks, static_cast<long long>(timer.elapsed()));
        QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    }

    // A reloaded history full of replies: every quote line becomes a reply line in one edit, only the
    // blocks from the first edited one are read again, and unloading gives them all back in one edit.
    // One edit per reply line laid the whole chat out again each time: 1000 lines took 1.4 s each way.
    void manyRepliesInOneEdit()
    {
        QTextDocument doc;
        QString       html;
        html.reserve(4000 * 400);
        const QString text = QStringLiteral("Anyone up for a match tonight? I'm thinking around nine, the usual server, bring snacks %1");
        for (int i = 0; i < 4000; ++i) {
            const int     minute = (i / 10) % 60;
            const QString time   = QStringLiteral("10:%1:00").arg(minute, 2, 10, QLatin1Char('0'));
            if (i % 4 == 3) {
                html += messageHtml(time, 3, kUidAlice, QStringLiteral("Alice"),
                                    quoteHtml(QStringLiteral("Bob"), 3, kUidBob, replies::formatTime(10 * 60 + minute), replies::makeSnippet(text.arg(i - 1)))
                                        + QStringLiteral("<br>reply %1").arg(i));
            } else {
                html += messageHtml(time, 3, kUidBob, QStringLiteral("Bob"), text.arg(i));
            }
        }
        doc.setHtml(html);
        doc.setTextWidth(800);
        doc.documentLayout()->documentSize();
        const QString                    before   = doc.toPlainText();
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);

        QElapsedTimer timer;
        timer.start();
        int         collapsed = 0;
        int         first     = -1;
        QTextCursor batch(&doc);
        batch.beginEditBlock();
        for (int i = messages.size() - 1; i >= 0; --i) {
            if (messages.at(i).hasQuote && replydoc::collapse(&doc, messages.at(i), replydoc::objectPrefix() + QString::number(i), QSizeF(600, 14))) {
                ++collapsed;
                first = i;
            }
        }
        batch.endEditBlock();
        doc.documentLayout()->documentSize();
        const qint64 collapseMs = timer.restart();
        QCOMPARE(collapsed, 1000);

        // The messages before the first edit as they were, the rest read again: the same as reading it all.
        QVector<replydoc::Message> partial = messages.mid(0, first);
        partial += replydoc::scanFrom(&doc, messages.at(first).block);
        const QVector<replydoc::Message> all = replydoc::scan(&doc);
        QCOMPARE(partial.size(), all.size());
        for (int i = 0; i < all.size(); ++i) {
            QCOMPARE(partial.at(i).block, all.at(i).block);
            QCOMPARE(partial.at(i).position, all.at(i).position);
            QCOMPARE(partial.at(i).restyled, all.at(i).restyled);
            QCOMPARE(partial.at(i).object, all.at(i).object);
            QCOMPARE(partial.at(i).text, all.at(i).text);
            QCOMPARE(partial.at(i).quote.snippet, all.at(i).quote.snippet);
        }

        timer.restart();
        QCOMPARE(replydoc::restoreAll(&doc), 1000);
        doc.documentLayout()->documentSize();
        const qint64 restoreMs = timer.elapsed();
        QCOMPARE(doc.toPlainText(), before);
        QVERIFY(replydoc::objectPositions(&doc).isEmpty());
        qInfo("1000 reply lines in a 4000-message chat: collapsed in %lld ms, given back in %lld ms", static_cast<long long>(collapseMs),
              static_cast<long long>(restoreMs));
        QVERIFY2(collapseMs < 700, qPrintable(QString::number(collapseMs)));
        QVERIFY2(restoreMs < 700, qPrintable(QString::number(restoreMs)));
    }

    void giantEmojiRuns()
    {
        QCOMPARE(replies::leftChars(QStringLiteral("a😀"), 2), QStringLiteral("a"));
        QCOMPARE(replies::leftChars(QStringLiteral("a😀"), 3), QStringLiteral("a😀"));
        QCOMPARE(replies::leftChars(QStringLiteral("abc"), 0), QString());
        QString emoji;
        for (int i = 0; i < 250; ++i)
            emoji += QStringLiteral("😀");
        // A quoted snippet that is a long run of emoji is cut short, never inside one (a lone surrogate
        // would be drawn as a box).
        const replies::Quote q = replies::parseQuoteBBCode(arrow() + QStringLiteral(" Alice · 21∶14: “x") + emoji + QStringLiteral("”"));
        QVERIFY(q.valid());
        QVERIFY(q.snippet.size() <= replies::kMaxSnippet);
        QVERIFY(!q.snippet.at(q.snippet.size() - 1).isHighSurrogate());
        // A file label like that in the chat, too.
        QTextDocument doc;
        doc.setHtml(messageHtml(QStringLiteral("11:00:00"), 3, kUidAlice, QStringLiteral("Alice"),
                                QStringLiteral("<a href=\"%1\">x%2</a>").arg(photo().toUrl().toHtmlEscaped(), emoji)));
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 1);
        const QString label = messages.first().mediaLabel;
        QVERIFY(!label.isEmpty() && label.size() <= replies::kMaxNickChars * 4);
        QVERIFY(!label.at(label.size() - 1).isHighSurrogate());
        // The reply line stays within its width whatever it is given.
        replyart::Header header;
        header.nick    = QStringLiteral("x") + emoji.left(60);
        header.snippet = q.snippet;
        replyart::HeaderStyle style;
        style.font.setPixelSize(13);
        const QSize size = replyart::headerSize(header, style, 400);
        QVERIFY(size.width() <= 400);
        QVERIFY(!replyart::renderHeader(header, style, size).isNull());
    }

    // ---- the reply line's drawing ---------------------------------------------------------------------------
    void headerGeometry()
    {
        replyart::Header header;
        header.nick      = QStringLiteral("Alice");
        header.snippet   = QStringLiteral("Hello world, this is a fairly long message that will not fit");
        header.found     = true;
        header.nickColor = QColor(0x1c, 0xb0, 0xf4);
        replyart::HeaderStyle style;
        style.font.setPixelSize(13);
        style.lineHeight = 15;
        for (const bool dark : {false, true}) {
            style.dark = dark;
            style.base = dark ? QColor(0x2b, 0x2d, 0x31) : QColor(Qt::white);
            for (const qreal dpr : {1.0, 2.0}) {
                style.dpr          = dpr;
                const QSize narrow = replyart::headerSize(header, style, 200);
                QCOMPARE(narrow.width(), 200);
                QCOMPARE(narrow.height(), 15);
                const QImage image = replyart::renderHeader(header, style, narrow);
                QCOMPARE(image.size(), narrow * dpr);
                QCOMPARE(image.devicePixelRatio(), dpr);
                const QSize wide = replyart::headerSize(header, style, 2000);
                QVERIFY(wide.width() < 2000); // as wide as it needs
                // The name keeps 4.5:1 on the chat; an unknown original's name is muted, not coloured.
                QVERIFY(ui::contrastRatio(replyart::headerNameColor(header, style), style.base) >= 4.5);
                replyart::Header lost = header;
                lost.found            = false;
                QCOMPARE(replyart::headerNameColor(lost, style), replyart::paletteFor(dark, style.base).muted);
                QVERIFY(ui::contrastRatio(replyart::paletteFor(dark, style.base).muted, style.base) >= 4.5);
            }
        }
        // A low-contrast nickname colour is made readable.
        QVERIFY(ui::contrastRatio(replyart::readable(QColor(0x00, 0x2f, 0x5d), QColor(0x20, 0x20, 0x20), QColor()), QColor(0x20, 0x20, 0x20)) >= 4.5);
    }

    // ---- files sent as a reply -------------------------------------------------------------------------------
    void leadBeforeCaption()
    {
        ComposeOptions options;
        options.lead    = replies::quoteLine(alice());
        options.caption = QStringLiteral("look at this");
        const QStringList messages = composeChatMessages({photo()}, options);
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.first().startsWith(options.lead + QStringLiteral("\nlook at this\n[URL=ts3file://")));
        QCOMPARE(MediaLink::findInMessage(messages.first()).size(), 1);
        // No caption: the quote line, then the link.
        options.caption = QString();
        const QString only = composeChatMessages({photo()}, options).first();
        QVERIFY(only.startsWith(options.lead + QStringLiteral("\n[URL=ts3file://")));
        // Read back by a receiver: the first line is the quote.
        QVERIFY(replies::parseQuoteBBCode(only.section(QLatin1Char('\n'), 0, 0)).valid());
        // A caption too long to share the message goes alone, with the quote line in front of it.
        options.caption  = QString(kCaptionMaxChars, QLatin1Char('c'));
        options.maxBytes = escapedMessageSize(composeChatMessages({photo()}, ComposeOptions()).first()) + 40;
        const QStringList split = composeChatMessages({photo()}, options);
        QCOMPARE(split.size(), 2);
        QVERIFY(split.first().startsWith(options.lead + QLatin1Char('\n')));
        QVERIFY(MediaLink::findInMessage(split.first()).isEmpty());
    }
};

TSMEDIA_REGISTER_TEST(TestReplies)
#include "tst_replies.moc"
