// 2.2.1 voice: the recorder's flow without a chat: VoiceController with a fake Host (tests/fakets3.* for
// the connection), generated sound (FakeCapture, never a microphone; nothing is played) and its window
// off the screen. Send at once, Cancel (and its question), Too short, 5:00 sends by itself, a lost
// microphone, a hidden window and a lost connection keep the recording for Send (closing one of 3 s or
// more asks). TeamSpeak's microphone (a fake mute flag) is muted while recording and given back every
// way, after a dropped connection once it is back (checked after every test). Then the window's states
// and keys, and the chat input's microphone button: EmojiInput's right-edge slot, its states, tool tips
// and clicks (a click while the window records stops and sends; while sending or on a kept recording it
// only shows the window). Holding the button to record is tst_voicehold.cpp.

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QtTest>

#include <memory>

#include "audio/capturedevice.h"
#include "audio/fakecapture.h"
#include "emojiinput.h"
#include "fakets3.h"
#include "medialink.h"
#include "micbutton.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "uiutil.h"
#include "video/mfvideo.h"
#include "voicecontroller.h"
#include "voicefakes.h"
#include "voicepanel.h"

namespace {

using namespace voicefakes;
using Phase = VoiceController::Phase;
using Mode  = MicButton::Mode;

constexpr int kEncodeTimeoutMs = 60000; // five minutes of sound through the AAC encoder, on a slow machine

// A receiver for MicButton's gestures: holds are taken while takeHolds is set.
class Clicks : public QObject
{
  public:
    bool                    takeHolds = false;
    int                     pressed   = 0;
    int                     clicked   = 0;
    int                     hovered   = 0;
    QList<MicButton::Release> released;
    static bool onPress(QObject* r, MicButton*)
    {
        auto* c = static_cast<Clicks*>(r);
        c->pressed++;
        return c->takeHolds;
    }
    static void onRelease(QObject* r, MicButton::Release how) { static_cast<Clicks*>(r)->released.append(how); }
    static void onClick(QObject* r) { static_cast<Clicks*>(r)->clicked++; }
    static void onHover(QObject* r) { static_cast<Clicks*>(r)->hovered++; }
    MicButton::Handler handler() const
    {
        MicButton::Handler h;
        h.pressed  = &onPress;
        h.released = &onRelease;
        h.clicked  = &onClick;
        h.hovered  = &onHover;
        return h;
    }
};

} // namespace

