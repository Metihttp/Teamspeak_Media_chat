#pragma once

// 2.3 live music: the arithmetic of mixing music into TeamSpeak's voice. Pure functions, no allocation,
// safe on TeamSpeak's audio threads; unit-tested (tests/tst_music.cpp).
//
// TeamSpeak's voice data is 16-bit at 48 kHz, frames of `channels` interleaved samples ("sampleCount" in
// its callbacks is the number of frames). The music is 48 kHz interleaved stereo float (mf::MusicReader).

#include <QtGlobal>

namespace music {

constexpr int kRate     = 48000;
constexpr int kChannels = 2;

// ts3plugin_onEditCapturedVoiceDataEvent's *edited, as TeamSpeak's client SDK documents it:
//   value 1  set by the plugin when it changed the samples;
//   value 2  on entry: TeamSpeak is going to send these samples (voice activation heard speech, or the
//            input is simply open); set by the plugin to have them sent anyway.
constexpr int kEditedChanged = 1;
constexpr int kEditedSend    = 2;

struct CaptureDecision {
    bool  mix     = false; // the music goes into these samples
    bool  silence = false; // the samples are cleared and not sent (no music, and the voice isn't allowed)
    float micGain = 1.0f;  // the microphone's share: 1 while TeamSpeak would send the voice anyway
    int   edited  = 0;     // the new *edited
};

// What happens to one block of captured voice. edited: *edited on entry; music: there is music for this
// block; voiceAllowed: false while the plugin itself keeps a push-to-talk input open (then nothing of the
// microphone goes out). With music the block is always sent (value 2), and the voice only where TeamSpeak
// would have sent it (value 2 or 1 on entry): between words (voice activation said "silence") the
// microphone is left out, so the room isn't sent along with the music. Without music and with the voice
// allowed nothing changes. Without music while the voice isn't allowed (the decoder not ready yet, a seek,
// the moment after a pause or the track's end, before the input is given back) the block is silence and
// not sent: TeamSpeak's input is open only because of the plugin, so its microphone never goes out alone.
CaptureDecision decideCapture(int edited, bool music, bool voiceAllowed);

// Above kKnee a sample is bent smoothly towards full scale instead of being cut off: loud music plus
// voice never wraps or clips hard. Odd (softClip(-x) == -softClip(x)), monotonic, |result| < 1.
constexpr float kKnee = 0.75f;
float softClip(float x);
qint16 toPcm16(float x); // rounded, clamped to -32768..32767

// Volume 0..100 (the bar's slider) as a gain: quadratic, so the low half is usable (50 = -12 dB).
float volumeGain(int percent);

// Captured voice (frames x channels, interleaved) with the first musicFrames frames of music mixed in at
// musicGain. The microphone's gain moves from micFrom to micTo across the block (no click when voice
// activation opens or closes). Mono: the music's average; stereo: left and right; more channels: left,
// right, then the average.
void mixCapture(qint16* samples, int frames, int channels, const float* music, int musicFrames, float musicGain, float micFrom, float micTo);

// TeamSpeak's mixed playback (what this computer plays) with the music added at gain. speakers: each
// channel's SPEAKER_* bits (may be null: mono, or left / right); fillMask: bit c set means channel c
// already holds sound, otherwise its samples are undefined and are treated as silence. The music goes to
// the front (or headphone) left and right channels, or as mono to the centre / single channel; each
// channel written gets its bit in fillMask.
void mixPlayback(qint16* samples, int frames, int channels, const unsigned* speakers, unsigned* fillMask, const float* music, int musicFrames, float gain);

// Fades frames of interleaved stereo music in (from 0 to 1) or out (1 to 0) across the block: the first
// block after a start, a seek or a resume, and the last one before a pause.
void fade(float* music, int frames, bool in);

// Scales frames of interleaved stereo music by a gain moving from `from` to `to` across the block (2.3
// review: a volume change, mute or unmute, and TeamSpeak's sound muted, never step: no click).
void ramp(float* music, int frames, float from, float to);

} // namespace music
