// 2.7 (R-DEF-1): the reader's place on TeamSpeak's own text (chatscroll::captureReader / restoreReader), its carry across
// a plugin reload (chatscroll::Carry, a dynamic property on TeamSpeak's chat widget), and the load hold (LoadHold) that
// keeps it while the plugin styles a chat again. TeamSpeak-like chats (tests/tschat.h) in a shown QTextBrowser, styled
// with the real layout (layoutdoc::styleAll + renderAll, Cozy with bubbles and Compact); a card is an image object after
// a line break at a message's end, like the link previews'. Made-up people only.

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPointer>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtTest>

#include <memory>

#include "chatscale.h"
#include "chatscroll.h"
#include "layoutart.h"
#include "layoutdoc.h"
#include "layoutformat.h"
#include "loadhold.h"
#include "showplace.h"
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
    f.setPointSizeF(8.0);
    return f;
}

QString source(const char* relative)
{
    QFile f(QStringLiteral(TSMEDIA_SOURCE_DIR "/") + QString::fromLatin1(relative));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// The lines of a block that hold TeamSpeak's text: their top (document y), height and first such character (in the
// block).
struct ReaderLine {
    qreal top    = 0;
    qreal height = 0;
    int   at     = -1;
};

QVector<ReaderLine> readerLines(QTextDocument* doc, int n)
{
    QVector<ReaderLine> out;
    const QTextBlock    block = doc->findBlockByNumber(n);
    const qreal         top   = doc->documentLayout()->blockBoundingRect(block).top();
    for (int i = 0; block.layout() && i < block.layout()->lineCount(); ++i) {
        const QTextLine line = block.layout()->lineAt(i);
        const int       at   = chatscroll::firstReaderChar(block, line.textStart(), line.textStart() + line.textLength());
        if (at >= 0)
            out.append({top + line.y(), line.height(), at});
    }
    return out;
}

qreal blockTop(QTextDocument* doc, int n)
{
    return doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(n)).top();
}

// TeamSpeak's message icons are resources of its chat (else QTextBrowser looks for files on every layout).
void addIcons(QTextDocument* doc)
{
    for (const char* name : {"MESSAGE_INCOMING", "MESSAGE_OUTGOING", "MESSAGE_INFO"}) {
        QImage icon(13, 13, QImage::Format_ARGB32_Premultiplied);
        icon.fill(QColor(0x43, 0x8b, 0xe8));
        doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("iconpath:%1?size=13x13").arg(QString::fromLatin1(name))), icon);
    }
}

// A TeamSpeak chat: message i is block i, Alice and Bob in turn (every message gets its own Cozy head), every sixth one
// long enough to take several lines.
struct Chat {
    QTextBrowser        browser;
    layoutdoc::Env      env;
    QVector<QPair<int, int>> cards; // block, height
    int                 serial = 0;

    explicit Chat(int messages)
    {
        browser.resize(520, 400);
        browser.setFont(chatFont());
        addIcons(doc());
        for (int i = 0; i < messages; ++i)
            append(i);
        browser.show();
        layoutNow();
    }

    QTextDocument* doc() { return browser.document(); }
    QScrollBar*    bar() { return browser.verticalScrollBar(); }

    void append(int i)
    {
        const tschat::Skin s    = tschat::darkSkin();
        const bool         bob  = i % 2 == 1;
        const QString      time = QStringLiteral("16:%1:%2").arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
        const QString      body = i % 6 == 3 ? QStringLiteral("rp %1 a long message that wraps over several lines in this chat, so that its bubble is "
                                                              "taller than the others and a reader can stop in the middle of it, which happens "
                                                              "more often than one would think when a chat is busy; and then it goes on for a while longer, with a few more "
                                                              "words about nothing in particular, so it takes five or six lines at any width")
                                                   .arg(i)
                                             : QStringLiteral("rp %1 short line").arg(i);
        tschat::append(doc(), tschat::messageHtml(s, time, bob ? 19 : 17, bob ? kBob : kAlice, bob ? QStringLiteral("Bob") : QStringLiteral("Alice"), body));
    }

    void layoutNow()
    {
        doc()->documentLayout()->documentSize();
        chatscroll::syncRange(&browser);
        QCoreApplication::processEvents();
    }

    void style(Mode mode, bool bubbles)
    {
        env              = layoutdoc::Env();
        env.mode         = mode;
        env.tokens       = layoutart::tokensFor(mode, chatFont(), false, bubbles && mode == Mode::Cozy);
        env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
        env.dpr          = 1.0;
        env.contentWidth = browser.viewport()->width() - 2 * doc()->documentMargin();
        env.today        = QDate(2026, 11, 15);
        env.tag          = QStringLiteral("rp");
        env.ownNick      = QStringLiteral("Sam");
        env.ownUid       = kMe;
        layoutdoc::styleAll(doc(), env);
        layoutdoc::renderAll(doc(), env, env.today);
        layoutNow();
    }

    void unstyle()
    {
        layoutdoc::restoreAll(doc());
        layoutNow();
    }

    // A card under message n: a line break and an image object at its end (like a link preview's card).
    void addCard(int n, int height, bool remember = true)
    {
        const QTextBlock block = doc()->findBlockByNumber(n);
        QTextCursor      c(doc());
        c.setPosition(block.position() + block.length() - 1);
        const QString name = QStringLiteral("tsmedia-test-card:%1").arg(++serial);
        QImage        image(300, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        doc()->addResource(QTextDocument::ImageResource, QUrl(name), image);
        QTextImageFormat f;
        f.setName(name);
        f.setWidth(300);
        f.setHeight(height);
        c.insertText(QString(QChar::LineSeparator));
        c.insertImage(f);
        if (remember)
            cards.append({n, height});
    }

    void removeCards()
    {
        QVector<int> at;
        for (QTextBlock b = doc()->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (f.isValid() && f.charFormat().isImageFormat() && f.charFormat().toImageFormat().name().startsWith(QLatin1String("tsmedia-test-card:")))
                    at.append(f.position());
            }
        }
        for (int i = at.size() - 1; i >= 0; --i) {
            QTextCursor c(doc());
            c.setPosition(at.at(i) - 1);
            c.setPosition(at.at(i) + 1, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        layoutNow();
    }

    void addCardsAgain()
    {
        for (const auto& card : qAsConst(cards))
            addCard(card.first, card.second, false);
        layoutNow();
    }

    void setTop(qreal y)
    {
        chatscroll::syncRange(&browser);
        bar()->setValue(qRound(y));
    }
};

// A long plain chat for the hold (fast to build): line i is block i, every seventh one long.
void fillPlain(QTextBrowser& browser, int lines)
{
    browser.resize(520, 400);
    browser.setFont(chatFont());
    QTextCursor c(browser.document());
    for (int i = 0; i < lines; ++i) {
        if (i > 0)
            c.insertBlock();
        c.insertText(i % 7 == 3 ? QStringLiteral("hold %1 a long line that wraps over a few lines in this chat, so that a reader can stop in "
                                                 "the middle of it, which is what the line anchor is for; more words here, and then some more words after "
                                                 "those, so that it takes four lines or more with any font the test machine has")
                                      .arg(i)
                                : QStringLiteral("hold %1 short").arg(i));
    }
    browser.show();
    browser.document()->documentLayout()->documentSize();
    chatscroll::syncRange(&browser);
    QCoreApplication::processEvents();
}

void insertPicture(QTextDocument* doc, int position, int height, int* serial)
{
    const QString name = QStringLiteral("tsmedia-test-pic:%1").arg(++*serial);
    QImage        image(200, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::darkMagenta);
    doc->addResource(QTextDocument::ImageResource, QUrl(name), image);
    QTextImageFormat f;
    f.setName(name);
    f.setWidth(200);
    f.setHeight(height);
    QTextCursor c(doc);
    c.setPosition(position);
    c.insertImage(f);
    c.insertText(QString(QChar::LineSeparator));
}

void hideBlocks(QTextDocument* doc, int from, int to)
{
    for (int n = from; n <= to; ++n) {
        QTextBlock b = doc->findBlockByNumber(n);
        b.setVisible(false);
        doc->markContentsDirty(b.position(), b.length());
    }
}

int trueMaximum(QTextBrowser& b)
{
    return qMax(0, qRound(b.document()->documentLayout()->documentSize().height()) - b.viewport()->height());
}

// 2.7.1 (issue 3): TeamSpeak's chat tabs, one shown and a new chat filled while it is hidden the way TeamSpeak fills a chat
// (QTextEdit::append: the view follows the end, the widget's cursor stays at the end), styled by the layout (Cozy or
// Compact), then shown for the first time.
struct NewChat {
    enum class Ending { Run, Album, TallCard, Plain };

    QTabWidget     tabs;
    QTextBrowser*  fresh = nullptr; // the new chat
    layoutdoc::Env env;
    int            serial = 0;

    explicit NewChat(QSize size = QSize(520, 400))
    {
        tabs.resize(size);
        auto* server = new QTextBrowser;
        server->append(QStringLiteral("Connected to Server"));
        tabs.addTab(server, QStringLiteral("Server"));
        tabs.show();
        QCoreApplication::processEvents();
        fresh = new QTextBrowser; // made without a parent, like TeamSpeak's (640 x 480), then added as a hidden tab
        fresh->setFont(chatFont());
        addIcons(fresh->document());
        tabs.addTab(fresh, QStringLiteral("Channel"));
        QCoreApplication::processEvents();
    }

    QTextDocument* doc() { return fresh->document(); }
    QScrollBar*    bar() { return fresh->verticalScrollBar(); }

    // TeamSpeak's hidden chats are laid out at their own size (640 x 480) and their range follows each append; the hidden
    // tab here has its resize pending until its Show (range 0/0), unless this lays it out (review fix tests).
    void layOutHidden()
    {
        QResizeEvent outer(fresh->size(), QSize());
        QCoreApplication::sendEvent(fresh, &outer);
        QResizeEvent inner(fresh->viewport()->size(), QSize());
        QCoreApplication::sendEvent(fresh->viewport(), &inner);
    }

    void fill(int messages, Ending ending)
    {
        const tschat::Skin s = tschat::darkSkin();
        fresh->append(tschat::historyHtml(s, QStringLiteral("2026-11-14 20:10:00")));
        for (int i = 0; i < messages; ++i) {
            const bool    bob  = i % 2 == 1;
            const QString time = QStringLiteral("20:%1:%2").arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'));
            const QString body = i % 6 == 3 ? QStringLiteral("nc %1 a longer message that wraps over more than one line in this chat, so the chat is "
                                                             "taller than its view several times over")
                                                  .arg(i)
                                            : QStringLiteral("nc %1 short line").arg(i);
            fresh->append(tschat::messageHtml(s, time, bob ? 19 : 17, bob ? kBob : kAlice, bob ? QStringLiteral("Bob") : QStringLiteral("Alice"), body));
        }
        const QString talkA = QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, 1, QStringLiteral("Lobby"));
        const QString talkB = QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, 2, QStringLiteral("Games"));
        if (ending == Ending::Run) { // the history's last sessions (channel switches), then this one: five events in a row
            const QString talkC = QStringLiteral("You are now talking in channel: ") + tschat::channelHtml(s, 3, QStringLiteral("Music"));
            fresh->append(tschat::systemHtml(s, QStringLiteral("21:00:00"), talkA));
            fresh->append(tschat::systemHtml(s, QStringLiteral("21:05:00"), talkB));
            fresh->append(tschat::systemHtml(s, QStringLiteral("21:09:00"), talkC));
            fresh->append(tschat::historyEndHtml(s));
            fresh->append(tschat::systemHtml(s, QStringLiteral("09:00:01"), talkA));
            return;
        }
        // An album's three messages (the album layout folds the later two under the first), or three plain ones.
        for (int k = 0; k < 3; ++k)
            fresh->append(tschat::messageHtml(s, QStringLiteral("21:00:0%1").arg(k), 17, kAlice, QStringLiteral("Alice"), QStringLiteral("nc album %1 of 3").arg(k + 1)));
    }

