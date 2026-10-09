#include "fileverify.h"

#include <QMetaObject>
#include <QTimer>

#include "i18n.h"

namespace fileverify {

// ---- sender -------------------------------------------------------------------------------------

StagedDigest finalizeStaged(const QString& stagedPath, const QByteArray& previewJpeg, const std::atomic<bool>* cancel, const HashFile& hash)
{
    StagedDigest digest;
    QString      error;
    digest.sha256 = hash ? hash(stagedPath, cancel, {}, &error) : hashing::sha256File(stagedPath, cancel, {}, &error);
    if (digest.sha256.size() != kShaBytes) {
        digest.sha256.clear();
        digest.error = error.isEmpty() ? QString::fromLatin1("no digest") : error;
    }
    if (!previewJpeg.isEmpty())
        digest.previewSha = previewDigest(previewJpeg);
    return digest;
}

// ---- preview ------------------------------------------------------------------------------------

QByteArray previewDigest(const QByteArray& previewBytes)
{
    return hashing::sha256(previewBytes).left(kPreviewShaBytes);
}

bool previewMatches(const QByteArray& previewBytes, const QByteArray& previewSha)
{
    return previewSha.size() == kPreviewShaBytes && previewDigest(previewBytes) == previewSha;
}

// ---- KnownDigests -------------------------------------------------------------------------------

namespace {

QString knownKey(const QString& remoteId, quint64 size)
{
    return remoteId + QLatin1Char('\n') + QString::number(size);
}

} // namespace

void KnownDigests::note(const QString& remoteId, quint64 size, const QByteArray& digest)
{
    if (remoteId.isEmpty() || size == 0 || digest.size() != kShaBytes)
        return;
    const QString key = knownKey(remoteId, size);
    auto          it  = m_digests.find(key);
    if (it != m_digests.end())
        m_byAge.remove(it->stamp);
    else
        it = m_digests.insert(key, {});
    it->digest = digest;
    it->stamp  = ++m_stamp;
    m_byAge.insert(it->stamp, key);
    while (m_digests.size() > kMaxEntries && !m_byAge.isEmpty()) {
        const QString oldest = m_byAge.take(m_byAge.firstKey());
        m_digests.remove(oldest);
    }
}

KnownDigests::Verdict KnownDigests::check(const QString& remoteId, quint64 size, const QByteArray& claimed) const
{
    if (claimed.size() != kShaBytes)
        return Verdict::Unknown;
    const QByteArray digest = known(remoteId, size);
    if (digest.isEmpty())
        return Verdict::Unknown;
    return digest == claimed ? Verdict::Agrees : Verdict::Contradicts;
}

QByteArray KnownDigests::known(const QString& remoteId, quint64 size) const
{
    if (size == 0)
        return {};
    return m_digests.value(knownKey(remoteId, size)).digest;
}

void KnownDigests::clear()
{
    m_digests.clear();
    m_byAge.clear();
}

// ---- Verifier -----------------------------------------------------------------------------------

Verifier::Verifier(Environment environment, QObject* parent)
    : QObject(parent)
    , m_env(std::move(environment))
{
    m_pool.setMaxThreadCount(qMax(1, m_env.threads));
    m_clock.start();
    // A member timer (a child), never a functor singleShot: nothing of ours may be pending in Qt once
    // the plugin is unloaded.
    m_poll = new QTimer(this);
    m_poll->setInterval(qMax(1, m_env.writePollMs));
    connect(m_poll, &QTimer::timeout, this, &Verifier::pollWaiting);
}

Verifier::~Verifier()
{
    shutdown();
}

void Verifier::start(const QString& key, const QString& path, const QByteArray& expected)
{
    if (m_closing)
        return;
    cancel(key);
    Job job;
    job.id       = ++m_nextId;
    job.path     = path;
    job.expected = expected;
    job.cancel   = std::make_shared<std::atomic<bool>>(false);
    job.started.start();
    if (expected.size() != kShaBytes) {
        // Nothing to compare with: never a match. Reported like any result, from the event loop.
        job.pass = 2;
        m_jobs.insert(key, job);
        const quint64 id = job.id;
        QMetaObject::invokeMethod(this, [this, key, id] { onPassDone(key, id, 2, {}, QString::fromLatin1("no expected digest")); }, Qt::QueuedConnection);
        return;
    }
    m_jobs.insert(key, job);
    runPass(key);
}

void Verifier::cancel(const QString& key)
{
    auto it = m_jobs.find(key);
    if (it == m_jobs.end())
        return;
    it->cancel->store(true);
    m_jobs.erase(it);
}

void Verifier::cancelAll()
{
    for (const Job& job : qAsConst(m_jobs))
        job.cancel->store(true);
    m_jobs.clear();
    if (m_poll)
        m_poll->stop();
}

void Verifier::shutdown()
{
    m_closing = true;
    cancelAll();
    m_pool.clear();
    m_pool.waitForDone();
}

bool Verifier::isChecking(const QString& key) const
{
    return m_jobs.contains(key);
}

bool Verifier::isSecondPass(const QString& key) const
{
    const auto it = m_jobs.constFind(key);
    return it != m_jobs.constEnd() && (it->pass >= 2 || it->waiting);
}

double Verifier::progress(const QString& key) const
{
    const auto it = m_jobs.constFind(key);
    return it == m_jobs.constEnd() ? -1.0 : it->progress;
}

void Verifier::runPass(const QString& key)
{
    auto it = m_jobs.find(key);
    if (it == m_jobs.end() || m_closing)
        return;
    Job& job = it.value();
    ++job.pass;
    job.waiting  = false;
    job.dueAt    = -1;
    job.progress = -1.0;

    const quint64  id     = job.id;
    const int      pass   = job.pass;
    const QString  path   = job.path;
    const auto     cancel = job.cancel;
    const HashFile hash   = m_env.hashFile ? m_env.hashFile : HashFile(&hashing::sha256File);
    // shutdown() waits for the pool before this object goes, so it is alive while a pass runs; the
    // queued calls are dropped if it is gone by the time they would run.
    Verifier* self = this;
    m_pool.start([self, key, id, pass, path, cancel, hash] {
        const hashing::Progress progress = [self, key, id, pass, cancel](qint64 done, qint64 total) {
            if (cancel->load() || total <= 0)
                return;
            const double fraction = qBound(0.0, static_cast<double>(done) / static_cast<double>(total), 1.0);
            QMetaObject::invokeMethod(self, [self, key, id, pass, fraction] { self->onPassProgress(key, id, pass, fraction); }, Qt::QueuedConnection);
        };
        QString          error;
        const QByteArray digest = hash(path, cancel.get(), progress, &error);
        if (cancel->load())
            return; // dropped: nobody waits for it
        QMetaObject::invokeMethod(self, [self, key, id, pass, digest, error] { self->onPassDone(key, id, pass, digest, error); }, Qt::QueuedConnection);
    });
}

void Verifier::onPassProgress(const QString& key, quint64 id, int pass, double fraction)
{
    auto it = m_jobs.find(key);
    if (it == m_jobs.end() || it->id != id || it->pass != pass || it->waiting)
        return;
    if (fraction <= it->progress)
        return;
    it->progress = fraction;
    emit progressChanged(key, fraction, pass >= 2);
}

void Verifier::onPassDone(const QString& key, quint64 id, int pass, const QByteArray& digest, const QString& error)
{
    auto it = m_jobs.find(key);
    if (it == m_jobs.end() || it->id != id || it->pass != pass || it->waiting)
        return; // canceled or replaced meanwhile
    Job& job = it.value();
    if (digest.size() == kShaBytes && digest == job.expected) {
        finish(key, Outcome::Match, digest, {});
        return;
    }
    if (pass == 1) {
        // Maybe still being written (2.0.5): once more, after the writer is gone and a moment later.
        job.firstReceived = digest.size() == kShaBytes ? digest : QByteArray();
        job.waiting       = true;
        job.waitFrom      = m_clock.elapsed();
        job.dueAt         = -1;
        job.progress      = -1.0;
        emit progressChanged(key, -1.0, true);
        if (!m_poll->isActive())
            m_poll->start();
        pollWaiting(); // no writer: the delay starts now, not one poll later
        return;
    }
    if (digest.size() == kShaBytes)
        finish(key, Outcome::Mismatch, digest, {});
    else
        finish(key, Outcome::ReadError, {}, error.isEmpty() ? QString::fromLatin1("no digest") : error);
}

void Verifier::pollWaiting()
{
    const qint64 now = m_clock.elapsed();
    QStringList  due;
    bool         waiting = false;
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        Job& job = it.value();
        if (!job.waiting)
            continue;
        // Someone writes the file (again): the pause starts over once they are done.
        const bool writing = now - job.waitFrom < m_env.maxWriteWaitMs && m_env.isBeingWritten && m_env.isBeingWritten(job.path);
        if (writing) {
            job.dueAt = -1;
            waiting   = true;
            continue;
        }
        if (job.dueAt < 0)
            job.dueAt = now + qMax(0, m_env.recheckDelayMs);
        if (now >= job.dueAt)
            due.append(it.key());
        else
            waiting = true;
    }
    if (!waiting)
        m_poll->stop();
    for (const QString& key : qAsConst(due))
        runPass(key);
}

