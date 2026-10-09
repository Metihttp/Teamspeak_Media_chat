#include "audioplayback.h"

#include "settings.h"

namespace audioplayback {

namespace {

quint64 megabytes(int mb)
{
    return static_cast<quint64>(qMax(0, mb)) * 1024 * 1024;
}

} // namespace

quint64 autoDownloadLimit(const Settings& settings, bool voice)
{
    if (voice)
        return settings.autoDownloadImages ? megabytes(qMin(settings.autoDownloadMaxMB, kVoiceAutoDownloadMaxMB)) : 0;
    return megabytes(settings.videoAutoDownloadMB);
}

bool downloadsAutomatically(const Settings& settings, bool voice, quint64 linkSize)
{
    const quint64 limit = autoDownloadLimit(settings, voice);
    return limit > 0 && linkSize <= limit;
}

} // namespace audioplayback
