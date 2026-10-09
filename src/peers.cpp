#include "peers.h"

#include <QPointer>
#include <QTimer>

#include <algorithm>

#include "i18n.h"

namespace peers {

namespace {

// TextMessageTarget_* (public_definitions.h); this file doesn't include the SDK (unit tests).
constexpr int kTargetClient  = 1;
constexpr int kTargetChannel = 2;
constexpr int kTargetServer  = 3;

constexpr int kNameChars    = 32; // per name in the line and the "Who?" panel
constexpr int kNamesPerList = 10;

bool isBidiOrInvisible(QChar ch)
{
    const ushort c = ch.unicode();
    return c == 0x061c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

QString nameList(const QStringList& names)
{
    QStringList shown;
    for (int i = 0; i < names.size() && i < kNamesPerList; ++i)
        shown.append(presenceName(names.at(i)));
    const int rest = names.size() - shown.size();
    if (rest > 0)
        return i18n::t("%1 and %2 more").arg(shown.join(QStringLiteral(", "))).arg(rest);
    return shown.join(QStringLiteral(", "));
}

} // namespace

// ---- texts ----------------------------------------------------------------------------------------

QString presenceName(const QString& nickname)
{
    QString out;
    out.reserve(nickname.size());
    for (const QChar ch : nickname) {
        const QChar::Category category = ch.category();
        if (isBidiOrInvisible(ch) || category == QChar::Other_Control || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            continue;
        out += ch;
    }
    out = out.trimmed();
    if (out.isEmpty())
        return i18n::t("Someone");
    if (out.size() > kNameChars) {
        int cut = kNameChars - 1;
        if (out.at(cut - 1).isHighSurrogate())
            --cut;
        out = out.left(cut) + QChar(0x2026);
    }
    return out;
}

QString presenceText(const PresenceSummary& s)
{
    switch (s.kind) {
    case PresenceSummary::Kind::Hidden:
        return {};
    case PresenceSummary::Kind::Server:
        return i18n::t("People without TS Media get a download link.");
    case PresenceSummary::Kind::Private: {
        if (s.partner.isEmpty())
            return {};
        const QString name = presenceName(s.partner);
        switch (s.partnerState) {
        case PeerState::Has:
            return i18n::t("%1 has TS Media and will see it in the chat.").arg(name);
        case PeerState::Without:
            return i18n::t("%1 doesn't seem to have TS Media, so they'll get a download link.").arg(name);
        case PeerState::Unknown:
        case PeerState::Checking:
            break;
        }
        return i18n::t("Checking whether %1 has TS Media…").arg(name);
    }
    case PresenceSummary::Kind::Channel:
        break;
    }

    const int others = s.others();
    const int has    = s.has.size();
    if (others == 0)
        return i18n::t("You're the only one in this channel. No one else will get this message.");
    if (s.checking.size() == others || (has == 0 && !s.checking.isEmpty()))
        return i18n::t("Checking who here has TS Media…");
    if (!s.checking.isEmpty())
        return i18n::t("So far %1 of %2 people here will see it in the chat.").arg(has).arg(others);
    if (others == 1) {
        return has == 1 ? i18n::t("%1 will see it in the chat.").arg(presenceName(s.has.first()))
                        : i18n::t("%1 doesn't seem to have TS Media, so they'll get a download link.").arg(presenceName(s.without.first()));
    }
    if (has == others)
        return i18n::t("All %1 people here will see it in the chat.").arg(others);
    if (has == 0)
        return i18n::t("Nobody else here seems to have TS Media, so they'll get a download link.");
    return i18n::t("%1 of %2 people here will see it in the chat. The others get a download link.").arg(has).arg(others);
}

QStringList presenceDetails(const PresenceSummary& s)
{
    QStringList lines;
    if (s.kind == PresenceSummary::Kind::Channel) {
        if (!s.has.isEmpty())
            lines << i18n::t("Will see it in the chat: %1").arg(nameList(s.has));
        if (!s.without.isEmpty())
            lines << i18n::t("Will get a link: %1").arg(nameList(s.without));
        if (!s.checking.isEmpty())
            lines << i18n::t("Still checking: %1").arg(nameList(s.checking));
    } else if (s.kind != PresenceSummary::Kind::Private || s.partner.isEmpty()) {
        return lines;
    }
    if (!lines.isEmpty() || s.kind == PresenceSummary::Kind::Private)
        lines << i18n::t("People on TS Media 2.1 or older count as getting a link.");
    return lines;
}

// ---- PeerDirectory --------------------------------------------------------------------------------

PeerDirectory::PeerDirectory(Env& env, Sender& sender, QObject* parent)
    : PeerDirectory(env, sender, Limits(), parent)
{
}

PeerDirectory::PeerDirectory(Env& env, Sender& sender, const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_env(env)
    , m_sender(sender)
    , m_limits(limits)
{
    // A member timer (child of this): nothing of it can be pending once the directory is gone.
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
}

PeerDirectory::~PeerDirectory() = default;

void PeerDirectory::setAutoTimer(bool on)
{
    m_autoTimer = on;
    if (!on)
        m_timer->stop();
    else
        reschedule();
}

bool PeerDirectory::presenceOn() const
{
    return m_local.presence;
}

QStringList PeerDirectory::caps() const
{
    QStringList list{QStringLiteral("p")};
    if (m_local.reactions)
        list << QStringLiteral("r");
    return list;
}

void PeerDirectory::setLocal(const Local& local)
{
    const bool wasOn = presenceOn();
    m_local          = local;
    if (wasOn && !local.presence) {
        sayGoodbye();
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
            it->helloDue = -1;
            it->hiDue    = -1;
            it->hiTargets.clear();
            it->syncAt = -1;
        }
        // What the transport still holds back (a flood pause, its reserve) doesn't go out either. A
        // HELLO already on its way is taken back once its answer comes (helloDone).
        m_sender.dropQueued("HELLO");
        m_sender.dropQueued("HI");
    } else if (!wasOn && local.presence) {
        const qint64 now = m_env.nowMs();
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
            if (it->channel == 0)
                continue;
            it->helloAttempts = 0;
            it->helloDue      = now + m_limits.settleMs;
        }
    }
    for (quint64 sch : m_conns.keys())
        changed(sch);
    reschedule();
}

// ---- events ---------------------------------------------------------------------------------------

void PeerDirectory::connected(quint64 sch)
{
    const quint64 channel = m_env.ownChannel(sch);
    if (channel != 0)
        enterChannel(sch, channel);
}

void PeerDirectory::disconnected(quint64 sch)
{
    if (m_conns.remove(sch) > 0)
        emit presenceChanged(sch);
    reschedule();
}

void PeerDirectory::clientMoved(quint64 sch, ClientId client, quint64 fromChannel, quint64 toChannel)
{
    const ClientId own = m_env.ownClientId(sch);
    if (own != 0 && client == own) {
        if (toChannel == 0)
            disconnected(sch);
        else if (toChannel != channelOf(sch) || !m_conns.contains(sch))
            enterChannel(sch, toChannel);
        return;
    }
    auto it = m_conns.find(sch);
    if (it == m_conns.end() || it->channel == 0)
        return;
    Conn& c = it.value();
    if (toChannel == c.channel && !c.members.contains(client)) {
        addMember(c, sch, client, m_env.nowMs() + m_limits.joinerTimeoutMs);
        changed(sch);
    } else if (fromChannel == c.channel && toChannel != c.channel) {
        if (c.members.remove(client) > 0)
            changed(sch);
    }
    reschedule();
}

void PeerDirectory::clientRenamed(quint64 sch, ClientId client, const QString& name)
{
    auto it = m_conns.find(sch);
    if (it == m_conns.end() || name.isEmpty())
        return;
    bool any = false;
    auto m   = it->members.find(client);
    if (m != it->members.end()) {
        m->name = name;
        any     = true;
    }
    for (auto p = it->peers.begin(); p != it->peers.end(); ++p) {
        if (p->clientId == client)
            p->name = name;
    }
    if (any)
        changed(sch);
}

PeerDirectory::PeerInfo& PeerDirectory::remember(Conn& c, const QString& uid, ClientId client, const QString& name)
{
    PeerInfo& p = c.peers[uid];
    p.clientId  = client;
    if (!name.isEmpty())
        p.name = name;
    return p;
}

void PeerDirectory::received(quint64 sch, ClientId from, const QString& uid, const QString& name, const proto::Message& message)
{
    if (uid.isEmpty() || from == 0)
        return;
    Conn&        c   = m_conns[sch];
    const qint64 now = m_env.nowMs();

    if (message.type == "BYE") {
        PeerInfo& p = remember(c, uid, from, name);
        p.state     = PeerState::Without;
        p.caps.clear();
        p.pingDeadline = -1;
        for (auto m = c.members.begin(); m != c.members.end(); ++m) {
            if (m->uid == uid)
                m->state = PeerState::Without;
        }
        changed(sch);
        reschedule();
        return;
    }

    const std::optional<proto::Hello> hello = proto::readHello(message);
    if (!hello)
        return;
    PeerInfo& p    = remember(c, uid, from, name);
    p.state        = PeerState::Has;
    p.version      = hello->version;
    p.caps         = hello->caps;
    p.heardMs      = now;
    p.pingDeadline = -1;

    const quint64 where     = m_env.channelOf(sch, from);
    const bool    inChannel = c.channel != 0 && where == c.channel;
    auto          member    = c.members.find(from);
    if (member != c.members.end())
        member->state = PeerState::Has;
    else if (inChannel)
        addMember(c, sch, from, -1); // the move event may come later; known now anyway

    if (hello->replyRequested && presenceOn() && !m_sender.blocked(sch) && where != 0) {
        PeerInfo&   info = c.peers[uid];
        const int   gap  = inChannel ? m_limits.answerInChannelMs : m_limits.answerOutsideMs;
        const bool  due  = info.answeredMs < 0 || now - info.answeredMs >= gap || info.answeredId != from;
        if (due) {
            info.answeredMs = now;
            info.answeredId = from;
            c.hiTargets.insert(from);
            const qint64 at = now + m_env.randomBetween(m_limits.hiJitterMinMs, m_limits.hiJitterMaxMs);
            c.hiDue         = c.hiDue < 0 ? at : qMin(c.hiDue, at);
        }
    }
    changed(sch);
    reschedule();
}

void PeerDirectory::commandsBlocked(quint64 sch)
{
    auto it = m_conns.find(sch);
    if (it != m_conns.end()) {
        it->helloDue = -1;
        it->hiDue    = -1;
        it->hiTargets.clear();
        it->syncAt = -1;
    }
    changed(sch);
    reschedule();
}

// ---- channel entry and HELLO ----------------------------------------------------------------------

void PeerDirectory::addMember(Conn& c, quint64 sch, ClientId client, qint64 deadlineMs)
{
    if (client == 0 || client == m_env.ownClientId(sch) || m_env.isQueryClient(sch, client))
        return;
    const QString uid = m_env.uid(sch, client);
    if (uid.isEmpty() || uid == c.ownUid)
        return; // own clones are not "people here"
    Member m;
    m.uid  = uid;
    m.name = m_env.nickname(sch, client);
    const auto known = c.peers.constFind(uid);
    if (known != c.peers.constEnd() && known->state == PeerState::Has) {
        m.state = PeerState::Has;
    } else {
        m.state      = PeerState::Checking;
        m.deadlineMs = deadlineMs;
    }
    c.members.insert(client, m);
}

void PeerDirectory::enterChannel(quint64 sch, quint64 channel)
{
    const qint64 now = m_env.nowMs();
    Conn&        c   = m_conns[sch];
    c.channel        = channel;
    c.enteredMs      = now;
    ++c.generation;
    c.ownUid = m_env.uid(sch, m_env.ownClientId(sch));
    c.members.clear();
    for (const ClientId client : m_env.channelClients(sch, channel))
        addMember(c, sch, client, -1);

    c.helloAttempts = 0;
    if (presenceOn() && !m_sender.blocked(sch)) {
        // Hopping through channels: the settle time restarts, so only the last channel gets a HELLO.
        qint64 due = now + m_limits.settleMs;
        if (c.lastHelloMs >= 0)
            due = qMax(due, c.lastHelloMs + m_limits.helloMinIntervalMs);
        c.helloDue = due;
    } else {
        c.helloDue = -1;
    }
    c.syncAt    = now + m_limits.syncDelayMs;
    c.syncUntil = now + m_limits.settleMs + m_limits.memberTimeoutMs + 1000;
    changed(sch);
    reschedule();
}

void PeerDirectory::sendHello(quint64 sch)
{
    auto it = m_conns.find(sch);
    if (it == m_conns.end())
        return;
    it->helloDue = -1;
    if (!presenceOn() || m_sender.blocked(sch))
        return;
    it->lastHelloMs   = m_env.nowMs();
    it->helloInFlight = true;
    ++it->helloAttempts;
    const quint64           generation = it->generation;
    QPointer<PeerDirectory> guard(this);
    m_sender.send(sch, proto::makeHello(m_local.version, caps(), true), Target::toChannel(), [guard, sch, generation](SendResult result) {
        if (guard)
            guard->helloDone(sch, generation, result);
    });
}

void PeerDirectory::helloDone(quint64 sch, quint64 generation, SendResult result)
{
    auto it = m_conns.find(sch);
    if (it == m_conns.end())
        return;
    Conn&        c   = it.value();
    const qint64 now = m_env.nowMs();
    // It reached the channel: announced, unless presence was switched off meanwhile (then a BYE takes
    // it back, as sayGoodbye would have).
    const auto arrived = [this, &c, sch] {
        if (presenceOn())
            c.announced = true;
        else if (!c.announced && !m_sender.blocked(sch))
            m_sender.send(sch, proto::makeBye(), Target::toChannel(), {});
    };
    if (result == SendResult::LateOk) {
        arrived(); // it counted as failed (that was handled then), but it did arrive
        changed(sch);
        return;
    }
    c.helloInFlight = false;
    if (result == SendResult::Blocked) {
        commandsBlocked(sch);
        return;
    }
    if (result == SendResult::Ok)
        arrived();
    if (generation != c.generation)
        return; // we moved on meanwhile: that channel has its own HELLO
    if (result != SendResult::Ok && c.helloAttempts <= m_limits.helloRetries && presenceOn()) {
        c.helloDue = now + m_limits.helloRetryMs; // flood protection: try again later
    } else {
        // The question is out (or can't be asked): whoever hasn't answered by then doesn't have it.
        for (auto m = c.members.begin(); m != c.members.end(); ++m) {
            if (m->state == PeerState::Checking && m->deadlineMs < 0)
                m->deadlineMs = now + m_limits.memberTimeoutMs;
        }
    }
    changed(sch);
    reschedule();
}

void PeerDirectory::sendHi(quint64 sch)
{
    auto it = m_conns.find(sch);
    if (it == m_conns.end())
        return;
    it->hiDue = -1;
    QVector<ClientId> ids;
    for (const ClientId id : qAsConst(it->hiTargets)) {
        if (m_env.channelOf(sch, id) != 0) // one id that left would fail the whole command
            ids.append(id);
    }
    it->hiTargets.clear();
    if (ids.isEmpty() || !presenceOn() || m_sender.blocked(sch))
        return;
    std::sort(ids.begin(), ids.end());
    const proto::Message hi = proto::makeHi(m_local.version, caps());
    for (int i = 0; i < ids.size(); i += m_limits.hiMaxTargets)
        m_sender.send(sch, hi, Target::toClients(ids.mid(i, m_limits.hiMaxTargets)), {});
}

void PeerDirectory::checkPartner(quint64 sch, ClientId client)
{
    if (!isAvailable(sch) || client == 0 || client == m_env.ownClientId(sch) || m_env.channelOf(sch, client) == 0)
        return;
    const QString uid = m_env.uid(sch, client);
    if (uid.isEmpty())
        return;
    const qint64 now = m_env.nowMs();
    Conn&        c   = m_conns[sch];
    PeerInfo&    p   = remember(c, uid, client, m_env.nickname(sch, client));
    if (p.state == PeerState::Has && p.heardMs >= 0 && now - p.heardMs < m_limits.partnerFreshMs)
        return;
    if (p.pingMs >= 0 && now - p.pingMs < m_limits.partnerPingMs)
        return;
    p.pingMs       = now;
    p.pingDeadline = now + m_limits.memberTimeoutMs;
    if (p.state != PeerState::Has)
        p.state = PeerState::Checking;
    QPointer<PeerDirectory> guard(this);
    m_sender.send(sch, proto::makeHello(m_local.version, caps(), true), Target::toClients({client}), [guard, sch](SendResult result) {
        if (guard && result == SendResult::Blocked)
            guard->commandsBlocked(sch);
    });
    changed(sch);
    reschedule();
}

// ---- queries --------------------------------------------------------------------------------------

PresenceSummary PeerDirectory::summary(quint64 sch, int targetMode, ClientId partner)
{
    PresenceSummary s;
    if (targetMode == kTargetServer) {
        s.kind = PresenceSummary::Kind::Server;
        return s;
    }
    if (!isAvailable(sch))
        return s;

    if (targetMode == kTargetClient) {
        if (partner == 0)
            return s;
        checkPartner(sch, partner);
        const QString uid = m_env.uid(sch, partner);
        if (uid.isEmpty())
            return s;
        s.kind    = PresenceSummary::Kind::Private;
        s.partner = m_env.nickname(sch, partner);
        const auto conn = m_conns.constFind(sch);
        const PeerInfo info = conn == m_conns.constEnd() ? PeerInfo() : conn->peers.value(uid);
        switch (info.state) {
        case PeerState::Has:
        case PeerState::Without:
            s.partnerState = info.state;
            break;
        case PeerState::Unknown:
        case PeerState::Checking:
            s.partnerState = PeerState::Checking;
            break;
        }
        return s;
    }
    if (targetMode != kTargetChannel)
        return s;

    const auto conn = m_conns.constFind(sch);
    if (conn == m_conns.constEnd() || conn->channel == 0)
        return s;
    s.kind = PresenceSummary::Kind::Channel;
    // One entry per person: someone with two clients has TS Media if either has it.
    QHash<QString, Member> people;
    for (const Member& m : conn->members) {
        auto it = people.find(m.uid);
        if (it == people.end()) {
            people.insert(m.uid, m);
        } else if (m.state == PeerState::Has || (m.state == PeerState::Checking && it->state == PeerState::Without)) {
            it->state = m.state;
        }
    }
    for (const Member& m : qAsConst(people)) {
        switch (m.state) {
        case PeerState::Has:
            s.has << m.name;
            break;
        case PeerState::Without:
            s.without << m.name;
            break;
        case PeerState::Unknown:
        case PeerState::Checking:
            s.checking << m.name;
            break;
        }
    }
    const auto byName = [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; };
    std::sort(s.has.begin(), s.has.end(), byName);
    std::sort(s.without.begin(), s.without.end(), byName);
    std::sort(s.checking.begin(), s.checking.end(), byName);
    return s;
}

PeerState PeerDirectory::stateOf(quint64 sch, const QString& uid) const
{
    const auto conn = m_conns.constFind(sch);
    return conn == m_conns.constEnd() ? PeerState::Unknown : conn->peers.value(uid).state;
}

bool PeerDirectory::hasCapability(quint64 sch, const QString& uid, const char* cap) const
{
    const auto conn = m_conns.constFind(sch);
    if (conn == m_conns.constEnd())
        return false;
    const auto p = conn->peers.constFind(uid);
    return p != conn->peers.constEnd() && p->state == PeerState::Has && p->caps.contains(QString::fromLatin1(cap));
}

bool PeerDirectory::channelHasCapability(quint64 sch, const char* cap) const
{
    const auto conn = m_conns.constFind(sch);
    if (conn == m_conns.constEnd())
        return false;
    for (const Member& m : conn->members) {
        if (m.state == PeerState::Has && hasCapability(sch, m.uid, cap))
            return true;
    }
    return false;
}

bool PeerDirectory::isAvailable(quint64 sch) const
{
    return presenceOn() && !m_sender.blocked(sch);
}

quint64 PeerDirectory::channelOf(quint64 sch) const
{
    return m_conns.value(sch).channel;
}

QVector<quint64> PeerDirectory::connections() const
{
    QVector<quint64> list;
    for (auto it = m_conns.constBegin(); it != m_conns.constEnd(); ++it)
        list.append(it.key());
    return list;
}

QHash<QString, int> PeerDirectory::versions(quint64 sch) const
{
    QHash<QString, int> histogram;
    const auto          conn = m_conns.constFind(sch);
    if (conn == m_conns.constEnd())
        return histogram;
    for (const PeerInfo& p : conn->peers) {
        if (p.state == PeerState::Has)
            ++histogram[p.version.isEmpty() ? QString::fromLatin1("?") : p.version]; // may reach the clipboard (diagnostics)
    }
    return histogram;
}

void PeerDirectory::sayGoodbye()
{
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
        if (!it->announced)
            continue;
        it->announced = false;
        if (!m_sender.blocked(it.key()))
            m_sender.send(it.key(), proto::makeBye(), Target::toChannel(), {});
    }
}

// ---- time -----------------------------------------------------------------------------------------

void PeerDirectory::tick()
{
    if (m_ticking)
        return;
    m_ticking        = true;
    const qint64 now = m_env.nowMs();
    for (const quint64 sch : m_conns.keys()) {
        if (!m_conns.contains(sch))
            continue;
        if (m_conns[sch].helloDue >= 0 && now >= m_conns[sch].helloDue)
            sendHello(sch);
        if (m_conns.contains(sch) && m_conns[sch].hiDue >= 0 && now >= m_conns[sch].hiDue)
            sendHi(sch);
        if (!m_conns.contains(sch))
            continue;

        Conn& c   = m_conns[sch];
        bool  any = false;
        for (auto m = c.members.begin(); m != c.members.end(); ++m) {
            if (m->state == PeerState::Checking && m->deadlineMs >= 0 && now >= m->deadlineMs) {
                m->state = PeerState::Without;
                any      = true;
                PeerInfo& p = c.peers[m->uid];
                if (p.state != PeerState::Has)
                    p.state = PeerState::Without;
            }
        }
        for (auto p = c.peers.begin(); p != c.peers.end(); ++p) {
            if (p->pingDeadline >= 0 && now >= p->pingDeadline) {
                p->pingDeadline = -1;
                if (p->state != PeerState::Has)
                    p->state = PeerState::Without;
                any = true;
            }
        }
        bool sync = false;
        if (c.syncAt >= 0 && now >= c.syncAt) {
            if (!m_local.reactions || !presenceOn() || m_sender.blocked(sch) || now >= c.syncUntil) {
                c.syncAt = -1;
            } else if (channelHasCapability(sch, "r")) {
                c.syncAt = -1;
                sync     = true;
            } else {
                c.syncAt = now + 500; // answers may still be on their way
            }
        }
        if (any)
            changed(sch);
        if (sync)
            emit syncDue(sch);
    }
    m_ticking = false;
    reschedule();
}

qint64 PeerDirectory::nextWakeMs() const
{
    qint64     wake = -1;
    const auto take = [&wake](qint64 at) {
        if (at >= 0)
            wake = wake < 0 ? at : qMin(wake, at);
    };
    for (const Conn& c : m_conns) {
        take(c.helloDue);
        take(c.hiDue);
        take(c.syncAt);
        for (const Member& m : c.members) {
            if (m.state == PeerState::Checking)
                take(m.deadlineMs);
        }
        for (const PeerInfo& p : c.peers)
            take(p.pingDeadline);
    }
    return wake;
}

void PeerDirectory::reschedule()
{
    if (!m_autoTimer || m_ticking)
        return;
    const qint64 wake = nextWakeMs();
    if (wake < 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(static_cast<int>(qBound<qint64>(0, wake - m_env.nowMs(), 60 * 60 * 1000)));
}

void PeerDirectory::changed(quint64 sch)
{
    emit presenceChanged(sch);
}

} // namespace peers
