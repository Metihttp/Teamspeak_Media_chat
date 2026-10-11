// Test builds only (TSMEDIA_TESTHOOKS): the live-test driver's audio, input and file-dialog commands
// (selftest.cpp hands them over). Start a track with {"cmd":"command","text":"music <path>"} (/tsmedia music).
//   {"cmd":"music.state"}                     one line: engine state, position, which sides run, the bar, and
//                                             this connection's capture counters (capstats); whether it waits for
//                                             its server tab and whether voice activation is off for it (2.3 polish)
//   {"cmd":"music.volume","value":0}          the bar's volume (0: the music is silent, also for the channel)
//   {"cmd":"music.devices","on":true,"signal":"silence|tone|speech","amp":300}
//                                             this (localhost) connection captures from a custom test device fed
//                                             here (silence, a 440 Hz tone or speech-like bursts, amp in 1/1000
//                                             of full scale) and plays into a custom device that is read and
//                                             thrown away: the voice path runs without a microphone and nothing
//                                             is heard on this computer. "on":false closes them (TeamSpeak's
//                                             capture stays closed: never the real microphone again; its default
//                                             playback only with "playback":true). "query":true only logs the
//                                             connection's devices, voice activation settings and input flags.
//                                             Off before another server tab opens: TeamSpeak 3.6.2 crashes in its
//                                             own device code when a tab opens while a custom device is in use
//                                             (the driver's connect refuses then).
//   {"cmd":"music.signal","signal":"speech","amp":300}   what the custom capture device sends from now on
//   {"cmd":"music.vad","on":true,"level":-30} voice activation of this connection's capture (runtime only; refused
//                                             unless it captures from the custom test device)
//   {"cmd":"capstats","reset":true}           what TeamSpeak captured on this connection: blocks, voice activation
//                                             (bit 2 on entry), blocks the guard silenced, blocks sent and their level
//   {"cmd":"voicerx","reset":true}            the voice each client sent us (blocks, RMS, peak) and its talk status
//   {"cmd":"music.bar","action":"log|play|stop|mute|close|seek|volume|key","ms":..,"value":..,"key":"Right","name":..,"up":1}
//                                             the music bar's own controls, as a click / drag / key uses them;
//                                             name: a grab of the bar (or of its up-th parent)
//   {"cmd":"attach.state","name":..,"row":..,"up":1}  the paperclip and the other buttons of the chat input, where
//                                             the input's text starts, and the open attach menu (entries, current);
//                                             name: a grab of the menu, row: of the input's up-th parent
//   {"cmd":"listpick","window":"Options","text":"Addons"}   a row of a list in a visible window
//   {"cmd":"filedlg","action":"log|set|ok|cancel","text":".."}
//                                             the open file chooser (Windows' own, or Qt's): its file name box
//                                             set, then Open or Cancel (posted: the dialog's own loop runs them)
//
// The capture guard (live pass B): in a test build, a captured block of a connection whose capture device
// isn't the custom test device (checked on every pump tick) is silenced and not sent, so a real microphone
// never reaches a server, whatever a test or TeamSpeak opens. The music mixed in after it still goes out.

#ifdef TSMEDIA_TESTHOOKS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "attachbutton.h"
#include "attachpopup.h"
#include "music/musicbar.h"
#include "music/musiccontroller.h"
#include "ts3api.h"

