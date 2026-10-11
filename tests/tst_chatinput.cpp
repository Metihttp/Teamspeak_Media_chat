// 2.2.1: TeamSpeak's chat input holds "Enter Chat Message..." as grey text of its own while it is empty
// and has no focus, and takes it out on focus-in. An emoji picked (or "Use in the chat input") while the
// picker or the chat had the focus went in next to it, and Enter sent "<emoji>Enter Chat Message...".
// A ChatLineEdit that behaves like TeamSpeak's, with ChatInputs (chatinput.h), the picker and ChatEmoji's
// chat menu on it.

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

#include "chatemoji.h"
#include "chatinput.h"
#include "emojidata.h"
#include "emojiinput.h"
#include "emojiformat.h"
#include "emojipicker.h"
#include "emojiprefs.h"
#include "emojirender.h"
#include "settings.h"
#include "testmain.h"

namespace {

const QString kPlaceholder = QStringLiteral("Enter Chat Message...");
const QString kGrin        = QStringLiteral("\U0001F600");
const QString kRocket      = QStringLiteral("\U0001F680");

// The focus-ins on which the TeamSpeak of a test takes its placeholder out.
enum class Takes {
    Any,    // every one
    Users,  // a click's, Tab's, the window's: not Qt::OtherFocusReason (the plugin's) or a popup's
    TabOnly // only Tab's (not even a click's): only the safety net takes it out for the plugin
};

// How the TeamSpeak of a test shows and handles its placeholder (the defaults: as the bug report shows).
struct Look {
    bool    grey  = true;  // in a grey of its own (else in the text colour)
    bool    late  = false; // taken out from a queued call after the focus-in, not during it
    Takes   takes = Takes::Any;
    QString text  = kPlaceholder; // the words (TeamSpeak's language)
    QColor  typed;                // typing after it is taken out gets this colour (invalid: none)
};

// TeamSpeak 3.6.2's chat input as the bug report shows it: empty and without the focus, it holds the
// placeholder as text in its document (grey, or in the text colour), the cursor before it; on focus-in
// it takes it out when that is still all it holds (what was put in next to it stayed, and went out).
class ChatLineEdit : public QTextEdit
{
  public:
    explicit ChatLineEdit(bool grey, QWidget* parent = nullptr, bool late = false)
        : ChatLineEdit(lookOf(grey, late), parent)
    {
    }

    ChatLineEdit(const Look& look, QWidget* parent)
        : QTextEdit(parent)
        , m_look(look)
    {
        setObjectName(QStringLiteral("ChatLineEdit"));
        setAcceptRichText(false);
        showPlaceholder();
    }

    bool showing() const { return m_showing; }

    // Text dropped onto the input (a client dragged from the channel list): in place of the placeholder,
    // without the focus.
    void dropText(const QString& text)
    {
        if (m_showing) {
            clear();
            m_showing = false;
        }
        QTextCursor cursor(document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
    }

  protected:
    void focusInEvent(QFocusEvent* event) override
    {
        const Qt::FocusReason reason = event->reason();
        const bool            tab    = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason;
        const bool            takes  = m_look.takes == Takes::Any || (m_look.takes == Takes::TabOnly && tab)
                                   || (m_look.takes == Takes::Users && reason != Qt::OtherFocusReason && reason != Qt::PopupFocusReason);
        if (!takes) {
            QTextEdit::focusInEvent(event);
            return;
        }
        if (m_look.late)
            QMetaObject::invokeMethod(this, [this] { hidePlaceholder(); }, Qt::QueuedConnection);
        else
            hidePlaceholder();
        QTextEdit::focusInEvent(event);
    }

    void hidePlaceholder()
    {
        if (m_showing && toPlainText() == m_look.text) {
            clear();
            QTextCharFormat typing;
            if (m_look.typed.isValid())
                typing.setForeground(m_look.typed);
            setCurrentCharFormat(typing);
        }
        m_showing = false;
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        QTextEdit::focusOutEvent(event);
        if (toPlainText().isEmpty())
            showPlaceholder();
    }

  private:
    static Look lookOf(bool grey, bool late)
    {
        Look look;
        look.grey = grey;
        look.late = late;
        return look;
    }

    void showPlaceholder()
    {
        m_showing = true;
        QTextCharFormat format;
        if (m_look.grey)
            format.setForeground(QColor(0x80, 0x80, 0x80));
        QTextCursor cursor(document());
        cursor.insertText(m_look.text, format);
        moveCursor(QTextCursor::Start);
    }

    Look m_look;
    bool m_showing = false;
};

// TeamSpeak's chat area: the chat, a field like the picker's search (to have the focus elsewhere), the input.
struct Window {
    QWidget       window;
    QTextBrowser* chat  = nullptr;
    QLineEdit*    other = nullptr;
    ChatLineEdit* input = nullptr;

