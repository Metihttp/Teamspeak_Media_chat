#include "router.h"

#include <windows.h>

#include <atomic>
#include <vector>

namespace proxy {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;

// Heap state, never destroyed by a static destructor: a pinned thread may still read it while the process exits.
struct State {
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<const Snapshot>();
    std::shared_ptr<RecordSource>   records;
    std::vector<HANDLE>             workers;
};

State& state()
{
    static State* s = new State;
    return *s;
}

std::atomic<qint64> g_lastProxyRouteChangeMs{0};

class EmptyRecords : public RecordSource
{
  public:
    bool            outcome(char, quint64, Record*) const override { return false; }
    QVector<Record> recent(char, quint64) const override { return {}; }
};

void pruneWorkersLocked(std::vector<HANDLE>& workers)
{
    for (size_t i = workers.size(); i-- > 0;) {
        if (WaitForSingleObject(workers[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(workers[i]);
            workers.erase(workers.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

} // namespace

std::shared_ptr<RecordSource> emptyRecords()
{
    return std::make_shared<EmptyRecords>();
}

std::shared_ptr<const Snapshot> snapshot()
{
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<const Snapshot> s = state().snapshot;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

void publish(std::shared_ptr<const Snapshot> next)
{
    if (!next)
        next = std::make_shared<const Snapshot>();
    std::shared_ptr<const Snapshot> old;
    AcquireSRWLockExclusive(&g_lock);
    old              = std::move(state().snapshot);
    state().snapshot = std::move(next);
    ReleaseSRWLockExclusive(&g_lock);
    // old goes here, outside the lock
}

std::shared_ptr<RecordSource> liveRecords()
{
    AcquireSRWLockShared(&g_lock);
    std::shared_ptr<RecordSource> r = state().records;
    ReleaseSRWLockShared(&g_lock);
    return r ? r : emptyRecords();
}

void setLiveRecords(std::shared_ptr<RecordSource> records)
{
    std::shared_ptr<RecordSource> old;
    AcquireSRWLockExclusive(&g_lock);
    old             = std::move(state().records);
    state().records = std::move(records);
    ReleaseSRWLockExclusive(&g_lock);
}

qint64 lastProxyRouteChangeMs()
{
    return g_lastProxyRouteChangeMs.load();
}

void markProxyRouteChange(qint64 wallMs)
{
    g_lastProxyRouteChangeMs.store(wallMs);
}

void init()
{
    publish(std::make_shared<const Snapshot>());
    setLiveRecords(nullptr);
    g_lastProxyRouteChangeMs.store(0);
}

void registerWorker(void* thread)
{
    if (!thread)
        return;
    AcquireSRWLockExclusive(&g_lock);
    pruneWorkersLocked(state().workers);
    state().workers.push_back(static_cast<HANDLE>(thread));
    ReleaseSRWLockExclusive(&g_lock);
}

void joinWorkers(int waitMs)
{
    std::vector<HANDLE> workers;
    AcquireSRWLockExclusive(&g_lock);
    workers.swap(state().workers);
    ReleaseSRWLockExclusive(&g_lock);
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(qMax(0, waitMs));
    for (HANDLE h : workers) {
        const ULONGLONG now  = GetTickCount64();
        const DWORD     left = now >= end ? 0 : static_cast<DWORD>(end - now);
        WaitForSingleObject(h, left); // a worker still running keeps its own pin and ends by itself
        CloseHandle(h);
    }
}

int workerCount()
{
    AcquireSRWLockExclusive(&g_lock);
    pruneWorkersLocked(state().workers);
    const int count = static_cast<int>(state().workers.size());
    ReleaseSRWLockExclusive(&g_lock);
    return count;
}

} // namespace proxy
