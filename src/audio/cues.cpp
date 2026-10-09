#include "audio/cues.h"

#include <QtEndian>
#include <QtGlobal>

#include <cmath>

namespace voice {

namespace {

void put32(QByteArray& out, quint32 v)
{
    char b[4];
    qToLittleEndian(v, b);
    out.append(b, 4);
}

void put16(QByteArray& out, quint16 v)
{
    char b[2];
    qToLittleEndian(v, b);
    out.append(b, 2);
}

} // namespace

QByteArray cueWav(Cue cue)
{
    constexpr int    kRate   = 48000;
    constexpr int    kFrames = kRate * kCueMs / 1000;
    constexpr int    kFade   = kRate / 100; // 10 ms
    const double     amplitude = 32767.0 * std::pow(10.0, -18.0 / 20.0);
    const double     from      = cue == Cue::Start ? 880.0 : 1320.0;
    const double     to        = cue == Cue::Start ? 1320.0 : 880.0;
    const quint32    dataBytes = kFrames * 2;

    QByteArray out;
    out.reserve(44 + static_cast<int>(dataBytes));
    out.append("RIFF", 4);
    put32(out, 36 + dataBytes);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    put32(out, 16);
    put16(out, 1); // PCM
    put16(out, 1); // mono
    put32(out, kRate);
    put32(out, kRate * 2);
    put16(out, 2);
    put16(out, 16);
    out.append("data", 4);
    put32(out, dataBytes);

    double phase = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const double t    = static_cast<double>(i) / (kFrames - 1);
        const double freq = from + (to - from) * t;
        phase += 2.0 * 3.14159265358979323846 * freq / kRate;
        double gain = 1.0;
        if (i < kFade)
            gain = static_cast<double>(i) / kFade;
        else if (i >= kFrames - kFade)
            gain = static_cast<double>(kFrames - 1 - i) / kFade;
        const auto sample = static_cast<qint16>(std::lround(amplitude * gain * std::sin(phase)));
        put16(out, static_cast<quint16>(sample));
    }
    return out;
}

} // namespace voice
