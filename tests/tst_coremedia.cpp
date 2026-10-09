// 2.2 integration, end to end inside Core (core.cpp against the fake TeamSpeak of fakets3.*), for what
// wave 2 added to sending: a video compressed before it is sent (the link's SHA-256 describes the MP4
// that was uploaded, its album waits for it and is numbered again without it, a failed upload keeps the
// MP4 for Retry, the server's own upload limit) and a recorded voice message (hashed like any file,
// vm/d/wf in the link, kept for Retry until dismissed). The test video is made here with Media
// Foundation's H.264 encoder (fine grain at a high rate, so it is large); the voice message with the plugin's AAC writer.
// Nothing is played and no microphone is opened.

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "audio/aacwriter.h"
#include "core.h"
#include "fakets3.h"
#include "fileverify.h"
#include "hashing.h"
#include "i18n.h"
#include "serversettings.h"
#include "settings.h"
#include "testmain.h"
#include "ts3api.h"
#include "video/mfcommon.h"
#include "video/mfvideo.h"

namespace {

constexpr int kTimeoutMs = 60000; // a transcode on the processor, on a slow machine

using Microsoft::WRL::ComPtr;

// An H.264 MP4 of a moving gradient with fine grain: costly at the high rate it is written with, cheap
// once the grain is smoothed away at a compression's rate (pure noise would stay large at any rate).
// Written on a thread of its own with its own COM and Media Foundation scopes, like the plugin's workers.
bool writeNoiseVideo(const QString& path, int width, int height, int fps, int seconds, int kbps)
{
    bool ok = false;
    std::thread worker([&] {
        mf::detail::ComScope      com(COINIT_MULTITHREADED);
        mf::detail::PlatformScope platform;
        if (!com.usable() || FAILED(platform.result()))
            return;
        ComPtr<IMFSinkWriter> writer;
        if (FAILED(mf::detail::createMpeg4SinkWriter(path, nullptr, false, writer.GetAddressOf())))
            return;
        ComPtr<IMFMediaType> out;
        ComPtr<IMFMediaType> in;
        if (FAILED(MFCreateMediaType(out.GetAddressOf())) || FAILED(MFCreateMediaType(in.GetAddressOf())))
            return;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(kbps) * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        DWORD stream = 0;
        if (FAILED(writer->AddStream(out.Get(), &stream)))
            return;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(writer->SetInputMediaType(stream, in.Get(), nullptr)) || FAILED(writer->BeginWriting()))
            return;
        const DWORD    bytes    = static_cast<DWORD>(width * height * 3 / 2);
        const LONGLONG duration = 10000000LL / fps;
        quint32        state    = 0x9e3779b9u;
        for (int frame = 0; frame < fps * seconds; ++frame) {
            ComPtr<IMFMediaBuffer> buffer;
            BYTE*                  data = nullptr;
            if (FAILED(MFCreateMemoryBuffer(bytes, buffer.GetAddressOf())) || FAILED(buffer->Lock(&data, nullptr, nullptr)))
                return;
            for (int y = 0; y < height; ++y) { // luma: a gradient moving right, plus grain of +-12
                for (int x = 0; x < width; ++x) {
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    const int value = 40 + ((x + y / 2 + frame * 8) % 176) + static_cast<int>(state % 25) - 12;
                    data[y * width + x] = static_cast<BYTE>(qBound(0, value, 255));
                }
            }
            memset(data + width * height, 128, static_cast<size_t>(bytes) - static_cast<size_t>(width * height)); // grey chroma
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            ComPtr<IMFSample> sample;
            if (FAILED(MFCreateSample(sample.GetAddressOf())) || FAILED(sample->AddBuffer(buffer.Get())))
                return;
            sample->SetSampleTime(frame * duration);
            sample->SetSampleDuration(duration);
            if (FAILED(writer->WriteSample(stream, sample.Get())))
                return;
        }
        ok = SUCCEEDED(writer->Finalize());
    });
    worker.join();
    return ok && QFileInfo(path).size() > 0;
}

QByteArray pattern(int size, int seed)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 8) + seed) & 0xff);
    return data;
}

bool writeBytes(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(data) == data.size();
}

QList<MediaLink> linksIn(const fakets3::Message& message)
{
    return MediaLink::findInMessage(QString::fromUtf8(message.text));
}

QStringList filesUnder(const QString& dir)
{
    QStringList  files;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    return files;
}

} // namespace

