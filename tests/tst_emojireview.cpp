// 2.2 emoji: regression tests for the emoji review. TeamSpeak-like widgets (a chat browser with an S0
// header block format and a ChatLineEdit input) with ChatEmoji on them, the picker's keys and its
// opener button, and the renderer's worker queue.

#include <QAbstractTextDocumentLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QVBoxLayout>
#include <QtTest>

#include "chatemoji.h"
#include "emojidata.h"
#include "emojipicker.h"
#include "emojiprefs.h"
#include "emojirender.h"
#include "peerprotocol.h"
#include "settings.h"
#include "testmain.h"

namespace {

// TeamSpeak's chat input is a QTextEdit subclass of this name (S0 j).
class ChatLineEdit : public QTextEdit
{
  public:
    using QTextEdit::QTextEdit;
};

const QString kGrin   = QStringLiteral("\U0001F600");
const QString kThumbs = QStringLiteral("\U0001F44D");
const QString kRocket = QStringLiteral("\U0001F680");

// A message the way TeamSpeak 3.6.2 builds it (S0 h): "<time>", nickname link, ": text".
void message(QTextDocument* doc, const QString& nick, const QString& text)
{
    QTextCursor c(doc);
    c.movePosition(QTextCursor::End);
    if (!doc->isEmpty())
        c.insertBlock();
    c.insertText(QStringLiteral("<20:02:13> "), QTextCharFormat());
    QTextCharFormat link;
    link.setAnchor(true);
    link.setAnchorHref(QStringLiteral("client://17/KpNqZMq7js/JajRo+3zOFPViX1E=~") + nick);
    c.insertText(QLatin1Char('"') + nick + QLatin1Char('"'), link);
    c.insertText(QStringLiteral(": ") + text, QTextCharFormat());
}

// The HD emoji of a document: their picture names and widths.
struct Pictures {
    QStringList names;
    QList<qreal> widths;
};

Pictures picturesOf(QTextDocument* doc)
{
    Pictures out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            if (cf.isImageFormat() && cf.toImageFormat().name().startsWith(QLatin1String("tsmemoji:"))) {
                for (int i = 0; i < it.fragment().length(); ++i) {
                    out.names << cf.toImageFormat().name();
                    out.widths << cf.toImageFormat().width();
                }
            }
        }
    }
    return out;
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

// A window with a chat browser and an input, like TeamSpeak's chat area.
struct ChatWindow {
    QWidget       window;
    QTextBrowser* browser = nullptr;
    ChatLineEdit* input   = nullptr;

    ChatWindow()
    {
        window.resize(520, 420);
        auto* layout = new QVBoxLayout(&window);
        browser      = new QTextBrowser(&window);
        input        = new ChatLineEdit(&window);
        input->setObjectName(QStringLiteral("ChatLineEdit"));
        input->setAcceptRichText(false);
        input->setFixedHeight(30);
        layout->addWidget(browser);
        layout->addWidget(input);
        QFont font(QStringLiteral("Segoe UI"));
        font.setPointSizeF(9);
        browser->setFont(font);
        input->setFont(font);
        window.show();
    }
};

} // namespace