namespace {

constexpr char   kDevice[]      = "tsmedia_selftest";
constexpr char   kDeviceLabel[] = "TS Media test device";
constexpr int    kBlock         = 960; // 20 ms at 48 kHz
constexpr double kPi            = 3.14159265358979323846;

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void atomicMax(std::atomic<int>& slot, int value)
{
    int seen = slot.load(std::memory_order_relaxed);
    while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

QString nickOf(quint64 sch, anyID client)
{
    char* name = nullptr;
    if (ts3::funcs.getClientVariableAsString && ts3::funcs.getClientVariableAsString(sch, client, CLIENT_NICKNAME, &name) == ERROR_ok)
        return ts3::takeString(name);
    return QString();
}

// ---- the custom devices --------------------------------------------------------------------------------------

QPointer<QTimer>     g_pump;
std::atomic<int>     g_peak{0}; // loudest sample the custom playback device got
std::atomic<qint64>  g_blocks{0};
std::atomic<quint64> g_deviceSch{0};
std::atomic<bool>    g_customVerified{false}; // the pump saw the custom device as g_deviceSch's capture device
std::atomic<int>     g_signal{0};             // 0 silence, 1 tone, 2 speech-like bursts
std::atomic<int>     g_amp{300};              // 1/1000 of full scale
qint64               g_signalFrame = 0;       // GUI thread (the pump)

QString signalName(int s)
{
    return QString::fromLatin1(s == 1 ? "tone" : s == 2 ? "speech" : "silence");
}

int signalOf(const QString& name)
{
    return name == QLatin1String("tone") ? 1 : name == QLatin1String("speech") ? 2 : 0;
}

// One 20 ms block of the chosen test signal (mono, 48 kHz).
void fillSignal(std::vector<short>* block)
{
    const int    kind = g_signal.load();
    const double amp  = qBound(0, g_amp.load(), 900) / 1000.0 * 32767.0;
    for (int i = 0; i < kBlock; ++i, ++g_signalFrame) {
        const double t = static_cast<double>(g_signalFrame) / 48000.0;
        double       v = 0.0;
        if (kind == 1) {
            v = std::sin(2.0 * kPi * 440.0 * t);
        } else if (kind == 2) {
            // Speech-like: a 140 Hz buzz with falling harmonics, 4 syllables a second, 1.2 s on and 0.8 s off.
            const double cycle = std::fmod(t, 2.0);
            if (cycle < 1.2) {
                double buzz = 0.0;
                for (int k = 1; k <= 8; ++k)
                    buzz += std::sin(2.0 * kPi * 140.0 * k * t) / k;
                const double syllable = 0.55 + 0.45 * std::sin(2.0 * kPi * 4.0 * t);
                const double edge     = qMin(1.0, qMin(cycle / 0.03, (1.2 - cycle) / 0.03));
                v                     = buzz / 2.2 * syllable * edge;
            }
        }
        (*block)[static_cast<size_t>(i)] = static_cast<short>(qBound(-32767.0, v * amp, 32767.0));
    }
}

QString captureDevice(quint64 sch, QString* mode)
{
    char* m = nullptr;
    if (ts3::funcs.getCurrentCaptureMode && ts3::funcs.getCurrentCaptureMode(sch, &m) == ERROR_ok)
        *mode = ts3::takeString(m);
    char* name      = nullptr;
    int   isDefault = 0;
    if (ts3::funcs.getCurrentCaptureDeviceName && ts3::funcs.getCurrentCaptureDeviceName(sch, &name, &isDefault) == ERROR_ok)
        return ts3::takeString(name) + (isDefault ? QString::fromLatin1(" (default)") : QString());
    return QString::fromLatin1("(none)");
}

QString playbackDevice(quint64 sch)
{
    char* name      = nullptr;
    int   isDefault = 0;
    if (ts3::funcs.getCurrentPlaybackDeviceName && ts3::funcs.getCurrentPlaybackDeviceName(sch, &name, &isDefault) == ERROR_ok)
        return ts3::takeString(name) + (isDefault ? QString::fromLatin1(" (default)") : QString());
    return QString::fromLatin1("(none)");
}

bool isCustomCapture(quint64 sch)
{
    QString       mode;
    const QString name = captureDevice(sch, &mode);
    return mode == QLatin1String("custom") && (name == QLatin1String(kDevice) || name == QLatin1String(kDeviceLabel));
}

QString preprocessor(quint64 sch, const char* ident)
{
    char* value = nullptr;
    if (!ts3::funcs.getPreProcessorConfigValue || ts3::funcs.getPreProcessorConfigValue(sch, ident, &value) != ERROR_ok)
        return QString::fromLatin1("?");
    return ts3::takeString(value);
}

QString devicesLine(quint64 sch)
{
    QString       mode;
    const QString capture = captureDevice(sch, &mode);
    float         level   = 0.0f;
    if (ts3::funcs.getPreProcessorInfoValueFloat)
        ts3::funcs.getPreProcessorInfoValueFloat(sch, "decibel_last_period", &level);
    int muted = -1, deact = -1, hw = -1, out = -1;
    ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, &muted);
    ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, &deact);
    ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &hw);
    ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_OUTPUT_MUTED, &out);
    int talking = -1;
    ts3::funcs.getClientSelfVariableAsInt(sch, CLIENT_FLAG_TALKING, &talking);
    return QString::fromLatin1("talking %16 | sch %1 capture mode \"%2\" device \"%3\" custom %4 verified %5 | playback \"%6\" | vad %7 level %8 dB last %9 dB "
                               "| input_muted %10 input_deactivated %11 input_hardware %12 output_muted %13 | signal %14 amp %15")
        .arg(sch)
        .arg(mode, capture)
        .arg(isCustomCapture(sch) ? 1 : 0)
        .arg(g_deviceSch.load() == sch && g_customVerified.load() ? 1 : 0)
        .arg(playbackDevice(sch), preprocessor(sch, "vad"), preprocessor(sch, "voiceactivation_level"))
        .arg(static_cast<double>(level), 0, 'f', 1)
        .arg(muted)
        .arg(deact)
        .arg(hw)
        .arg(out)
        .arg(signalName(g_signal.load()))
        .arg(g_amp.load())
        .arg(talking);
}

void stopDevices(const std::function<void(const QString&)>& say, bool reopenPlayback)
{
    if (g_pump)
        delete g_pump.data();
    g_customVerified  = false;
    const quint64 sch = g_deviceSch.exchange(0);
    if (!sch)
        return;
    const unsigned c1 = ts3::funcs.closeCaptureDevice ? ts3::funcs.closeCaptureDevice(sch) : 1u;
    const unsigned c2 = ts3::funcs.closePlaybackDevice ? ts3::funcs.closePlaybackDevice(sch) : 1u;
    const unsigned u  = ts3::funcs.unregisterCustomDevice ? ts3::funcs.unregisterCustomDevice(kDevice) : 1u;
    // TeamSpeak's capture stays closed (live pass B: a test build never opens a real microphone); its default
    // playback only when asked (whatever it plays is then heard on this computer).
    const unsigned o2 = reopenPlayback && ts3::funcs.openPlaybackDevice ? ts3::funcs.openPlaybackDevice(sch, "", "") : 0u;
    say(QString::fromLatin1("music.devices: off (close %1/%2, unregister %3, default playback %4)")
            .arg(c1)
            .arg(c2)
            .arg(u)
            .arg(reopenPlayback ? QString::number(o2) : QString::fromLatin1("not opened")));
}