class TestCoreMedia : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void compressOptionsUseTheServersLimit();
    void compressedVideoInAlbumIsHashedAsSent();
    void canceledVideoLeavesTheAlbumRenumbered();
    void failedCompressedUploadKeepsTheMp4ForRetry();
    void voiceMessageIsHashedAndKeptForRetry();
    void voiceDownloadsFollowDataSaverNotSpoilers();
    void ownFileGoesWithItsFolderWhenDismissed();

  private:
    ChatTarget channel() const;
    QString    video(const QString& name, int seconds);
    void       serve();
    bool       serveUntil(const std::function<bool()>& done, int timeoutMs = kTimeoutMs);
    int        jobIn(UploadState state) const;
    QString    dataDir() const { return ts3::dataDir(); }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Core>          m_core;
    QSet<QString>                  m_answered;
    QSet<anyID>                    m_completed;
    QSet<anyID>                    m_skip;
    bool                           m_holdUploads = false; // serve() leaves uploads alone (to refuse one)
    QSet<int>                      m_compressedIds; // jobs seen in UploadState::Compressing
    QHash<int, QString>            m_heldTexts;     // job -> its message while held
    TS3Functions                   m_savedFuncs{};
    QString                        m_savedPluginId;
    Settings                       m_savedSettings;
    bool                           m_mediaFoundation = false;
};

void TestCoreMedia::initTestCase()
{
    m_savedFuncs      = ts3::funcs;
    m_savedPluginId   = ts3::pluginId;
    m_savedSettings   = Settings::instance();
    m_mediaFoundation = mf::available() && mf::startup();
}

void TestCoreMedia::cleanupTestCase()
{
    if (m_mediaFoundation)
        mf::shutdown();
    ts3::funcs           = m_savedFuncs;
    ts3::pluginId        = m_savedPluginId;
    Settings::instance() = m_savedSettings;
    fileverify::resetCounters();
}

void TestCoreMedia::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    fakets3::install(m_dir->path());
    Settings& s               = Settings::instance();
    s                         = Settings();
    s.uploadDirectory         = QStringLiteral("/");
    s.generatePreviews        = false;
    s.addRequiredNotice       = false;
    s.videoAutoDownloadMB     = 0;
    s.compressVideos          = true;
    s.compressVideosOverMB    = 1;
    s.compressVideoQuality    = 720;
    s.convertUnplayableVideos = true;
    s.compressUseGpu          = false; // the same encoder on every machine
    m_answered.clear();
    m_completed.clear();
    m_skip.clear();
    m_holdUploads = false;
    m_compressedIds.clear();
    m_heldTexts.clear();
    m_core = std::make_unique<Core>();
    connect(m_core.get(), &Core::uploadChanged, this, [this](int id) {
        const UploadJob* job = m_core ? m_core->upload(id) : nullptr;
        if (!job)
            return;
        if (job->state == UploadState::Compressing)
            m_compressedIds.insert(id);
        if (job->state == UploadState::Posting && job->waiting)
            m_heldTexts.insert(id, job->message);
    });
    m_core->start();
}

void TestCoreMedia::cleanup()
{
    m_core.reset();
    m_dir.reset();
}

ChatTarget TestCoreMedia::channel() const
{
    ChatTarget target;
    target.sch  = fakets3::kConnection;
    target.mode = TextMessageTarget_CHANNEL;
    return target;
}

// A 1280x720 30 fps video with grain at 16 Mbit/s: about 2 MB a second, well over the 1 MB threshold,
// compressed to 720p at 2.5 Mbit/s.
QString TestCoreMedia::video(const QString& name, int seconds)
{
    const QString path = m_dir->path() + QStringLiteral("/outside/") + name;
    QDir().mkpath(QFileInfo(path).absolutePath());
    return writeNoiseVideo(path, 1280, 720, 30, seconds, 16000) ? path : QString();
}

void TestCoreMedia::serve()
{
    for (const fakets3::Directory& d : fakets3::directories()) {
        if (m_answered.contains(d.returnCode))
            continue;
        m_answered.insert(d.returnCode);
        m_core->onServerError(fakets3::kConnection, ERROR_ok, d.returnCode, QString(), false);
    }
    for (const fakets3::Transfer& t : fakets3::uploads()) {
        if (m_holdUploads || t.halted || m_completed.contains(t.id) || m_skip.contains(t.id))
            continue;
        m_completed.insert(t.id);
        if (fakets3::completeUpload(t.id))
            m_core->onTransferStatus(t.id, ERROR_file_transfer_complete, QString(), fakets3::kConnection);
    }
    for (const fakets3::Message& message : fakets3::messages()) {
        if (m_answered.contains(message.returnCode))
            continue;
        m_answered.insert(message.returnCode);
        m_core->onServerError(fakets3::kConnection, ERROR_ok, message.returnCode, QString(), false);
    }
}