class TestVoiceFlow : public QObject
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
        FakeTsMic::install(); // every flow mutes it while recording, and must give it back
        m_chat = FakeChat();
        m_chat.sound.levelDb = -18.0;
    }
    void cleanup()
    {
        // Whatever the test did, once its controller is gone TeamSpeak's microphone is on again.
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
    }

    // ---- the flow -----------------------------------------------------------------------------------
    void sendsAtOnce()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true; // paced like a microphone
        VoiceController voice(m_chat.host());
        QCOMPARE(MicButton::sharedState().mode, Mode::Ready);
        voice.start(VoiceController::Origin::Menu);
        QCOMPARE(voice.phase(), Phase::Starting);
        QVERIFY(voice.panel());
        QCOMPARE(m_chat.pauses, 1); // whatever plays is paused
        QCOMPARE(MicButton::sharedState().mode, Mode::Recording);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QCOMPARE(m_chat.microphones, 1);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 800, 5000);
        const VoicePanel::View recording = voice.panel()->view();
        QCOMPARE(recording.mode, VoicePanel::Mode::Recording);
        QCOMPARE(recording.limitMs, VoiceController::kMaxMs);
        QVERIFY(!recording.liveBins.isEmpty());
        QVERIFY(voice.panel()->sendButton()->isEnabled());

        // The big Send: stop and send, no review.
        voice.panel()->sendButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!voice.panel()); // the upload panel takes over
        const SendRequest& request = m_chat.sent.first();
        QCOMPARE(request.target.sch, fakets3::kConnection);
        QCOMPARE(request.items.size(), 1);
        const SendItem& item = request.items.first();
        QVERIFY(item.voice);
        QVERIFY(item.ownTemp);
        QVERIFY2(item.durationMs >= 800 && item.durationMs < 10000, qPrintable(QString::number(item.durationMs)));
        QCOMPARE(item.waveform.size(), MediaLink::kWaveformLevels);
        QVERIFY(QFileInfo(item.path).size() > 0);
        QCOMPARE(MicButton::sharedState().mode, Mode::Ready);
    }

    void enterSendsEscCancels()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        {
            VoiceController voice(m_chat.host());
            voice.start(VoiceController::Origin::Command);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 700, 5000);
            QTest::keyClick(voice.panel(), Qt::Key_Return);
            QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        }
        {
            VoiceController voice(m_chat.host());
            voice.start(VoiceController::Origin::Command);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 300, 5000);
            QTest::keyClick(voice.panel(), Qt::Key_Escape); // under 3 s: discarded without a question
            QCOMPARE(voice.phase(), Phase::Idle);
            QCOMPARE(m_chat.sent.size(), 1);
            QVERIFY(recordings().size() <= 1); // only the one Core was given (the test keeps it)
        }
    }

    void cancelAsksFromThreeSeconds()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= VoiceController::kConfirmFromMs + 100, 10000);
        voice.panel()->leftButton()->click(); // Cancel
        QCOMPARE(voice.phase(), Phase::Recording); // recording goes on while it asks
        QVERIFY(voice.panel()->view().confirmDiscard);
        QCOMPARE(voice.panel()->leftButton()->text(), QStringLiteral("&Keep"));
        QCOMPARE(voice.panel()->sendButton()->text(), QStringLiteral("&Discard"));
        QTest::keyClick(voice.panel(), Qt::Key_Escape); // Esc = Keep
        QVERIFY(!voice.panel()->view().confirmDiscard);
        QCOMPARE(voice.phase(), Phase::Recording);
        voice.requestCancel();
        QVERIFY(voice.panel()->view().confirmDiscard);
        voice.panel()->sendButton()->click(); // Discard
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(recordings().isEmpty());
    }

    void tooShort()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        // The sound ends after 200 ms: under half a second, nothing is sent.
        m_chat.sound.realtime   = false;
        m_chat.sound.endAfterMs = 200;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::TooShort, 5000);
        QCOMPARE(voice.panel()->view().mode, VoicePanel::Mode::TooShort);
        QVERIFY(voice.panel()->view().hints.value(0).contains(QStringLiteral("Too short")));
        QVERIFY(m_chat.sent.isEmpty());
        QVERIFY(recordings().isEmpty());
        // Nothing to show or send: the button records again ("Hold to record a voice message"); a click
        // alone (no hold) leaves "Too short" as it is.
        QCOMPARE(MicButton::sharedState().mode, Mode::Ready);
        voice.toggle();
        QCOMPARE(voice.phase(), Phase::TooShort);
        // The menu again: a new recording right away.
        m_chat.sound.endAfterMs = -1;
        m_chat.sound.realtime   = true;
        voice.start(VoiceController::Origin::Menu);
        QCOMPARE(voice.phase(), Phase::Starting);
        // Send before the microphone is even open: too short as well.
        voice.requestSend();
        QCOMPARE(voice.phase(), Phase::TooShort);
        voice.panel()->sendButton()->click(); // Close
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(m_chat.sent.isEmpty());
    }

    void limitSendsByItself()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = false; // endless, as fast as it is read: 5:00 comes in a moment
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_VERIFY_WITH_TIMEOUT(voice.phase() == Phase::Saving || !m_chat.sent.isEmpty(), kEncodeTimeoutMs);
        if (voice.phase() == Phase::Saving) { // the window says why it goes now
            const VoicePanel::View v = voice.panel()->view();
            QVERIFY(v.sending);
            QCOMPARE(v.timeMs, VoiceController::kMaxMs);
            QVERIFY2(v.hints.join(QLatin1Char('\n')).contains(QStringLiteral("Reached the 5:00 limit")), qPrintable(v.hints.join(QLatin1Char('|'))));
            QVERIFY(!voice.panel()->sendButton()->isEnabled());
        }
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        QCOMPARE(m_chat.sent.first().items.first().durationMs, VoiceController::kMaxMs);
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    void limitWaitsForTheQuestion()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = false;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_VERIFY_WITH_TIMEOUT(voice.phase() != Phase::Starting && voice.panel() && voice.panel()->view().timeMs >= VoiceController::kConfirmFromMs, kEncodeTimeoutMs);
        voice.requestCancel(); // "Discard this voice message?" while 5:00 comes
        QVERIFY(voice.panel()->view().confirmDiscard);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Saving, kEncodeTimeoutMs);
        QTest::qWait(1500);
        QVERIFY(m_chat.sent.isEmpty()); // never sent while you are asked
        QVERIFY(voice.panel()->view().confirmDiscard);
        voice.panel()->leftButton()->click(); // Keep: it goes
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
    }

    void lostMicrophoneKeepsTheRecording()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime      = false;
        m_chat.sound.unplugAfterMs = 1500;
        {
            VoiceController voice(m_chat.host());
            voice.start(VoiceController::Origin::Menu);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
            const VoicePanel::View v = voice.panel()->view();
            QCOMPARE(v.fix, VoicePanel::Fix::Send);
            QCOMPARE(v.errorTitle, QStringLiteral("The microphone was disconnected"));
            QVERIFY(v.errorBody.contains(QStringLiteral("0:01")));
            QVERIFY(m_chat.sent.isEmpty()); // not asked for: offered
            QTest::keyClick(voice.panel(), Qt::Key_Return); // Enter = Send
            QCOMPARE(m_chat.sent.size(), 1);
            QCOMPARE(voice.phase(), Phase::Idle);
        }
        // Lost within the first second: nothing worth keeping.
        m_chat.sound.unplugAfterMs = 300;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, 5000);
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::TryAgain);
        voice.panel()->leftButton()->click(); // Close
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(m_chat.sent.size(), 1);
    }

    void hiddenWindowStopsAndKeeps()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 1200, 5000);
        voice.panel()->hide(); // never recording without the window
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QVERIFY(voice.panel()->isVisible()); // shown again
        QCOMPARE(voice.panel()->view().errorTitle, QStringLiteral("Recording stopped"));
        QVERIFY(m_chat.sent.isEmpty());
        voice.panel()->sendButton()->click();
        QCOMPARE(m_chat.sent.size(), 1);
    }

    void notConnectedKeepsUntilBack()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 700, 5000);
        fakets3::setConnected(false);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().hintError, 5000); // "Not connected …" while recording
        voice.requestSend();
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QCOMPARE(voice.panel()->view().errorTitle, QStringLiteral("Not connected to this server"));
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
        QVERIFY(m_chat.sent.isEmpty());
        voice.requestSend(); // still offline: kept
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(voice.phase(), Phase::Error);
        fakets3::setConnected(true);
        voice.requestSend();
        QCOMPARE(m_chat.sent.size(), 1);
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    // ---- TeamSpeak's microphone ---------------------------------------------------------------------------
    void micIsGivenBackEveryWay()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        const auto recording  = [](VoiceController& voice) {
            voice.start(VoiceController::Origin::Menu);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED)); // muted before the microphone opens
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        };
        {
            VoiceController voice(m_chat.host()); // Esc
            recording(voice);
            QTest::keyClick(voice.panel(), Qt::Key_Escape);
            QCOMPARE(voice.phase(), Phase::Idle);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        }
        {
            VoiceController voice(m_chat.host()); // the window's close button
            recording(voice);
            QVERIFY(voice.panel()->close() == false); // asks the controller; under 3 s it discards
            QCOMPARE(voice.phase(), Phase::Idle);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        }
        {
            VoiceController voice(m_chat.host()); // plugin shutdown while recording
            recording(voice);
            voice.shutdown();
            QCOMPARE(voice.phase(), Phase::Idle);
            QVERIFY(!voice.panel());
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        }
        {
            auto voice = std::make_unique<VoiceController>(m_chat.host()); // deleted while recording
            recording(*voice);
            QCOMPARE(MicButton::sharedState().mode, Mode::Recording);
            voice.reset();
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
            QCOMPARE(MicButton::sharedState().mode, Mode::Ready); // no button stays "Recording"
        }
        {
            m_chat.sound.openError = voice::CaptureError::Busy; // the microphone fails to open
            VoiceController voice(m_chat.host());
            voice.start(VoiceController::Origin::Menu);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, 5000);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE)); // while the error shows
            m_chat.sound.openError = voice::CaptureError::None;
        }
        {
            VoiceController::Host host = m_chat.host();
            host.microphone            = nullptr; // no microphone at all
            VoiceController voice(host);
            voice.start(VoiceController::Origin::Menu);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, 5000);
            QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE)); // while the error shows
        }
        QVERIFY(m_chat.sent.isEmpty());
    }

    void droppedConnectionGivesTheMicBackLater()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime = true;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED));
        fakets3::setConnected(false); // TeamSpeak lost the connection while recording
        voice.requestCancel();         // under 3 s: discarded
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED)); // can't be read: left muted (never open)
        QTest::qWait(1300);
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_MUTED));
        fakets3::setConnected(true); // connected again: given back within a second
        QTRY_COMPARE_WITH_TIMEOUT(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE), 3000);
        QVERIFY(voice.diagnosticLines().join(QLatin1Char('\n')).contains(QStringLiteral("once connected again")));
    }

    // ---- the mic button in the other phases ------------------------------------------------------------
    void micButtonShowsAKeptRecording()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        // Unplugged at 4:00 (generated at once): the recording is kept, not sent; a click on the (Busy)
        // button while it is saved or kept only shows the window.
        m_chat.sound.realtime      = false;
        m_chat.sound.unplugAfterMs = 240000;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_VERIFY_WITH_TIMEOUT(voice.phase() == Phase::Saving || voice.phase() == Phase::Error, kEncodeTimeoutMs);
        if (voice.phase() == Phase::Saving) {
            QCOMPARE(MicButton::sharedState().mode, Mode::Busy);
            voice.toggle();
        }
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
        QCOMPARE(MicButton::sharedState().mode, Mode::Busy);
        voice.toggle();
        QCOMPARE(voice.phase(), Phase::Error);
        QVERIFY(voice.panel()->isVisible());
        QVERIFY(m_chat.sent.isEmpty()); // never sent by a click that promises to show the window
        voice.requestSend();
        QCOMPARE(m_chat.sent.size(), 1);
    }

    void closingAKeptRecordingAsks()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        m_chat.sound.realtime      = false;
        m_chat.sound.unplugAfterMs = 3500;
        {
            VoiceController voice(m_chat.host());
            voice.start(VoiceController::Origin::Menu);
            QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
            QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
            QTest::keyClick(voice.panel(), Qt::Key_Escape); // Close: 3.5 s would be lost
            QCOMPARE(voice.phase(), Phase::Error);
            QVERIFY(voice.panel()->view().confirmDiscard);
            QCOMPARE(voice.panel()->leftButton()->text(), QStringLiteral("&Keep"));
            QTest::keyClick(voice.panel(), Qt::Key_Escape); // Keep: back to the kept recording
            QVERIFY(!voice.panel()->view().confirmDiscard);
            QCOMPARE(voice.panel()->view().errorTitle, QStringLiteral("The microphone was disconnected"));
            QCOMPARE(voice.panel()->sendButton()->text(), QStringLiteral("&Send"));
            QVERIFY(voice.panel()->close() == false); // the close button asks too
            QVERIFY(voice.panel()->view().confirmDiscard);
            voice.panel()->sendButton()->click(); // Discard
            QCOMPARE(voice.phase(), Phase::Idle);
            QVERIFY(m_chat.sent.isEmpty());
            QVERIFY(recordings().isEmpty());
        }
        // Under 3 s: Close discards at once.
        m_chat.sound.unplugAfterMs = 1500;
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Error, kEncodeTimeoutMs);
        QCOMPARE(voice.panel()->view().fix, VoicePanel::Fix::Send);
        voice.panel()->leftButton()->click(); // Close
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(recordings().isEmpty());
    }

    void slowMicrophoneClickStops()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        // A microphone that takes 2.5 s to open (a Bluetooth headset switching modes): a click on the red
        // button meanwhile is not lost (after the double-click time): nothing was recorded, "Too short".
        VoiceController::Host host = m_chat.host();
        host.microphone = [](quint64, std::shared_ptr<voice::DeviceChoice> device) -> voice::VoiceRecorder::BackendFactory {
            return [device]() -> std::unique_ptr<voice::CaptureBackend> {
                QThread::msleep(2500);
                device->source = voice::DeviceChoice::Source::TeamSpeak;
                return std::make_unique<voice::FakeCapture>(voice::FakeCapture::Options());
            };
        };
        VoiceController voice(host);
        voice.start(VoiceController::Origin::Menu);
        QCOMPARE(voice.phase(), Phase::Starting);
        voice.toggle(); // a click right after it started (a double click's second): ignored
        QCOMPARE(voice.phase(), Phase::Starting);
        QTest::qWait(qMax(600, QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100));
        QCOMPARE(voice.phase(), Phase::Starting);
        voice.toggle();
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::TooShort, 8000);
        QVERIFY(m_chat.sent.isEmpty());
        QCOMPARE(FakeTsMic::muted, static_cast<int>(MUTEINPUT_NONE));
        // The menu from "Too short" starts a new recording at once (no waiting for it to close).
        voice.start(VoiceController::Origin::Menu);
        QCOMPARE(voice.phase(), Phase::Starting);
        voice.requestCancel();
        QCOMPARE(voice.phase(), Phase::Idle);
    }

    void blockedChatStartsNothing()
    {
        m_chat.canSend = false;
        m_chat.block   = QStringLiteral("Connect to a server to send voice messages.");
        VoiceController voice(m_chat.host());
        voice.start(VoiceController::Origin::Menu);
        QCOMPARE(voice.phase(), Phase::Idle);
        QVERIFY(!voice.panel());
        QCOMPARE(m_chat.microphones, 0);
        if (mf::available()) {
            QCOMPARE(MicButton::sharedState().mode, Mode::Unavailable);
            QCOMPARE(MicButton::sharedState().reason, m_chat.block);
        }
        m_chat.block.clear();
        voice.updateMicButtons();
        if (mf::available())
            QCOMPARE(MicButton::sharedState().mode, Mode::Ready);
    }

    void micButtonStopsTheWindowsRecording()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        // The Plugins menu (or /tsmedia voice) records in the window; the red button in the input is then a
        // plain button: a click stops and sends, as in 2.2.1 before hold to record.
        m_chat.sound.realtime = true;
        ChatLineEdit input;
        input.setAttribute(Qt::WA_DontShowOnScreen);
        input.resize(400, 30);
        EmojiInput emoji;
        emoji.attach(&input);
        input.show();
        MicButton* mic = MicButton::of(&input);
        QVERIFY(mic);
        VoiceController voice(m_chat.host());
        QTRY_VERIFY(mic->isVisible());
        QCOMPARE(mic->state().mode, Mode::Ready);
        voice.start(VoiceController::Origin::Menu);
        QVERIFY(voice.panel());
        QVERIFY(!voice.holding());
        QCOMPARE(mic->state().mode, Mode::Recording);
        QCOMPARE(mic->toolTip(), QStringLiteral("Stop and send the voice message"));
        // A click right after it started (a double click's second): it goes on.
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QVERIFY(voice.isActive());
        QVERIFY(!mic->holding()); // never a hold while the window records
        QTRY_COMPARE_WITH_TIMEOUT(voice.phase(), Phase::Recording, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(voice.panel()->view().timeMs >= 800, 5000);
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center()); // stop and send
        QTRY_COMPARE_WITH_TIMEOUT(m_chat.sent.size(), 1, kEncodeTimeoutMs);
        QCOMPARE(voice.phase(), Phase::Idle);
        QCOMPARE(mic->state().mode, Mode::Ready);
        QVERIFY(!voice.strip()); // the window's flow never shows the strip
    }

    // ---- the window ---------------------------------------------------------------------------------
    // TeamSpeak's Quit closes its main window, and Qt quits only if no other visible window without a
    // parent and with Qt::WA_QuitOnClose is left (QWidgetPrivate::close_helper). The recorder window has no
    // parent and may stay open then (the discard question refuses a close): it must not count, or TeamSpeak
    // keeps running without a window (live test, 2.2.1).
    void panelNeverKeepsTeamSpeakRunning()
    {
        const auto keepsRunning = [](const QWidget& w) { return w.isVisible() && !w.parentWidget() && w.testAttribute(Qt::WA_QuitOnClose); };
        QWidget main; // TeamSpeak's main window, as Qt sees it
        main.setAttribute(Qt::WA_DontShowOnScreen);
        main.show();
        QVERIFY(keepsRunning(main));
        VoicePanel panel;
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        VoicePanel::View v;
        v.mode           = VoicePanel::Mode::Recording;
        v.timeMs         = 12000;
        v.confirmDiscard = true;
        panel.setView(v);
        panel.show();
        QVERIFY(panel.isVisible());
        QVERIFY(!panel.parentWidget());
        QVERIFY(!keepsRunning(panel));
        QSignalSpy cancel(&panel, &VoicePanel::cancelRequested);
        QVERIFY(!panel.close()); // the question is up: a close changes nothing
        QVERIFY(panel.isVisible());
        QCOMPARE(cancel.count(), 0);
        QVERIFY(!keepsRunning(panel));
    }

    void panelButtonsAndKeys()
    {
        VoicePanel panel;
        panel.setAttribute(Qt::WA_DontShowOnScreen);
        panel.show();
        QSignalSpy send(&panel, &VoicePanel::sendRequested);
        QSignalSpy cancel(&panel, &VoicePanel::cancelRequested);
        QSignalSpy close(&panel, &VoicePanel::closeRequested);
        QSignalSpy fix(&panel, &VoicePanel::fixRequested);
        QSignalSpy keep(&panel, &VoicePanel::keepRequested);

        VoicePanel::View v;
        v.mode   = VoicePanel::Mode::Starting;
        panel.setView(v);
        QVERIFY(!panel.sendButton()->isEnabled()); // nothing to send before the microphone is open
        QTest::keyClick(&panel, Qt::Key_Return);
        QCOMPARE(send.count(), 0);

        v.mode   = VoicePanel::Mode::Recording;
        v.timeMs = 12000;
        panel.setView(v);
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("&Send"));
        QCOMPARE(panel.sendButton()->objectName(), QStringLiteral("voicePrimary"));
        QVERIFY(panel.sendButton()->isEnabled());
        QVERIFY(!panel.sendButton()->icon().isNull());
        QCOMPARE(panel.leftButton()->text(), QStringLiteral("&Cancel"));
        QVERIFY(panel.sendButton()->minimumHeight() >= 36); // the big one
        QVERIFY(panel.sendButton()->minimumWidth() > panel.leftButton()->minimumWidth());
        QTest::keyClick(&panel, Qt::Key_Return);
        QCOMPARE(send.count(), 1);
        QTest::keyClick(&panel, Qt::Key_Escape);
        QCOMPARE(cancel.count(), 1);
        QTest::keyClick(&panel, Qt::Key_Space); // no meaning any more (2.2.0: Stop for the review)
        QCOMPARE(send.count() + cancel.count(), 2);

        v.mode         = VoicePanel::Mode::Saving;
        v.sendingShown = true;
        v.sending      = true;
        panel.setView(v);
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("Sending…"));
        QVERIFY(!panel.sendButton()->isEnabled());
        QTest::keyClick(&panel, Qt::Key_Return);
        QCOMPARE(send.count(), 1);

        v.mode = VoicePanel::Mode::TooShort;
        panel.setView(v);
        QVERIFY(!panel.leftButton()->isVisibleTo(&panel));
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("&Close"));
        QTest::keyClick(&panel, Qt::Key_Escape);
        QCOMPARE(close.count(), 1);

        v.mode       = VoicePanel::Mode::Error;
        v.errorTitle = QStringLiteral("Recording stopped");
        v.fix        = VoicePanel::Fix::Send;
        v.fixText    = QStringLiteral("&Send");
        panel.setView(v);
        QCOMPARE(panel.sendButton()->text(), QStringLiteral("&Send"));
        QTest::keyClick(&panel, Qt::Key_Return);
        QCOMPARE(fix.count(), 1);
        QTest::keyClick(&panel, Qt::Key_Escape);
        QCOMPARE(close.count(), 2);

        v.mode           = VoicePanel::Mode::Recording;
        v.confirmDiscard = true;
        panel.setView(v);
        QCOMPARE(panel.leftButton()->text(), QStringLiteral("&Keep"));
        QCOMPARE(panel.sendButton()->objectName(), QStringLiteral("voiceDanger"));
        QTest::keyClick(&panel, Qt::Key_Escape);
        QCOMPARE(keep.count(), 1);
    }

    // ---- the mic button -------------------------------------------------------------------------------
    void micButtonSitsInTheEmojiSlot()
    {
        Settings& s       = Settings::instance();
        const bool before = s.emojiButton;
        s.emojiButton     = true;
        ChatLineEdit input;
        input.setAttribute(Qt::WA_DontShowOnScreen);
        input.resize(400, 30);
        input.show();
        QTRY_VERIFY(input.isVisible());
        const QRect plain = input.viewport()->geometry(); // the input's own viewport
        {
            EmojiInput emoji;
            emoji.attach(&input);
            MicButton*        mic   = MicButton::of(&input);
            QPointer<QWidget> smile = input.findChild<QWidget*>(QStringLiteral("tsmediaEmojiButton")); // the fallback (no TeamSpeak button here)
            QVERIFY(mic);
            QVERIFY(smile);
            QTRY_VERIFY(mic->isVisible());
            const QRect area = input.viewport()->geometry();
            // The right-edge slot is the microphone's; the emoji button left of it, the text left of both.
            QCOMPARE(area.right(), plain.right() - 60);
            QCOMPARE(mic->width(), 30);
            QCOMPARE(mic->x(), plain.right() - 29);
            QCOMPARE(smile->x(), area.right() + 1);
            QCOMPARE(mic->height(), area.height());
            QCOMPARE(mic->iconRect().width(), MicButton::kIcon);
            QVERIFY(mic->focusPolicy() == Qt::NoFocus);
            QCOMPARE(mic->objectName(), QStringLiteral("tsmediaMicButton")); // shutdown's sweep finds it

            // Without the emoji button in the input (the 2.2.1 takeover): the fallback is gone, the
            // microphone stays where it is.
            s.emojiButton = false;
            emoji.settingsChanged();
            QTRY_COMPARE(input.viewport()->geometry().right(), plain.right() - 30);
            QVERIFY(!smile);
            QVERIFY(mic->isVisible());
            QCOMPARE(mic->x(), plain.right() - 29);

            // A taller input: the icon goes to the bottom, like the emoji button's.
            input.resize(400, 90);
            QTRY_COMPARE(mic->height(), input.viewport()->height());
            QCOMPARE(mic->iconRect().bottom(), mic->height() - 3 - 1);
        }
        // Given back: no button, the input's own width.
        QVERIFY(!MicButton::of(&input));
        QCOMPARE(input.viewport()->geometry().right(), plain.right());
        s.emojiButton = before;
    }

    void micButtonStates()
    {
        MicButton::setSharedState(MicButton::State());
        ChatLineEdit input;
        input.setAttribute(Qt::WA_DontShowOnScreen);
        input.resize(300, 30);
        MicButton* mic = MicButton::placeIn(&input, QRect(270, 0, 30, 30));
        input.show();
        QTRY_VERIFY(mic->isVisible());
        QVERIFY(MicButton::anyVisible());
        QCOMPARE(mic->toolTip(), QStringLiteral("Hold to record a voice message"));
        QCOMPARE(mic->accessibleName(), QStringLiteral("Record a voice message"));
        QVERIFY(mic->accessibleDescription().contains(QStringLiteral("Press and hold to record, release to send")));
        QVERIFY(mic->accessibleDescription().contains(QStringLiteral("away from the button before you release to cancel")));
        QVERIFY(mic->accessibleDescription().contains(QStringLiteral("Plugins menu")));
        QVERIFY(mic->isEnabled());
        QCOMPARE(mic->cursor().shape(), Qt::PointingHandCursor);

        MicButton::State unavailable;
        unavailable.mode   = Mode::Unavailable;
        unavailable.reason = QStringLiteral("Voice messages can't be sent from password-protected channels. Join another channel.");
        MicButton::setSharedState(unavailable);
        QVERIFY(!mic->isEnabled());
        QCOMPARE(mic->toolTip(), unavailable.reason);
        QCOMPARE(mic->accessibleDescription(), unavailable.reason);
        QCOMPARE(mic->cursor().shape(), Qt::ArrowCursor);

        // Held: no tool tip (the strip says what to do), the pulse; none in the cancel zone.
        MicButton::State holding;
        holding.mode = Mode::Holding;
        MicButton::setSharedState(holding);
        QVERIFY(mic->isEnabled());
        QVERIFY(mic->toolTip().isEmpty());
        QCOMPARE(mic->accessibleName(), QStringLiteral("Recording a voice message"));
        QCOMPARE(mic->accessibleDescription(), QStringLiteral("Release to send. Move away to cancel."));
        QCOMPARE(mic->pulsing(), ui::animationsEnabled());
        holding.cancel = true;
        MicButton::setSharedState(holding);
        QCOMPARE(mic->accessibleDescription(), QStringLiteral("Release to cancel."));
        QVERIFY(!mic->pulsing());

        MicButton::State recording;
        recording.mode = Mode::Recording;
        MicButton::setSharedState(recording);
        QVERIFY(mic->isEnabled());
        QCOMPARE(mic->toolTip(), QStringLiteral("Stop and send the voice message"));
        QCOMPARE(mic->pulsing(), ui::animationsEnabled()); // still with Windows animations off
        // A button made later starts with the same state.
        ChatLineEdit other;
        MicButton* second = MicButton::placeIn(&other, QRect(0, 0, 30, 30));
        QCOMPARE(second->state(), recording);
        QCOMPARE(MicButton::placeIn(&other, QRect(0, 0, 30, 30)), second); // one per input

        MicButton::State busy;
        busy.mode = Mode::Busy;
        MicButton::setSharedState(busy);
        QVERIFY(!mic->pulsing());
        QCOMPARE(mic->toolTip(), QStringLiteral("Show the voice message window"));
        busy.reason = QStringLiteral("Sending the voice message…"); // a held one: no window to show
        MicButton::setSharedState(busy);
        QCOMPARE(mic->toolTip(), busy.reason);
        MicButton::setSharedState(MicButton::State());
        MicButton::removeFrom(&other);
        QVERIFY(!MicButton::of(&other));
    }

    void micButtonClicks()
    {
        MicButton::setSharedState(MicButton::State());
        ChatLineEdit input;
        input.setAttribute(Qt::WA_DontShowOnScreen);
        input.resize(300, 60);
        MicButton* mic = MicButton::placeIn(&input, QRect(270, 0, 30, 60)); // a tall strip: the icon at its bottom
        input.show();
        QTRY_VERIFY(mic->isVisible());
        auto clicks = std::make_unique<Clicks>();
        MicButton::setHandler(clicks.get(), clicks->handler());
        // A press the handler doesn't take as a hold: an ordinary click.
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QCOMPARE(clicks->pressed, 1);
        QCOMPARE(clicks->clicked, 1);
        QVERIFY(clicks->released.isEmpty());
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, QPoint(15, 5)); // above the icon (and its 3 px)
        QCOMPARE(clicks->pressed, 1);
        QCOMPARE(clicks->clicked, 1);
        // Taken: a hold, no click; it ends with the release.
        clicks->takeHolds = true;
        QTest::mousePress(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QVERIFY(mic->holding());
        QVERIFY(mic->isDown());
        QTest::mouseRelease(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QVERIFY(!mic->holding());
        QVERIFY(!mic->isDown());
        QCOMPARE(clicks->clicked, 1);
        QCOMPARE(clicks->released.size(), 1);
        QVERIFY(clicks->released.first() == MicButton::Release::Send);
        MicButton::State unavailable;
        unavailable.mode = Mode::Unavailable;
        MicButton::setSharedState(unavailable);
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QCOMPARE(clicks->clicked, 1); // disabled: nothing
        QCOMPARE(clicks->pressed, 2);
        MicButton::setSharedState(MicButton::State());
        clicks.reset(); // the receiver goes: nothing is called
        QTest::mouseClick(mic, Qt::LeftButton, Qt::NoModifier, mic->iconRect().center());
        QVERIFY(!mic->holding());
        MicButton::clearHandler(nullptr);
    }

  private:
    std::unique_ptr<QTemporaryDir> m_dir;
    FakeChat                       m_chat;
    TS3Functions                   m_savedFuncs{};
    Settings                       m_savedSettings;
};

TSMEDIA_REGISTER_TEST(TestVoiceFlow)

#include "tst_voiceflow.moc"
