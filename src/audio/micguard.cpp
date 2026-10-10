#include "audio/micguard.h"

namespace voice {

namespace {
constexpr int kMuteNone  = 0; // MUTEINPUT_NONE
constexpr int kMuteMuted = 1; // MUTEINPUT_MUTED
} // namespace

MicGuard::MicGuard(MicEnvironment& environment)
    : m_env(environment)
{
}

MicGuard::~MicGuard()
{
    release();
}

int MicGuard::engage()
{
    if (m_engaged)
        return m_muted.size();
    m_engaged = true;
    for (const quint64 sch : m_env.connections()) {
        int hardware = 0;
        int muted    = kMuteMuted;
        if (!m_env.readVariable(sch, MicEnvironment::Variable::InputHardware, &hardware) || hardware == 0)
            continue; // no capture device open: nobody hears this connection's mic anyway
        if (!m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &muted) || muted != kMuteNone)
            continue; // already muted by the user: theirs to keep
        if (m_env.setInputMuted(sch, true)) {
            m_muted.append(sch);
            continue;
        }
        // The flag can be set even though sending it to the server failed (flushClientSelfUpdates): the
        // microphone is muted all the same, so it is ours to give back.
        int now = kMuteNone;
        if (m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &now) && now == kMuteMuted)
            m_muted.append(sch);
    }
    return m_muted.size();
}

MicGuard::Released MicGuard::release()
{
    Released result;
    if (!m_engaged)
        return result;
    m_engaged = false;
    const QList<quint64> muted = m_muted;
    m_muted.clear();
    for (const quint64 sch : muted) {
        int value = kMuteNone;
        if (!m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &value) || value != kMuteMuted) {
            ++result.kept; // unreadable (fail closed) or already unmuted by hand
            continue;
        }
        // One more try on failure: staying muted is safe, but it is not what the user expects.
        if (m_env.setInputMuted(sch, false) || m_env.setInputMuted(sch, false)) {
            ++result.restored;
            continue;
        }
        // Unmuted here although the server wasn't told (the flush failed): given back all the same.
        int now = kMuteMuted;
        if (m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &now) && now == kMuteNone)
            ++result.restored;
        else
            ++result.kept;
    }
    return result;
}

} // namespace voice