bool TestCoreMedia::serveUntil(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        serve();
        if (done())
            return true;
        QTest::qWait(20);
    }
    serve();
    return done();
}

int TestCoreMedia::jobIn(UploadState state) const
{
    for (const int id : m_core->uploadIds()) {
        const UploadJob* job = m_core->upload(id);
        if (job && job->state == state)
            return id;
    }
    return 0;
}

// ---- compression ----------------------------------------------------------------------------------

void TestCoreMedia::compressOptionsUseTheServersLimit()
{
    // decisions: the upload size limit can be a server's own; compression plans against that one.
    Settings& s   = Settings::instance();
    s.uploadMaxMB = 100;
    ServerOverrides own;
    own.uploadMaxMB = 7;
    s.servers.insert(serversettings::serverKey(QString::fromLatin1(fakets3::kServerUid)), own);
    QCOMPARE(Core::compressOptions(SendQuality::Auto, QString::fromLatin1(fakets3::kServerUid)).limitBytes, 7ull * 1024 * 1024);
    QCOMPARE(Core::compressOptions(SendQuality::Auto, QStringLiteral("anotherServer")).limitBytes, 100ull * 1024 * 1024);
    QCOMPARE(Core::compressOptions(SendQuality::Auto).limitBytes, 100ull * 1024 * 1024);
    QCOMPARE(Core::compressOptions(SendQuality::P480, QString::fromLatin1(fakets3::kServerUid)).request, videocompress::Request::P480);
}

