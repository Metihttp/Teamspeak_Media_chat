#pragma once

// 2.2.1 voice: the fakes the recorder's flow tests share (tst_voiceflow.cpp, tst_voicehold.cpp): the chat
// side of VoiceController (one channel of tests/fakets3, what was sent and paused, generated sound, never a
// microphone), TeamSpeak's microphone mute flag as MicGuard reads and sets it, and TeamSpeak's chat input
// class name.

#include <QDir>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextEdit>

#include <memory>

#include "audio/capturedevice.h"
#include "audio/fakecapture.h"
#include "fakets3.h"
#include "ts3api.h"
#include "voicecontroller.h"

namespace voicefakes {

// TeamSpeak's chat input is a QTextEdit subclass of this name.
class ChatLineEdit : public QTextEdit
{
  public:
    using QTextEdit::QTextEdit;
};

// The chat side of the recorder: one channel of the fake TeamSpeak, what was sent, what was paused.
struct FakeChat {
    QList<SendRequest>          sent;
    int                         pauses = 0;
    bool                        canSend = true; // target(): false means "the chat said why"
    QString                     block;          // blockReason()
    voice::FakeCapture::Options sound;
    int                         microphones = 0; // factories handed out
    bool                        watchPointer = false; // hold to record: Host::pointerHeld reads mouseDown
    bool                        mouseDown    = true;
    int                         visibleMode  = TextMessageTarget_CHANNEL; // visibleTarget(): the chat the input shows

    VoiceController::Host host()
    {
        VoiceController::Host h;
        h.target = [this](ChatTarget* target, QString* description, QWidget** anchor) {
            if (!canSend)
                return false;
            target->sch  = fakets3::kConnection;
            target->mode = TextMessageTarget_CHANNEL;
            *description = QStringLiteral("the channel “Lobby”");
            *anchor      = nullptr;
            return true;
        };
        h.blockReason      = [this] { return block; };
        h.describe         = [](const ChatTarget&) { return QStringLiteral("the channel “Lobby”"); };
        h.pauseAllPlayback = [this] { ++pauses; };
        h.send             = [this](const SendRequest& request) { sent.append(request); };
        h.microphone       = [this](quint64, std::shared_ptr<voice::DeviceChoice> device) -> voice::VoiceRecorder::BackendFactory {
            ++microphones;
            const voice::FakeCapture::Options options = sound;
            return [options, device]() -> std::unique_ptr<voice::CaptureBackend> {
                device->source = voice::DeviceChoice::Source::TeamSpeak;
                return std::make_unique<voice::FakeCapture>(options);
            };
        };
        if (watchPointer)
            h.pointerHeld = [this] { return mouseDown; };
        h.visibleTarget = [this] {
            ChatTarget t;
            t.sch  = fakets3::kConnection;
            t.mode = visibleMode;
            return t;
        };
        h.offscreen = true;
        return h;
    }
};

// TeamSpeak's own microphone on the fake connection, as MicGuard reads and sets it: a capture device is
// open, CLIENT_INPUT_MUTED as stored here; unreadable while the fake is disconnected.
struct FakeTsMic {
    static inline int muted = MUTEINPUT_NONE;
    static inline int sets  = 0;

    static unsigned int get(uint64 sch, size_t flag, int* result)
    {
        if (sch != fakets3::kConnection || !ts3::isConnected(sch))
            return ERROR_not_connected;
        *result = flag == CLIENT_INPUT_HARDWARE ? 1 : muted;
        return ERROR_ok;
    }
    static unsigned int set(uint64 sch, size_t flag, int value)
    {
        if (sch != fakets3::kConnection || !ts3::isConnected(sch))
            return ERROR_not_connected;
        if (flag == CLIENT_INPUT_MUTED) {
            muted = value;
            ++sets;
        }
        return ERROR_ok;
    }
    static unsigned int flush(uint64 sch, const char*) { return ts3::isConnected(sch) ? ERROR_ok : ERROR_not_connected; }
    static void install()
    {
        muted                                 = MUTEINPUT_NONE;
        sets                                  = 0;
        ts3::funcs.getClientSelfVariableAsInt = &get;
        ts3::funcs.setClientSelfVariableAsInt = &set;
        ts3::funcs.flushClientSelfUpdates     = &flush;
    }
};

// The recordings waiting in the voice folder.
inline QStringList recordings()
{
    return QDir(ts3::dataDir() + QStringLiteral("/voice")).entryList({QStringLiteral("voice_*.m4a")}, QDir::Files);
}

} // namespace voicefakes
