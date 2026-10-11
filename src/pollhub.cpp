#include "pollhub.h"

#include <QPointer>
#include <QTimer>

#include <algorithm>

#include "i18n.h"

namespace {

QString askedKey(quint64 sch, const QString& key)
{
    return QString::number(sch) + QLatin1Char('/') + key;
}

} // namespace

PollHub::PollHub(Env& env, const QString& storePath, QObject* parent)
    : PollHub(env, storePath, Limits(), parent)
{
}

PollHub::PollHub(Env& env, const QString& storePath, const Limits& limits, QObject* parent)
    : QObject(parent)
    , m_env(env)
    , m_limits(limits)
    , m_path(storePath)
{
    if (!m_path.isEmpty())
        m_store.load(m_path);
    // Member timers only (children): nothing of ours can be pending once the hub is gone.
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &PollHub::tick);
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    m_coalesce->setInterval(50);
    connect(m_coalesce, &QTimer::timeout, this, [this] {
        const QSet<QString> keys = m_changed;
        m_changed.clear();
        for (const QString& key : keys)
            emit changed(key);
    });
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, &PollHub::save);
}

PollHub::~PollHub()
{
    m_closing = true;
    if (m_dirty)
        save();
}

void PollHub::prepareShutdown()
{
    m_closing = true;
    m_answers.clear();
    m_syncWanted.clear();
    m_syncDue.clear();
    m_timer->stop();
    save();
}

void PollHub::save()
{
    m_saveTimer->stop();
    if (m_path.isEmpty())
        return;
    if (m_store.save(m_path))
        m_dirty = false;
}

void PollHub::scheduleSave()
{
    m_dirty = true;
    if (!m_saveTimer->isActive())
        m_saveTimer->start(m_limits.saveDelayMs);
}

void PollHub::noteChanged(const QString& key)
{
    for (auto it = m_tallies.begin(); it != m_tallies.end();) {
        if (it.key().startsWith(key))
            it = m_tallies.erase(it);
        else
            ++it;
    }
    m_changed.insert(key);
    if (!m_coalesce->isActive())
        m_coalesce->start();
}

void PollHub::setAutoTimer(bool on)
{
    m_autoTimer = on;
    if (!on)
        m_timer->stop();
    else
        reschedule();
}

// ---- your vote --------------------------------------------------------------------------------------------

PollHub::VoteError PollHub::canVote(quint64 sch, const polls::Poll& poll) const
{
    if (!poll.isValid())
        return VoteError::Invalid;
    if (!m_env.isConnected(sch))
        return VoteError::NotConnected;
    if (m_env.ownChannel(sch) != poll.channelId)
        return VoteError::WrongChannel;
    if (m_env.blocked(sch))
        return VoteError::Blocked;
    const std::optional<polls::Store::Own> own = m_store.own(poll.key);
    if (own && own->step >= polls::kChainLength)
        return VoteError::Locked;
    return VoteError::None;
}

PollHub::VoteError PollHub::vote(quint64 sch, const polls::Poll& poll, quint16 mask)
{
    const VoteError error = canVote(sch, poll);
    if (error != VoteError::None)
        return error;
    if (mask != 0 && !polls::validMask(mask, poll.options.size(), poll.multiple))
        return VoteError::Invalid;
    watch(poll);
    const std::optional<polls::Ballot> ballot = m_store.castOwn(poll.key, mask, m_env.wallMs());
    if (!ballot)
        return VoteError::Locked;
    Own& own      = m_own[poll.key];
    own.sch       = sch;
    own.channelId = poll.channelId;
    own.attempts  = 0;
    own.retryAt   = -1;
    own.sending   = true;
    own.failed    = false;
    // At once: a step the others have must never be used again (after a crash, a lost polls.json would
    // reuse it, and that vote would be ignored everywhere).
    m_dirty = true;
    save();
    noteChanged(poll.key);
    sendOwn(poll.key, *ballot);
    reschedule();
    return VoteError::None;
}

void PollHub::retry(quint64 sch, const polls::Poll& poll)
{
    const std::optional<polls::Store::Own> own = m_store.own(poll.key);
    if (!own)
        return;
    vote(sch, poll, own->mask);
}

void PollHub::sendOwn(const QString& key, const polls::Ballot& ballot)
{
    const auto it = m_own.constFind(key);
    if (it == m_own.constEnd() || m_closing)
        return;
    const QVector<proto::Message> messages = polls::makeVotes(key, {ballot}, false);
    if (messages.isEmpty())
        return;
    const QPointer<PollHub> self(this);
    const int               step = ballot.step;
    const quint16           mask = ballot.mask;
    m_env.send(it->sch, messages.first(), false, QString::fromLatin1("pv:") + key, [self, key, step, mask](peers::SendResult result) {
        if (self)
            self->ownDone(key, step, mask, result);
    });
}

