#include "hashing.h"

#include <QCryptographicHash>
#include <QFile>

namespace hashing {

QByteArray sha256File(const QString& path, const std::atomic<bool>* cancel, const Progress& progress, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return QByteArray();
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    const qint64       total = file.size();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray         buffer(static_cast<int>(kChunkBytes), Qt::Uninitialized);
    qint64             done       = 0;
    qint64             nextReport = kProgressStepBytes;
    for (;;) {
        if (cancel && cancel->load())
            return fail(QStringLiteral("canceled"));
        const qint64 read = file.read(buffer.data(), kChunkBytes);
        if (read < 0)
            return fail(file.errorString());
        if (read == 0)
            break;
        hash.addData(buffer.constData(), static_cast<int>(read));
        done += read;
        if (progress && done >= nextReport) {
            progress(done, total);
            nextReport = done + kProgressStepBytes;
        }
    }
    // A file that changed size while it was read isn't the file that was measured.
    if (done != total)
        return fail(QStringLiteral("the file changed while it was read"));
    if (progress)
        progress(done, total);
    if (error)
        error->clear();
    return hash.result();
}

QByteArray sha256(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString toHex(const QByteArray& digest)
{
    return QString::fromLatin1(digest.toHex()).toUpper();
}

} // namespace hashing
