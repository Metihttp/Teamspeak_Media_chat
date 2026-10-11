// 2.3 hold strip fix (found in 2.2.1): TeamSpeak 3.6.2 shows every server tab's chat above one shared chat
// input, so switching server tabs hides nothing of it, and the hold strip or its notices stayed over the
// other tab's chat for up to 2 s. Now, the moment another server tab comes up (serverTabChanged, or the
// hold's watch seeing TeamSpeak's current connection change): a hold ends as "another chat came up" (kept
// in the window, never sent), "Sending…" and notices go at once and don't come back on that tab. With the
// fake chat of tests/voicefakes.h and generated sound (never a microphone), in a window never on the screen.

#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtTest>

#include <memory>

#include "emojiinput.h"
#include "holdstrip.h"
#include "inputbars.h"
#include "micbutton.h"
#include "music/musicbar.h"
#include "settings.h"
#include "testmain.h"
#include "video/mfvideo.h"
#include "voicecontroller.h"
#include "voicefakes.h"
#include "voicepanel.h"

namespace {

using namespace voicefakes;
using Phase = VoiceController::Phase;

constexpr int kEncodeTimeoutMs = 60000;

uint64 g_visibleTab = fakets3::kConnection;
uint64 visibleTab()
{
    return g_visibleTab;
}

struct TabWindow {
    QWidget                window;
    QTextBrowser*          browser = nullptr;
    QPointer<ChatLineEdit> input;
    EmojiInput             emoji; // after the window: it gives the input back first
    MicButton*             mic = nullptr;

