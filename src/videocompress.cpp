#include "videocompress.h"

#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "medialink.h"

namespace videocompress {

namespace {

constexpr quint64 kMiB               = 1024ull * 1024;
constexpr quint64 kContainerOverhead = 64ull * 1024; // MP4 header and index
constexpr double  kMuxOverhead       = 1.01;          // per-sample framing

// One rung of the quality ladder. The presets are the first three; 360p is only used to make a long
// video fit the upload limit.
struct Step {
    int shortSide;
    int fpsCap;
    int kbps;         // up to 30 fps
    int kbpsHighFps;  // above 30 fps
    int floorKbps;    // below this the picture falls apart: try a smaller size instead
};

constexpr Step kSteps[] = {
    {1080, 60, 5000, 7500, 1600},
    {720, 60, 2500, 3750, 800},
    {480, 30, 1200, 1200, 400},
    {360, 30, 700, 700, 250},
};

const Step& stepFor(int shortSide)
{
    for (const Step& step : kSteps) {
        if (shortSide >= step.shortSide)
            return step;
    }
    return kSteps[3];
}

int presetShortSide(Request request, int settingsShortSide)
{
    switch (request) {
    case Request::P1080:
        return 1080;
    case Request::P720:
        return 720;
    case Request::P480:
        return 480;
    default:
        return settingsShortSide == 480 || settingsShortSide == 1080 ? settingsShortSide : 720;
    }
}

bool isPreset(Request request)
{
    return request == Request::P1080 || request == Request::P720 || request == Request::P480;
}

int shortSideOf(const QSize& size)
{
    return qMin(size.width(), size.height());
}

int outputFps(double sourceFps, int cap)
{
    if (!(sourceFps > 0.0) || !std::isfinite(sourceFps))
        return qMin(30, cap);
    return qBound(1, qMin(static_cast<int>(std::lround(sourceFps)), cap), kMaxFps);
}

// The floor for an output frame: by its real short side (a small source stays small).
int floorKbps(const QSize& frame)
{
    return stepFor(shortSideOf(frame)).floorKbps;
}

// The step's bitrate for this output: the higher rate above 30 fps, scaled down when the source is
// smaller than the step (a 640 x 360 clip doesn't need the bitrate of 1280 x 720).
int stepKbps(const Step& step, const QSize& frame, int fps)
{
    const int    base    = fps > 30 ? step.kbpsHighFps : step.kbps;
    const double nominal = static_cast<double>(step.shortSide) * step.shortSide * 16.0 / 9.0;
    const double pixels  = static_cast<double>(frame.width()) * frame.height();
    int          kbps    = base;
    if (pixels > 0.0 && pixels < nominal)
        kbps = static_cast<int>(std::lround(base * pixels / nominal));
    return qBound(kMinVideoKbps, qMax(kbps, floorKbps(frame)), kMaxVideoKbps);
}

// The source's video bitrate, from its size and length (the sound counted at our own audio rate).
double sourceVideoKbps(const VideoFacts& facts, int audioKbps)
{
    if (facts.durationMs <= 0)
        return 0.0;
    const double total = static_cast<double>(facts.bytes) * 8.0 / (static_cast<double>(facts.durationMs) / 1000.0) / 1000.0;
    return qMax(0.0, total - (facts.hasAudio ? audioKbps : 0));
}

// The highest video bitrate whose estimate stays within kFitTarget of the limit.
double budgetKbps(quint64 limitBytes, qint64 durationMs, int audioKbps)
{
    const double target = static_cast<double>(limitBytes) * kFitTarget - static_cast<double>(kContainerOverhead);
    if (target <= 0.0 || durationMs <= 0)
        return 0.0;
    return target / kMuxOverhead * 8.0 / (static_cast<double>(durationMs) / 1000.0) / 1000.0 - audioKbps;
}

Plan sendOriginal(Reason reason)
{
    Plan plan;
    plan.decision = Decision::SendOriginal;
    plan.reason   = reason;
    return plan;
}

Plan fail(Reason reason, const QString& detail = {})
{
    Plan plan;
    plan.decision = Decision::Fail;
    plan.reason   = reason;
    plan.detail   = detail;
    return plan;
}

// SendOriginal when the original fits the limit, otherwise Fail for the same reason.
Plan originalOrFail(bool over, Reason reason, const QString& detail = {})
{
    return over ? fail(reason, detail) : sendOriginal(reason);
}

void setAudio(Plan& plan, const VideoFacts& facts)
{
    if (!facts.hasAudio) {
        plan.audioKbps     = 0;
        plan.audioChannels = 0;
        return;
    }
    const bool mono    = facts.audioChannels == 1;
    plan.audioChannels = mono ? 1 : 2;
    plan.audioKbps     = mono ? 96 : 128;
}

// "about 18 MB": whole megabytes from 10 MB, one decimal below.
QString approxSize(quint64 bytes)
{
    const double mb = static_cast<double>(bytes) / static_cast<double>(kMiB);
    if (mb >= 10.0)
        return i18n::t("%1 MB").arg(static_cast<qint64>(std::lround(mb)));
    if (mb >= 1.0)
        return i18n::t("%1 MB").arg(mb, 0, 'f', 1);
    return formatSize(bytes);
}

QString presetName(Request request, const QSize& frame)
{
    switch (request) {
    case Request::P1080:
        return i18n::t("High");
    case Request::P720:
        return i18n::t("Balanced");
    case Request::P480:
        return i18n::t("Smaller");
    default:
        return shortSideOf(frame) >= 1080 ? i18n::t("High") : shortSideOf(frame) >= 720 ? i18n::t("Balanced") : i18n::t("Smaller");
    }
}

bool samePlan(const Plan& a, const Plan& b)
{
    return a.decision == b.decision && a.frameSize == b.frameSize && a.fps == b.fps && a.videoKbps == b.videoKbps && a.audioKbps == b.audioKbps;
}

} // namespace

QSize scaledEven(const QSize& display, int shortSide)
{
    if (display.width() <= 0 || display.height() <= 0)
        return {};
    const int    srcShort = shortSideOf(display);
    const int    srcLong  = qMax(display.width(), display.height());
    double       scale    = srcShort > shortSide && shortSide > 0 ? static_cast<double>(shortSide) / srcShort : 1.0;
    if (srcLong * scale > kMaxOutputSide)
        scale = static_cast<double>(kMaxOutputSide) / srcLong;
    // Even sides (4:2:0), at least 2; rounded to the nearest even number.
    const auto even = [](double value) { return qMax(2, static_cast<int>(std::lround(value / 2.0)) * 2); };
    QSize      out(even(display.width() * scale), even(display.height() * scale));
    out.setWidth(qMin(out.width(), kMaxOutputSide));
    out.setHeight(qMin(out.height(), kMaxOutputSide));
    return out;
}

bool isCompressibleVideoName(const QString& fileName)
{
    static const char* const extensions[] = {"mp4", "m4v", "mov", "webm", "mkv", "avi", "wmv", "3gp", "3g2", "mts", "m2ts", "mpg", "mpeg"};
    const QString            ext          = QFileInfo(fileName).suffix().toLower();
    for (const char* known : extensions) {
        if (ext == QLatin1String(known))
            return true;
    }
    return false;
}

QString codecId(const QString& codecName)
{
    const QString name = codecName.toLower();
    if (name.isEmpty())
        return {};
    if (name.startsWith(QLatin1String("h.264")) || name == QLatin1String("h264"))
        return QString::fromLatin1("h264");
    if (name.startsWith(QLatin1String("hevc")) || name.contains(QLatin1String("h.265")))
        return QString::fromLatin1("hevc");
    if (name == QLatin1String("vp9"))
        return QString::fromLatin1("vp9");
    if (name == QLatin1String("vp8"))
        return QString::fromLatin1("vp8");
    if (name == QLatin1String("av1"))
        return QString::fromLatin1("av1");
    if (name == QLatin1String("mpeg-2"))
        return QString::fromLatin1("mpeg2");
    if (name.startsWith(QLatin1String("mpeg-4")))
        return QString::fromLatin1("mpeg4");
    if (name.startsWith(QLatin1String("wmv")))
        return QString::fromLatin1("wmv");
    if (name == QLatin1String("vc-1"))
        return QString::fromLatin1("vc1");
    if (name == QLatin1String("motion jpeg"))
        return QString::fromLatin1("mjpeg");
    return QString::fromLatin1("other");
}

bool mayNotPlayForOthers(const VideoFacts& facts)
{
    static const char* const codecs[]     = {"hevc", "av1", "vp9", "vp8", "mpeg2"};
    static const char* const containers[] = {"mts", "m2ts", "3gp", "3g2", "mpg", "mpeg"};
    for (const char* codec : codecs) {
        if (facts.videoCodec == QLatin1String(codec))
            return true;
    }
    for (const char* container : containers) {
        if (facts.extension == QLatin1String(container))
            return true;
    }
    return false;
}

quint64 estimateBytes(int videoKbps, int audioKbps, qint64 durationMs)
{
    if (durationMs <= 0)
        return kContainerOverhead;
    const double bytes = (static_cast<double>(videoKbps) + audioKbps) * 1000.0 / 8.0 * (static_cast<double>(durationMs) / 1000.0) * kMuxOverhead;
    return static_cast<quint64>(std::llround(bytes)) + kContainerOverhead;
}

Plan planCompression(const VideoFacts& facts, const Options& options)
{
    // An .mp4 with only a sound track (or a file that isn't a video after all) goes as it is.
    if (facts.probed && !facts.hasVideo)
        return sendOriginal(Reason::NotVideo);
    const bool over = facts.bytes > options.limitBytes;
    if (options.request == Request::Original)
        return sendOriginal(Reason::OriginalAsked); // Core's upload limit check applies as for any file
    if (!options.mediaFoundation)
        return originalOrFail(over, Reason::NoMediaFoundation);

    const bool chosen     = isPreset(options.request);
    const bool unplayable = options.convertUnplayable && mayNotPlayForOthers(facts);
    const bool shrink     = options.compressLarge && facts.bytes > options.thresholdBytes;
    if (!chosen) {
        if (over && !options.compressLarge && !unplayable)
            return fail(Reason::DisabledOverLimit);
        if (!over && !shrink && !unplayable)
            return sendOriginal(options.compressLarge ? Reason::SmallEnough : Reason::Disabled);
    }

    // From here on the video should be transcoded: can it be?
    if (!facts.probed || facts.durationMs <= 0)
        return originalOrFail(over, Reason::UnknownLength);
    if (!facts.decodable)
        return originalOrFail(over, Reason::Undecodable, facts.undecodableText);
    if (facts.is32Bit && static_cast<qint64>(facts.display.width()) * facts.display.height() > kMax32BitPixels)
        return originalOrFail(over, Reason::TooLarge32Bit);
    if (facts.display.isEmpty())
        return originalOrFail(over, Reason::UnknownLength);

    Plan plan;
    plan.decision = Decision::Compress;
    setAudio(plan, facts);
    const int  presetSide = presetShortSide(options.request, options.shortSide);
    const Step& preset    = stepFor(presetSide);
    plan.frameSize        = scaledEven(facts.display, preset.shortSide);
    plan.fps              = outputFps(facts.fps, preset.fpsCap);
    plan.fpsCap           = preset.fpsCap;
    plan.videoKbps        = stepKbps(preset, plan.frameSize, plan.fps);

    // Never spend more than the source did (a format that may not play elsewhere may use up to twice its
    // rate: H.264 needs more bits than HEVC or AV1 for the same picture).
    const double source = sourceVideoKbps(facts, plan.audioKbps);
    if (source > 0.0) {
        const double cap = source * (unplayable ? 2.0 : kSourceBitrateCap);
        plan.videoKbps   = qMin(plan.videoKbps, qMax(floorKbps(plan.frameSize), static_cast<int>(std::lround(cap))));
    }
    plan.videoKbps      = qBound(kMinVideoKbps, plan.videoKbps, kMaxVideoKbps);
    plan.estimatedBytes = estimateBytes(plan.videoKbps, plan.audioKbps, facts.durationMs);

    const double fitBytes = static_cast<double>(options.limitBytes) * kFitTarget;
    if (static_cast<double>(plan.estimatedBytes) > fitBytes) {
        // Over the limit at this quality: a lower bitrate, then (Auto only) smaller sizes until it fits.
        const double budget = budgetKbps(options.limitBytes, facts.durationMs, plan.audioKbps);
        for (const Step& step : kSteps) {
            if (step.shortSide > preset.shortSide)
                continue;
            if (chosen && step.shortSide != preset.shortSide)
                break; // a picked preset is that size or nothing
            const QSize frame = scaledEven(facts.display, step.shortSide);
            const int   fps   = outputFps(facts.fps, step.fpsCap);
            const int   kbps  = qMin(stepKbps(step, frame, fps), plan.videoKbps);
            const int   fit   = static_cast<int>(std::floor(qMin<double>(kbps, budget)));
            if (fit >= floorKbps(frame) && fit >= kMinVideoKbps) {
                plan.frameSize      = frame;
                plan.fps            = fps;
                plan.fpsCap         = step.fpsCap;
                plan.videoKbps      = fit;
                plan.estimatedBytes = estimateBytes(plan.videoKbps, plan.audioKbps, facts.durationMs);
                plan.reason         = Reason::FitToLimit;
                return plan;
            }
        }
        return fail(Reason::CannotFit);
    }

    if (chosen) {
        plan.reason = Reason::Chosen;
        return plan;
    }
    if (shrink || over) {
        plan.reason = Reason::Shrink;
        // Not worth the wait when it saves little (unless the format needs converting anyway).
        if (!over && !unplayable && static_cast<double>(plan.estimatedBytes) > kWorthItRatio * static_cast<double>(facts.bytes))
            return sendOriginal(Reason::NotWorthIt);
        return plan;
    }
    plan.reason = Reason::Convert;
    return plan;
}

QString resolutionLabel(const QSize& frameSize)
{
    if (frameSize.isEmpty())
        return {};
    return i18n::t("%1p").arg(shortSideOf(frameSize));
}

QString compressFailedNote(const QString& cause)
{
    return i18n::t("Couldn't compress this video (%1), so the original was sent.").arg(cause);
}

QString compressFailedText(const QString& cause, int limitMB)
{
    return i18n::t("Couldn't compress this video (%1), and the original is larger than your %2 MB upload limit.").arg(cause).arg(limitMB);
}

QString diskSpaceText(quint64 neededBytes)
{
    return i18n::t("There isn't enough free disk space to compress this video (about %1 needed).").arg(formatSize(neededBytes));
}

QString failureText(const Plan& plan, int limitMB)
{
    switch (plan.reason) {
    case Reason::DisabledOverLimit:
        return i18n::t("This video is larger than your %1 MB upload limit. Turn on video compression or raise the limit in Settings → Sending.").arg(limitMB);
    case Reason::CannotFit:
        return i18n::t("This video is too long to fit your %1 MB upload limit, even at low quality. Shorten it, or raise the limit in Settings → Sending.").arg(limitMB);
    case Reason::Undecodable: {
        const QString why = plan.detail.isEmpty() ? i18n::t("This video format can't be compressed on this computer.") : plan.detail;
        return i18n::t("%1 It's larger than your %2 MB upload limit, so it can't be sent without compression.").arg(why).arg(limitMB);
    }
    case Reason::TooLarge32Bit:
        return i18n::t("This video is larger than your %1 MB upload limit, and videos above 2560 × 1600 can't be compressed in 32-bit TeamSpeak. "
                       "Use 64-bit TeamSpeak or raise the limit in Settings → Sending.")
            .arg(limitMB);
    case Reason::UnknownLength:
        return i18n::t("This video is larger than your %1 MB upload limit, and its length can't be read, so it can't be compressed to fit.").arg(limitMB);
    case Reason::NoMediaFoundation:
        return i18n::t("This video is larger than your %1 MB upload limit. Compressing videos needs Windows Media Foundation; on Windows N editions, "
                       "install the Media Feature Pack.")
            .arg(limitMB);
    default:
        return i18n::t("This file is larger than your %1 MB upload limit. You can raise the limit in Settings → Sending.").arg(limitMB);
    }
}

QVector<Choice> choices(const VideoFacts& facts, const Options& options)
{
    QVector<Choice> list;
    if (!options.mediaFoundation || !facts.probed || !facts.hasVideo || facts.durationMs <= 0 || !facts.decodable || facts.display.isEmpty())
        return list;
    if (facts.is32Bit && static_cast<qint64>(facts.display.width()) * facts.display.height() > kMax32BitPixels)
        return list;

    Options autoOptions = options;
    autoOptions.request = Request::Auto;
    const Plan autoPlan = planCompression(facts, autoOptions);
    const bool over     = facts.bytes > options.limitBytes;

    Choice original;
    original.request       = Request::Original;
    original.plan          = sendOriginal(Reason::OriginalAsked);
    original.enabled       = !over;
    const QString size     = formatSize(facts.bytes);
    original.label         = over ? i18n::t("Original · %1 · over the limit").arg(size) : i18n::t("Original · %1").arg(size);
    original.longLabel     = over ? i18n::t("Original · %1 (over your %2 MB limit)").arg(size).arg(options.limitBytes / kMiB) : i18n::t("Original · %1").arg(size);
    list.append(original);

    // Smallest first, so a preset that gives the same picture as a smaller one (a 720p source and
    // "High") is listed once, under the name that matches its size.
    QVector<Choice> presets;
    for (const Request request : {Request::P480, Request::P720, Request::P1080}) {
        Options asked = options;
        asked.request = request;
        const Plan plan = planCompression(facts, asked);
        if (plan.decision != Decision::Compress)
            continue;
        const bool duplicate = std::any_of(presets.cbegin(), presets.cend(), [&plan](const Choice& c) { return c.plan.frameSize == plan.frameSize; });
        if (duplicate)
            continue;
        Choice choice;
        choice.request = request;
        choice.plan    = plan;
        presets.append(choice);
    }
    // The planner's own choice when no preset gives it (a long video made to fit at 360p).
    if (autoPlan.decision == Decision::Compress
        && std::none_of(presets.cbegin(), presets.cend(), [&autoPlan](const Choice& c) { return samePlan(c.plan, autoPlan); })) {
        Choice choice;
        choice.request = Request::Auto;
        choice.plan    = autoPlan;
        // In place of a preset with the same size (Auto's lower bitrate wins); the list is sorted below.
        auto same = std::find_if(presets.begin(), presets.end(), [&autoPlan](const Choice& c) { return c.plan.frameSize == autoPlan.frameSize; });
        if (same != presets.end())
            *same = choice;
        else
            presets.prepend(choice);
    }
    std::sort(presets.begin(), presets.end(), [](const Choice& a, const Choice& b) {
        return a.plan.frameSize.width() * a.plan.frameSize.height() > b.plan.frameSize.width() * b.plan.frameSize.height();
    });
    for (Choice& choice : presets) {
        const QString resolution = resolutionLabel(choice.plan.frameSize);
        const QString estimate   = approxSize(choice.plan.estimatedBytes);
        choice.label             = i18n::t("%1 · about %2").arg(resolution, estimate);
        choice.longLabel         = i18n::t("%1 (%2) · about %3").arg(presetName(choice.request, choice.plan.frameSize), resolution, estimate);
        list.append(choice);
    }

    // The default: what Auto does.
    bool selected = false;
    if (autoPlan.decision == Decision::SendOriginal) {
        list[0].automatic = true;
        list[0].selected  = list[0].enabled;
        selected          = list[0].selected;
    } else if (autoPlan.decision == Decision::Compress) {
        for (Choice& choice : list) {
            if (choice.request != Request::Original && samePlan(choice.plan, autoPlan)) {
                choice.automatic = true;
                choice.selected  = true;
                selected         = true;
                break;
            }
        }
    }
    if (!selected) {
        // Auto would fail (compression is off and the video is over the limit): the largest entry that
        // can be sent. When nothing can (CannotFit), nothing is selected and the window says why.
        for (Choice& choice : list) {
            if (choice.enabled) {
                choice.selected = true;
                break;
            }
        }
    }
    return list;
}

Request requestFor(const QVector<Choice>& choices, int index)
{
    if (index < 0 || index >= choices.size())
        return Request::Auto;
    return choices.at(index).automatic ? Request::Auto : choices.at(index).request;
}

} // namespace videocompress