// ---- counters filled on TeamSpeak's audio threads -------------------------------------------------------------

struct CaptureSlot {
    std::atomic<quint64> sch{0};
    std::atomic<qint64>  blocks{0}, entryTalk{0}, guarded{0}, sent{0}, sentLoud{0};
    std::atomic<qint64>  inMeanSq{0}, sentMeanSq{0}; // sums of each block's mean square
    std::atomic<int>     inPeak{0}, sentPeak{0};
    std::atomic<qint64>  lastSentLoudMs{0};
};
CaptureSlot g_capture[4];

struct ClientSlot {
    std::atomic<quint64> key{0}; // sch << 16 | client
    std::atomic<qint64>  blocks{0}, meanSq{0}, loud{0}, lastMs{0};
    std::atomic<int>     peak{0};
    std::atomic<int>     talk{-1}, on{0}, off{0};
    std::atomic<qint64>  talkChangedMs{0};
};
ClientSlot g_clients[24];

CaptureSlot* captureSlot(quint64 sch)
{
    for (CaptureSlot& s : g_capture) {
        quint64 seen = s.sch.load();
        if (seen == sch)
            return &s;
        if (seen == 0 && (s.sch.compare_exchange_strong(seen, sch) || seen == sch))
            return &s;
    }
    return nullptr;
}

ClientSlot* clientSlot(quint64 sch, anyID client)
{
    const quint64 key = (sch << 16) | client;
    for (ClientSlot& s : g_clients) {
        quint64 seen = s.key.load();
        if (seen == key)
            return &s;
        if (seen == 0 && (s.key.compare_exchange_strong(seen, key) || seen == key))
            return &s;
    }
    return nullptr;
}

// Mean square and peak of a block (interleaved, all channels).
void measure(const short* samples, int count, qint64* meanSq, int* peak)
{
    double sum = 0.0;
    int    top = 0;
    for (int i = 0; i < count; ++i) {
        const int v = samples[i];
        sum += static_cast<double>(v) * v;
        top = qMax(top, std::abs(v));
    }
    *meanSq = count > 0 ? static_cast<qint64>(sum / count) : 0;
    *peak   = top;
}

QString rms(qint64 meanSqSum, qint64 blocks)
{
    return blocks > 0 ? QString::number(std::sqrt(static_cast<double>(meanSqSum) / blocks), 'f', 0) : QString::fromLatin1("-");
}

QString captureLine(quint64 sch)
{
    for (const CaptureSlot& s : g_capture) {
        if (s.sch.load() != sch)
            continue;
        const qint64 last = s.lastSentLoudMs.load();
        return QString::fromLatin1("capture blocks %1 vadTalk %2 guarded %3 sent %4 sentLoud %5 inRms %6 inPeak %7 sentRms %8 sentPeak %9 lastLoudSent %10 ms ago")
            .arg(s.blocks.load())
            .arg(s.entryTalk.load())
            .arg(s.guarded.load())
            .arg(s.sent.load())
            .arg(s.sentLoud.load())
            .arg(rms(s.inMeanSq.load(), s.blocks.load() - s.guarded.load()))
            .arg(s.inPeak.load())
            .arg(rms(s.sentMeanSq.load(), s.sent.load()))
            .arg(s.sentPeak.load())
            .arg(last ? nowMs() - last : -1);
    }
    return QString::fromLatin1("capture: nothing captured on sch %1").arg(sch);
}

void resetCapture(quint64 sch)
{
    for (CaptureSlot& s : g_capture) {
        if (s.sch.load() != sch)
            continue;
        s.blocks         = 0;
        s.entryTalk      = 0;
        s.guarded        = 0;
        s.sent           = 0;
        s.sentLoud       = 0;
        s.inMeanSq       = 0;
        s.sentMeanSq     = 0;
        s.inPeak         = 0;
        s.sentPeak       = 0;
        s.lastSentLoudMs = 0;
    }
}

// ---- the music bar and the chat input -------------------------------------------------------------------------

QString saveGrabOf(QWidget* w, const QString& name)
{
    QDir().mkpath(ts3::dataDir() + QString::fromLatin1("/debug"));
    const QString path = ts3::dataDir() + QString::fromLatin1("/debug/") + name + QString::fromLatin1(".png");
    w->grab().save(path);
    return path;
}

