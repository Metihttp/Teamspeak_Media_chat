// 2.7 F1: text spoilers as text (textspoiler.h): the pairing rules, masking for reply quotes and snippets (sender and
// receiver alike, so a quote still finds its original; a quote made by TS Media 2.6 shows "(spoiler)" too), and what
// the sender's link previews and the GIFs tab read of a message (nothing inside a spoiler). Made-up people only.

#include <QtTest>

#include "gifrules.h"
#include "linkpreview/lpmessage.h"
#include "medialink.h"
#include "replies.h"
#include "testmain.h"
#include "textspoiler.h"

namespace {

const QString kUidAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");

replies::Original alice(const QString& text)
{
    replies::Original o;
    o.nick     = QStringLiteral("Alice");
    o.uid      = kUidAlice;
    o.clientId = 17;
    o.minutes  = 21 * 60 + 14;
    o.text     = text;
    return o;
}

QString arrow()
{
    return replies::arrow();
}

} // namespace

class TestTextSpoiler : public QObject
{
    Q_OBJECT

  private slots:
    void markers()
    {
        QCOMPARE(textspoiler::openMarker(), QStringLiteral("[spoiler]"));
        QCOMPARE(textspoiler::closeMarker(), QStringLiteral("[/spoiler]"));
        QCOMPARE(textspoiler::placeholder(), QStringLiteral("(spoiler)"));
    }

    void pairs()
    {
        const QString text = QStringLiteral("a [spoiler]b[/spoiler] c [SPOILER]d e[/Spoiler]");
        const QVector<textspoiler::Range> r = textspoiler::ranges(text);
        QCOMPARE(r.size(), 2);
        QCOMPARE(r.at(0).open, 2);
        QCOMPARE(text.mid(r.at(0).contentStart, r.at(0).contentEnd - r.at(0).contentStart), QStringLiteral("b"));
        QCOMPARE(r.at(0).closeEnd, 22);
        QCOMPARE(text.mid(r.at(1).contentStart, r.at(1).contentEnd - r.at(1).contentStart), QStringLiteral("d e"));
        QCOMPARE(r.at(1).closeEnd, text.size());
    }

    void notSpoilers()
    {
        QVERIFY(textspoiler::ranges(QStringLiteral("[spoiler]never closed")).isEmpty());
        QVERIFY(textspoiler::ranges(QStringLiteral("[spoiler][/spoiler]")).isEmpty());
        QVERIFY(textspoiler::ranges(QStringLiteral("[spoiler]  \n [/spoiler]")).isEmpty());
        QVERIFY(textspoiler::ranges(QStringLiteral("[/spoiler]x[spoiler]")).isEmpty());
        QVERIFY(textspoiler::ranges(QStringLiteral("[spoiler=Title]x[/spoiler] [sp]y[/sp] ||z||")).isEmpty());
        // An empty pair is skipped, the next one counts.
        QCOMPARE(textspoiler::ranges(QStringLiteral("[spoiler][/spoiler] [spoiler]x[/spoiler]")).size(), 1);
    }

    void noNesting()
    {
        // The first closing marker ends it; the inner opening marker is part of its text.
        const QString text = QStringLiteral("[spoiler]a [spoiler]b[/spoiler] c[/spoiler]");
        const QVector<textspoiler::Range> r = textspoiler::ranges(text);
        QCOMPARE(r.size(), 1);
        QCOMPARE(text.mid(r.at(0).contentStart, r.at(0).contentEnd - r.at(0).contentStart), QStringLiteral("a [spoiler]b"));
    }

    void atMost32()
    {
        QString text;
        for (int i = 0; i < 33; ++i)
            text += QStringLiteral("[spoiler]%1[/spoiler] ").arg(i);
        QCOMPARE(textspoiler::ranges(text).size(), 32);
        QCOMPARE(textspoiler::ranges(text, 5).size(), 5);
        QVERIFY(textspoiler::mayHold(QStringLiteral("x [SpOiLeR")));
        QVERIFY(!textspoiler::mayHold(QStringLiteral("spoiler")));
    }

    void masking()
    {
        QCOMPARE(textspoiler::masked(QStringLiteral("x [spoiler]secret[/spoiler] y")), QStringLiteral("x (spoiler) y"));
        QCOMPARE(textspoiler::masked(QStringLiteral("[spoiler]a[/spoiler][spoiler]b[/spoiler]")), QStringLiteral("(spoiler)(spoiler)"));
        // Unclosed: shown as typed, or hidden to the end for a cut snippet.
        QCOMPARE(textspoiler::masked(QStringLiteral("x [spoiler]secr")), QStringLiteral("x [spoiler]secr"));
        QCOMPARE(textspoiler::masked(QStringLiteral("x [spoiler]secr"), true), QStringLiteral("x (spoiler)"));
        QCOMPARE(textspoiler::masked(QStringLiteral("x [spoiler]a[/spoiler] [spoiler]b…"), true), QStringLiteral("x (spoiler) (spoiler)"));
        QCOMPARE(textspoiler::masked(QStringLiteral("x [spoiler]  "), true), QStringLiteral("x [spoiler]  "));
    }

