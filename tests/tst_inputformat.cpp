// 2.7 F1: formatting in TeamSpeak's chat input (inputformat.h) on a ChatLineEdit like TeamSpeak's: its right-click
// menu is Qt's standard text menu plus TeamSpeak's Format submenu (Bold, Italic, Underline, Color with their
// Alt+Shift keys as QAction shortcuts), deleted right after it closes. With a selection the four entries come after
// "Select All" between separators, ticked when applied; Format gets Spoiler after Underline (found by keys, any title);
// a menu opened one turn late is still caught; each action is one Ctrl+Z; Alt+Shift+S wraps (or inserts an empty
// pair); Ctrl+B is not taken; nothing happens over TeamSpeak's placeholder; the catcher is disarmed after one menu and
// at shutdown.

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>
#include <QtTest>

#include "inputformat.h"
#include "testmain.h"

namespace {

// TeamSpeak's chat input (inherits("ChatLineEdit") is what the catcher looks for).
class ChatLineEdit : public QTextEdit
{
    Q_OBJECT

  public:
    enum class Menu { Now, Late, None };
    explicit ChatLineEdit(Menu mode = Menu::Now, const QString& formatTitle = QStringLiteral("Format"), QWidget* parent = nullptr)
        : QTextEdit(parent)
        , m_mode(mode)
        , m_title(formatTitle)
    {
        setObjectName(QStringLiteral("ChatLineEdit"));
        setAcceptRichText(false);
    }
    QPointer<QMenu> menu; // the one shown last (deleted when it closes, like TeamSpeak's)
    int             menus = 0;

    void select(int from, int to)
    {
        QTextCursor c = textCursor();
        c.setPosition(from);
        c.setPosition(to, QTextCursor::KeepAnchor);
        setTextCursor(c);
    }
    QString selected() const { return textCursor().selectedText(); }

  protected:
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (m_mode == Menu::None)
            return;
        if (m_mode == Menu::Late) {
            const QPoint at = e->globalPos();
            QMetaObject::invokeMethod(this, [this, at] { open(at); }, Qt::QueuedConnection);
            return;
        }
        open(e->globalPos());
    }

  private:
    void open(const QPoint& at)
    {
        QMenu* m = createStandardContextMenu();
        m->addSeparator();
        m->addAction(QStringLiteral("Paste && Send"));
        QMenu* format = m->addMenu(m_title);
        const char* names[] = {"Bold", "Italic", "Underline", "Color"};
        const int   keys[]  = {Qt::Key_B, Qt::Key_I, Qt::Key_U, Qt::Key_C};
        for (int i = 0; i < 4; ++i) {
            QAction* a = format->addAction(QString::fromLatin1(names[i]));
            a->setShortcut(QKeySequence(static_cast<int>(Qt::ALT | Qt::SHIFT) | keys[i]));
            a->setShortcutContext(Qt::WidgetShortcut);
        }
        connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
        menu = m;
        ++menus;
        m->popup(at);
    }

    Menu    m_mode;
    QString m_title;
};

QString plain(const QString& text)
{
    QString out = text;
    out.remove(QLatin1Char('&'));
    return out.section(QLatin1Char('\t'), 0, 0);
}

QStringList texts(const QMenu* menu)
{
    QStringList out;
    for (const QAction* a : menu->actions())
        out << (a->isSeparator() ? QStringLiteral("---") : plain(a->text()));
    return out;
}

void mouseMenu(ChatLineEdit* edit)
{
    QContextMenuEvent e(QContextMenuEvent::Mouse, QPoint(5, 5), edit->viewport()->mapToGlobal(QPoint(5, 5)));
    QApplication::sendEvent(edit->viewport(), &e);
}

void keyboardMenu(ChatLineEdit* edit)
{
    QContextMenuEvent e(QContextMenuEvent::Keyboard, QPoint(5, 5), edit->mapToGlobal(QPoint(5, 5)));
    QApplication::sendEvent(edit, &e);
}

// A key as the shortcut map sends it: ShortcutOverride first (taken: accepted), then the key press.
bool key(QWidget* w, int k, Qt::KeyboardModifiers mods, bool* taken = nullptr)
{
    QKeyEvent over(QEvent::ShortcutOverride, k, mods);
    over.ignore();
    QApplication::sendEvent(w, &over);
    if (taken)
        *taken = over.isAccepted();
    QKeyEvent press(QEvent::KeyPress, k, mods);
    return QApplication::sendEvent(w, &press);
}

struct Window {
    QWidget       window;
    ChatLineEdit* edit = nullptr;
    InputFormat   format;
    explicit Window(ChatLineEdit::Menu mode = ChatLineEdit::Menu::Now, const QString& title = QStringLiteral("Format"))
    {
        edit = new ChatLineEdit(mode, title, &window);
        edit->resize(300, 40);
        window.resize(320, 60);
        window.show();
        format.attach(edit);
        edit->setPlainText(QStringLiteral("Hello world"));
    }
    ~Window()
    {
        if (edit->menu)
            delete edit->menu.data();
    }
};

} // namespace