QString rectText(const QRect& r)
{
    return QString::fromLatin1("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QString barLine(MusicBar* bar)
{
    if (!bar)
        return QString::fromLatin1("no bar");
    const MusicBar::View&     v      = bar->view();
    const char* const         mode   = v.mode == MusicBar::Mode::Loading ? "loading" : v.mode == MusicBar::Mode::Playing ? "playing" : v.mode == MusicBar::Mode::Paused ? "paused" : "notice";
    const MusicBar::Geometry& g      = bar->parts();
    QWidget*                  parent = bar->parentWidget();
    QWidget*                  play   = bar->playButton();
    return QString::fromLatin1("bar visible %1 %2 in %3#%4 | mode %5 title \"%6\" artist \"%7\" pos %8/%9 volume %10 muted %11 live %12 away \"%13\" problem \"%14\" "
                               "notice \"%15\" | shown: elapsed %16 seek %17 total %18 volumeButton %19 volumeSlider %20 | play \"%21\" stop %22 close %23")
        .arg(bar->isVisible() ? 1 : 0)
        .arg(rectText(bar->geometry()))
        .arg(parent ? QString::fromLatin1(parent->metaObject()->className()) : QString::fromLatin1("-"), parent ? parent->objectName() : QString())
        .arg(QString::fromLatin1(mode), v.title, v.artist)
        .arg(v.positionMs)
        .arg(v.durationMs)
        .arg(v.volume)
        .arg(v.muted ? 1 : 0)
        .arg(v.live ? 1 : 0)
        .arg(v.away, v.problem, v.notice)
        .arg(g.showElapsed ? 1 : 0)
        .arg(g.showSeek ? 1 : 0)
        .arg(g.showTotal ? 1 : 0)
        .arg(g.showVolumeButton ? 1 : 0)
        .arg(g.showVolumeSlider ? 1 : 0)
        .arg(play && play->isVisible() ? play->accessibleName() : QString::fromLatin1("hidden"))
        .arg(bar->stopButton() && bar->stopButton()->isVisible() ? 1 : 0)
        .arg(bar->closeButton() && bar->closeButton()->isVisible() ? 1 : 0);
}

QTextEdit* visibleChatInput()
{
    for (QWidget* w : QApplication::allWidgets()) {
        if (w->inherits("ChatLineEdit") && w->isVisible())
            return qobject_cast<QTextEdit*>(w);
    }
    return nullptr;
}

// ---- the file chooser -------------------------------------------------------------------------------------------

struct HwndList {
    QList<HWND> list;
};

BOOL CALLBACK collectDialog(HWND hwnd, LPARAM data)
{
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (IsWindowVisible(hwnd) && wcscmp(cls, L"#32770") == 0)
        reinterpret_cast<HwndList*>(data)->list.append(hwnd);
    return TRUE;
}

BOOL CALLBACK findFileNameBox(HWND child, LPARAM data)
{
    if (GetDlgCtrlID(child) == 0x47C) {
        reinterpret_cast<HwndList*>(data)->list.append(child);
        return FALSE;
    }
    return TRUE;
}

HWND nativeDialog()
{
    HwndList found;
    EnumThreadWindows(GetCurrentThreadId(), collectDialog, reinterpret_cast<LPARAM>(&found));
    return found.list.isEmpty() ? nullptr : found.list.first();
}

QString windowText(HWND hwnd)
{
    wchar_t text[512] = {};
    GetWindowTextW(hwnd, text, 511);
    return QString::fromWCharArray(text);
}

// The "File name:" box of a common file dialog: the edit inside the combo box with id 1148 (0x47C).
HWND fileNameEdit(HWND dialog)
{
    HWND combo = GetDlgItem(dialog, 0x47C);
    if (!combo) { // the Vista style dialog nests it
        HwndList found;
        EnumChildWindows(dialog, findFileNameBox, reinterpret_cast<LPARAM>(&found));
        combo = found.list.isEmpty() ? nullptr : found.list.first();
    }
    if (!combo)
        return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(combo, cls, 64);
    if (wcscmp(cls, L"Edit") == 0)
        return combo;
    HWND inner = FindWindowExW(combo, nullptr, L"ComboBox", nullptr);
    return FindWindowExW(inner ? inner : combo, nullptr, L"Edit", nullptr);
}

QFileDialog* qtFileDialog()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* d = qobject_cast<QFileDialog*>(w); d && d->isVisible())
            return d;
    }
    return nullptr;
}

} // namespace

// ---- TeamSpeak's audio threads (plugin.cpp) ----------------------------------------------------------------------

void selfTestCapturePre(quint64 sch, short* samples, int sampleCount, int channels, int* edited)
{
    if (!samples || sampleCount <= 0 || channels <= 0 || !edited)
        return;
    const int    count = sampleCount * channels;
    CaptureSlot* s     = captureSlot(sch);
    if (s) {
        ++s->blocks;
        if (*edited & 2)
            ++s->entryTalk;
    }
    if (sch != g_deviceSch.load() || !g_customVerified.load()) {
        // Not the custom test device: whatever it is, it is never sent from a test build.
        std::memset(samples, 0, static_cast<size_t>(count) * sizeof(short));
        *edited = (*edited | 1) & ~2;
        if (s)
            ++s->guarded;
        return;
    }
    if (!s)
        return;
    qint64 meanSq = 0;
    int    peak   = 0;
    measure(samples, count, &meanSq, &peak);
    s->inMeanSq += meanSq;
    atomicMax(s->inPeak, peak);
}

