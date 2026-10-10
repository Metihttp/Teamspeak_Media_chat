// Unit tests for the 2.2 voice-message logic that needs no device and no Media Foundation: the
// waveform (peak bins -> link levels -> bars), MicGuard against a fake TeamSpeak, the choice of
// microphone, sample conversion, the start / stop sounds, names and the microphone setting.
// Part of tsmedia_tests (registered with tests/testmain.h).

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QtEndian>
#include <QtTest>

#include <cmath>

#include "audio/capturedevice.h"
#include "audio/cues.h"
#include "audio/micguard.h"
#include "audio/waveform.h"
#include "medialink.h"
#include "settings.h"
#include "testmain.h"

namespace {

// TeamSpeak as MicGuard sees it: per connection whether a capture device is open, the mute state, and
// failures to inject.
class FakeMic : public voice::MicEnvironment
{
  public:
    struct Conn {
        int  hardware   = 1;
        int  muted      = 0;
        bool readable   = true; // false: disconnected (reads fail)
        int  failSets   = 0;    // the next n sets fail
        int  failFlushes = 0;   // the next n sets change the flag but report failure (the flush to the server failed)
        int  sets       = 0;
    };
    QHash<quint64, Conn> conns;
    QList<quint64>       order;
    QList<QString>       calls; // "set <sch> <0|1>"

    void add(quint64 sch, int hardware, int muted)
    {
        Conn c;
        c.hardware = hardware;
        c.muted    = muted;
        conns.insert(sch, c);
        order.append(sch);
    }

    QList<quint64> connections() const override { return order; }
    bool readVariable(quint64 sch, Variable variable, int* value) const override
    {
        const auto it = conns.constFind(sch);
        if (it == conns.constEnd() || !it->readable)
            return false;
        *value = variable == Variable::InputHardware ? it->hardware : it->muted;
        return true;
    }
    bool setInputMuted(quint64 sch, bool muted) override
    {
        auto it = conns.find(sch);
        if (it == conns.end())
            return false;
        ++it->sets;
        calls << QStringLiteral("set %1 %2").arg(sch).arg(muted ? 1 : 0);
        if (it->failSets > 0) {
            --it->failSets;
            return false;
        }
        it->muted = muted ? 1 : 0;
        if (it->failFlushes > 0) {
            --it->failFlushes;
            return false;
        }
        return true;
    }
};

QByteArray levels(std::initializer_list<int> values)
{
    QByteArray out;
    for (int v : values)
        out.append(static_cast<char>(v));
    return out;
}

voice::EndpointList endpoints()
{
    voice::EndpointList list;
    list.endpoints = {
        {QStringLiteral("{0.0.1.00000000}.{11111111-2222-3333-4444-555555555555}"), QStringLiteral("Headset Microphone (USB Audio Device)")},
        {QStringLiteral("{0.0.1.00000000}.{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}"), QStringLiteral("Microphone (Realtek High Definition Audio)")},
        {QStringLiteral("{0.0.1.00000000}.{99999999-8888-7777-6666-555555555555}"), QStringLiteral("Microphone (Webcam C920)")},
    };
    list.defaultCommunications = list.endpoints.at(1).id;
    return list;
}

} // namespace

class TestVoice : public QObject
{
    Q_OBJECT

  private slots:
    // ---- waveform -------------------------------------------------------------------------------
    void peakDb()
    {
        QCOMPARE(waveform::peakDb(nullptr, 0), waveform::kSilenceDb);
        const int16_t silence[4] = {0, 0, 0, 0};
        QCOMPARE(waveform::peakDb(silence, 4), waveform::kSilenceDb);
        const int16_t full[3] = {0, -32768, 100};
        QVERIFY(qAbs(waveform::peakDb(full, 3)) < 0.001f);
        const int16_t half[2] = {16384, -100};
        QVERIFY(qAbs(waveform::peakDb(half, 2) + 6.0206f) < 0.01f);
    }

