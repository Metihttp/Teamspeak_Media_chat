#pragma once

// 2.4 compress: the one place that decides whether and how a video is made smaller (or converted to an
// MP4 that plays everywhere) before it is sent. Pure logic (QtCore + i18n texts), unit-tested in
// tests/tst_videocompress.cpp. Core asks it for every video it sends; the send window asks it for the
// entries of its Quality combo and their estimated sizes, so both always agree.
//
// Output is always H.264 (High) + AAC-LC in an MP4. Presets:
//   Smaller  480p, up to 30 fps, 1200 kbps
//   Balanced 720p, up to 60 fps, 2500 kbps (3750 above 30 fps)   (the default)
//   High    1080p, up to 60 fps, 5000 kbps (7500 above 30 fps)
// Audio: AAC 128 kbps stereo (96 kbps mono), none when the video has no sound.

#include <QSize>
#include <QString>
#include <QVector>

namespace videocompress {

// What the sender asked for: the settings decide (Auto), never compress (Original), or a preset.
enum class Request { Auto, Original, P1080, P720, P480 };

// What the planner knows about a video (from mf::probe of the file where it is).
struct VideoFacts {
    quint64 bytes      = 0;
    qint64  durationMs = 0;
    QSize   display;               // upright display size (pixel aspect and rotation applied)
    double  fps         = 0.0;     // 0: unknown
    bool    probed      = false;   // the container could be read
    bool    hasVideo    = false;
    bool    decodable   = false;   // a frame was decoded on this computer
    QString undecodableText;       // why not (mf's text, e.g. the HEVC Store extension sentence)
    bool    hasAudio      = false;
    int     audioChannels = 0;
    QString videoCodec;            // codecId(): "h264", "hevc", "vp9", "av1", ... empty if unknown
    QString extension;             // the file's extension, lower case ("mov", "mts")
    bool    is32Bit = sizeof(void*) == 4;
};

// The settings (a snapshot; Core takes it when the send starts) and the request of this send.
struct Options {
    bool    mediaFoundation  = true;  // Media Foundation can be used on this computer
    bool    compressLarge    = true;  // Settings::compressVideos
    quint64 thresholdBytes   = 25ull * 1024 * 1024;
    int     shortSide        = 720;   // Settings::compressVideoQuality: 480 | 720 | 1080
    bool    convertUnplayable = true; // Settings::convertUnplayableVideos
    quint64 limitBytes       = 100ull * 1024 * 1024; // the upload size limit (hard)
    Request request          = Request::Auto;
};

enum class Decision { SendOriginal, Compress, Fail };

enum class Reason {
    NotVideo,          // nothing to do (audio-only, not a video file)
    OriginalAsked,     // Request::Original
    Disabled,          // compression is off (and the video fits)
    SmallEnough,       // under the threshold and plays everywhere
    NotWorthIt,        // the result would save less than 30%
    UnknownLength,     // duration unreadable
    Undecodable,       // no decoder on this computer
    TooLarge32Bit,     // above 2560 x 1600 in 32-bit TeamSpeak
    NoMediaFoundation, // Windows N without the Media Feature Pack
    Shrink,            // Compress: over the threshold
    Convert,           // Compress: a format that may not play for others
    FitToLimit,        // Compress: over the upload limit, made to fit
    Chosen,            // Compress: the sender picked this preset
    CannotFit,         // Fail: even the lowest quality is over the limit
    DisabledOverLimit, // Fail: compression is off and the video is over the limit
};

struct Plan {
    Decision decision = Decision::SendOriginal;
    Reason   reason   = Reason::NotVideo;
    QSize    frameSize;          // even, upright; empty unless Compress
    int      fps            = 0; // the expected output rate (the source's, at most fpsCap)
    int      fpsCap         = 0; // the preset's cap: the transcoder drops frames above it (60 or 30)
    int      videoKbps      = 0;
    int      audioKbps      = 0; // 0 | 96 | 128
    int      audioChannels  = 0; // 0 | 1 | 2
    quint64  estimatedBytes = 0; // Compress: the expected output size
    QString  detail;             // Fail(Undecodable): the decoder text from the probe
};

// Limits the transcoder and the planner share.
constexpr int    kMaxOutputSide     = 4096;
constexpr int    kMaxFps            = 60;
constexpr int    kMinVideoKbps      = 250;
constexpr int    kMaxVideoKbps      = 15000;
constexpr double kFitTarget         = 0.92; // planned size as a share of the limit
constexpr double kOvershootAbort    = 1.03; // the transcoder stops above this share of its target
constexpr double kWorthItRatio      = 0.70; // Auto compresses only when the result is at most this share
constexpr double kSourceBitrateCap  = 0.80; // Shrink never uses more than this share of the source's video bitrate
constexpr int    kMax32BitPixels    = 2560 * 1600;

Plan planCompression(const VideoFacts& facts, const Options& options);

// display scaled so its short side is shortSide (never upscaled), both sides even, the long side at
// most kMaxOutputSide.
QSize scaledEven(const QSize& display, int shortSide);

// Files Core probes as possible candidates: the video kinds plus camcorder and phone formats
// (3gp, 3g2, mts, m2ts, mpg, mpeg). Not ".ts" (TypeScript files share it).
bool isCompressibleVideoName(const QString& fileName);

// A format that many receivers can't play inline: HEVC, AV1, VP8/VP9, MPEG-2, or a container that the
// plugin doesn't show as a video (mts, m2ts, 3gp, 3g2, mpg, mpeg).
bool mayNotPlayForOthers(const VideoFacts& facts);

// "hevc" for the readable codec names mfcommon uses ("HEVC (H.265)"), empty if unknown.
QString codecId(const QString& codecName);

// The size estimate for a plan (video + audio bitrate over the length, plus container overhead).
quint64 estimateBytes(int videoKbps, int audioKbps, qint64 durationMs);

// The text for a Fail plan (the toast and the chat warning): cause and fix. limitMB names the limit.
QString failureText(const Plan& plan, int limitMB);

// "720p" for a plan's frame size ("1080p", "480p", "360p" ...), empty when there is none.
QString resolutionLabel(const QSize& frameSize);

// When compressing itself failed (cause: "error 0xC00D36B4", "it stopped responding", ...):
// the original fits, so it is sent: "Couldn't compress this video (cause), so the original was sent."
QString compressFailedNote(const QString& cause);
// It doesn't fit: "Couldn't compress this video (cause), and the original is larger than your N MB upload limit."
QString compressFailedText(const QString& cause, int limitMB);
// "There isn't enough free disk space to compress this video (about 1.2 GB needed)."
QString diskSpaceText(quint64 neededBytes);

// ---- the send window's Quality combo -----------------------------------------------------------------

struct Choice {
    Request request = Request::Auto;
    Plan    plan;            // what this entry does
    QString label;           // "Original · 240 MB", "720p · about 18 MB"
    QString longLabel;       // "Original (240 MB)", "Balanced (720p), about 18 MB" (the single item view)
    bool    enabled   = true;  // false: Original over the limit (shown, but can't be picked)
    bool    selected  = false; // the default entry
    bool    automatic = false; // does what Auto does (Core's own planning gives the same result)
};

// Original first, then each preset that the planner can do as asked (largest first; presets that would
// give the same frame size are listed once), each with its estimate. Exactly one enabled entry is
// selected: the planner's own Auto decision. Empty when there is nothing to choose (not a video,
// unknown length, undecodable, no Media Foundation).
QVector<Choice> choices(const VideoFacts& facts, const Options& options);

// What a choice means for the send: the request to pass to Core. Auto for an entry that does what Auto
// does, so a send that keeps the default behaves like a drop or a paste.
Request requestFor(const QVector<Choice>& choices, int index);

} // namespace videocompress