    TabWindow()
    {
        window.setAttribute(Qt::WA_DontShowOnScreen);
        auto* layout = new QVBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        browser = new QTextBrowser(&window);
        input   = new ChatLineEdit(&window);
        input->setFixedHeight(30);
        layout->addWidget(browser, 1);
        layout->addWidget(input);
        window.resize(520, 320);
        emoji.attach(input);
        window.show();
        mic = MicButton::of(input);
    }
    void send(QEvent::Type type, Qt::MouseButtons buttons)
    {
        const QPoint local = mic->iconRect().center();
        QMouseEvent  event(type, local, mic->mapToGlobal(local), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(mic, &event);
    }
    void press() { send(QEvent::MouseButtonPress, Qt::LeftButton); }
    void release() { send(QEvent::MouseButtonRelease, Qt::NoButton); }
};

bool recordingFor(VoiceController& voice, qint64 ms)
{
    return voice.phase() == Phase::Recording && voice.strip() && voice.strip()->view().timeMs >= ms;
}

bool stripShown(VoiceController& voice)
{
    return voice.strip() && voice.strip()->isVisible();
}

} // namespace

class TestHoldTab : public QObject
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
        g_visibleTab                                    = fakets3::kConnection;
        ts3::funcs.getCurrentServerConnectionHandlerID = &visibleTab;
        m_chat                                          = FakeChat();
        m_chat.sound.levelDb                            = -18.0;
        m_chat.sound.realtime                           = true;
    }
    void cleanup() { QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE)); }

    void heldWhenAnotherTabComesUpIsKept()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow       c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 700), 5000);
        QVERIFY(stripShown(voice));
        g_visibleTab = 2; // TeamSpeak shows the other server tab's chat above the same input
        voice.serverTabChanged(2);
        QVERIFY(!stripShown(voice)); // at once, not 2 s later
        QVERIFY(!c.mic->holding());  // the hold is over (its release will do nothing)
        QCOMPARE(MicButton::sharedState().mode, MicButton::Mode::Busy); // stopping: not "held" any more
        // Saving, then kept in the window; the strip never comes back on this tab meanwhile.
        for (int i = 0; i < 200 && voice.phase() != Phase::Error; ++i) {
            QTest::qWait(20);
            QVERIFY(!stripShown(voice));
        }
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QVERIFY(voice.panel());
        const VoicePanel::View v = voice.panel()->view();
        QVERIFY2(v.errorBody.contains(QStringLiteral("Another chat came up")), qPrintable(v.errorBody));
        QVERIFY2(v.errorBody.contains(QStringLiteral("nothing was sent")), qPrintable(v.errorBody));
        c.release(); // the release that comes after all: nothing
        QVERIFY(m_chat.sent.isEmpty());
        voice.panel()->leftButton()->click(); // close: discarded
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    void theWatchSeesTheTabWithoutTheEvent()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow       c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 600), 5000);
        g_visibleTab = 2; // the queued event hasn't come yet
        QTRY_VERIFY_WITH_TIMEOUT(!stripShown(voice), 4 * VoiceController::kHoldWatchMs);
        QVERIFY(!c.mic->holding());
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        c.release();
        QVERIFY(m_chat.sent.isEmpty());
        voice.panel()->leftButton()->click();
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    void noticeGoesWithItsTab()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow       c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        c.release(); // a tap: "Hold to record, release to send" for about 2 s
        QVERIFY(stripShown(voice));
        QCOMPARE(voice.strip()->view().mode, HoldStrip::Mode::Notice);
        g_visibleTab = 2;
        voice.serverTabChanged(2);
        QVERIFY(!stripShown(voice)); // at once
        g_visibleTab = fakets3::kConnection;
        voice.serverTabChanged(fakets3::kConnection);
        QVERIFY(!stripShown(voice)); // and it doesn't come back
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(m_chat.sent.isEmpty());
    }

    void sendingStripStaysOffTheOtherTab()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow       c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 800), 5000);
        c.release(); // let go on its own tab: it is sent there
        g_visibleTab = 2;
        voice.serverTabChanged(2);
        QVERIFY(!stripShown(voice));
        for (int i = 0; i < 400 && m_chat.sent.isEmpty(); ++i) {
            QTest::qWait(10);
            QVERIFY(!stripShown(voice)); // "Sending…" never over the other tab's chat
        }
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(m_chat.sent.first().target.sch, fakets3::kConnection);
        QTRY_COMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!stripShown(voice));
    }

    void otherTabEventsLeaveTheHoldAlone()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow       c;
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QTRY_VERIFY_WITH_TIMEOUT(recordingFor(voice, 300), 5000);
        voice.serverTabChanged(fakets3::kConnection); // a late event for the tab that is on screen anyway
        QVERIFY(c.mic->holding());
        QVERIFY(stripShown(voice));
        QTest::keyClick(c.input, Qt::Key_Escape);
        c.release();
        QVERIFY(m_chat.sent.isEmpty());
    }

    void stripSitsOnTopOfTheMusicBar()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        TabWindow c;
        MusicBar* bar = new MusicBar;
        QVERIFY(inputbars::insertAbove(c.input, bar));
        MusicBar::View v;
        v.mode       = MusicBar::Mode::Playing;
        v.title      = QStringLiteral("Morning Walk");
        v.durationMs = 60000;
        v.animate    = false;
        bar->setView(v);
        bar->show();
        c.window.layout()->activate();
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(c.mic->isVisible());
        c.press();
        QVERIFY(stripShown(voice));
        const int barTop = bar->mapTo(&c.window, QPoint(0, 0)).y();
        QCOMPARE(voice.strip()->geometry().bottom() + 1, barTop); // not over the music bar
        QVERIFY(!voice.strip()->geometry().intersects(QRect(bar->mapTo(&c.window, QPoint(0, 0)), bar->size())));
        QTest::keyClick(c.input, Qt::Key_Escape);
        c.release();
        delete bar;
    }

  private:
    TS3Functions                   m_savedFuncs;
    Settings                       m_savedSettings;
    std::unique_ptr<QTemporaryDir> m_dir;
    FakeChat                       m_chat;
};

TSMEDIA_REGISTER_TEST(TestHoldTab)
#include "tst_holdtab.moc"
