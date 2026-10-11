#include "music/musicmix.h"

#include <cmath>

#include "teamspeak/public_definitions.h" // SPEAKER_*

namespace music {

CaptureDecision decideCapture(int edited, bool music, bool voiceAllowed)
{
    CaptureDecision d;
    d.edited = edited;
    if (!music && !voiceAllowed) { // the plugin keeps the input open: nothing of the microphone, nothing sent
        d.silence = true;
        d.micGain = 0.0f;
        d.edited  = (edited | kEditedChanged) & ~kEditedSend;
        return d;
    }
    if (!music)
        return d; // TeamSpeak's own decision, untouched
    // TeamSpeak would send the voice: value 2 as documented. Value 1 counts too (another plugin changed
    // the samples, or a client that reads the bits the other way round): the voice is never cut off
    // because of how the bits are read; at worst the room between words goes along with the music.
    const bool talking = (edited & (kEditedSend | kEditedChanged)) != 0;
    d.mix              = true;
    d.micGain          = talking && voiceAllowed ? 1.0f : 0.0f;
    d.edited           = edited | kEditedChanged | kEditedSend;
    return d;
}

float softClip(float x)
{
    if (!std::isfinite(x))
        return 0.0f;
    const float a = std::fabs(x);
    if (a <= kKnee)
        return x;
    const float room = 1.0f - kKnee;
    const float y    = kKnee + room * std::tanh((a - kKnee) / room) * 0.9999f;
    return x < 0.0f ? -y : y;
}

qint16 toPcm16(float x)
{
    const float scaled = std::nearbyint(x * 32768.0f);
    if (!(scaled > -32768.0f)) // also NaN
        return -32768;
    if (scaled > 32767.0f)
        return 32767;
    return static_cast<qint16>(scaled);
}

float volumeGain(int percent)
{
    const float p = static_cast<float>(qBound(0, percent, 100)) / 100.0f;
    return p * p;
}

void mixCapture(qint16* samples, int frames, int channels, const float* music, int musicFrames, float musicGain, float micFrom, float micTo)
{
    if (!samples || frames <= 0 || channels <= 0)
        return;
    musicFrames = music ? qBound(0, musicFrames, frames) : 0;
    for (int f = 0; f < frames; ++f) {
        const float t   = frames > 1 ? static_cast<float>(f) / static_cast<float>(frames - 1) : 1.0f;
        const float mic = micFrom + (micTo - micFrom) * t;
        float       l = 0.0f, r = 0.0f;
        if (f < musicFrames) {
            l = music[2 * f] * musicGain;
            r = music[2 * f + 1] * musicGain;
        }
        const float mono = (l + r) * 0.5f;
        for (int c = 0; c < channels; ++c) {
            const float m = channels == 1 ? mono : c == 0 ? l : c == 1 ? r : mono;
            qint16&     s = samples[f * channels + c];
            s             = toPcm16(softClip(static_cast<float>(s) / 32768.0f * mic + m));
        }
    }
}

void mixPlayback(qint16* samples, int frames, int channels, const unsigned* speakers, unsigned* fillMask, const float* music, int musicFrames, float gain)
{
    if (!samples || !music || frames <= 0 || channels <= 0 || musicFrames <= 0)
        return;
    musicFrames = qMin(musicFrames, frames);
    enum Route : unsigned char { None, Left, Right, Mono };
    constexpr int kMaxRouted = 32; // fillMask has 32 bits
    const int     routed     = qMin(channels, kMaxRouted);
    Route         route[kMaxRouted];
    int           left = -1, right = -1, mono = -1;
    for (int c = 0; c < routed; ++c) {
        unsigned speaker = speakers ? speakers[c] : (channels == 1 ? SPEAKER_MONO : c == 0 ? SPEAKER_FRONT_LEFT : c == 1 ? SPEAKER_FRONT_RIGHT : 0u);
        route[c]         = None;
        if (speaker & (SPEAKER_FRONT_LEFT | SPEAKER_HEADPHONES_LEFT)) {
            route[c] = Left;
            if (left < 0)
                left = c;
        } else if (speaker & (SPEAKER_FRONT_RIGHT | SPEAKER_HEADPHONES_RIGHT)) {
            route[c] = Right;
            if (right < 0)
                right = c;
        } else if ((speaker & (SPEAKER_MONO | SPEAKER_FRONT_CENTER)) && mono < 0) {
            mono = c;
        }
    }
    if (left < 0 && right < 0) { // no stereo pair: the music as mono, to the centre or the only channel
        if (mono < 0)
            mono = 0;
        route[mono] = Mono;
    } else if (left < 0 || right < 0) { // half a pair: that side gets both
        route[left >= 0 ? left : right] = Mono;
    }
    for (int c = 0; c < routed; ++c) {
        if (route[c] == None)
            continue;
        const bool filled = !fillMask || (*fillMask & (1u << c));
        for (int f = 0; f < frames; ++f) {
            qint16&     s    = samples[f * channels + c];
            const float have = filled ? static_cast<float>(s) / 32768.0f : 0.0f;
            float       m    = 0.0f;
            if (f < musicFrames) {
                const float l = music[2 * f], r = music[2 * f + 1];
                m             = (route[c] == Left ? l : route[c] == Right ? r : (l + r) * 0.5f) * gain;
            }
            s = toPcm16(softClip(have + m));
        }
        if (fillMask)
            *fillMask |= 1u << c;
    }
}

void fade(float* music, int frames, bool in)
{
    if (!music || frames <= 0)
        return;
    for (int f = 0; f < frames; ++f) {
        const float t = frames > 1 ? static_cast<float>(f) / static_cast<float>(frames - 1) : 1.0f;
        const float g = in ? t : 1.0f - t;
        music[2 * f] *= g;
        music[2 * f + 1] *= g;
    }
}

void ramp(float* music, int frames, float from, float to)
{
    if (!music || frames <= 0)
        return;
    for (int f = 0; f < frames; ++f) {
        const float t = frames > 1 ? static_cast<float>(f) / static_cast<float>(frames - 1) : 1.0f;
        const float g = from + (to - from) * t;
        music[2 * f] *= g;
        music[2 * f + 1] *= g;
    }
}

} // namespace music
