#include "uploadwait.h"

#include <QFile>
#include <QIODevice>

namespace uploadwait {

namespace {

// "/tsmedia/a.png", "tsmedia/a.png" and "/tsmedia//a.png" alike.
QString normalizedPath(const QString& path)
{
    QString out;
    out.reserve(path.size() + 1);
    for (const QChar c : path) {
        const QChar ch = c == QLatin1Char('\\') ? QChar(QLatin1Char('/')) : c;
        if (ch == QLatin1Char('/') && out.endsWith(QLatin1Char('/')))
            continue;
        out += ch;
    }
    if (!out.startsWith(QLatin1Char('/')))
        out.prepend(QLatin1Char('/'));
    while (out.length() > 1 && out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

QString fileNameOf(const QString& path)
{
    const QString normalized = normalizedPath(path);
    return normalized.mid(normalized.lastIndexOf(QLatin1Char('/')) + 1);
}

} // namespace

bool sameRemoteFile(const QString& reported, const QString& requested)
{
    if (reported.isEmpty() || requested.isEmpty())
        return false;
    const QString path = normalizedPath(reported);
    if (path == normalizedPath(requested))
        return true;
    // The name alone ("a.png", "/a.png"): our names carry a random part, the server and channel match.
    return path.lastIndexOf(QLatin1Char('/')) == 0 && path.mid(1) == fileNameOf(requested);
}

// ---- sender ---------------------------------------------------------------------------------------

int confirmPollDelayMs(int asked)
{
    static const int kDelays[] = {kFirstPollMs, kFirstPollMs, 500, 800, 1200};
    constexpr int    kCount    = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
    if (asked < 0)
        asked = 0;
    return asked < kCount ? kDelays[asked] : kMaxPollMs;
}

qint64 confirmTimeoutMs(quint64 fileSize)
{
    const quint64 slowMs = fileSize / static_cast<quint64>(kSlowBytesPerSecond) * 1000
                           + fileSize % static_cast<quint64>(kSlowBytesPerSecond) * 1000 / static_cast<quint64>(kSlowBytesPerSecond);
    const quint64 limit  = static_cast<quint64>(kConfirmMaxMs - kConfirmBaseMs);
    return kConfirmBaseMs + static_cast<qint64>(qMin(slowMs, limit));
}

ConfirmState startConfirm(quint64 fileSize, qint64 nowMs)
{
    ConfirmState state;
    state.startedAt = nowMs;
    state.deadline  = nowMs + confirmTimeoutMs(fileSize);
    state.grewAt    = nowMs;
    return state;
}

void nextConfirmFile(ConfirmState& state, qint64 nowMs)
{
    state.grewAt   = nowMs;
    state.lastSize = -1;
    state.asked    = 0;
}

ConfirmStep confirmStep(ConfirmState& state, quint64 expected, Answer answer, quint64 size, qint64 nowMs)
{
    switch (answer) {
    case Answer::Missing:
        return {Confirm::Missing, 0};
    case Answer::Refused:
        return {Confirm::Refused, 0};
    case Answer::Size:
        if (size == expected)
            return {Confirm::Confirmed, 0};
        if (size > expected)
            return {Confirm::Larger, 0};
        // Still arriving: any growth counts as progress.
        if (static_cast<qint64>(size) > state.lastSize) {
            if (state.lastSize >= 0)
                state.grewAt = nowMs;
            state.lastSize = static_cast<qint64>(size);
        }
        break;
    case Answer::None:
        break;
    }
    if (nowMs >= state.deadline)
        return {Confirm::TimedOut, 0};
    if (nowMs - state.grewAt >= kConfirmStallMs)
        return {Confirm::Stalled, 0};
    return {Confirm::Poll, confirmPollDelayMs(state.asked++)};
}

QString confirmName(Confirm what)
{
    switch (what) {
    case Confirm::Confirmed:
        return QStringLiteral("confirmed");
    case Confirm::Poll:
        return QStringLiteral("still arriving");
    case Confirm::Larger:
        return QStringLiteral("larger than uploaded");
    case Confirm::Missing:
        return QStringLiteral("no such file");
    case Confirm::Refused:
        return QStringLiteral("can't be asked");
    case Confirm::TimedOut:
        return QStringLiteral("timed out");
    case Confirm::Stalled:
        return QStringLiteral("stopped growing");
    }
    return {};
}

// ---- sender: the server's bytes -----------------------------------------------------------------

quint64 tailLength(quint64 fileSize)
{
    return qMin<quint64>(fileSize, static_cast<quint64>(kTailBytes));
}

QByteArray tailWindow(const QByteArray& lastBytes, quint64 fileSize)
{
    if (lastBytes.isEmpty() || static_cast<quint64>(lastBytes.size()) > fileSize)
        return {};
    const int zeros = trailingZeros(lastBytes);
    if (zeros == lastBytes.size())
        return {}; // all zeros as far as known: nothing shows whether the data arrived
    const int data = lastBytes.size() - zeros; // the data in lastBytes, up to the last non-zero byte
    return lastBytes.right(zeros + qMin(data, kTailBytes));
}

int contentRetryDelayMs(int retries)
{
    static const int kDelays[] = {500, 1000, 2000, 3000, 5000, 8000};
    constexpr int    kCount    = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
    if (retries < 0)
        retries = 0;
    return kDelays[retries < kCount ? retries : kCount - 1];
}

int contentTransferTimeoutMs(Method method, quint64 fileSize)
{
    if (method != Method::Full)
        return kContentTransferTimeoutMs;
    const quint64 wholeMs = qMin(fileSize, kMaxFullCheckBytes) * 1000 / static_cast<quint64>(kFullCheckBytesPerSecond);
    return kContentTransferTimeoutMs + static_cast<int>(wholeMs);
}

Method firstMethod(bool haveTail, bool canHash, quint64 fileSize)
{
    if (haveTail)
        return Method::Tail;
    if (canHash && fileSize <= kMaxFullCheckBytes)
        return Method::Full;
    return Method::SizeOnly;
}

Bytes compareTail(const QByteArray& expected, quint64 fileSize, qint64 downloadedSize, const QByteArray& gotTail)
{
    if (downloadedSize < 0 || static_cast<quint64>(downloadedSize) > fileSize || static_cast<quint64>(expected.size()) > fileSize)
        return Bytes::WrongLength; // nothing, or more than the file (the whole file appended: the server can't seek)
    const qint64 stub = static_cast<qint64>(fileSize) - expected.size();
    if (static_cast<quint64>(downloadedSize) < fileSize) {
        // Some bytes from where they were asked for, but not all: a server that sends what it has so far
        // (the upload still arriving). Nothing at all: can't tell, as if it couldn't seek.
        return downloadedSize > stub ? Bytes::Different : Bytes::WrongLength;
    }
    if (gotTail == expected)
        return Bytes::Same;
    // Still arriving at a preallocating server: zeros where the bytes haven't arrived, so more of them at
    // the end than the uploaded end has (once the last non-zero byte is there, the rest is equal).
    // Other bytes are another part of the file (the resume's offset ignored) or another file.
    if (gotTail.size() == expected.size() && trailingZeros(gotTail) > trailingZeros(expected))
        return Bytes::Different;
    return Bytes::NotTheEnd;
}

ContentState startContent(Method method, qint64 nowMs)
{
    ContentState state;
    state.method    = method;
    state.startedAt = nowMs;
    state.changedAt = nowMs;
    return state;
}

ContentStep contentStep(ContentState& state, Bytes result, const QByteArray& fingerprint, bool canHash, quint64 fileSize, qint64 deadline, qint64 nowMs)
{
    switch (result) {
    case Bytes::Same:
        return {Content::Confirmed, 0};
    case Bytes::Refused:
        // No permission to download it: the whole file would be refused too.
        state.method = Method::SizeOnly;
        return {Content::SizeOnly, 0};
    case Bytes::WrongLength:
    case Bytes::NotTheEnd:
    case Bytes::Failed:
        // The tail can't be read: the whole file, while it can be compared and isn't too large. A whole
        // file of another length is just other bytes (the server is still short).
        if (state.method == Method::Tail) {
            if (canHash && fileSize <= kMaxFullCheckBytes) {
                state.method    = Method::Full;
                state.seen      = QByteArray();
                state.changedAt = nowMs;
                return {Content::Full, 0};
            }
            state.method = Method::SizeOnly;
            return {Content::SizeOnly, 0};
        }
        if (state.method != Method::Full || result == Bytes::Failed) {
            state.method = Method::SizeOnly;
            return {Content::SizeOnly, 0};
        }
        break;
    case Bytes::Different:
        break;
    }
    // Other bytes: the upload is still arriving. What the server shows changing counts as progress.
    if (!fingerprint.isEmpty() && fingerprint != state.seen) {
        if (!state.seen.isEmpty())
            state.changedAt = nowMs;
        state.seen = fingerprint;
    }
    if (nowMs >= deadline)
        return {Content::TimedOut, 0};
    if (nowMs - state.changedAt >= kConfirmStallMs)
        return {Content::Stalled, 0};
    return {Content::Retry, contentRetryDelayMs(state.retries++)};
}

QString methodName(Method method)
{
    switch (method) {
    case Method::Tail:
        return QStringLiteral("tail");
    case Method::Full:
        return QStringLiteral("full");
    case Method::SizeOnly:
        return QStringLiteral("size only");
    }
    return {};
}

QString contentName(Content what)
{
    switch (what) {
    case Content::Confirmed:
        return QStringLiteral("confirmed");
    case Content::Retry:
        return QStringLiteral("still arriving");
    case Content::Full:
        return QStringLiteral("tail unreadable");
    case Content::SizeOnly:
        return QStringLiteral("not possible");
    case Content::TimedOut:
        return QStringLiteral("timed out");
    case Content::Stalled:
        return QStringLiteral("stopped changing");
    }
    return {};
}

// ---- receiver: zero tails -----------------------------------------------------------------------

qint64 zeroRunThreshold(bool audioOrVideo, bool mismatched)
{
    if (mismatched)
        return kMismatchZeroRunBytes;
    return audioOrVideo ? kMediaZeroRunBytes : kZeroRunBytes;
}

int trailingZeros(const QByteArray& bytes)
{
    int end = bytes.size();
    while (end > 0 && bytes.at(end - 1) == '\0')
        --end;
    return bytes.size() - end;
}

qint64 dataEnd(QIODevice& device, const std::atomic<bool>* cancel)
{
    constexpr qint64 kChunk = 1024 * 1024;
    if (!device.isOpen() || !device.isReadable() || device.isSequential())
        return -1;
    qint64     end = device.size();
    QByteArray chunk;
    while (end > 0) {
        if (cancel && cancel->load())
            return -1;
        const qint64 length = qMin(kChunk, end);
        if (!device.seek(end - length))
            return -1;
        chunk = device.read(length);
        if (chunk.size() != length)
            return -1;
        const int zeros = trailingZeros(chunk);
        if (zeros < chunk.size())
            return end - zeros;
        end -= length;
    }
    return 0;
}

qint64 dataEnd(const QString& path, const std::atomic<bool>* cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    return dataEnd(file, cancel);
}

bool isZeroTail(qint64 size, qint64 dataEnd, bool audioOrVideo, bool mismatched)
{
    return size > 0 && dataEnd >= 0 && dataEnd <= size && size - dataEnd >= zeroRunThreshold(audioOrVideo, mismatched);
}

// ---- receiver -------------------------------------------------------------------------------------

int refetchDelayMs(int step)
{
    static const int kDelays[] = {2000, 4000, 8000, 15000, 30000};
    constexpr int    kCount    = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
    if (step < 0)
        step = 0;
    return kDelays[step < kCount ? step : kCount - 1];
}

RefetchStep nextStep(RefetchState& state, Seen seen, quint64 linkSize, quint64 downloaded, const QByteArray& digest, Answer answer, quint64 serverSize,
                     qint64 nowMs)
{
    if (state.startedAt < 0) {
        state.startedAt = nowMs;
        state.grewAt    = nowMs;
    }
    // No answer (the server's flood protection, a timeout): that says nothing about the file. The same
    // question again in a moment (Core asks once the flood pause is over); only after a few in a row
    // does the server count as one that can't tell.
    if (answer == Answer::None) {
        if (++state.noAnswers < kMaxNoAnswers)
            return {Next::Ask, kSoonMs, false};
        answer = Answer::Refused;
    }
    state.noAnswers = 0;

    // Progress: the server has more than before, or a download brought more than any before it (of a
    // zero-tailed one: the bytes before its zeros; the server's size says nothing then). A short or
    // zero-tailed download started again that brought no more than the longest before it is flat.
    if (answer == Answer::Size) {
        if (state.serverSize >= 0 && static_cast<qint64>(serverSize) > state.serverSize)
            state.grewAt = nowMs;
        state.serverSize = static_cast<qint64>(serverSize);
    }
    if (seen != Seen::Poll) {
        if (downloaded > state.longest) {
            state.grewAt  = nowMs; // also the first data after a download that had none (all zeros)
            state.longest = downloaded;
            state.flat    = 0;
        } else if ((seen == Seen::Short || seen == Seen::ZeroTail) && state.downloads > 0) {
            ++state.flat;
        }
    }
    const bool stalled = nowMs - state.grewAt >= kNoProgressMs;
    // A zero tail of a link with a sha: the same digest as the zero-tailed download before it means the
    // server's copy didn't change at all.
    bool sameZeroTail = false;
    if (seen == Seen::ZeroTail && !digest.isEmpty()) {
        sameZeroTail     = digest == state.lastDigest;
        state.lastDigest = digest;
    }
    const bool serverFull = answer == Answer::Size && serverSize == linkSize;

    // A zero tail that ends while its copy on the server stays the same and the server reports the whole
    // file: confirmed (Core notes the digest for the forged-hash guard, see RefetchStep::confirmed).
    const auto failure = [&] {
        if (seen == Seen::Mismatch)
            return RefetchStep{Next::FailMismatch, 0, false};
        return RefetchStep{Next::FailIncomplete, 0, seen == Seen::ZeroTail && sameZeroTail && serverFull};
    };
    // Zero tails: the bytes before the zeros may stand still for a while (an old sender's last bytes on a
    // slow uplink, a server that writes what arrives in batches): flat downloads end it only once nothing
    // grew for kZeroTailStallMs (four downloads, 2 + 4 + 8 + 15 s apart).
    const bool flatEnough = state.flat >= kMaxFlatDownloads && (seen != Seen::ZeroTail || nowMs - state.grewAt >= kZeroTailStallMs);
    const auto download   = [&](int delayMs, bool afterFull) {
        if (state.downloads >= kMaxDownloads || stalled || flatEnough)
            return failure();
        ++state.downloads;
        state.afterFull = afterFull;
        return RefetchStep{Next::Download, delayMs, false};
    };
    // A complete download that doesn't match while the server has (or may have) the whole file.
    // serverFull: the server reports the whole file now. Real (confirmed, worth noting) once a download
    // started after the server reported the whole file gives the same wrong digest as the one before it.
    // While the server can't tell, the same wrong digest again only shows that nothing changes: it ends
    // without being noted (a click tries again).
    const auto mismatch = [&](bool serverFull) {
        if (!digest.isEmpty() && digest == state.lastDigest) {
            if (serverFull && state.afterFull)
                return RefetchStep{Next::FailMismatch, 0, true};
            if (!serverFull)
                return RefetchStep{Next::FailMismatch, 0, false};
        }
        state.lastDigest = digest;
        return download(refetchDelayMs(state.pauses++), serverFull);
    };

    switch (answer) {
    case Answer::Missing:
        return {Next::FailMissing, 0, false};
    case Answer::Size:
        if (serverSize > linkSize)
            return {Next::FailChanged, 0, false};
        if (serverSize < linkSize) {
            // Still arriving: ask again after a pause, as long as it keeps growing.
            if (stalled)
                return {Next::FailIncomplete, 0, false};
            return {Next::Poll, refetchDelayMs(state.pauses++), false};
        }
        // The server has the whole file now.
        if (seen == Seen::Mismatch)
            return mismatch(true);
        // A zero tail: the server preallocates, its size says nothing. Downloaded again after a pause, as
        // long as the bytes before the zeros keep growing.
        if (seen == Seen::ZeroTail)
            return download(refetchDelayMs(state.pauses++), true);
        return download(seen == Seen::Poll ? 0 : kSoonMs, true);
    case Answer::Refused:
    case Answer::None:
        // The server can't tell: the next download has to.
        if (seen == Seen::Mismatch)
            return mismatch(false);
        if (seen == Seen::Poll)
            return download(0, false);
        return download(refetchDelayMs(state.pauses++), false);
    }
    return failure();
}

bool zeroTailSettled(const RefetchState& state, quint64 dataEnd, qint64 nowMs)
{
    if (state.startedAt < 0 || state.downloads == 0 || dataEnd > state.longest)
        return false; // the first download (it is waited for), or more data than before: still arriving
    return state.flat + 1 >= kMaxFlatDownloads || nowMs - state.grewAt >= kNoProgressMs;
}

} // namespace uploadwait
