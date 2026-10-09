#pragma once

// 2.2 sha: checking downloads against the SHA-256 in their link, the sender's one hashing step, and
// the guard against links with made-up hashes. QtCore only, so the unit tests drive all of it with a
// fake environment (tests/tst_fileverify.cpp); Core wires it to TeamSpeak.
//
// Receiver. Core hands a finished download to Verifier::start() once TeamSpeak has closed the file
// (Core::finishWhenWritten). The file is hashed on the verifier's own threads, never on the GUI thread.
// A file that doesn't match is checked once more before that counts: TeamSpeak may report a transfer
// complete while the file is still being written (the 2.0.5 write race), so the second pass waits
// until nothing has the file open for writing, and a moment longer. Only a match lets Core move the
// file into the cache. A mismatch is final (decisions: "Block only"): the file is deleted, and the
// preview asks for the file to be sent again.
//
// Sender. finalizeStaged() is the one step that hashes what gets uploaded: the final staged bytes,
// right before the preview and the file are uploaded (2.4: after a transcode or an edit). A read error
// isn't fatal: the link goes out without a sha.
//
// Forged hashes. Core remembers the real digest of server files it has seen (its own uploads, checked
// downloads). A link naming the same file and size with another hash is refused at once, without
// downloading the file again (KnownDigests).

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>

#include "hashing.h"

class QTimer;

namespace fileverify {

constexpr int kShaBytes        = 32;
constexpr int kPreviewShaBytes = 16;

// hashing::sha256File's signature, so tests can put a fake in its place.
using HashFile = std::function<QByteArray(const QString& path, const std::atomic<bool>* cancel, const hashing::Progress& progress, QString* error)>;

// ---- sender ---------------------------------------------------------------------------------------

struct StagedDigest {
    QByteArray sha256;     // of the staged file; empty when it couldn't be read (the link goes without)
    QByteArray previewSha; // the first 16 bytes of SHA-256(preview JPEG); empty without a preview
    QString    error;      // why sha256 is empty ("canceled" after a cancel)
};

// Worker thread. hash: hashing::sha256File when empty.
StagedDigest finalizeStaged(const QString& stagedPath, const QByteArray& previewJpeg, const std::atomic<bool>* cancel, const HashFile& hash = {});

// ---- preview ("ph") -------------------------------------------------------------------------------

QByteArray previewDigest(const QByteArray& previewBytes); // the first 16 bytes of its SHA-256
// The preview's bytes match ph. Previews are at most 5 MB (Core's limit), cheap enough for the GUI
// thread.
bool previewMatches(const QByteArray& previewBytes, const QByteArray& previewSha);

// ---- forged-hash guard ----------------------------------------------------------------------------

// The real SHA-256 of files on servers, by Core::remoteId (server, channel, path) and size. Bounded:
// the entries noted longest ago are forgotten first. GUI thread only.
class KnownDigests
{
  public:
    enum class Verdict { Unknown, Agrees, Contradicts };

    static constexpr int kMaxEntries = 4096;

    void       note(const QString& remoteId, quint64 size, const QByteArray& digest); // a newer note replaces an older one
    Verdict    check(const QString& remoteId, quint64 size, const QByteArray& claimed) const;
    QByteArray known(const QString& remoteId, quint64 size) const; // empty if unknown
    int        count() const { return m_digests.size(); }
    void       clear();

  private:
    struct Known {
        QByteArray digest;
        quint64    stamp = 0;
    };
    QHash<QString, Known>   m_digests; // remoteId + "\n" + size
    QMap<quint64, QString>  m_byAge;   // stamp -> key, oldest first
    quint64                 m_stamp = 0;
};

// ---- receiver -------------------------------------------------------------------------------------

enum class Outcome { Match, Mismatch, ReadError };

struct Result {
    QString    key;
    Outcome    outcome = Outcome::ReadError;
    QByteArray expected;
    QByteArray received;      // what the file hashed to (the last pass); empty after a read error
    QByteArray firstReceived; // the first pass, when a second one ran (empty if it couldn't read)
    bool       rechecked = false;
    QString    error;         // ReadError: why
    qint64     elapsedMs = 0; // start() to the result