void Verifier::finish(const QString& key, Outcome outcome, const QByteArray& received, const QString& error)
{
    const Job job = m_jobs.take(key);
    Result    result;
    result.key       = key;
    result.outcome   = outcome;
    result.expected  = job.expected;
    result.received  = received;
    result.rechecked = job.pass >= 2;
    if (result.rechecked)
        result.firstReceived = job.firstReceived;
    result.error     = error;
    result.elapsedMs = job.started.isValid() ? job.started.elapsed() : 0;

    switch (outcome) {
    case Outcome::Match:
        count(Counter::Verified);
        if (result.rechecked)
            count(Counter::RecoveredOnRecheck);
        break;
    case Outcome::Mismatch:
        count(Counter::Mismatched);
        break;
    case Outcome::ReadError:
        count(Counter::ReadErrors);
        break;
    }
    emit finished(result);
}

// ---- texts --------------------------------------------------------------------------------------

QString mismatchDetails(const QByteArray& expected, const QByteArray& received, const QString& remoteFile)
{
    const QString none = i18n::t("(unknown)");
    return i18n::t("Expected SHA-256: %1\nReceived SHA-256: %2\nFile: %3")
        .arg(expected.isEmpty() ? none : hashing::toHex(expected), received.isEmpty() ? none : hashing::toHex(received), remoteFile);
}

