// 2.2 integration: replies and HD emoji in the same chat. A reply line (replydoc.h) sits at a block's
// start while HD emoji (chatemoji.h) are pictures inside its text: neither is taken for the other, either
// can be given back first without leaving a picture of the other behind, messages and quote lines with HD
// emoji read back as their text, a reply of only emoji becomes large once its quote line is out, and the
// reply line draws emoji as HD pictures.

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QImage>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QtTest>

#include "chatemoji.h"
#include "emojiformat.h"
#include "emojirender.h"
#include "emojitext.h"
#include "replies.h"
#include "replyart.h"
#include "replydoc.h"
#include "settings.h"
#include "testmain.h"

namespace {

const QString kUidAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kUidBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");

QString u(const char* utf8)
{
    return QString::fromUtf8(utf8);
}

// "|emoticons:smile.svg|" in text: one of TeamSpeak's emoticon pictures.
void insertBody(QTextCursor& c, const QString& text, const QTextCharFormat& format)
{
    for (const QString& part : text.split(QLatin1Char('|'))) {
        if (part.startsWith(QLatin1String("emoticons:"))) {
            QTextImageFormat emoticon;
            emoticon.setName(part);
            c.insertImage(emoticon);
        } else if (!part.isEmpty()) {
            c.insertText(part, format);
        }
    }
}

// One chat message the way TeamSpeak shows it (S0): icon, time, the quoted nickname linked to the client,
// ": " and the text. quoted: a reply's quote line first (to Alice at 21:14), then a line break.
void message(QTextDocument* doc, const QString& nick, const QString& uid, const QString& text, const QString& quoted = QString(), bool isReply = false)
{
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (!doc->isEmpty())
        c.insertBlock();
    QTextImageFormat icon;
    icon.setName(QStringLiteral("iconpath:MESSAGE_INCOMING?size=13x13"));
    c.insertImage(icon);
    c.insertText(QStringLiteral("<21:15:30> "), QTextCharFormat());
    QTextCharFormat link;
    link.setAnchor(true);
    link.setAnchorHref(QStringLiteral("client://18/") + uid + QLatin1Char('~') + nick);
    c.insertText(QLatin1Char('"') + nick + QLatin1Char('"'), link);
    c.insertText(QStringLiteral(": "), QTextCharFormat());
    if (isReply) {
        QTextCharFormat italic;
        italic.setFontItalic(true);
        c.insertText(replies::arrow() + QLatin1Char(' '), italic);
        QTextCharFormat author = italic;
        author.setAnchor(true);
        author.setAnchorHref(QStringLiteral("client://17/") + kUidAlice + QStringLiteral("~Alice"));
        c.insertText(QStringLiteral("Alice"), author);
        c.insertText(QStringLiteral(" · 21") + replies::timeColon() + QStringLiteral("14: “"), italic);
        insertBody(c, quoted, italic);
        c.insertText(QStringLiteral("”"), italic);
        c.insertText(QString(QChar(QChar::LineSeparator)), QTextCharFormat());
    }
    insertBody(c, text, QTextCharFormat());
}

QString formatsOf(QTextDocument* doc)
{
    QString out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            const QTextCharFormat cf = f.charFormat();
            out += f.text() + QLatin1Char('|') + (cf.isImageFormat() ? cf.toImageFormat().name() : QString()) + QLatin1Char('|') + cf.anchorHref() + QLatin1Char('|')
                   + QString::number(cf.fontItalic()) + QLatin1Char('\n');
        }
    }
    return out;
}

QTextCharFormat formatAt(QTextDocument* doc, int position)
{
    QTextCursor c(doc);
    c.setPosition(position);
    c.setPosition(position + 1, QTextCursor::KeepAnchor);
    return c.charFormat();
}

// HD emoji pictures in doc (ChatEmoji's, by their format).
int hdCount(QTextDocument* doc)
{
    return ChatEmoji::countEmoji(doc);
}

// The reply in doc collapsed into a reply line; false if there was none to collapse.
bool collapseReplies(QTextDocument* doc)
{
    doc->setTextWidth(600);
    doc->documentLayout()->documentSize();
    bool                             any      = false;
    const QVector<replydoc::Message> messages = replydoc::scan(doc);
    for (int i = messages.size() - 1; i >= 0; --i) {
        if (messages.at(i).hasQuote && !messages.at(i).restyled)
            any = replydoc::collapse(doc, messages.at(i), replydoc::objectPrefix() + QStringLiteral("t.%1").arg(i), QSizeF(240, 13)) || any;
    }
    return any;
}