    void levelsMapping()
    {
        // 64+ bins: equal buckets, the loudest bin of each; -45 dBFS and below -> 0, 0 dBFS -> 15.
        QVector<float> bins(64, -45.0f);
        bins[0] = 0.0f;
        bins[1] = -22.5f; // halfway: 7.5 rounds to 8
        bins[2] = -60.0f;
        const QByteArray l = waveform::levelsFromBins(bins);
        QCOMPARE(l.size(), MediaLink::kWaveformLevels);
        QCOMPARE(int(l.at(0)), 15);
        QCOMPARE(int(l.at(1)), 8);
        QCOMPARE(int(l.at(2)), 0);
        QCOMPARE(int(l.at(63)), 0);

        // 128 bins: each level is the max of two bins.
        QVector<float> many(128, -45.0f);
        many[5]   = 0.0f; // bucket 2
        many[127] = 0.0f; // bucket 63
        const QByteArray m = waveform::levelsFromBins(many);
        QCOMPARE(int(m.at(2)), 15);
        QCOMPARE(int(m.at(63)), 15);
        QCOMPARE(int(m.at(3)), 0);

        // Fewer bins than levels: repeated (nearest), so a 1 s message still has 64 levels.
        const QByteArray few = waveform::levelsFromBins({0.0f, -45.0f});
        QCOMPARE(few.size(), 64);
        QCOMPARE(int(few.at(0)), 15);
        QCOMPARE(int(few.at(31)), 15);
        QCOMPARE(int(few.at(32)), 0);

        // No bins: a flat line.
        QCOMPARE(waveform::levelsFromBins({}), QByteArray(64, '\0'));
    }

    void levelsNormalize()
    {
        // Loudest level 4..14: scaled so the loudest is 15.
        QVector<float> quiet(64, -45.0f);
        quiet[10] = -45.0f + 45.0f * 6.0f / 15.0f; // level 6
        quiet[11] = -45.0f + 45.0f * 3.0f / 15.0f; // level 3
        const QByteArray q = waveform::levelsFromBins(quiet);
        QCOMPARE(int(q.at(10)), 15);
        QCOMPARE(int(q.at(11)), 8); // 3 * 15 / 6 = 7.5 -> 8
        // Loudest under 4: left alone (near silence stays near silence).
        QVector<float> faint(64, -45.0f);
        faint[0] = -45.0f + 45.0f * 3.0f / 15.0f;
        QCOMPARE(int(waveform::levelsFromBins(faint).at(0)), 3);
        // Already 15: unchanged.
        QVector<float> loud(64, -45.0f);
        loud[0] = 0.0f;
        loud[1] = -45.0f + 45.0f * 5.0f / 15.0f;
        QCOMPARE(int(waveform::levelsFromBins(loud).at(1)), 5);
        // Every level stays in 0..15.
        QVector<float> any(300);
        for (int i = 0; i < any.size(); ++i)
            any[i] = -96.0f + static_cast<float>(i % 97);
        for (const char c : waveform::levelsFromBins(any))
            QVERIFY(c >= 0 && c <= 15);
    }

    void resample()
    {
        QByteArray source(64, '\0');
        source[10] = 15;
        source[63] = 9;
        // Fewer bars: max pooling keeps every peak.
        const QVector<quint8> down = waveform::resample(source, 43);
        QCOMPARE(down.size(), 43);
        QCOMPARE(*std::max_element(down.begin(), down.end()), quint8(15));
        QCOMPARE(int(down.last()), 9);
        int peaks = 0;
        for (quint8 v : down)
            peaks += v == 15 ? 1 : 0;
        QCOMPARE(peaks, 1);
        // More bars: nearest level, peaks widen.
        const QVector<quint8> up = waveform::resample(source, 100);
        QCOMPARE(up.size(), 100);
        QCOMPARE(int(up.at(16)), 15); // bar 16 covers level 10
        QCOMPARE(int(up.last()), 9);
        // Same count: identity.
        const QVector<quint8> same = waveform::resample(source, 64);
        for (int i = 0; i < 64; ++i)
            QCOMPARE(int(same.at(i)), int(source.at(i)));
        // Empty input: a flat line; no bars: nothing.
        const QVector<quint8> flat = waveform::resample({}, 5);
        QCOMPARE(flat.size(), 5);
        for (const quint8 v : flat)
            QCOMPARE(int(v), 0);
        QVERIFY(waveform::resample(source, 0).isEmpty());
        // Out-of-range input is clamped.
        QCOMPARE(int(waveform::resample(levels({200, 3}), 2).at(0)), 15);
    }

