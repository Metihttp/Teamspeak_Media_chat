#pragma once

// 2.2 voice: mutes the user's TeamSpeak microphone while a voice message is recorded, so people with
// voice activation aren't heard live in the channel, and always gives it back.
//
// engage(): every connection whose capture device is open (CLIENT_INPUT_HARDWARE) and whose microphone
// is on (CLIENT_INPUT_MUTED == MUTEINPUT_NONE) is muted, and remembered. release(): those connections
// are unmuted again, but only where the microphone is still muted: if the user unmuted it by hand in
// the meantime, nothing changes. A connection whose state can't be read (disconnected) is left as it is:
// it fails closed (muted), never open. CLIENT_INPUT_DEACTIVATED (push-to-talk) is never touched.
// The destructor releases, so every way out (stop, cancel, error, plugin shutdown) gives the mic back.
//
// Pure logic: TeamSpeak is behind MicEnvironment (voicecontroller.cpp has the real one; the unit tests
// a fake).

#include <QList>
#include <QtGlobal>

namespace voice {

class MicEnvironment
{
  public:
    enum class Variable { InputHardware, InputMuted }; // CLIENT_INPUT_HARDWARE, CLIENT_INPUT_MUTED

    virtual ~MicEnvironment() = default;
    virtual QList<quint64> connections() const = 0;
    // False if the value can't be read (not connected, ...).
    virtual bool readVariable(quint64 sch, Variable variable, int* value) const = 0;
    // Sets CLIENT_INPUT_MUTED and sends it to the server (flushClientSelfUpdates). False on failure.
    virtual bool setInputMuted(quint64 sch, bool muted) = 0;
};

class MicGuard
{
  public:
    explicit MicGuard(MicEnvironment& environment);
    ~MicGuard();
    MicGuard(const MicGuard&)            = delete;
    MicGuard& operator=(const MicGuard&) = delete;

    struct Released {
        int restored = 0; // unmuted again
        int kept     = 0; // left muted on purpose: unmuted by hand already, or unreadable (fail closed)
    };

    // Mutes as described above; returns how many connections it muted (0 is fine: nothing to do).
    // Engaging twice does not mute anything new.
    int      engage();
    Released release();

    bool           engaged() const { return m_engaged; }
    QList<quint64> mutedConnections() const { return m_muted; }

  private:
    MicEnvironment& m_env;
    QList<quint64>  m_muted;
    bool            m_engaged = false;
};

} // namespace voice