// Pixels that differ between two pictures of the same size, and the area they are in.
int differingPixels(const QImage& a, const QImage& b)
{
    const QImage x     = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y     = b.convertToFormat(QImage::Format_ARGB32);
    int          count = 0;
    for (int row = 0; row < qMin(x.height(), y.height()); ++row) {
        const QRgb* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const QRgb* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < qMin(x.width(), y.width()); ++col)
            count += p[col] != q[col] ? 1 : 0;
    }
    return count;
}

QRect differingArea(const QImage& a, const QImage& b)
{
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    QRect        area;
    for (int row = 0; row < qMin(x.height(), y.height()); ++row) {
        const QRgb* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
        const QRgb* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
        for (int col = 0; col < qMin(x.width(), y.width()); ++col) {
            if (p[col] != q[col])
                area |= QRect(col, row, 1, 1);
        }
    }
    return area;
}

} // namespace

class TestReplyEmoji : public QObject
{
    Q_OBJECT

  private slots:
    void init()
    {
        Settings::instance() = Settings();
        emoji::setTextPicturesEnabled(true);
    }

    void cleanup()
    {
        Settings::instance() = Settings();
        emoji::setTextPicturesEnabled(true);
    }

    // A reply line and HD emoji keep apart: their formats never pass for each other.
    void formatsDontCollide()
    {
        QTextDocument doc;
        message(&doc, QStringLiteral("Alice"), kUidAlice, QStringLiteral("Hello world"));
        message(&doc, QStringLiteral("Bob"), kUidBob, QStringLiteral("my reply"), QStringLiteral("Hello world"), true);
        const QString before = doc.toPlainText();
        QVERIFY(collapseReplies(&doc));
        const QVector<int> objects = replydoc::objectPositions(&doc);
        QCOMPARE(objects.size(), 1);
        const QTextCharFormat object = formatAt(&doc, objects.first());
        QVERIFY(!emojiformat::isHd(object));
        QCOMPARE(hdCount(&doc), 0);
        // ChatEmoji giving back "its" pictures touches nothing of the reply line's.
        ChatEmoji::restore(&doc);
        QCOMPARE(replydoc::objectPositions(&doc).size(), 1);
        QVERIFY(formatAt(&doc, objects.first()).hasProperty(replydoc::kRunsProperty));
        QCOMPARE(replydoc::restoreAll(&doc), 1);
        QCOMPARE(doc.toPlainText(), before);
        // The ids themselves differ (both are kept in the same chat formats).
        for (const int p : {emojiformat::kOriginalText, emojiformat::kOriginalFormat, emojiformat::kEmoticonName, emojiformat::kJumbo}) {
            QVERIFY(p != replydoc::kObjectProperty && p != replydoc::kRunsProperty);
            QVERIFY(p != replydoc::kOffsetProperty && p != replydoc::kSeparatorProperty);
        }
    }

