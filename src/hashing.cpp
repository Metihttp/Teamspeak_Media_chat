#include "hashing.h"

#include <QCryptographicHash>
#include <QFile>

#include <memory>

#include "crypto.h" // 2.2: Windows CNG (the updater's module)

namespace hashing {

namespace {

// One SHA-256 computation: Windows CNG (crypto::Sha256, about ten times faster than Qt's own code)
// when its algorithm can be opened, else QCryptographicHash. Chosen once per computation; nothing is
// shared between threads or kept after it.
class Digest
{
  public:
    Digest()
        : m_qt(QCryptographicHash::Sha256)
    {
        m_cng = std::make_unique<crypto::Sha256>();
        if (!m_cng->ok())
            m_cng.reset();
    }

    void add(const char* data, qint64 size)
    {
        if (m_cng)
            m_cng->add(data, static_cast<size_t>(size));
        else
            m_qt.addData(data, static_cast<int>(size));
    }

    // The 32-byte digest; empty if CNG failed part of the way (nothing to fall back to then).
    QByteArray finish()
    {
        if (!m_cng)
            return m_qt.result();
        const QByteArray digest = m_cng->ok() ? m_cng->finish() : QByteArray();
        return digest.size() == 32 ? digest : QByteArray();
    }

  private:
    std::unique_ptr<crypto::Sha256> m_cng;
    QCryptographicHash              m_qt;
};

} // namespace

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
    const qint64 total = file.size();
    Digest       hash;
    QByteArray   buffer(static_cast<int>(kChunkBytes), Qt::Uninitialized);
    qint64       done       = 0;
    qint64       nextReport = kProgressStepBytes;
    for (;;) {
        if (cancel && cancel->load())
            return fail(QStringLiteral("canceled"));
        const qint64 read = file.read(buffer.data(), kChunkBytes);
        if (read < 0)
            return fail(file.errorString());
        if (read == 0)
            break;
        hash.add(buffer.constData(), read);
        done += read;
        if (progress && done >= nextReport) {
            progress(done, total);
            nextReport = done + kProgressStepBytes;
        }
    }
    // A file that changed size while it was read isn't the file that was measured.
    if (done != total)
        return fail(QStringLiteral("the file changed while it was read"));
    const QByteArray digest = hash.finish();
    if (digest.isEmpty())
        return fail(QStringLiteral("hashing failed"));
    if (progress)
        progress(done, total);
    if (error)
        error->clear();
    return digest;
}

QByteArray sha256(const QByteArray& data)
{
    const QByteArray digest = crypto::Sha256::hash(data);
    return digest.size() == 32 ? digest : QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString backendName()
{
    const crypto::Sha256 probe;
    return probe.ok() ? QString::fromLatin1("Windows CNG (BCrypt)") : QString::fromLatin1("Qt (QCryptographicHash)");
}

QString toHex(const QByteArray& digest)
{
    return QString::fromLatin1(digest.toHex()).toUpper();
}

} // namespace hashing
