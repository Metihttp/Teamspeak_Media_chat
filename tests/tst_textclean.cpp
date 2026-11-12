// Unit tests for 2.5 link previews: the one text cleaner (src/linkpreview/textclean.*) and the 2.4 wrappers that
// call it (content::cleanText, announcement's cleanText, linkcard::cleanText).
//
// Every non-ASCII character is written as a code point (s({...})): no invisible or bidi character in the source.

#include <QtTest>

#include <initializer_list>

#include "linkpreview/announcement.h"
#include "linkpreview/content.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/textclean.h"
#include "testmain.h"

using namespace linkpreview;

namespace {

// A string from code points; ASCII letters may be given as characters ('a').
QString s(std::initializer_list<uint> codes)
{
    QString out;
    for (const uint code : codes)
        out += QString::fromUcs4(&code, 1);
    return out;
}

QString a(const char* ascii)
{
    return QString::fromLatin1(ascii);
}

const QString kEllipsis = QString(QChar(0x2026));
const QString kZwj      = QString(QChar(0x200D));

// man ZWJ woman ZWJ girl (one grapheme)
QString family()
{
    return s({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467});
}

// "mi-khaham" in Persian: a ZWNJ between its second and third letter
QString persian()
{
    return s({0x0645, 0x06CC, 0x200C, 0x062E, 0x0648, 0x0627, 0x0647, 0x0645});
}

} // namespace

class TestTextClean : public QObject
{
    Q_OBJECT

