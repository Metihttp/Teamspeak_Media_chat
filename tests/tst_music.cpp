// 2.3 live music: the sound side. Nothing is ever played and no microphone is opened: the "voice" blocks
// are arrays the tests fill and read, the track is generated (musicfakes.h) or a WAV / M4A file written here.
//   mixing      the edited flags, soft clipping, mono / stereo / more channels, the microphone's ramp,
//               TeamSpeak's playback with speaker layouts and fill masks, fades, the volume curve;
//   engine      the worker decodes ahead; the send side leads and the monitor follows (snapped back when
//               apart), a side that comes back starts where the other is; pause fades out and resumes in
//               place; seek lands on the frame; the end; failures; while the plugin keeps a push-to-talk
//               input open, never the microphone (blocks without music are silence, not sent); a muted
//               client never made to send; the callbacks only for the active engine's connection; the
//               source lives on the worker; callbacks racing a teardown;
//   decoding    Media Foundation: 44.1 kHz mono WAV -> 48 kHz stereo (length, pitch), channel order,
//               seeking, AAC (.m4a), a file that is no sound at all;
//   2.3 review  a short capture gap never pulls the monitor back, volume / mute ramped (no click), the
//               sound muted: nothing added here, Stop's fade out, 8 / 22.05 / 96 kHz, 24-bit and 5.1 files.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "audio/aacwriter.h"
#include "music/musicengine.h"
#include "music/musicmix.h"
#include "musicfakes.h"
#include "testmain.h"
#include "video/mfmusicreader.h"
#include "video/mfvideo.h"

