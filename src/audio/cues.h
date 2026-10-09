#pragma once

// 2.2 voice: the short sounds when recording starts and stops. 120 ms, 48 kHz mono 16-bit WAV at
// -18 dBFS with 10 ms fades: a rising 880 -> 1320 Hz glide to start, the same falling to stop. TeamSpeak
// plays them (playWaveFile) on its own playback device, so the user hears them where they hear the
// channel. Pure QtCore.

#include <QByteArray>

namespace voice {

enum class Cue { Start, Stop };

QByteArray cueWav(Cue cue);

constexpr int kCueMs = 120;

} // namespace voice
