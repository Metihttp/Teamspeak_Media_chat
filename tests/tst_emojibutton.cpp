// Chat redesign: TeamSpeak's own emoticon button shows TS Media's HD face and opens TS Media's picker (the
// takeover, emojiinput.h). A fake MainWindowChatWidget like TeamSpeak's: a ChatLineEdit, the
// "EmoticonButton" tool button styled with dark.qss's QToolButton rules, and a decoy EmoticonsDisplay whose
// cells use the same object name. The right button is found, TeamSpeak never sees our clicks, the fallback
// follows the button's visibility, everything is given back, and the skin's painting stays TeamSpeak's.

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalSpy>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

#include "emojiinput.h"
#include "emojipicker.h"
#include "emojiprefs.h"
#include "settings.h"
#include "testmain.h"

class ChatLineEdit : public QTextEdit
{
    Q_OBJECT
  public:
    using QTextEdit::QTextEdit;
};

class MainWindowChatWidget : public QWidget
{
    Q_OBJECT
  public:
    using QWidget::QWidget;
};

// TeamSpeak's emoticon list: its cells may use the button's object name.
class EmoticonsDisplay : public QWidget
{
    Q_OBJECT
  public:
    explicit EmoticonsDisplay(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        auto* cell = new QToolButton(this);
        cell->setObjectName(QStringLiteral("EmoticonButton"));
        cell->setText(QStringLiteral(":)"));
        row->addWidget(cell);
    }
};

namespace {

constexpr int kMargin = 30; // one slot at the input's right edge (emojiinput.cpp); slot 0 is the microphone's

// dark.qss (the user's skin): TeamSpeak's tool buttons.
const char kDarkToolButtons[] = "QToolButton { border: none; border-radius: 3px; background-color: #3C3F44; font-weight: bold; color: #dddddd; padding: 3px; }"
                                "QToolButton:pressed { background: #25252A; color: #ffffff; }"
                                "QToolButton:hover { background: #25252A; color: #ffffff; }"
                                "QToolButton:checked { background: #25252A; color: #dddddd; }";

QIcon greySmiley()
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xb9, 0xba, 0xbc));
    p.drawEllipse(QRectF(1, 1, 14, 14));
    p.setBrush(QColor(0x45, 0x47, 0x4a));
    p.drawEllipse(QRectF(4.5, 5, 2, 2.5));
    p.drawEllipse(QRectF(9.5, 5, 2, 2.5));
    return QIcon(pm);
}

// TeamSpeak's chat area: the input, then (nearer to the input than the button) the decoy, then the button.
struct ChatArea {
    QWidget               window;
    MainWindowChatWidget* area  = nullptr;
    ChatLineEdit*         input = nullptr;
    QToolButton*          button = nullptr;
    EmoticonsDisplay*     decoy  = nullptr;
    int                   teamSpeakClicks = 0;

    explicit ChatArea(bool styled = true)
    {
        window.resize(520, 200);
        if (styled)
            window.setStyleSheet(QString::fromLatin1(kDarkToolButtons));
        auto* outer = new QVBoxLayout(&window);
        area        = new MainWindowChatWidget(&window);
        area->setObjectName(QStringLiteral("MainWindowChatWidget"));
        outer->addWidget(area);
        auto* row = new QHBoxLayout(area);
        input     = new ChatLineEdit(area);
        input->setObjectName(QStringLiteral("ChatLineEdit"));
        input->setFixedHeight(28);
        decoy  = new EmoticonsDisplay(area);
        button = new QToolButton(area);
        button->setObjectName(QStringLiteral("EmoticonButton"));
        button->setToolTip(QStringLiteral("Show Emoticons"));
        button->setAccessibleName(QStringLiteral("Emoticons"));
        button->setIcon(greySmiley());
        button->setIconSize(QSize(16, 16));
        row->addWidget(input, 1);
        row->addWidget(decoy);
        row->addWidget(button);
        QObject::connect(button, &QToolButton::clicked, button, [this] { ++teamSpeakClicks; });
        window.show();
        QTest::qWaitForWindowExposed(&window);
    }
};

