#include "pluginlog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

#include <memory>

#include "logtext.h"

namespace plog {

namespace {

struct State {
    QMutex                 mutex;
    QString                directory; // empty: not started
    std::unique_ptr<QFile> file;
};

State& state()
{
    static State s;
    return s;
}

QString backupPath(const QString& directory)
{
    return directory + QStringLiteral("/tsmedia.1.log");
}

QString filePath(const QString& directory)
{
    return directory + QStringLiteral("/tsmedia.log");
}

const char* levelName(int level)
{
    switch (level) {
    case 0:
        return "CRIT";
    case 1:
        return "ERROR";
    case 2:
        return "WARN";
    case 3:
        return "DEBUG";
    case 4:
        return "INFO";
    default:
        return "DEVEL";
    }
}

// Cut to kMaxLineChars without splitting a surrogate pair; a span cut open is closed again, so the
// part that is left stays marked.
QString capped(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (text.size() <= kMaxLineChars)
        return text;
    int cut = kMaxLineChars - 1; // room for "…"
    if (text.at(cut - 1).isHighSurrogate())
        --cut;
    text.truncate(cut);
    const int open  = text.lastIndexOf(ts3::kLogMarkStart);
    const int close = text.lastIndexOf(ts3::kLogMarkEnd);
    if (open >= 0 && open > close) {
        if (open + 1 >= text.size()) // not even the kind letter is left
            text.truncate(open);
        else
            text += ts3::kLogMarkEnd;
    }
    return text + QChar(0x2026);
}

void closeFile(State& s)
{
    if (s.file) {
        s.file->close();
        s.file.reset();
    }
}

bool openFile(State& s)
{
    if (s.file)
        return true;
    QDir().mkpath(s.directory);
    auto file = std::make_unique<QFile>(filePath(s.directory));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    s.file = std::move(file);
    return true;
}

QStringList readLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = file.size();
    if (size > 4 * kMaxFileBytes) // not ours, or damaged: only its end
        file.seek(size - 4 * kMaxFileBytes);
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

void start(const QString& directory)
{
    State&       s = state();
    QMutexLocker lock(&s.mutex);
    closeFile(s);
    s.directory = QDir::cleanPath(directory);
}

void shutdown()
{
    State&       s = state();
    QMutexLocker lock(&s.mutex);
    closeFile(s);
    s.directory.clear();
}

QString formatLine(qint64 msecsSinceEpoch, int offsetFromUtcSeconds, int level, const QString& marked)
{
    const QString time = QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch, Qt::OffsetFromUTC, offsetFromUtcSeconds).toString(Qt::ISODateWithMs);
    return time + QLatin1Char(' ') + QString::fromLatin1(levelName(level)).leftJustified(5) + QLatin1Char(' ') + capped(marked);
}

void write(int level, const QString& marked)
{
    State&       s = state();
    QMutexLocker lock(&s.mutex);
    if (s.directory.isEmpty())
        return;
    const QDateTime  now   = QDateTime::currentDateTime();
    const QByteArray bytes = (formatLine(now.toMSecsSinceEpoch(), now.offsetFromUtc(), level, marked) + QLatin1Char('\n')).toUtf8();
    if (!openFile(s))
        return;
    if (s.file->size() > 0 && s.file->size() + bytes.size() > kMaxFileBytes) {
        closeFile(s);
        QFile::remove(backupPath(s.directory));
        QFile::rename(filePath(s.directory), backupPath(s.directory));
        if (!openFile(s))
            return;
    }
    s.file->write(bytes);
    s.file->flush(); // a crash keeps what was written
}

QStringList tail(int n)
{
    State&       s = state();
    QMutexLocker lock(&s.mutex);
    if (s.directory.isEmpty() || n <= 0)
        return {};
    if (s.file)
        s.file->flush();
    QStringList lines = readLines(filePath(s.directory));
    if (lines.size() < n)
        lines = readLines(backupPath(s.directory)) + lines;
    return lines.mid(qMax(0, lines.size() - n));
}

QString currentFilePath()
{
    State&       s = state();
    QMutexLocker lock(&s.mutex);
    return s.directory.isEmpty() ? QString() : filePath(s.directory);
}

} // namespace plog
