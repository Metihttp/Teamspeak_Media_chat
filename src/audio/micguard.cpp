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
    // Still muted by an earlier recording whose connection couldn't be read when it ended: ours again,
    // given back with this one (readable now and still muted), or forgotten (unmuted by hand).
    const QList<quint64> pending = m_pending;
    m_pending.clear();
    for (const quint64 sch : pending) {
        int muted = kMuteNone;
        if (!m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &muted))
            m_pending.append(sch); // still unreadable: retried later
        else if (muted == kMuteMuted)
            m_muted.append(sch);
    }
    for (const quint64 sch : m_env.connections()) {
        if (m_muted.contains(sch))
            continue;
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

// Unmutes sch if it still reads muted. False if it can't be read; *restored: it was unmuted here (not
// when the user had unmuted it by hand already).
bool MicGuard::giveBack(quint64 sch, bool* restored)
{
    *restored = false;
    int value = kMuteNone;
    if (!m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &value))
        return false;
    if (value != kMuteMuted)
        return true; // already unmuted by hand
    // One more try on failure: staying muted is safe, but it is not what the user expects.
    if (m_env.setInputMuted(sch, false) || m_env.setInputMuted(sch, false)) {
        *restored = true;
        return true;
    }
    // Unmuted here although the server wasn't told (the flush failed): given back all the same.
    int now   = kMuteMuted;
    *restored = m_env.readVariable(sch, MicEnvironment::Variable::InputMuted, &now) && now == kMuteNone;
    return true;
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
        bool restored = false;
        if (!giveBack(sch, &restored)) {
            // Unreadable (disconnected): left muted (fail closed), and given back once it can be read
            // again (retryPending).
            ++result.kept;
            ++result.pending;
            if (!m_pending.contains(sch))
                m_pending.append(sch);
            continue;
        }
        if (restored)
            ++result.restored;
        else
            ++result.kept; // unmuted by hand meanwhile, or the unmute didn't take
    }
    return result;
}

int MicGuard::retryPending()
{
    if (m_engaged || m_pending.isEmpty())
        return 0; // while recording, engage() has taken them over
    int                  restored = 0;
    const QList<quint64> pending  = m_pending;
    m_pending.clear();
    for (const quint64 sch : pending) {
        bool back = false;
        if (!giveBack(sch, &back))
            m_pending.append(sch); // still unreadable
        else if (back)
            ++restored;
    }
    return restored;
}

} // namespace voice