namespace {

using musicfakes::Clock;
using musicfakes::GenSource;
using musicfakes::gen;
using Engine = music::Engine;

constexpr float kEps = 1.0f / 32768.0f + 1e-6f;

// The first block a side gets once the worker has decoded enough (a start or a seek), then a wait until
// a second more is decoded (or the end), so the test's next blocks never find the decoder behind.
int firstTake(Engine& e, Engine::Side side, float* out, int frames)
{
    int n = 0;
    for (int i = 0; i < 5000 && n <= 0; ++i) {
        n = e.take(side, out, frames);
        if (n <= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Half a second beyond where it plays now (the worker decodes two seconds ahead at most).
    const qint64 want = e.snapshot().positionMs * 48 + 24000;
    for (int i = 0; i < 5000; ++i) {
        bool all = false;
        if (e.decodedFrames(&all) >= want || all || e.snapshot().state == Engine::State::Failed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return n;
}

// A 16-bit PCM WAV file.
QByteArray wav(const std::vector<qint16>& samples, int rate, int channels)
{
    QByteArray out;
    const auto u32 = [&out](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        out.append(b, 4);
    };
    const auto u16 = [&out](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        out.append(b, 2);
    };
    const quint32 data = static_cast<quint32>(samples.size() * 2);
    out.append("RIFF", 4);
    u32(36 + data);
    out.append("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<quint16>(channels));
    u32(static_cast<quint32>(rate));
    u32(static_cast<quint32>(rate * channels * 2));
    u16(static_cast<quint16>(channels * 2));
    u16(16);
    out.append("data", 4);
    u32(data);
    for (qint16 s : samples)
        u16(static_cast<quint16>(s));
    return out;
}

std::vector<qint16> sine(double hz, int rate, double seconds, double amplitude, int channels, int silentChannel = -1)
{
    const int           frames = static_cast<int>(rate * seconds);
    std::vector<qint16> s(static_cast<size_t>(frames * channels));
    for (int f = 0; f < frames; ++f) {
        const qint16 v = static_cast<qint16>(std::lround(amplitude * 32767.0 * std::sin(2.0 * M_PI * hz * f / rate)));
        for (int c = 0; c < channels; ++c)
            s[static_cast<size_t>(f * channels + c)] = c == silentChannel ? qint16(0) : v;
    }
    return s;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

// Every frame of a file, decoded as the engine's worker would.
std::vector<float> decodeAll(const QString& path, mf::MusicInfo* info = nullptr, QString* error = nullptr)
{
    mf::ThreadScope    scope;
    mf::MusicReader    reader;
    std::vector<float> all;
    if (!reader.open(path)) {
        if (error)
            *error = reader.error();
        return all;
    }
    if (info)
        *info = reader.info();
    for (int guard = 0; guard < 100000; ++guard) {
        qint64 first = -1;
        const auto r = reader.read(&all, &first);
        if (r != mf::MusicReader::Read::Data)
            break;
    }
    return all;
}

int zeroCrossings(const std::vector<float>& stereo, int channel, size_t fromFrame, size_t frames)
{
    int count = 0;
    for (size_t f = fromFrame + 1; f < fromFrame + frames && 2 * f + 1 < stereo.size(); ++f) {
        const float a = stereo[2 * (f - 1) + channel], b = stereo[2 * f + channel];
        if ((a < 0.0f) != (b < 0.0f))
            ++count;
    }
    return count;
}

} // namespace

class TestMusic : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase() { m_saved = ts3::funcs; }
    void cleanupTestCase() { ts3::funcs = m_saved; }
    void cleanup() { QVERIFY(!music::active()); } // every test detaches its engine

    // ---- mixing ------------------------------------------------------------------------------------------

    void editedFlags()
    {
        using music::decideCapture;
        // No music: TeamSpeak's own decision, untouched.
        for (int e : {0, 1, 2, 3}) {
            const auto d = decideCapture(e, false, true);
            QVERIFY(!d.mix);
            QCOMPARE(d.edited, e);
        }
        // Music while voice activation hears speech (2 on entry): the voice stays, changed + sent.
        auto d = decideCapture(music::kEditedSend, true, true);
        QVERIFY(d.mix);
        QCOMPARE(d.micGain, 1.0f);
        QCOMPARE(d.edited, music::kEditedChanged | music::kEditedSend);
        // Music between words (voice activation silent): sent anyway, the room left out.
        d = decideCapture(0, true, true);
        QCOMPARE(d.micGain, 0.0f);
        QCOMPARE(d.edited, music::kEditedChanged | music::kEditedSend);
        // The plugin keeps a push-to-talk input open: never the microphone.
        d = decideCapture(music::kEditedSend, true, false);
        QCOMPARE(d.micGain, 0.0f);
        QCOMPARE(d.edited, 3);
        QVERIFY(!d.silence);
        // ... and a block without music then is silence, not sent (the input is open only for the music).
        for (int e : {0, 1, 2, 3, 0x12}) {
            d = decideCapture(e, false, false);
            QVERIFY(!d.mix);
            QVERIFY(d.silence);
            QCOMPARE(d.micGain, 0.0f);
            QCOMPARE(d.edited, (e | music::kEditedChanged) & ~music::kEditedSend);
        }
        QVERIFY(!decideCapture(music::kEditedSend, false, true).silence);
        // Other bits TeamSpeak may set stay.
        QCOMPARE(decideCapture(0x10 | music::kEditedSend, true, true).edited, 0x13);
        // Samples another plugin changed (value 1) still carry the voice: never cut off by how the bits are read.
        QCOMPARE(decideCapture(music::kEditedChanged, true, true).micGain, 1.0f);
    }

    void softClipAndPcm()
    {
        using music::softClip;
        QCOMPARE(softClip(0.0f), 0.0f);
        QCOMPARE(softClip(0.5f), 0.5f);
        QCOMPARE(softClip(-music::kKnee), -music::kKnee);
        float last = 0.0f;
        for (float x = 0.0f; x < 20.0f; x += 0.01f) { // monotonic, never full scale
            const float y = softClip(x);
            QVERIFY2(y >= last - 1e-7f, qPrintable(QString::number(x)));
            QVERIFY(y < 1.0f);
            QCOMPARE(softClip(-x), -y);
            last = y;
        }
        QCOMPARE(softClip(std::nanf("")), 0.0f);
        QCOMPARE(softClip(INFINITY), 0.0f);
        QCOMPARE(music::toPcm16(1.0f), qint16(32767));
        QCOMPARE(music::toPcm16(-1.0f), qint16(-32768));
        QCOMPARE(music::toPcm16(-2.0f), qint16(-32768));
        QCOMPARE(music::toPcm16(0.5f), qint16(16384));
        QCOMPARE(music::toPcm16(std::nanf("")), qint16(-32768)); // never reached: softClip removed it
        // Every 16-bit sample comes back unchanged below the knee (the voice isn't touched without music).
        for (int s = -24000; s <= 24000; s += 7)
            QCOMPARE(int(music::toPcm16(softClip(static_cast<float>(s) / 32768.0f))), s);
    }

    void volumeCurve()
    {
        QCOMPARE(music::volumeGain(0), 0.0f);
        QCOMPARE(music::volumeGain(100), 1.0f);
        QCOMPARE(music::volumeGain(50), 0.25f); // -12 dB
        QCOMPARE(music::volumeGain(-5), 0.0f);
        QCOMPARE(music::volumeGain(500), 1.0f);
        QVERIFY(music::volumeGain(20) < music::volumeGain(21));
    }

    void captureChannels()
    {
        // Mono: the music's average on top of the voice.
        std::vector<float>  m{0.2f, 0.4f, 0.2f, 0.4f};
        std::vector<qint16> mono{8192, 8192};
        music::mixCapture(mono.data(), 2, 1, m.data(), 2, 1.0f, 1.0f, 1.0f);
        QCOMPARE(int(mono[0]), int(music::toPcm16(0.25f + 0.3f)));
        QCOMPARE(int(mono[1]), int(music::toPcm16(0.25f + 0.3f)));
        // Stereo: left and right.
        std::vector<qint16> st{0, 0, 0, 0};
        music::mixCapture(st.data(), 2, 2, m.data(), 2, 1.0f, 1.0f, 1.0f);
        QCOMPARE(int(st[0]), int(music::toPcm16(0.2f)));
        QCOMPARE(int(st[1]), int(music::toPcm16(0.4f)));
        // Three channels: left, right, then the average.
        std::vector<qint16> three(6, 0);
        music::mixCapture(three.data(), 2, 3, m.data(), 2, 0.5f, 1.0f, 1.0f);
        QCOMPARE(int(three[0]), int(music::toPcm16(0.1f)));
        QCOMPARE(int(three[1]), int(music::toPcm16(0.2f)));
        QCOMPARE(int(three[2]), int(music::toPcm16(0.15f)));
        // Less music than voice (the decoder fell behind): the rest is the voice alone.
        std::vector<qint16> part{1000, 1000, 1000};
        music::mixCapture(part.data(), 3, 1, m.data(), 1, 1.0f, 1.0f, 1.0f);
        QVERIFY(part[0] > 1000);
        QCOMPARE(int(part[1]), 1000);
        QCOMPARE(int(part[2]), 1000);
        // Bad arguments do nothing.
        music::mixCapture(nullptr, 2, 1, m.data(), 2, 1.0f, 1.0f, 1.0f);
        music::mixCapture(mono.data(), 0, 1, m.data(), 2, 1.0f, 1.0f, 1.0f);
        music::mixCapture(mono.data(), 2, 0, m.data(), 2, 1.0f, 1.0f, 1.0f);
    }

    void captureMicRampAndHeadroom()
    {
        // Voice activation closes mid-block: the voice fades from full to nothing across it (no click).
        const int           n = 480;
        std::vector<qint16> voice(n, 16000);
        std::vector<float>  silence(2 * n, 0.0f);
        music::mixCapture(voice.data(), n, 1, silence.data(), n, 1.0f, 1.0f, 0.0f);
        QCOMPARE(int(voice.front()), 16000);
        QCOMPARE(int(voice.back()), 0);
        for (int i = 1; i < n; ++i)
            QVERIFY(voice[i] <= voice[i - 1]);
        // Full-scale voice plus full-scale music: bent, never wrapped round or stuck at the rail.
        std::vector<qint16> loud(n, 32767);
        std::vector<float>  full(2 * n, 1.0f);
        music::mixCapture(loud.data(), n, 1, full.data(), n, 1.0f, 1.0f, 1.0f);
        for (qint16 s : loud)
            QVERIFY(s > 30000 && s <= 32767);
        std::vector<qint16> neg(n, -32768);
        std::vector<float>  negFull(2 * n, -1.0f);
        music::mixCapture(neg.data(), n, 1, negFull.data(), n, 1.0f, 1.0f, 1.0f);
        for (qint16 s : neg)
            QVERIFY(s < -30000);
        // Opposite signs cancel instead of clipping.
        std::vector<qint16> mixed(n, -16384);
        std::vector<float>  pos(2 * n, 0.5f);
        music::mixCapture(mixed.data(), n, 1, pos.data(), n, 1.0f, 1.0f, 1.0f);
        for (qint16 s : mixed)
            QVERIFY(std::abs(s) <= 1);
    }

    void playbackSpeakers()
    {
        std::vector<float> m{0.2f, 0.4f};
        // Stereo, nothing playing yet (undefined samples, no fill bits): the music alone, both bits set.
        std::vector<qint16> st{12345, -777};
        unsigned            stereo[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        unsigned            mask     = 0;
        music::mixPlayback(st.data(), 1, 2, stereo, &mask, m.data(), 1, 1.0f);
        QCOMPARE(int(st[0]), int(music::toPcm16(0.2f)));
        QCOMPARE(int(st[1]), int(music::toPcm16(0.4f)));
        QCOMPARE(mask, 3u);
        // Somebody talks (filled): added.
        std::vector<qint16> talk{8192, 8192};
        mask = 3;
        music::mixPlayback(talk.data(), 1, 2, stereo, &mask, m.data(), 1, 1.0f);
        QCOMPARE(int(talk[0]), int(music::toPcm16(0.25f + 0.2f)));
        QCOMPARE(int(talk[1]), int(music::toPcm16(0.25f + 0.4f)));
        // 5.1: only the front left and right get it; the other channels and their bits stay.
        unsigned            surround[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER, SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT};
        std::vector<qint16> six{0, 0, 111, 222, 333, 444};
        mask = 0x3c;
        music::mixPlayback(six.data(), 1, 6, surround, &mask, m.data(), 1, 1.0f);
        QCOMPARE(int(six[0]), int(music::toPcm16(0.2f)));
        QCOMPARE(int(six[1]), int(music::toPcm16(0.4f)));
        QCOMPARE(int(six[2]), 111);
        QCOMPARE(int(six[5]), 444);
        QCOMPARE(mask, 0x3fu);
        // Mono output (no speaker list): the average.
        std::vector<qint16> one{0};
        mask = 0;
        music::mixPlayback(one.data(), 1, 1, nullptr, &mask, m.data(), 1, 1.0f);
        QCOMPARE(int(one[0]), int(music::toPcm16(0.3f)));
        QCOMPARE(mask, 1u);
        // Headphones and a centre-only layout.
        unsigned            phones[] = {SPEAKER_HEADPHONES_LEFT, SPEAKER_HEADPHONES_RIGHT};
        std::vector<qint16> hp{0, 0};
        music::mixPlayback(hp.data(), 1, 2, phones, nullptr, m.data(), 1, 0.5f);
        QCOMPARE(int(hp[0]), int(music::toPcm16(0.1f)));
        QCOMPARE(int(hp[1]), int(music::toPcm16(0.2f)));
        unsigned            centre[] = {SPEAKER_LOW_FREQUENCY, SPEAKER_FRONT_CENTER};
        std::vector<qint16> c2{5, 0};
        mask = 0;
        music::mixPlayback(c2.data(), 1, 2, centre, &mask, m.data(), 1, 1.0f);
        QCOMPARE(int(c2[0]), 5);
        QCOMPARE(int(c2[1]), int(music::toPcm16(0.3f)));
        QCOMPARE(mask, 2u);
        // Half a pair: that side gets both.
        unsigned            leftOnly[] = {SPEAKER_FRONT_LEFT};
        std::vector<qint16> l1{0};
        music::mixPlayback(l1.data(), 1, 1, leftOnly, nullptr, m.data(), 1, 1.0f);
        QCOMPARE(int(l1[0]), int(music::toPcm16(0.3f)));
    }

    void fades()
    {
        std::vector<float> in(2 * 5, 1.0f), out(2 * 5, 1.0f);
        music::fade(in.data(), 5, true);
        music::fade(out.data(), 5, false);
        QCOMPARE(in[0], 0.0f);
        QCOMPARE(in[9], 1.0f);
        QCOMPARE(out[0], 1.0f);
        QCOMPARE(out[9], 0.0f);
        QVERIFY(in[4] > in[2] && out[4] < out[2]);
    }

    // ---- the engine ------------------------------------------------------------------------------------------

    void sendLeadsMonitorFollows()
    {
        Clock  clock;
        auto   log = std::make_shared<musicfakes::SourceLog>();
        GenSource::Options o;
        o.log = log;
        Engine e(1, gen(o), clock.fn());
        QCOMPARE(e.snapshot().state, Engine::State::Opening);
        std::vector<float> buf(2 * 960);
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 0); // not started
        e.start();
        QCOMPARE(firstTake(e, Engine::Send, buf.data(), 960), 960);
        // Faded in over the first 5 ms, then the track exactly.
        QCOMPARE(buf[0], 0.0f);
        QCOMPARE(buf[2 * 300], GenSource::left(300));
        QCOMPARE(buf[2 * 300 + 1], -GenSource::left(300));
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[0], GenSource::left(960));
        QCOMPARE(e.snapshot().positionMs, qint64(40));
        // The monitor comes in where the send side is.
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(1920 + 300));
        const Engine::Snapshot s = e.snapshot();
        QCOMPARE(s.state, Engine::State::Playing);
        QVERIFY(s.sending && s.monitoring);
        QCOMPARE(s.durationMs, qint64(5000));
        QCOMPARE(s.positionMs, qint64(40)); // the send side leads
        {
            std::lock_guard<std::mutex> lock(log->lock);
            QVERIFY(log->openThread != std::this_thread::get_id()); // the worker opened it
        }
    }

    void monitorSnapsBackWhenApart()
    {
        Clock              clock;
        Engine             e(1, gen(), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960); // monitor at 960..1920
        for (int i = 0; i < 4; ++i)                              // the send side 4 blocks ahead: within 100 ms
            QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        QCOMPARE(buf[0], GenSource::left(1920)); // 3840 apart: it plays on
        for (int i = 0; i < 8; ++i)
            QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960); // send now at 12480, monitor at 2880
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(12480 + 300)); // snapped (and faded in again)
        QCOMPARE(buf[0], 0.0f);
    }

    void aSideComingBackStartsWhereTheOtherIs()
    {
        Clock              clock;
        Engine             e(1, gen(), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        // Push-to-talk released: TeamSpeak stops capturing; the monitor goes on and leads.
        for (int i = 0; i < 10; ++i) {
            clock.advance(40);
            QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        }
        Engine::Snapshot s = e.snapshot();
        QVERIFY(!s.sending);
        QVERIFY(s.monitoring);
        QCOMPARE(s.positionMs, qint64((960 + 11 * 960) * 1000 / 48000));
        // Capturing again: the channel hears the music from where it is now, not from where it left.
        clock.advance(10);
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(12 * 960 + 300));
        // Neither side called for a while: the position stays.
        clock.advance(5000);
        s = e.snapshot();
        QVERIFY(!s.sending && !s.monitoring);
        QCOMPARE(s.positionMs, qint64(13 * 960 * 1000 / 48000));
    }

    void pauseFadesOutAndResumesInPlace()
    {
        Clock              clock;
        Engine             e(1, gen(), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        e.pause();
        QCOMPARE(e.snapshot().state, Engine::State::Paused);
        const int n = e.take(Engine::Send, buf.data(), 960);
        QCOMPARE(n, Engine::kFadeFrames); // a short fade out
        QCOMPARE(buf[0], GenSource::left(1920));
        QCOMPARE(buf[2 * (n - 1)], 0.0f);
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 0);
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 0);
        QCOMPARE(e.snapshot().positionMs, qint64(40));
        e.resume();
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[0], 0.0f); // faded in
        QCOMPARE(buf[2 * 300], GenSource::left(1920 + 300));
        // Paused while still opening: it starts paused.
        Engine early(1, gen(), clock.fn());
        early.pause();
        early.start();
        QTRY_COMPARE(early.snapshot().state, Engine::State::Paused);
        QCOMPARE(early.take(Engine::Send, buf.data(), 960), 0);
    }

    void seekLandsOnTheFrame()
    {
        Clock              clock;
        auto               log = std::make_shared<musicfakes::SourceLog>();
        GenSource::Options o;
        o.log = log;
        Engine             e(1, gen(o), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        e.seek(2000);
        QCOMPARE(e.snapshot().positionMs, qint64(2000)); // at once, before the worker got there
        QCOMPARE(firstTake(e, Engine::Send, buf.data(), 960), 960);
        QCOMPARE(log->seeks.load(), 1);
        QCOMPARE(buf[2 * 300], GenSource::left(96000 + 300)); // the source landed at 95616: skipped to the frame
        QCOMPARE(e.snapshot().positionMs, qint64(2020));
        // Backwards, then past the end (clamped to the end).
        e.seek(100);
        QCOMPARE(firstTake(e, Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(4800 + 300));
        e.seek(99999);
        QCOMPARE(e.snapshot().positionMs, qint64(5000));
        // Seeks in a row: the last one counts.
        e.seek(1000);
        e.seek(3000);
        QCOMPARE(firstTake(e, Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(144000 + 300));
    }

    // A file's header may claim any duration (damaged or made up), and a seek may ask for any place. The
    // place is clamped in milliseconds before it becomes frames: before, ms * 48000 and durationMs * 48000
    // were signed overflows (undefined; a negative place, and once decoded there the ring was written before
    // its start). The source refuses seeks here, so a wrong place is never decoded in this test.
    // 2.3 polish: the source holds the worker in its seek while the test looks at the pending place. Before,
    // the test read it right after seek() and raced the worker: once (a full-suite run) the worker had already
    // been refused and gone back to where it was ("positionMs 20 vs 2000"). Also the place shown while that
    // seek to the claimed end was pending: its milliseconds overflowed (frames * 1000 beyond 2^63).
    void seekWithAnAbsurdDuration()
    {
        Clock              clock;
        auto               log  = std::make_shared<musicfakes::SourceLog>();
        auto               gate = std::make_shared<musicfakes::Gate>();
        GenSource::Options o;
        o.log       = log;
        o.seekGate  = gate;
        o.canSeek   = false;
        o.claimedMs = 250000000000000LL; // about 7900 years: times 48000 is beyond 2^63
        Engine             e(1, gen(o), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        QCOMPARE(e.snapshot().durationMs, o.claimedMs);
        const qint64 before = e.snapshot().positionMs; // 960 frames played: 20 ms
        QCOMPARE(before, qint64(20));
        gate->hold();
        e.seek(2000);
        QCOMPARE(e.snapshot().positionMs, qint64(2000)); // at once, before the worker got there
        QTRY_COMPARE(log->seeks.load(), 1);              // the worker asks the source (held there)
        QCOMPARE(log->lastSeek.load(), qint64(96000));
        QCOMPARE(e.snapshot().positionMs, qint64(2000)); // still pending
        gate->release();
        QTRY_COMPARE(e.snapshot().positionMs, before); // refused: it plays on where it was
        gate->hold();
        e.seek(std::numeric_limits<qint64>::max()); // the end the file claims, without overflowing either
        QCOMPARE(e.snapshot().positionMs, o.claimedMs);
        QTRY_COMPARE(log->seeks.load(), 2);
        QCOMPARE(log->lastSeek.load(), o.claimedMs * 48);
        QCOMPARE(e.snapshot().positionMs, o.claimedMs);
        gate->release();
        QTRY_COMPARE(e.snapshot().positionMs, before);
        QVERIFY(e.decodedFrames() >= 0);
    }

    void endsAtTheEnd()
    {
        Clock              clock;
        GenSource::Options o;
        o.frames = 48000 / 2; // half a second
        Engine             e(1, gen(o), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        int total = 960;
        for (int i = 0; i < 200 && e.snapshot().state != Engine::State::Ended; ++i) {
            const int n = e.take(Engine::Send, buf.data(), 960);
            total += n;
            if (n == 0)
                QTest::qWait(2);
        }
        const Engine::Snapshot s = e.snapshot();
        QCOMPARE(s.state, Engine::State::Ended);
        QCOMPARE(total, 24000);
        QCOMPARE(s.positionMs, qint64(500));
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 0);
    }

    void failuresSayWhy()
    {
        Clock              clock;
        GenSource::Options bad;
        bad.failOpen = true;
        Engine opened(1, gen(bad), clock.fn());
        opened.start();
        QTRY_COMPARE(opened.snapshot().state, Engine::State::Failed);
        QCOMPARE(opened.snapshot().error, QStringLiteral("This file has no sound to play."));
        QCOMPARE(opened.snapshot().code, 0x0123L);

        GenSource::Options broken;
        broken.failAt = 48000;
        Engine             mid(1, gen(broken), clock.fn());
        std::vector<float> buf(2 * 960);
        mid.start();
        // Damaged after a second: that second plays, then it fails and says why.
        int played = firstTake(mid, Engine::Send, buf.data(), 960);
        QVERIFY(played > 0);
        for (int i = 0; i < 200 && mid.snapshot().state == Engine::State::Playing; ++i) {
            const int n = mid.take(Engine::Send, buf.data(), 960);
            played += n;
            if (n == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QCOMPARE(mid.snapshot().state, Engine::State::Failed);
        QCOMPARE(played, 42 * 1152); // every piece the source gave before the damage
        QCOMPARE(mid.snapshot().error, QStringLiteral("The file is damaged."));
        QCOMPARE(mid.take(Engine::Send, buf.data(), 960), 0);
    }

    void capturedVoiceGetsTheMusic()
    {
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        e.setGain(1.0f);
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0); // decoded (the send side snaps to it)
        // Voice activation hears speech: voice and music, changed and sent.
        std::vector<qint16> block(960, 4096);
        int                 edited = music::kEditedSend;
        e.captured(block.data(), 960, 1, &edited);
        QCOMPARE(edited, 3);
        const qint64 at = 960; // where the send side started (the monitor's place)
        QCOMPARE(int(block[300]), int(music::toPcm16(music::softClip(0.125f + 0.0f * GenSource::left(at + 300)))));
        // (left and right cancel in mono: the voice is what is left; check stereo for the music itself)
        std::vector<qint16> stereo(2 * 960, 0);
        edited = 0; // silence: the room is left out, the music goes anyway
        e.captured(stereo.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        QCOMPARE(int(stereo[2 * 300]), int(music::toPcm16(GenSource::left(at + 960 + 300))));
        QCOMPARE(int(stereo[2 * 300 + 1]), int(music::toPcm16(-GenSource::left(at + 960 + 300))));
        QVERIFY(e.snapshot().mixing);
        // Push-to-talk kept open by the plugin: the voice never goes along.
        e.setVoiceAllowed(false);
        std::vector<qint16> voice(2 * 960, 9000);
        edited = music::kEditedSend;
        e.captured(voice.data(), 960, 2, &edited); // the ramp from the last block's 0 stays at 0
        QCOMPARE(int(voice[2 * 900]), int(music::toPcm16(GenSource::left(at + 1920 + 900))));
        e.setVoiceAllowed(true);
        // Paused: after the fade out TeamSpeak's samples and decision are its own again.
        e.pause();
        std::vector<qint16> fadeBlock(960, 100);
        edited = 0;
        e.captured(fadeBlock.data(), 960, 1, &edited);
        QCOMPARE(edited, 3);
        std::vector<qint16> quiet(960, 100);
        edited = 0;
        e.captured(quiet.data(), 960, 1, &edited);
        QCOMPARE(edited, 0);
        QCOMPARE(int(quiet[0]), 100);
        QCOMPARE(int(quiet[959]), 100);
        // Bad blocks are ignored.
        e.captured(nullptr, 960, 1, &edited);
        e.captured(quiet.data(), 960, 1, nullptr);
        e.captured(quiet.data(), -1, 1, &edited);
        e.captured(quiet.data(), 960, 99, &edited);
        // A huge block is mixed in parts.
        e.resume();
        std::vector<qint16> huge(20000, 0);
        edited = 0;
        e.captured(huge.data(), 20000, 1, &edited);
        QCOMPARE(edited, 3);
    }

    void noMicrophoneWhileTheInputIsKeptOpen()
    {
        // The plugin keeps a push-to-talk input open (the voice not allowed): TeamSpeak would send whatever
        // the microphone has, so every block without music (nothing decoded yet, a seek, a pause, the end)
        // is silence and not sent, and the voice never fades out over the music's first block.
        Clock              clock;
        GenSource::Options o;
        o.frames = 48000; // one second
        Engine e(1, gen(o), clock.fn());
        e.setGain(1.0f);
        e.setVoiceAllowed(false);
        const auto silentAndNotSent = [](const std::vector<qint16>& block, int edited) {
            return edited == music::kEditedChanged && std::all_of(block.begin(), block.end(), [](qint16 s) { return s == 0; });
        };
        std::vector<qint16> stereo(2 * 960);
        int                 edited = music::kEditedSend;
        std::fill(stereo.begin(), stereo.end(), qint16(12000));
        e.captured(stereo.data(), 960, 2, &edited); // not even started
        QVERIFY(silentAndNotSent(stereo, edited));
        e.start();
        // Opening and the prebuffer: silence, not sent; then the music, never the voice.
        bool music = false;
        for (int i = 0; i < 5000 && !music; ++i) {
            std::fill(stereo.begin(), stereo.end(), qint16(12000));
            edited = music::kEditedSend;
            e.captured(stereo.data(), 960, 2, &edited);
            music = (edited & music::kEditedSend) != 0;
            if (!music) {
                QVERIFY(silentAndNotSent(stereo, edited));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        QVERIFY(music);
        QCOMPARE(edited, 3);
        for (int f = 0; f < 960; ++f) { // the track's first block, faded in: the music alone
            const float fadeIn = f < Engine::kFadeFrames ? static_cast<float>(f) / (Engine::kFadeFrames - 1) : 1.0f;
            QVERIFY2(qAbs(int(stereo[2 * f]) - int(music::toPcm16(GenSource::left(f) * fadeIn))) <= 1, qPrintable(QString::number(f)));
        }
        // A seek: until the worker is there, silence, not sent.
        e.seek(500);
        std::fill(stereo.begin(), stereo.end(), qint16(12000));
        edited = music::kEditedSend;
        e.captured(stereo.data(), 960, 2, &edited);
        if (!(edited & music::kEditedSend))
            QVERIFY(silentAndNotSent(stereo, edited));
        // Paused: the fade out, then silence, not sent (until the controller gives the input back).
        for (int i = 0; i < 5000 && e.snapshot().positionMs < 500; ++i) {
            e.captured(stereo.data(), 960, 2, &edited);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        e.pause();
        e.captured(stereo.data(), 960, 2, &edited); // the fade out
        std::fill(stereo.begin(), stereo.end(), qint16(12000));
        edited = music::kEditedSend;
        e.captured(stereo.data(), 960, 2, &edited);
        QVERIFY(silentAndNotSent(stereo, edited));
        // The end: once everything is played, silence, not sent.
        e.resume();
        for (int i = 0; i < 5000 && e.snapshot().state != Engine::State::Ended; ++i) {
            e.captured(stereo.data(), 960, 2, &edited);
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // the worker decodes meanwhile
        }
        QCOMPARE(e.snapshot().state, Engine::State::Ended);
        std::fill(stereo.begin(), stereo.end(), qint16(12000));
        edited = music::kEditedSend;
        e.captured(stereo.data(), 960, 2, &edited);
        QVERIFY(silentAndNotSent(stereo, edited));
        // The voice allowed again (the input given back): TeamSpeak's own block and decision.
        e.setVoiceAllowed(true);
        std::fill(stereo.begin(), stereo.end(), qint16(12000));
        edited = music::kEditedSend;
        e.captured(stereo.data(), 960, 2, &edited);
        QCOMPARE(edited, int(music::kEditedSend));
        QCOMPARE(int(stereo[100]), 12000);
    }

    void aMutedClientIsNeverMadeToSend()
    {
        // TeamSpeak's microphone or sound muted (setSendAllowed(false)): the voice gets no music and keeps
        // TeamSpeak's decision; the monitor plays on (it leads meanwhile).
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        e.setGain(1.0f);
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0);
        e.setSendAllowed(false);
        std::vector<qint16> block(2 * 960, 700);
        int                 edited = 0;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 0);
        QCOMPARE(int(block[2 * 300]), 700);
        clock.advance(Engine::kAliveMs + 10);
        QVERIFY(!e.snapshot().sending);
        QVERIFY(!e.snapshot().mixing);
        std::vector<qint16> out(2 * 960, 0);
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        unsigned            mask       = 0;
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 3u);
        QVERIFY(e.snapshot().monitoring);
        // Unmuted: the music goes into the voice again, from where the monitor is.
        e.setSendAllowed(true);
        edited = 0;
        std::fill(block.begin(), block.end(), qint16(0));
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        QVERIFY(e.snapshot().mixing);
    }

    void mixingIsSeenWhenTheFlagsAlreadySaySo()
    {
        // Another plugin changed the samples and voice activation sends them (3 on entry): *edited needs no
        // change, but the music went in, and the bar's badge must say the channel hears it.
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0);
        std::vector<qint16> block(2 * 960, 0);
        int                 edited = music::kEditedChanged | music::kEditedSend;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        QVERIFY(e.snapshot().mixing);
    }

    void playbackGetsTheMusic()
    {
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        e.setGain(0.5f);
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, scratch.data(), 960) > 0);
        std::vector<qint16> out(2 * 960, 31111); // undefined: not filled
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        unsigned            mask       = 0;
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 3u);
        QCOMPARE(int(out[2 * 500]), int(music::toPcm16(0.5f * GenSource::left(960 + 500))));
        QVERIFY(e.snapshot().monitoring);
    }

    void callbacksReachOnlyTheActiveEngine()
    {
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0);
        std::vector<qint16> block(960, 0);
        int                 edited = 0;
        music::onCaptured(1, block.data(), 960, 1, &edited); // not active yet
        QCOMPARE(edited, 0);
        music::setActive(&e);
        QCOMPARE(music::active(), &e);
        music::onCaptured(2, block.data(), 960, 1, &edited); // another server tab's voice
        QCOMPARE(edited, 0);
        music::onCaptured(1, block.data(), 960, 1, &edited);
        QCOMPARE(edited, 3);
        unsigned mask = 0;
        music::onPlayback(2, block.data(), 960, 1, nullptr, &mask);
        QCOMPARE(mask, 0u);
        music::onPlayback(1, block.data(), 960, 1, nullptr, &mask);
        QCOMPARE(mask, 1u);
        music::setActive(nullptr);
        edited = 0;
        music::onCaptured(1, block.data(), 960, 1, &edited);
        QCOMPARE(edited, 0);
        // An engine deleted while active detaches itself.
        {
            Engine other(1, gen(), clock.fn());
            music::setActive(&other);
        }
        QVERIFY(!music::active());
    }

    void theSourceLivesOnTheWorker()
    {
        auto log = std::make_shared<musicfakes::SourceLog>();
        {
            GenSource::Options o;
            o.log = log;
            Engine e(1, gen(o));
            e.start();
            QTRY_VERIFY(log->reads.load() > 0);
        }
        QVERIFY(log->destroyed.load());
        std::lock_guard<std::mutex> lock(log->lock);
        QVERIFY(log->openThread != std::this_thread::get_id());
        QVERIFY(log->destroyThread == log->openThread);
        // Never started: destroyed with the engine, nothing opened.
        auto idle = std::make_shared<musicfakes::SourceLog>();
        {
            GenSource::Options o;
            o.log = idle;
            Engine e(1, gen(o));
        }
        QVERIFY(idle->destroyed.load());
        QVERIFY(idle->openThread == std::thread::id());
    }

    void callbacksRacingTheTeardown()
    {
        // TeamSpeak's audio threads keep calling while the GUI seeks, pauses and finally deletes the engine.
        std::atomic<bool> run{true};
        std::atomic<int>  mixed{0};
        auto worker = [&](bool capture) {
            std::vector<qint16> block(2 * 480, 0);
            unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
            while (run.load()) {
                int      edited = capture ? 2 : 0;
                unsigned mask   = 0;
                if (capture)
                    music::onCaptured(7, block.data(), 480, 2, &edited);
                else
                    music::onPlayback(7, block.data(), 480, 2, speakers, &mask);
                if (edited == 3 || mask)
                    ++mixed;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        };
        std::thread capture(worker, true), playback(worker, false);
        for (int round = 0; round < 3; ++round) {
            auto e = std::make_unique<Engine>(7, gen());
            e->start();
            music::setActive(e.get());
            for (int i = 0; i < 20; ++i) {
                if (i % 5 == 0)
                    e->seek(i * 100);
                if (i % 7 == 0)
                    e->pause();
                if (i % 7 == 3)
                    e->resume();
                QTest::qWait(5);
            }
            music::setActive(nullptr);
            e.reset();
        }
        run = false;
        capture.join();
        playback.join();
        QVERIFY(mixed.load() > 0);
    }

    // ---- decoding (Media Foundation) -------------------------------------------------------------------------

    void decodesWavTo48kStereo()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(writeFile(path, wav(sine(1000.0, 44100, 1.5, 0.5, 1), 44100, 1)));
        mf::MusicInfo            info;
        QString                  error;
        const std::vector<float> all = decodeAll(path, &info, &error);
        QVERIFY2(!all.empty(), qPrintable(error));
        QCOMPARE(info.sourceRate, 44100);
        QCOMPARE(info.sourceChannels, 1);
        QVERIFY2(qAbs(info.durationMs - 1500) <= 20, qPrintable(QString::number(info.durationMs)));
        const qint64 frames = static_cast<qint64>(all.size() / 2);
        QVERIFY2(qAbs(frames - 72000) <= 1500, qPrintable(QString::number(frames))); // 1.5 s at 48 kHz
        // 1 kHz stays 1 kHz (2000 zero crossings a second), both channels carry it.
        const int crossL = zeroCrossings(all, 0, 12000, 48000);
        const int crossR = zeroCrossings(all, 1, 12000, 48000);
        QVERIFY2(qAbs(crossL - 2000) <= 20, qPrintable(QString::number(crossL)));
        QVERIFY2(qAbs(crossR - 2000) <= 20, qPrintable(QString::number(crossR)));
        float peak = 0.0f;
        for (size_t i = 24000; i < 48000; ++i)
            peak = qMax(peak, std::fabs(all[i]));
        QVERIFY2(peak > 0.3f && peak < 0.6f, qPrintable(QString::number(peak)));
    }

    void keepsChannelOrder()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("left.wav"));
        QVERIFY(writeFile(path, wav(sine(440.0, 48000, 1.0, 0.4, 2, 1), 48000, 2))); // right silent
        const std::vector<float> all = decodeAll(path);
        QVERIFY(all.size() >= 2 * 40000);
        double left = 0, right = 0;
        for (size_t f = 4800; f < 40000; ++f) {
            left += std::fabs(all[2 * f]);
            right += std::fabs(all[2 * f + 1]);
        }
        QVERIFY2(left > 1000.0 && right < 1.0, qPrintable(QStringLiteral("%1 %2").arg(left).arg(right)));
    }

    void seeksInAFile()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("seek.wav"));
        QVERIFY(writeFile(path, wav(sine(500.0, 48000, 3.0, 0.3, 2), 48000, 2)));
        // Through the engine with the real file source: seek to 2 s, then play to the end.
        Clock              clock;
        Engine             e(1, music::fileSource(path), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        QCOMPARE(e.snapshot().durationMs, qint64(3000));
        e.seek(2000);
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0);
        int total = 960;
        for (int i = 0; i < 2000 && e.snapshot().state != Engine::State::Ended; ++i) {
            const int n = e.take(Engine::Send, buf.data(), 960);
            total += n;
            if (n == 0)
                QTest::qWait(2);
        }
        QCOMPARE(e.snapshot().state, Engine::State::Ended);
        QVERIFY2(qAbs(total - (960 + 48000)) <= 1500, qPrintable(QString::number(total))); // a block, then the last second
    }

    void decodesAac()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QTemporaryDir             dir;
        const QString             path  = dir.filePath(QStringLiteral("tone.m4a"));
        const std::vector<qint16> tone  = sine(880.0, 48000, 2.0, 0.4, 1);
        const voice::AacResult    wrote = voice::writeAac(path, tone.data(), static_cast<qint64>(tone.size()), 48000, 1.0);
        QVERIFY2(wrote.ok, qPrintable(wrote.error));
        mf::MusicInfo            info;
        QString                  error;
        const std::vector<float> all = decodeAll(path, &info, &error);
        QVERIFY2(!all.empty(), qPrintable(error));
        QVERIFY2(qAbs(info.durationMs - 2000) <= 100, qPrintable(QString::number(info.durationMs)));
        const int cross = zeroCrossings(all, 0, 24000, 48000);
        QVERIFY2(qAbs(cross - 1760) <= 30, qPrintable(QString::number(cross)));
    }

    void refusesWhatIsNoSound()
    {
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("notes.mp3")); // a text file with a music name
        QVERIFY(writeFile(path, QByteArray("Alice's shopping list: milk, eggs, bread.\n").repeated(20)));
        QString error;
        QVERIFY(decodeAll(path, nullptr, &error).empty());
        QVERIFY(!error.isEmpty());
        QString missing;
        QVERIFY(decodeAll(dir.filePath(QStringLiteral("gone.mp3")), nullptr, &missing).empty());
        QVERIFY(!missing.isEmpty());
        // Through the engine: Failed, with the reason.
        Engine e(1, music::fileSource(path));
        e.start();
        QTRY_COMPARE_WITH_TIMEOUT(e.snapshot().state, Engine::State::Failed, 10000);
        QVERIFY(!e.snapshot().error.isEmpty());
    }

    // ---- 2.3 cross review (audio) ---------------------------------------------------------------------------

    void aShortGapDoesntPullTheMonitorBack()
    {
        // Push-to-talk let go: TeamSpeak captures nothing until the plugin opens the input again on its next
        // tick (up to 200 ms). The send side then rejoins where the monitor is; before, it went on from where
        // it stopped, and the monitor snapped back to it and played those 200 ms here a second time.
        Clock              clock;
        Engine             e(1, gen(), clock.fn());
        std::vector<float> buf(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Send, buf.data(), 960) > 0); // send at 960
        for (int i = 0; i < 5; ++i) {                             // both every 20 ms: send at 5760, monitor at 6720
            clock.advance(20);
            QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
            QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        }
        for (int i = 0; i < 10; ++i) { // 200 ms without capture: the monitor plays on to 16320
            clock.advance(20);
            QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        }
        QCOMPARE(buf[0], GenSource::left(15360));
        clock.advance(20);
        QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
        QCOMPARE(buf[2 * 300], GenSource::left(16320 + 300)); // the channel hears what is played here
        QCOMPARE(buf[0], 0.0f);                               // (faded in at the new place)
        QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
        QCOMPARE(buf[0], GenSource::left(16320)); // the monitor simply goes on
        QCOMPARE(buf[2 * 300], GenSource::left(16320 + 300));
        // Regular large blocks (100 ms each, both sides) never count as a gap: no jumps, no fades.
        for (int i = 0; i < 4; ++i) {
            clock.advance(100);
            QCOMPARE(e.take(Engine::Send, buf.data(), 960), 960);
            QCOMPARE(buf[0], GenSource::left(17280 + i * 960));
            QCOMPARE(e.take(Engine::Monitor, buf.data(), 960), 960);
            QCOMPARE(buf[0], GenSource::left(17280 + i * 960));
        }
    }

    void volumeChangesAreRamped()
    {
        // The volume slider, the bar's mute and unmute: the gain moves across one block instead of stepping
        // (a click in everybody's ears). Muting sends that one fading block, then nothing.
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        e.setGain(1.0f);
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0); // the send side starts at 960
        std::vector<qint16> block(2 * 960, 0);
        int                 edited = 0;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        qint64 at = 960 + 960;
        e.setGain(0.0f);
        std::fill(block.begin(), block.end(), qint16(0));
        edited = 0;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3); // the fading block
        for (int f = 0; f < 960; f += 97) {
            const float g = 1.0f - static_cast<float>(f) / 959.0f;
            QVERIFY2(qAbs(int(block[2 * f]) - int(music::toPcm16(GenSource::left(at + f) * g))) <= 1, qPrintable(QString::number(f)));
        }
        QVERIFY(qAbs(int(block[2 * 959])) <= 1); // down to nothing at its end
        at += 960;
        std::fill(block.begin(), block.end(), qint16(77));
        edited = 0;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 0); // then muted music sends nothing and leaves TeamSpeak's block alone
        QCOMPARE(int(block[700]), 77);
        at += 960;
        e.setGain(0.25f); // unmuted: up from nothing
        std::fill(block.begin(), block.end(), qint16(0));
        edited = 0;
        e.captured(block.data(), 960, 2, &edited);
        QCOMPARE(edited, 3);
        QCOMPARE(int(block[0]), 0);
        QVERIFY2(qAbs(int(block[2 * 959]) - int(music::toPcm16(GenSource::left(at + 959) * 0.25f))) <= 1, qPrintable(QString::number(block[2 * 959])));
        // The monitor side the same way.
        std::vector<qint16> out(2 * 960, 0);
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        unsigned            mask       = 0;
        e.playback(out.data(), 960, 2, speakers, &mask); // at 0.25 (frames 960 to 1920)
        QCOMPARE(mask, 3u);
        e.setGain(0.0f);
        mask = 0;
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 3u); // the fading block
        QVERIFY(qAbs(int(out[2 * 959])) <= 1);
        QVERIFY(qAbs(int(out[2 * 10])) > 1000); // (0.465 * 0.25 near its start)
        mask = 0;
        std::fill(out.begin(), out.end(), qint16(55));
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 0u); // nothing added
        QCOMPARE(int(out[100]), 55);
    }

    void soundMutedNothingPlaysHere()
    {
        // TeamSpeak's sound muted (setMonitorAllowed(false)): nothing is added to this computer's playback
        // (one fading block), but the place moves on; unmuted, the music comes back up from nothing.
        Clock  clock;
        Engine e(1, gen(), clock.fn());
        e.setGain(1.0f);
        std::vector<float> scratch(2 * 960);
        e.start();
        QVERIFY(firstTake(e, Engine::Monitor, scratch.data(), 960) > 0);
        std::vector<qint16> out(2 * 960, 0);
        unsigned            speakers[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
        unsigned            mask       = 0;
        e.playback(out.data(), 960, 2, speakers, &mask); // heard here
        QCOMPARE(mask, 3u);
        e.setMonitorAllowed(false);
        mask = 0;
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 3u);
        QVERIFY(qAbs(int(out[2 * 959])) <= 1); // faded out
        const qint64 before = e.snapshot().positionMs;
        for (int i = 0; i < 5; ++i) {
            mask = 0;
            std::fill(out.begin(), out.end(), qint16(-9));
            e.playback(out.data(), 960, 2, speakers, &mask);
            QCOMPARE(mask, 0u);
            QCOMPARE(int(out[2 * 480]), -9);
        }
        QCOMPARE(e.snapshot().positionMs, before + 5 * 20); // the music went on meanwhile
        QVERIFY(e.snapshot().monitoring);
        e.setMonitorAllowed(true);
        mask = 0;
        e.playback(out.data(), 960, 2, speakers, &mask);
        QCOMPARE(mask, 3u);
        QVERIFY(qAbs(int(out[0])) <= 1); // back up from nothing
    }

    void stopFadesOutFirst()
    {
        // TeamSpeak's audio thread keeps calling while the GUI stops: fadeOut() returns once that side has
        // played the 5 ms fade out (well before its time limit), and nothing plays after it.
        Engine e(1, gen());
        e.setGain(1.0f);
        e.start();
        std::atomic<bool> run{true};
        std::atomic<int>  calls{0};
        std::vector<float> last;   // the side's last block with music in it
        std::mutex         lastLock;
        std::atomic<int>   afterFade{0};
        std::atomic<bool>  faded{false};
        std::thread        audio([&] {
            std::vector<float> buf(2 * 960);
            while (run.load()) {
                const int n = e.take(Engine::Send, buf.data(), 960); // 20 ms blocks, about in real time
                ++calls;
                if (n > 0) {
                    if (faded.load())
                        ++afterFade;
                    std::lock_guard<std::mutex> lock(lastLock);
                    last.assign(buf.begin(), buf.begin() + 2 * n);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        QTRY_VERIFY(e.snapshot().state == Engine::State::Playing && e.snapshot().positionMs > 200);
        QElapsedTimer t;
        t.start();
        e.fadeOut(1000);
        faded = true;
        const qint64 took = t.elapsed();
        const int    at   = calls.load();
        QTRY_VERIFY(calls.load() > at + 5);
        run = false;
        audio.join();
        QVERIFY2(took < 500, qPrintable(QString::number(took)));
        QCOMPARE(e.snapshot().state, Engine::State::Paused);
        QCOMPARE(afterFade.load(), 0);
        std::lock_guard<std::mutex> lock(lastLock);
        QCOMPARE(int(last.size()), 2 * Engine::kFadeFrames);
        QCOMPARE(last[last.size() - 2], 0.0f); // down to nothing
        // Nothing called (no audio thread), or nothing playing: at once.
        Engine idle(1, gen());
        t.restart();
        idle.fadeOut(1000);
        QVERIFY(t.elapsed() < 100);
    }

    void decodesOddRatesAndSurround()
    {
        // 8 / 22.05 / 96 kHz, mono, 24-bit and 5.1 (WAVE_FORMAT_EXTENSIBLE) files all come out as 48 kHz
        // stereo at the right length, pitch and level; a 5.1 file's front left stays left, its centre goes to
        // both sides (Media Foundation's mix down).
        if (!mf::available())
            QSKIP("Media Foundation is not available on this computer");
        struct Case {
            int     rate;
            int     channels;
            int     bits;
            quint32 mask; // 0: a plain PCM header
            int     toneOn; // -1: every channel
        };
        const Case cases[] = {{8000, 1, 16, 0, -1}, {22050, 1, 16, 0, -1}, {96000, 2, 16, 0, -1}, {44100, 2, 24, 0x3, -1},
                              {48000, 6, 16, 0x3F, 0}, {48000, 6, 16, 0x3F, 2}, {44100, 6, 24, 0x3F, -1}};
        QTemporaryDir dir;
        for (const Case& k : cases) {
            const int  frames = k.rate; // one second
            const int  bytes  = k.bits / 8;
            QByteArray data;
            data.reserve(frames * k.channels * bytes);
            for (int f = 0; f < frames; ++f) {
                const double v = 0.5 * std::sin(2.0 * M_PI * 1000.0 * f / k.rate);
                for (int c = 0; c < k.channels; ++c) {
                    const qint32 s = static_cast<qint32>(std::lround((k.toneOn < 0 || k.toneOn == c ? v : 0.0) * ((1 << (k.bits - 1)) - 1)));
                    char         le[4];
                    qToLittleEndian(s, le);
                    data.append(le, bytes);
                }
            }
            QByteArray file;
            const auto u32 = [&file](quint32 v) {
                char b[4];
                qToLittleEndian(v, b);
                file.append(b, 4);
            };
            const auto u16 = [&file](quint16 v) {
                char b[2];
                qToLittleEndian(v, b);
                file.append(b, 2);
            };
            const bool ext = k.mask != 0;
            file.append("RIFF", 4);
            u32(static_cast<quint32>((ext ? 60 : 36) + data.size()));
            file.append("WAVEfmt ", 8);
            u32(ext ? 40 : 16);
            u16(ext ? 0xFFFE : 1);
            u16(static_cast<quint16>(k.channels));
            u32(static_cast<quint32>(k.rate));
            u32(static_cast<quint32>(k.rate * k.channels * bytes));
            u16(static_cast<quint16>(k.channels * bytes));
            u16(static_cast<quint16>(k.bits));
            if (ext) {
                u16(22);
                u16(static_cast<quint16>(k.bits));
                u32(k.mask);
                const unsigned char pcm[16] = {0x01, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}; // KSDATAFORMAT_SUBTYPE_PCM
                file.append(reinterpret_cast<const char*>(pcm), 16);
            }
            file.append("data", 4);
            u32(static_cast<quint32>(data.size()));
            file.append(data);
            const QString name = QStringLiteral("%1-%2ch-%3bit-%4.wav").arg(k.rate).arg(k.channels).arg(k.bits).arg(k.toneOn);
            const QString path = dir.filePath(name);
            QVERIFY(writeFile(path, file));
            QString                  error;
            const std::vector<float> all = decodeAll(path, nullptr, &error);
            QVERIFY2(!all.empty(), qPrintable(name + QLatin1Char(' ') + error));
            const qint64 got = static_cast<qint64>(all.size() / 2);
            QVERIFY2(qAbs(got - 48000) <= 50, qPrintable(name + QLatin1Char(' ') + QString::number(got)));
            double left = 0, right = 0;
            for (qint64 f = 12000; f < 36000; ++f) {
                left += double(all[2 * f]) * all[2 * f];
                right += double(all[2 * f + 1]) * all[2 * f + 1];
            }
            left  = std::sqrt(left / 24000);
            right = std::sqrt(right / 24000);
            if (k.toneOn == 0) { // front left only
                QVERIFY2(left > 0.05 && right < 0.001, qPrintable(QStringLiteral("%1 %2 %3").arg(name).arg(left).arg(right)));
            } else if (k.toneOn == 2) { // centre: both sides alike
                QVERIFY2(left > 0.03 && qAbs(left - right) < 0.001, qPrintable(QStringLiteral("%1 %2 %3").arg(name).arg(left).arg(right)));
            } else { // every channel: the level of the source (0.5 peak, 0.354 RMS)
                QVERIFY2(qAbs(left - 0.3536) < 0.01 && qAbs(right - 0.3536) < 0.01, qPrintable(QStringLiteral("%1 %2 %3").arg(name).arg(left).arg(right)));
            }
            const int cross = zeroCrossings(all, k.toneOn == 0 ? 0 : 1, 12000, 24000); // half a second of 1 kHz
            QVERIFY2(qAbs(cross - 1000) <= 4, qPrintable(name + QLatin1Char(' ') + QString::number(cross)));
        }
    }

  private:
    TS3Functions m_saved;
};

TSMEDIA_REGISTER_TEST(TestMusic)
#include "tst_music.moc"
