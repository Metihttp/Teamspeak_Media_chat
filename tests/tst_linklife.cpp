// 2.4 review (lifetime / chat integration) of the link previews: what the slots under web links do to the rest of
// the chat. Copying (Copy text, Ctrl+C) gives the message's text as TeamSpeak showed it; a slot that changes size
// (a card that becomes the link's media, a sender's preview arriving) keeps the reader's place, also at the chat's
// bottom; a long chat with many links stays cheap to read. TeamSpeak-like documents and widgets (tests/tschat.h),
// made-up people and addresses only, never the network.

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

#include "chatemoji.h"
#include "chatscroll.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "linkpreview/linkcard.h"
#include "linkpreview/linkdoc.h"
#include "linkpreview/linkurl.h"
#include "replies.h"
#include "replydoc.h"
#include "testmain.h"
#include "textreactdoc.h"
#include "tschat.h"

namespace {

using layoutart::Mode;

const QString kAlice = QStringLiteral("q0Xn6Alice+00000/000000000=");
const QString kBob   = QStringLiteral("q0Xn6Bob00000000000000000+=");
const QString kPng   = QStringLiteral("https://cdn.example.org/pics/lake.png?w=800");
const QString kGif   = QStringLiteral("https://media.example.net/cat.gif");
const QString kTube  = QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ");

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

layoutdoc::Env envFor(Mode mode, bool bubbles)
{
    layoutdoc::Env env;
    env.mode         = mode;
    env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
    env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
    env.dpr          = 1.0;
    env.contentWidth = 512;
    env.today        = QDate(2026, 10, 10);
    env.tag          = QStringLiteral("l");
    env.ownNick      = QStringLiteral("Sam");
    env.ownUid       = QStringLiteral("q0Xn6Mine000000000000000000=");
    return env;
}

QString time(int i)
{
    return QStringLiteral("%1:%2:%3").arg(8 + i / 3600, 2, 10, QLatin1Char('0')).arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
}

void message(QTextDocument* doc, int i, const QString& html)
{
    const tschat::Skin s = tschat::darkSkin();
    tschat::append(doc, tschat::messageHtml(s, time(i), 17 + i % 2, i % 2 ? kBob : kAlice, i % 2 ? QStringLiteral("Bob") : QStringLiteral("Alice"), html));
}

QString linkTo(const QString& url)
{
    return tschat::linkHtml(tschat::darkSkin(), url);
}

// A card under every web link, as ChatLinkPreviews puts them (back to front).
int cardAll(QTextDocument& doc, const QSizeF& size = QSizeF(340, 64))
{
    int                          n     = 0;
    const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
    for (int i = links.size() - 1; i >= 0; --i)
        n += linkdoc::insertSlot(&doc, links.at(i), linkdoc::cardPrefix() + QStringLiteral("c%1").arg(i), size) ? 1 : 0;
    return n;
}

// The top of the laid-out line that holds position, in document coordinates (-1: none).
qreal lineTop(QTextDocument* doc, int position)
{
    const QTextBlock b = doc->findBlock(position);
    if (!b.isValid() || !b.layout())
        return -1;
    const QTextLine line = b.layout()->lineForTextPosition(position - b.position());
    if (!line.isValid())
        return -1;
    return doc->documentLayout()->blockBoundingRect(b).top() + line.y();
}

// A chat widget like TeamSpeak's, laid out (the offscreen platform).
struct Chat {
    QTextBrowser browser;
    explicit Chat(int messages, int linkAt = -1, const QString& url = kPng, const QString& after = QString())
    {
        browser.resize(560, 420);
        setup(*browser.document());
        for (int i = 0; i < messages; ++i)
            message(browser.document(), i, i == linkAt ? linkTo(url) + after : QStringLiteral("message %1 about the map").arg(i));
        browser.show();
        settle();
    }
    void settle()
    {
        browser.document()->documentLayout()->documentSize();
        chatscroll::syncRange(&browser);
        QCoreApplication::processEvents();
    }
    QScrollBar* bar() { return browser.verticalScrollBar(); }
};

} // namespace

class TestLinkLife : public QObject
{
    Q_OBJECT