    // A message with HD emoji (and an emoticon HD emoji replaced) reads back as the text it was.
    void messagesReadBackTheirEmoji()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here: the chat keeps TeamSpeak's rendering");
        QTextDocument doc;
        message(&doc, QStringLiteral("Alice"), kUidAlice, u("Party \xF0\x9F\x8E\x89 tonight |emoticons:smile.svg| \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD ok"));
        const QString plain = replydoc::scan(&doc).first().text;
        ChatEmoji::processDocument(&doc, 1.0);
        QCOMPARE(hdCount(&doc), 3);
        const QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().text, plain);
        QCOMPARE(messages.first().text, u("Party \xF0\x9F\x8E\x89 tonight :) \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD ok"));
        // ... so a reply to it quotes the emoji, and people without the plugin see them.
        replies::Original original;
        original.nick = QStringLiteral("Alice");
        original.uid  = kUidAlice;
        original.text = messages.first().text;
        QVERIFY(replies::quoteLine(original).contains(u("\xF0\x9F\x8E\x89")));
        QVERIFY(replies::makeSnippet(messages.first().text).contains(u("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD")));
    }

    // HD emoji inside a quote line and in the reply: the quote reads back with its emoji, the reply line
    // takes the line out as it was received, and both can be given back in either order, exactly.
    void quoteLineWithHdEmoji_data()
    {
        QTest::addColumn<bool>("emojiFirst");
        QTest::newRow("emoji restored first") << true;
        QTest::newRow("reply restored first") << false;
    }

    void quoteLineWithHdEmoji()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QFETCH(bool, emojiFirst);
        QTextDocument doc;
        message(&doc, QStringLiteral("Alice"), kUidAlice, u("Pizza \xF0\x9F\x8D\x95 or sushi |emoticons:smile.svg|"));
        message(&doc, QStringLiteral("Bob"), kUidBob, u("sushi \xF0\x9F\x8D\xA3 for sure"), u("Pizza \xF0\x9F\x8D\x95 or sushi |emoticons:smile.svg|"), true);
        const QString before        = doc.toPlainText();
        const QString beforeFormats = formatsOf(&doc);

        ChatEmoji::processDocument(&doc, 1.0); // the emoji got there first: also the quote line's
        QCOMPARE(hdCount(&doc), 5);
        QVector<replydoc::Message> messages = replydoc::scan(&doc);
        QCOMPARE(messages.size(), 2);
        QVERIFY(messages.at(1).hasQuote);
        QCOMPARE(messages.at(1).quote.snippet, u("Pizza \xF0\x9F\x8D\x95 or sushi :)"));
        QCOMPARE(messages.at(1).text, u("sushi \xF0\x9F\x8D\xA3 for sure"));

        QVERIFY(collapseReplies(&doc));
        // The quote line left the document as TeamSpeak showed it: none of ChatEmoji's pictures in it.
        QCOMPARE(hdCount(&doc), 3); // Alice's two and the reply's one
        const int  object = replydoc::objectPositions(&doc).value(0, -1);
        QVERIFY(object >= 0);
        const QVariantList runs = formatAt(&doc, object).property(replydoc::kRunsProperty).toList();
        for (int i = 1; i < runs.size(); i += 2)
            QVERIFY(!emojiformat::isHd(qvariant_cast<QTextFormat>(runs.at(i)).toCharFormat()));
        messages = replydoc::scan(&doc);
        QVERIFY(messages.at(1).restyled);
        QCOMPARE(messages.at(1).quote.snippet, u("Pizza \xF0\x9F\x8D\x95 or sushi :)"));
        QCOMPARE(messages.at(1).text, u("sushi \xF0\x9F\x8D\xA3 for sure"));
        // ChatEmoji works on the restyled block again: nothing breaks, the reply line stays.
        ChatEmoji::processDocument(&doc, 1.0);
        QCOMPARE(replydoc::objectPositions(&doc).size(), 1);

        if (emojiFirst) {
            ChatEmoji::restore(&doc);
            QCOMPARE(hdCount(&doc), 0);
            QCOMPARE(replydoc::objectPositions(&doc).size(), 1);
            QCOMPARE(replydoc::restoreAll(&doc), 1);
        } else {
            QCOMPARE(replydoc::restoreAll(&doc), 1);
            QCOMPARE(hdCount(&doc), 3);
            ChatEmoji::restore(&doc);
        }
        QCOMPARE(hdCount(&doc), 0);
        QVERIFY(replydoc::objectPositions(&doc).isEmpty());
        QCOMPARE(doc.toPlainText(), before);
        QCOMPARE(formatsOf(&doc), beforeFormats);
    }

    // Two of TeamSpeak's smileys in a row in a quote line are one piece: both come back.
    void smileysInARowComeBack()
    {
        QTextDocument doc;
        message(&doc, QStringLiteral("Alice"), kUidAlice, QStringLiteral("|emoticons:smile.svg||emoticons:smile.svg|"));
        message(&doc, QStringLiteral("Bob"), kUidBob, QStringLiteral("same"), QStringLiteral("|emoticons:smile.svg||emoticons:smile.svg|"), true);
        const QString before = formatsOf(&doc);
        QCOMPARE(replydoc::scan(&doc).at(1).quote.snippet, QStringLiteral(":):)"));
        QVERIFY(collapseReplies(&doc));
        QCOMPARE(replydoc::restoreAll(&doc), 1);
        QCOMPARE(formatsOf(&doc), before);
    }

    // A reply of nothing but emoji is large once its quote line is out, as in Discord, whichever came first.
    void replyOfOnlyEmojiIsLarge()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QTextDocument doc;
        QFont         font(QStringLiteral("Segoe UI"));
        font.setPixelSize(12);
        doc.setDefaultFont(font);
        message(&doc, QStringLiteral("Alice"), kUidAlice, QStringLiteral("Who won?"));
        message(&doc, QStringLiteral("Bob"), kUidBob, u("\xF0\x9F\x98\x82\xF0\x9F\x98\x82"), QStringLiteral("Who won?"), true);
        ChatEmoji::processDocument(&doc, 1.0); // with the quote line still there: inline
        const QTextBlock reply = doc.findBlockByNumber(1);
        const int        first = reply.position() + reply.text().lastIndexOf(QChar::ObjectReplacementCharacter);
        QVERIFY(formatAt(&doc, first).toImageFormat().width() < 48);
        QVERIFY(collapseReplies(&doc));
        ChatEmoji::processDocument(&doc, 1.0); // the block changed: done again
        const QTextBlock restyled = doc.findBlockByNumber(1);
        const int        last     = restyled.position() + restyled.text().lastIndexOf(QChar::ObjectReplacementCharacter);
        QCOMPARE(formatAt(&doc, last).toImageFormat().width(), 48.0);
        QVERIFY(formatAt(&doc, last).boolProperty(emojiformat::kJumbo));
        QCOMPARE(ChatEmoji::originalText(&doc, restyled.position(), restyled.position() + restyled.length() - 1),
                 u("<21:15:30> \"Bob\": \xF0\x9F\x98\x82\xF0\x9F\x98\x82")); // copying: no stray line for the reply line
        // Given back exactly, both.
        ChatEmoji::restore(&doc);
        QCOMPARE(replydoc::restoreAll(&doc), 1);
        QCOMPARE(hdCount(&doc), 0);
        QVERIFY(doc.findBlockByNumber(1).text().contains(u("\xF0\x9F\x98\x82\xF0\x9F\x98\x82")));
    }

    // The reply line draws the emoji of a name and a snippet as HD pictures (where their glyphs would
    // be); with HD emoji off, and for text without emoji, exactly as text.
    void replyLineDrawsHdEmoji()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        replyart::Header header;
        header.nick      = QStringLiteral("Alice");
        header.snippet   = u("\xF0\x9F\x8D\x95\xF0\x9F\x8D\x95\xF0\x9F\x8D\x95 pizza night \xF0\x9F\x8E\x89");
        header.found     = true;
        header.nickColor = QColor(0x1c, 0xb0, 0xf4);
        replyart::HeaderStyle style;
        style.font = QFont(QStringLiteral("Segoe UI"));
        style.font.setPixelSize(12);
        style.dpr        = 2.0;
        style.lineHeight = 13;
        const QSize  size = replyart::headerSize(header, style, 520);
        const QImage hd   = replyart::renderHeader(header, style, size);
        emoji::setTextPicturesEnabled(false);
        const QImage text = replyart::renderHeader(header, style, size);
        emoji::setTextPicturesEnabled(true);
        QCOMPARE(hd.size(), text.size()); // widths stay what the text measures
        // The name and the words are the same pixels; the emoji differ (Qt draws Windows' flat colour
        // layers at best, the pictures are the Fluent ones).
        const int differing = differingPixels(hd, text);
        QVERIFY2(differing > 300, qPrintable(QString::number(differing)));
        // Where the pictures go: inside the glyphs' room, after the name.
        const QRect changed = differingArea(hd, text);
        const qreal nameEnd = 24.0 + QFontMetricsF(replyart::scaledFont(style.font, 0.92)).horizontalAdvance(header.nick);
        QVERIFY2(changed.left() / 2.0 >= nameEnd, qPrintable(QStringLiteral("%1 < %2").arg(changed.left() / 2.0).arg(nameEnd)));

        // No emoji: the same picture with HD emoji on or off.
        header.snippet = QStringLiteral("pizza night, nine o'clock");
        const QSize  plainSize = replyart::headerSize(header, style, 520);
        const QImage plainOn   = replyart::renderHeader(header, style, plainSize);
        emoji::setTextPicturesEnabled(false);
        const QImage plainOff = replyart::renderHeader(header, style, plainSize);
        emoji::setTextPicturesEnabled(true);
        QCOMPARE(differingPixels(plainOn, plainOff), 0);
    }
};

TSMEDIA_REGISTER_TEST(TestReplyEmoji)

#include "tst_replyemoji.moc"
