#include "audio/waveform.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

#include "medialink.h" // MediaLink::kWaveformLevels / kWaveformMax (constants only)

namespace waveform {

namespace {

constexpr int kLevels   = MediaLink::kWaveformLevels;
constexpr int kMaxLevel = MediaLink::kWaveformMax;

int levelFor(float db)
{
    const double t = qBound(0.0, (static_cast<double>(db) - kFloorDb) / -static_cast<double>(kFloorDb), 1.0);
    return static_cast<int>(std::lround(t * kMaxLevel));
}

} // namespace

float peakDb(const int16_t* samples, int count)
{
    if (!samples || count <= 0)
        return kSilenceDb;
    int peak = 0;
    for (int i = 0; i < count; ++i) {
        const int v = samples[i] < 0 ? -static_cast<int>(samples[i]) : samples[i];
        if (v > peak)
            peak = v;
    }
    if (peak == 0)
        return kSilenceDb;
    const double db = 20.0 * std::log10(static_cast<double>(peak) / 32768.0);
    return static_cast<float>(qMax(static_cast<double>(kSilenceDb), db));
}

QByteArray levelsFromBins(const QVector<float>& binsDb)
{
    QByteArray levels(kLevels, '\0');
    const int  bins = binsDb.size();
    if (bins == 0)
        return levels;
    int loudest = 0;
    for (int i = 0; i < kLevels; ++i) {
        float db = kSilenceDb;
        if (bins >= kLevels) {
            // Equal buckets over the whole recording; each bin belongs to exactly one bucket.
            const int from = static_cast<int>(static_cast<qint64>(i) * bins / kLevels);
            const int to   = static_cast<int>(static_cast<qint64>(i + 1) * bins / kLevels);
            for (int b = from; b < to; ++b)
                db = qMax(db, binsDb.at(b));
        } else {
            db = binsDb.at(static_cast<int>(static_cast<qint64>(i) * bins / kLevels));
        }
        const int level = levelFor(db);
        levels[i]       = static_cast<char>(level);
        loudest         = qMax(loudest, level);
    }
    if (loudest >= 4 && loudest < kMaxLevel) {
        for (int i = 0; i < kLevels; ++i) {
            const int scaled = static_cast<int>(std::lround(static_cast<double>(static_cast<uchar>(levels.at(i))) * kMaxLevel / loudest));
            levels[i]        = static_cast<char>(qBound(0, scaled, kMaxLevel));
        }
    }
    return levels;
}

QVector<quint8> resample(const QByteArray& levels, int bars)
{
    QVector<quint8> out;
    if (bars <= 0)
        return out;
    out.fill(0, bars);
    const int n = levels.size();
    if (n == 0)
        return out;
    auto at = [&levels](int i) { return static_cast<quint8>(qBound(0, static_cast<int>(static_cast<uchar>(levels.at(i))), kMaxLevel)); };
    for (int b = 0; b < bars; ++b) {
        if (bars <= n) {
            const int from = static_cast<int>(static_cast<qint64>(b) * n / bars);
            const int to   = qMax(from + 1, static_cast<int>(static_cast<qint64>(b + 1) * n / bars));
            quint8    v    = 0;
            for (int i = from; i < to && i < n; ++i)
                v = qMax(v, at(i));
            out[b] = v;
        } else {
            // Nearest level: the bar's centre mapped onto the levels.
            const int i = qBound(0, static_cast<int>((static_cast<qint64>(2 * b + 1) * n) / (2 * static_cast<qint64>(bars))), n - 1);
            out[b]      = at(i);
        }
    }
    return out;
}

qreal liveHeight(float db)
{
    return qBound(0.0, (static_cast<double>(db) - kFloorDb) / -static_cast<double>(kFloorDb), 1.0);
}

double normalizeGain(float peakDbfs)
{
    if (peakDbfs <= kSilenceDb || peakDbfs >= kNormalizeIf)
        return 1.0;
    const double gainDb = qMin(static_cast<double>(kMaxGainDb), static_cast<double>(kNormalizeTo) - peakDbfs);
    return gainDb > 0.0 ? std::pow(10.0, gainDb / 20.0) : 1.0;
}

int16_t applyGain(int16_t sample, double gain)
{
    const long v = std::lround(static_cast<double>(sample) * gain);
    return static_cast<int16_t>(std::clamp(v, -32768L, 32767L));
}

} // namespace waveform