  private slots:
    // ---- copying -------------------------------------------------------------------------------------------
    // "Copy text" (ChatEmoji::originalText, the menu of a message) gives the same text with the slots under the
    // links as without them, in every layout: our line breaks and pictures give nothing.
    void copyTextLeavesSlotsOut()
    {
        for (const Mode mode : {Mode::Classic, Mode::Cozy, Mode::Compact}) {
            QTextDocument with, without;
            for (QTextDocument* d : {&with, &without}) {
                setup(*d);
                message(d, 0, QStringLiteral("look ") + linkTo(kPng) + QStringLiteral(" nice lake"));
                message(d, 1, linkTo(kGif) + QStringLiteral(" and ") + linkTo(kPng) + QStringLiteral(" both"));
                message(d, 2, linkTo(kTube)); // its slot ends the message
                if (mode != Mode::Classic)
                    layoutdoc::styleAll(d, envFor(mode, true));
            }
            QCOMPARE(cardAll(with), 4);
            // A line TeamSpeak adds after a slot at a message's end takes our picture's format over: its text stays.
            for (QTextDocument* d : {&with, &without}) {
                QTextCursor c(d);
                c.movePosition(QTextCursor::End);
                c.insertBlock();
                c.insertText(QStringLiteral("typed after"));
            }
            const QString a = ChatEmoji::originalText(&with, 0, with.characterCount() - 1);
            const QString b = ChatEmoji::originalText(&without, 0, without.characterCount() - 1);
            QVERIFY2(a == b, qPrintable(QStringLiteral("mode %1:\n[%2]\n--- vs ---\n[%3]").arg(static_cast<int>(mode)).arg(a, b)));
            QVERIFY(!a.contains(QChar::ObjectReplacementCharacter));
            QVERIFY(a.endsWith(QStringLiteral("typed after")));
            // One message, from its text to its block's end.
            const QTextBlock first = with.firstBlock();
            const QString    one   = ChatEmoji::originalText(&with, replydoc::parseBlock(first).textStart, first.position() + first.length() - 1);
            QCOMPARE(one, QStringLiteral("look ") + kPng + QStringLiteral(" nice lake"));
        }
    }

    // Ctrl+C over a message with a slot: TeamSpeak's own copy would put our line breaks and the picture's U+FFFC on
    // the clipboard; it is copied as the message's text, like a selection with a text reaction row or a poll card.
    void ctrlCOverASlotCopiesTheText()
    {
        for (const bool withSlot : {true, false}) {
            QTextBrowser   browser;
            QTextDocument* doc = browser.document();
            setup(*doc);
            message(doc, 0, QStringLiteral("look ") + linkTo(kPng) + QStringLiteral(" nice lake"));
            message(doc, 1, QStringLiteral("me"));
            if (withSlot)
                QCOMPARE(cardAll(*doc), 1);
            ChatEmoji chat;
            chat.attach(&browser);
            const QTextBlock first = doc->firstBlock();
            QTextCursor      selection(doc);
            selection.setPosition(replydoc::parseBlock(first).textStart);
            selection.setPosition(first.position() + first.length() - 1, QTextCursor::KeepAnchor);
            browser.setTextCursor(selection);
            QGuiApplication::clipboard()->clear();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
            QApplication::sendEvent(&browser, &press);
            const QString copied = QGuiApplication::clipboard()->text();
            QVERIFY2(copied == QStringLiteral("look ") + kPng + QStringLiteral(" nice lake"),
                     qPrintable(QStringLiteral("slot %1: [%2]").arg(withSlot).arg(copied)));
            QGuiApplication::clipboard()->clear();
        }
    }

    // A reply to a message with a slot quotes it as it would without the slot (replydoc reads our line breaks as
    // spaces; the quote's snippet collapses them) and finds the original again.
    void replyQuotesTheTextWithoutTheSlot()
    {
        QTextDocument with, without;
        for (QTextDocument* d : {&with, &without}) {
            setup(*d);
            message(d, 0, QStringLiteral("look ") + linkTo(kPng) + QStringLiteral(" nice lake"));
        }
        QCOMPARE(cardAll(with), 1);
        const replydoc::Message a = replydoc::parseBlock(with.firstBlock());
        const replydoc::Message b = replydoc::parseBlock(without.firstBlock());
        QCOMPARE(replies::makeSnippet(a.text, replies::kSnippetChars), replies::makeSnippet(b.text, replies::kSnippetChars));
        QCOMPARE(replies::matchKey(a.text), replies::matchKey(b.text));
        QCOMPARE(a.nick, b.nick);
        QCOMPARE(a.minutes, b.minutes);
    }