    void style(Mode mode)
    {
        env              = layoutdoc::Env();
        env.mode         = mode;
        env.tokens       = layoutart::tokensFor(mode, chatFont(), false, mode == Mode::Cozy);
        env.colors       = layoutart::colorsFor(true, QColor(0x2f, 0x31, 0x36), QColor(0xdc, 0xdd, 0xde), QColor(0x1c, 0xb0, 0xf4));
        env.dpr          = 1.0;
        env.contentWidth = fresh->viewport()->width() - 2 * doc()->documentMargin();
        env.today        = QDate(2026, 11, 15);
        env.tag          = QStringLiteral("nc");
        env.ownNick      = QStringLiteral("Sam");
        env.ownUid       = kMe;
        env.collapse     = true;
        layoutdoc::styleAll(doc(), env);
        layoutdoc::renderAll(doc(), env, env.today);
    }

    // The layout's runs of events (ChatLayout::collapseRuns' planning and edits): a run at the chat's end hides its last
    // rows.
    void collapse()
    {
        for (const layoutdoc::RunPlan& run : layoutdoc::planRuns(doc(), 0, doc()->blockCount() - 1, QSet<int>(), 0))
            layoutdoc::applyRun(doc(), run, env, &serial);
    }

    // A card under the last message (a link preview's picture), taller than the view.
    void tallCard(int height)
    {
        const QTextBlock block = doc()->lastBlock();
        QTextCursor      c(doc());
        c.setPosition(block.position() + block.length() - 1);
        const QString name = QStringLiteral("tsmedia-test-card:%1").arg(++serial);
        QImage        image(300, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        doc()->addResource(QTextDocument::ImageResource, QUrl(name), image);
        QTextImageFormat f;
        f.setName(name);
        f.setWidth(300);
        f.setHeight(height);
        c.insertText(QString(QChar::LineSeparator));
        c.insertImage(f);
    }

    // What TeamSpeak's own appends and TS Media's place keeping leave a hidden chat with: at its end.
    void toEnd()
    {
        chatscroll::toBottom(fresh);
        QCoreApplication::processEvents();
    }

    void show()
    {
        tabs.setCurrentIndex(1);
        QCoreApplication::processEvents();
    }

    int lastShownBlock()
    {
        for (QTextBlock b = doc()->lastBlock(); b.isValid(); b = b.previous()) {
            if (b.isVisible())
                return b.blockNumber();
        }
        return -1;
    }
};

// A test filter in front of everything (like dark mode's application filter): at the chat's Show it puts the view at
// the top of a block it kept.
struct AtShow : QObject {
    QTextBrowser* browser = nullptr;
    int           block   = -1;
    bool          eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == browser && event->type() == QEvent::Show) {
            chatscroll::syncRange(browser);
            const QTextBlock b = browser->document()->findBlockByNumber(block);
            browser->verticalScrollBar()->setValue(qRound(browser->document()->documentLayout()->blockBoundingRect(b).top()));
        }
        return false;
    }
};

// ChatZoom's Show handler (chatzoom.cpp): the saved zoom for a chat hidden through a zoom change, set before its first
// paint. Records whether the chat was at its end then.
struct ZoomAtShow : QObject {
    QTextBrowser* browser = nullptr;
    QFont         font;
    bool          set     = false;
    bool          atEnd   = false;
    int           value   = -1;
    int           maximum = -1;
    bool          eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == browser && event->type() == QEvent::Show && browser->font() != font) {
            atEnd   = chatscroll::atBottom(browser);
            value   = browser->verticalScrollBar()->value();
            maximum = browser->verticalScrollBar()->maximum();
            browser->setFont(font);
            set = true;
        }
        return false;
    }
};

// Whether the chat is at its end when the Show reaches this filter; with sync, the chat's range is brought up to date
// first (as a module that lays the chat out at its Show would: QTextEdit takes a new font's range only a turn later).
struct SeenAtShow : QObject {
    QTextBrowser* browser = nullptr;
    bool          sync    = false;
    bool          atEnd   = false;
    bool          eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == browser && event->type() == QEvent::Show) {
            if (sync)
                chatscroll::syncRange(browser);
            atEnd = chatscroll::atBottom(browser);
        }
        return false;
    }
};

} // namespace

class TestReaderPlace : public QObject
{
    Q_OBJECT

  private slots:
    // ---- 4.1 the capture -------------------------------------------------------------------------------------------

