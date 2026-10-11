#include "updatepolicy.h"

namespace upd {

namespace {

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

#ifdef TSMEDIA_TESTHOOKS
QString& testOriginStorage()
{
    static QString origin; // only ever holds Qt-allocated data (fromLatin1 / user input)
    return origin;
}
#endif

QString origin()
{
#ifdef TSMEDIA_TESTHOOKS
    if (!testOriginStorage().isEmpty())
        return testOriginStorage();
#endif
    return latin("https://github.com");
}

QUrl repoUrl(const QString& suffix)
{
    return QUrl(origin() + QLatin1Char('/') + latin(kRepository) + suffix, QUrl::StrictMode);
}

bool isDnsLabel(const QString& label)
{
    if (label.isEmpty() || label.size() > 63 || label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-')))
        return false;
    for (QChar c : label) {
        const ushort u = c.unicode();
        if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-'))
            return false;
    }
    return true;
}

} // namespace

QUrl manifestUrl()
{
    return repoUrl(latin("/releases/latest/download/") + latin(kManifestName));
}

QUrl legacyManifestUrl()
{
    return repoUrl(latin("/releases/latest/download/") + latin(kLegacyManifestName));
}

QUrl assetUrl(const QString& tag, const QString& asset)
{
    return repoUrl(latin("/releases/download/") + tag + QLatin1Char('/') + asset);
}

QUrl releaseNotesUrl(const QString& tag)
{
    return QUrl(latin("https://github.com/") + latin(kRepository) + latin("/releases/tag/") + tag, QUrl::StrictMode);
}

QUrl latestReleaseUrl()
{
    return QUrl(latin("https://github.com/") + latin(kRepository) + latin("/releases/latest"), QUrl::StrictMode);
}

bool isAllowedUrl(const QUrl& url)
{
    if (!url.isValid() || url.isRelative() || !url.userName().isEmpty() || !url.password().isEmpty())
        return false;
#ifdef TSMEDIA_TESTHOOKS
    if (!testOriginStorage().isEmpty()) {
        const QUrl test(testOriginStorage());
        return url.scheme() == test.scheme() && url.host() == test.host() && url.port() == test.port();
    }
#endif
    if (url.scheme() != QLatin1String("https"))
        return false;
    if (url.port() != -1 && url.port() != 443)
        return false;
    const QString host = url.host(QUrl::FullyEncoded);
    if (host == QLatin1String("github.com"))
        return true;
    const QString suffix = latin(".githubusercontent.com");
    if (!host.endsWith(suffix) || host.size() == suffix.size())
        return false;
    const QStringList labels = host.left(host.size() - suffix.size()).split(QLatin1Char('.'));
    for (const QString& label : labels) {
        if (!isDnsLabel(label))
            return false;
    }
    return true;
}

#ifdef TSMEDIA_TESTHOOKS
bool setTestOrigin(const QString& text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    const QString host = url.host();
    const bool    loopback = host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1");
    if (!url.isValid() || url.scheme() != QLatin1String("http") || !loopback || url.port() <= 0 || !url.path().isEmpty()
        || !url.userInfo().isEmpty()) {
        testOriginStorage().clear();
        return false;
    }
    testOriginStorage() = url.toString(QUrl::FullyEncoded);
    return true;
}

QString testOrigin()
{
    return testOriginStorage();
}
#endif

qint64 initialDelayMs(quint32 rand)
{
    constexpr qint64 minMs  = 3 * 60 * 1000;
    constexpr qint64 spanMs = 5 * 60 * 1000;
    return minMs + static_cast<qint64>(rand % static_cast<quint32>(spanMs + 1));
}

QDateTime nextAfterSuccess(const QDateTime& now, quint32 rand)
{
    constexpr qint64 jitterSec = 3 * 3600;
    return now.addSecs(24 * 3600 + static_cast<qint64>(rand % static_cast<quint32>(jitterSec + 1)));
}

QDateTime nextAfterFailure(const QDateTime& now, int failures, qint64 retryAfterSec)
{
    static constexpr qint64 kHours[] = {1, 3, 6, 12, 24};
    const int               index    = qBound(0, failures - 1, 4);
    qint64                  delay    = kHours[index] * 3600;
    delay                            = qMax(delay, qBound<qint64>(0, retryAfterSec, kMaxRetryAfterSec));
    return now.addSecs(delay);
}

QDateTime sanitizeNext(const QDateTime& next, const QDateTime& now)
{
    if (!next.isValid())
        return now;
    if (next > now.addSecs(48 * 3600))
        return now.addSecs(24 * 3600);
    return next;
}

bool shouldAskConsent(CheckSetting setting, int timesAsked)
{
    return setting == CheckSetting::NotAsked && timesAsked < kMaxConsentAsks;
}

qint64 downloadDeadlineMs(qint64 bytes)
{
    const qint64 atSlowRate = bytes / 16; // 16 KB/s = 16 bytes per ms
    return qMax<qint64>(120 * 1000, atSlowRate);
}

QStringList relaunchFlags(const QStringList& originalArguments)
{
    static constexpr const char* kAllowed[] = {"-nosingleinstance", "-silentstart", "-nohotkeys", "-console"};
    QStringList result;
    for (const QString& argument : originalArguments) {
        const QString lower = argument.trimmed().toLower();
        for (const char* allowed : kAllowed) {
            if (lower == QLatin1String(allowed) && !result.contains(lower))
                result.append(lower);
        }
    }
    return result;
}

} // namespace upd