    void normalizeGain()
    {
        QCOMPARE(waveform::normalizeGain(waveform::kSilenceDb), 1.0);
        QCOMPARE(waveform::normalizeGain(-3.0f), 1.0);  // loud enough
        QCOMPARE(waveform::normalizeGain(-6.0f), 1.0);  // at the threshold
        QVERIFY(qAbs(waveform::normalizeGain(-10.0f) - std::pow(10.0, 9.0 / 20.0)) < 1e-6); // to -1 dBFS
        QVERIFY(qAbs(waveform::normalizeGain(-30.0f) - std::pow(10.0, 12.0 / 20.0)) < 1e-6); // capped at +12 dB
        QCOMPARE(waveform::applyGain(30000, 2.0), int16_t(32767));
        QCOMPARE(waveform::applyGain(-30000, 2.0), int16_t(-32768));
        QCOMPARE(waveform::applyGain(1000, 1.5), int16_t(1500));
    }

    void liveHeight()
    {
        QCOMPARE(waveform::liveHeight(0.0f), 1.0);
        QCOMPARE(waveform::liveHeight(-45.0f), 0.0);
        QCOMPARE(waveform::liveHeight(-96.0f), 0.0);
        QVERIFY(qAbs(waveform::liveHeight(-22.5f) - 0.5) < 1e-9);
    }

    // The sender's levels survive the link (wf=, 43 characters) unchanged.
    void waveformInLink()
    {
        QVector<float> bins;
        for (int i = 0; i < 240; ++i) // 12 s
            bins.append(-40.0f + 38.0f * static_cast<float>(std::fabs(std::sin(i / 9.0))));
        MediaLink link;
        link.host       = QStringLiteral("127.0.0.1");
        link.port       = 9987;
        link.serverUid  = QStringLiteral("uid");
        link.channelId  = 1;
        link.path       = QStringLiteral("/tsmedia");
        link.fileName   = QStringLiteral("voice_message_3f9a1c2e.m4a");
        link.size       = 146320;
        link.protocol   = MediaLink::kProtocol;
        link.durationMs = 12040;
        link.voice      = true;
        link.waveform   = waveform::levelsFromBins(bins);
        const QString url = link.toUrl();
        QVERIFY(url.contains(QStringLiteral("&vm=1&wf=")));
        const QString wf = url.mid(url.indexOf(QStringLiteral("&wf=")) + 4);
        QCOMPARE(wf.size(), 43);
        const MediaLink back = MediaLink::parse(url);
        QVERIFY(back.voice);
        QCOMPARE(back.waveform, link.waveform);
        QCOMPARE(linkLabel(back), QStringLiteral("Voice message (0:12)"));
    }

    // ---- MicGuard ------------------------------------------------------------------------------
    void micGuardMutesAndRestores()
    {
        FakeMic env;
        env.add(1, 1, 0); // capturing, unmuted: muted while recording
        env.add(2, 0, 0); // no capture device open: left alone
        env.add(3, 1, 1); // muted by the user: theirs
        {
            voice::MicGuard guard(env);
            QCOMPARE(guard.engage(), 1);
            QVERIFY(guard.engaged());
            QCOMPARE(guard.mutedConnections(), QList<quint64>{1});
            QCOMPARE(env.conns[1].muted, 1);
            QCOMPARE(env.conns[2].muted, 0);
            QCOMPARE(env.conns[3].muted, 1);
            QCOMPARE(guard.engage(), 1); // twice: nothing new
            QCOMPARE(env.conns[1].sets, 1);
            const voice::MicGuard::Released r = guard.release();
            QCOMPARE(r.restored, 1);
            QCOMPARE(r.kept, 0);
            QVERIFY(!guard.engaged());
            QCOMPARE(guard.release().restored, 0); // idempotent
        }
        QCOMPARE(env.conns[1].muted, 0);
        QCOMPARE(env.conns[3].muted, 1); // the user's own mute stays
        QCOMPARE(env.calls, (QList<QString>{QStringLiteral("set 1 1"), QStringLiteral("set 1 0")}));
    }

    void micGuardUserUnmutedMeanwhile()
    {
        FakeMic env;
        env.add(7, 1, 0);
        voice::MicGuard guard(env);
        guard.engage();
        env.conns[7].muted = 0; // the user pressed unmute while recording
        const voice::MicGuard::Released r = guard.release();
        QCOMPARE(r.restored, 0);
        QCOMPARE(r.kept, 1);
        QCOMPARE(env.conns[7].sets, 1); // only the mute; nothing written back
    }

