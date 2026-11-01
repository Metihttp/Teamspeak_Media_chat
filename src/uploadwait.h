#pragma once

// 2.3.2: the upload race. TeamSpeak reports an upload complete once it has handed the last bytes to its
// socket: on a real internet link part of the file is still on its way to the server then. The chat
// message announcing it reaches the channel at once (the control connection), and whoever downloads it
// right away gets what the server has so far: a file that is too short, or one that doesn't match its
// SHA-256 ("File doesn't match what was sent"). While an upload is still arriving the server reports the
// bytes it has received so far as the file's size (requestFileInfo; seen on server 3.13).
//
// Both sides of the fix, as pure logic (tests/tst_uploadwait.cpp); Core wires them to TeamSpeak:
//
// Sender. A file's chat message waits until the server reports the file's whole size, and the same for
// its preview when the file itself had to be waited for (the preview went out before the file, so
// otherwise it is there). Core asks right away and again after confirmPollDelayMs() while the size is
// short. When the server never gets there (no growth for kConfirmStallMs, or longer than
// confirmTimeoutMs()) or can't be asked (no such file, no permission), the message goes anyway:
// receivers cope (below). A connection lost meanwhile fails the message like any other; Retry asks the
// server again before it posts it (no second upload).
//
// Receiver. A download that came out short, or didn't match its SHA-256, isn't final at once: Core asks
// the server for the file's size and nextStep() decides. Still arriving: it asks again after a pause
// (refetchDelayMs) and downloads again once the server has all of it; no growth for kNoProgressMs and it
// fails (and Core notes the size it stopped at as the file's real one, so other links naming another
// size aren't fetched automatically). The server has all of it: downloaded again after kSoonMs, a file
// that didn't match after a backoff (refetchDelayMs). A mismatch is real (a made-up hash, a
// replaced file) only when a download started after the server reported the whole file gives the same
// wrong digest as the one before it; only then does Core note it for the forged-hash guard
// (fileverify::KnownDigests). No such file any more: NotFound, as before.
//
// A question without an answer (flood protection, a timeout) is asked again (Next::Ask) a few times
// before the server counts as one that can't tell. When it can't (no permission to look at files), the
// next download has to: it goes after a pause; two downloads in a row that bring nothing new end it
// (short: FailIncomplete; the same wrong digest again: an unconfirmed FailMismatch, never noted).
//
// Servers that preallocate uploads (TeaSpeak, seen on Linux): the file has its whole size from the
// start, its end is zeros until the bytes arrive. Its size says nothing then, so:
//
// Sender. Once the server reports the whole size (or can't tell), Core reads the end of the server's
// copy back (tailWindow(): the last kTailBytes of the file's data and any zeros after them, with
// TeamSpeak's resume: a sparse local file of size - N bytes, then requestFile with resume) and compares
// it with the end of what was uploaded (compareTail, contentStep). Equal: the message goes. More zeros
// at the end than uploaded (still arriving): again after contentRetryDelayMs(), within the size check's
// deadline and stall limit, then it goes anyway with a warning. A tail that can't be read (a server that
// can't seek or sends other bytes, a failed transfer): the whole file is downloaded and its SHA-256
// compared with the link's (up to kMaxFullCheckBytes), larger files go after the size check; no
// permission to download (Bytes::Refused): after the size check at once.
//
// Receiver. A complete download that didn't match its SHA-256 (or one without a sha) and ends in a run
// of zeros (isZeroTail) is an upload still arriving on such a server, never a mismatch: Seen::ZeroTail
// waits and downloads again like a short download, its progress being the bytes before the zeros
// (dataEnd); no progress (two downloads in a row and kZeroTailStallMs, or kNoProgressMs) ends it as
// FailIncomplete (the same digest twice while the server reports the whole file: confirmed, Core notes it
// for the forged-hash guard). Without a sha nothing contradicts the file: a zero tail that stops growing is the file's own
// end (zeroTailSettled), kept as it is.

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <atomic>

class QIODevice;