void PollHub::ownDone(const QString& key, int step, quint16 mask, peers::SendResult result)
{
    if (m_closing)
        return;
    const std::optional<polls::Store::Own> stored = m_store.own(key);
    const bool                             latest = stored && stored->step == step;
    switch (result) {
    case peers::SendResult::Ok:
    case peers::SendResult::LateOk:
        m_store.confirmOwn(key, step, mask);
        scheduleSave();
        if (latest) {
            m_own.remove(key);
            noteChanged(key);
        }
        return;
    case peers::SendResult::Superseded:
        return; // a newer state of the same ballot replaced it
    case peers::SendResult::Flooded:
    case peers::SendResult::Failed:
    case peers::SendResult::Blocked:
        break;
    }
    auto it = m_own.find(key);
    if (!latest || it == m_own.end())
        return;
    const int retries = static_cast<int>(sizeof(m_limits.retryMs) / sizeof(m_limits.retryMs[0]));
    if (result != peers::SendResult::Blocked && it->attempts < retries) {
        it->retryAt = m_env.nowMs() + m_limits.retryMs[it->attempts];
        ++it->attempts;
    } else {
        it->sending = false;
        it->failed  = true;
        it->retryAt = -1;
        noteChanged(key);
    }
    reschedule();
}

QString PollHub::errorText(VoteError error)
{
    switch (error) {
    case VoteError::None:
        return {};
    case VoteError::NotConnected:
        return i18n::t("You're not connected to this server.");
    case VoteError::WrongChannel:
        return i18n::t("Join the channel this poll was posted in to vote.");
    case VoteError::Blocked:
        return i18n::t("This server doesn't pass on TS Media's messages, so votes can't be sent here.");
    case VoteError::Locked:
        return i18n::t("Your vote can't be changed any more.");
    case VoteError::Invalid:
        return i18n::t("This poll can't be voted on.");
    }
    return {};
}

PollHub::View PollHub::view(const polls::Poll& poll) const
{
    View          v;
    const int     count = poll.options.size();
    const QString cache = poll.key + QLatin1Char('|') + QString::number(count) + QLatin1Char(poll.multiple ? 'm' : 's');
    auto          known = m_tallies.constFind(cache);
    if (known == m_tallies.constEnd()) {
        if (m_tallies.size() > 256)
            m_tallies.clear();
        known = m_tallies.insert(cache, m_store.tally(poll.key, count, poll.multiple)); // the chat draws often: counted once per change
    }
    v.tally = known.value();
    v.percents      = polls::percentages(v.tally, poll.multiple);
    if (const std::optional<polls::Store::Own> own = m_store.own(poll.key)) {
        v.own    = polls::validMask(own->mask, count, poll.multiple) ? own->mask : 0;
        v.locked = own->step >= polls::kChainLength;
    }
    const auto it = m_own.constFind(poll.key);
    if (it != m_own.constEnd())
        v.state = it->failed ? SendState::Failed : SendState::Sending;
    return v;
}

// ---- the polls the chats show ---------------------------------------------------------------------------------

void PollHub::watch(const polls::Poll& poll)
{
    if (!poll.isValid())
        return;
    Watched& w = m_watched[poll.key];
    w.seenMs   = m_env.nowMs();
    if (w.channels.size() < 8)
        w.channels.insert(poll.channelId);
    while (m_watched.size() > qMax(1, m_limits.maxWatched)) {
        auto oldest = m_watched.end();
        for (auto it = m_watched.begin(); it != m_watched.end(); ++it) {
            if (it.key() != poll.key && (oldest == m_watched.end() || it->seenMs < oldest->seenMs))
                oldest = it;
        }
        if (oldest == m_watched.end())
            break;
        m_watched.erase(oldest);
    }
}

bool PollHub::watching(const QString& key, quint64 channel) const
{
    const auto it = m_watched.constFind(key);
    return it != m_watched.constEnd() && channel != 0 && it->channels.contains(channel);
}

// ---- late joiners -------------------------------------------------------------------------------------------

void PollHub::requestSync(quint64 sch, const QStringList& keys)
{
    if (m_closing || !m_env.isConnected(sch))
        return;
    const qint64 now   = m_env.nowMs();
    bool         added = false;
    for (const QString& key : keys) {
        if (!polls::isKey(key))
            continue;
        const auto asked = m_askedMs.constFind(askedKey(sch, key));
        if (asked != m_askedMs.constEnd() && now - asked.value() < m_limits.syncEveryMs)
            continue;
        QSet<QString>& wanted = m_syncWanted[sch];
        if (wanted.size() >= 4 * polls::kMaxQueried || wanted.contains(key))
            continue;
        wanted.insert(key);
        added = true;
    }
    if (added && !m_syncDue.contains(sch)) {
        m_syncDue.insert(sch, now + m_limits.syncDelayMs);
        reschedule();
    }
}