    void micGuardFailsClosed()
    {
        FakeMic env;
        env.add(1, 1, 0);
        env.add(2, 1, 0);
        voice::MicGuard guard(env);
        QCOMPARE(guard.engage(), 2);
        env.conns[2].readable = false; // disconnected while recording: its state is unknown
        const voice::MicGuard::Released r = guard.release();
        QCOMPARE(r.restored, 1);
        QCOMPARE(r.kept, 1);
        QCOMPARE(env.conns[2].muted, 1); // never unmuted blindly
        QCOMPARE(env.conns[2].sets, 1);
    }

    void micGuardRetriesAndSkipsFailedMute()
    {
        FakeMic env;
        env.add(1, 1, 0);
        env.add(2, 1, 0);
        env.conns[2].failSets = 1; // muting 2 fails: not ours to give back
        voice::MicGuard guard(env);
        QCOMPARE(guard.engage(), 1);
        QCOMPARE(guard.mutedConnections(), QList<quint64>{1});
        env.conns[1].failSets = 1; // the first unmute fails, the retry works
        const voice::MicGuard::Released r = guard.release();
        QCOMPARE(r.restored, 1);
        QCOMPARE(env.conns[1].muted, 0);
        QCOMPARE(env.conns[1].sets, 3);
        QCOMPARE(env.conns[2].muted, 0);
    }

    void micGuardKeepsMuteWhoseFlushFailed()
    {
        // The flag was set but sending it to the server failed: the microphone is muted all the same,
        // so the guard remembers it, says so, and gives it back.
        FakeMic env;
        env.add(1, 1, 0);
        env.conns[1].failFlushes = 1;
        voice::MicGuard guard(env);
        QCOMPARE(guard.engage(), 1);
        QCOMPARE(guard.mutedConnections(), QList<quint64>{1});
        QCOMPARE(env.conns[1].muted, 1);
        env.conns[1].failFlushes = 2; // both unmute tries reach the client, not the server
        const voice::MicGuard::Released r = guard.release();
        QCOMPARE(r.restored, 1);
        QCOMPARE(r.kept, 0);
        QCOMPARE(env.conns[1].muted, 0);
        QCOMPARE(env.conns[1].sets, 3);
    }

    void micGuardDestructorReleases()
    {
        FakeMic env;
        env.add(5, 1, 0);
        {
            voice::MicGuard guard(env);
            guard.engage();
            QCOMPARE(env.conns[5].muted, 1);
        } // plugin shutdown / an error path: the destructor gives it back
        QCOMPARE(env.conns[5].muted, 0);
        // Never engaged: nothing happens.
        {
            voice::MicGuard guard(env);
        }
        QCOMPARE(env.conns[5].sets, 2);
    }

    void micGuardNoConnections()
    {
        FakeMic env;
        voice::MicGuard guard(env);
        QCOMPARE(guard.engage(), 0);
        QCOMPARE(guard.release().restored, 0);
        QVERIFY(env.calls.isEmpty());
    }

