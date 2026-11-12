#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// 2.5: which looping videos get one of the loop pool's players (pure; unit-tested). A loop wants to play while it
// is visible (autoplay) or hovered. Players stay on the loops that want them; a new one takes a free player, else
// the least recently used invisible loop's, else nothing (it shows its poster); only the hovered loop may take the
// player of the visible loop used longest ago. No churn: the same view gives the same plan.
namespace loopplayers {
struct Slot {
    QString key;
    bool    hasPlayer = false;
    bool    visible   = false;
    bool    hovered   = false;
    quint64 lastUsed  = 0;
};
struct Plan {
    QStringList start; // make a player and play
    QStringList evict; // destroy the player (it goes back to its poster)
    QStringList play;  // has a player: play
    QStringList pause; // has a player: pause
};

inline Plan plan(const QVector<Slot>& loops, bool autoplay, int maxPlayers)
{
    Plan        out;
    int         players = 0;
    QSet<QString> evicted;
    for (const Slot& s : loops)
        players += s.hasPlayer ? 1 : 0;
    const auto wants = [autoplay](const Slot& s) { return s.visible && (autoplay || s.hovered); };
    for (const Slot& s : loops) {
        if (!s.hasPlayer)
            continue;
        if (wants(s))
            out.play << s.key;
        else
            out.pause << s.key;
    }
    // The hovered loop first, then the others in their order.
    QVector<Slot> order;
    for (const Slot& s : loops) {
        if (s.hovered)
            order.prepend(s);
        else
            order.append(s);
    }
    for (const Slot& s : qAsConst(order)) {
        if (s.hasPlayer || !wants(s))
            continue;
        if (players < maxPlayers) {
            out.start << s.key;
            ++players;
            continue;
        }
        // A player of a loop out of view, the least recently used; for the hovered loop also a visible one.
        const Slot* victim = nullptr;
        for (const Slot& o : loops) {
            if (!o.hasPlayer || evicted.contains(o.key) || o.key == s.key || o.hovered)
                continue;
            if (o.visible && !s.hovered)
                continue;
            if (!victim || (victim->visible && !o.visible) || (victim->visible == o.visible && o.lastUsed < victim->lastUsed))
                victim = &o;
        }
        if (!victim)
            continue; // every player is on a loop in view: this one shows its poster
        evicted.insert(victim->key);
        out.evict << victim->key;
        out.play.removeAll(victim->key);
        out.pause.removeAll(victim->key);
        out.start << s.key;
    }
    return out;
}

} // namespace loopplayers
