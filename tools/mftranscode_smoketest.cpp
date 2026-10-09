// mftranscode_smoketest <video> <outdir> [options]
// mftranscode_smoketest --encoders
//
// 2.4 compress: runs the compression planner and the Media Foundation transcoder (src/videocompress.*,
// src/video/mftranscode.*) outside TeamSpeak, the way the plugin does: probe the original, plan, transcode
// on this thread, verify. Prints the plan, the encoder used, the realtime factor, process CPU time, peak
// memory and the output size against the estimate. Nothing is played; no sound.
//
// Options:
//   --quality 480|720|1080   the settings' quality (default 720)
//   --request auto|original|480|720|1080   what the send asks for (default auto)
//   --threshold-mb N         compress above this size (default 25)
//   --limit-mb N             the upload limit (default 100)
//   --no-convert             don't convert formats that may not play for others
//   --cpu                    processor only (no graphics card)
//   --force                  transcode even when the planner says to send the original (at the preset)
//   --kbps N                 override the planned video bitrate
//   --abort-above-mb N       the size guard (default: the plan's)
//   --cancel-at P            set cancel once P per mille is written; reports how long stopping took
//   --log FILE               also append the report lines to FILE
// Exit code: 0 done (or the planner said not to transcode), 1 usage, 2 Media Foundation unavailable,
// 3 probe failed, 4 transcode failed, 5 canceled as asked.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <windows.h>

#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "videocompress.h"
#include "video/mftranscode.h"
#include "video/mfvideo.h"