class TestEmojiReview : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        emoji::prefs::setFile(QString());
        Settings::instance() = Settings();
    }

    void cleanup()
    {
        Settings::instance() = Settings();
    }

    // TeamSpeak clears a chat (QTextDocument::clear() drops every resource): emoji that come afterwards
    // must get their pictures again, not show as empty images.
    void picturesSurviveAClearedChat()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        ChatWindow w;
        for (int i = 0; i < 5; ++i)
            message(w.browser->document(), QStringLiteral("Sara"), QStringLiteral("line %1 %2 %3").arg(i).arg(kGrin, kThumbs));
        ChatEmoji chat;
        chat.attach(w.browser);
        chat.processNow(w.browser);
        QCOMPARE(ChatEmoji::countEmoji(w.browser->document()), 10);
        w.browser->clear();
        message(w.browser->document(), QStringLiteral("Reza"), QStringLiteral("again %1 %2").arg(kGrin, kThumbs));
        chat.processNow(w.browser);
        const Pictures pictures = picturesOf(w.browser->document());
        QCOMPARE(pictures.names.size(), 2);
        for (const QString& name : pictures.names)
            QVERIFY2(!w.browser->document()->resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>().isNull(), qPrintable(name));
    }

    // Saving the settings dialog with no emoji option changed leaves the chats alone; turning HD emoji
    // off gives everything back, and on again does it again.
    void otherSettingsLeaveTheChatAlone()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        ChatWindow w;
        message(w.browser->document(), QStringLiteral("Sara"), QStringLiteral("hi %1").arg(kGrin));
        ChatEmoji chat;
        chat.attach(w.browser);
        chat.processNow(w.browser);
        int changes = 0;
        connect(w.browser->document(), &QTextDocument::contentsChange, this, [&changes](int, int, int) { ++changes; });
        Settings::instance().emojiButton = false; // only the button
        chat.settingsChanged();
        QTest::qWait(50);
        QCOMPARE(changes, 0);
        QCOMPARE(ChatEmoji::countEmoji(w.browser->document()), 1);
        Settings::instance().hdEmoji = false;
        chat.settingsChanged();
        QCOMPARE(ChatEmoji::countEmoji(w.browser->document()), 0);
        Settings::instance().hdEmoji = true;
        chat.settingsChanged();
        chat.processNow(w.browser);
        QCOMPARE(ChatEmoji::countEmoji(w.browser->document()), 1);
    }

    // TeamSpeak may keep a document it swapped out of a view (and show it again later): unloading the
    // plugin gives that one back too.
    void swappedOutDocumentIsRestored()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QObject    keeper; // outlives the window: the browser shows one of its documents at the end
        ChatWindow w;
        auto*      first  = new QTextDocument(&keeper);
        auto*      second = new QTextDocument(&keeper);
        message(first, QStringLiteral("Ali"), QStringLiteral("first %1").arg(kRocket));
        message(second, QStringLiteral("Ali"), QStringLiteral("second %1").arg(kRocket));
        const QString firstText = first->toPlainText();
        auto*         chat      = new ChatEmoji;
        chat->attach(w.browser);
        w.browser->setDocument(first);
        chat->documentSwapped(w.browser);
        chat->processNow(w.browser);
        w.browser->setDocument(second);
        chat->documentSwapped(w.browser);
        chat->processNow(w.browser);
        QCOMPARE(ChatEmoji::countEmoji(first), 1);
        QCOMPARE(ChatEmoji::countEmoji(second), 1);
        delete chat;
        QCOMPARE(ChatEmoji::countEmoji(second), 0);
        QCOMPARE(ChatEmoji::countEmoji(first), 0);
        QCOMPARE(first->toPlainText(), firstText);
    }

    // Zooming the chat (Ctrl + wheel) or another chat font: inline emoji follow the text's size, jumbo
    // ones stay 48 px, and restoring still gives the exact original back.
    void zoomResizesInlineEmoji()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        ChatWindow w;
        message(w.browser->document(), QStringLiteral("Sara"), QStringLiteral("hi %1 there").arg(kGrin));
        message(w.browser->document(), QStringLiteral("Reza"), kThumbs + kThumbs);
        const QString before = formatsOf(w.browser->document());
        ChatEmoji     chat;
        chat.attach(w.browser);
        chat.processNow(w.browser);
        const Pictures small = picturesOf(w.browser->document());
        QCOMPARE(small.widths.size(), 3);
        QCOMPARE(small.widths.at(1), 48.0); // jumbo
        w.browser->zoomIn(6);
        QTRY_VERIFY_WITH_TIMEOUT(picturesOf(w.browser->document()).widths.value(0) > small.widths.at(0), 5000);
        const Pictures large = picturesOf(w.browser->document());
        QCOMPARE(large.widths.at(1), 48.0);
        QCOMPARE(large.widths.at(2), 48.0);
        // The line is still no taller than its text (bottom-aligned, at most the line height).
        QVERIFY(large.widths.at(0) <= QFontMetrics(w.browser->document()->defaultFont()).height());
        for (const QString& name : large.names)
            QVERIFY(!w.browser->document()->resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>().isNull());
        w.browser->zoomOut(6);
        QTRY_COMPARE_WITH_TIMEOUT(picturesOf(w.browser->document()).widths.value(0), small.widths.at(0), 5000);
        ChatEmoji::restore(w.browser->document());
        QCOMPARE(formatsOf(w.browser->document()), before);
    }

    // At most 400 HD emoji per message, also when the document is worked through again (a settings
    // change, a document swap).
    void capHoldsWhenDoneAgain()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        QTextDocument doc;
        QString       many;
        for (int i = 0; i < 1000; ++i)
            many += kGrin;
        message(&doc, QStringLiteral("Spammer"), many);
        ChatEmoji::processDocument(&doc, 1.0);
        QCOMPARE(ChatEmoji::countEmoji(&doc), 400);
        ChatEmoji::processDocument(&doc, 1.0);
        ChatEmoji::processDocument(&doc, 1.0);
        QCOMPARE(ChatEmoji::countEmoji(&doc), 400);
        ChatEmoji::restore(&doc);
        QCOMPARE(doc.toPlainText().count(kGrin), 1000);
    }

    // The emoji button follows the input when it grows or shrinks (TeamSpeak's input grows with its text).
    void inputButtonFollowsTheInput()
    {
        ChatWindow w;
        ChatEmoji  chat;
        chat.attachInput(w.input);
        QWidget* button = w.input->findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton"));
        QVERIFY(button);
        QTRY_VERIFY(button->isVisible());
        for (int height : {90, 30, 64}) {
            w.input->setFixedHeight(height);
            QTRY_COMPARE(button->height(), w.input->viewport()->height());
            QCOMPARE(button->x(), w.input->viewport()->geometry().right() + 1);
            QVERIFY(w.input->viewport()->geometry().right() < button->x()); // text never runs under it
        }
    }

    // After a search the grid has other sections: the keys use the new ones (Page Up has nowhere to
    // go in the single results section), and screen readers hear the focused emoji's name.
    void pickerKeysAfterASearch()
    {
        auto* picker = new EmojiPicker(EmojiPicker::Mode::Insert, false, QColor());
        QPointer<EmojiPicker> guard(picker);
        picker->openAt(QRect(600, 600, 24, 24));
        auto* grid = picker->findChild<QWidget*>(QStringLiteral("tsmediaEmojiGrid"));
        QVERIFY(grid);
        picker->setSearchText(QStringLiteral("face"));
        picker->focusItem(30);
        const QString focused = grid->accessibleName();
        QVERIFY(!focused.isEmpty());
        QVERIFY(focused != QStringLiteral("Emoji"));
        QTest::keyClick(grid, Qt::Key_PageUp);
        QCOMPARE(grid->accessibleName(), focused);
        QTest::keyClick(grid, Qt::Key_Up);
        QVERIFY(grid->accessibleName() != focused); // a row up
        // Esc in the grid clears the search first (as in the search field), then closes.
        QTest::keyClick(grid, Qt::Key_Escape);
        QVERIFY(picker->isVisible());
        QVERIFY(picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch"))->text().isEmpty());
        // Ctrl+E, which opened it, closes it.
        QTest::keyClick(picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch")), Qt::Key_E, Qt::ControlModifier);
        QVERIFY(!picker->isVisible());
        QTRY_VERIFY(!guard); // deleted on close
    }

    // A press on the button that opened the picker closes it without Qt handing the press on to the
    // button (which would open it again at once); a press anywhere else is handed on as usual.
    void pressOnTheOpenerClosesThePicker()
    {
        const QRect opener(600, 600, 24, 24);
        for (const bool onOpener : {true, false}) {
            auto* picker = new EmojiPicker(EmojiPicker::Mode::Insert, false, QColor());
            picker->setOpener(opener);
            picker->openAt(opener);
            const QPoint global = onOpener ? opener.center() : QPoint(5, 5);
            QVERIFY(!picker->geometry().contains(global));
            QMouseEvent press(QEvent::MouseButtonPress, picker->mapFromGlobal(global), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(picker, &press);
            QCOMPARE(picker->testAttribute(Qt::WA_NoMouseReplay), onOpener);
            QVERIFY(!picker->isVisible());
        }
    }

    // The chat input's button opens the picker, and a press on it while the picker is open closes it for
    // good; a press in the text only closes it (and goes on to the text).
    void inputButtonTogglesThePicker()
    {
        ChatWindow w;
        ChatEmoji  chat;
        chat.attachInput(w.input);
        QWidget* button = w.input->findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton"));
        QVERIFY(button);
        QTRY_VERIFY(button->isVisible());
        for (const bool onButton : {true, false}) {
            QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
            auto* picker = w.input->findChild<EmojiPicker*>();
            QVERIFY(picker);
            QTRY_VERIFY(picker->isVisible());
            const QPoint global = onButton ? button->mapToGlobal(button->rect().center()) : w.input->viewport()->mapToGlobal(QPoint(10, 10));
            QVERIFY(!picker->geometry().contains(global));
            QMouseEvent  press(QEvent::MouseButtonPress, picker->mapFromGlobal(global), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(picker, &press);
            QCOMPARE(picker->testAttribute(Qt::WA_NoMouseReplay), onButton);
            QVERIFY(!picker->isVisible());
            QPointer<EmojiPicker> gone(picker);
            QTRY_VERIFY(!gone);
        }
    }

    // ---- the renderer's worker --------------------------------------------------------------------
    // Every paint of the picker asks again for what it still lacks: each picture is drawn once anyway.
    void workerDrawsEachPictureOnce()
    {
        emoji::shutdown();
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        const QVector<int> ids = emoji::members(emoji::Group::TravelPlaces).mid(0, 6);
        for (int paint = 0; paint < 40; ++paint) {
            for (int id : ids)
                emoji::requestImage(id, 37, 1.0);
        }
        const auto allCached = [&ids] {
            for (int id : ids) {
                if (emoji::cachedImage(emoji::text(id), 37, 1.0).isNull())
                    return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(allCached(), 10000);
        QTest::qWait(100);
        QCOMPARE(emoji::cacheInfo().drawn, qint64(ids.size()));
        QCOMPARE(emoji::cacheInfo().pending, 0);
    }

    // Prefetching draws the first ids first (the next screen of the picker before the ones after it).
    void prefetchDrawsInOrder()
    {
        emoji::shutdown();
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        const QVector<int> ids   = emoji::members(emoji::Group::Objects).mid(0, 200);
        const auto         ready = [](int id) { return !emoji::cachedImage(emoji::text(id), 35, 1.0).isNull(); };
        emoji::prefetch(ids, 35, 1.0);
        QTRY_VERIFY_WITH_TIMEOUT(ready(ids.first()), 10000);
        QVERIFY(!ready(ids.last()));
        QTRY_VERIFY_WITH_TIMEOUT(ready(ids.last()), 10000);
        emoji::shutdown();
    }

    // ---- the protocol -----------------------------------------------------------------------------
    // e= may only describe items of i= (as documented): one for another key makes it malformed as a whole.
    void emojiFieldOnlyForListedItems()
    {
        const QByteArray a    = QByteArrayLiteral("0123456789abcdef0123");
        const QByteArray b    = QByteArrayLiteral("fedcba9876543210fedc");
        const QByteArray wire = "tsm1 R s=c i=" + a + ":up e=" + a + ":1f923," + b + ":1f389";
        const std::optional<proto::React> r = proto::readReact(*proto::parse(wire));
        QVERIFY(r);
        QCOMPARE(r->items.size(), 1);
        QCOMPARE(r->items.first().set.size(), 1);
        QCOMPARE(r->items.first().set.first(), proto::legacyReactionEmoji(proto::ThumbsUp));
    }
};

TSMEDIA_REGISTER_TEST(TestEmojiReview)

#include "tst_emojireview.moc"