    // ---- which microphone ----------------------------------------------------------------------
    void chooseDevice()
    {
        using Source = voice::DeviceChoice::Source;
        const voice::EndpointList list = endpoints();
        voice::TeamSpeakCapture   ts;
        ts.known = true;
        ts.mode  = QStringLiteral("Windows Audio Session");

        // The setting wins when that microphone is there (case doesn't matter).
        ts.device         = list.endpoints.at(0).id;
        voice::DeviceChoice c = voice::chooseCaptureDevice(list.endpoints.at(2).id.toUpper(), ts, list);
        QCOMPARE(c.source, Source::Setting);
        QCOMPARE(c.endpointId, list.endpoints.at(2).id);
        QVERIFY(!c.settingMissing);

        // A picked microphone that is unplugged: TeamSpeak's, and the window says so.
        c = voice::chooseCaptureDevice(QStringLiteral("{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}"), ts, list);
        QCOMPARE(c.source, Source::TeamSpeak);
        QCOMPARE(c.endpointId, list.endpoints.at(0).id);
        QVERIFY(c.settingMissing);

        // WASAPI mode: the id (alone or inside a longer string).
        c = voice::chooseCaptureDevice({}, ts, list);
        QCOMPARE(c.source, Source::TeamSpeak);
        QCOMPARE(c.endpointId, list.endpoints.at(0).id);
        ts.device = QStringLiteral("SWD\\MMDEVAPI\\") + list.endpoints.at(1).id;
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).endpointId, list.endpoints.at(1).id);

        // The device name, with the id from TeamSpeak's own list.
        ts.device  = QStringLiteral("My Headset");
        ts.devices = {qMakePair(QStringLiteral("My Headset"), list.endpoints.at(0).id)};
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).endpointId, list.endpoints.at(0).id);

        // DirectSound: a friendly name, maybe cut at 31 characters.
        ts.mode = QStringLiteral("DirectSound");
        ts.devices.clear();
        ts.device = QStringLiteral("microphone (realtek high definition audio)");
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).endpointId, list.endpoints.at(1).id);
        ts.device = QStringLiteral("Headset Microphone (USB Audio D");
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).endpointId, list.endpoints.at(0).id);

        // Ambiguous ("Microphone (" fits two): no guess.
        ts.device = QStringLiteral("Microphone (");
        c         = voice::chooseCaptureDevice({}, ts, list);
        QCOMPARE(c.source, Source::DefaultCommunications);
        QVERIFY(c.endpointId.isEmpty());

        // Unknown, "Default" or no answer: Windows' default communications microphone.
        ts.device = QStringLiteral("Some USB mic that isn't plugged in");
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).source, Source::DefaultCommunications);
        ts.device    = list.endpoints.at(0).id;
        ts.isDefault = true;
        QCOMPARE(voice::chooseCaptureDevice({}, ts, list).source, Source::DefaultCommunications);
        QCOMPARE(voice::chooseCaptureDevice({}, voice::TeamSpeakCapture(), list).source, Source::DefaultCommunications);
        QCOMPARE(voice::chooseCaptureDevice({}, ts, voice::EndpointList()).source, Source::DefaultCommunications);
    }

    void endpointIdIn()
    {
        const QString id = QStringLiteral("{0.0.1.00000000}.{11111111-2222-3333-4444-555555555555}");
        QCOMPARE(voice::endpointIdIn(id), id);
        QCOMPARE(voice::endpointIdIn(QStringLiteral("x") + id + QStringLiteral("y")), id);
        QVERIFY(voice::endpointIdIn(QStringLiteral("{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}")).isEmpty()); // a render endpoint
        QVERIFY(voice::endpointIdIn(QStringLiteral("Microphone")).isEmpty());
    }

    // ---- sample conversion ------------------------------------------------------------------------
    void convertSamples()
    {
        std::vector<int16_t> out;
        // Float stereo: the channels averaged, clipped to full scale.
        const float stereo[6] = {0.5f, -0.5f, 1.0f, 1.0f, 2.0f, 2.0f};
        voice::appendMono16(stereo, 3, {2, 32, true}, false, out);
        QCOMPARE(out.size(), size_t(3));
        QCOMPARE(out[0], int16_t(0));
        QCOMPARE(out[1], int16_t(32767));
        QCOMPARE(out[2], int16_t(32767));
        // 24-bit mono: the top 16 bits.
        out.clear();
        const unsigned char s24[6] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0}; // +0.5, -0.5
        voice::appendMono16(s24, 2, {1, 24, false}, false, out);
        QCOMPARE(out[0], int16_t(16384));
        QCOMPARE(out[1], int16_t(-16384));
        // 32-bit integer and 16-bit stereo.
        out.clear();
        const int32_t s32[1] = {-1073741824}; // -0.5
        voice::appendMono16(s32, 1, {1, 32, false}, false, out);
        QCOMPARE(out[0], int16_t(-16384));
        out.clear();
        const int16_t s16[4] = {1000, 3000, -32768, -32768};
        voice::appendMono16(s16, 2, {2, 16, false}, false, out);
        QCOMPARE(out[0], int16_t(2000));
        QCOMPARE(out[1], int16_t(-32768));
        // SILENT packets and layouts we can't read: zeros of the right length.
        out.clear();
        voice::appendMono16(s16, 2, {2, 16, false}, true, out);
        voice::appendMono16(s16, 2, {2, 8, false}, false, out);
        QCOMPARE(out, std::vector<int16_t>(4, 0));
        QVERIFY(!voice::isConvertible({0, 16, false}));
        QVERIFY(!voice::isConvertible({2, 16, true}));
        QVERIFY(voice::isConvertible({8, 24, false}));
    }

    // ---- the start / stop sounds --------------------------------------------------------------------
    void cues()
    {
        for (const voice::Cue cue : {voice::Cue::Start, voice::Cue::Stop}) {
            const QByteArray wav = voice::cueWav(cue);
            const int        frames = 48000 * voice::kCueMs / 1000;
            QCOMPARE(wav.size(), 44 + frames * 2);
            QCOMPARE(wav.left(4), QByteArray("RIFF"));
            QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
            QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 22), quint16(1));     // mono
            QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 24), quint32(48000)); // 48 kHz
            QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 34), quint16(16));
            QCOMPARE(wav.mid(36, 4), QByteArray("data"));
            const auto* pcm  = reinterpret_cast<const int16_t*>(wav.constData() + 44);
            const float peak = waveform::peakDb(pcm, frames);
            QVERIFY2(peak <= -17.9f && peak >= -18.6f, qPrintable(QString::number(peak))); // -18 dBFS
            QCOMPARE(pcm[0], int16_t(0));                                                 // faded in and out
            QCOMPARE(pcm[frames - 1], int16_t(0));
        }
        QVERIFY(voice::cueWav(voice::Cue::Start) != voice::cueWav(voice::Cue::Stop));
    }

    // ---- names and the setting ------------------------------------------------------------------------
    void names()
    {
        MediaLink link = MediaLink::parse(QStringLiteral("ts3file://h?port=9987&serverUID=u&channel=1&path=%2Ftsmedia&filename=voice_message_3f9a1c2e.m4a"
                                                         "&isDir=0&size=1000&fileDateTime=1760025720&tsm=2&d=12040&vm=1"));
        QVERIFY(link.voice);
        QCOMPARE(displayNameFor(link), QStringLiteral("Voice message.m4a"));
        const QString expected = QStringLiteral("Voice message %1.m4a").arg(QDateTime::fromSecsSinceEpoch(1760025720).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH-mm")));
        QCOMPARE(voiceSaveName(link), expected);
        link.dateTime = 0;
        QCOMPARE(voiceSaveName(link), QStringLiteral("Voice message.m4a"));
        // vm on something that isn't audio is dropped by the parser: the file keeps its name.
        const MediaLink exe = MediaLink::parse(QStringLiteral("ts3file://h?port=9987&serverUID=u&channel=1&path=%2F&filename=voice_3f9a1c2e.exe"
                                                              "&isDir=0&size=1000&fileDateTime=1&tsm=2&vm=1"));
        QVERIFY(!exe.voice);
        QCOMPARE(displayNameFor(exe), QStringLiteral("voice.exe"));
    }

    void microphoneSetting()
    {
        const QString id = QStringLiteral("{0.0.1.00000000}.{11111111-2222-3333-4444-555555555555}");
        QCOMPARE(Settings::validVoiceMicrophone(id), id);
        QCOMPARE(Settings::validVoiceMicrophone(QStringLiteral("  ") + id + QStringLiteral(" ")), id);
        QVERIFY(Settings::validVoiceMicrophone(QString(Settings::maxVoiceMicrophoneLength + 1, QLatin1Char('a'))).isEmpty());
        QCOMPARE(Settings::validVoiceMicrophone(QString(Settings::maxVoiceMicrophoneLength, QLatin1Char('a'))).size(), Settings::maxVoiceMicrophoneLength);
        QVERIFY(Settings::validVoiceMicrophone(QStringLiteral("mic\nx")).isEmpty());
        QVERIFY(Settings::validVoiceMicrophone(QString::fromUtf8("\u0645\u06cc\u06a9\u0631\u0648\u0641\u0648\u0646")).isEmpty());
        const Settings defaults;
        QVERIFY(defaults.voiceMicrophone.isEmpty());
        QVERIFY(defaults.voiceMuteTeamSpeakMic);
        QVERIFY(defaults.voiceReview);
        QVERIFY(defaults.voiceSounds);
    }
};

TSMEDIA_REGISTER_TEST(TestVoice)

#include "tst_voice.moc"