void selfTestCapturePost(quint64 sch, const short* samples, int sampleCount, int channels, const int* edited)
{
    if (!samples || sampleCount <= 0 || channels <= 0 || !edited || !(*edited & 2))
        return;
    CaptureSlot* s = captureSlot(sch);
    if (!s)
        return;
    ++s->sent;
    qint64 meanSq = 0;
    int    peak   = 0;
    measure(samples, sampleCount * channels, &meanSq, &peak);
    s->sentMeanSq += meanSq;
    atomicMax(s->sentPeak, peak);
    if (peak > 300) {
        ++s->sentLoud;
        s->lastSentLoudMs = nowMs();
    }
}

void selfTestPlaybackVoice(quint64 sch, anyID client, const short* samples, int sampleCount, int channels)
{
    ClientSlot* s = clientSlot(sch, client);
    if (!s || !samples || sampleCount <= 0 || channels <= 0)
        return;
    qint64 meanSq = 0;
    int    peak   = 0;
    measure(samples, sampleCount * channels, &meanSq, &peak);
    ++s->blocks;
    s->meanSq += meanSq;
    atomicMax(s->peak, peak);
    if (peak > 300)
        ++s->loud;
    s->lastMs = nowMs();
}

void selfTestTalkStatus(quint64 sch, int status, anyID client)
{
    ClientSlot* s = clientSlot(sch, client);
    if (!s)
        return;
    s->talk = status;
    if (status == STATUS_TALKING)
        ++s->on;
    else
        ++s->off;
    s->talkChangedMs = nowMs();
}

// ---- the driver's commands -----------------------------------------------------------------------------------