    void withoutHidden()
    {
        QCOMPARE(textspoiler::withoutHidden(QStringLiteral("see [spoiler]https://example.com/a.png[/spoiler] now")), QStringLiteral("see  now"));
        QCOMPARE(textspoiler::withoutHidden(QStringLiteral("[spoiler]x [URL]https://example.com/a.gif[/URL][/spoiler] https://example.org/b.png")),
                 QStringLiteral(" https://example.org/b.png"));
        // The client wrapped an address that ran into the closing marker: its closing tag goes too.
        QCOMPARE(textspoiler::withoutHidden(QStringLiteral("[spoiler][URL]https://example.com/a.png[/spoiler][/URL] end")), QStringLiteral(" end"));
        QCOMPARE(textspoiler::withoutHidden(QStringLiteral("[spoiler]a[/spoiler] b [spoiler]c[/spoiler]")), QStringLiteral(" b "));
        QCOMPARE(textspoiler::withoutHidden(QStringLiteral("plain [b]text[/b]")), QStringLiteral("plain [b]text[/b]"));
    }

    // The sender's link previews: no preview made or announced for a link inside a spoiler; the identity hash still
    // reads the whole message (receivers see the markers and the text as TeamSpeak shows them).
    void senderReadsNoHiddenLink()
    {
        const QString bbcode = QStringLiteral("x [spoiler]secret [URL]https://example.com/a.png[/URL][/spoiler] [URL]https://example.org/b[/URL]");
        const QStringList hrefs = linkpreview::message::linkHrefs(textspoiler::withoutHidden(bbcode), 8);
        QCOMPARE(hrefs, QStringList{QStringLiteral("https://example.org/b")});
        QCOMPARE(linkpreview::message::visibleText(bbcode), QStringLiteral("x [spoiler]secret https://example.com/a.png[/spoiler] https://example.org/b"));
    }

    // The GIFs tab: a GIF sent inside a spoiler isn't remembered.
    void gifsSkipHiddenLinks()
    {
        const QString bbcode = QStringLiteral("[spoiler][URL]https://example.com/funny.gif[/URL][/spoiler]");
        QVERIFY(!gifs::messageLinks(bbcode).isEmpty());
        QVERIFY(gifs::messageLinks(textspoiler::withoutHidden(bbcode)).isEmpty());
    }

    // Snippets, quotes and matching mask alike on both sides.
    void repliesMaskSpoilers()
    {
        const QString text = QStringLiteral("look [spoiler]the butler did it[/spoiler] ok");
        QCOMPARE(replies::makeSnippet(text), QStringLiteral("look (spoiler) ok"));
        // A link inside: "(spoiler)", never "(link)" with the rest of the spoiler around it.
        QCOMPARE(replies::makeSnippet(QStringLiteral("[spoiler]https://example.com/x[/spoiler]")), QStringLiteral("(spoiler)"));
        const QString line = replies::quoteLine(alice(text));
        QVERIFY2(!line.contains(QLatin1String("butler")), qPrintable(line));
        QVERIFY2(line.contains(QLatin1String("(spoiler)")), qPrintable(line));
        QCOMPARE(replies::matchKey(text), replies::normalized(QStringLiteral("look (spoiler) ok")));
        const QString reply = replies::composeReply(alice(text), QStringLiteral("no way"), kMaxMessageBytes);
        QVERIFY(!reply.contains(QLatin1String("butler")));
    }

    void quoteFindsItsOriginal()
    {
        const QString original = QStringLiteral("x [spoiler]secret[/spoiler] y");
        const replies::Quote q = replies::parseQuoteBBCode(replies::quoteLine(alice(original)));
        QVERIFY(q.valid());
        QCOMPARE(q.snippet, QStringLiteral("x (spoiler) y"));
        QVector<replies::Candidate> all;
        replies::Candidate          other;
        other.uid     = kUidAlice;
        other.nick    = QStringLiteral("Alice");
        other.minutes = 21 * 60 + 14;
        other.source  = QStringLiteral("x something else y");
        replies::Candidate mine = other;
        mine.source             = original;
        all << mine << other;
        QCOMPARE(replies::findOriginal(q, all, all.size()), 0);
    }

    // A quote made by TS Media 2.6 or older quoted the text: it reads "(spoiler)" here (also cut inside the spoiler).
    void olderQuoteShowsPlaceholder()
    {
        const replies::Quote whole = replies::parseQuoteBBCode(arrow() + QStringLiteral(" Alice · 21:14: “x [spoiler]secret[/spoiler] y”"));
        QVERIFY(whole.valid());
        QCOMPARE(whole.snippet, QStringLiteral("x (spoiler) y"));
        const replies::Quote cut = replies::parseQuoteBBCode(arrow() + QStringLiteral(" Alice · 21:14: “x [spoiler]a long secr…”"));
        QVERIFY(cut.valid());
        QCOMPARE(cut.snippet, QStringLiteral("x (spoiler)…"));
        // ... and still finds its original.
        QVector<replies::Candidate> all;
        replies::Candidate          c;
        c.uid     = kUidAlice;
        c.nick    = QStringLiteral("Alice");
        c.minutes = 21 * 60 + 14;
        c.source  = QStringLiteral("x [spoiler]secret[/spoiler] y");
        all << c;
        QCOMPARE(replies::findOriginal(whole, all, 1), 0);
    }
};

TSMEDIA_REGISTER_TEST(TestTextSpoiler)
#include "tst_textspoiler.moc"