    // A mismatch both passes agree on: the file on the server really is a different one (Core then
    // remembers its digest for the forged-hash guard). Not after a race: the passes differ then.
    bool stableMismatch() const { return outcome == Outcome::Mismatch && rechecked && !received.isEmpty() && received == firstReceived; }
};

struct Environment {
    HashFile                                 hashFile;       // worker threads; hashing::sha256File when empty
    std::function<bool(const QString& path)> isBeingWritten; // GUI thread; when empty nothing ever is
    int                                      threads        = 2;
    int                                      recheckDelayMs = 1000;  // once no one writes the file, before the second pass
    int                                      writePollMs    = 100;
    int                                      maxWriteWaitMs = 30000; // the second pass runs anyway after this long
};

// Checks downloaded files on its own threads. GUI thread only (results arrive there too).
class Verifier : public QObject
{
    Q_OBJECT

  public:
    explicit Verifier(Environment environment = {}, QObject* parent = nullptr);
    ~Verifier() override; // shutdown()

    // Checks the file at path against expected (32 bytes) for key. A check still running for key is
    // dropped first (its result never arrives). The file must stay where it is until finished().
    void start(const QString& key, const QString& path, const QByteArray& expected);
    void cancel(const QString& key); // its result never arrives
    void cancelAll();
    // cancelAll(), then waits for passes still running: each stops within one chunk (hashing.h). For
    // ~Core and plugin unload. Nothing is reported afterwards.
    void shutdown();

    bool   isChecking(const QString& key) const;
    bool   isSecondPass(const QString& key) const;
    double progress(const QString& key) const; // of the current pass, 0..1; < 0 before its first report
    int    running() const { return m_jobs.size(); }

  signals:
    void progressChanged(const QString& key, double fraction, bool secondPass);
    void finished(const fileverify::Result& result);

  private:
    struct Job {
        quint64                            id = 0;
        QString                            path;
        QByteArray                         expected;
        std::shared_ptr<std::atomic<bool>> cancel;
        int                                pass     = 0;     // 1 or 2 while hashing
        bool                               waiting  = false; // for the second pass
        qint64                             waitFrom = 0;
        qint64                             dueAt    = -1;    // the second pass starts then (-1: writer not gone yet)
        double                             progress = -1.0;
        QByteArray                         firstReceived;
        QElapsedTimer                      started;
    };

    void runPass(const QString& key);
    void onPassDone(const QString& key, quint64 id, int pass, const QByteArray& digest, const QString& error);
    void onPassProgress(const QString& key, quint64 id, int pass, double fraction);
    void pollWaiting();
    void finish(const QString& key, Outcome outcome, const QByteArray& received, const QString& error);

    Environment         m_env;
    QHash<QString, Job> m_jobs;
    quint64             m_nextId  = 0;
    bool                m_closing = false;
    QTimer*             m_poll    = nullptr; // runs while a check waits for its second pass
    QElapsedTimer       m_clock;
    QThreadPool         m_pool;              // waited for in shutdown()
};

// ---- texts ----------------------------------------------------------------------------------------

// "Copy details" on a file that didn't match: the expected and received SHA-256 (upper-case hex, like
// Get-FileHash) and the file's path on the server.
QString mismatchDetails(const QByteArray& expected, const QByteArray& received, const QString& remoteFile);

// ---- diagnostics ----------------------------------------------------------------------------------

// Session counters (counts only, never names) for the diagnostic info. The diagnostics integration
// adds diagnosticLines() as a section of its own (diag::addSectionProvider). Thread-safe.
enum class Counter {
    Verified,           // downloads that matched their sha
    RecoveredOnRecheck, // ... of them only on the second pass (the file was still being written)
    Mismatched,         // downloads that didn't match (deleted, never shown)
    ReadErrors,         // downloads that couldn't be read to check them
    ForgedBlocked,      // links refused because their sha contradicts the file's known one
    PreviewMismatched,  // previews that didn't match their ph (the placeholder stays)
    SentWithSha,
    SentWithoutSha,     // the staged file couldn't be read: the link went out without a sha
    Count
};
void        count(Counter counter);
int         counted(Counter counter);
void        resetCounters(); // tests
QString     hashBackend();   // what hashing::sha256File uses
QString     diagnosticsTitle();
QStringList diagnosticLines();

} // namespace fileverify