  private slots:
    void table_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("plain") << a("Hello") << a("Hello");
        QTest::newRow("rlo exe") << s({'a', 0x202E, 'g', 'p', 'j', '.', 'e', 'x', 'e'}) << a("agpj.exe");
        QTest::newRow("tag chars") << s({'x', 0xE0041, 'y'}) << a("xy");
        QTest::newRow("hangul fillers") << s({0x3164, 0x3164}) << QString();
        QTest::newRow("fillers mixed") << s({'a', 0x115F, 0x1160, 'b', 0xFFA0, 'c', 0x2800, 'd', 0x034F, 'e', 0x17B4, 'f', 0x17B5, 'g', 0x180E, 'h'})
                                       << a("abcdefgh");
        QTest::newRow("persian zwnj kept") << persian() << persian();
        QTest::newRow("zwnj at start") << s({0x200C, 0x0645, 0x06CC}) << s({0x0645, 0x06CC});
        QTest::newRow("zwnj at end") << s({0x0645, 0x06CC, 0x200C}) << s({0x0645, 0x06CC});
        QTest::newRow("zwnj before space") << s({0x0645, 0x200C, ' ', 0x06CC}) << s({0x0645, ' ', 0x06CC});
        QTest::newRow("two zwnj") << s({'a', 0x200C, 0x200C, 'b'}) << s({'a', 0x200C, 'b'});
        QTest::newRow("family emoji") << family() << family();
        QTest::newRow("zwj alone") << kZwj << QString();
        QTest::newRow("marks capped") << s({'e', 0x0301, 0x0301, 0x0301, 0x0301, 'x'}) << s({'e', 0x0301, 0x0301, 'x'});
        QTest::newRow("marks capped through zwj") << s({'e', 0x0301, 0x200D, 0x0301, 0x200D, 0x0301, 0x0301}) << s({'e', 0x0301, 0x200D, 0x0301});
        QTest::newRow("vs16 after symbol") << s({0x2764, 0xFE0F}) << s({0x2764, 0xFE0F});
        QTest::newRow("vs16 after letter") << s({'a', 0xFE0F}) << s({'a', 0xFE0F});
        QTest::newRow("vs16 alone") << s({0xFE0F}) << QString();
        QTest::newRow("vs16 after space") << s({'a', ' ', 0xFE0F, 'b'}) << a("a b");
        QTest::newRow("vs16 twice") << s({'a', 0xFE0F, 0xFE0F}) << s({'a', 0xFE0F});
        QTest::newRow("keycap") << s({'1', 0xFE0F, 0x20E3}) << s({'1', 0xFE0F, 0x20E3});
        QTest::newRow("noncharacters") << s({'a', 0xFDD0, 'b', 0xFFFE, 'c', 0xFFFF, 'd'}) << a("abcd");
        QTest::newRow("plane noncharacter") << s({'a', 0x1FFFE, 'b'}) << a("ab");
        QTest::newRow("pua") << s({'a', 0xE000, 'b', 0xF8FF, 'c'}) << a("abc");
        QTest::newRow("plane 15 16 pua") << s({'a', 0xF0001, 0x10FFFD, 'b'}) << a("ab");
        QTest::newRow("vs supplement") << s({'a', 0xE0100, 'b'}) << a("ab");
        QTest::newRow("lone high") << (QString(QChar(0xD83D)) + a("a")) << a("a");
        QTest::newRow("lone low") << (a("a") + QChar(0xDE00)) << a("a");
        QTest::newRow("replacement char") << s({'a', 0xFFFD, 'b', 0xFFFC, 'c'}) << a("abc");
        QTest::newRow("white runs") << s({' ', ' ', 'a', ' ', '\t', '\n', '\r', ' ', 'b', 0x2028, 'c', 0x2029, 'd', 0x85, 'e', 0xA0, 'f', 0x3000, 'g', ' '})
                                    << a("a b c d e f g");
        QTest::newRow("controls") << s({'a', 0x0, 'b', 0x7, 'c', 0x9F, 'd', 0x7F, 'e'}) << a("abcde");
        QTest::newRow("bidi") << s({'a', 0x2066, 'b', 0x2067, 'c', 0x2068, 'd', 0x2069, 'e', 0x061C, 'f', 0x200E, 'g', 0x200F, 'h', 0x202A, 'i', 0x202B, 'j'})
                              << a("abcdefghij");
        QTest::newRow("zero width") << s({'a', 0x200B, 'b', 0x2060, 'c', 0x2061, 'd', 0xFEFF, 'e', 0x00AD, 'f'}) << a("abcdef");
        QTest::newRow("only invisible") << s({0x202E, 0x200B, ' ', ' '}) << QString();
        QTest::newRow("joiner after removed") << s({'a', 0x200B, 0x200C, 'b'}) << s({'a', 0x200C, 'b'});
        QTest::newRow("space joiner space") << s({'a', ' ', 0x200D, ' ', 'b'}) << a("a b");
    }
    void table()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(textclean::clean(in, 1000), out);
        QCOMPARE(textclean::cleanToBytes(in, 4000), out);
        QCOMPARE(textclean::clean(out, 1000), out); // idempotent
    }

    void capsWithEllipsis()
    {
        const QString smile = s({0x1F600});
        const QString cut   = textclean::clean(QString(9, QLatin1Char('a')) + smile + smile, 11);
        QVERIFY(cut.size() <= 11);
        QVERIFY(cut.endsWith(kEllipsis));
        QVERIFY(!cut.at(cut.size() - 2).isHighSurrogate());
        QCOMPARE(textclean::clean(QString(500, QLatin1Char('t')), 200).size(), 200);
        QCOMPARE(textclean::clean(a("abcdef"), 6), a("abcdef"));
        QCOMPARE(textclean::clean(a("abcdefg"), 6), a("abcde") + kEllipsis);
        QCOMPARE(textclean::clean(a("abc def"), 5), a("abc") + kEllipsis); // no space before the ellipsis
        QCOMPARE(textclean::clean(a("abc"), 0), QString());
        // A hostile, very long value is bounded before it is looked at.
        const QString huge = QString(2000000, QChar(0x200B)) + a("x");
        QCOMPARE(textclean::clean(huge, 100), QString());
    }

    void bytesCap()
    {
        QCOMPARE(textclean::cleanToBytes(s({0xE9, 0xE9, 0xE9}), 4), s({0xE9, 0xE9}));
        QCOMPARE(textclean::cleanToBytes(s({0x1F600, 0x1F600}), 5), s({0x1F600}));
        QCOMPARE(textclean::cleanToBytes(a("abc"), 3), a("abc"));
        QCOMPARE(textclean::cleanToBytes(a("abcd"), 3), a("abc"));
        QCOMPARE(textclean::cleanToBytes(a("ab cd"), 3), a("ab"));
        QCOMPARE(textclean::utf8Bytes(s({'a', 0xE9, 0x4E2D, 0x1F600})), 1 + 2 + 3 + 4);
        // Whole characters only, never more than the cap.
        const QString mixed = persian() + a(" ") + family() + s({' ', 'e', 0x0301, 0x0302});
        for (int cap = 0; cap < 48; ++cap) {
            const QString out = textclean::cleanToBytes(mixed, cap);
            QVERIFY(out.toUtf8().size() <= cap);
            QCOMPARE(QString::fromUtf8(out.toUtf8()), out);
            QCOMPARE(textclean::cleanToBytes(out, cap), out);
        }
    }

    void cutAtGraphemes()
    {
        // A family emoji at the cut is kept whole or dropped whole.
        const QString fam  = family();
        const QString text = a("ab") + fam + a("cd");
        for (int max = 1; max <= fam.size() + 6; ++max) {
            const QString out = textclean::clean(text, max);
            QVERIFY(out.size() <= max);
            if (out.contains(kZwj) || out.contains(s({0x1F469})) || out.contains(s({0x1F467})))
                QVERIFY2(out.contains(fam), qPrintable(QString::number(max)));
            QVERIFY(!out.endsWith(kZwj));
            const QString bytes = textclean::cleanToBytes(text, max * 2);
            if (bytes.contains(kZwj) || bytes.contains(s({0x1F469})))
                QVERIFY(bytes.contains(fam));
        }
        // A ZWNJ left at the cut goes ("mi-khaham" cut right after the ZWNJ).
        QCOMPARE(textclean::clean(persian(), 4), s({0x0645, 0x06CC}) + kEllipsis); // room for 3 units and the ellipsis
        QCOMPARE(textclean::cleanToBytes(persian(), 7), s({0x0645, 0x06CC}));     // 4 bytes + the ZWNJ's 3 fit, but it ends the text
        // A base and its marks are never split.
        const QString marked = s({'a', 'e', 0x0301, 0x0302, 'b'});
        for (int max = 1; max <= 6; ++max) {
            const QString out = textclean::clean(marked, max);
            QVERIFY(out.contains(s({'e', 0x0301, 0x0302})) || !out.contains(QLatin1Char('e')));
        }
        for (int max = 1; max <= 8; ++max) {
            const QString out = textclean::cleanToBytes(marked, max);
            QVERIFY(out.contains(s({'e', 0x0301, 0x0302})) || !out.contains(QLatin1Char('e')));
        }
        // Flags (regional indicator pairs) stay whole.
        const QString flag = s({0x1F1EE, 0x1F1F7});
        QCOMPARE(textclean::clean(a("a") + flag + flag, 7), a("a") + flag + kEllipsis);
    }

    void wrappersAgree()
    {
        const QStringList samples = {a("  Hello \t\n  World  "),
                                     s({'a', 0x202E, 'g', 'p', 'j', '.', 'e', 'x', 'e'}),
                                     persian(),
                                     family() + a(" family"),
                                     s({'e', 0x0301, 0x0301, 0x0301, 0x0301}),
                                     QString(300, QLatin1Char('x')),
                                     a("<b>tag</b> & text")};
        for (const QString& t : samples) {
            QCOMPARE(content::cleanText(t, 200), textclean::clean(t, 200));
            QCOMPARE(linkcard::cleanText(t, 120), textclean::clean(t, 120));
            QCOMPARE(linkpreview::cleanText(t, 256), textclean::cleanToBytes(t, 256));
        }
    }
};

TSMEDIA_REGISTER_TEST(TestTextClean)

#include "tst_textclean.moc"
