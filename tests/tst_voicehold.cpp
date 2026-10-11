// 2.2.1 hold to record: the chat input's microphone button held with the mouse. The mouse events go to the
// button the way Qt delivers them under its implicit grab (never the real pointer); VoiceController runs
// with the fake chat of tests/voicefakes.h and generated sound (never a microphone; nothing is played), in
// a TeamSpeak-like chat window that is never on the screen.
//   press records at once (the microphone opened right away, no window; the strip above the input),
//   release sends; a tap (under half a second) sends nothing and the strip says how, a double click or a
//   press right after starts nothing; away from the button is the cancel zone (red, "Release to
//   cancel"), back re-arms; Esc cancels; 5:00 sends while still held (the release then does nothing); a
//   lost grab (deactivated window, a popup, the button no longer held without a release, another server
//   tab, the button disabled) or another chat coming up in the input's tabs keeps the recording in the
//   window with Send, and so does 5:00 in the cancel zone; a press that finds the chat blocked leaves no
//   button down; the strip is never a window and goes with its input; unload, the controller deleted or
//   the input gone while held cancel quietly. TeamSpeak's microphone is given back after every test.

#include <QApplication>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QVBoxLayout>
#include <QtTest>

#include <memory>

#include "emojiinput.h"
#include "holdstrip.h"
#include "medialink.h"
#include "micbutton.h"
#include "settings.h"
#include "testmain.h"
#include "uiutil.h"
#include "video/mfvideo.h"
#include "voicecontroller.h"
#include "voicefakes.h"
#include "voicepanel.h"

namespace {

using namespace voicefakes;
using Phase = VoiceController::Phase;
using Mode  = MicButton::Mode;

constexpr int kEncodeTimeoutMs = 60000;

// A TeamSpeak-like chat: the chat above, the input at the bottom, the microphone in the input's slot.
struct ChatWindow {
    QWidget                 window;
    QTextBrowser*           browser = nullptr;
    QPointer<ChatLineEdit>  input;
    EmojiInput              emoji; // declared after the window: it gives the input back first
    MicButton*              mic = nullptr;

    ChatWindow()
    {
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* layout = new QVBoxLayout(&window);
        browser      = new QTextBrowser(&window);
        input        = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        layout->addWidget(browser, 1);
        layout->addWidget(input);
        window.resize(520, 320);
        emoji.attach(input);
        window.show();
        mic = MicButton::of(input);
    }
    QPoint center() const { return mic->iconRect().center(); }
    void   send(QEvent::Type type, const QPoint& offset, Qt::MouseButtons buttons)
    {
        const QPoint    local = center() + offset;
        const auto      which = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent     event(type, local, mic->mapToGlobal(local), which, buttons, Qt::NoModifier);
        QApplication::sendEvent(mic, &event);
    }
    void press() { send(QEvent::MouseButtonPress, QPoint(), Qt::LeftButton); }
    void doubleClickPress() { send(QEvent::MouseButtonDblClick, QPoint(), Qt::LeftButton); }
    void move(const QPoint& offset) { send(QEvent::MouseMove, offset, Qt::LeftButton); }
    void release(const QPoint& offset = QPoint()) { send(QEvent::MouseButtonRelease, offset, Qt::NoButton); }
    QRect inputRect() { return QRect(input->mapTo(&window, QPoint(0, 0)), input->size()); }
};

bool recordingFor(VoiceController& voice, qint64 ms)
{
    return voice.phase() == Phase::Recording && voice.strip() && voice.strip()->view().timeMs >= ms;
}

} // namespace