class TestInputFormat : public QObject
{
    Q_OBJECT

  private slots:
    void blockAfterSelectAll()
    {
        Window w;
        w.edit->select(6, 11);
        mouseMenu(w.edit);
        QVERIFY(w.edit->menu);
        QVERIFY(!w.format.armed()); // disarmed when caught
        const QStringList t   = texts(w.edit->menu);
        const int         all = t.indexOf(QStringLiteral("Select All"));
        QVERIFY2(all >= 0, qPrintable(t.join(QLatin1Char('|'))));
        QCOMPARE(t.mid(all + 1, 6), (QStringList{QStringLiteral("---"), QStringLiteral("Bold"), QStringLiteral("Italic"), QStringLiteral("Underline"),
                                                QStringLiteral("Spoiler"), QStringLiteral("---")}));
        // Qt's own entries stay where they were (Undo, Redo, Cut, Copy, Paste first).
        QVERIFY(t.indexOf(QStringLiteral("Copy")) < all);
        // Format: Spoiler after Underline.
        QMenu* format = nullptr;
        for (QAction* a : w.edit->menu->actions()) {
            if (a->menu())
                format = a->menu();
        }
        QVERIFY(format);
        QCOMPARE(texts(format), (QStringList{QStringLiteral("Bold"), QStringLiteral("Italic"), QStringLiteral("Underline"), QStringLiteral("Spoiler"),
                                             QStringLiteral("Color")}));
        // The key text after a tab, never a shortcut (TeamSpeak has QShortcuts for Alt+Shift+B/I/U/C).
        QCOMPARE(w.format.action(textformat::Format::Bold)->text().section(QLatin1Char('\t'), 1),
                 QKeySequence(static_cast<int>(Qt::ALT | Qt::SHIFT) | Qt::Key_B).toString(QKeySequence::NativeText));
        for (const textformat::Format f : {textformat::Format::Bold, textformat::Format::Italic, textformat::Format::Underline, textformat::Format::Spoiler})
            QVERIFY(w.format.action(f)->shortcut().isEmpty());
        QVERIFY(w.format.formatSpoilerAction()->shortcut().isEmpty());
        QVERIFY(w.format.action(textformat::Format::Bold)->isCheckable());
        QVERIFY(!w.format.action(textformat::Format::Bold)->isChecked());
    }

    void withoutSelectionOnlyFormat()
    {
        Window w;
        w.edit->select(3, 3);
        mouseMenu(w.edit);
        QVERIFY(w.edit->menu);
        QVERIFY(!texts(w.edit->menu).contains(QStringLiteral("Bold")));
        bool inFormat = false;
        for (QAction* a : w.edit->menu->actions()) {
            if (a->menu())
                inFormat = a->menu()->actions().contains(w.format.formatSpoilerAction());
        }
        QVERIFY(inFormat);
        // Only white space selected: no block either.
        w.edit->setPlainText(QStringLiteral("a    b"));
        w.edit->select(1, 5);
        mouseMenu(w.edit);
        QVERIFY(!texts(w.edit->menu).contains(QStringLiteral("Bold")));
    }

    void formatFoundByItsKeys()
    {
        Window w(ChatLineEdit::Menu::Now, QStringLiteral("Formatieren"));
        w.edit->select(0, 5);
        keyboardMenu(w.edit); // the Menu key / Shift+F10 come to the input itself
        QVERIFY(w.edit->menu);
        QMenu* format = nullptr;
        for (QAction* a : w.edit->menu->actions()) {
            if (a->menu())
                format = a->menu();
        }
        QVERIFY(format);
        QCOMPARE(texts(format).value(3), QStringLiteral("Spoiler"));
    }

    void ticksFollowTheText()
    {
        Window w;
        w.edit->setPlainText(QStringLiteral("Hello [B]world[/B]"));
        w.edit->select(9, 14);
        mouseMenu(w.edit);
        QVERIFY(w.format.action(textformat::Format::Bold)->isChecked());
        QVERIFY(!w.format.action(textformat::Format::Italic)->isChecked());
        QVERIFY(!w.format.action(textformat::Format::Spoiler)->isChecked());
    }