    // ---- the reader's place ------------------------------------------------------------------------------------
    // A card at the top of the view becomes the link's media (the click loaded it, the sender's preview came):
    // what the reader looks at stays where it is on screen, whether the view's top is in the message's text above
    // the slot, in the slot or in the text below it. (chatscroll::TextPlace: ChatLinkPreviews' edits.)
    void slotGrowingKeepsTheReadersPlace_data()
    {
        QTest::addColumn<int>("into"); // where the view's top is: 0 the message's text, 1 the card, 2 the text below
        QTest::newRow("text above") << 0;
        QTest::newRow("in the card") << 1;
        QTest::newRow("text below") << 2;
    }
    void slotGrowingKeepsTheReadersPlace()
    {
        QFETCH(int, into);
        Chat           chat(60, 30, kPng, QStringLiteral(" nice lake, and a second line of text that wraps in a narrow chat window for sure"));
        QTextDocument* doc = chat.browser.document();
        QCOMPARE(cardAll(*doc), 1);
        chat.settle();
        const linkdoc::Slot slot  = linkdoc::allSlots(doc).first();
        const QTextBlock    block = doc->findBlock(slot.object);
        // The character the view's top is in, and where its line is on screen.
        int watched = block.position() + 3;
        if (into == 1)
            watched = slot.object;
        else if (into == 2)
            watched = block.position() + block.length() - 2;
        const qreal top = lineTop(doc, watched) + (into == 1 ? 20 : 2);
        chat.bar()->setValue(qRound(top));
        QCOMPARE(chat.bar()->value(), qRound(top));
        const qreal before = lineTop(doc, watched) - chat.bar()->value();

        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        QVERIFY(!place.bottom);
        {
            QTextCursor batch(doc);
            batch.beginEditBlock();
            QVERIFY(linkdoc::replaceObject(doc, slot.object, linkdoc::mediaPrefix() + QStringLiteral("0123456789abcdef0123"), QSizeF(400, 308)));
            batch.endEditBlock();
        }
        chatscroll::restoreText(&chat.browser, place);
        QCoreApplication::processEvents();
        const qreal after = lineTop(doc, watched) - chat.bar()->value();
        QVERIFY2(qAbs(after - before) <= 1.0, qPrintable(QStringLiteral("moved from %1 to %2").arg(before).arg(after)));
    }