class TestVoiceHold : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        m_savedFuncs    = ts3::funcs;
        m_savedSettings = Settings::instance();
    }
    void cleanupTestCase()
    {
        ts3::funcs           = m_savedFuncs;
        Settings::instance() = m_savedSettings;
        MicButton::setSharedState(MicButton::State());
    }
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        fakets3::install(m_dir->path());
        FakeTsMic::install();
        m_chat               = FakeChat();
        m_chat.sound.levelDb = -18.0;
        m_chat.sound.realtime = true; // paced like a microphone
    }
    void cleanup()
    {
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE)); // given back, whatever happened
    }

    void pressRecordsReleaseSends()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        QCOMPARE(c.mic->toolTip(), QStringLiteral("Hold to record a voice message"));
        c.press();
        // At once: the microphone opened (no wait for the start sound), TeamSpeak's muted, no window.
        QVERIFY(voice.holding());
        QVERIFY(c.mic->holding());
        QCOMPARE(m_chat.microphones, 1);
        QCOMPARE(voice.phase(), Phase::Starting);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED));
        QCOMPARE(m_chat.pauses, 1);
        QVERIFY(!voice.panel());
        QCOMPARE(c.mic->state().mode, Mode::Holding);
        // The strip: right above the input, as wide, in its window; never the mouse or the focus.
        QPointer<HoldStrip> strip = voice.strip();
        QVERIFY(strip);
        QVERIFY(strip->isVisible());
        QCOMPARE(strip->parentWidget(), &c.window);
        QCOMPARE(strip->objectName(), QStringLiteral("tsmediaHoldStrip"));
        QCOMPARE(strip->geometry().bottom() + 1, c.inputRect().top());
        QCOMPARE(strip->x(), c.inputRect().x());
        QCOMPARE(strip->width(), c.inputRect().width());
        QVERIFY(strip->height() >= 32);
        QVERIFY(strip->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(strip->focusPolicy(), Qt::NoFocus);
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        const HoldStrip::View v = strip->view();
        QCOMPARE(v.mode, HoldStrip::Mode::Recording);
        QCOMPARE(v.limitMs, VoiceController::kMaxMs);
        QVERIFY(!v.liveBins.isEmpty());
        QCOMPARE(HoldStrip::line(v), QStringLiteral("Release to send · Move away to cancel"));
        QCOMPARE(strip->accessibleDescription(), HoldStrip::line(v));

        c.release();
        QVERIFY(!c.mic->holding());
        QVERIFY(!c.mic->isDown());
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Saving); // "Sending…" at once
        QCOMPARE(strip->view().text, QStringLiteral("Sending…"));
        QCOMPARE(c.mic->state().mode, Mode::Busy);
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        const SendItem& item = m_chat.sent.first().items.first();
        QVERIFY(item.voice);
        QVERIFY(item.ownTemp);
        QVERIFY2(item.durationMs >= 800 && item.durationMs < 10000, qPrintable(QString::number(item.durationMs)));
        QCOMPARE(item.waveform.size(), MediaLink::kWaveformLevels);
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!voice.holding());
        QVERIFY(!voice.panel());
        QVERIFY(strip && !strip->isVisible()); // the upload panel takes over
        QCOMPARE(c.mic->state().mode, Mode::Ready);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
    }

    void tapShowsTheHintAndSendsNothing()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        c.release(); // a click
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!voice.holding());
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(recordings().isEmpty());
        QVERIFY(!voice.panel());
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        QPointer<HoldStrip> strip = voice.strip();
        QVERIFY(strip && strip->isVisible());
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Notice);
        QCOMPARE(strip->view().text, QStringLiteral("Hold to record, release to send"));
        QCOMPARE(m_chat.microphones, 1);

        // A double click's second press, then a plain press right after: nothing starts.
        c.doubleClickPress();
        QVERIFY(!c.mic->holding());
        QCOMPARE(voice.phase(), Phase::Idle);
        c.release();
        c.press();
        QVERIFY(!c.mic->holding());
        QCOMPARE(voice.phase(), Phase::Idle);
        c.release();
        QCOMPARE(m_chat.microphones, 1);
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(strip->isVisible()); // the hint stays

        // It goes by itself after about 2 s; then a press records again.
        QTRY_VERIFY_WITH_TIMEOUT(!strip->isVisible(), qMax(VoiceController::kNoticeMs, ui::notificationDurationMs()) + 3000);
        c.press();
        QVERIFY(c.mic->holding());
        QCOMPARE(voice.phase(), Phase::Starting);
        QCOMPARE(m_chat.microphones, 2);
        QTest::keyClick(c.input, Qt::Key_Escape); // canceled
        QCOMPARE(voice.phase(), Phase::Idle);
        c.release();
        QVERIFY(m_chat.sent.isEmpty());
    }

    void blockedChatHoldsNothing()
    {
        m_chat.canSend = false;
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QVERIFY(!c.mic->holding());
        QVERIFY(!voice.holding());
        c.release();
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.microphones, 0);
        QVERIFY(!voice.strip());
        QVERIFY(!voice.panel());
    }

    void moveAwayCancelsMoveBackRearms()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 300), 5000);
        HoldStrip* strip = voice.strip();
        c.move(QPoint(-40, 0)); // a little: still sends
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Recording);
        QVERIFY(!c.mic->inCancelZone());
        c.move(QPoint(-MicButton::kCancelPx - 20, 0)); // away to the left: the cancel zone
        QVERIFY(c.mic->inCancelZone());
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Cancel);
        QCOMPARE(HoldStrip::line(strip->view()), QStringLiteral("Release to cancel"));
        QVERIFY(c.mic->state().cancel);
        QCOMPARE(c.mic->accessibleDescription(), QStringLiteral("Release to cancel."));
        QCOMPARE(voice.phase(), Phase::Recording); // still recording meanwhile
        c.move(QPoint(-(MicButton::kRearmPx + MicButton::kCancelPx) / 2, 0)); // between the two: no flicker
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Cancel);
        c.move(QPoint(-30, 0)); // back: it sends again
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Recording);
        QVERIFY(!c.mic->state().cancel);
        c.move(QPoint(0, -(MicButton::kRearmPx + MicButton::kCancelPx) / 2)); // up, not far enough yet
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Recording);
        c.move(QPoint(0, -MicButton::kCancelPx - 20)); // up: the cancel zone
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Cancel);
        c.release(QPoint(0, -MicButton::kCancelPx - 20)); // let go there: discarded
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!voice.holding());
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(recordings().isEmpty());
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Notice);
        QCOMPARE(strip->view().icon, HoldStrip::Icon::Canceled);
        QCOMPARE(strip->view().text, QStringLiteral("Canceled — nothing was sent."));

        // Away and back again before letting go: sent.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        c.move(QPoint(-120, -40));
        QVERIFY(c.mic->inCancelZone());
        c.move(QPoint(4, 2));
        QVERIFY(!c.mic->inCancelZone());
        c.release(QPoint(4, 2));
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        QCOMPARE(voice.phase(), Phase::Idle);

        // Released right away in the zone (no move came first): the release's own place counts.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        c.release(QPoint(-200, 0));
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.sent.size(), 1);
    }

    void escCancels()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.input->setFocus();
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        QTest::keyClick(c.input, Qt::Key_Escape);
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!c.mic->holding());
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(voice.strip()->view().text, QStringLiteral("Canceled — nothing was sent."));
        c.release(); // the release that still comes: nothing (no click, no new recording)
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.microphones, 1);
        QVERIFY(!voice.panel());
        QTest::keyClick(c.input, Qt::Key_Escape); // our filter is gone: Esc is the input's again
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    void limitSendsWhileHeld()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = false; // as fast as it is read: 5:00 in a moment
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(voice.phase() == Phase::Saving || !m_chat.sent.isEmpty(), kEncodeTimeoutMs);
        if (voice.phase() == Phase::Saving) { // the strip says why it goes now
            QVERIFY(!c.mic->holding()); // the hold ended by itself
            QCOMPARE(voice.strip()->view().mode, HoldStrip::Mode::Saving);
            QCOMPARE(voice.strip()->view().text, QStringLiteral("Reached the 5:00 limit, sending…"));
        }
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        QCOMPARE(m_chat.sent.first().items.first().durationMs, VoiceController::kMaxMs);
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!c.mic->holding());
        QVERIFY(voice.strip()->isVisible());
        QCOMPARE(voice.strip()->view().mode, HoldStrip::Mode::Notice);
        QCOMPARE(voice.strip()->view().icon, HoldStrip::Icon::Sent);
        QCOMPARE(voice.strip()->view().text, QStringLiteral("Reached the 5:00 limit, so it was sent."));
        c.release(); // the mouse is let go only now: nothing more
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.microphones, 1);
        QVERIFY(!voice.panel());
    }

    void lostGrabKeepsTheRecording()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.watchPointer = true; // Host::pointerHeld reads m_chat.mouseDown
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        const auto kept = [&voice]() {
            QVERIFY(voice.panel());
            QVERIFY(voice.panel()->isVisible());
            const VoicePanel::View v = voice.panel()->view();
            QCOMPARE(v.mode, VoicePanel::Mode::Error);
            QCOMPARE(v.errorTitle, QStringLiteral("Recording stopped"));
            QVERIFY2(v.errorBody.contains(QStringLiteral("nothing was sent")), qPrintable(v.errorBody));
            QCOMPARE(v.fix, VoicePanel::Fix::Send);
            QCOMPARE(voice.panel()->sendButton()->objectName(), QStringLiteral("voicePrimary"));
            QVERIFY(!voice.holding());
            QVERIFY(!voice.strip() || !voice.strip()->isVisible());
            QVERIFY(!voice.panel()->testAttribute(Qt::WA_QuitOnClose)); // never keeps TeamSpeak running
        };

        // 1. TeamSpeak's window deactivated (Alt+Tab): stopped, kept, never sent by itself.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 1200), 5000);
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&c.window, &deactivate);
        QVERIFY(!c.mic->holding());
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        kept();
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(c.mic->state().mode, Mode::Busy); // a click shows the window
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        c.release(); // a release that comes after all: still not sent
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(voice.phase(), Phase::Error);
        voice.panel()->sendButton()->click(); // Send
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(voice.phase(), Phase::Idle);

        // 2. The button came up where TeamSpeak didn't see it (no release): the same.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        m_chat.mouseDown = false;
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        kept();
        QVERIFY(!c.mic->holding());
        voice.panel()->leftButton()->click(); // Close: under 3 s it is discarded
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.sent.size(), 1);
        m_chat.mouseDown = true; // (its release never comes here)

        // 3. The next press records (nothing is left waiting for that release), and a release that
        //    arrives right after the button came up is an ordinary send.
        c.press();
        QVERIFY(c.mic->holding());
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        m_chat.mouseDown = false;
        c.release();
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 2, kEncodeTimeoutMs);
        QVERIFY(!voice.panel());
        m_chat.mouseDown = true;

        // 4. A popup menu took the mouse.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        {
            QMenu menu;
            menu.addAction(QStringLiteral("Something"));
            menu.popup(QPoint(10, 10));
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
            menu.close();
        }
        kept();
        voice.panel()->leftButton()->click();
        c.release();

        // 5. Another server tab (the input hidden while held): the strip goes at once, never over that
        //    tab's chat while the recording is saved.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        c.input->hide();
        QVERIFY(!c.mic->holding());
        QVERIFY(voice.strip() && !voice.strip()->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        kept();
        voice.panel()->leftButton()->click();
        QCOMPARE(m_chat.sent.size(), 2);
        c.input->show();
    }

    // The chat's tabs (server, channel, private chats) share one input, so its button stays while another
    // chat comes up (a key, the mouse wheel on the tabs): the recording must not go to a chat that is no
    // longer the one shown. It is kept for the chat it was recorded for, and only sent with Send.
    void anotherChatWhileHeldIsKept()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());

        // 1. While held: the watch sees it.
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        m_chat.visibleMode = TextMessageTarget_SERVER;
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QVERIFY(voice.panel());
        VoicePanel::View v = voice.panel()->view();
        QCOMPARE(v.errorTitle, QStringLiteral("Recording stopped"));
        QVERIFY2(v.errorBody.contains(QStringLiteral("Another chat came up")), qPrintable(v.errorBody));
        QVERIFY2(v.errorBody.contains(QStringLiteral("nothing was sent")), qPrintable(v.errorBody));
        QVERIFY2(v.errorBody.contains(QStringLiteral("the channel “Lobby”")), qPrintable(v.errorBody));
        QCOMPARE(v.fix, VoicePanel::Fix::Send);
        QVERIFY(!c.mic->holding());
        QVERIFY(!voice.holding());
        QVERIFY(!voice.strip() || !voice.strip()->isVisible());
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        c.release(); // the release that comes after all: nothing
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(voice.phase(), Phase::Error);
        voice.panel()->sendButton()->click(); // Send: to the chat it was recorded for
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(m_chat.sent.first().target.mode, static_cast<int>(TextMessageTarget_CHANNEL));
        QCOMPARE(voice.phase(), Phase::Idle);

        // 2. Let go right after, before the watch looked: the release finds out.
        m_chat.visibleMode = TextMessageTarget_CHANNEL;
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        m_chat.visibleMode = TextMessageTarget_SERVER;
        c.release();
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        v = voice.panel()->view();
        QVERIFY2(v.errorBody.contains(QStringLiteral("Another chat came up")), qPrintable(v.errorBody));
        QCOMPARE(m_chat.sent.size(), 1);
        voice.panel()->leftButton()->click(); // Close: under 3 s it is discarded
        QCOMPARE(voice.phase(), Phase::Idle);

        // 3. Disconnected, TeamSpeak's tabs tell nothing: no false alarm (the release keeps it anyway).
        m_chat.visibleMode = TextMessageTarget_CHANNEL;
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        fakets3::setConnected(false);
        m_chat.visibleMode = TextMessageTarget_SERVER;
        QTest::qWait(10 * VoiceController::kHoldWatchMs);
        QVERIFY(c.mic->holding());
        QCOMPARE(voice.phase(), Phase::Recording);
        m_chat.visibleMode = TextMessageTarget_CHANNEL;
        fakets3::setConnected(true);
        QTest::keyClick(c.input, Qt::Key_Escape); // canceled
        c.release();
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.sent.size(), 1);
        QVERIFY(!voice.panel());
    }

    // A press on a button that still looked usable, in a chat that can't take a voice message any more
    // (disconnected a moment ago): the chat says why, every button is greyed out, and none stays down (a
    // disabled button never gets its release; it would be drawn pressed once usable again).
    void pressThatGreysTheButtonOutLeavesItUp()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        QCOMPARE(c.mic->state().mode, Mode::Ready);
        m_chat.canSend = false;
        m_chat.block   = QStringLiteral("Connect to a server to send voice messages.");
        c.press();
        QCOMPARE(c.mic->state().mode, Mode::Unavailable);
        QVERIFY(!c.mic->isEnabled());
        QVERIFY(!c.mic->isDown());
        QVERIFY(!c.mic->holding());
        QVERIFY(!voice.holding());
        c.release(); // a disabled button never gets it
        m_chat.canSend = true;
        m_chat.block.clear();
        voice.updateMicButtons(); // connected again
        QCOMPARE(c.mic->state().mode, Mode::Ready);
        QVERIFY(c.mic->isEnabled());
        QVERIFY(!c.mic->isDown()); // not drawn pressed
        QCOMPARE(m_chat.microphones, 0);
        c.press(); // and a hold records again
        QVERIFY(c.mic->holding());
        QCOMPARE(voice.phase(), Phase::Starting);
        QTest::keyClick(c.input, Qt::Key_Escape);
        c.release();
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(m_chat.sent.isEmpty());
    }

    // TeamSpeak greys its input out while the button is held: a disabled button gets no release, so the
    // hold ends there (kept in the window), even with only Qt's events to go by.
    void disabledWhileHeldIsKept()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host()); // no Host::pointerHeld here
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        c.input->setEnabled(false);
        QVERIFY(!c.mic->holding());
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QVERIFY(voice.panel());
        QVERIFY2(voice.panel()->view().errorBody.contains(QStringLiteral("nothing was sent")), qPrintable(voice.panel()->view().errorBody));
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
        c.release();
        QVERIFY(m_chat.sent.isEmpty());
        c.input->setEnabled(true);
        voice.panel()->leftButton()->click(); // Close: discarded
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
    }

    // 5:00 while the pointer is in the cancel zone ("Release to cancel" showing): never sent by itself,
    // nor thrown away; kept in the window, sent only with Send.
    void limitInTheCancelZoneIsKept()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = false; // as fast as it is read: 5:00 in a moment
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        const QPoint away(-MicButton::kCancelPx - 30, 0);
        c.press();
        c.move(away); // before 5:00 arrives (the recorder's signals wait for the event loop)
        QCOMPARE(voice.strip()->view().mode, HoldStrip::Mode::Cancel);
        QTRY_VERIFY_WITH_TIMEOUT(voice.phase() != Phase::Starting && voice.phase() != Phase::Recording, kEncodeTimeoutMs);
        if (voice.phase() == Phase::Saving) // the strip never says it is being sent
            QCOMPARE(voice.strip()->view().text, QStringLiteral("Saving…"));
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QVERIFY(m_chat.sent.isEmpty());
        const VoicePanel::View v = voice.panel()->view();
        QCOMPARE(v.errorTitle, QStringLiteral("Reached the 5:00 limit"));
        QVERIFY2(v.errorBody.contains(QStringLiteral("nothing was sent")), qPrintable(v.errorBody));
        QCOMPARE(v.fix, VoicePanel::Fix::Send);
        QVERIFY(!c.mic->holding());
        c.release(away); // the release that comes after all: nothing
        QCOMPARE(voice.phase(), Phase::Error);
        QVERIFY(m_chat.sent.isEmpty());
        voice.panel()->sendButton()->click(); // Send
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(m_chat.sent.first().items.first().durationMs, VoiceController::kMaxMs);
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.microphones, 1);
    }

    // The strip is TeamSpeak's window's child, never a window (nor one that keeps TeamSpeak running), and
    // it goes with its input: a notice never stays over another server tab's chat.
    void stripGoesWithItsInput()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QPointer<HoldStrip> strip = voice.strip();
        QVERIFY(strip && strip->isVisible());
        QCOMPARE(strip->parentWidget(), &c.window);
        QVERIFY(!strip->isWindow());
        QVERIFY(!strip->testAttribute(Qt::WA_QuitOnClose));
        c.release(); // a tap: the hint
        QCOMPARE(strip->view().mode, HoldStrip::Mode::Notice);
        QVERIFY(strip->isVisible());
        c.input->hide(); // another server tab
        QVERIFY(!strip->isVisible());
        c.input->show();
        QVERIFY(!strip->isVisible()); // it doesn't come back
        QCOMPARE(voice.phase(), Phase::Idle);
        QTest::qWait(qMax(400, QGuiApplication::styleHints()->mouseDoubleClickInterval()) + 100); // no longer right after the tap
        QVERIFY(!strip->isVisible());
        c.press(); // the next hold shows it again
        QVERIFY(strip && strip->isVisible());
        QTest::keyClick(c.input, Qt::Key_Escape);
        c.release();
        QVERIFY(m_chat.sent.isEmpty());
    }

    void connectionLostWhileHeldIsKept()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow      c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        fakets3::setConnected(false);
        QTRY_VERIFY_WITH_TIMEOUT(!voice.strip()->view().problem.isEmpty(), 5000);
        QCOMPARE(HoldStrip::line(voice.strip()->view()), QStringLiteral("Not connected · release to keep it"));
        c.release();
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QCOMPARE(voice.panel()->view().errorTitle, QStringLiteral("Not connected to this server"));
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
        QVERIFY(m_chat.sent.isEmpty());
        fakets3::setConnected(true);
        voice.requestSend();
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(voice.phase(), Phase::Idle);
        // TeamSpeak's microphone could only be given back once connected again (within a second).
        QTRY_COMPARE_WITH_TIMEOUT(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE), 3000);
    }

    void slowMicrophoneHeldBrieflyIsTooShort()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        // A microphone that takes 2.5 s to open, held 0.7 s (no tap): nothing was recorded.
        VoiceController::Host host = m_chat.host();
        host.microphone            = [](quint64, std::shared_ptr<voice::DeviceChoice> device) -> voice::VoiceRecorder::BackendFactory {
            return [device]() -> std::unique_ptr<voice::CaptureBackend> {
                QThread::msleep(2500);
                device->source = voice::DeviceChoice::Source::TeamSpeak;
                return std::make_unique<voice::FakeCapture>(voice::FakeCapture::Options());
            };
        };
        ChatWindow      c;
        VoiceController voice(host);
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTest::qWait(700);
        QCOMPARE(voice.phase(), Phase::Starting);
        c.release();
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Idle, 8000);
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(!voice.panel());
        QCOMPARE(voice.strip()->view().text, QStringLiteral("Too short to send — nothing was sent."));
    }

    void unloadWhileHeldCancelsQuietly()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        ChatWindow c;
        QTRY_VERIFY(c.mic->isVisible());
        {
            VoiceController voice(m_chat.host()); // plugin shutdown
            c.press();
            QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 500), 5000);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED));
            QPointer<HoldStrip> strip = voice.strip();
            voice.shutdown();
            QCOMPARE(voice.phase(), Phase::Idle);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
            QVERIFY(!strip); // out of TeamSpeak's window
            QVERIFY(!voice.panel());
            QVERIFY(!c.mic->holding());
            c.release();
            QTest::keyClick(c.input, Qt::Key_Escape);
            QVERIFY(m_chat.sent.isEmpty());
        }
        {
            auto voice = std::make_unique<VoiceController>(m_chat.host()); // deleted while held
            c.press();
            QTRY_VERIFY_WITH_TIMEOUT(recordingFor(*voice, 500), 5000);
            QCOMPARE(c.mic->state().mode, Mode::Holding);
            QPointer<HoldStrip> strip = voice->strip();
            voice.reset();
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
            QVERIFY(!strip);
            QVERIFY(!c.mic->holding());
            QCOMPARE(c.mic->state().mode, Mode::Ready); // no button stays "Recording"
            c.release();
        }
        {
            VoiceController voice(m_chat.host()); // the input goes while held (TeamSpeak closing)
            c.press();
            QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 500), 5000);
            delete c.input.data(); // the button with it
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Idle, 2000);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
            QVERIFY(!voice.panel());
            QVERIFY(!voice.strip() || !voice.strip()->isVisible());
        }
        QVERIFY(m_chat.sent.isEmpty());
    }

    // ---- the strip ----------------------------------------------------------------------------------
    void stripLinesAndPictures()
    {
        HoldStrip::View v;
        QCOMPARE(HoldStrip::line(v), QStringLiteral("Release to send · Move away to cancel"));
        v.problem = QStringLiteral("No sound from the microphone");
        QCOMPARE(HoldStrip::line(v), v.problem);
        v.mode = HoldStrip::Mode::Cancel;
        QCOMPARE(HoldStrip::line(v), QStringLiteral("Release to cancel"));
        v.mode = HoldStrip::Mode::Saving;
        v.text = QStringLiteral("Sending…");
        QCOMPARE(HoldStrip::line(v), v.text);
        v.mode = HoldStrip::Mode::Notice;
        v.text = QStringLiteral("Hold to record, release to send");
        QCOMPARE(HoldStrip::line(v), v.text);
        QVERIFY(HoldStrip::heightFor(QFont()) >= 32);

        // Every state draws at 1x and 2x, light and dark, at any width (the narrowest drops the waveform).
        for (const HoldStrip::Mode mode : {HoldStrip::Mode::Recording, HoldStrip::Mode::Cancel, HoldStrip::Mode::Saving, HoldStrip::Mode::Notice}) {
            for (const bool dark : {false, true}) {
                for (const qreal dpr : {1.0, 2.0}) {
                    for (const int width : {120, 300, 640}) {
                        HoldStrip::View view;
                        view.mode   = mode;
                        view.timeMs = 7000;
                        view.text   = QStringLiteral("Sending…");
                        view.liveBins.fill(-20.0f, 140);
                        HoldStrip::Look look;
                        look.dark  = dark;
                        look.base  = dark ? QColor(0x31, 0x33, 0x38) : QColor(Qt::white);
                        look.phase = 0.25;
                        const int height = HoldStrip::heightFor(look.font);
                        QImage    image(QSize(width, height) * dpr, QImage::Format_ARGB32_Premultiplied);
                        image.setDevicePixelRatio(dpr);
                        image.fill(Qt::transparent);
                        QPainter p(&image);
                        HoldStrip::render(p, QRectF(0, 0, width, height), view, look);
                        p.end();
                        QVERIFY(qAlpha(image.pixel(image.width() / 2, image.height() - 2)) == 255); // opaque, attached to the input
                        QVERIFY(qAlpha(image.pixel(0, 0)) < 255); // the rounded top corner
                    }
                }
            }
        }
    }

  private:
    std::unique_ptr<QTemporaryDir> m_dir;
    FakeChat                       m_chat;
    TS3Functions                   m_savedFuncs{};
    Settings                       m_savedSettings;
};

TSMEDIA_REGISTER_TEST(TestVoiceHold)

#include "tst_voicehold.moc"
