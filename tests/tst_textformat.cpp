// 2.7 F1: the chat input's formatting (textformat.h): Bold, Italic, Underline (TeamSpeak's own BBCode) and Spoiler
// wrap the selection and toggle off again; the selection is trimmed, grows over pairs it cuts (no broken tags), a
// bare web address at its end goes into [URL] first, nothing selected inserts an empty pair, multi-line works.

#include <QtTest>

#include "testmain.h"
#include "textformat.h"

using textformat::Format;

namespace {

// text with "|" marking the selection's ends (two) or the cursor (one); returns the text without the marks.
QString parse(const QString& marked, int* s, int* e)
{
    const int a = marked.indexOf(QLatin1Char('|'));
    const int b = marked.indexOf(QLatin1Char('|'), a + 1);
    QString   text = marked;
    text.remove(QLatin1Char('|'));
    *s = a;
    *e = b < 0 ? a : b - 1;
    return text;
}

// The result with the new selection marked the same way.
QString run(const QString& marked, Format f)
{
    int                    s = 0, e = 0;
    const QString          text = parse(marked, &s, &e);
    const textformat::Edit edit = textformat::apply(text, s, e, f);
    if (!edit.valid())
        return QStringLiteral("<invalid>");
    QString out = textformat::applied(text, edit);
    if (edit.selEnd > edit.selStart)
        out.insert(edit.selEnd, QLatin1Char('|'));
    out.insert(edit.selStart, QLatin1Char('|'));
    return out;
}

bool applied(const QString& marked, Format f)
{
    int           s = 0, e = 0;
    const QString text = parse(marked, &s, &e);
    return textformat::isApplied(text, s, e, f);
}

} // namespace

class TestTextFormat : public QObject
{
    Q_OBJECT

  private slots:
    void tags()
    {
        QCOMPARE(textformat::openTag(Format::Bold), QStringLiteral("[B]"));
        QCOMPARE(textformat::closeTag(Format::Italic), QStringLiteral("[/I]"));
        QCOMPARE(textformat::openTag(Format::Underline), QStringLiteral("[U]"));
        QCOMPARE(textformat::openTag(Format::Spoiler), QStringLiteral("[spoiler]"));
        QCOMPARE(textformat::closeTag(Format::Spoiler), QStringLiteral("[/spoiler]"));
    }

    void wrapEachFormat()
    {
        QCOMPARE(run(QStringLiteral("Hello |world|"), Format::Bold), QStringLiteral("Hello [B]|world|[/B]"));
        QCOMPARE(run(QStringLiteral("Hello |world|"), Format::Italic), QStringLiteral("Hello [I]|world|[/I]"));
        QCOMPARE(run(QStringLiteral("|Hello| world"), Format::Underline), QStringLiteral("[U]|Hello|[/U] world"));
        QCOMPARE(run(QStringLiteral("Hello |world|"), Format::Spoiler), QStringLiteral("Hello [spoiler]|world|[/spoiler]"));
    }

    void trimsWhiteSpace()
    {
        QCOMPARE(run(QStringLiteral("say |hello |there"), Format::Bold), QStringLiteral("say [B]|hello|[/B] there"));
        QCOMPARE(run(QStringLiteral("say| hello |there"), Format::Spoiler), QStringLiteral("say [spoiler]|hello|[/spoiler] there"));
    }

    void togglesOff()
    {
        // Tags right around the selection (the inner text stays selected, as after wrapping).
        QCOMPARE(run(QStringLiteral("Hello [B]|world|[/B]"), Format::Bold), QStringLiteral("Hello |world|"));
        // Tags inside the selection, any case.
        QCOMPARE(run(QStringLiteral("Hello |[b]world[/b]|"), Format::Bold), QStringLiteral("Hello |world|"));
        QCOMPARE(run(QStringLiteral("|[SPOILER]x y[/Spoiler]| z"), Format::Spoiler), QStringLiteral("|x y| z"));
        QVERIFY(applied(QStringLiteral("Hello [B]|world|[/B]"), Format::Bold));
        QVERIFY(applied(QStringLiteral("Hello |[i]world[/i]|"), Format::Italic));
        QVERIFY(!applied(QStringLiteral("Hello [B]|world|[/B]"), Format::Italic));
        QVERIFY(!applied(QStringLiteral("Hello |world|"), Format::Bold));
        // Wrap then toggle gives the text back.
        QCOMPARE(run(run(QStringLiteral("a |b| c"), Format::Underline), Format::Underline), QStringLiteral("a |b| c"));
    }

    void nestedFormatsToggleEachOne()
    {
        // Bold, then Italic on the same selection; each still toggles off whichever way it is selected.
        const QString both = run(run(QStringLiteral("|x|"), Format::Bold), Format::Italic);
        QCOMPARE(both, QStringLiteral("[B][I]|x|[/I][/B]"));
        QVERIFY(applied(both, Format::Bold));
        QVERIFY(applied(both, Format::Italic));
        QCOMPARE(run(both, Format::Bold), QStringLiteral("[I]|x|[/I]"));
        QVERIFY(applied(QStringLiteral("|[I][B]x[/B][/I]|"), Format::Bold));
        QCOMPARE(run(QStringLiteral("|[I][B]x[/B][/I]|"), Format::Bold), QStringLiteral("|[I]x[/I]|"));
    }