void PollHub::sendQueries(quint64 sch)
{
    const QSet<QString> wanted = m_syncWanted.take(sch);
    if (wanted.isEmpty() || !m_env.isConnected(sch) || m_env.blocked(sch) || m_env.quiet(sch) || !m_env.othersHaveTsMedia(sch))
        return; // nobody to ask, or presence off (asked again when the poll is seen again)
    const qint64  now     = m_env.nowMs();
    const quint64 channel = m_env.ownChannel(sch);
    QStringList   keys;
    for (const QString& key : wanted) {
        if (watching(key, channel))
            keys << key; // still the channel we are in (it may have changed since the poll was seen)
    }
    std::sort(keys.begin(), keys.end());
    QVector<polls::Digest> digests;
    for (const QString& key : qAsConst(keys)) {
        m_askedMs.insert(askedKey(sch, key), now);
        digests.append(m_store.digest(key));
    }
    for (int from = 0; from < digests.size(); from += polls::kMaxQueried) {
        const proto::Message query = polls::makeQuery(digests.mid(from, polls::kMaxQueried));
        m_env.send(sch, query, true, QString(), [](peers::SendResult) {});
    }
}

int PollHub::answerRoom(quint64 sch)
{
    const qint64 now    = m_env.nowMs();
    const int    budget = qMax(1, m_limits.answerBudget);
    Budget&      b      = m_budgets[sch];
    if (!b.known) {
        b.known  = true;
        b.tokens = budget;
    } else {
        const double perMs = static_cast<double>(budget) / qMax(1, m_limits.answerBudgetMs);
        b.tokens           = qMin<double>(budget, b.tokens + perMs * qMax<qint64>(0, now - b.atMs));
    }
    b.atMs = now;
    return static_cast<int>(b.tokens);
}

void PollHub::sendAnswer(const Answer& answer)
{
    if (!m_env.isConnected(answer.sch) || m_env.blocked(answer.sch) || m_env.quiet(answer.sch))
        return;
    // The answer goes to the channel we are in now: only for a poll of that channel (we may have moved on
    // since the question came).
    if (!watching(answer.key, m_env.ownChannel(answer.sch)))
        return;
    // The connection's budget for answers, whoever asks and for whichever polls: a question costs its
    // asker one plugin command, and must never make everyone here send dozens.
    const int room = qMin(answerRoom(answer.sch), (m_limits.maxAnswerBallots + polls::kMaxEntries - 1) / polls::kMaxEntries);
    if (room <= 0)
        return; // asked again later (every 2 minutes per poll)
    QVector<polls::Ballot> ballots;
    for (const QByteArray& id : answer.remaining) {
        const polls::Ballot b = m_store.ballot(answer.key, id);
        if (b.step > 0)
            ballots.append(b);
    }
    if (ballots.isEmpty())
        return;
    // Another order each time: several members answering at once tend to cover different ballots.
    for (int i = ballots.size() - 1; i > 0; --i)
        std::swap(ballots[i], ballots[m_env.randomBetween(0, i)]);
    ballots.resize(qMin(ballots.size(), qMin(m_limits.maxAnswerBallots, room * polls::kMaxEntries)));
    m_answeredMs.insert(answer.key, m_env.nowMs());
    const QVector<proto::Message> messages = polls::makeVotes(answer.key, ballots, true);
    m_budgets[answer.sch].tokens -= messages.size();
    for (const proto::Message& m : messages)
        m_env.send(answer.sch, m, true, QString(), [](peers::SendResult) {});
}

