#pragma once

// 2.6 chat proxy: what the bridge knows about each tunnel (design 3.4.6). A record is created when the bridge accepts
// a connection (step Head) and updated at every step, so a request that is still in the proxy's handshake is
// visible too. The Fetcher, the updater, the player and the Test ask "what happened to my request" by their client
// letter and id (the user name tsm-<client><id> they authenticated with): the record is the truth, never a status
// code or a header (S3, F16).
//
// No token, password, user name or payload is ever in a record. Pure QtCore. FROZEN (design 3.1).

#include <QString>
#include <QVector>

#include <memory>

#include "failure.h"

namespace proxy {

struct Record {
    char    client = 0;   // 'f' Fetcher, 'u' updater, 'p' player, 't' test; 0: not authenticated (yet)
    quint64 id     = 0;   // the decimal id after the letter (0: none)
    QString host;         // the validated target name (empty before the head was read)
    Step    step    = Step::Head;
    Failure failure = Failure::None; // None: fine so far (or done)
    int     code    = 0;  // the proxy's HTTP status or SOCKS REP when it refused; the bridge's own status otherwise
    bool    loginUnused = false; // a login was set but the proxy didn't ask for it
    qint64  up   = 0;     // bytes client -> site
    qint64  down = 0;     // bytes site -> client (0: nothing came from the site)
    quint64 startMs = 0;  // GetTickCount64()
    quint64 endMs   = 0;  // 0: still open

    bool established() const { return step == Step::Relay || (step == Step::Done && failure == Failure::None); }
};

class RecordSource
{
  public:
    virtual ~RecordSource() = default;
    // The newest record of (client, id) (a retry of the same request reuses the id). False when there is none.
    virtual bool outcome(char client, quint64 id, Record* out) const = 0;
    // Every record of client that started at or after sinceMs, oldest first.
    virtual QVector<Record> recent(char client, quint64 sinceMs) const = 0;
};

// A record source with nothing in it (no bridge in this session).
std::shared_ptr<RecordSource> emptyRecords();

// Tests: records added by hand.
class FakeRecords : public RecordSource
{
  public:
    void add(const Record& record) { m_records.append(record); }
    void clear() { m_records.clear(); }
    bool outcome(char client, quint64 id, Record* out) const override
    {
        for (int i = m_records.size() - 1; i >= 0; --i) {
            if (m_records.at(i).client == client && m_records.at(i).id == id) {
                *out = m_records.at(i);
                return true;
            }
        }
        return false;
    }
    QVector<Record> recent(char client, quint64 sinceMs) const override
    {
        QVector<Record> out;
        for (const Record& r : m_records) {
            if (r.client == client && r.startMs >= sinceMs)
                out.append(r);
        }
        return out;
    }

  private:
    QVector<Record> m_records;
};

} // namespace proxy
