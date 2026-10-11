#include "chatavatars.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>

#include "ts3api.h"

namespace chatavatars {

QString cacheFileName(const QString& uid)
{
    static const QRegularExpression base64(QStringLiteral("^[A-Za-z0-9+/]{4,86}={0,2}$"));
    if (uid.isEmpty() || uid.size() > 88 || !base64.match(uid).hasMatch())
        return {};
    const QByteArray raw = QByteArray::fromBase64(uid.toLatin1());
    if (raw.isEmpty() || raw.size() > 64) // (20 bytes for a real uid)
        return {};
    QByteArray letters = raw.toHex();
    for (char& c : letters)
        c = static_cast<char>('a' + (c <= '9' ? c - '0' : c - 'a' + 10));
    return QLatin1String("avatar_") + QString::fromLatin1(letters);
}

QString cachePath(const QString& configDir, const QString& serverUid, const QString& uid)
{
    const QString name = cacheFileName(uid);
    if (configDir.isEmpty() || name.isEmpty() || serverUid.isEmpty() || serverUid.size() > 128)
        return {};
    // The folder is the server uid in base64: only base64 characters (no way out either).
    return QDir::cleanPath(configDir) + QLatin1String("/cache/") + QString::fromLatin1(serverUid.toUtf8().toBase64()) + QLatin1String("/clients/") + name;
}

QImage decode(const QString& path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || path.size() > 1024 || !info.isFile() || info.size() <= 0 || info.size() > kMaxBytes)
        return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true); // TeamSpeak's files have no extension
    const QSize source = reader.size();
    if (source.isValid() && (source.width() > kMaxSide || source.height() > kMaxSide))
        return {};
    if (source.isValid() && source.width() > 0 && source.height() > 0) {
        const qreal scale = qMax(kPixels / qreal(source.width()), kPixels / qreal(source.height()));
        if (scale < 1.0)
            reader.setScaledSize(QSize(qMax(1, qRound(source.width() * scale)), qMax(1, qRound(source.height() * scale))));
    }
    QImage image = reader.read();
    if (image.isNull() || image.width() > kMaxSide || image.height() > kMaxSide)
        return {};
    image = image.scaled(kPixels, kPixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect square((image.width() - kPixels) / 2, (image.height() - kPixels) / 2, kPixels, kPixels);
    return image.copy(square).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

namespace {

Found found(const QString& path, const QImage& image)
{
    Found f;
    f.image = image;
    f.file  = path;
    const QFileInfo info(path);
    f.modified = info.lastModified();
    f.size     = info.size();
    return f;
}

} // namespace

Found find(const Query& query)
{
    QStringList tried;
    for (const QString& path : query.paths) {
        if (path.isEmpty() || tried.contains(path))
            continue;
        tried << path;
        const QImage image = decode(path);
        if (!image.isNull())
            return found(path, image);
    }
    // Another server's copy of the same identity's avatar (newest first).
    if (query.configDir.isEmpty() || !query.fileName.startsWith(QLatin1String("avatar_")) || query.fileName.contains(QLatin1Char('/'))
        || query.fileName.contains(QLatin1Char('\\')))
        return {};
    const QDir                 cache(QDir::cleanPath(query.configDir) + QLatin1String("/cache"));
    const QFileInfoList        servers = cache.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort);
    QVector<QPair<QDateTime, QString>> copies;
    for (int i = 0; i < servers.size() && i < kMaxServers; ++i) {
        const QString   path = servers.at(i).absoluteFilePath() + QLatin1String("/clients/") + query.fileName;
        const QFileInfo info(path);
        if (!tried.contains(path) && info.isFile() && info.size() > 0 && info.size() <= kMaxBytes)
            copies.append({info.lastModified(), path});
    }
    std::sort(copies.begin(), copies.end(), [](const QPair<QDateTime, QString>& a, const QPair<QDateTime, QString>& b) { return a.first > b.first; });
    for (int i = 0; i < copies.size() && i < 8; ++i) {
        const QImage image = decode(copies.at(i).second);
        if (!image.isNull())
            return found(copies.at(i).second, image);
    }
    return {};
}

bool unchanged(const Found& f)
{
    if (f.file.isEmpty())
        return false;
    const QFileInfo info(f.file);
    return info.isFile() && info.size() == f.size && info.lastModified() == f.modified;
}

Query queryFor(const QString& teamSpeak, bool none, const QString& remembered, const QString& configDir, const QString& serverUid, const QString& uid)
{
    Query query;
    const QString name = cacheFileName(uid);
    if (none || name.isEmpty())
        return query;
    query.paths << teamSpeak << remembered << cachePath(configDir, serverUid, uid);
    query.paths.removeAll(QString());
    query.configDir = configDir;
    query.fileName  = name;
    return query;
}

QString fromTeamSpeak(quint64 sch, quint16 clientId, bool* none)
{
    if (none)
        *none = false;
    if (!sch || !clientId || !ts3::funcs.getAvatar)
        return {};
    char               path[1024] = {};
    const unsigned int error      = ts3::funcs.getAvatar(sch, clientId, path, sizeof(path));
    if (error == ERROR_database_empty_result && none)
        *none = true; // "the client simply has no avatar set" (the SDK)
    if (error != ERROR_ok)
        return {};
    path[sizeof(path) - 1] = '\0';
    const QString file = QString::fromUtf8(path);
    return file.size() <= 1000 ? QDir::cleanPath(file) : QString();
}

} // namespace chatavatars
