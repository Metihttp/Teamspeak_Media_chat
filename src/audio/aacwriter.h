#pragma once

// 2.2 voice: a recording as an .m4a file: AAC-LC, mono, 96 kbps (the lowest rate Windows' own AAC
// encoder offers, about 0.73 MB a minute), through mf::detail's MPEG-4 sink writer (mfreadwrite.dll,
// delay-loaded like the rest of Media Foundation). Five minutes encode in about a second.
//
// Runs on the calling thread, which must not be the GUI thread for long recordings: it sets up its own
// COM (multithreaded) and Media Foundation scopes. Checks Media Foundation is present first.

#include <QString>

#include <atomic>
#include <cstdint>

namespace voice {

struct AacResult {
    bool    ok       = false;
    bool    canceled = false;
    long    code     = 0; // HRESULT of the failed step
    QString error;        // for the log (English, no names)
};

// Encodes `frames` 16-bit mono samples at sampleRate (44100 or 48000) to path (replaced if it exists),
// each sample multiplied by gain (waveform::normalizeGain; 1 = as recorded). cancel (optional) is
// checked between 100 ms samples; a canceled or failed run deletes the partial file. writtenMs
// (optional) is set to how much of the sound has been handed to the writer after each sample.
AacResult writeAac(const QString& path, const int16_t* samples, qint64 frames, int sampleRate, double gain, const std::atomic<bool>* cancel = nullptr,
                   std::atomic<qint64>* writtenMs = nullptr);

} // namespace voice
