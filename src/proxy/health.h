#pragma once

// 2.6 chat proxy: route health, custom mode only (design 5.2). Pure; a clock function decides (tests inject one), no
// timers: proxy::Control calls tick() from its own owned timer.
//
//  * Keyed by the route generation: reset() starts a new one Unknown; reports of another generation are ignored.
//  * report(): only route-level failures count. A decisive one (isDecisive(failure, step)) -> Down at once. Ambiguous
//    ones are kept for 30 s: Down when the window holds at least 3 of them for at least 2 different sites (registrable
//    domains) and no success came in those 30 s (F11). The same site failing three times is that site's problem.
//  * reportOk() (a site answered through the route, a player got ready, a passed Test) -> Working, and it ends Down at
//    once.
//  * Down -> Probation after a fixed 30 s (the proxy is on this PC) or 60 s (elsewhere); no backoff (F21). In
//    Probation one automatic load is admitted at a time; its success -> Working, its route-level failure -> Down
//    again (same delay). A probe that never reports frees the slot after 30 s. Clicks are admitted in every state.

#include <QPair>
#include <QString>
#include <QVector>

#include <functional>

#include "failure.h"

namespace proxy {

class Health
{
  public:
    enum class State { Unknown, Working, Down, Probation };
    static constexpr qint64 kWindowMs         = 30 * 1000;
    static constexpr int    kAmbiguousCount   = 3;
    static constexpr int    kAmbiguousSites   = 2;
    static constexpr qint64 kLocalDelayMs     = 30 * 1000;
    static constexpr qint64 kRemoteDelayMs    = 60 * 1000;
    static constexpr qint64 kProbeSlotMs      = 30 * 1000;

    explicit Health(std::function<qint64()> clockMs = {});

    void    reset(quint64 generation, bool proxyOnThisPc); // a new generation: Unknown, nothing remembered
    quint64 generation() const { return m_generation; }

    // Each returns true when the state changed.
    bool report(quint64 generation, Failure failure, Step step, const QString& site);
    bool reportOk(quint64 generation);
    bool tick(); // the clock's transitions: Down -> Probation; a probation slot that was never reported freed (both true)

    // The admission of one load (design 3.3.5 routeGate): clicks always; automatic loads not while Down, and in
    // Probation only one at a time (this call takes the slot).
    bool admits(bool userAsked);

    State   state() const { return m_state; }
    Failure lastFailure() const { return m_lastFailure; }
    qint64  downSinceMs() const { return m_state == State::Down || m_state == State::Probation ? m_downAt : -1; }
    // When tick() has something to do next, in ms from now (-1: nothing pending).
    qint64  msUntilTick() const;
    qint64  probationDelayMs() const { return m_onThisPc ? kLocalDelayMs : kRemoteDelayMs; }

  private:
    qint64 now() const;
    void   goDown(Failure failure);

    std::function<qint64()>         m_clock;
    quint64                         m_generation = 0;
    bool                            m_onThisPc   = true;
    State                           m_state      = State::Unknown;
    Failure                         m_lastFailure = Failure::None;
    qint64                          m_downAt     = 0;
    qint64                          m_lastOkAt   = -1;
    qint64                          m_slotTakenAt = -1; // Probation: when the probe was admitted (-1: the slot is free)
    QVector<QPair<qint64, QString>> m_ambiguous;        // (time, site) of ambiguous route-level failures
};

} // namespace proxy