// ---- diagnostics --------------------------------------------------------------------------------

namespace {

// Plain atomics: nothing here needs destroying when the DLL is unloaded.
std::atomic<int> g_counts[static_cast<int>(Counter::Count)];

} // namespace

void count(Counter counter)
{
    g_counts[static_cast<int>(counter)].fetch_add(1, std::memory_order_relaxed);
}

int counted(Counter counter)
{
    return g_counts[static_cast<int>(counter)].load(std::memory_order_relaxed);
}

void resetCounters()
{
    for (auto& value : g_counts)
        value.store(0, std::memory_order_relaxed);
}

QString hashBackend()
{
    // TODO(2.2 integration): Windows CNG through src/crypto (the updater's module) once it is merged;
    // about 10x faster. hashing::sha256File is the one place to switch.
    return QString::fromLatin1("Qt (QCryptographicHash)");
}

QString diagnosticsTitle()
{
    return i18n::t("File checks (SHA-256)");
}

QStringList diagnosticLines()
{
    QStringList lines;
    lines << i18n::t("Downloads checked: %1 matched (%2 on the second pass), %3 didn't match, %4 couldn't be read")
                 .arg(counted(Counter::Verified))
                 .arg(counted(Counter::RecoveredOnRecheck))
                 .arg(counted(Counter::Mismatched))
                 .arg(counted(Counter::ReadErrors));
    lines << i18n::t("Links refused for a made-up checksum: %1; previews that didn't match: %2")
                 .arg(counted(Counter::ForgedBlocked))
                 .arg(counted(Counter::PreviewMismatched));
    lines << i18n::t("Files sent: %1 with a checksum, %2 without").arg(counted(Counter::SentWithSha)).arg(counted(Counter::SentWithoutSha));
    lines << i18n::t("Hashing: %1").arg(hashBackend());
    return lines;
}

} // namespace fileverify