    void triggeringIsOneUndoStep()
    {
        Window w;
        w.edit->select(6, 11);
        mouseMenu(w.edit);
        w.format.action(textformat::Format::Bold)->trigger();
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello [B]world[/B]"));
        QCOMPARE(w.edit->selected(), QStringLiteral("world"));
        // Again from a new menu (the tick is set): it comes off.
        delete w.edit->menu.data();
        mouseMenu(w.edit);
        QVERIFY(w.format.action(textformat::Format::Bold)->isChecked());
        w.format.action(textformat::Format::Bold)->trigger();
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello world"));
        w.edit->undo();
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello [B]world[/B]"));
        w.edit->undo();
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello world"));
        // Format > Spoiler.
        w.edit->select(0, 5);
        mouseMenu(w.edit);
        w.format.formatSpoilerAction()->trigger();
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("[spoiler]Hello[/spoiler] world"));
    }

    // TeamSpeak deletes its menu right after it closes: our actions are ours.
    void actionsSurviveTheMenu()
    {
        Window w;
        w.edit->select(6, 11);
        mouseMenu(w.edit);
        QPointer<QAction> bold = w.format.action(textformat::Format::Bold);
        delete w.edit->menu.data();
        QVERIFY(bold);
        mouseMenu(w.edit);
        QVERIFY(texts(w.edit->menu).contains(QStringLiteral("Bold")));
        QCOMPARE(w.edit->menus, 2);
    }

    // Critique L9: a menu TeamSpeak opens one event-loop turn late is still caught.
    void lateMenuIsCaught()
    {
        Window w(ChatLineEdit::Menu::Late);
        w.edit->select(6, 11);
        mouseMenu(w.edit);
        QVERIFY(!w.edit->menu);
        QVERIFY(w.format.armed());
        QCoreApplication::processEvents(); // the posted menu, then the first hop
        QVERIFY(w.edit->menu);
        QVERIFY(texts(w.edit->menu).contains(QStringLiteral("Spoiler")));
        QVERIFY(!w.format.armed());
    }

    void disarmedWithoutAMenuAndAtShutdown()
    {
        {
            Window w(ChatLineEdit::Menu::None);
            mouseMenu(w.edit);
            QVERIFY(w.format.armed());
            QTRY_VERIFY_WITH_TIMEOUT(!w.format.armed(), 1000); // two hops
        }
        {
            Window w(ChatLineEdit::Menu::None);
            mouseMenu(w.edit);
            QVERIFY(w.format.armed());
            w.format.shutdown();
            QVERIFY(!w.format.armed());
            // Its inputs are let go too: no menu event arms it any more.
            mouseMenu(w.edit);
            QVERIFY(!w.format.armed());
        }
    }

    void altShiftS()
    {
        Window w;
        w.edit->setFocus();
        w.edit->select(6, 11);
        bool taken = false;
        QVERIFY(key(w.edit, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier, &taken));
        QVERIFY(taken);
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello [spoiler]world[/spoiler]"));
        QCOMPARE(w.edit->selected(), QStringLiteral("world"));
        // Again: off.
        key(w.edit, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello world"));
        // Nothing selected: an empty pair, the cursor between.
        w.edit->select(5, 5);
        key(w.edit, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello[spoiler][/spoiler] world"));
        QCOMPARE(w.edit->textCursor().position(), 14);
    }

    void ctrlBIsNotTaken()
    {
        Window w;
        w.edit->select(6, 11);
        bool taken = true;
        key(w.edit, Qt::Key_B, Qt::ControlModifier, &taken);
        QVERIFY(!taken); // TeamSpeak's Bookmarks keep it
        key(w.edit, Qt::Key_I, Qt::ControlModifier, &taken);
        QVERIFY(!taken);
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Hello world"));
    }

    void nothingOverThePlaceholder()
    {
        Window w;
        w.edit->setPlainText(QStringLiteral("Enter Chat Message..."));
        w.edit->setProperty("helpTextVisible", true);
        w.edit->select(0, 5);
        QVERIFY(!w.format.run(w.edit, textformat::Format::Bold));
        key(w.edit, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("Enter Chat Message..."));
        mouseMenu(w.edit);
        QVERIFY(!texts(w.edit->menu).contains(QStringLiteral("Bold")));
        // ... and the hook (ChatInputs) says so too.
        w.edit->setProperty("helpTextVisible", false);
        w.format.showsPlaceholder = [](const QTextEdit*) { return true; };
        QVERIFY(!w.format.run(w.edit, textformat::Format::Bold));
        w.format.showsPlaceholder = nullptr;
        QVERIFY(w.format.run(w.edit, textformat::Format::Bold));
    }

    void multiLineSelection()
    {
        Window w;
        w.edit->setPlainText(QStringLiteral("one\ntwo"));
        w.edit->select(0, 7);
        QVERIFY(w.format.run(w.edit, textformat::Format::Italic));
        QCOMPARE(w.edit->toPlainText(), QStringLiteral("[I]one\ntwo[/I]"));
        QCOMPARE(w.edit->document()->blockCount(), 2);
    }
};

TSMEDIA_REGISTER_TEST(TestInputFormat)
#include "tst_inputformat.moc"