    // The view's top in the gap above a message, or at its top: that message, the offset kept as it is.
    void captureInTheGap_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("offset");
        QTest::newRow("cozy, 5 px above (the hit in the block above)") << int(Mode::Cozy) << -5;
        QTest::newRow("cozy, at its top (the hit in the block)") << int(Mode::Cozy) << 0;
        QTest::newRow("compact, 5 px above") << int(Mode::Compact) << -5;
        QTest::newRow("compact, at its top") << int(Mode::Compact) << 0;
        QTest::newRow("raw, 5 px above") << -1 << -5;
    }
    void captureInTheGap()
    {
        QFETCH(int, mode);
        QFETCH(int, offset);
        Chat chat(120);
        if (mode >= 0)
            chat.style(static_cast<Mode>(mode), true);
        chat.setTop(blockTop(chat.doc(), 60) + offset);
        QCOMPARE(chat.bar()->value(), qRound(blockTop(chat.doc(), 60)) + offset);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.bottom);
        QVERIFY(!p.line);
        QCOMPARE(p.cursor.block().blockNumber(), 60);
        QCOMPARE(p.cursor.position(), chat.doc()->findBlockByNumber(60).position());
        QVERIFY2(qAbs(p.offset - (chat.bar()->value() - blockTop(chat.doc(), 60))) < 0.01, qPrintable(QString::number(p.offset)));
        QCOMPARE(qRound(chatscroll::readerY(chat.doc(), p) + p.offset), chat.bar()->value());
    }

    // The view's top inside a Cozy head row (our picture above the message's first text line): the message.
    void captureInTheHeadRow()
    {
        Chat chat(120);
        chat.style(Mode::Cozy, true);
        const QVector<ReaderLine> lines = readerLines(chat.doc(), 60);
        QVERIFY(!lines.isEmpty());
        const qreal top = blockTop(chat.doc(), 60);
        QVERIFY2(lines.first().top - top >= 8, "a Cozy head row above the first text line");
        chat.setTop(top + 4);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.line);
        QCOMPARE(p.cursor.block().blockNumber(), 60);
        QVERIFY(qAbs(p.offset - (chat.bar()->value() - top)) < 0.01);
        QVERIFY(p.offset > 0);
    }

    // The view's top on the third text line of a long message: that line, on TeamSpeak's first character there.
    void captureOnALaterLine_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("cozy") << int(Mode::Cozy);
        QTest::newRow("compact") << int(Mode::Compact);
        QTest::newRow("raw") << -1;
    }
    void captureOnALaterLine()
    {
        QFETCH(int, mode);
        Chat chat(120);
        if (mode >= 0)
            chat.style(static_cast<Mode>(mode), true);
        const QVector<ReaderLine> lines = readerLines(chat.doc(), 63);
        QVERIFY2(lines.size() >= 4, qPrintable(QString::number(lines.size())));
        chat.setTop(lines.at(2).top + 2);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(p.line);
        QCOMPARE(p.cursor.position(), chat.doc()->findBlockByNumber(63).position() + lines.at(2).at);
        QVERIFY(qAbs(p.offset - (chat.bar()->value() - lines.at(2).top)) < 0.01);
        QCOMPARE(qRound(chatscroll::readerY(chat.doc(), p) + p.offset), chat.bar()->value());
    }

    // Our tail under a message (a card) or less than half of its last line in view: the next message, above the view.
    void captureSkipsOurTail_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QByteArray>("where");
        for (int mode : {int(Mode::Cozy), int(Mode::Compact), -1}) {
            const QByteArray m = mode == int(Mode::Cozy) ? "cozy" : mode == int(Mode::Compact) ? "compact" : "raw";
            QTest::newRow(m + ", inside a card") << mode << QByteArray("card");
            QTest::newRow(m + ", a quarter of the last line") << mode << QByteArray("quarter");
        }
    }
    void captureSkipsOurTail()
    {
        QFETCH(int, mode);
        QFETCH(QByteArray, where);
        Chat chat(120);
        if (mode >= 0)
            chat.style(static_cast<Mode>(mode), true);
        if (where == "card") {
            chat.addCard(60, 180);
            chat.layoutNow();
            const QTextBlock block = chat.doc()->findBlockByNumber(60);
            const QTextLine  last  = block.layout()->lineAt(block.layout()->lineCount() - 1);
            chat.setTop(blockTop(chat.doc(), 60) + last.y() + last.height() - 40); // 40 px above the card's bottom (the next text in view)
        } else {
            const QVector<ReaderLine> lines = readerLines(chat.doc(), 60);
            chat.setTop(lines.last().top + lines.last().height * 0.75);
        }
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QString                       lines; // what the view shows, if it fails
        for (int n = 60; n <= 61; ++n) {
            const QTextBlock block = chat.doc()->findBlockByNumber(n);
            for (int i = 0; i < block.layout()->lineCount(); ++i)
                lines += QStringLiteral(" %1.%2@%3+%4").arg(n).arg(i).arg(blockTop(chat.doc(), n) + block.layout()->lineAt(i).y()).arg(block.layout()->lineAt(i).height());
        }
        const QString what = QStringLiteral("view %1+%2, lines%3").arg(chat.bar()->value()).arg(chat.browser.viewport()->height()).arg(lines);
        QVERIFY2(!p.line, qPrintable(what));
        QVERIFY2(p.cursor.block().blockNumber() == 61, qPrintable(what));
        QVERIFY2(p.offset <= 0, qPrintable(QString::number(p.offset)));
        QCOMPARE(qRound(chatscroll::readerY(chat.doc(), p) + p.offset), chat.bar()->value());
    }

    // A hidden message at the view's top (an album's later message, a folded run of events) is never the place.
    void captureSkipsHiddenBlocks()
    {
        Chat chat(120);
        hideBlocks(chat.doc(), 60, 60);
        chat.layoutNow();
        chat.setTop(blockTop(chat.doc(), 61) - 3);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.line);
        QCOMPARE(p.cursor.block().blockNumber(), 61);
        QVERIFY(qAbs(p.offset + 3) < 1.01);
    }

    // The view shows only one big picture (no line of TeamSpeak's text): the message at the view's top.
    void captureOnlyAPicture()
    {
        Chat chat(120);
        chat.addCard(60, 600);
        chat.layoutNow();
        const QTextBlock block = chat.doc()->findBlockByNumber(60);
        const QTextLine  last  = block.layout()->lineAt(block.layout()->lineCount() - 1);
        const qreal      top   = blockTop(chat.doc(), 60);
        chat.setTop(top + last.y() + 50);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.line);
        QCOMPARE(p.cursor.block().blockNumber(), 60);
        QVERIFY(qAbs(p.offset - (chat.bar()->value() - top)) < 0.01);
    }

    // At the end, the layout's room under the last line counted: the end.
    void captureAtTheEnd()
    {
        Chat chat(60);
        chat.style(Mode::Cozy, true);
        layoutdoc::applyEndGap(chat.doc(), chat.env);
        chat.layoutNow();
        const int extra = qCeil(layoutdoc::endGapExtra(chat.doc()));
        QVERIFY(extra > 0);
        chat.setTop(chat.bar()->maximum());
        QVERIFY(chatscroll::captureReader(&chat.browser).bottom);
        chat.setTop(chat.bar()->maximum() - 4 - extra);
        QVERIFY(chatscroll::captureReader(&chat.browser).bottom);
        chat.setTop(chat.bar()->maximum() - 30 - extra);
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.bottom);
        QVERIFY(!p.cursor.isNull());
    }

    // The place read on the styled chat survives the unload (the layout gives the chat back, the cards go) and the
    // load (styled again, the cards back): back where it was, to the pixel.
    void roundTripThroughTheLayout_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QByteArray>("where");
        for (int mode : {int(Mode::Cozy), int(Mode::Compact)}) {
            for (const char* where : {"gap", "head", "line", "card"})
                QTest::newRow(qPrintable(QStringLiteral("%1, %2").arg(mode == int(Mode::Cozy) ? QStringLiteral("cozy bubbles") : QStringLiteral("compact"),
                                                                      QLatin1String(where))))
                    << mode << QByteArray(where);
        }
    }
    void roundTripThroughTheLayout()
    {
        QFETCH(int, mode);
        QFETCH(QByteArray, where);
        Chat chat(120);
        chat.style(static_cast<Mode>(mode), true);
        for (int n = 2; n < 120; n += 4)
            chat.addCard(n, n % 8 == 2 ? 160 : 90);
        chat.layoutNow();
        int n = 60;
        if (where == "gap") {
            chat.setTop(blockTop(chat.doc(), 60) - 5);
        } else if (where == "head") {
            chat.setTop(blockTop(chat.doc(), 60) + 3);
        } else if (where == "line") {
            n                               = 63;
            const QVector<ReaderLine> lines = readerLines(chat.doc(), 63);
            QVERIFY(lines.size() >= 4);
            chat.setTop(lines.at(2).top + 2);
        } else {
            n                      = 62; // a card under it
            const QTextBlock block = chat.doc()->findBlockByNumber(62);
            const QTextLine  last  = block.layout()->lineAt(block.layout()->lineCount() - 1);
            chat.setTop(blockTop(chat.doc(), 62) + last.y() + 30);
        }
        const int                     before = chat.bar()->value();
        const chatscroll::ReaderPlace p      = chatscroll::captureReader(&chat.browser);
        QVERIFY(!p.cursor.isNull());
        const int     anchorBlock = p.cursor.block().blockNumber();
        const QString anchorText  = chat.doc()->findBlockByNumber(anchorBlock).text().mid(p.cursor.position() - p.cursor.block().position(), 12);
        QCOMPARE(p.line, where == "line");
        QCOMPARE(anchorBlock, where == "card" ? n + 1 : n);

        // The unload: the layout gives every block back, then the cards go; the place back once.
        chat.unstyle();
        chat.removeCards();
        chatscroll::restoreReader(&chat.browser, p);
        QCOMPARE(p.cursor.block().blockNumber(), anchorBlock);
        if (p.line)
            QCOMPARE(chat.doc()->findBlockByNumber(anchorBlock).text().mid(p.cursor.position() - p.cursor.block().position(), 12), anchorText);
        QVERIFY(chat.bar()->value() > 0 && chat.bar()->value() < chat.bar()->maximum());
        QVERIFY(qAbs(chat.bar()->value() - (chatscroll::readerY(chat.doc(), p) + p.offset)) <= 1);

        // The load: styled again, the cards back; the place back once: the styled view exactly as it was.
        chat.style(static_cast<Mode>(mode), true);
        chat.addCardsAgain();
        chatscroll::restoreReader(&chat.browser, p);
        QCOMPARE(p.cursor.block().blockNumber(), anchorBlock);
        QVERIFY2(qAbs(chat.bar()->value() - before) <= 1, qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(chat.bar()->value())));
    }

    // At the end on a range Qt hasn't caught up with (a new width laid out lazily, a big picture appended): the restore
    // syncs the range first and lands on the real end.
    void restoreReaderSyncsTheRange()
    {
        Chat chat(150);
        chat.setTop(chat.bar()->maximum());
        const chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QVERIFY(p.bottom);
        chat.browser.resize(460, 400);
        chat.addCard(149, 400, false);
        chatscroll::restoreReader(&chat.browser, p);
        QCOMPARE(chat.bar()->value(), chat.bar()->maximum());
        QCOMPARE(chat.bar()->maximum(), trueMaximum(chat.browser));
    }

    // ---- 4.1 the carry -----------------------------------------------------------------------------------------------

    void carryRoundTrip()
    {
        chatscroll::Carry c;
        c.bottom      = false;
        c.line        = true;
        c.pos         = 12345;
        c.inBlock     = 17;
        c.blockLength = 230;
        c.blockHash   = 4000000001u;
        c.fromEnd     = 98765;
        c.offset      = -12.25;
        c.rawOffset   = 7.5;
        c.doc         = Q_UINT64_C(0x00007ffde1234560);
        const QByteArray bytes = chatscroll::encodeCarry(c);
        QVERIFY(bytes.startsWith("r1 "));
        chatscroll::Carry d;
        QVERIFY(chatscroll::decodeCarry(bytes, &d));
        QCOMPARE(d.bottom, c.bottom);
        QCOMPARE(d.line, c.line);
        QCOMPARE(d.pos, c.pos);
        QCOMPARE(d.inBlock, c.inBlock);
        QCOMPARE(d.blockLength, c.blockLength);
        QCOMPARE(d.blockHash, c.blockHash);
        QCOMPARE(d.fromEnd, c.fromEnd);
        QCOMPARE(d.offset, c.offset);
        QCOMPARE(d.rawOffset, c.rawOffset);
        QCOMPARE(d.doc, c.doc);
        // Nothing worth carrying: no encoding.
        chatscroll::Carry none;
        QVERIFY(chatscroll::encodeCarry(none).isEmpty());
        none.bottom = true;
        QVERIFY(!chatscroll::encodeCarry(none).isEmpty());
        // Refused: empty, another version, garbage, a field short or too many.
        QVERIFY(!chatscroll::decodeCarry(QByteArray(), &d));
        QByteArray other = bytes;
        other[1]         = '2';
        QVERIFY(!chatscroll::decodeCarry(other, &d));
        QVERIFY(!chatscroll::decodeCarry(QByteArray("r1 x y z"), &d));
        QVERIFY(!chatscroll::decodeCarry(QByteArray("r1 1 0 5 0 10 7 9 0.0 0.0"), &d));
        QVERIFY(!chatscroll::decodeCarry(bytes + " 1", &d));
        QVERIFY(!chatscroll::decodeCarry(QByteArray("r1 2 0 5 0 10 7 9 0.0 0.0 1"), &d));
        QVERIFY(!chatscroll::decodeCarry(QByteArray("r1 0 0 5 0 10 7 9 nan 0.0 1"), &d));
        QVERIFY(!chatscroll::decodeCarry(QByteArray("\xff\xfe garbage \x01"), &d));
    }

    void carryResolves_data()
    {
        QTest::addColumn<QByteArray>("change");
        QTest::addColumn<bool>("resolves");
        QTest::newRow("unchanged") << QByteArray("none") << true;
        QTest::newRow("text appended") << QByteArray("append") << true;
        QTest::newRow("first 20 blocks removed, the value left") << QByteArray("trim") << false;
        QTest::newRow("first 20 blocks removed, the view kept") << QByteArray("trimkept") << true;
        QTest::newRow("the anchor's text changed") << QByteArray("text") << false;
        QTest::newRow("another document") << QByteArray("document") << false;
        QTest::newRow("scrolled 3 px") << QByteArray("move") << false;
        QTest::newRow("at the end") << QByteArray("end") << true;
        QTest::newRow("at the end, then scrolled up") << QByteArray("endup") << false;
    }
    void carryResolves()
    {
        QFETCH(QByteArray, change);
        QFETCH(bool, resolves);
        Chat       chat(120);
        const bool atEnd = change.startsWith("end");
        if (atEnd)
            chat.setTop(chat.bar()->maximum());
        else
            chat.setTop(blockTop(chat.doc(), 60) - 5);
        chatscroll::ReaderPlace p = chatscroll::captureReader(&chat.browser);
        QCOMPARE(p.bottom, atEnd);
        chatscroll::restoreReader(&chat.browser, p);
        chatscroll::Carry carry = chatscroll::carryOf(&chat.browser, p);
        if (!atEnd) {
            QCOMPARE(carry.rawOffset, -5.0);
            carry.offset = -9.5; // as read in our look (what the load must restore)
        }
        QVERIFY(chatscroll::decodeCarry(chatscroll::encodeCarry(carry), &carry));
        p = chatscroll::ReaderPlace(); // the unload's cursor is gone with the plugin

        std::unique_ptr<QTextDocument> keepAlive; // a replaced document stays alive (its address is never reused)
        if (change == "append") {
            for (int i = 120; i < 125; ++i)
                chat.append(i);
            chat.layoutNow();
        } else if (change == "trim" || change == "trimkept") {
            const qreal removed = blockTop(chat.doc(), 20) - blockTop(chat.doc(), 0);
            const int   value   = chat.bar()->value();
            QTextCursor c(chat.doc());
            c.setPosition(0);
            c.setPosition(chat.doc()->findBlockByNumber(20).position(), QTextCursor::KeepAnchor);
            c.removeSelectedText();
            chat.layoutNow();
            chat.bar()->setValue(change == "trimkept" ? qRound(value - removed) : value);
        } else if (change == "text") {
            QTextCursor c(chat.doc());
            c.setPosition(chat.doc()->findBlockByNumber(60).position() + 20);
            c.insertText(QStringLiteral("x"));
            chat.layoutNow();
        } else if (change == "document") {
            auto* other = new QTextDocument(&chat.browser);
            addIcons(other);
            other->setDefaultFont(chat.doc()->defaultFont());
            other->setHtml(chat.doc()->toHtml());
            keepAlive.reset(chat.doc());
            keepAlive->setParent(nullptr);
            chat.browser.setDocument(other);
            chat.layoutNow();
            chat.setTop(blockTop(chat.doc(), 60) - 5);
        } else if (change == "move") {
            chat.bar()->setValue(chat.bar()->value() + 3);
        } else if (change == "endup") {
            chat.bar()->setValue(chat.bar()->maximum() - 100);
        }

        chatscroll::ReaderPlace out;
        QCOMPARE(chatscroll::resolveCarry(&chat.browser, carry, &out), resolves);
        if (!resolves)
            return;
        QCOMPARE(out.bottom, atEnd);
        if (atEnd)
            return;
        QCOMPARE(out.offset, -9.5);
        QVERIFY(!out.line);
        QVERIFY(out.cursor.block().text().contains(QLatin1String("rp 60 ")));
    }

    // ---- 4.2 LoadHold ------------------------------------------------------------------------------------------------

    // A chat at its end stays at its end through edits below and above it, hidden blocks, a lazy relayout and a module
    // that sets a wrong value right after its edit.
    void holdKeepsTheEnd()
    {
        QTextBrowser browser;
        fillPlain(browser, 3000);
        QScrollBar* bar = browser.verticalScrollBar();
        bar->setValue(bar->maximum());
        const chatscroll::ReaderPlace place = chatscroll::captureReader(&browser);
        QVERIFY(place.bottom);
        LoadHold hold(&browser, place, [] { return true; }, nullptr);
        QTextDocument* doc    = browser.document();
        int            serial = 0;
        for (int turn = 0; turn < 10; ++turn) {
            switch (turn % 5) {
            case 0: // a picture below (at the end)
                insertPicture(doc, doc->lastBlock().position(), 400, &serial);
                break;
            case 1: // a picture above
                insertPicture(doc, doc->findBlockByNumber(100 + turn).position(), 400, &serial);
                break;
            case 2: // blocks hidden above
                hideBlocks(doc, 200 + turn * 10, 205 + turn * 10);
                break;
            case 3: { // a narrower chat (laid out lazily), then a line appended
                browser.resize(turn == 3 ? 460 : 520, 400);
                QTextCursor c(doc);
                c.movePosition(QTextCursor::End);
                c.insertBlock();
                c.insertText(QStringLiteral("hold appended %1").arg(turn));
                break;
            }
            default: { // an edit, then a module's wrong value in the same turn
                QTextCursor c(doc);
                c.movePosition(QTextCursor::End);
                c.insertText(QStringLiteral(" more"));
                bar->setValue(bar->maximum() - 300);
                break;
            }
            }
            QCoreApplication::processEvents();
        }
        QTest::qWait(50);
        QTRY_VERIFY2_WITH_TIMEOUT(bar->value() == bar->maximum() && bar->maximum() == trueMaximum(browser),
                                  qPrintable(QStringLiteral("%1/%2, laid out %3").arg(bar->value()).arg(bar->maximum()).arg(trueMaximum(browser))), 5000);
        QVERIFY(hold.active());
        QVERIFY(hold.corrections() > 0);
    }

    // A reader above keeps their message (Block) or line (Line) through edits above the view and a module's wrong value.
    void holdKeepsTheLine_data()
    {
        QTest::addColumn<bool>("line");
        QTest::newRow("block") << false;
        QTest::newRow("line") << true;
    }
    void holdKeepsTheLine()
    {
        QFETCH(bool, line);
        QTextBrowser browser;
        fillPlain(browser, 3000);
        QTextDocument* doc = browser.document();
        QScrollBar*    bar = browser.verticalScrollBar();
        const int      n   = 1500 + 3 - 1500 % 7; // a long line (i % 7 == 3)
        QCOMPARE(n % 7, 3);
        if (line) {
            const QVector<ReaderLine> lines = readerLines(doc, n);
            QVERIFY(lines.size() >= 3);
            bar->setValue(qRound(lines.at(1).top) + 2);
        } else {
            bar->setValue(qRound(blockTop(doc, n)) - 5);
        }
        const chatscroll::ReaderPlace place = chatscroll::captureReader(&browser);
        QCOMPARE(place.line, line);
        QCOMPARE(place.cursor.block().blockNumber(), n);
        LoadHold hold(&browser, place, [] { return true; }, nullptr);
        int      serial = 0;
        // The place back within a few turns of the event loop (the range comes on Qt's 0 ms size timer).
        auto kept = [&](const char* step) {
            QElapsedTimer t;
            t.start();
            qreal y = -1;
            do {
                QCoreApplication::processEvents();
                y = chatscroll::readerY(doc, place);
                if (qAbs(bar->value() - (y + place.offset)) <= 1)
                    return QString();
            } while (t.elapsed() < 500);
            return QStringLiteral("%1: value %2, place at %3 offset %4").arg(QLatin1String(step)).arg(bar->value()).arg(y).arg(place.offset);
        };
        auto check = [&](const char* step) {
            const QString failed = kept(step);
            QVERIFY2(failed.isEmpty(), qPrintable(failed));
        };
        insertPicture(doc, doc->findBlockByNumber(100).position(), 200, &serial);
        bar->setValue(bar->value() + 137);
        check("a picture above");
        {
            QTextCursor c(doc);
            c.setPosition(doc->findBlockByNumber(10).position());
            c.setPosition(doc->findBlockByNumber(20).position(), QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        bar->setValue(bar->value() - 61);
        check("lines removed above");
        hideBlocks(doc, 300, 305);
        check("blocks hidden above");
        { // the layout's way: the header taken from the block's start, a picture and a line break there instead
            const QTextBlock b = place.cursor.block();
            QTextCursor      c(doc);
            c.setPosition(b.position());
            c.setPosition(b.position() + 5, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            insertPicture(doc, b.position(), 40, &serial);
        }
        bar->setValue(0);
        check("a head at the anchor's block");
        insertPicture(doc, doc->findBlockByNumber(5).position(), 300, &serial);
        check("another picture above");
        QVERIFY(hold.active());
        QVERIFY(hold.corrections() >= 4);
    }

    // TeamSpeak's line limit takes the first blocks while the hold keeps the place: the reader's message stays.
    void holdFollowsTrims()
    {
        QTextBrowser browser;
        fillPlain(browser, 3000);
        QTextDocument* doc = browser.document();
        QScrollBar*    bar = browser.verticalScrollBar();
        bar->setValue(qRound(blockTop(doc, 1500)) - 5);
        const chatscroll::ReaderPlace place = chatscroll::captureReader(&browser);
        QCOMPARE(place.cursor.block().blockNumber(), 1500);
        const QString text = place.cursor.block().text();
        LoadHold      hold(&browser, place, [] { return true; }, nullptr);
        QTextCursor   c(doc);
        c.setPosition(0);
        c.setPosition(doc->findBlockByNumber(50).position(), QTextCursor::KeepAnchor);
        c.removeSelectedText();
        QCOMPARE(place.cursor.block().blockNumber(), 1450);
        QCOMPARE(place.cursor.block().text(), text);
        QTRY_VERIFY2_WITH_TIMEOUT(qAbs(bar->value() - (blockTop(doc, 1450) - 5)) <= 1,
                                  qPrintable(QStringLiteral("%1 vs %2").arg(bar->value()).arg(blockTop(doc, 1450) - 5)), 1000);
        QVERIFY(hold.corrections() > 0);
    }

    // Review fix: the load folds the place's message away (a run of join and leave events the reader had expanded, an
    // album's later message): the hold and restoreReader keep the shown row above it, which it folded into, at the view's
    // top, instead of giving the place up.
    void holdFallsBackToTheRowAbove_data()
    {
        QTest::addColumn<bool>("line");
        QTest::newRow("message") << false;
        QTest::newRow("line") << true;
    }
    void holdFallsBackToTheRowAbove()
    {
        QFETCH(bool, line);
        QTextBrowser browser;
        fillPlain(browser, 3000);
        QTextDocument* doc = browser.document();
        QScrollBar*    bar = browser.verticalScrollBar();
        const int      n   = 1500 + 3 - 1500 % 7; // a long line (i % 7 == 3)
        if (line) {
            const QVector<ReaderLine> lines = readerLines(doc, n);
            QVERIFY(lines.size() >= 3);
            bar->setValue(qRound(lines.at(2).top) + 2);
        } else {
            bar->setValue(qRound(blockTop(doc, n)) + 3);
        }
        const chatscroll::ReaderPlace place = chatscroll::captureReader(&browser);
        QCOMPARE(place.line, line);
        QCOMPARE(place.cursor.block().blockNumber(), n);
        LoadHold hold(&browser, place, [] { return true; }, nullptr);
        hideBlocks(doc, n - 2, n + 1); // folded into the row above (n - 3)
        bar->setValue(bar->value() + 400);
        QTRY_VERIFY2_WITH_TIMEOUT(qAbs(bar->value() - blockTop(doc, n - 3)) <= 1,
                                  qPrintable(QStringLiteral("%1 vs %2").arg(bar->value()).arg(blockTop(doc, n - 3))), 1000);
        QVERIFY(hold.active());
        QCOMPARE(chatscroll::readerTarget(doc, place), blockTop(doc, n - 3));
        // restoreReader (the unload's and the load's own restore) follows the same rule.
        hold.finish(LoadHold::End::Settled);
        bar->setValue(0);
        chatscroll::restoreReader(&browser, place);
        QVERIFY(qAbs(bar->value() - blockTop(doc, n - 3)) <= 1);
        // Shown again: the place itself.
        for (int k = n - 2; k <= n + 1; ++k) {
            QTextBlock b = doc->findBlockByNumber(k);
            b.setVisible(true);
            doc->markContentsDirty(b.position(), b.length());
        }
        chatscroll::restoreReader(&browser, place);
        QVERIFY(qAbs(bar->value() - (chatscroll::readerY(doc, place) + place.offset)) <= 1);
    }

    // The reader's first scroll, wheel, key or click ends the hold; later edits no longer move the chat back.
    void holdLetsGo_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("scroll bar step") << QByteArray("action");
        QTest::newRow("wheel on the viewport") << QByteArray("wheel");
        QTest::newRow("PageUp on the chat") << QByteArray("key");
        QTest::newRow("mouse press on the viewport") << QByteArray("press");
    }
    void holdLetsGo()
    {
        QFETCH(QByteArray, input);
        QTextBrowser browser;
        fillPlain(browser, 800);
        QTextDocument* doc = browser.document();
        QScrollBar*    bar = browser.verticalScrollBar();
        bar->setValue(qRound(blockTop(doc, 400)) - 5);
        LoadHold hold(&browser, chatscroll::captureReader(&browser), [] { return true; }, nullptr);
        if (input == "action") {
            bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
        } else if (input == "wheel") {
            QWheelEvent wheel(QPointF(20, 20), browser.viewport()->mapToGlobal(QPoint(20, 20)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(browser.viewport(), &wheel);
        } else if (input == "key") {
            QTest::keyClick(&browser, Qt::Key_PageUp);
        } else {
            QTest::mousePress(browser.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
            QTest::mouseRelease(browser.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        }
        QVERIFY(!hold.active());
        QCOMPARE(hold.ended(), LoadHold::End::User);
        const int value       = bar->value();
        const int corrections = hold.corrections();
        int       serial      = 0;
        insertPicture(doc, doc->findBlockByNumber(50).position(), 200, &serial);
        QTest::qWait(20);
        QCOMPARE(bar->value(), value);
        QCOMPARE(hold.corrections(), corrections);
    }

    // Settled: quiet for quietMs and nothing left to style; never while busy() says otherwise; the cap regardless.
    void holdSettles()
    {
        QTextBrowser browser;
        fillPlain(browser, 200);
        int calls = 0;
        {
            LoadHold hold(&browser, chatscroll::captureReader(&browser), [&calls] { return ++calls <= 3; }, nullptr, 50, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!hold.active(), 3000);
            QCOMPARE(hold.ended(), LoadHold::End::Settled);
            QCOMPARE(calls, 4);
            QVERIFY(hold.ageMs() >= 150);
        }
        {
            LoadHold hold(&browser, chatscroll::captureReader(&browser), [] { return true; }, nullptr, 50, 200);
            QTRY_VERIFY_WITH_TIMEOUT(!hold.active(), 3000);
            QCOMPARE(hold.ended(), LoadHold::End::Cap);
            QVERIFY(hold.ageMs() >= 150);
        }
        { // the finished callback runs once
            int      finished = 0;
            LoadHold hold(&browser, chatscroll::captureReader(&browser), [] { return false; }, nullptr, 20, 30000);
            hold.onFinished = [&finished](LoadHold::End) { ++finished; };
            QTRY_VERIFY_WITH_TIMEOUT(!hold.active(), 3000);
            hold.finish(LoadHold::End::User);
            QCOMPARE(finished, 1);
            QCOMPARE(hold.ended(), LoadHold::End::Settled);
        }
    }

    // TeamSpeak swapped the chat's document: the hold lets go at its next trigger and never sets a value there.
    void holdSwapped()
    {
        QTextBrowser browser;
        fillPlain(browser, 400);
        QScrollBar* bar = browser.verticalScrollBar();
        bar->setValue(qRound(blockTop(browser.document(), 200)) - 5);
        LoadHold hold(&browser, chatscroll::captureReader(&browser), [] { return true; }, nullptr);
        auto*    other = new QTextDocument(&browser);
        QTextCursor c(other);
        for (int i = 0; i < 900; ++i) {
            if (i > 0)
                c.insertBlock();
            c.insertText(QStringLiteral("other %1").arg(i));
        }
        browser.setDocument(other);
        chatscroll::syncRange(&browser);
        QTRY_VERIFY_WITH_TIMEOUT(!hold.active(), 2000);
        QCOMPARE(hold.ended(), LoadHold::End::Swapped);
        QCOMPARE(hold.corrections(), 0);
    }

    // Deleted with an apply pending, or after its chat: nothing runs, nothing crashes.
    void holdGoesQuietly()
    {
        {
            QTextBrowser browser;
            fillPlain(browser, 400);
            QScrollBar* bar = browser.verticalScrollBar();
            bar->setValue(qRound(blockTop(browser.document(), 200)) - 5);
            auto* hold   = new LoadHold(&browser, chatscroll::captureReader(&browser), [] { return true; }, nullptr);
            int   serial = 0;
            insertPicture(browser.document(), browser.document()->findBlockByNumber(20).position(), 200, &serial); // the apply is pending
            const int value = bar->value();
            delete hold;
            QTest::qWait(20);
            QCOMPARE(bar->value(), value);
        }
        {
            auto* browser = new QTextBrowser;
            fillPlain(*browser, 200);
            auto* hold = new LoadHold(browser, chatscroll::captureReader(browser), [] { return true; }, nullptr);
            int   serial = 0;
            insertPicture(browser->document(), 0, 100, &serial);
            delete browser;
            QTest::qWait(20);
            QCOMPARE(hold->ended(), LoadHold::End::Swapped); // its pending apply found the chat gone
            delete hold;
            QTest::qWait(20);
        }
    }

    // ---- 2.7.1 (issue 3): a new chat's first Show -----------------------------------------------------------------

    // A chat filled while hidden, at its end, shown for the first time. Qt's own first-show move (QTextEdit::showEvent:
    // ensureCursorVisible) puts TeamSpeak's cursor in view, and that cursor is at the chat's end: in a row the layout hid
    // (a run of events at the end, an album's later messages), or behind the last line when that line is a card taller
    // than the view. Qt alone shows the chat's top or the card's top (checked first, the negative control); with ShowPlace
    // the chat shows its end, before anything paints. In Cozy and Compact, and in a chat narrower than the hidden one (it
    // grows taller under the same scroll value).
    void firstShowShowsTheEnd_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("ending");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("top"); // Qt alone shows the chat's top (else: short of its end)
        for (const Mode mode : {Mode::Cozy, Mode::Compact}) {
            const QString m = mode == Mode::Cozy ? QStringLiteral("cozy") : QStringLiteral("compact");
            QTest::newRow(qPrintable(m + QStringLiteral(", a run of events at the end"))) << int(mode) << int(NewChat::Ending::Run) << QSize(520, 400) << true;
            QTest::newRow(qPrintable(m + QStringLiteral(", an album's later messages at the end"))) << int(mode) << int(NewChat::Ending::Album) << QSize(520, 400)
                                                                                                    << true;
            QTest::newRow(qPrintable(m + QStringLiteral(", a card taller than the view at the end"))) << int(mode) << int(NewChat::Ending::TallCard)
                                                                                                      << QSize(520, 200) << false;
            QTest::newRow(qPrintable(m + QStringLiteral(", a run of events, a chat narrower than the hidden one"))) << int(mode) << int(NewChat::Ending::Run)
                                                                                                                    << QSize(360, 400) << true;
        }
    }
    void firstShowShowsTheEnd()
    {
        QFETCH(int, mode);
        QFETCH(int, ending);
        QFETCH(QSize, size);
        QFETCH(bool, top);
        const auto kind = static_cast<NewChat::Ending>(ending);
        for (const bool watched : {false, true}) {
            NewChat   chat(size);
            ShowPlace places(ShowPlace::Hooks{});
            if (watched)
                places.watch(chat.fresh);
            chat.fill(60, kind);
            chat.style(static_cast<Mode>(mode));
            if (kind == NewChat::Ending::Run)
                chat.collapse();
            else if (kind == NewChat::Ending::Album)
                hideBlocks(chat.doc(), chat.doc()->blockCount() - 2, chat.doc()->blockCount() - 1);
            else if (kind == NewChat::Ending::TallCard)
                chat.tallCard(320);
            chat.toEnd();
            QVERIFY(!chat.fresh->isVisible());
            QVERIFY2(chatscroll::atBottom(chat.fresh), "the hidden chat is at its end before its Show");
            QCOMPARE(chat.fresh->textCursor().position(), chat.doc()->characterCount() - 1); // TeamSpeak's cursor: the end
            if (kind != NewChat::Ending::TallCard)
                QVERIFY2(!chat.doc()->lastBlock().isVisible() && chat.lastShownBlock() < chat.doc()->blockCount() - 1, "the chat ends in hidden rows");
            chat.show();
            QVERIFY(chat.fresh->isVisible());
            const QString what = QStringLiteral("%1: scroll %2/%3 (true max %4)")
                                     .arg(watched ? QStringLiteral("with ShowPlace") : QStringLiteral("Qt alone"))
                                     .arg(chat.bar()->value())
                                     .arg(chat.bar()->maximum())
                                     .arg(trueMaximum(*chat.fresh));
            qInfo("%s", qPrintable(what));
            if (!watched) {
                // Qt's move (the negative control: what 2.6.0 and 2.7.0 showed).
                QVERIFY2(!chatscroll::atBottom(chat.fresh), qPrintable(what));
                if (top)
                    QVERIFY2(chat.bar()->value() <= 40, qPrintable(what));
                continue;
            }
            QVERIFY2(chatscroll::atBottom(chat.fresh), qPrintable(what));
            QCOMPARE(chat.bar()->value(), chat.bar()->maximum());
            QCOMPARE(places.armed(), 1);
            QCOMPARE(places.fixed(), 1);
            QTest::qWait(20); // the watch goes on the next turn
            // Later Shows move nothing (Qt's move is once per widget): switched away and back, the chat stays.
            chat.tabs.setCurrentIndex(0);
            QCoreApplication::processEvents();
            chat.show();
            QVERIFY2(chatscroll::atBottom(chat.fresh), qPrintable(what));
            QCOMPARE(places.fixed(), 1);
            QTest::qWait(20);
        }
    }

    // A chat whose first Show finds its cursor's line in view (the plain case): ShowPlace arms nothing and Qt's move is
    // nothing; the chat is at its end as TeamSpeak keeps it.
    void firstShowPlainArmsNothing()
    {
        NewChat   chat;
        ShowPlace places(ShowPlace::Hooks{});
        places.watch(chat.fresh);
        chat.fill(60, NewChat::Ending::Plain);
        chat.style(Mode::Cozy);
        chat.toEnd();
        chat.show();
        QVERIFY(chatscroll::atBottom(chat.fresh));
        QCOMPARE(places.armed(), 0);
        QCOMPARE(places.fixed(), 0);
    }

    // R-DEF-1 unchanged: a place a load hold keeps (a reader above the end, carried across a Reload while the chat was
    // hidden) is the chat's place at its first Show, not its end and not its top.
    void firstShowKeepsAHeldPlace()
    {
        NewChat chat;
        chat.fill(60, NewChat::Ending::Run);
        chat.style(Mode::Cozy);
        chat.collapse();
        chat.toEnd();
        chatscroll::ReaderPlace held;
        held.cursor = QTextCursor(chat.doc());
        held.cursor.setPosition(chat.doc()->findBlockByNumber(30).position());
        held.offset = 3;
        ShowPlace::Hooks hooks;
        hooks.heldPlace = [&](QTextBrowser* b) { return b == chat.fresh ? &held : nullptr; };
        ShowPlace places(hooks);
        places.watch(chat.fresh);
        chat.show();
        const int want = qRound(blockTop(chat.doc(), 30) + 3);
        QVERIFY2(qAbs(chat.bar()->value() - want) <= 1, qPrintable(QStringLiteral("scroll %1, want %2").arg(chat.bar()->value()).arg(want)));
        QCOMPARE(places.fixed(), 1);
    }

    // Dark mode's place for a hidden chat (a toggle while it was hidden) is put back at the Show by its application filter,
    // before ShowPlace's: that place is the chat's, kept through Qt's move. Dark mode says so at the hidden resize, so the
    // end read there doesn't win.
    void firstShowKeepsAPlacePutBackAtTheShow()
    {
        NewChat chat;
        chat.fill(60, NewChat::Ending::Run);
        chat.style(Mode::Cozy);
        chat.collapse();
        chat.toEnd();
        AtShow atShow;
        atShow.browser = chat.fresh;
        atShow.block   = 25;
        qApp->installEventFilter(&atShow);
        ShowPlace::Hooks hooks;
        int              asked = 0;
        hooks.placeWaits       = [&](QTextBrowser* b) {
            ++asked;
            return b == chat.fresh;
        };
        ShowPlace places(hooks);
        places.watch(chat.fresh);
        chat.show();
        qApp->removeEventFilter(&atShow);
        QVERIFY(asked >= 1);
        const int want = qRound(blockTop(chat.doc(), 25));
        QVERIFY2(qAbs(chat.bar()->value() - want) <= 1, qPrintable(QStringLiteral("scroll %1, want %2").arg(chat.bar()->value()).arg(want)));
        QCOMPARE(places.fixed(), 1);
    }

    // Review fix: the chat zoom changed while the chat was hidden, and the chat is shown narrower than its hidden layout.
    // The saved zoom comes at the Show, before ShowPlace (ChatZoom's filter is installed later, so it runs first): its
    // FontChange starts a scale pass that reads the place there, short of the end (the chat grew taller under the same
    // scroll value). ShowPlace puts the end back and tells the pass (Hooks::placed -> ChatScale::retake), so the pass's
    // settle 150 ms later keeps the end. Without the hook (the negative control) the settle takes the chat back up. A
    // reader's place is never handed to the pass (it read that place before the new font moved the text).
    void firstShowZoomedPassKeepsTheEnd_data()
    {
        QTest::addColumn<int>("ending");
        QTest::addColumn<qreal>("points"); // the zoomed font at the Show
        QTest::addColumn<QSize>("size");   // the tabs (the hidden chat is laid out at 640 px)
        const qreal base = chatFont().pointSizeF();
        QTest::newRow("a run of events at the end, zoomed in, 360 x 400") << int(NewChat::Ending::Run) << base + 2 << QSize(360, 400);
        QTest::newRow("a run of events at the end, zoomed in, 400 x 300") << int(NewChat::Ending::Run) << base + 2 << QSize(400, 300);
        QTest::newRow("a plain end, zoomed in, 360 x 400") << int(NewChat::Ending::Plain) << base + 2 << QSize(360, 400);
        QTest::newRow("a plain end, zoomed in, 400 x 300") << int(NewChat::Ending::Plain) << base + 2 << QSize(400, 300);
        QTest::newRow("a run of events at the end, zoomed out, 360 x 400") << int(NewChat::Ending::Run) << base - 1 << QSize(360, 400);
        QTest::newRow("a plain end, zoomed out, 400 x 300") << int(NewChat::Ending::Plain) << base - 1 << QSize(400, 300);
    }
    void firstShowZoomedPassKeepsTheEnd()
    {
        QFETCH(int, ending);
        QFETCH(qreal, points);
        QFETCH(QSize, size);
        const auto kind = static_cast<NewChat::Ending>(ending);
        for (const bool hooked : {false, true}) {
            NewChat chat(size);
            chat.layOutHidden();
            chat.fill(60, kind);
            chat.style(Mode::Cozy);
            if (kind == NewChat::Ending::Run)
                chat.collapse();
            chat.toEnd();
            QVERIFY2(chatscroll::atBottom(chat.fresh) && chat.bar()->maximum() > 0, "the hidden chat at the end of its own range");
            ChatScale*       scale = nullptr;
            int              placed = 0;
            ShowPlace::Hooks hooks;
            if (hooked) {
                hooks.placed = [&](QTextBrowser* b) {
                    ++placed;
                    if (b == chat.fresh && scale && scale->pending())
                        scale->retake();
                };
            }
            ShowPlace places(hooks);
            places.watch(chat.fresh); // first, as in attachBrowser
            ZoomAtShow zoom;          // ChatZoom: attached after ShowPlace, so its filter runs before ShowPlace's
            zoom.browser = chat.fresh;
            zoom.font    = chatFont();
            zoom.font.setPointSizeF(points);
            chat.fresh->installEventFilter(&zoom);
            ChatScale::Host host;
            host.fontEvent = [&scale] {
                if (scale)
                    scale->request();
            };
            ChatScale pass(chat.fresh, host);
            scale = &pass;
            chat.show();
            QVERIFY(zoom.set);
            QVERIFY2(!zoom.atEnd, "the new width took the chat short of its end before the zoom");
            QVERIFY(pass.pending());
            QTRY_VERIFY_WITH_TIMEOUT(!pass.pending(), 2000); // the 150 ms settle
            QTest::qWait(20);
            const QString what = QStringLiteral("%1: at the zoom %5/%6; settled %2/%3 (true max %4)")
                                     .arg(hooked ? QStringLiteral("hooked") : QStringLiteral("no hook"))
                                     .arg(chat.bar()->value())
                                     .arg(chat.bar()->maximum())
                                     .arg(trueMaximum(*chat.fresh))
                                     .arg(zoom.value)
                                     .arg(zoom.maximum);
            qInfo("%s", qPrintable(what));
            chat.fresh->removeEventFilter(&zoom);
            if (!hooked) {
                QVERIFY2(trueMaximum(*chat.fresh) - chat.bar()->value() > 40, qPrintable(what)); // what 2.7.0 showed
                continue;
            }
            QVERIFY2(qAbs(trueMaximum(*chat.fresh) - chat.bar()->value()) <= 2, qPrintable(what));
            QVERIFY(placed >= 1);
        }
    }

    // The hook is for the end and a load hold's place only: a hidden chat a reader left above its end keeps the place the
    // scale pass read (no retake), and ShowPlace still undoes Qt's move.
    void firstShowReaderPlaceIsNotRetaken()
    {
        NewChat chat;
        chat.layOutHidden();
        chat.fill(60, NewChat::Ending::Run);
        chat.style(Mode::Cozy);
        chat.collapse();
        chat.toEnd();
        chat.bar()->setValue(qRound(blockTop(chat.doc(), 30)));
        const QString hidden = QStringLiteral("hidden scroll %1/%2").arg(chat.bar()->value()).arg(chat.bar()->maximum());
        QVERIFY2(chat.bar()->value() > 40 && !chatscroll::atBottom(chat.fresh), qPrintable(hidden));
        int              placed = 0;
        ShowPlace::Hooks hooks;
        hooks.placed = [&](QTextBrowser*) { ++placed; };
        ShowPlace places(hooks);
        places.watch(chat.fresh);
        const int value = chat.bar()->value();
        chat.show();
        const QString what = hidden + QStringLiteral(", shown %1/%2").arg(chat.bar()->value()).arg(chat.bar()->maximum());
        qInfo("%s", qPrintable(what));
        QCOMPARE(places.armed(), 1); // Qt's move to the hidden row (the top) ...
        QCOMPARE(places.fixed(), 1); // ... undone: the reader's place as the Show found it
        QCOMPARE(placed, 0);
        QVERIFY2(qAbs(chat.bar()->value() - value) <= 2 && !chatscroll::atBottom(chat.fresh), qPrintable(what));
    }

    // The end ShowPlace hands to the pass is the one read at the hidden resize, before the zoom's font: a reader a little
    // above the end of a chat shown before, hidden through a zoom-out, can be pushed down to the end by the smaller font
    // by the time ShowPlace sees the Show (here: once the range is up to date at the Show; QTextEdit alone takes it a turn
    // later); that is not the end to keep. The pass keeps the reader's line, which it read before the font.
    void zoomedOutReaderNearTheEndIsNotRetaken_data()
    {
        QTest::addColumn<bool>("sync");
        QTest::newRow("the range as QTextEdit has it at the Show") << false;
        QTest::newRow("the range brought up to date before ShowPlace (the reader at the end then)") << true;
    }
    void zoomedOutReaderNearTheEndIsNotRetaken()
    {
        QFETCH(bool, sync);
        NewChat chat;
        chat.fill(60, NewChat::Ending::Plain);
        chat.style(Mode::Cozy);
        chat.show(); // shown before: no resize pending at the next Show
        chat.toEnd();
        chat.bar()->setValue(chat.bar()->maximum() - 100);
        const int               value  = chat.bar()->value();
        const layoutdoc::Anchor anchor = layoutdoc::anchorAt(chat.doc(), value);
        QVERIFY(anchor.block >= 0 && !chatscroll::atBottom(chat.fresh));
        chat.tabs.setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY(!chat.fresh->isVisible());

        ChatScale*       scale  = nullptr;
        int              placed = 0;
        ShowPlace::Hooks hooks;
        hooks.placed = [&](QTextBrowser* b) {
            ++placed;
            if (b == chat.fresh && scale && scale->pending())
                scale->retake();
        };
        ShowPlace places(hooks);
        places.watch(chat.fresh);
        SeenAtShow seen; // between the zoom and ShowPlace: what ShowPlace finds
        seen.browser = chat.fresh;
        seen.sync    = sync;
        chat.fresh->installEventFilter(&seen);
        ZoomAtShow zoom;
        zoom.browser = chat.fresh;
        zoom.font    = chatFont();
        zoom.font.setPointSizeF(chatFont().pointSizeF() - 3);
        chat.fresh->installEventFilter(&zoom);
        ChatScale::Host host;
        host.fontEvent = [&scale] {
            if (scale)
                scale->request();
        };
        ChatScale pass(chat.fresh, host);
        scale = &pass;
        chat.show();
        chat.fresh->removeEventFilter(&zoom);
        chat.fresh->removeEventFilter(&seen);
        QVERIFY(zoom.set && !zoom.atEnd);
        if (sync)
            QVERIFY2(seen.atEnd, "the smaller font pushed the reader down to the end before ShowPlace's filter");
        QTRY_VERIFY_WITH_TIMEOUT(!pass.pending(), 2000);
        QTest::qWait(20);
        const qreal   top  = layoutdoc::anchorY(chat.doc(), anchor);
        const QString what = QStringLiteral("hidden %1, settled %2/%3, the reader's line at %4").arg(value).arg(chat.bar()->value()).arg(chat.bar()->maximum()).arg(top);
        qInfo("%s", qPrintable(what));
        QCOMPARE(placed, 0);
        QVERIFY2(!chatscroll::atBottom(chat.fresh) && qAbs(chat.bar()->value() - qRound(top)) <= 2, qPrintable(what));
    }

    // stop(): every filter off, nothing armed (the plugin's unload); a chat shown afterwards gets Qt's move as it is.
    void showPlaceStops()
    {
        NewChat chat;
        chat.fill(60, NewChat::Ending::Run);
        chat.style(Mode::Cozy);
        chat.collapse();
        chat.toEnd();
        ShowPlace places(ShowPlace::Hooks{});
        places.watch(chat.fresh);
        places.stop();
        chat.show();
        QCOMPARE(places.armed(), 0);
        QVERIFY(!chatscroll::atBottom(chat.fresh));
    }

    // Where QTextEdit's ensureCursorVisible goes, as cursorJumpTarget says it would: checked against Qt itself.
    void cursorJumpTargetIsQts_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("a run of events at the end (the top)") << int(NewChat::Ending::Run);
        QTest::newRow("a card taller than the view") << int(NewChat::Ending::TallCard);
        QTest::newRow("plain (no move)") << int(NewChat::Ending::Plain);
    }
    void cursorJumpTargetIsQts()
    {
        QFETCH(int, ending);
        const auto kind = static_cast<NewChat::Ending>(ending);
        NewChat    chat(QSize(520, 200));
        chat.fill(60, kind);
        chat.style(Mode::Cozy);
        if (kind == NewChat::Ending::Run)
            chat.collapse();
        else if (kind == NewChat::Ending::TallCard)
            chat.tallCard(320);
        chat.show();
        chat.toEnd();
        const int before = chat.bar()->value();
        const int target = chatscroll::cursorJumpTarget(chat.fresh);
        chat.fresh->ensureCursorVisible();
        QVERIFY2(qAbs(chat.bar()->value() - target) <= 2, qPrintable(QStringLiteral("Qt %1, cursorJumpTarget %2").arg(chat.bar()->value()).arg(target)));
        if (kind == NewChat::Ending::Plain)
            QCOMPARE(target, before);
        else
            QVERIFY(target < before);
    }

    // ChatIntegration's wiring: every chat watched first in attachBrowser (its filter the last of ours before QTextEdit's
    // showEvent), stopped first at unload; the CRASH RULE in the new code.
    void showPlaceSourceRules()
    {
        const QString code = source("src/showplace.cpp") + source("src/showplace.h");
        QVERIFY(!code.isEmpty());
        QVERIFY(!code.contains(QLatin1String("QTimer::singleShot")));
        QVERIFY(!code.contains(QLatin1String("QStringLiteral")));
        QVERIFY(!code.contains(QLatin1String("QByteArrayLiteral")));
        QVERIFY(!code.contains(QLatin1String("setProperty")));
        const QString chat   = source("src/chatintegration.cpp");
        const int     attach = chat.indexOf(QLatin1String("void ChatIntegration::attachBrowser("));
        QVERIFY(attach > 0);
        const int watch = chat.indexOf(QLatin1String("m_showPlace->watch(browser)"), attach);
        const int zoom  = chat.indexOf(QLatin1String("m_zoom->attach(browser)"), attach);
        QVERIFY(watch > attach && zoom > watch);
        // Review fix: the end or a load hold's place ShowPlace set is taken again by a pending scale pass.
        const int hook = chat.indexOf(QLatin1String("hooks.placed = [this](QTextBrowser* browser)"));
        QVERIFY(hook > 0 && hook < attach);
        const int retake = chat.indexOf(QLatin1String("view->scaler->retake()"), hook);
        QVERIFY(retake > hook && retake < chat.indexOf(QLatin1String("m_showPlace = new ShowPlace(hooks, this)"), hook));
        const int dtor = chat.indexOf(QLatin1String("ChatIntegration::~ChatIntegration()"));
        const int stop = chat.indexOf(QLatin1String("m_showPlace->stop()"), dtor);
        const int kept = chat.indexOf(QLatin1String("QVector<KeptPlace> keptPlaces"), dtor);
        QVERIFY(dtor > 0 && stop > dtor && kept > stop);
    }

    // The CRASH RULE in the new code: no functor singleShot, no QStringLiteral (static data of this DLL in Qt's state);
    // the carry is built at run time.
    void holdSourceRules()
    {
        const QString hold = source("src/loadhold.cpp") + source("src/loadhold.h");
        QVERIFY(!hold.isEmpty());
        QVERIFY(!hold.contains(QLatin1String("QTimer::singleShot")));
        QVERIFY(!hold.contains(QLatin1String("QStringLiteral")));
        QVERIFY(!hold.contains(QLatin1String("QByteArrayLiteral")));
        const QString scroll = source("src/chatscroll.h");
        const int     from   = scroll.indexOf(QLatin1String("2.7 (R-DEF-1): the reader's place across the plugin's unload and load"));
        QVERIFY(from > 0);
        const QString carry = scroll.mid(from);
        QVERIFY(carry.contains(QLatin1String("inline QByteArray encodeCarry(")));
        QVERIFY(!carry.contains(QLatin1String("QByteArrayLiteral")));
        QVERIFY(!carry.contains(QLatin1String("QStringLiteral")));
    }
};

TSMEDIA_REGISTER_TEST(TestReaderPlace)
#include "tst_readerplace.moc"