    // 2.4 auto-load: a card that waits (for our pipeline, the sender), says a size or fails is a strip of one size,
    // so its slot is never edited and the chat doesn't move; when a wait ends in the media (a cache hit, the sender's
    // preview), the reader's place is kept like any slot that grows.
    void waitingAndSizedCardsKeepTheirPlace()
    {
        Chat           chat(60, 30, kPng, QStringLiteral(" nice lake, and a second line of text that wraps in a narrow chat window for sure"));
        QTextDocument* doc = chat.browser.document();
        PreviewStyle   style;
        style.font     = chatFont();
        style.dark     = true;
        style.dpr      = 1.0;
        style.maxWidth = 400;
        linkcard::Card card;
        card.kind          = linkpreview::Kind::Image;
        card.host          = QStringLiteral("cdn.example.org");
        const QSize  strip = linkcard::layout(card, style).size;
        const QSizeF size(strip.width(), strip.height() + 8); // ChatLinkPreviews' gaps above and below
        QCOMPARE(cardAll(*doc, size), 1);
        chat.settle();
        const linkdoc::Slot slot    = linkdoc::allSlots(doc).first();
        const QTextBlock    block   = doc->findBlock(slot.object);
        const int           watched = block.position() + block.length() - 2; // the text below the card
        chat.bar()->setValue(qRound(lineTop(doc, watched)) + 2); // the view's top in the text below the card
        const qreal before   = lineTop(doc, watched) - chat.bar()->value();
        const int   revision = doc->revision();
        // Click -> waiting for the sender -> Click with a size -> waiting for our own pipeline -> Error: one size.
        QVector<linkcard::Card> looks;
        linkcard::Card          c = card;
        c.look                    = linkcard::Look::Loading;
        c.waiting                 = linkcard::Waiting::Sender;
        looks << c;
        c.look    = linkcard::Look::Click;
        c.waiting = linkcard::Waiting::None;
        c.size    = 38LL * 1024 * 1024;
        looks << c;
        c.look    = linkcard::Look::Loading;
        c.waiting = linkcard::Waiting::Own;
        c.size    = -1;
        looks << c;
        c.look    = linkcard::Look::Error;
        c.waiting = linkcard::Waiting::None;
        looks << c;
        for (const linkcard::Card& look : qAsConst(looks)) {
            const QSize s = linkcard::layout(look, style).size;
            QCOMPARE(s, strip);
            // What ChatLinkPreviews::reconcile does: a resize only when the size differs; the same size is no edit.
            QVERIFY(!linkdoc::resizeObject(doc, slot.object, QSizeF(s.width(), s.height() + 8)));
        }
        QCOMPARE(doc->revision(), revision);
        chat.settle();
        QCOMPARE(lineTop(doc, watched) - chat.bar()->value(), before);
        // The wait ends in the media (a cache hit): the slot grows, the reader's place stays.
        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        QVERIFY(!place.bottom);
        {
            QTextCursor batch(doc);
            batch.beginEditBlock();
            QVERIFY(linkdoc::replaceObject(doc, slot.object, linkdoc::mediaPrefix() + QStringLiteral("0123456789abcdef0123"), QSizeF(400, 308)));
            batch.endEditBlock();
        }
        chatscroll::restoreText(&chat.browser, place);
        QCoreApplication::processEvents();
        const qreal after = lineTop(doc, watched) - chat.bar()->value();
        QVERIFY2(qAbs(after - before) <= 1.0, qPrintable(QStringLiteral("moved from %1 to %2").arg(before).arg(after)));
    }

    // The setting goes off (or the plugin): every slot goes, also the ones above the view's top; the line the reader
    // looks at stays where it is on screen.
    void slotsGoingKeepTheReadersPlace()
    {
        Chat           chat(0);
        QTextDocument* doc = chat.browser.document();
        for (int i = 0; i < 80; ++i)
            message(doc, i, i % 4 ? QStringLiteral("message %1 about the map").arg(i) : linkTo(QStringLiteral("https://cdn.example.org/p/%1.png").arg(i)));
        QCOMPARE(cardAll(*doc, QSizeF(400, 308)), 20);
        chat.settle();
        const int watched = doc->findBlockByNumber(50).position() + 2;
        chat.bar()->setValue(qRound(lineTop(doc, watched)) + 3);
        const qreal before = lineTop(doc, watched) - chat.bar()->value();
        QVERIFY(!chatscroll::atBottom(&chat.browser));
        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        {
            QTextCursor batch(doc);
            batch.beginEditBlock();
            QCOMPARE(linkdoc::removeAll(doc), 20);
            batch.endEditBlock();
        }
        chatscroll::restoreText(&chat.browser, place);
        QCoreApplication::processEvents();
        const qreal after = lineTop(doc, watched) - chat.bar()->value();
        QVERIFY2(qAbs(after - before) <= 1.0, qPrintable(QStringLiteral("moved from %1 to %2").arg(before).arg(after)));
    }

    // The chat at its bottom stays there when the last message's card grows (the scroll range of the edited
    // document, not the one QTextEdit learns of a moment later).
    void slotGrowingAtTheBottomStaysAtTheBottom()
    {
        Chat           chat(60, 59, kTube);
        QTextDocument* doc = chat.browser.document();
        chat.bar()->setValue(chat.bar()->maximum());
        QVERIFY(chatscroll::atBottom(&chat.browser));
        const chatscroll::TextPlace place = chatscroll::captureText(&chat.browser);
        QVERIFY(place.bottom);
        QCOMPARE(cardAll(*doc), 1); // a new message's card comes in
        chatscroll::restoreText(&chat.browser, place);
        QCoreApplication::processEvents();
        QCOMPARE(chat.bar()->value(), chat.bar()->maximum());
        const linkdoc::Slot         slot  = linkdoc::allSlots(doc).first();
        const chatscroll::TextPlace again = chatscroll::captureText(&chat.browser);
        QVERIFY(linkdoc::resizeObject(doc, slot.object, QSizeF(400, 300))); // it became the YouTube card
        chatscroll::restoreText(&chat.browser, again);
        QCoreApplication::processEvents();
        QCOMPARE(chat.bar()->value(), chat.bar()->maximum());
    }