void TestCoreMedia::compressedVideoInAlbumIsHashedAsSent()
{
    if (!m_mediaFoundation)
        QSKIP("Media Foundation is not available on this computer");
    // An album of a photo, a video and a photo. The photos are uploaded long before the video is
    // compressed and wait for it ("Waiting for the rest of the album…"); then one message carries all
    // three, the video as the MP4 that was uploaded, its sha that of the uploaded bytes.
    const QString clip = video(QStringLiteral("tripclip.mp4"), 4);
    QVERIFY(!clip.isEmpty());
    const QByteArray original = [&] {
        QFile f(clip);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    SendRequest request;
    request.target = channel();
    request.album  = true;
    const QString photo1 = m_dir->path() + QStringLiteral("/outside/one.jpg");
    const QString photo2 = m_dir->path() + QStringLiteral("/outside/two.jpg");
    QVERIFY(writeBytes(photo1, pattern(30000, 1)));
    QVERIFY(writeBytes(photo2, pattern(30000, 2)));
    for (const QString& path : {photo1, clip, photo2}) {
        SendItem item;
        item.path = path;
        request.items.append(item);
    }
    QVERIFY(m_core->send(request) != 0);
    QVERIFY(serveUntil([&] { return fakets3::messages().size() == 1; }));
    QCOMPARE(m_compressedIds.size(), 1); // the video, and only the video
    const int videoId = *m_compressedIds.cbegin();
    // The photos waited for the video while it was compressed.
    int heldForAlbum = 0;
    for (auto it = m_heldTexts.cbegin(); it != m_heldTexts.cend(); ++it)
        heldForAlbum += it.key() != videoId && it.value() == i18n::t("Waiting for the rest of the album…") ? 1 : 0;
    QCOMPARE(heldForAlbum, 2);

    const QList<MediaLink> links = linksIn(fakets3::messages().first());
    QCOMPARE(links.size(), 3);
    for (int i = 0; i < links.size(); ++i) {
        QVERIFY(links.at(i).hasAlbum());
        QCOMPARE(links.at(i).albumCount, 3);
        QCOMPARE(links.at(i).albumIndex, i + 1);
    }
    const MediaLink sent = links.at(1);
    QVERIFY2(sent.fileName.endsWith(QLatin1String(".mp4")), qPrintable(sent.fileName));
    QVERIFY2(sent.fileName.startsWith(QLatin1String("tripclip_")), qPrintable(sent.fileName));
    const QByteArray uploaded = fakets3::serverFile(sent.remoteFile());
    QVERIFY(!uploaded.isEmpty());
    QVERIFY2(uploaded.size() < original.size() / 2, qPrintable(QStringLiteral("%1 of %2").arg(uploaded.size()).arg(original.size())));
    QCOMPARE(sent.size, static_cast<quint64>(uploaded.size()));
    QCOMPARE(sent.sha256, hashing::sha256(uploaded)); // the compressed bytes ...
    QVERIFY(sent.sha256 != hashing::sha256(original)); // ... never the original's
    QCOMPARE(sent.width, 1280);
    QCOMPARE(sent.height, 720);
    QVERIFY2(qAbs(sent.durationMs - 4000) <= 200, qPrintable(QString::number(sent.durationMs)));
    QVERIFY(m_core->upload(videoId));
    QCOMPARE(m_core->upload(videoId)->compression, CompressOutcome::Compressed);
    QVERIFY(m_core->lastCompression().contains(QLatin1String("720p")));
    QVERIFY(QFileInfo(clip).isFile()); // the user's file is never touched
}

void TestCoreMedia::canceledVideoLeavesTheAlbumRenumbered()
{
    if (!m_mediaFoundation)
        QSKIP("Media Foundation is not available on this computer");
    // decisions: a canceled (or failed) item is left out of its album and the rest are numbered again.
    const QString clip = video(QStringLiteral("long.mp4"), 8);
    QVERIFY(!clip.isEmpty());
    SendRequest request;
    request.target = channel();
    request.album  = true;
    const QString photo1 = m_dir->path() + QStringLiteral("/outside/a.jpg");
    const QString photo2 = m_dir->path() + QStringLiteral("/outside/b.jpg");
    QVERIFY(writeBytes(photo1, pattern(30000, 3)));
    QVERIFY(writeBytes(photo2, pattern(30000, 4)));
    for (const QString& path : {photo1, clip, photo2}) {
        SendItem item;
        item.path = path;
        request.items.append(item);
    }
    // The photos go up once the video is being compressed (a few hundred ms on the processor), and are
    // then held for it.
    m_holdUploads = true;
    QVERIFY(m_core->send(request) != 0);
    QVERIFY(serveUntil([&] { return !m_compressedIds.isEmpty(); }));
    m_holdUploads = false;
    QVERIFY(serveUntil([&] {
        const int compressing = jobIn(UploadState::Compressing);
        int       held        = 0;
        for (const int id : m_core->uploadIds()) {
            const UploadJob* job = m_core->upload(id);
            held += job && job->state == UploadState::Posting && job->waiting ? 1 : 0;
        }
        return compressing != 0 && held == 2;
    }));
    const int videoId = jobIn(UploadState::Compressing);
    QVERIFY(m_core->canCancelUpload(videoId));
    m_core->cancelUpload(videoId);
    QVERIFY(serveUntil([&] { return fakets3::messages().size() == 1; }));
    const QList<MediaLink> links = linksIn(fakets3::messages().first());
    QCOMPARE(links.size(), 2);
    for (int i = 0; i < links.size(); ++i) {
        QVERIFY2(links.at(i).fileName.endsWith(QLatin1String(".jpg")), qPrintable(links.at(i).fileName));
        QCOMPARE(links.at(i).albumCount, 2);
        QCOMPARE(links.at(i).albumIndex, i + 1);
    }
    // Nothing of the compression is left behind.
    QTRY_VERIFY_WITH_TIMEOUT(filesUnder(dataDir() + QStringLiteral("/upload")).isEmpty(), 10000);
}

void TestCoreMedia::failedCompressedUploadKeepsTheMp4ForRetry()
{
    if (!m_mediaFoundation)
        QSKIP("Media Foundation is not available on this computer");
    // The own-file rule: what Core made for a send (here the compressed MP4) stays while its failed job
    // can be retried; Retry sends it without compressing again; afterwards it is gone.
    const QString clip = video(QStringLiteral("holiday.mp4"), 3);
    QVERIFY(!clip.isEmpty());
    m_holdUploads = true;
    m_core->uploadFiles({clip}, channel());
    QVERIFY(serveUntil([&] { return !fakets3::uploads().isEmpty() || jobIn(UploadState::Failed) != 0; }, kTimeoutMs));
    QCOMPARE(fakets3::uploads().size(), 1);
    const fakets3::Transfer first = fakets3::uploads().first();
    QVERIFY(first.remotePath.endsWith(QLatin1String(".mp4")));
    m_skip.insert(first.id);
    m_holdUploads = false;
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, first.returnCode, QStringLiteral("refused"), false);
    const int failed = jobIn(UploadState::Failed);
    QVERIFY(failed != 0);
    const QStringList kept = filesUnder(dataDir() + QStringLiteral("/compressed"));
    QCOMPARE(kept.size(), 1);
    QCOMPARE(QFileInfo(kept.first()).fileName(), QStringLiteral("holiday.mp4"));
    QVERIFY(m_core->canRetryUpload(failed));

    m_compressedIds.clear();
    const QByteArray keptBytes = [&] {
        QFile f(kept.first());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    const int again = m_core->retryUpload(failed);
    QVERIFY(again != 0);
    QVERIFY(serveUntil([&] { return fakets3::messages().size() == 1; }));
    QVERIFY(m_compressedIds.isEmpty()); // not compressed twice
    const QList<MediaLink> links = linksIn(fakets3::messages().first());
    QCOMPARE(links.size(), 1);
    QVERIFY2(links.first().fileName.startsWith(QLatin1String("holiday_")) && links.first().fileName.endsWith(QLatin1String(".mp4")),
             qPrintable(links.first().fileName));
    const QByteArray uploaded = fakets3::serverFile(links.first().remoteFile());
    QCOMPARE(uploaded, keptBytes);
    QCOMPARE(links.first().sha256, hashing::sha256(uploaded));
    QVERIFY(serveUntil([&] { return m_core->upload(again) && m_core->upload(again)->state == UploadState::Done; }));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(kept.first()), 5000);
    QVERIFY(!QFileInfo::exists(QFileInfo(kept.first()).absolutePath())); // its folder went with it

    // Dismissed instead of retried: deleted too.
    m_holdUploads = true;
    m_core->uploadFiles({clip}, channel());
    QVERIFY(serveUntil([&] { return fakets3::uploads().size() == 3; }, kTimeoutMs)); // the earlier two, then this one
    const fakets3::Transfer third = fakets3::uploads().last();
    m_skip.insert(third.id);
    m_holdUploads = false;
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, third.returnCode, QStringLiteral("refused"), false);
    const int failedAgain = jobIn(UploadState::Failed);
    QVERIFY(failedAgain != 0);
    QCOMPARE(filesUnder(dataDir() + QStringLiteral("/compressed")).size(), 1);
    m_core->dismissUpload(failedAgain);
    QVERIFY(filesUnder(dataDir() + QStringLiteral("/compressed")).isEmpty());
}