namespace uploadwait {

// What the server said when asked for a file's size (requestFileInfo).
enum class Answer {
    Size,    // the file's size: the bytes the server has
    Missing, // no such file (ERROR_file_not_found)
    Refused, // another error (no permission to browse files, not connected, the request failed)
    None,    // no answer in time, or held back by the server's flood protection
};

// The file the server's file info event names is the one asked for. TeamSpeak's event carries the name
// without the return code; it may be the full path ("/tsmedia/a.png") or the name alone ("a.png",
// "/a.png"). Core only compares it with questions on the same connection and channel.
bool sameRemoteFile(const QString& reported, const QString& requested);

// 2.3.3: TeaSpeak answers a file info request (requestFileInfo) with a notification the TeamSpeak client
// can't parse ("Error while parsing packet: parameter not found"), and the client drops the connection
// at once (seen on TeaSpeak 1.5.6 on Linux). So a file's size is asked only of servers that look
// like TeamSpeak's own: a version such as "3.13.7 [Build: 1655727713]", nothing that names TeaSpeak.
// Any other server (or one whose version isn't known yet) counts as one that can't tell
// (Answer::Refused, without asking): the sender checks the file's bytes, the receiver downloads again.
bool looksLikeTeaSpeak(const QString& version, const QString& platform);
bool canAskFileInfo(const QString& version, const QString& platform);

// ---- sender ---------------------------------------------------------------------------------------

constexpr int    kFirstPollMs        = 300;              // the second question, after a short size
constexpr int    kMaxPollMs          = 2000;             // the pause between questions grows to this
constexpr qint64 kConfirmStallMs     = 60 * 1000;        // the server's size didn't grow for this long: post anyway
constexpr qint64 kConfirmBaseMs      = 60 * 1000;        // the time limit: this ...
constexpr qint64 kSlowBytesPerSecond = 100 * 1024;       // ... plus the whole file at this rate ...
constexpr qint64 kConfirmMaxMs       = 15 * 60 * 1000;   // ... but never more than this

// The pause before the next question once `asked` questions got a short size: 300, 300, 500, 800,
// 1200 ms, then 2 s.
int confirmPollDelayMs(int asked);
// How long a message waits at most for the server to have the whole file.
qint64 confirmTimeoutMs(quint64 fileSize);

enum class Confirm {
    Confirmed, // the server has the whole file
    Poll,      // ask again after delayMs
    Larger,    // the server reports more than was uploaded (not ours any more?): post anyway
    Missing,   // no such file: post anyway
    Refused,   // can't be asked: post anyway
    TimedOut,  // still short after confirmTimeoutMs: post anyway
    Stalled,   // still short, no growth for kConfirmStallMs: post anyway
};

struct ConfirmState {
    qint64 startedAt = 0;  // the upload was reported complete (any monotonic clock, ms)
    qint64 deadline  = 0;  // startedAt + confirmTimeoutMs(); shared by the file and its preview
    qint64 grewAt    = 0;  // the server's size last grew (startedAt, then each new file's start)
    qint64 lastSize  = -1; // what the server reported last for the current file (-1: nothing yet)
    int    asked     = 0;  // questions about the current file that got a short (or no) size
};

struct ConfirmStep {
    Confirm what    = Confirm::Poll;
    int     delayMs = 0; // Poll: before the next question
};

// The state for an upload of fileSize bytes reported complete at nowMs.
ConfirmState startConfirm(quint64 fileSize, qint64 nowMs);
// The next file of the same upload (its preview): the deadline stays, the rest starts over.
void nextConfirmFile(ConfirmState& state, qint64 nowMs);
// The answer to a question about a file of expected bytes: what to do next.
ConfirmStep confirmStep(ConfirmState& state, quint64 expected, Answer answer, quint64 size, qint64 nowMs);
// For the log: "confirmed", "timed out", ...
QString confirmName(Confirm what);

// ---- sender: the server's bytes (preallocating servers) -------------------------------------------

constexpr int     kTailBytes                = 64 * 1024;         // compared at the end of a file('s data)
constexpr int     kMaxTailWindowBytes       = 1024 * 1024;       // a tail with the zeros after the data: at most this long
constexpr quint64 kMaxFullCheckBytes        = 16 * 1024 * 1024;  // downloaded whole when the tail can't be read
constexpr int     kContentTransferTimeoutMs = 20 * 1000;         // a check's download taking longer: the bytes aren't there yet
constexpr qint64  kFullCheckBytesPerSecond  = 100 * 1024;        // ... plus a whole file's download at this rate

// How a file's bytes on the server are checked.
enum class Method {
    Tail,     // its tailWindow(), compared with the uploaded bytes
    Full,     // the whole file, its SHA-256 compared with the link's
    SizeOnly, // not at all: the message goes after the size check
};

// min(fileSize, kTailBytes).
quint64 tailLength(quint64 fileSize);
// The bytes a tail check compares, from lastBytes: the last bytes of an uploaded file of fileSize bytes
// (its last min(fileSize, kMaxTailWindowBytes) when they end in zeros, else tailLength() of them are
// enough). A preallocating server has zeros where the bytes haven't arrived, and the bytes arrive in
// order: once the file's last non-zero byte is there, so is the rest. So the window is the last
// kTailBytes of the data and the zeros after them (the last tailLength() bytes of a file that doesn't
// end in zeros). Empty: no window (lastBytes are all zeros: the file is, or its zeros at the end are
// longer than kMaxTailWindowBytes).
QByteArray tailWindow(const QByteArray& lastBytes, quint64 fileSize);
// The pause before checking again once `retries` checks found other bytes: 0.5, 1, 2, 3, 5, then 8 s.
int contentRetryDelayMs(int retries);
// How long a check's download may take: kContentTransferTimeoutMs, plus a whole file at
// kFullCheckBytesPerSecond for Method::Full.
int contentTransferTimeoutMs(Method method, quint64 fileSize);
// The first way to check a file of fileSize bytes: its tail when the uploaded tail is known, else the
// whole file when it has a SHA-256 to compare with (canHash) and is small enough, else none.
Method firstMethod(bool haveTail, bool canHash, quint64 fileSize);

// What a check's download showed.
enum class Bytes {
    Same,        // the uploaded bytes
    Different,   // still arriving: more zeros at the end than uploaded, fewer bytes than the file has (a
                 // tail: some, from where it was asked for), the file busy, or the download didn't finish in time
    WrongLength, // a tail came back longer than the file, or with nothing at all: the server can't seek
                 // (or ignored the resume)
    NotTheEnd,   // a tail of the right length with other bytes, not just more zeros: not the file's end
                 // (the server sent the bytes from elsewhere, ignoring the resume's offset)
    Failed,      // the download failed (an error, the local file couldn't be made)
    Refused,     // no permission to download the file: no other way to download it works either
};

// A tail check's download of a file of fileSize bytes: downloadedSize (the local file of fileSize -
// expected.size() bytes it continued, and what came), and its last expected.size() bytes (gotTail)
// compared with the uploaded ones (expected).
Bytes compareTail(const QByteArray& expected, quint64 fileSize, qint64 downloadedSize, const QByteArray& gotTail);

enum class Content {
    Confirmed, // the server has the uploaded bytes
    Retry,     // check again the same way after delayMs
    Full,      // the tail can't be read: download the whole file (at once)
    SizeOnly,  // no content check is possible: the message goes after the size check
    TimedOut,  // still other bytes at the size check's deadline: posted anyway
    Stalled,   // the server's bytes didn't change for kConfirmStallMs: posted anyway
};

struct ContentState {
    Method     method    = Method::Tail;
    qint64     startedAt = 0;
    qint64     changedAt = 0; // what the checks found last changed (startedAt at first)
    QByteArray seen;          // what the last check found (a fingerprint; empty: nothing yet)
    int        retries   = 0; // checks that found other bytes
};

struct ContentStep {
    Content what    = Content::Retry;
    int     delayMs = 0; // Retry: before the next check
};

ContentState startContent(Method method, qint64 nowMs);
// The result of the check that ran with state.method. fingerprint: what it found (any digest of the
// bytes; a change counts as progress). deadline: the size check's (ConfirmState::deadline).
ContentStep contentStep(ContentState& state, Bytes result, const QByteArray& fingerprint, bool canHash, quint64 fileSize, qint64 deadline, qint64 nowMs);
// For the log: "tail", "full", "size only".
QString methodName(Method method);
QString contentName(Content what);

// ---- receiver -------------------------------------------------------------------------------------

constexpr qint64 kNoProgressMs     = 3 * 60 * 1000; // nothing grew for this long: the upload isn't coming
constexpr int    kMaxDownloads     = 8;             // downloads started again for one file at most
constexpr int    kMaxFlatDownloads = 2;             // short downloads in a row that brought no more than before
constexpr int    kMaxNoAnswers     = 3;             // questions in a row without an answer: the server can't tell
constexpr int    kSoonMs           = 2000;          // the server has the whole file: downloaded again after this
constexpr qint64 kZeroTailStallMs  = 25 * 1000;     // zero tails: flat downloads end it only once nothing grew for this long

// The pause before the next question (or download) once `step` pauses were taken: 2, 4, 8, 15, 30 s,
// then 30 s.
int refetchDelayMs(int step);

// What happened before the server was asked.
enum class Seen {
    Short,    // a download came out short (or the server announced fewer bytes than the link says)
    Mismatch, // a complete download didn't match the link's SHA-256
    Poll,     // nothing: waiting for the upload, asked again
    ZeroTail, // a complete download ended in zeros (isZeroTail): downloaded = the bytes before them
};

// ---- receiver: zero tails (preallocating servers) -------------------------------------------------

constexpr qint64 kZeroRunBytes         = 4 * 1024;  // a file ending in this many zeros ...
constexpr qint64 kMediaZeroRunBytes    = 64 * 1024; // ... a video or audio container in this many ...
constexpr qint64 kMismatchZeroRunBytes = 512;       // ... a download that didn't match its SHA-256 in this many

// The zeros a complete download has to end in to count as an upload still arriving. Without a SHA-256
// nothing contradicts the file, and real files end in runs of zeros of their own: a long run is needed
// (kZeroRunBytes, kMediaZeroRunBytes for audio and video). A download that didn't match its SHA-256
// (mismatched) isn't the uploaded file anyway: a short run is enough, so a race on a preallocating
// server with little left to arrive (or a small voice message) never ends as a confirmed mismatch.
qint64 zeroRunThreshold(bool audioOrVideo, bool mismatched = false);
// Zero bytes at the end of bytes.
int trailingZeros(const QByteArray& bytes);
// The length of device's data up to and including its last non-zero byte (0: all zeros), read
// backwards from the end in chunks (a worker thread for large files). -1: it couldn't be read, or
// *cancel was set.
qint64 dataEnd(QIODevice& device, const std::atomic<bool>* cancel = nullptr);
qint64 dataEnd(const QString& path, const std::atomic<bool>* cancel = nullptr);
// A complete download of size bytes whose data ends at dataEnd ends in a zero run long enough to be an
// upload still arriving at a preallocating server (or not written yet). mismatched: see zeroRunThreshold.
bool isZeroTail(qint64 size, qint64 dataEnd, bool audioOrVideo, bool mismatched = false);

enum class Next {
    Download,       // download again after delayMs
    Poll,           // ask the server again after delayMs (what was seen is accounted for)
    Ask,            // no answer: the same question again after delayMs (seen still waits for its answer)
    FailMissing,    // the file is gone from the server (NotFound)
    FailChanged,    // the server's file is larger than the link says: another file (NotFound)
    FailMismatch,   // Mismatch (confirmed: the server had the whole file and two downloads agreed)
    FailIncomplete, // the upload never arrived completely (no growth, kMaxDownloads, or flat downloads;
                    // confirmed: zero tails that stayed the same digest while the server reported the whole file)
};

struct RefetchState {
    qint64     startedAt  = -1; // the first problem (any monotonic clock, ms)
    qint64     grewAt     = -1; // the server's size (or the longest download) last grew
    qint64     serverSize = -1; // what the server reported last (-1: nothing yet)
    quint64    longest    = 0;  // the longest download so far (of a zero-tailed one: the bytes before its zeros)
    int        downloads  = 0;  // downloads started again
    int        pauses     = 0;  // backoff steps taken (refetchDelayMs)
    int        flat       = 0;  // short (or zero-tailed) downloads in a row that brought no more than the longest before
    int        noAnswers  = 0;  // questions in a row without an answer
    bool       afterFull  = false; // the download started last went after the server reported the whole file
    QByteArray lastDigest;         // the wrong digest of the last complete download judged (not one that raced a short server)
};

struct RefetchStep {
    Next next      = Next::Poll;
    int  delayMs   = 0;
    // FailMismatch: a real mismatch, worth noting (see the comment at the top). FailIncomplete after zero
    // tails: the server's copy stays as it is (the same digest twice while it reported the whole file):
    // its digest is worth noting too, so links naming that file with ever new made-up hashes aren't
    // all downloaded again (fileverify::KnownDigests).
    bool confirmed = false;
};

// The server's answer (and serverSize for Answer::Size) after seen: what to do next. linkSize: the
// size the link names (never 0: links without a size aren't fetched again). downloaded: the bytes the
// download that came out short or didn't match brought (ZeroTail: the bytes before its zeros); digest:
// what a Mismatch (or a ZeroTail of a link with a sha) hashed to. A ZeroTail is never a mismatch: it
// waits like a short download (the server's whole size says nothing on a preallocating server:
// downloaded again after a pause) and ends as FailIncomplete when the bytes before the zeros stop
// growing. After Next::Ask the caller asks again with the same seen, downloaded and digest.
RefetchStep nextStep(RefetchState& state, Seen seen, quint64 linkSize, quint64 downloaded, const QByteArray& digest, Answer answer, quint64 serverSize,
                     qint64 nowMs);
// A zero-tailed download of a link without a sha, its data ending at dataEnd, judged before the server
// is asked: true when the refetch so far (state) shows it won't change. It is a download started again
// that brought no more data than the longest before it, for the kMaxFlatDownloads-th time in a row (or
// nothing grew for kNoProgressMs). Nothing contradicts the file then (there is no digest to compare
// with), so its zeros are its own end (a WAV's silence, a BMP's black rows, a tar's padding), and Core
// keeps it as it is instead of failing it.
bool zeroTailSettled(const RefetchState& state, quint64 dataEnd, qint64 nowMs);

} // namespace uploadwait