bool selfTestMusic(const QString& cmd, const QJsonObject& c, quint64 sch, QObject* owner, const std::function<void(const QString&)>& say)
{
    const auto str = [&c](const char* key, const char* fallback) { return c.value(QString::fromLatin1(key)).toString(QString::fromLatin1(fallback)); };
    MusicController* music = MusicController::current();
    if (cmd == QLatin1String("music.state")) {
        if (!music) {
            say(QString::fromLatin1("music.state: no controller"));
            return true;
        }
        QString line = QString::fromLatin1("music.state: playing %1").arg(music->playing() ? 1 : 0);
        if (music::Engine* e = music->engine()) {
            const music::Engine::Snapshot s = e->snapshot();
            line += QString::fromLatin1(" state %1 pos %2/%3 sending %4 mixing %5 monitoring %6 underruns %7 sch %8")
                        .arg(static_cast<int>(s.state))
                        .arg(s.positionMs)
                        .arg(s.durationMs)
                        .arg(s.sending ? 1 : 0)
                        .arg(s.mixing ? 1 : 0)
                        .arg(s.monitoring ? 1 : 0)
                        .arg(s.underruns)
                        .arg(e->connection());
        }
        line += QString::fromLatin1(" waitingForItsTab %1 vadOff %2").arg(music->waitingForItsTab() ? 1 : 0).arg(music->voiceActivationPaused() ? 1 : 0); // 2.3 polish
        line += QString::fromLatin1(" inputOpen %1 volume %2 muted %3 bar %4 problem \"%5\" devicePeak %6 deviceBlocks %7 | %8")
                    .arg(music->inputKeptOpen() ? 1 : 0)
                    .arg(music->volume())
                    .arg(music->muted() ? 1 : 0)
                    .arg(music->bar() && music->bar()->isVisible() ? 1 : 0)
                    .arg(music->problem())
                    .arg(g_peak.load())
                    .arg(g_blocks.load())
                    .arg(captureLine(sch));
        say(line);
        if (c.value(QString::fromLatin1("resetPeak")).toBool())
            g_peak = 0;
        return true;
    }
    if (cmd == QLatin1String("music.volume")) {
        if (music)
            music->setVolume(c.value(QString::fromLatin1("value")).toInt(0));
        say(QString::fromLatin1("music.volume: %1").arg(music ? music->volume() : -1));
        return true;
    }
    if (cmd == QLatin1String("music.signal")) {
        g_signal = signalOf(str("signal", "silence"));
        g_amp    = qBound(0, c.value(QString::fromLatin1("amp")).toInt(g_amp.load()), 900);
        say(QString::fromLatin1("music.signal: %1 amp %2").arg(signalName(g_signal.load())).arg(g_amp.load()));
        return true;
    }
    if (cmd == QLatin1String("music.vad")) { // this connection's voice activation (runtime only), on the custom device only
        if (g_deviceSch.load() != sch || !g_customVerified.load() || !ts3::funcs.setPreProcessorConfigValue) {
            say(QString::fromLatin1("music.vad: refused (not on the custom test device)"));
            return true;
        }
        const unsigned e1 = ts3::funcs.setPreProcessorConfigValue(sch, "vad", c.value(QString::fromLatin1("on")).toBool(true) ? "true" : "false");
        unsigned       e2 = 0;
        if (c.contains(QString::fromLatin1("level")))
            e2 = ts3::funcs.setPreProcessorConfigValue(sch, "voiceactivation_level", QByteArray::number(c.value(QString::fromLatin1("level")).toInt(-30)).constData());
        say(QString::fromLatin1("music.vad: set (errors %1/%2): %3").arg(e1).arg(e2).arg(devicesLine(sch)));
        return true;
    }
    if (cmd == QLatin1String("capstats")) {
        say(QString::fromLatin1("capstats: sch %1 %2").arg(sch).arg(captureLine(sch)));
        if (c.value(QString::fromLatin1("reset")).toBool())
            resetCapture(sch);
        return true;
    }
    if (cmd == QLatin1String("voicerx")) {
        const qint64 now = nowMs();
        int          n   = 0;
        for (ClientSlot& s : g_clients) {
            const quint64 key = s.key.load();
            if (!key)
                continue;
            ++n;
            const qint64 last    = s.lastMs.load();
            const qint64 changed = s.talkChangedMs.load();
            say(QString::fromLatin1("voicerx: sch %1 client %2 \"%3\" blocks %4 loud %5 rms %6 peak %7 last %8 ms ago | talk %9 on %10 off %11 changed %12 ms ago")
                    .arg(key >> 16)
                    .arg(key & 0xffff)
                    .arg(nickOf(key >> 16, static_cast<anyID>(key & 0xffff)))
                    .arg(s.blocks.load())
                    .arg(s.loud.load())
                    .arg(rms(s.meanSq.load(), s.blocks.load()))
                    .arg(s.peak.load())
                    .arg(last ? now - last : -1)
                    .arg(s.talk.load())
                    .arg(s.on.load())
                    .arg(s.off.load())
                    .arg(changed ? now - changed : -1));
            if (c.value(QString::fromLatin1("reset")).toBool()) {
                s.blocks = 0;
                s.meanSq = 0;
                s.loud   = 0;
                s.peak   = 0;
                s.on     = 0;
                s.off    = 0;
            }
        }
        if (!n)
            say(QString::fromLatin1("voicerx: no voice received, no talk status seen"));
        return true;
    }
    if (cmd == QLatin1String("music.devices")) {
        if (c.value(QString::fromLatin1("query")).toBool()) {
            say(QString::fromLatin1("music.devices: ") + devicesLine(sch));
            return true;
        }
        if (!c.value(QString::fromLatin1("on")).toBool(true)) {
            stopDevices(say, c.value(QString::fromLatin1("playback")).toBool(false));
            say(QString::fromLatin1("music.devices: now ") + devicesLine(sch));
            return true;
        }
        if (g_deviceSch.load() || !ts3::funcs.registerCustomDevice) {
            say(QString::fromLatin1("music.devices: already on, or no custom devices"));
            return true;
        }
        if (c.contains(QString::fromLatin1("signal")))
            g_signal = signalOf(str("signal", "silence"));
        if (c.contains(QString::fromLatin1("amp")))
            g_amp = qBound(0, c.value(QString::fromLatin1("amp")).toInt(300), 900);
        const unsigned r  = ts3::funcs.registerCustomDevice(kDevice, kDeviceLabel, 48000, 1, 48000, 2);
        const unsigned c1 = ts3::funcs.closeCaptureDevice(sch);
        const unsigned c2 = ts3::funcs.closePlaybackDevice(sch);
        const unsigned o1 = ts3::funcs.openCaptureDevice(sch, "custom", kDevice);
        const unsigned o2 = ts3::funcs.openPlaybackDevice(sch, "custom", kDevice);
        g_deviceSch       = sch;
        g_customVerified  = isCustomCapture(sch);
        g_peak            = 0;
        g_blocks          = 0;
        auto* pump        = new QTimer(owner); // a child of the driver: it goes with it
        pump->setTimerType(Qt::PreciseTimer);
        pump->setInterval(20);
        QObject::connect(pump, &QTimer::timeout, pump, [] {
            static std::vector<short> captured(kBlock, 0);
            static std::vector<short> played(kBlock * 2, 0);
            const quint64             device = g_deviceSch.load();
            // The guard lets this connection's capture through only while its device is really ours.
            g_customVerified = device && isCustomCapture(device);
            fillSignal(&captured);
            if (ts3::funcs.processCustomCaptureData)
                ts3::funcs.processCustomCaptureData(kDevice, captured.data(), kBlock);
            if (ts3::funcs.acquireCustomPlaybackData && ts3::funcs.acquireCustomPlaybackData(kDevice, played.data(), kBlock) == ERROR_ok) {
                int top = 0;
                for (short s : played)
                    top = qMax(top, std::abs(static_cast<int>(s)));
                atomicMax(g_peak, top);
            }
            ++g_blocks;
        });
        pump->start();
        g_pump = pump;
        say(QString::fromLatin1("music.devices: on (register %1, close %2/%3, open %4/%5)").arg(r).arg(c1).arg(c2).arg(o1).arg(o2));
        say(QString::fromLatin1("music.devices: ") + devicesLine(sch));
        return true;
    }
    if (cmd == QLatin1String("music.bar")) {
        MusicBar*     bar    = music ? music->bar() : nullptr;
        const QString action = str("action", "log");
        if (!bar) {
            say(QString::fromLatin1("music.bar: no bar"));
            return true;
        }
        const auto clickOn = [&say, &action](QWidget* w) {
            auto* b = qobject_cast<QAbstractButton*>(w);
            say(QString::fromLatin1("music.bar: %1 %2").arg(action, b && b->isVisible() ? QString::fromLatin1("clicked \"") + b->accessibleName() + QLatin1Char('"') : QString::fromLatin1("not shown")));
            if (b && b->isVisible())
                b->click();
        };
        if (action == QLatin1String("play")) {
            clickOn(bar->playButton());
        } else if (action == QLatin1String("stop")) {
            clickOn(bar->stopButton());
        } else if (action == QLatin1String("mute")) {
            clickOn(bar->volumeButton());
        } else if (action == QLatin1String("close")) {
            clickOn(bar->closeButton());
        } else if (action == QLatin1String("seek")) { // a drag of the position slider to ms, then let go
            auto* s = qobject_cast<QAbstractSlider*>(bar->seekSlider());
            if (s && s->isVisible()) {
                const int ms = c.value(QString::fromLatin1("ms")).toInt(0);
                s->setSliderDown(true);
                s->setSliderPosition(ms);
                s->setSliderDown(false);
                say(QString::fromLatin1("music.bar: seek dragged to %1 (range %2..%3)").arg(ms).arg(s->minimum()).arg(s->maximum()));
            } else {
                say(QString::fromLatin1("music.bar: seek slider not shown"));
            }
        } else if (action == QLatin1String("volume")) {
            auto* s = qobject_cast<QAbstractSlider*>(bar->volumeSlider());
            if (s && s->isVisible()) {
                s->setValue(c.value(QString::fromLatin1("value")).toInt(50));
                say(QString::fromLatin1("music.bar: volume slider set to %1").arg(s->value()));
            } else {
                say(QString::fromLatin1("music.bar: volume slider not shown"));
            }
        } else if (action == QLatin1String("key")) { // a key on the position slider (Right, Left, PgUp, PgDown, Home, End)
            QWidget*      s   = bar->seekSlider();
            const QString k   = str("key", "Right");
            const int     key = k == QLatin1String("Left")     ? Qt::Key_Left
                              : k == QLatin1String("PgUp")     ? Qt::Key_PageUp
                              : k == QLatin1String("PgDown")   ? Qt::Key_PageDown
                              : k == QLatin1String("Home")     ? Qt::Key_Home
                              : k == QLatin1String("End")      ? Qt::Key_End
                                                               : Qt::Key_Right;
            if (s && s->isVisible()) {
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                QApplication::sendEvent(s, &press);
                QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
                QApplication::sendEvent(s, &release);
            }
            say(QString::fromLatin1("music.bar: key %1 on the position slider (%2)").arg(k, s && s->isVisible() ? QString::fromLatin1("shown") : QString::fromLatin1("not shown")));
        }
        say(QString::fromLatin1("music.bar: ") + barLine(bar));
        if (!str("name", "").isEmpty()) {
            QWidget* target = bar;
            for (int i = 0; i < c.value(QString::fromLatin1("up")).toInt(0) && target->parentWidget(); ++i)
                target = target->parentWidget();
            if (target->isVisible())
                say(QString::fromLatin1("music.bar: grab %1 -> %2").arg(rectText(target->geometry()), saveGrabOf(target, str("name", "bar"))));
            else
                say(QString::fromLatin1("music.bar: not visible, no grab"));
        }
        return true;
    }
    if (cmd == QLatin1String("attach.state")) {
        QTextEdit* edit = visibleChatInput();
        if (!edit) {
            say(QString::fromLatin1("attach.state: no visible chat input"));
            return true;
        }
        QStringList buttons;
        for (QAbstractButton* b : edit->findChildren<QAbstractButton*>()) {
            if (b->isVisible())
                buttons << QString::fromLatin1("%1#%2 %3").arg(QString::fromLatin1(b->metaObject()->className()), b->objectName(), rectText(QRect(b->mapTo(edit, QPoint(0, 0)), b->size())));
        }
        const QRect viewport(edit->viewport()->mapTo(edit, QPoint(0, 0)), edit->viewport()->size());
        const QRect caret = edit->cursorRect(QTextCursor(edit->document()));
        say(QString::fromLatin1("attach.state: input %1x%2 viewport %3 text starts at x %4 (input coords) | buttons [%5]")
                .arg(edit->width())
                .arg(edit->height())
                .arg(rectText(viewport))
                .arg(caret.left() + viewport.left())
                .arg(buttons.join(QString::fromLatin1("; "))));
        if (!str("row", "").isEmpty()) { // the input with what is around it (up: that many parents)
            QWidget* target = edit;
            for (int i = 0; i < c.value(QString::fromLatin1("up")).toInt(1) && target->parentWidget(); ++i)
                target = target->parentWidget();
            say(QString::fromLatin1("attach.state: row grab %1 -> %2").arg(rectText(target->geometry()), saveGrabOf(target, str("row", "row"))));
        }
        if (AttachPopup* popup = AttachPopup::current()) {
            QStringList items;
            for (int i = 0; i < popup->count(); ++i) {
                QAbstractButton* item = popup->item(i);
                const QString    text = item ? (item->accessibleName().isEmpty() ? item->text() : item->accessibleName()) : QString();
                items << QString::fromLatin1("%1:%2 \"%3\"%4 %5")
                             .arg(i)
                             .arg(popup->entryId(i), text)
                             .arg(item && !item->isEnabled() ? QString::fromLatin1(" (off)") : QString())
                             .arg(item ? rectText(item->geometry()) : QString());
            }
            AttachButton* clip   = AttachButton::of(edit);
            const QPoint  anchor = clip ? clip->mapToGlobal(QPoint(0, 0)) : QPoint();
            say(QString::fromLatin1("attach.state: menu open at %1 (paperclip at %2,%3 global), current %4, keyboard %5, focus %6 | %7")
                    .arg(rectText(popup->geometry()))
                    .arg(anchor.x())
                    .arg(anchor.y())
                    .arg(popup->currentIndex())
                    .arg(popup->keyboardUsed() ? 1 : 0)
                    .arg(QApplication::focusWidget() ? QString::fromLatin1(QApplication::focusWidget()->metaObject()->className()) : QString::fromLatin1("-"))
                    .arg(items.join(QString::fromLatin1(" | "))));
            if (!str("name", "").isEmpty())
                say(QString::fromLatin1("attach.state: menu grab -> ") + saveGrabOf(popup, str("name", "menu")));
        } else {
            say(QString::fromLatin1("attach.state: menu closed"));
        }
        return true;
    }
    if (cmd == QLatin1String("listpick")) { // a row of a list in a visible window (e.g. Options > Addons)
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (!top->isVisible() || !top->windowTitle().contains(str("window", ""), Qt::CaseInsensitive))
                continue;
            for (QListWidget* list : top->findChildren<QListWidget*>()) {
                for (int i = 0; i < list->count(); ++i) {
                    if (list->item(i)->text() == str("text", "")) {
                        list->setCurrentRow(i);
                        say(QString::fromLatin1("listpick: \"%1\" in %2#%3").arg(list->item(i)->text(), top->windowTitle(), list->objectName()));
                        return true;
                    }
                }
            }
        }
        say(QString::fromLatin1("listpick: no row \"%1\"").arg(str("text", "")));
        return true;
    }
    if (cmd == QLatin1String("filedlg")) {
        const QString action = str("action", "log");
        HWND          dialog = nativeDialog(); // Windows' own chooser (Qt then keeps a small hidden stand-in)
        if (QFileDialog* qd = dialog ? nullptr : qtFileDialog()) {
            say(QString::fromLatin1("filedlg: Qt dialog \"%1\" dir \"%2\"").arg(qd->windowTitle(), qd->directory().absolutePath()));
            if (action == QLatin1String("set")) {
                if (auto* field = qd->findChild<QLineEdit*>(QString::fromLatin1("fileNameEdit")))
                    field->setText(str("text", ""));
                say(QString::fromLatin1("filedlg: Qt file name set"));
            } else if (action == QLatin1String("ok")) {
                QMetaObject::invokeMethod(qd, "accept", Qt::QueuedConnection);
            } else if (action == QLatin1String("cancel")) {
                QMetaObject::invokeMethod(qd, "reject", Qt::QueuedConnection);
            }
            return true;
        }
        if (!dialog) {
            say(QString::fromLatin1("filedlg: no file dialog open"));
            return true;
        }
        HWND edit  = fileNameEdit(dialog);
        HWND owned = GetWindow(dialog, GW_OWNER);
        RECT r{};
        GetWindowRect(dialog, &r);
        say(QString::fromLatin1("filedlg: native dialog \"%1\" %2x%3 owner \"%4\" file name box %5 \"%6\"")
                .arg(windowText(dialog))
                .arg(r.right - r.left)
                .arg(r.bottom - r.top)
                .arg(owned ? windowText(owned) : QString::fromLatin1("-"))
                .arg(edit ? 1 : 0)
                .arg(edit ? windowText(edit) : QString()));
        if (action == QLatin1String("set") && edit) {
            const std::wstring text = str("text", "").toStdWString();
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
            say(QString::fromLatin1("filedlg: file name box now \"%1\"").arg(windowText(edit)));
        } else if (action == QLatin1String("ok")) {
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDOK)));
            say(QString::fromLatin1("filedlg: Open posted"));
        } else if (action == QLatin1String("cancel")) {
            PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDCANCEL)));
            say(QString::fromLatin1("filedlg: Cancel posted"));
        }
        return true;
    }
    return false;
}

bool selfTestCustomDevicesOn()
{
    return g_deviceSch.load() != 0;
}

// Plugin shutdown: the custom devices go before the DLL does (nothing feeds them after). TeamSpeak's capture
// stays closed (never a real microphone from a test build).
void selfTestMusicShutdown()
{
    stopDevices([](const QString& text) { ts3::log(QString::fromLatin1("[test] ") + text); }, false);
}

#endif