// ---- voice messages and other own files -------------------------------------------------------------

void TestCoreMedia::voiceMessageIsHashedAndKeptForRetry()
{
    if (!m_mediaFoundation)
        QSKIP("Media Foundation is not available on this computer");
    // As the recorder sends one: its own file in <data>/voice, vm=1 with the length and the waveform.
    const QString        path = dataDir() + QStringLiteral("/voice/voice_0badf00d.m4a");
    std::vector<int16_t> samples(48000 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 48000.0));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QVERIFY(voice::writeAac(path, samples.data(), static_cast<qint64>(samples.size()), 48000, 1.0).ok);
    QByteArray levels;
    for (int i = 0; i < MediaLink::kWaveformLevels; ++i)
        levels.append(static_cast<char>(i % 16));
    SendRequest request;
    request.target = channel();
    SendItem item;
    item.path        = path;
    item.displayName = QStringLiteral("Voice message");
    item.ownTemp     = true;
    item.voice       = true;
    item.durationMs  = 2000;
    item.waveform    = levels;
    request.items.append(item);
    m_holdUploads = true;
    QVERIFY(m_core->send(request) != 0);

    // The first try is refused: the recording exists nowhere else, so it stays for Retry.
    QVERIFY(serveUntil([] { return !fakets3::uploads().isEmpty(); }, 10000));
    m_holdUploads = false;
    const fakets3::Transfer first = fakets3::uploads().first();
    QVERIFY2(first.remotePath.contains(QLatin1String("voice_message_")) && first.remotePath.endsWith(QLatin1String(".m4a")), qPrintable(first.remotePath));
    m_skip.insert(first.id);
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, first.returnCode, QStringLiteral("refused"), false);
    const int failed = jobIn(UploadState::Failed);
    QVERIFY(failed != 0);
    QVERIFY(QFileInfo(path).isFile());
    QVERIFY(m_core->canRetryUpload(failed));
    const int again = m_core->retryUpload(failed);
    QVERIFY(again != 0);
    QVERIFY(serveUntil([&] { return fakets3::messages().size() == 1; }, 10000));
    QVERIFY(m_compressedIds.isEmpty());

    const QList<MediaLink> links = linksIn(fakets3::messages().first());
    QCOMPARE(links.size(), 1);
    const MediaLink sent = links.first();
    QVERIFY(sent.voice);
    QCOMPARE(sent.durationMs, qint64(2000));
    QCOMPARE(sent.waveform, levels);
    QVERIFY(!sent.spoiler);
    const QByteArray uploaded = fakets3::serverFile(sent.remoteFile());
    QVERIFY(!uploaded.isEmpty());
    QCOMPARE(sent.sha256, hashing::sha256(uploaded)); // hashed like any other upload
    QVERIFY(serveUntil([&] { return m_core->upload(again) && m_core->upload(again)->state == UploadState::Done; }, 10000));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 5000); // sent: the recording is gone
}

