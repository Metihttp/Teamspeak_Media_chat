#pragma once

// Rules for inline audio (2.2) that do not need Media Foundation or a widget: when an audio file is
// downloaded without a click. Pure logic, unit-tested (tests/tst_audio.cpp).

#include <QtGlobal>

struct Settings;

namespace audioplayback {

// Voice messages are small: they download automatically like images, but never beyond this.
constexpr int kVoiceAutoDownloadMaxMB = 16;

// The largest size (bytes) an audio file is downloaded at automatically when it appears in a chat;
// 0 = only when the user presses play.
//  * Plain audio files follow the video rule: Settings::videoAutoDownloadMB (0 = when played).
//  * Voice messages (the link's vm flag, from 2.2 on): with "download images automatically" on, up
//    to min(autoDownloadMaxMB, kVoiceAutoDownloadMaxMB); otherwise when played.
// The size in a link is untrusted: Core still stops an automatic download whose real size is larger
// (abortOversizedAutoDownload). Data saver and per-server settings override this rule.
quint64 autoDownloadLimit(const Settings& settings, bool voice);

// Whether a file of linkSize bytes starts downloading on its own under that rule (an unknown size,
// 0, counts as small: the real size is checked once the transfer starts).
bool downloadsAutomatically(const Settings& settings, bool voice, quint64 linkSize);

} // namespace audioplayback