    // ---- a long chat ----------------------------------------------------------------------------------------------
    // 4000 messages in Cozy with bubbles, every fifth with a link (800 links): finding the links, putting the slots
    // in and reading them back (what a scan of the chat does) stays cheap; the message keys don't change.
    void longChatWithManyLinks()
    {
        QTextDocument doc;
        setup(doc);
        const tschat::Skin s = tschat::darkSkin();
        tschat::append(&doc, tschat::dayHtml(s, QStringLiteral("10/10/2026"), false));
        for (int i = 0; i < 4000; ++i)
            message(&doc, i, i % 5 ? QStringLiteral("message %1 about the map").arg(i) : linkTo(QStringLiteral("https://cdn.example.org/p/%1.png").arg(i)));
        layoutdoc::styleAll(&doc, envFor(Mode::Cozy, true));
        QElapsedTimer clock;
        clock.start();
        const QVector<linkdoc::Link> links = linkdoc::findLinks(&doc);
        const qint64                 find  = clock.restart();
        QCOMPARE(links.size(), 800);
        QCOMPARE(cardAll(doc), 800);
        const qint64 insert = clock.restart();
        // A later scan: every link finds its slot, nothing to do.
        const QVector<linkdoc::Link> again = linkdoc::findLinks(&doc);
        int                          found = 0;
        for (const linkdoc::Link& l : again)
            found += linkdoc::slotAfter(doc.findBlockByNumber(l.block), l).exists() ? 1 : 0;
        const QVector<linkdoc::Slot> all = linkdoc::allSlots(&doc);
        const qint64                 rescan = clock.restart();
        QCOMPARE(found, 800);
        QCOMPARE(all.size(), 800);
        // The card's geometry for each (ChatLinkPreviews sizes every slot on a scan).
        PreviewStyle style;
        style.font     = chatFont();
        style.dpr      = 1.0;
        style.maxWidth = 400;
        int heights    = 0;
        for (int i = 0; i < all.size(); ++i) {
            linkcard::Card card;
            card.kind = linkpreview::Kind::Image;
            card.host = QStringLiteral("cdn.example.org");
            heights += linkcard::layout(card, style).size.height();
        }
        const qint64 sizes = clock.restart();
        QVERIFY(heights > 0);
        // Every link message's key with one reading of the chat's days, as ChatLinkPreviews reads them on a pass
        // (ChatTextReactions::KeyDays): a message the text reactions haven't read (yet, or again since its slot went
        // in) used to read every day line of the chat for itself, 800 times on a pass here.
        clock.restart();
        const QDate          today(2026, 10, 10);
        const QVector<QDate> dates    = layoutdoc::datesOf(&doc, today);
        const qint64         daysOnce = clock.restart();
        QSet<QString>        keys;
        for (const linkdoc::Link& l : again) {
            const textreactdoc::Identity id = textreactdoc::identify(doc.findBlockByNumber(l.block));
            qint64                       t  = 0;
            keys.insert(textreactdoc::localKeyOf(id, textreactdoc::dayOf(dates.value(l.block), id.seconds, today, 23 * 3600), &t));
        }
        const qint64 keyed = clock.restart();
        QCOMPARE(keys.size(), 800);
        clock.restart();
        QCOMPARE(linkdoc::removeAll(&doc), 800);
        const qint64 removed = clock.restart();
        qInfo("4000 messages, 800 links: find %lld ms, slots in %lld ms, rescan %lld ms, card sizes %lld ms, keys %lld ms (the days once: %lld ms), "
              "all out %lld ms",
              find, insert, rescan, sizes, keyed + daysOnce, daysOnce, removed);
        QVERIFY2(find + rescan + sizes < 1500, "a scan of a long chat must stay cheap");
        QVERIFY2(keyed + daysOnce < 500, "the keys of a long chat's links must stay cheap");
    }
};

TSMEDIA_REGISTER_TEST(TestLinkLife)
#include "tst_linklife.moc"
