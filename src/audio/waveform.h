#pragma once

// 2.2 voice: the numbers behind a voice message's waveform and level meter. Pure QtCore (no Windows,
// no Media Foundation), shared by the recorder, the recorder window, the chat's voice card and the
// unit tests.
//
// The recorder keeps one peak value per 50 ms of sound ("bins", in dBFS). When the recording stops,
// they become the link's waveform: MediaLink::kWaveformLevels levels of 0..MediaLink::kWaveformMax
// (sent as wf=, 43 base64url characters; MediaLink packs them). Receivers resample those levels to as
// many bars as their card has room for.

#include <QByteArray>
#include <QVector>

#include <cstdint>

namespace waveform {

constexpr int   kBinMs       = 50;     // one peak value per 50 ms of recording
constexpr float kSilenceDb   = -96.0f; // the floor: what digital silence reads as
constexpr float kFloorDb     = -45.0f; // the quietest level a bar shows (level 0 below it)
constexpr float kQuietDb     = -50.0f; // "No sound from the microphone" when everything stays below this
constexpr float kNormalizeTo = -1.0f;  // peak normalisation target before encoding (dBFS)
constexpr float kMaxGainDb   = 12.0f;  // ... never more gain than this
constexpr float kNormalizeIf = -6.0f;  // ... and only when the loudest peak is below this

// The peak of 16-bit samples in dBFS (kSilenceDb for silence or no samples).
float peakDb(const int16_t* samples, int count);

// The link's waveform from the 50 ms peak bins: kWaveformLevels equal buckets over the recording
// (the loudest bin in each), each mapped to 0..15 from kFloorDb..0 dBFS; when the loudest level is
// 4..14 every level is scaled up so the loudest is 15 (quiet recordings still show their shape).
// Fewer bins than levels: bins are repeated (nearest). No bins: kWaveformLevels zeros.
QByteArray levelsFromBins(const QVector<float>& binsDb);

// levels resampled to `bars` bars: the loudest of the levels a bar covers when there are fewer bars
// (max pooling), the nearest level when there are more. Values are clamped to 0..15; an empty input
// gives `bars` zeros (a flat line).
QVector<quint8> resample(const QByteArray& levels, int bars);

// A bin's height for the live view while recording: 0 (silent) .. 1 (full scale).
qreal liveHeight(float db);

// Gain (linear factor, >= 1) for peak normalisation before encoding: lifts a quiet recording so its
// loudest peak reaches kNormalizeTo, by at most kMaxGainDb; 1 when the peak is already at kNormalizeIf
// or louder, or for silence.
double normalizeGain(float peakDbfs);

// One sample with that gain, rounded and saturated to 16 bits.
int16_t applyGain(int16_t sample, double gain);

} // namespace waveform