void press(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, pos, w->mapToGlobal(pos), Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void click(QWidget* w)
{
    const QPoint at = w->rect().center();
    press(w, QEvent::MouseButtonPress, at, Qt::LeftButton);
    press(w, QEvent::MouseButtonRelease, at, Qt::NoButton);
}

void closePopups()
{
    while (QWidget* p = QApplication::activePopupWidget())
        p->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QMargins viewportMargins(QTextEdit* edit)
{
    const QRect inner = edit->contentsRect();
    const QRect vp    = edit->viewport()->geometry();
    return QMargins(vp.left() - inner.left(), vp.top() - inner.top(), inner.right() - vp.right(), inner.bottom() - vp.bottom());
}

// The button's pixels, the icon's square (plus a pixel) left out.
QImage withoutIcon(QWidget* button, const QSize& iconSize)
{
    QImage image = button->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    image.setDevicePixelRatio(1.0);
    const qreal dpr = button->devicePixelRatioF();
    const QRect icon(QPoint(qRound((button->width() - iconSize.width()) / 2.0 * dpr) - qRound(dpr), qRound((button->height() - iconSize.height()) / 2.0 * dpr) - qRound(dpr)),
                     QSize(qRound((iconSize.width() + 2) * dpr), qRound((iconSize.height() + 2) * dpr)));
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(icon, Qt::transparent);
    return image;
}

} // namespace

class TestEmojiButton : public QObject
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
        closePopups();
        Settings::instance() = Settings();
    }

    // 1. By its object name, never a cell of TeamSpeak's emoticon list (even nearer to the input); then by
    //    "moticon" in a name or tool tip next to the input; then the nearest small button right of it.
    void findsTheRightButton()
    {
        ChatArea a;
        QCOMPARE(EmojiInput::findTeamSpeakButton(a.input), a.button);
        a.button->setObjectName(QStringLiteral("SomethingElse"));
        a.decoy->findChild<QToolButton*>()->setObjectName(QStringLiteral("cell"));
        QCOMPARE(EmojiInput::findTeamSpeakButton(a.input), a.button); // its tool tip "Show Emoticons"
        a.button->setToolTip(QString());
        a.decoy->hide(); // rule 3 looks at visible buttons only
        QCOMPARE(EmojiInput::findTeamSpeakButton(a.input), a.button); // right of the input, on its line
        a.button->setFixedSize(60, 60);
        QVERIFY(EmojiInput::findTeamSpeakButton(a.input) != a.button); // larger than 48 px: not an emoticon button
    }

    // 2. A press and a release open our picker; TeamSpeak's button never emits pressed() or clicked().
    void clickOpensOurPicker()
    {
        ChatArea   a;
        EmojiInput input;
        input.attach(a.input);
        QCOMPARE(input.takenButton(a.input), a.button);
        QVERIFY(!input.fallbackButton(a.input));
        QCOMPARE(a.button->toolTip(), QStringLiteral("Emoji (Ctrl+E)"));
        QCOMPARE(a.button->accessibleName(), QStringLiteral("Emoji"));
        QSignalSpy pressed(a.button, &QAbstractButton::pressed);
        QSignalSpy clicked(a.button, &QAbstractButton::clicked);
        click(a.button);
        QVERIFY(input.pickerOpen());
        QCOMPARE(pressed.count(), 0);
        QCOMPARE(clicked.count(), 0);
        QCOMPARE(a.teamSpeakClicks, 0);
        QVERIFY(!a.button->isDown());
        // A release outside the button opens nothing.
        closePopups();
        QVERIFY(!input.pickerOpen());
        press(a.button, QEvent::MouseButtonPress, a.button->rect().center(), Qt::LeftButton);
        press(a.button, QEvent::MouseMove, QPoint(-20, -20), Qt::LeftButton);
        press(a.button, QEvent::MouseButtonRelease, QPoint(-20, -20), Qt::NoButton);
        QVERIFY(!input.pickerOpen());
        QCOMPARE(clicked.count(), 0);
    }

    // 3. Space on the focused button opens it; a press on the button while it is open closes it for good.
    void spaceOpensAndASecondClickCloses()
    {
        ChatArea   a;
        EmojiInput input;
        input.attach(a.input);
        a.button->setFocus();
        QTest::keyClick(a.button, Qt::Key_Space);
        QVERIFY(input.pickerOpen());
        QCOMPARE(a.teamSpeakClicks, 0);
        auto* picker = a.input->findChild<EmojiPicker*>();
        QVERIFY(picker);
        const QPoint global = a.button->mapToGlobal(a.button->rect().center());
        QVERIFY(!picker->geometry().contains(global));
        QMouseEvent pressOnButton(QEvent::MouseButtonPress, picker->mapFromGlobal(global), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(picker, &pressOnButton);
        QVERIFY(picker->testAttribute(Qt::WA_NoMouseReplay)); // not handed on: it doesn't open again
        QVERIFY(!input.pickerOpen());
        // Ctrl+E in the input still opens it.
        QTest::keyClick(a.input, Qt::Key_E, Qt::ControlModifier);
        QVERIFY(input.pickerOpen());
    }

    // 4. TeamSpeak hides its button (its emoticon option): our in-input button, left of the microphone;
    //    shown again: gone, and only the microphone's slot is left (2.2.1 mic).
    void fallbackFollowsTheButton()
    {
        ChatArea       a;
        EmojiInput     input;
        const QMargins before = viewportMargins(a.input);
        const QMargins mic(before.left(), before.top(), before.right() + kMargin, before.bottom());
        input.attach(a.input);
        QCOMPARE(viewportMargins(a.input), mic); // the input gives only the microphone's slot
        QWidget* micButton = a.input->findChild<QWidget*>(QStringLiteral("tsmediaMicButton"));
        QVERIFY(micButton);
        QTRY_VERIFY(micButton->isVisible());
        QCOMPARE(micButton->x(), a.input->viewport()->geometry().right() + 1);
        a.button->hide();
        QVERIFY(input.fallbackButton(a.input));
        QTRY_COMPARE(viewportMargins(a.input).right(), before.right() + 2 * kMargin);
        QCOMPARE(input.fallbackButton(a.input)->x(), a.input->viewport()->geometry().right() + 1);
        QCOMPARE(micButton->x(), a.input->viewport()->geometry().right() + 1 + kMargin); // still at the edge
        a.button->show();
        QVERIFY(!input.fallbackButton(a.input));
        QCOMPARE(input.takenButton(a.input), a.button);
        QTRY_COMPARE(viewportMargins(a.input), mic);
        QCOMPARE(micButton->x(), a.input->viewport()->geometry().right() + 1);
        // No button at all: the fallback.
        ChatArea bare;
        delete bare.button;
        bare.button = nullptr;
        EmojiInput other;
        other.attach(bare.input);
        QVERIFY(other.fallbackButton(bare.input));
    }

    // 5. Turned off (or unloaded): TeamSpeak's tool tip and accessible texts are back, its icon was never
    //    touched, our filter is gone (its own click works again).
    void restoreGivesEverythingBack()
    {
        ChatArea       a;
        const qint64   iconKey = a.button->icon().cacheKey();
        const QMargins before  = viewportMargins(a.input);
        {
            EmojiInput input;
            input.attach(a.input);
            QCOMPARE(a.button->toolTip(), QStringLiteral("Emoji (Ctrl+E)"));
            Settings::instance().emojiButton = false;
            input.settingsChanged();
            QCOMPARE(a.button->toolTip(), QStringLiteral("Show Emoticons"));
            QCOMPARE(a.button->accessibleName(), QStringLiteral("Emoticons"));
            QVERIFY(!input.takenButton(a.input));
            QVERIFY(!input.fallbackButton(a.input)); // off: no in-input button either
            // ... but the microphone keeps its slot (2.2.1 mic).
            QVERIFY(a.input->findChild<QWidget*>(QStringLiteral("tsmediaMicButton")));
            QCOMPARE(viewportMargins(a.input).right(), before.right() + kMargin);
            click(a.button);
            QCOMPARE(a.teamSpeakClicks, 1);
            QVERIFY(!input.pickerOpen());
            Settings::instance().emojiButton = true;
            input.settingsChanged();
            QCOMPARE(input.takenButton(a.input), a.button);
        } // the destructor gives it back too
        QVERIFY(!a.input->findChild<QWidget*>(QStringLiteral("tsmediaMicButton")));
        QCOMPARE(viewportMargins(a.input), before);
        QCOMPARE(a.button->toolTip(), QStringLiteral("Show Emoticons"));
        QCOMPARE(a.button->icon().cacheKey(), iconKey);
        click(a.button);
        QCOMPARE(a.teamSpeakClicks, 2);
        // TeamSpeak deletes its button: nothing dangles, the fallback comes.
        EmojiInput input;
        input.attach(a.input);
        delete a.button;
        a.button = nullptr;
        QTRY_VERIFY(input.fallbackButton(a.input));
    }

    // 6. Around the icon, the button is painted exactly as TeamSpeak's skin paints it: idle, hovered and
    //    pressed.
    void skinPaintingIsKept()
    {
        for (const int state : {0, 1, 2}) {
            ChatArea a;
            if (state == 1)
                a.button->setAttribute(Qt::WA_UnderMouse, true);
            if (state == 2)
                a.button->setDown(true);
            const QImage teamSpeak = withoutIcon(a.button, a.button->iconSize());
            if (state == 2)
                a.button->setDown(false);
            EmojiInput input;
            input.attach(a.input);
            if (state == 2)
                press(a.button, QEvent::MouseButtonPress, a.button->rect().center(), Qt::LeftButton);
            const QImage ours = withoutIcon(a.button, a.button->iconSize());
            QCOMPARE(ours.size(), teamSpeak.size());
            int differ = 0;
            for (int y = 0; y < ours.height(); ++y) {
                for (int x = 0; x < ours.width(); ++x)
                    differ += ours.pixel(x, y) != teamSpeak.pixel(x, y) ? 1 : 0;
            }
            QVERIFY2(differ == 0, qPrintable(QStringLiteral("state %1: %2 pixels differ outside the icon").arg(state).arg(differ)));
            // The skin's background for the state: #3C3F44, or #25252A hovered and pressed.
            const QColor back = ours.pixelColor(qRound(3 * a.button->devicePixelRatioF()), ours.height() / 2);
            QCOMPARE(back.name(), state == 0 ? QStringLiteral("#3c3f44") : QStringLiteral("#25252a"));
            // ... and the icon itself is ours.
            QVERIFY(a.button->grab().toImage() != QImage());
            if (state == 2)
                press(a.button, QEvent::MouseButtonRelease, QPoint(-30, -30), Qt::NoButton);
        }
    }
};

TSMEDIA_REGISTER_TEST(TestEmojiButton)

#include "tst_emojibutton.moc"