    explicit Window(bool grey = true, bool late = false)
    {
        Look look;
        look.grey = grey;
        look.late = late;
        build(look);
    }

    explicit Window(const Look& look) { build(look); }

    void build(const Look& look)
    {
        window.resize(520, 360);
        auto* layout = new QVBoxLayout(&window);
        chat         = new QTextBrowser(&window);
        other        = new QLineEdit(&window);
        input        = new ChatLineEdit(look, &window);
        input->setFixedHeight(30);
        layout->addWidget(chat);
        layout->addWidget(other);
        layout->addWidget(input);
        window.show();
        activate();
    }

    // As TeamSpeak's window in front (Qt's state; the platform may keep a test's window behind).
    void activate()
    {
        QApplication::setActiveWindow(&window);
        QCoreApplication::processEvents();
    }

    void focusChat()
    {
        activate();
        chat->setFocus(Qt::MouseFocusReason);
    }

    void focusInput(Qt::FocusReason reason = Qt::MouseFocusReason)
    {
        activate();
        input->setFocus(reason);
    }
};

// The viewport point in the middle of document position p.
QPoint pointAt(QTextBrowser* chat, int p)
{
    QTextDocument*     doc    = chat->document();
    const QTextBlock   block  = doc->findBlock(p);
    const QTextLayout* layout = block.layout();
    const int          rel    = p - block.position();
    const QTextLine    line   = layout->lineForTextPosition(rel);
    const QRectF       br     = doc->documentLayout()->blockBoundingRect(block);
    const qreal        x      = (line.cursorToX(rel) + line.cursorToX(rel + 1)) / 2.0;
    return QPoint(qRound(br.left() + x) - chat->horizontalScrollBar()->value(), qRound(br.top() + line.y() + line.height() / 2.0) - chat->verticalScrollBar()->value());
}

// Typed as a keyboard of that language would (QTest::keyClicks takes Latin-1 only): one key press per
// character, carrying its text.
void typeText(QWidget* widget, const QString& text)
{
    for (const QChar ch : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(widget, &press);
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(widget, &release);
    }
}

int firstHdEmoji(QTextDocument* doc)
{
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            if (emojiformat::isHd(it.fragment().charFormat()))
                return it.fragment().position();
        }
    }
    return -1;
}

} // namespace

