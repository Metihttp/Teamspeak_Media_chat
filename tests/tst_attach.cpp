// 2.3 attach: the paperclip at the left edge inside the chat input (EmojiInput's left slot) and its menu:
// the registry's order, the button's place and lifetime, the menu's entries, keyboard, Esc and triggers.

#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QPointer>
#include <QTextEdit>
#include <QtTest>

#include "attachart.h"
#include "attachbutton.h"
#include "attachmenu.h"
#include "attachpopup.h"
#include "emojiinput.h"
#include "testmain.h"

namespace {

QMargins marginsOf(QTextEdit* edit)
{
    const QRect vp    = edit->viewport()->geometry();
    const QRect inner = edit->contentsRect();
    return QMargins(vp.left() - inner.left(), vp.top() - inner.top(), inner.right() - vp.right(), inner.bottom() - vp.bottom());
}

void closePopups()
{
    while (QWidget* p = QApplication::activePopupWidget())
        p->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

attachmenu::Entry entry(const char* id, int order, int* hits, bool enabled = true)
{
    attachmenu::Entry e;
    e.id      = QString::fromLatin1(id);
    e.order   = order;
    e.text    = [id] { return QString::fromLatin1(id); };
    e.icon    = [] { return attachart::icon(attachart::Glyph::Photo); };
    e.enabled = [enabled] { return enabled; };
    e.trigger = [hits](QWidget*) { ++*hits; };
    return e;
}

void sendKey(QWidget* w, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

} // namespace

class TestAttach : public QObject
{
    Q_OBJECT

  private slots:
    void cleanup()
    {
        closePopups();
        for (const attachmenu::Entry& e : attachmenu::entries())
            attachmenu::removeEntry(e.id);
    }

    // The registry: sorted by order, an id twice replaces, removeEntry takes it out.
    void registryOrder()
    {
        int hits = 0;
        attachmenu::addEntry(entry("poll", 30, &hits));
        attachmenu::addEntry(entry("photo", 10, &hits));
        attachmenu::addEntry(entry("music", 40, &hits));
        attachmenu::addEntry(entry("document", 20, &hits));
        attachmenu::addEntry(entry("photo", 10, &hits)); // again: replaced, not twice
        QStringList ids;
        for (const attachmenu::Entry& e : attachmenu::entries())
            ids << e.id;
        QCOMPARE(ids, (QStringList{QStringLiteral("photo"), QStringLiteral("document"), QStringLiteral("poll"), QStringLiteral("music")}));
        attachmenu::removeEntry(QStringLiteral("music"));
        QCOMPARE(attachmenu::entries().size(), 3);
        attachmenu::addEntry(attachmenu::Entry()); // no id: ignored
        QCOMPARE(attachmenu::entries().size(), 3);
    }

    // EmojiInput gives the paperclip a slot at the input's left edge (the text starts right of it), and
    // takes it away again with the input's margins.
    void buttonSitsInTheLeftSlot()
    {
        QWidget window;
        window.resize(520, 80);
        auto* row  = new QHBoxLayout(&window);
        auto* edit = new QTextEdit(&window);
        edit->setFixedHeight(28);
        row->addWidget(edit);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QMargins before = marginsOf(edit);
        {
            EmojiInput input;
            input.attach(edit);
            AttachButton* button = AttachButton::of(edit);
            QVERIFY(button);
            QTRY_VERIFY(button->isVisible());
            QCOMPARE(marginsOf(edit).left(), before.left() + 30);
            QCOMPARE(button->geometry().right() + 1, edit->viewport()->geometry().left());
            QCOMPARE(button->objectName(), QStringLiteral("tsmediaAttachButton"));
            QCOMPARE(button->toolTip(), QStringLiteral("Attach"));
            QVERIFY(button->accessibleName().contains(QStringLiteral("live music"))); // 2.3 ux review: all it offers
            QCOMPARE(button->focusPolicy(), Qt::NoFocus); // typing stays in the input
            // The input gets taller: the paperclip goes to its bottom, like the microphone.
            edit->setFixedHeight(90);
            QTRY_COMPARE(button->height(), edit->viewport()->height());
            QVERIFY(button->iconRect().bottom() > button->height() / 2);
        }
        QVERIFY(!AttachButton::of(edit));
        // Its own margins back (the right one also holds a scroll bar, which the taller input no longer needs).
        QCOMPARE(marginsOf(edit).left(), before.left());
        QCOMPARE(marginsOf(edit).top(), before.top());
    }

    // A click opens the menu with the registered entries in order; a disabled one is skipped by the keys;
    // Enter runs the focused entry after the menu closed; Esc closes without running anything.
    void menuKeyboardAndTriggers()
    {
        int photo = 0, document = 0, poll = 0, music = 0;
        attachmenu::addEntry(entry("photo", 10, &photo));
        attachmenu::addEntry(entry("document", 20, &document));
        attachmenu::addEntry(entry("poll", 30, &poll));
        attachmenu::addEntry(entry("music", 40, &music, false));
        QWidget window;
        window.resize(520, 80);
        auto* edit = new QTextEdit(&window);
        edit->setGeometry(10, 40, 480, 28);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        EmojiInput input;
        input.attach(edit);
        AttachButton* button = AttachButton::of(edit);
        QVERIFY(button);

        button->openMenu();
        QPointer<AttachPopup> popup = button->popup();
        QVERIFY(popup);
        QCOMPARE(popup->objectName(), QStringLiteral("tsmediaAttachMenu"));
        QCOMPARE(popup->count(), 4);
        QCOMPARE(popup->entryId(0), QStringLiteral("photo"));
        QCOMPARE(popup->entryId(3), QStringLiteral("music"));
        QVERIFY(!popup->item(3)->isEnabled());
        // 2.3 ux review: a grey entry says why (pointer and screen readers); the others don't need to.
        QVERIFY(!popup->item(3)->toolTip().isEmpty());
        QCOMPARE(popup->item(3)->accessibleDescription(), popup->item(3)->toolTip());
        QVERIFY(popup->item(0)->toolTip().isEmpty());
        QCOMPARE(popup->currentIndex(), 0); // the first entry, ready for the keys
        QVERIFY(!popup->keyboardUsed());
        sendKey(popup, Qt::Key_Down); // 2.3 live test: the first Down marks the first entry, it doesn't skip it
        QVERIFY(popup->keyboardUsed());
        QCOMPARE(popup->currentIndex(), 0);
        sendKey(popup, Qt::Key_Down);
        QCOMPARE(popup->currentIndex(), 1);
        sendKey(popup, Qt::Key_End); // the last enabled one, not the disabled music
        QCOMPARE(popup->currentIndex(), 2);
        sendKey(popup, Qt::Key_Down); // wraps past the disabled one
        QCOMPARE(popup->currentIndex(), 0);
        sendKey(popup, Qt::Key_Up);
        QCOMPARE(popup->currentIndex(), 2);
        sendKey(popup, Qt::Key_Return);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(popup.isNull());
        QTRY_COMPARE(poll, 1); // after the menu closed
        QCOMPARE(photo + document + music, 0);

        button->openMenu();
        popup = button->popup();
        QVERIFY(popup);
        sendKey(popup, Qt::Key_Up); // a first Up: the last enabled entry
        QCOMPARE(popup->currentIndex(), 2);
        sendKey(popup, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(popup.isNull());
        QCOMPARE(photo + document + poll + music, 1);

        // A click on the paperclip while the menu is open closes it.
        button->openMenu();
        QVERIFY(button->menuOpen());
        button->openMenu();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!button->menuOpen());
        // A click on an entry runs it.
        button->openMenu();
        popup = button->popup();
        QTest::mouseClick(popup->item(1), Qt::LeftButton);
        QTRY_COMPARE(document, 1);
    }

    // The glyphs draw something in both looks, and the icon has the usual scales.
    void glyphsDraw()
    {
        for (const attachart::Glyph g : {attachart::Glyph::Paperclip, attachart::Glyph::Photo, attachart::Glyph::Document, attachart::Glyph::Poll}) {
            QImage img(48, 48, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            attachart::draw(p, QRectF(0, 0, 48, 48), g, Qt::black);
            p.end();
            int opaque = 0;
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x)
                    opaque += qAlpha(img.pixel(x, y)) > 128 ? 1 : 0;
            QVERIFY2(opaque > 60, qPrintable(QString::number(opaque)));
            QVERIFY(opaque < 48 * 48 / 2);
        }
        QVERIFY(attachart::icon(attachart::Glyph::Poll).availableSizes().size() >= 4);
        for (const bool dark : {false, true}) {
            QImage img(24, 24, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter            p(&img);
            AttachButton::Look look;
            look.dark = dark;
            AttachButton::paintIcon(p, QRectF(0, 0, 24, 24), look);
        }
    }
};

TSMEDIA_REGISTER_TEST(TestAttach)
#include "tst_attach.moc"