namespace {

QString g_log;

void print(const QString& text)
{
    std::fputs(text.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (!g_log.isEmpty()) {
        QFile file(g_log);
        if (file.open(QIODevice::Append | QIODevice::Text))
            file.write(text.toUtf8() + '\n');
    }
}

double fileTimeSeconds(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart  = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return static_cast<double>(value.QuadPart) / 1.0e7;
}

double cpuSeconds()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0.0;
    return fileTimeSeconds(kernel) + fileTimeSeconds(user);
}

double peakMemoryMB()
{
    PROCESS_MEMORY_COUNTERS counters = {};
    counters.cb                      = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0.0;
    return static_cast<double>(counters.PeakPagefileUsage) / (1024.0 * 1024.0);
}

videocompress::Request requestFrom(const QString& text)
{
    if (text == QLatin1String("original"))
        return videocompress::Request::Original;
    if (text == QLatin1String("1080"))
        return videocompress::Request::P1080;
    if (text == QLatin1String("720"))
        return videocompress::Request::P720;
    if (text == QLatin1String("480"))
        return videocompress::Request::P480;
    return videocompress::Request::Auto;
}

const char* decisionName(videocompress::Decision decision)
{
    switch (decision) {
    case videocompress::Decision::SendOriginal:
        return "send-original";
    case videocompress::Decision::Compress:
        return "compress";
    case videocompress::Decision::Fail:
        return "fail";
    }
    return "?";
}

const char* reasonName(videocompress::Reason reason)
{
    using R = videocompress::Reason;
    switch (reason) {
    case R::NotVideo: return "not-video";
    case R::OriginalAsked: return "original-asked";
    case R::Disabled: return "disabled";
    case R::SmallEnough: return "small-enough";
    case R::NotWorthIt: return "not-worth-it";
    case R::UnknownLength: return "unknown-length";
    case R::Undecodable: return "undecodable";
    case R::TooLarge32Bit: return "too-large-32bit";
    case R::NoMediaFoundation: return "no-media-foundation";
    case R::Shrink: return "shrink";
    case R::Convert: return "convert";
    case R::FitToLimit: return "fit-to-limit";
    case R::Chosen: return "chosen";
    case R::CannotFit: return "cannot-fit";
    case R::DisabledOverLimit: return "disabled-over-limit";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    SetConsoleOutputCP(CP_UTF8);
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--log"))
            g_log = args.at(i + 1);
    }

    if (args.size() >= 2 && args.at(1) == QLatin1String("--encoders")) {
        const mf::EncoderList list = mf::h264Encoders();
        print(QStringLiteral("queried: %1").arg(list.queried ? QStringLiteral("yes") : QStringLiteral("no")));
        print(QStringLiteral("hardware H.264 encoders: %1").arg(list.hardware.isEmpty() ? QStringLiteral("none") : list.hardware.join(QStringLiteral("; "))));
        print(QStringLiteral("software H.264 encoders: %1").arg(list.software.isEmpty() ? QStringLiteral("none") : list.software.join(QStringLiteral("; "))));
        print(QStringLiteral("process: %1-bit").arg(sizeof(void*) * 8));
        return list.queried ? 0 : 2;
    }
    if (args.size() < 3) {
        print(QStringLiteral("usage: mftranscode_smoketest <video> <outdir> [--quality N] [--request R] [--threshold-mb N] [--limit-mb N] "
                             "[--no-convert] [--cpu] [--force] [--kbps N] [--abort-above-mb N] [--cancel-at P] [--log FILE]"));
        print(QStringLiteral("       mftranscode_smoketest --encoders"));
        return 1;
    }

    const QString source = QFileInfo(args.at(1)).absoluteFilePath();
    const QString outDir = QFileInfo(args.at(2)).absoluteFilePath();
    QDir().mkpath(outDir);

    videocompress::Options options;
    bool                   cpuOnly  = false;
    bool                   force    = false;
    int                    kbps     = 0;
    int                    cancelAt = -1;
    qint64                 abortMB  = -1;
    QString                tag      = QStringLiteral("auto");
    for (int i = 3; i < args.size(); ++i) {
        const QString& arg  = args.at(i);
        const QString  next = i + 1 < args.size() ? args.at(i + 1) : QString();
        if (arg == QLatin1String("--quality")) {
            options.shortSide = next.toInt();
            ++i;
        } else if (arg == QLatin1String("--request")) {
            options.request = requestFrom(next);
            tag             = next;
            ++i;
        } else if (arg == QLatin1String("--threshold-mb")) {
            options.thresholdBytes = next.toULongLong() * 1024 * 1024;
            ++i;
        } else if (arg == QLatin1String("--limit-mb")) {
            options.limitBytes = next.toULongLong() * 1024 * 1024;
            ++i;
        } else if (arg == QLatin1String("--no-convert")) {
            options.convertUnplayable = false;
        } else if (arg == QLatin1String("--cpu")) {
            cpuOnly = true;
        } else if (arg == QLatin1String("--force")) {
            force = true;
        } else if (arg == QLatin1String("--kbps")) {
            kbps = next.toInt();
            ++i;
        } else if (arg == QLatin1String("--abort-above-mb")) {
            abortMB = next.toLongLong();
            ++i;
        } else if (arg == QLatin1String("--cancel-at")) {
            cancelAt = next.toInt();
            ++i;
        } else if (arg == QLatin1String("--log")) {
            ++i;
        }
    }

    print(QStringLiteral("file: %1").arg(QDir::toNativeSeparators(source)));
    if (!mf::available()) {
        print(QStringLiteral("FAIL: Media Foundation is not available"));
        return 2;
    }

    QElapsedTimer probeClock;
    probeClock.start();
    const mf::ProbeResult probe = mf::probe(source, 960);
    const qint64          probeMs = probeClock.elapsed();
    videocompress::VideoFacts facts;
    facts.bytes           = static_cast<quint64>(QFileInfo(source).size());
    facts.durationMs      = probe.durationMs;
    facts.display         = probe.size;
    const double measured = mf::measuredFrameRate(source);
    facts.fps             = measured > 0.0 ? measured : probe.frameRate;
    facts.probed          = probe.ok;
    facts.hasVideo        = probe.hasVideo;
    facts.decodable       = !probe.poster.isNull();
    facts.undecodableText = probe.error;
    facts.hasAudio        = probe.hasAudio;
    facts.audioChannels   = probe.audioChannels;
    facts.videoCodec      = videocompress::codecId(probe.videoCodec);
    facts.extension       = QFileInfo(source).suffix().toLower();
    print(QStringLiteral("probe: ok=%1 %2x%3 rotation=%4 %5 fps %6 ms codec=%7 audio=%8ch@%9Hz decodable=%10 (%11 ms)%12")
              .arg(probe.ok)
              .arg(probe.size.width())
              .arg(probe.size.height())
              .arg(probe.rotation)
              .arg(probe.frameRate, 0, 'f', 3)
              .arg(probe.durationMs)
              .arg(probe.videoCodec.isEmpty() ? QStringLiteral("?") : probe.videoCodec)
              .arg(probe.audioChannels)
              .arg(probe.audioSampleRate)
              .arg(facts.decodable)
              .arg(probeMs)
              .arg(probe.error.isEmpty() ? QString() : QStringLiteral(" error: ") + probe.error));
    if (!probe.ok) {
        print(QStringLiteral("FAIL: probe"));
        return 3;
    }

    videocompress::Plan plan = videocompress::planCompression(facts, options);
    print(QStringLiteral("plan: %1 (%2) %3x%4 %5 fps video %6 kbps audio %7 kbps/%8ch estimate %9 bytes; source %10 bytes; may-not-play=%11")
              .arg(QLatin1String(decisionName(plan.decision)), QLatin1String(reasonName(plan.reason)))
              .arg(plan.frameSize.width())
              .arg(plan.frameSize.height())
              .arg(plan.fps)
              .arg(plan.videoKbps)
              .arg(plan.audioKbps)
              .arg(plan.audioChannels)
              .arg(plan.estimatedBytes)
              .arg(facts.bytes)
              .arg(videocompress::mayNotPlayForOthers(facts)));
    if (plan.decision == videocompress::Decision::Fail)
        print(QStringLiteral("text: ") + videocompress::failureText(plan, static_cast<int>(options.limitBytes / (1024 * 1024))));
    for (const videocompress::Choice& choice : videocompress::choices(facts, options))
        print(QStringLiteral("choice: %1%2%3 | %4").arg(choice.selected ? QStringLiteral("* ") : QStringLiteral("  "), choice.label,
                                                        choice.enabled ? QString() : QStringLiteral(" (disabled)"), choice.longLabel));
    if (plan.decision != videocompress::Decision::Compress) {
        if (!force) {
            print(QStringLiteral("RESULT: not transcoded (%1)").arg(QLatin1String(reasonName(plan.reason))));
            return 0;
        }
        videocompress::Options forced = options;
        forced.request                = options.shortSide == 480 ? videocompress::Request::P480
                                        : options.shortSide == 1080 ? videocompress::Request::P1080
                                                                    : videocompress::Request::P720;
        plan = videocompress::planCompression(facts, forced);
        if (plan.decision != videocompress::Decision::Compress) {
            print(QStringLiteral("RESULT: can't force (%1)").arg(QLatin1String(reasonName(plan.reason))));
            return 4;
        }
    }

    print(QStringLiteral("peak memory after the probe: %1 MB").arg(peakMemoryMB(), 0, 'f', 0));
    mf::TranscodeRequest request;
    request.source        = source;
    request.target        = outDir + QLatin1Char('/') + QFileInfo(source).completeBaseName() + QLatin1Char('_') + tag + (cpuOnly ? QStringLiteral("_cpu") : QStringLiteral("_gpu")) + QStringLiteral(".mp4");
    request.frameSize     = plan.frameSize;
    request.fps           = plan.fpsCap;
    request.videoKbps     = kbps > 0 ? kbps : plan.videoKbps;
    request.audioKbps     = plan.audioKbps;
    request.audioChannels = plan.audioChannels;
    request.allowHardware = !cpuOnly;
    request.durationMs    = facts.durationMs;
    request.abortAboveBytes = abortMB >= 0 ? static_cast<quint64>(abortMB) * 1024 * 1024
                              : plan.reason == videocompress::Reason::FitToLimit
                                  ? static_cast<quint64>(static_cast<double>(options.limitBytes) * videocompress::kFitTarget)
                                  : options.limitBytes;

    mf::TranscodeControl control;
    std::atomic<bool>    done{false};
    QElapsedTimer        cancelClock;
    std::atomic<qint64>  cancelSetAt{-1};
    std::thread          watcher([&] {
        int lastTenth = -1;
        while (!done.load()) {
            const int permille = control.permille.load();
            if (permille / 100 != lastTenth) {
                lastTenth = permille / 100;
                std::fprintf(stdout, "  %d%%%s\n", permille / 10, control.finishing.load() ? " (finishing)" : "");
                std::fflush(stdout);
            }
            if (cancelAt >= 0 && permille >= cancelAt && cancelSetAt.load() < 0) {
                cancelSetAt.store(cancelClock.elapsed());
                control.cancel.store(true);
            }
            Sleep(20);
        }
    });
    const double cpuBefore = cpuSeconds();
    cancelClock.start();
    const mf::TranscodeResult result = mf::transcodeToMp4(request, &control);
    const qint64              endMs  = cancelClock.elapsed();
    done.store(true);
    watcher.join();
    const double cpu = cpuSeconds() - cpuBefore;

    const double seconds  = static_cast<double>(result.elapsedMs) / 1000.0;
    const double realtime = seconds > 0.0 ? static_cast<double>(facts.durationMs) / 1000.0 / seconds : 0.0;
    print(QStringLiteral("encoder: %1 (%2)%3")
              .arg(result.encoderName.isEmpty() ? QStringLiteral("?") : result.encoderName, result.hardware ? QStringLiteral("graphics card") : QStringLiteral("processor"))
              .arg(result.gpuFailed ? QStringLiteral("; graphics card failed first: ") + result.gpuError : QString()));
    print(QStringLiteral("time: %1 s, %2x realtime, process CPU %3 s (%4 cores busy), peak memory %5 MB")
              .arg(seconds, 0, 'f', 2)
              .arg(realtime, 0, 'f', 2)
              .arg(cpu, 0, 'f', 1)
              .arg(seconds > 0.0 ? cpu / seconds : 0.0, 0, 'f', 1)
              .arg(peakMemoryMB(), 0, 'f', 0));
    if (cancelAt >= 0) {
        const qint64 setAt = cancelSetAt.load();
        print(QStringLiteral("cancel: set at %1 ms, returned %2 ms later; canceled=%3; output left behind=%4")
                  .arg(setAt)
                  .arg(setAt >= 0 ? endMs - setAt : -1)
                  .arg(result.canceled)
                  .arg(QFileInfo::exists(request.target)));
        if (result.canceled) {
            // The partial file must be gone (and nothing may hold it).
            print(QFileInfo::exists(request.target) ? QStringLiteral("FAIL: partial output left") : QStringLiteral("RESULT: canceled cleanly"));
            return QFileInfo::exists(request.target) ? 4 : 5;
        }
    }
    if (!result.ok) {
        print(QStringLiteral("RESULT: FAIL stage=%1 hr=0x%2 exceeded=%3 projected=%4 detail=%5")
                  .arg(QLatin1String(mf::stageName(result.stage)))
                  .arg(result.hr, 8, 16, QLatin1Char('0'))
                  .arg(result.exceeded)
                  .arg(result.projectedBytes)
                  .arg(result.detail));
        return 4;
    }
    print(QStringLiteral("detail: ") + result.detail);
    const double vsEstimate = plan.estimatedBytes > 0 ? 100.0 * static_cast<double>(result.bytes) / static_cast<double>(plan.estimatedBytes) : 0.0;
    print(QStringLiteral("RESULT: ok %1x%2 %3 ms, %4 bytes (%5% of the estimate, %6% of the original) -> %7")
              .arg(result.frameSize.width())
              .arg(result.frameSize.height())
              .arg(result.durationMs)
              .arg(result.bytes)
              .arg(vsEstimate, 0, 'f', 1)
              .arg(100.0 * static_cast<double>(result.bytes) / static_cast<double>(qMax<quint64>(1, facts.bytes)), 0, 'f', 1)
              .arg(QDir::toNativeSeparators(request.target)));
    return 0;
}