class TestChatInput : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        emoji::prefs::setFile(QString());
        Settings::instance() = Settings();
    }

    void cleanup() { Settings::instance() = Settings(); }

    // The placeholder is learned from TeamSpeak's own focus changes, in whatever words: taken out on
    // focus-in, put in again on focus-out. While it is in, nothing counts as typed.
    void learnsThePlaceholder()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QVERIFY(inputs.placeholder(w.input).isEmpty());
        w.focusInput();
        QVERIFY(w.input->hasFocus());
        QVERIFY(w.input->toPlainText().isEmpty());
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QVERIFY(inputs.showsPlaceholder(w.input));
        QCOMPARE(inputs.typedText(w.input), QString());
    }

    // The bug: an emoji into the empty input while the chat (or the picker) has the focus. It is exactly
    // the emoji afterwards, with the input focused: TeamSpeak took its placeholder out itself.
    void insertIntoTheEmptyUnfocusedInput_data()
    {
        QTest::addColumn<bool>("grey");
        QTest::addColumn<bool>("seen"); // the placeholder was seen coming and going before
        QTest::addColumn<bool>("late"); // TeamSpeak takes it out just after the focus-in
        QTest::newRow("grey, seen") << true << true << false;
        QTest::newRow("grey, never seen") << true << false << false;
        QTest::newRow("text colour, seen") << false << true << false;
        QTest::newRow("text colour, never seen") << false << false << false;
        QTest::newRow("late removal, grey, seen") << true << true << true;
        QTest::newRow("late removal, text colour, never seen") << false << false << true;
    }
    void insertIntoTheEmptyUnfocusedInput()
    {
        QFETCH(bool, grey);
        QFETCH(bool, seen);
        QFETCH(bool, late);
        Window     w(grey, late);
        ChatInputs inputs;
        inputs.attach(w.input);
        if (seen) {
            w.focusInput();
            w.focusChat();
        }
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE(w.input->toPlainText(), kGrin);
        QVERIFY(w.input->hasFocus());
        QVERIFY(!w.input->showing());
        QVERIFY(!inputs.hasPending(w.input));
        QCOMPARE(inputs.typedText(w.input), kGrin);
        // Typing goes on in the text colour, not the placeholder's.
        QTest::keyClicks(w.input, QStringLiteral("hi"));
        QCOMPARE(w.input->toPlainText(), kGrin + QStringLiteral("hi"));
    }

    // Text the user typed stays; the emoji goes in at the cursor.
    void insertAtTheCursorKeepsTypedText()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        QTest::keyClicks(w.input, QStringLiteral("hello"));
        QTextCursor cursor = w.input->textCursor();
        cursor.setPosition(2);
        w.input->setTextCursor(cursor);
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), QStringLiteral("hello")); // no placeholder next to typed text
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE(w.input->toPlainText(), QStringLiteral("he") + kGrin + QStringLiteral("llo"));
        QCOMPARE(w.input->textCursor().position(), 2 + kGrin.size());
        // And at the end.
        cursor = w.input->textCursor();
        cursor.movePosition(QTextCursor::End);
        w.input->setTextCursor(cursor);
        w.focusChat();
        inputs.insert(w.input, kRocket);
        QTRY_COMPARE(w.input->toPlainText(), QStringLiteral("he") + kGrin + QStringLiteral("llo") + kRocket);
    }

    // Someone who typed the placeholder's very words keeps them, also without the focus, also when
    // TeamSpeak's placeholder has the text colour.
    void typedPlaceholderWordsAreKept_data()
    {
        QTest::addColumn<bool>("grey");
        QTest::newRow("grey placeholder") << true;
        QTest::newRow("placeholder in the text colour") << false;
    }
    void typedPlaceholderWordsAreKept()
    {
        QFETCH(bool, grey);
        Window     w(grey);
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        w.focusChat();
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        w.focusInput();
        QTest::keyClicks(w.input, kPlaceholder);
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QVERIFY(!w.input->showing()); // TeamSpeak's own state: the user's text
        QVERIFY(!inputs.showsPlaceholder(w.input));
        QCOMPARE(inputs.typedText(w.input), kPlaceholder);
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE(w.input->toPlainText(), kPlaceholder + kGrin);
        // Shift in the picker (the focus goes back): the same.
        w.other->setFocus(Qt::MouseFocusReason);
        inputs.insert(w.input, kRocket, true);
        QCOMPARE(w.input->toPlainText(), kPlaceholder + kGrin + kRocket);
    }

    // The picker stays open (Shift): the input gets the focus just for the insertion, the keyboard goes
    // back to where it was, and the emoji are in (nothing of the placeholder).
    void keepFocusGivesTheKeyboardBack()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusChat();
        w.other->setFocus(Qt::MouseFocusReason); // the picker's search field
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        inputs.insert(w.input, kGrin, true);
        QCOMPARE(w.input->toPlainText(), kGrin);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(w.other));
        inputs.insert(w.input, kRocket, true);
        QCOMPARE(w.input->toPlainText(), kGrin + kRocket);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(w.other));
        // Closing the picker: the input gets the focus back, the text stays.
        w.input->setFocus(Qt::OtherFocusReason);
        QCOMPARE(w.input->toPlainText(), kGrin + kRocket);
    }

    // The safety net: no focus to be had (here: the input isn't shown). After the tries, the learned
    // placeholder goes first; an input that never showed its placeholder changing keeps its text, and the
    // emoji waits for its next focus-in.
    void withoutFocus_data()
    {
        QTest::addColumn<bool>("seen");
        QTest::newRow("placeholder learned") << true;
        QTest::newRow("nothing learned") << false;
    }
    void withoutFocus()
    {
        QFETCH(bool, seen);
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        if (seen) {
            w.focusInput();
            w.focusChat();
        }
        w.input->hide();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        inputs.insert(w.input, kGrin);
        if (seen) {
            QTRY_COMPARE_WITH_TIMEOUT(w.input->toPlainText(), kGrin, 4000);
            QVERIFY(!inputs.hasPending(w.input));
            return;
        }
        QTest::qWait(1500); // past the tries
        QCOMPARE(w.input->toPlainText(), kPlaceholder); // never next to it
        QVERIFY(inputs.hasPending(w.input));
        w.input->show();
        w.focusInput(); // TeamSpeak takes its placeholder out, then the emoji goes in
        QTRY_COMPARE(w.input->toPlainText(), kGrin);
        QVERIFY(!inputs.hasPending(w.input));
    }

    // A caption from the input (Ctrl+V) or a reply's text reads past the placeholder: typedText(). (An
    // input never seen changing focus, with no placeholder learned anywhere, reads as it is: it can't get
    // Ctrl+V or Enter without the focus, and a focus-in teaches the placeholder.)
    void typedTextNeverThePlaceholder_data()
    {
        QTest::addColumn<bool>("grey");
        QTest::newRow("grey") << true;
        QTest::newRow("text colour") << false;
    }
    void typedTextNeverThePlaceholder()
    {
        QFETCH(bool, grey);
        Window     w(grey);
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        QCOMPARE(inputs.typedText(w.input), QString());
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QCOMPARE(inputs.typedText(w.input), QString());
        // A second input (another server tab), its placeholder there since before: known from the first.
        auto* second = new ChatLineEdit(grey, &w.window);
        second->show();
        inputs.attach(second);
        QCOMPARE(inputs.typedText(second), QString());
        // Typed text reads as typed, with and without the focus.
        w.focusInput();
        QTest::keyClicks(w.input, QStringLiteral("a caption"));
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("a caption"));
        w.focusChat();
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("a caption"));
        // Even the placeholder's words, typed.
        w.focusInput();
        w.input->selectAll();
        QTest::keyClicks(w.input, kPlaceholder);
        w.focusChat();
        QCOMPARE(inputs.typedText(w.input), kPlaceholder);
    }

    // The emoji picker of the input (its button), with the input empty and the chat focused: one pick,
    // and with Shift two picks then Esc. The input holds exactly the emoji picked.
    void pickerIntoTheEmptyInput()
    {
        Window    w;
        ChatEmoji chat;
        chat.attachInput(w.input);
        w.focusInput();
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QWidget* button = w.input->findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton"));
        QVERIFY(button);
        QTRY_VERIFY(button->isVisible());
        for (const bool shift : {false, true}) {
            if (shift) {
                w.input->clear();
                w.focusChat();
                QCOMPARE(w.input->toPlainText(), kPlaceholder);
            }
            QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
            QPointer<EmojiPicker> picker = w.input->findChild<EmojiPicker*>();
            QVERIFY(picker);
            QTRY_VERIFY(picker->isVisible());
            QString expected;
            connect(picker.data(), &EmojiPicker::picked, this, [&expected](int id, bool) { expected += emoji::text(id); });
            picker->setSearchText(QStringLiteral("rocket"));
            auto* search = picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch"));
            QVERIFY(search);
            QTest::keyClick(search, Qt::Key_Return, shift ? Qt::ShiftModifier : Qt::NoModifier);
            if (shift) {
                QVERIFY(picker && picker->isVisible());
                QCOMPARE(w.input->toPlainText(), expected); // in at once, the placeholder out
                QTest::keyClick(search, Qt::Key_Return, Qt::ShiftModifier);
                QCOMPARE(w.input->toPlainText(), expected);
                QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(search)); // typing still searches
                QTest::keyClick(search, Qt::Key_Escape); // clears the search
                QTest::keyClick(search, Qt::Key_Escape); // closes: the focus back to the input
                QTRY_VERIFY(!picker);
            }
            QVERIFY(!expected.isEmpty());
            QTRY_COMPARE(w.input->toPlainText(), expected);
            QTRY_VERIFY(!picker);
            // The input has the window's focus (some platforms deactivate a test's window with its popup).
            QCOMPARE(w.window.focusWidget(), static_cast<QWidget*>(w.input));
        }
    }

    // With the chat redesign, TeamSpeak's own emoticon button (taken over, emojiinput.h) opens the picker
    // instead of a button in the input; its emoji go in through ChatInputs the same way. The input empty
    // and the chat focused: one pick, and with Shift two picks then Esc. Exactly the emoji picked, never
    // TeamSpeak's placeholder with them, and TeamSpeak's own handler never runs.
    void teamSpeaksButtonIntoTheEmptyInput()
    {
        Window w;
        auto*  emoticons = new QToolButton(&w.window);
        emoticons->setObjectName(QStringLiteral("EmoticonButton"));
        emoticons->setToolTip(QStringLiteral("Show Emoticons"));
        w.window.layout()->addWidget(emoticons);
        emoticons->show(); // the window is shown already
        int teamSpeakClicks = 0;
        connect(emoticons, &QToolButton::clicked, this, [&teamSpeakClicks] { ++teamSpeakClicks; });
        ChatEmoji chat;
        chat.attachInput(w.input);
        QVERIFY(chat.input());
        QCOMPARE(chat.input()->takenButton(w.input), static_cast<QAbstractButton*>(emoticons));
        QVERIFY(!chat.input()->fallbackButton(w.input)); // no extra button in the input
        QVERIFY(!w.input->findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton")));
        w.focusInput();
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        for (const bool shift : {false, true}) {
            if (shift) {
                w.input->clear();
                w.focusChat();
                QCOMPARE(w.input->toPlainText(), kPlaceholder);
            }
            QTRY_VERIFY(emoticons->isVisible());
            QTest::mouseClick(emoticons, Qt::LeftButton, Qt::NoModifier, emoticons->rect().center());
            QPointer<EmojiPicker> picker = w.input->findChild<EmojiPicker*>();
            QVERIFY(picker);
            QTRY_VERIFY(picker->isVisible());
            QString expected;
            connect(picker.data(), &EmojiPicker::picked, this, [&expected](int id, bool) { expected += emoji::text(id); });
            picker->setSearchText(QStringLiteral("rocket"));
            auto* search = picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch"));
            QVERIFY(search);
            QTest::keyClick(search, Qt::Key_Return, shift ? Qt::ShiftModifier : Qt::NoModifier);
            if (shift) {
                QVERIFY(picker && picker->isVisible());
                QCOMPARE(w.input->toPlainText(), expected); // in at once, the placeholder out
                QTest::keyClick(search, Qt::Key_Return, Qt::ShiftModifier);
                QCOMPARE(w.input->toPlainText(), expected);
                QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(search)); // typing still searches
                QTest::keyClick(search, Qt::Key_Escape); // clears the search
                QTest::keyClick(search, Qt::Key_Escape); // closes: the focus back to the input
                QTRY_VERIFY(!picker);
            }
            QVERIFY(!expected.isEmpty());
            QTRY_COMPARE(w.input->toPlainText(), expected);
            QTRY_VERIFY(!picker);
            QCOMPARE(w.window.focusWidget(), static_cast<QWidget*>(w.input));
        }
        QCOMPARE(teamSpeakClicks, 0);
    }

    // "Use in the chat input" on an HD emoji in the chat (the chat has the focus, the input is empty).
    void useInTheChatInput()
    {
        if (!emoji::hasColor())
            QSKIP("No colour emoji font here");
        Window w;
        w.chat->append(QStringLiteral("&lt;20:02:13&gt; \"Sara\": party ") + kRocket + QStringLiteral(" tonight"));
        ChatEmoji chat;
        chat.attach(w.chat);
        chat.attachInput(w.input);
        chat.processNow(w.chat);
        const int at = firstHdEmoji(w.chat->document());
        QVERIFY(at >= 0);
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QMenu         menu;
        const auto    actions = chat.emojiActions(&menu, w.chat, pointAt(w.chat, at));
        QAction*      use     = nullptr;
        for (QAction* a : actions) {
            if (a->text() == QStringLiteral("&Use in the chat input"))
                use = a;
        }
        QVERIFY(use);
        use->trigger();
        QTRY_COMPARE(w.input->toPlainText(), kRocket);
        QVERIFY(w.input->hasFocus());
        // Again, now next to the first one (the input has text and the chat the focus).
        w.focusChat();
        use->trigger();
        QTRY_COMPARE(w.input->toPlainText(), kRocket + kRocket);
    }

    // Review of 2.2.1: Ctrl+Z after the emoji went in takes the emoji out, and never brings TeamSpeak's
    // placeholder back as text of the (focused) input, which Enter would send. Also when ChatInputs took
    // the placeholder out itself: no focus to be had, or the focus given and TeamSpeak leaving it in.
    void undoNeverBringsThePlaceholderBack_data()
    {
        QTest::addColumn<bool>("hidden"); // no focus to be had: the safety net without the focus
        QTest::addColumn<int>("takes");   // Takes
        QTest::newRow("TeamSpeak takes it out") << false << static_cast<int>(Takes::Any);
        QTest::newRow("safety net, no focus") << true << static_cast<int>(Takes::Any);
        QTest::newRow("TeamSpeak ignores the plugin's focus") << false << static_cast<int>(Takes::Users);
        QTest::newRow("safety net, focused") << false << static_cast<int>(Takes::TabOnly);
    }
    void undoNeverBringsThePlaceholderBack()
    {
        QFETCH(bool, hidden);
        QFETCH(int, takes);
        Look look;
        look.takes = static_cast<Takes>(takes);
        Window     w(look);
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput(look.takes == Takes::TabOnly ? Qt::TabFocusReason : Qt::MouseFocusReason);
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        if (hidden)
            w.input->hide();
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE_WITH_TIMEOUT(w.input->toPlainText(), kGrin, 4000);
        if (hidden)
            w.input->show();
        w.focusInput();
        QVERIFY(w.input->hasFocus());
        QCOMPARE(w.input->toPlainText(), kGrin);
        for (int i = 0; i < 4; ++i) {
            QTest::keyClick(w.input, Qt::Key_Z, Qt::ControlModifier);
            QVERIFY2(!w.input->toPlainText().contains(kPlaceholder), qPrintable(QStringLiteral("after %1 x Ctrl+Z: \"%2\"").arg(i + 1).arg(w.input->toPlainText())));
            if (i == 0)
                QCOMPARE(w.input->toPlainText(), QString()); // the emoji undone
        }
        // Typing then is in the text colour.
        QTest::keyClicks(w.input, QStringLiteral("ok"));
        QCOMPARE(w.input->toPlainText(), QStringLiteral("ok"));
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("ok"));
    }

    // Review of 2.2.1: text the user deletes right after a focus-in (the input's own menu: Delete, Cut)
    // is not TeamSpeak's placeholder. Learned as one, the same words typed again read as nothing and an
    // emoji took their place when typing has a colour of TeamSpeak's own.
    void deletedTextIsNotThePlaceholder_data()
    {
        QTest::addColumn<bool>("ownColour"); // TeamSpeak gives typing a colour of its own
        QTest::newRow("typing in the text colour") << false;
        QTest::newRow("typing in a colour of TeamSpeak's") << true;
    }
    void deletedTextIsNotThePlaceholder()
    {
        QFETCH(bool, ownColour);
        Look look;
        if (ownColour)
            look.typed = QColor(0x10, 0x60, 0x30);
        Window     w(look);
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        w.focusChat();
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        w.focusInput();
        QTest::keyClicks(w.input, QStringLiteral("hello"));
        // Right-click, Delete: the menu takes the focus and gives it back, then the text goes.
        w.other->setFocus(Qt::PopupFocusReason);
        w.input->setFocus(Qt::PopupFocusReason);
        QTextCursor all(w.input->document());
        all.select(QTextCursor::Document);
        all.removeSelectedText();
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        QVERIFY(!inputs.showsPlaceholder(w.input));
        // The same words again (pasted back), and the focus to the chat.
        QTest::keyClicks(w.input, QStringLiteral("hello"));
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), QStringLiteral("hello"));
        QVERIFY(!inputs.showsPlaceholder(w.input));
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("hello"));
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE(w.input->toPlainText(), QStringLiteral("hello") + kGrin);
    }

    // Review of 2.2.1: only the text TeamSpeak puts into the input as it loses the focus is its
    // placeholder, not text that comes in a moment later (a client dragged from the channel list and
    // dropped onto the input). That text is the user's: it reads as typed and an emoji goes in next to it.
    void laterTextAfterTheFocusOutIsNotThePlaceholder()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        w.focusChat(); // the placeholder in (and learned)
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        const QString link = QStringLiteral("[URL=client://5/abc]Sara[/URL]");
        w.input->dropText(link); // at once: within TeamSpeak's moments after a focus change
        QCOMPARE(w.input->toPlainText(), link);
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        QVERIFY(!inputs.showsPlaceholder(w.input));
        QCOMPARE(inputs.typedText(w.input), link);
        inputs.insert(w.input, kGrin);
        QTRY_VERIFY(w.input->toPlainText().contains(kGrin));
        QCOMPARE(w.input->toPlainText(), link + kGrin);
    }

    // Review of 2.2.1: Ctrl+E in the focused input. The picker takes the focus (a popup's focus-out, and
    // an empty TeamSpeak input shows its placeholder then): the input holds exactly what was there plus
    // the emoji.
    void pickerFromTheFocusedInput_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("empty") << QString();
        QTest::newRow("typed text") << QStringLiteral("hi ");
    }
    void pickerFromTheFocusedInput()
    {
        QFETCH(QString, typed);
        Window    w;
        ChatEmoji chat;
        chat.attachInput(w.input);
        w.focusInput();
        QTest::keyClicks(w.input, typed);
        QCOMPARE(w.input->toPlainText(), typed);
        QTest::keyClick(w.input, Qt::Key_E, Qt::ControlModifier);
        QPointer<EmojiPicker> picker = w.input->findChild<EmojiPicker*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        QString expected = typed;
        connect(picker.data(), &EmojiPicker::picked, this, [&expected](int id, bool) { expected += emoji::text(id); });
        picker->setSearchText(QStringLiteral("rocket"));
        auto* search = picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch"));
        QVERIFY(search);
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_VERIFY(!picker);
        QVERIFY(expected.size() > typed.size());
        QTRY_COMPARE(w.input->toPlainText(), expected);
        QCOMPARE(w.window.focusWidget(), static_cast<QWidget*>(w.input));
    }

    // Review of 2.2.1: TeamSpeak's placeholder in other languages (learned, not known): the emoji goes in
    // alone, and the same words typed are kept.
    void otherLanguages_data()
    {
        QTest::addColumn<QString>("words");
        QTest::addColumn<bool>("seen");
        const QString german  = QStringLiteral("Chatnachricht eingeben...");
        const QString persian = QStringLiteral("\u067E\u06CC\u0627\u0645 \u0631\u0627 \u0648\u0627\u0631\u062F \u06A9\u0646\u06CC\u062F...");
        const QString chinese = QStringLiteral("\u8F93\u5165\u804A\u5929\u6D88\u606F...");
        QTest::newRow("German, seen") << german << true;
        QTest::newRow("German, never seen") << german << false;
        QTest::newRow("Persian, seen") << persian << true;
        QTest::newRow("Chinese, never seen") << chinese << false;
    }
    void otherLanguages()
    {
        QFETCH(QString, words);
        QFETCH(bool, seen);
        Look look;
        look.text = words;
        Window     w(look);
        ChatInputs inputs;
        inputs.attach(w.input);
        if (seen) {
            w.focusInput();
            w.focusChat();
            QCOMPARE(inputs.placeholder(w.input), words);
            QCOMPARE(inputs.typedText(w.input), QString());
        }
        QCOMPARE(w.input->toPlainText(), words);
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE(w.input->toPlainText(), kGrin);
        QCOMPARE(inputs.placeholder(w.input), words);
        // The same words typed are the user's.
        w.input->selectAll();
        typeText(w.input, words);
        QCOMPARE(w.input->toPlainText(), words);
        w.focusChat();
        QCOMPARE(inputs.typedText(w.input), words);
        inputs.insert(w.input, kRocket);
        QTRY_COMPARE(w.input->toPlainText(), words + kRocket);
    }

    // Review of 2.2.1: the picker closing while TeamSpeak takes its placeholder out only after the
    // focus-in (from the event loop): one pick, and Shift picks then Esc. Exactly the emoji picked.
    void pickerWithLateRemoval_data()
    {
        QTest::addColumn<bool>("shift");
        QTest::newRow("one pick") << false;
        QTest::newRow("Shift picks, then Esc") << true;
    }
    void pickerWithLateRemoval()
    {
        QFETCH(bool, shift);
        Look look;
        look.late = true;
        Window    w(look);
        ChatEmoji chat;
        chat.attachInput(w.input);
        w.focusInput();
        w.focusChat();
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        QWidget* button = w.input->findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton"));
        QVERIFY(button);
        QTRY_VERIFY(button->isVisible());
        QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
        QPointer<EmojiPicker> picker = w.input->findChild<EmojiPicker*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        QString expected;
        connect(picker.data(), &EmojiPicker::picked, this, [&expected](int id, bool) { expected += emoji::text(id); });
        picker->setSearchText(QStringLiteral("rocket"));
        auto* search = picker->findChild<QLineEdit*>(QStringLiteral("tsmediaEmojiSearch"));
        QVERIFY(search);
        QTest::keyClick(search, Qt::Key_Return, shift ? Qt::ShiftModifier : Qt::NoModifier);
        if (shift) {
            QCoreApplication::processEvents(); // TeamSpeak's late removal
            QTest::keyClick(search, Qt::Key_Return, Qt::ShiftModifier);
            QCoreApplication::processEvents();
            QVERIFY(picker && picker->isVisible());
            QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(search));
            QVERIFY(!w.input->toPlainText().contains(kPlaceholder));
            QTest::keyClick(search, Qt::Key_Escape); // clears the search
            QTest::keyClick(search, Qt::Key_Escape); // closes
        }
        QTRY_VERIFY(!picker);
        QVERIFY(!expected.isEmpty());
        QTRY_COMPARE(w.input->toPlainText(), expected);
        QCOMPARE(w.window.focusWidget(), static_cast<QWidget*>(w.input));
    }

    // Review of 2.2.1: TeamSpeak's window minimized (Windows may keep it from the front): the emoji
    // never goes in next to the placeholder, and is there once the window is back.
    void minimizedWindow()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        w.focusInput();
        w.focusChat();
        w.window.showMinimized();
        QCoreApplication::processEvents();
        inputs.insert(w.input, kGrin);
        for (int i = 0; i < 60 && inputs.hasPending(w.input); ++i) {
            QTest::qWait(25);
            QVERIFY2(!w.input->toPlainText().contains(kGrin) || !w.input->toPlainText().contains(kPlaceholder), qPrintable(w.input->toPlainText()));
        }
        w.window.showNormal();
        w.focusInput();
        QTRY_COMPARE_WITH_TIMEOUT(w.input->toPlainText(), kGrin, 3000);
        QVERIFY(!inputs.hasPending(w.input));
    }

    // Review of 2.2.1: a TeamSpeak that takes its placeholder out only on its user's own focus-in (a
    // click's), not on the plugin's: the emoji went in next to a placeholder never seen before ("<emoji>
    // Enter Chat Message...", the bug again). It gets a click's focus-in, takes the placeholder out itself
    // (its own state too), and the emoji goes in alone, also from the picker with Shift.
    void focusReasonTeamSpeakIgnores_data()
    {
        QTest::addColumn<bool>("grey");
        QTest::addColumn<bool>("seen");
        QTest::addColumn<bool>("keep");
        QTest::newRow("grey, never seen") << true << false << false;
        QTest::newRow("text colour, never seen") << false << false << false;
        QTest::newRow("grey, seen") << true << true << false;
        QTest::newRow("text colour, seen") << false << true << false;
        QTest::newRow("Shift in the picker, never seen") << true << false << true;
    }
    void focusReasonTeamSpeakIgnores()
    {
        QFETCH(bool, grey);
        QFETCH(bool, seen);
        QFETCH(bool, keep);
        Look look;
        look.takes = Takes::Users;
        look.grey  = grey;
        Window     w(look);
        ChatInputs inputs;
        inputs.attach(w.input);
        if (seen) {
            w.focusInput();
            w.focusChat();
        }
        if (keep)
            w.other->setFocus(Qt::MouseFocusReason); // the picker's search field
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        inputs.insert(w.input, kGrin, keep);
        QTRY_COMPARE(w.input->toPlainText(), kGrin);
        QVERIFY(!w.input->showing()); // taken out by TeamSpeak itself
        QVERIFY(!inputs.hasPending(w.input));
        QCOMPARE(inputs.placeholder(w.input), kPlaceholder);
        if (keep)
            QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(w.other));
        else
            QVERIFY(w.input->hasFocus());
        // Typing goes on after it, not grey.
        w.focusInput();
        QTest::keyClicks(w.input, QStringLiteral("hi"));
        QCOMPARE(w.input->toPlainText(), kGrin + QStringLiteral("hi"));
        QCOMPARE(inputs.typedText(w.input), kGrin + QStringLiteral("hi"));
    }

    // Review of 2.2.1: the plugin attached while the unfocused input already holds a draft of the user's
    // (loaded mid-session, nothing learned): the draft is never taken for TeamSpeak's placeholder, also not
    // after the click's focus-in; the emoji goes in at the cursor.
    void draftBeforeTheAttach_data()
    {
        QTest::addColumn<int>("takes");
        QTest::newRow("TeamSpeak takes it out") << static_cast<int>(Takes::Any);
        QTest::newRow("TeamSpeak ignores the plugin's focus") << static_cast<int>(Takes::Users);
    }
    void draftBeforeTheAttach()
    {
        QFETCH(int, takes);
        Look look;
        look.takes = static_cast<Takes>(takes);
        Window w(look);
        w.focusInput();
        QTest::keyClicks(w.input, QStringLiteral("draft"));
        w.focusChat();
        ChatInputs inputs;
        inputs.attach(w.input);
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("draft"));
        inputs.insert(w.input, kGrin);
        QTRY_COMPARE_WITH_TIMEOUT(w.input->toPlainText(), QStringLiteral("draft") + kGrin, 3000);
        QVERIFY(!inputs.hasPending(w.input));
        QCOMPARE(inputs.typedText(w.input), QStringLiteral("draft") + kGrin);
    }

    // Review of 2.2.1: an emoji waiting for the input's focus, and the user types as the focus comes back:
    // both go in, at the cursor, and nothing of the placeholder.
    void typingWhileTheEmojiWaits()
    {
        Window     w;
        ChatInputs inputs;
        inputs.attach(w.input);
        w.input->hide();
        inputs.insert(w.input, kGrin);
        QTest::qWait(1500); // past the tries: it waits for the next focus-in
        QVERIFY(inputs.hasPending(w.input));
        QCOMPARE(w.input->toPlainText(), kPlaceholder);
        w.input->show();
        w.focusInput(); // TeamSpeak takes its placeholder out; the emoji goes in from the event loop
        QTest::keyClicks(w.input, QStringLiteral("ab"));
        QTRY_VERIFY(!inputs.hasPending(w.input));
        QCOMPARE(w.input->toPlainText(), QStringLiteral("ab") + kGrin);
    }
};

TSMEDIA_REGISTER_TEST(TestChatInput)

#include "tst_chatinput.moc"