    // Critique M6: the toggle needs one balanced pair; a wrap grows over every pair it cuts.
    void severalPairsNeverBreak()
    {
        QVERIFY(!applied(QStringLiteral("|[B]a[/B] b [B]c[/B]|"), Format::Bold));
        QCOMPARE(run(QStringLiteral("|[B]a[/B] b [B]c[/B]|"), Format::Bold), QStringLiteral("[B]|a b c|[/B]"));
        QCOMPARE(run(QStringLiteral("|x [spoiler]b| c[/spoiler] y"), Format::Spoiler), QStringLiteral("[spoiler]|x b c|[/spoiler] y"));
        // A cut pair of another format is taken whole too.
        QCOMPARE(run(QStringLiteral("|a [I]b| c[/I] d"), Format::Bold), QStringLiteral("[B]|a [I]b c[/I]|[/B] d"));
        QCOMPARE(run(QStringLiteral("[B]a |b[/B] c|"), Format::Underline), QStringLiteral("[U]|[B]a b[/B] c|[/U]"));
    }

    void selectionInsideOnePair()
    {
        // Bold inside bold: nests (TeamSpeak reads it); Spoiler inside a spoiler: that spoiler toggles off.
        QCOMPARE(run(QStringLiteral("[B]a |b| c[/B]"), Format::Bold), QStringLiteral("[B]a [B]|b|[/B] c[/B]"));
        QVERIFY(applied(QStringLiteral("x [spoiler]b |c| d[/spoiler] y"), Format::Spoiler));
        QCOMPARE(run(QStringLiteral("x [spoiler]b |c| d[/spoiler] y"), Format::Spoiler), QStringLiteral("x b |c| d y"));
    }

    // Rule 4: no spoiler inside a spoiler (markers inside go, also a stray one).
    void spoilerMerges()
    {
        QCOMPARE(run(QStringLiteral("|a [spoiler]b[/spoiler] c|"), Format::Spoiler), QStringLiteral("[spoiler]|a b c|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|a [/spoiler] b|"), Format::Spoiler), QStringLiteral("[spoiler]|a  b|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|a [spoiler]b[/spoiler] [spoiler]c[/spoiler]|"), Format::Spoiler), QStringLiteral("[spoiler]|a b c|[/spoiler]"));
    }

    // Rule 5: a bare address at the selection's end can never take the closing tag in.
    void addressAtTheEnd()
    {
        QCOMPARE(run(QStringLiteral("|look https://example.com/a.png|"), Format::Spoiler),
                 QStringLiteral("[spoiler]|look [URL]https://example.com/a.png[/URL]|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|www.example.com| now"), Format::Bold), QStringLiteral("[B]|[URL]www.example.com[/URL]|[/B] now"));
        // Not at the end, already in [URL], or cut by the selection: left alone.
        QCOMPARE(run(QStringLiteral("|https://example.com is it|"), Format::Spoiler), QStringLiteral("[spoiler]|https://example.com is it|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|[URL]https://example.com[/URL]|"), Format::Bold), QStringLiteral("[B]|[URL]https://example.com[/URL]|[/B]"));
        QCOMPARE(run(QStringLiteral("|https://exa|mple.com"), Format::Bold), QStringLiteral("[B]|https://exa|[/B]mple.com"));
        // Review fix: a spoiler wraps it whatever follows (punctuation, or the rest of a cut address), so "[/spoiler]"
        // never sits inside a link that goes on after it.
        QCOMPARE(run(QStringLiteral("|look https://example.com/a.png|, ok"), Format::Spoiler),
                 QStringLiteral("[spoiler]|look [URL]https://example.com/a.png[/URL]|[/spoiler], ok"));
        QCOMPARE(run(QStringLiteral("|https://exa|mple.com"), Format::Spoiler), QStringLiteral("[spoiler]|[URL]https://exa[/URL]|[/spoiler]mple.com"));
    }

    void nothingSelectedInsertsAPair()
    {
        QCOMPARE(run(QStringLiteral("Hi|"), Format::Spoiler), QStringLiteral("Hi[spoiler]|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|"), Format::Bold), QStringLiteral("[B]|[/B]"));
        QCOMPARE(run(QStringLiteral("a |  | b"), Format::Spoiler), QStringLiteral("a   [spoiler]|[/spoiler] b"));
        QVERIFY(!applied(QStringLiteral("Hi|"), Format::Spoiler));
    }

    void multiLine()
    {
        const QString lines = QStringLiteral("|one") + QChar(0x2029) + QStringLiteral("two|");
        QCOMPARE(run(lines, Format::Spoiler), QStringLiteral("[spoiler]|one") + QChar(0x2029) + QStringLiteral("two|[/spoiler]"));
        QCOMPARE(run(QStringLiteral("|a\nb|"), Format::Bold), QStringLiteral("[B]|a\nb|[/B]"));
    }

    void outOfRangeIsClamped()
    {
        const textformat::Edit e = textformat::apply(QStringLiteral("abc"), 5, -2, Format::Bold);
        QVERIFY(e.valid());
        QCOMPARE(textformat::applied(QStringLiteral("abc"), e), QStringLiteral("[B]abc[/B]"));
    }
};

TSMEDIA_REGISTER_TEST(TestTextFormat)
#include "tst_textformat.moc"