void TestCoreMedia::voiceDownloadsFollowDataSaverNotSpoilers()
{
    // A received voice message downloads by itself like an image (decisions: at most 16 MB); the data
    // saver holds it for a click; a spoiler flag means nothing on a voice message.
    Settings& s          = Settings::instance();
    s.inlinePreviews     = true;
    s.autoDownloadImages = true;
    s.autoDownloadMaxMB  = 15;
    const auto voiceLink = [](const QString& name, bool spoiler) {
        MediaLink link;
        link.host       = QString::fromLatin1(fakets3::kHost);
        link.port       = fakets3::kPort;
        link.serverUid  = QString::fromLatin1(fakets3::kServerUid);
        link.channelId  = fakets3::kChannel;
        link.path       = QStringLiteral("/tsmedia");
        link.fileName   = name;
        link.size       = 48000;
        link.dateTime   = 1760000000;
        link.protocol   = MediaLink::kProtocol;
        link.voice      = true;
        link.durationMs = 3000;
        link.spoiler    = spoiler;
        for (int i = 0; i < MediaLink::kWaveformLevels; ++i)
            link.waveform.append(static_cast<char>(i % 16));
        return MediaLink::parse(link.toUrl()); // as a receiver gets it
    };
    const MediaLink first = voiceLink(QStringLiteral("voice_message_00aa11bb.m4a"), true);
    QVERIFY(first.voice);
    m_core->ensure(first);
    QCOMPARE(fakets3::downloads().size(), 1); // downloaded although marked as a spoiler
    QVERIFY(!m_core->isSpoilerHidden(first.key()));

    s.dataSaver            = true;
    const MediaLink second = voiceLink(QStringLiteral("voice_message_22cc33dd.m4a"), false);
    m_core->ensure(second);
    QCOMPARE(fakets3::downloads().size(), 1); // held for a click
    QVERIFY(m_core->entry(second.key()) && m_core->entry(second.key())->heldByDataSaver);
}

void TestCoreMedia::ownFileGoesWithItsFolderWhenDismissed()
{
    // An edited copy as the send window hands it over (<data>/edit/<hex>/<name>, ownTemp): kept while
    // its failed job can be retried, deleted with its folder once the job is dismissed.
    const QString path = dataDir() + QStringLiteral("/edit/1a2b3c4d/holiday.png");
    QVERIFY(writeBytes(path, pattern(40000, 5)));
    SendRequest request;
    request.target = channel();
    SendItem item;
    item.path        = path;
    item.ownTemp     = true;
    item.displayName = QStringLiteral("holiday.png");
    request.items.append(item);
    m_holdUploads = true;
    QVERIFY(m_core->send(request) != 0);
    QVERIFY(serveUntil([] { return !fakets3::uploads().isEmpty(); }, 10000));
    m_holdUploads = false;
    const fakets3::Transfer upload = fakets3::uploads().first();
    m_skip.insert(upload.id);
    m_core->onServerError(fakets3::kConnection, ERROR_parameter_invalid, upload.returnCode, QStringLiteral("refused"), false);
    const int failed = jobIn(UploadState::Failed);
    QVERIFY(failed != 0);
    QVERIFY(QFileInfo(path).isFile());
    QVERIFY(m_core->canRetryUpload(failed));
    m_core->dismissUpload(failed);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
}

TSMEDIA_REGISTER_TEST(TestCoreMedia)

#include "tst_coremedia.moc"