void PollHub::received(quint64 sch, quint16 from, const QString& uid, const QString& name, const proto::Message& message)
{
    Q_UNUSED(uid)
    Q_UNUSED(name)
    if (m_closing || (message.type != "PV" && message.type != "PQ"))
        return;
    const quint64 channel = m_env.ownChannel(sch);
    if (channel == 0 || m_env.channelOf(sch, from) != channel || m_env.isQueryClient(sch, from))
        return; // only from people in our channel right now
    const qint64 now = m_env.nowMs();

    if (message.type == "PV") {
        const std::optional<polls::Votes> votes = polls::readVotes(message);
        // Only polls a chat of ours shows, posted in this channel: anything else would only fill polls.json
        // (and push out the polls you voted on).
        if (!votes || !watching(votes->key, channel))
            return;
        bool changed = false;
        for (const polls::Ballot& b : votes->ballots) {
            const polls::Store::Merge merged = m_store.merge(votes->key, b, m_env.wallMs());
            changed                          = changed || merged != polls::Store::Merge::Ignored;
            // Overheard: whoever was about to answer with this ballot (or an older state of it) needn't.
            const polls::Ballot current = m_store.ballot(votes->key, b.id);
            for (Answer& a : m_answers) {
                if (a.key == votes->key && a.sch == sch && current.step > 0 && b.step >= current.step)
                    a.remaining.remove(b.id);
            }
        }
        m_answers.erase(std::remove_if(m_answers.begin(), m_answers.end(), [](const Answer& a) { return a.remaining.isEmpty(); }), m_answers.end());
        if (changed) {
            scheduleSave();
            noteChanged(votes->key);
        }
        return;
    }

    if (m_env.quiet(sch))
        return; // presence off: no answers (they would tell the channel we have TS Media)
    const std::optional<QVector<polls::Digest>> query = polls::readQuery(message);
    if (!query)
        return;
    for (const polls::Digest& theirs : *query) {
        if (!m_store.has(theirs.key) || !watching(theirs.key, channel))
            continue;
        const polls::Digest ours = m_store.digest(theirs.key);
        if (ours.count == 0 || ours == theirs)
            continue;
        const auto answered = m_answeredMs.constFind(theirs.key);
        if (answered != m_answeredMs.constEnd() && now - answered.value() < m_limits.answerEveryMs)
            continue;
        bool pending = false;
        for (const Answer& a : qAsConst(m_answers))
            pending = pending || a.key == theirs.key;
        if (pending || m_answers.size() >= 16)
            continue;
        Answer a;
        a.sch   = sch;
        a.key   = theirs.key;
        a.dueMs = now + m_env.randomBetween(m_limits.answerMinMs, m_limits.answerMaxMs);
        for (const polls::Ballot& b : m_store.ballots(theirs.key))
            a.remaining.insert(b.id);
        m_answers.append(a);
    }
    reschedule();
}

void PollHub::connectionLost(quint64 sch)
{
    m_syncWanted.remove(sch);
    m_syncDue.remove(sch);
    m_budgets.remove(sch);
    m_answers.erase(std::remove_if(m_answers.begin(), m_answers.end(), [sch](const Answer& a) { return a.sch == sch; }), m_answers.end());
    for (auto it = m_askedMs.begin(); it != m_askedMs.end();) {
        if (it.key().startsWith(QString::number(sch) + QLatin1Char('/')))
            it = m_askedMs.erase(it);
        else
            ++it;
    }
    for (auto it = m_own.begin(); it != m_own.end(); ++it) {
        if (it->sch == sch && (it->sending || it->retryAt >= 0)) {
            it->sending = false;
            it->failed  = true;
            it->retryAt = -1;
            noteChanged(it.key());
        }
    }
    reschedule();
}

// ---- time -----------------------------------------------------------------------------------------------------

qint64 PollHub::nextWakeMs() const
{
    qint64 next = -1;
    auto   take = [&next](qint64 at) {
        if (at >= 0 && (next < 0 || at < next))
            next = at;
    };
    for (const Own& own : m_own)
        take(own.retryAt);
    for (const qint64 due : m_syncDue)
        take(due);
    for (const Answer& a : m_answers)
        take(a.dueMs);
    return next;
}

void PollHub::reschedule()
{
    if (!m_autoTimer || m_closing)
        return;
    const qint64 next = nextWakeMs();
    if (next < 0) {
        m_timer->stop();
        return;
    }
    m_timer->start(static_cast<int>(qBound<qint64>(0, next - m_env.nowMs(), 600000)));
}

void PollHub::tick()
{
    if (m_closing)
        return;
    const qint64 now = m_env.nowMs();
    // Retries of your vote: each one a new state of the ballot (a step is never reused).
    for (auto it = m_own.begin(); it != m_own.end(); ++it) {
        if (it->retryAt < 0 || it->retryAt > now)
            continue;
        it->retryAt = -1;
        const std::optional<polls::Store::Own> stored = m_store.own(it.key());
        if (!stored || !m_env.isConnected(it->sch) || m_env.ownChannel(it->sch) != it->channelId || stored->step >= polls::kChainLength) {
            it->sending = false;
            it->failed  = true;
            noteChanged(it.key());
            continue;
        }
        const std::optional<polls::Ballot> ballot = m_store.castOwn(it.key(), stored->mask, m_env.wallMs());
        if (ballot) {
            m_dirty = true;
            save(); // the new step, at once (see vote())
            sendOwn(it.key(), *ballot);
        }
    }
    for (const quint64 sch : m_syncDue.keys()) {
        if (m_syncDue.value(sch) <= now) {
            m_syncDue.remove(sch);
            sendQueries(sch);
        }
    }
    QVector<Answer> due;
    for (int i = m_answers.size() - 1; i >= 0; --i) {
        if (m_answers.at(i).dueMs <= now) {
            due.append(m_answers.at(i));
            m_answers.removeAt(i);
        }
    }
    for (const Answer& a : qAsConst(due))
        sendAnswer(a);
    reschedule();
}
