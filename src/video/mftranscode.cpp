#include "mftranscode.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QVector>

#include <windows.h>

#include <codecapi.h>
#include <d3d10_1.h> // MF headers want d3d10_1.h rather than d3d10.h
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cmath>
#include <utility>

#include "mfcommon.h"
#include "mfvideo.h"

using Microsoft::WRL::ComPtr;

namespace mf {

namespace {

using Stage = TranscodeResult::Stage;

constexpr LONGLONG kHnsPerSecond          = 10'000'000;
constexpr LONGLONG kHnsPerMs              = 10'000;
constexpr int      kStallMs               = 20'000; // no progress inside the writer for this long: give up
constexpr int      kSizeCheckMs           = 2'000;
constexpr int      kSizeCheckFromPermille = 150;        // projections before this are too rough
constexpr LONGLONG kAudioTickHns          = 5'000'000;  // a stream tick for ended sound every 500 ms of video
constexpr int      kMaxPendingAudio       = 4096;       // sound before the first frame, waiting for it
constexpr DWORD    kNoStream              = MAXDWORD;
constexpr qint64   kMinLengthToleranceMs  = 1000;

bool isDiskFull(HRESULT hr)
{
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) || hr == STG_E_MEDIUMFULL;
}

Stage classify(HRESULT hr, Stage fallback)
{
    if (isDiskFull(hr))
        return Stage::Disk;
    if (hr == E_OUTOFMEMORY)
        return Stage::Memory;
    return fallback;
}

// Below normal while transcoding, so TeamSpeak's own threads (voice) come first. Pool threads are reused:
// the old priority comes back afterwards.
class ThreadPriority
{
  public:
    ThreadPriority()
        : m_old(GetThreadPriority(GetCurrentThread()))
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    ~ThreadPriority()
    {
        if (m_old != THREAD_PRIORITY_ERROR_RETURN)
            SetThreadPriority(GetCurrentThread(), m_old);
    }
    ThreadPriority(const ThreadPriority&)            = delete;
    ThreadPriority& operator=(const ThreadPriority&) = delete;

  private:
    int m_old;
};

struct Streams {
    DWORD video = kNoStream;
    DWORD audio = kNoStream;
};

Streams findStreams(IMFSourceReader* reader)
{
    Streams streams;
    for (DWORD index = 0; index < 64; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT        hr = reader->GetNativeMediaType(index, 0, &type);
        if (hr == MF_E_INVALIDSTREAMNUMBER)
            break;
        if (FAILED(hr))
            continue;
        GUID major = GUID_NULL;
        type->GetMajorType(&major);
        if (major == MFMediaType_Video && streams.video == kNoStream)
            streams.video = index;
        else if (major == MFMediaType_Audio && streams.audio == kNoStream)
            streams.audio = index;
    }
    return streams;
}

int defaultEncoderThreads()
{
    const int cores = qMax(1, QThread::idealThreadCount());
    return qBound(1, cores / 2, sizeof(void*) == 4 ? 4 : 8);
}

// The real frame rate of a video: the median gap between the presentation times of its first compressed
// frames (nothing is decoded, no graphics memory is held). Some containers report half the real rate
// (WebM and MKV from ffmpeg here). 0 when it can't be told.
double compressedFrameRate(const QString& path)
{
    ComPtr<IMFSourceReader> reader;
    if (FAILED(detail::createSourceReader(path, nullptr, &reader)))
        return 0.0;
    const Streams streams = findStreams(reader.Get());
    if (streams.video == kNoStream || FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE))
        || FAILED(reader->SetStreamSelection(streams.video, TRUE)))
        return 0.0;
    QVector<LONGLONG> times;
    for (int i = 0; i < 32; ++i) {
        DWORD             stream = 0, flags = 0;
        LONGLONG          timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(streams.video, 0, &stream, &flags, &timestamp, &sample)) || (flags & MF_SOURCE_READERF_ERROR))
            break;
        if (sample)
            times.append(timestamp);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    std::sort(times.begin(), times.end()); // decode order: B-frames come before the frames they precede
    QVector<LONGLONG> gaps;
    for (int i = 1; i < times.size(); ++i) {
        if (times.at(i) > times.at(i - 1))
            gaps.append(times.at(i) - times.at(i - 1));
    }
    if (gaps.size() < 3)
        return 0.0;
    std::sort(gaps.begin(), gaps.end());
    const double fps = static_cast<double>(kHnsPerSecond) / static_cast<double>(gaps.at(gaps.size() / 2));
    return fps >= 1.0 && fps <= 240.0 ? fps : 0.0;
}

// The rate the AAC encoder gets: 44.1 kHz for that family, 48 kHz otherwise (it takes only these two).
UINT32 aacSampleRate(UINT32 source)
{
    return source != 0 && 44100 % source == 0 ? 44100 : source == 88200 || source == 176400 ? 44100 : 48000;
}

// One try with or without the graphics card. Everything it creates is released when it returns, on
// this thread.
class Attempt
{
  public:
    Attempt(const TranscodeRequest& request, TranscodeControl* control, bool hardware, double measuredFps)
        : m_request(request)
        , m_control(control)
        , m_hardware(hardware)
        , m_measuredFps(measuredFps)
    {
    }

    TranscodeResult run();

  private:
    bool fail(Stage stage, HRESULT hr, const char* what)
    {
        m_result.stage  = classify(hr, stage);
        m_result.hr     = static_cast<quint32>(hr);
        m_result.detail = QString::fromLatin1(what) + QString::fromLatin1(" (") + detail::hexCode(hr) + QLatin1Char(')');
        return false;
    }
    bool canceled() const { return m_control && m_control->cancel.load(); }

    bool openReader();
    bool setReaderTypes();
    bool measureFrameRate();
    bool openWriter(UINT32 profile);
    void identifyEncoder();
    bool pump();
    bool writeVideo(IMFSample* sample, LONGLONG timestamp);
    ComPtr<IMFSample> plainFrame(IMFSample* sample) const;
    QString           readerChain() const;
    bool writeAudio(IMFSample* sample, LONGLONG timestamp);
    bool waitForEncoder();
    void updateProgress(LONGLONG written);
    bool checkSize();

    const TranscodeRequest& m_request;
    TranscodeControl*       m_control;
    const bool              m_hardware;
    const double            m_measuredFps; // from the compressed stream's timestamps; 0 if unknown
    TranscodeResult         m_result;

    ComPtr<ID3D11Device>         m_device;
    ComPtr<ID3D11DeviceContext>  m_context;
    ComPtr<IMFDXGIDeviceManager> m_manager;
    ComPtr<IMFSourceReader>      m_reader;
    ComPtr<IMFSinkWriter>        m_writer;
    ComPtr<IMFMediaType>         m_videoIn; // NV12 from the reader
    ComPtr<IMFMediaType>         m_audioIn; // PCM from the reader

    Streams  m_streams;
    bool     m_withAudio   = false;
    DWORD    m_videoOut    = 0;
    DWORD    m_audioOut    = 0;
    UINT32   m_rateNum     = 30; // output frame rate
    UINT32   m_rateDen     = 1;
    UINT32   m_nominalNum  = 0; // the container's frame rate (a fallback)
    UINT32   m_nominalDen  = 0;
    double   m_sourceFps   = 0.0;
    bool     m_decimate    = false;
    int      m_sourceRotation = 0;
    int      m_emptyFrames    = 0;
    int      m_copiedFrames   = 0;
    LONGLONG m_frameHns    = kHnsPerSecond / 30;
    LONGLONG m_sourceFrameHns = kHnsPerSecond / 30;
    LONGLONG m_nextKeep    = 0;
    LONGLONG m_base        = -1; // the first frame's timestamp: the output starts there
    LONGLONG m_lastVideo   = 0;
    LONGLONG m_lastTick    = 0;
    bool     m_videoEnded  = false;
    bool     m_audioEnded  = true;
    qint64   m_encoderDepth   = -1; // frames the encoder keeps before its first output; -1 until it has output
    qint64   m_firstOutputCap = sizeof(void*) == 4 ? 24 : 48; // frames given before the first output, at most
    QVector<QPair<LONGLONG, ComPtr<IMFSample>>> m_pendingAudio;
    QElapsedTimer m_sizeClock;
    QElapsedTimer m_clock;
};

bool Attempt::openReader()
{
    HRESULT hr = S_OK;
    if (m_hardware) {
        // A real graphics card only: WARP would be slower than the processor path.
        hr = detail::createD3D11Device(false, &m_device, &m_context);
        if (SUCCEEDED(hr))
            hr = detail::createDxgiDeviceManager(m_device.Get(), &m_manager);
        if (FAILED(hr))
            return fail(Stage::Encoder, hr, "no graphics device");
    }
    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 4);
    // Colour conversion to NV12, scaling, deinterlacing and rotation inside the reader.
    if (SUCCEEDED(hr))
        hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr))
        hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, m_hardware ? TRUE : FALSE);
    if (SUCCEEDED(hr) && m_manager)
        hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_manager.Get());
    if (SUCCEEDED(hr))
        hr = detail::createSourceReader(m_request.source, attributes.Get(), &m_reader);
    if (FAILED(hr))
        return fail(Stage::Source, hr, "open the source");

    m_streams = findStreams(m_reader.Get());
    if (m_streams.video == kNoStream)
        return fail(Stage::Source, MF_E_INVALIDSTREAMNUMBER, "no video stream");
    m_withAudio = m_request.audioKbps > 0;
    if (m_withAudio && m_streams.audio == kNoStream)
        return fail(Stage::Source, MF_E_INVALIDSTREAMNUMBER, "no sound track");
    m_audioEnded = !m_withAudio;

    // Only the first picture and the first sound: subtitles, other sound tracks and metadata are left out.
    hr = m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr))
        hr = m_reader->SetStreamSelection(m_streams.video, TRUE);
    if (SUCCEEDED(hr) && m_withAudio)
        hr = m_reader->SetStreamSelection(m_streams.audio, TRUE);
    if (FAILED(hr))
        return fail(Stage::Source, hr, "select streams");
    return true;
}

bool Attempt::setReaderTypes()
{
    ComPtr<IMFMediaType> native;
    HRESULT              hr = m_reader->GetNativeMediaType(m_streams.video, 0, &native);
    if (FAILED(hr))
        return fail(Stage::Source, hr, "read the video type");
    const detail::Geometry geometry = detail::geometryOf(native.Get());

    // The container's nominal rate is only a fallback: some sources report half the real rate (WebM and
    // MKV here), which would halve the encoder's bit budget per frame. measureFrameRate() decides.
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &m_nominalNum, &m_nominalDen)) && m_nominalNum > 0 && m_nominalDen > 0)
        m_sourceFps = static_cast<double>(m_nominalNum) / m_nominalDen;

    // NV12 at the planned size, progressive, square pixels. No frame rate: the processor would convert
    // to it (dropping frames when the container's rate is wrong).
    const UINT32         width  = static_cast<UINT32>(m_request.frameSize.width());
    const UINT32         height = static_cast<UINT32>(m_request.frameSize.height());
    ComPtr<IMFMediaType> nv12;
    hr = MFCreateMediaType(&nv12);
    if (SUCCEEDED(hr))
        hr = nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr))
        hr = nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(nv12.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr))
        hr = m_reader->SetCurrentMediaType(m_streams.video, nullptr, nv12.Get());
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(hr))
        hr = m_reader->GetCurrentMediaType(m_streams.video, &current);
    // A copy for the writer (it gets the measured frame rate).
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&m_videoIn);
    if (SUCCEEDED(hr))
        hr = current->CopyAllItems(m_videoIn.Get());
    if (FAILED(hr))
        return fail(Stage::Source, hr, "decode to NV12 at the planned size");

    UINT32 outWidth = 0, outHeight = 0;
    MFGetAttributeSize(m_videoIn.Get(), MF_MT_FRAME_SIZE, &outWidth, &outHeight);
    if (outWidth != width || outHeight != height)
        return fail(Stage::Source, MF_E_INVALIDMEDIATYPE, "the reader can't resize to the planned size");
    // The reader's processor turns phone videos upright; if it didn't, the picture would be sideways.
    m_sourceRotation = geometry.rotation;
    if (MFGetAttributeUINT32(m_videoIn.Get(), MF_MT_VIDEO_ROTATION, 0) != 0)
        return fail(Stage::Source, MF_E_INVALIDMEDIATYPE, "the reader can't turn the picture upright");

    if (!m_withAudio)
        return true;

    // 16-bit PCM at 44.1 or 48 kHz, mono or stereo: what the AAC encoder takes. The reader resamples and
    // mixes down (5.1 to stereo, 96 kHz to 48 kHz).
    ComPtr<IMFMediaType> nativeAudio;
    hr                       = m_reader->GetNativeMediaType(m_streams.audio, 0, &nativeAudio);
    const UINT32 sourceRate  = SUCCEEDED(hr) ? MFGetAttributeUINT32(nativeAudio.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) : 0;
    const UINT32 rate        = aacSampleRate(sourceRate);
    const UINT32 channels    = m_request.audioChannels == 1 ? 1 : 2;
    ComPtr<IMFMediaType> pcm;
    hr = MFCreateMediaType(&pcm);
    if (SUCCEEDED(hr))
        hr = pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr))
        hr = pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr))
        hr = pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr))
        hr = pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    if (SUCCEEDED(hr))
        hr = pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    if (SUCCEEDED(hr))
        hr = m_reader->SetCurrentMediaType(m_streams.audio, nullptr, pcm.Get());
    if (SUCCEEDED(hr))
        hr = m_reader->GetCurrentMediaType(m_streams.audio, &m_audioIn);
    if (FAILED(hr))
        return fail(Stage::Source, hr, "decode the sound to PCM");
    if (MFGetAttributeUINT32(m_audioIn.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0) != channels
        || MFGetAttributeUINT32(m_audioIn.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) != rate
        || MFGetAttributeUINT32(m_audioIn.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != 16)
        return fail(Stage::Source, MF_E_INVALIDMEDIATYPE, "the reader can't convert the sound"); // never drop sound silently
    return true;
}

// The output frame rate, from the rate measured on the compressed stream (compressedFrameRate) and the
// container's: the encoder plans its bits per frame from it.
bool Attempt::measureFrameRate()
{
    const double measured = m_measuredFps;
    const double nominal  = m_sourceFps;
    double       real    = measured >= 1.0 && measured <= 240.0 ? measured : nominal;
    // The container's exact ratio (30000/1001) when it agrees with what was measured.
    const bool nominalAgrees = nominal > 0.0 && (measured <= 0.0 || qAbs(nominal - measured) / nominal < 0.05);
    UINT32     exactNum      = m_nominalNum;
    UINT32     exactDen      = m_nominalDen;
    if (nominalAgrees) {
        real = nominal;
    } else if (real > 0.0) {
        // Millisecond timestamps (Matroska) measure 30.303 for 30: the nearest common rate when close.
        static const UINT32 common[][2] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1}, {50, 1}, {60000, 1001}, {60, 1}, {120, 1}};
        exactNum       = static_cast<UINT32>(std::lround(real * 1000.0));
        exactDen       = 1000;
        double nearest = 0.015;
        for (const auto& rate : common) {
            const double value = static_cast<double>(rate[0]) / rate[1];
            const double off   = qAbs(value - real) / value;
            if (off < nearest) {
                nearest  = off;
                exactNum = rate[0];
                exactDen = rate[1];
            }
        }
        real = static_cast<double>(exactNum) / exactDen;
    }
    m_sourceFps = real;

    const int cap = qBound(1, m_request.fps, 60);
    if (real > 0.0 && real <= cap + 0.5) {
        m_rateNum = exactNum; // every frame, at the source's own rate
        m_rateDen = exactDen;
    } else {
        m_rateNum  = static_cast<UINT32>(cap);
        m_rateDen  = 1;
        m_decimate = real > 0.0;
    }
    m_frameHns       = static_cast<LONGLONG>(std::llround(static_cast<double>(kHnsPerSecond) * m_rateDen / m_rateNum));
    m_sourceFrameHns = real > 0.0 ? static_cast<LONGLONG>(std::llround(kHnsPerSecond / real)) : m_frameHns;
    const HRESULT hr = MFSetAttributeRatio(m_videoIn.Get(), MF_MT_FRAME_RATE, m_rateNum, m_rateDen);
    if (FAILED(hr))
        return fail(Stage::Source, hr, "set the frame rate");
    m_result.detail = QString::fromLatin1("frame rate %1 (container %2, measured %3)")
                          .arg(static_cast<double>(m_rateNum) / m_rateDen, 0, 'f', 3)
                          .arg(nominal, 0, 'f', 3)
                          .arg(measured, 0, 'f', 3);
    return true;
}

bool Attempt::openWriter(UINT32 profile)
{
    m_writer.Reset();
    QFile::remove(m_request.target);
    HRESULT hr = detail::createMpeg4SinkWriter(m_request.target, m_hardware ? m_manager.Get() : nullptr, m_hardware, &m_writer);
    if (FAILED(hr))
        return fail(Stage::Disk, hr, "create the output file");

    const UINT32         width  = static_cast<UINT32>(m_request.frameSize.width());
    const UINT32         height = static_cast<UINT32>(m_request.frameSize.height());
    const UINT32         bps    = static_cast<UINT32>(qMax(1, m_request.videoKbps)) * 1000;
    ComPtr<IMFMediaType> h264;
    hr = MFCreateMediaType(&h264);
    if (SUCCEEDED(hr))
        hr = h264->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = h264->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr))
        hr = h264->SetUINT32(MF_MT_AVG_BITRATE, bps);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(h264.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(h264.Get(), MF_MT_FRAME_RATE, m_rateNum, m_rateDen);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(h264.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr))
        hr = h264->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = h264->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    if (SUCCEEDED(hr))
        hr = m_writer->AddStream(h264.Get(), &m_videoOut);
    if (FAILED(hr))
        return fail(Stage::Encoder, hr, "add the H.264 stream");

    // Unconstrained VBR around the mean, a key frame every 2 s (seeking in the player), balanced speed.
    const UINT32          fps = qMax<UINT32>(1, m_rateNum / qMax<UINT32>(1, m_rateDen));
    ComPtr<IMFAttributes> params;
    hr = MFCreateAttributes(&params, 5);
    if (SUCCEEDED(hr))
        hr = params->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
    if (SUCCEEDED(hr))
        hr = params->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bps);
    if (SUCCEEDED(hr))
        hr = params->SetUINT32(CODECAPI_AVEncMPVGOPSize, 2 * fps);
    if (SUCCEEDED(hr))
        hr = params->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 50);
    // The software encoder takes half the cores, so TeamSpeak's voice keeps room.
    if (SUCCEEDED(hr) && !m_hardware)
        hr = params->SetUINT32(CODECAPI_AVEncNumWorkerThreads, static_cast<UINT32>(m_request.encoderThreads > 0 ? m_request.encoderThreads : defaultEncoderThreads()));
    if (SUCCEEDED(hr))
        hr = m_writer->SetInputMediaType(m_videoOut, m_videoIn.Get(), params.Get());
    if (FAILED(hr))
        return fail(Stage::Encoder, hr, profile == eAVEncH264VProfile_High ? "set up the H.264 encoder (High)" : "set up the H.264 encoder (Main)");

    if (m_withAudio) {
        const UINT32         channels = MFGetAttributeUINT32(m_audioIn.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
        const UINT32         rate     = MFGetAttributeUINT32(m_audioIn.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        ComPtr<IMFMediaType> aac;
        hr = MFCreateMediaType(&aac);
        if (SUCCEEDED(hr))
            hr = aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr))
            hr = aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr))
            hr = aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr))
            hr = aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        if (SUCCEEDED(hr))
            hr = aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        // The Microsoft AAC encoder takes 12000/16000/20000/24000 bytes per second (96..192 kbps).
        if (SUCCEEDED(hr))
            hr = aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(qBound(96, m_request.audioKbps, 192)) * 1000 / 8);
        if (SUCCEEDED(hr))
            hr = aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(hr))
            hr = m_writer->AddStream(aac.Get(), &m_audioOut);
        if (SUCCEEDED(hr))
            hr = m_writer->SetInputMediaType(m_audioOut, m_audioIn.Get(), nullptr);
        if (FAILED(hr))
            return fail(Stage::Encoder, hr, "set up the AAC encoder");
    }

    hr = m_writer->BeginWriting();
    if (FAILED(hr))
        return fail(Stage::Encoder, hr, "start the encoders");
    return true;
}

// Which encoder the writer picked: the graphics card's or the processor's (Media Foundation decides).
void Attempt::identifyEncoder()
{
    ComPtr<IMFSinkWriterEx> writerEx;
    if (FAILED(m_writer.As(&writerEx)))
        return;
    for (DWORD index = 0; index < 8; ++index) {
        GUID                 category = GUID_NULL;
        ComPtr<IMFTransform> transform;
        if (FAILED(writerEx->GetTransformForStream(m_videoOut, index, &category, &transform)))
            break;
        if (category != MFT_CATEGORY_VIDEO_ENCODER)
            continue;
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
            UINT32 length = 0;
            m_result.hardware = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length));
            WCHAR* name       = nullptr;
            if (SUCCEEDED(attributes->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name) {
                m_result.encoderName = QString::fromWCharArray(name, static_cast<int>(length));
                CoTaskMemFree(name);
            }
        }
        if (m_result.encoderName.isEmpty())
            m_result.encoderName = m_result.hardware ? QString::fromLatin1("hardware H.264 encoder") : QString::fromLatin1("H264 Encoder MFT");
        break;
    }
    if (m_control)
        m_control->encoder.store(m_result.hardware ? 2 : 1);
}

// Throttling is off (WriteSample never blocks, so cancel and unload stay quick): wait here while too
// many frames are on their way through the encoder. An encoder keeps some frames before its first output
// (the software one about one per worker thread, hardware ones their look-ahead); that depth is learnt at
// the first output and allowed on top, so waiting never starves an encoder that needs more input.
bool Attempt::waitForEncoder()
{
    QElapsedTimer stall;
    stall.start();
    LONGLONG lastProcessed = -1;
    for (;;) {
        MF_SINK_WRITER_STATISTICS stats = {};
        stats.cb                        = sizeof(stats);
        if (FAILED(m_writer->GetStatistics(m_videoOut, &stats)))
            return true;
        const qint64 inFlight = static_cast<qint64>(stats.qwNumSamplesReceived) - static_cast<qint64>(stats.qwNumSamplesEncoded);
        if (m_encoderDepth < 0 && stats.qwNumSamplesEncoded > 0)
            m_encoderDepth = qBound<qint64>(0, inFlight, m_firstOutputCap);
        const qint64 cap = m_encoderDepth < 0 ? m_firstOutputCap : m_encoderDepth + qMax(1, m_request.maxQueuedFrames);
        if (inFlight <= cap)
            return true;
        if (canceled())
            return false;
        const LONGLONG processed = static_cast<LONGLONG>(stats.qwNumSamplesProcessed + stats.qwNumSamplesEncoded);
        if (processed != lastProcessed) {
            lastProcessed = processed;
            stall.restart();
        } else if (stall.elapsed() > kStallMs) {
            fail(Stage::Stall, HRESULT_FROM_WIN32(ERROR_TIMEOUT), "the encoder stopped taking frames");
            m_result.detail += QString::fromLatin1(": received %1, encoded %2, processed %3, queued %4 bytes, sink requests %5")
                                   .arg(stats.qwNumSamplesReceived)
                                   .arg(stats.qwNumSamplesEncoded)
                                   .arg(stats.qwNumSamplesProcessed)
                                   .arg(stats.dwByteCountQueued)
                                   .arg(stats.dwNumOutstandingSinkSampleRequests);
            return false;
        }
        Sleep(2);
    }
}

// An NV12 frame in a plain system-memory buffer, copied row by row through the frame's 2-D buffer.
// nullptr if the frame can't be read that way.
ComPtr<IMFSample> Attempt::plainFrame(IMFSample* sample) const
{
    ComPtr<IMFMediaBuffer> source;
    ComPtr<IMF2DBuffer>    source2d;
    if (FAILED(sample->GetBufferByIndex(0, &source)) || FAILED(source.As(&source2d)))
        return nullptr;
    const DWORD width  = static_cast<DWORD>(m_request.frameSize.width());
    const DWORD height = static_cast<DWORD>(m_request.frameSize.height());
    const DWORD size   = width * height * 3 / 2;
    BYTE*       scan0  = nullptr;
    LONG        pitch  = 0;
    if (FAILED(source2d->Lock2D(&scan0, &pitch)))
        return nullptr;
    ComPtr<IMFSample>      out;
    ComPtr<IMFMediaBuffer> buffer;
    BYTE*                  target = nullptr;
    bool                   ok     = pitch >= static_cast<LONG>(width) && SUCCEEDED(MFCreateMemoryBuffer(size, &buffer)) && SUCCEEDED(buffer->Lock(&target, nullptr, nullptr));
    // NV12: the luma rows, then the interleaved chroma rows (half as many), each at the same pitch. The
    // chroma starts after the buffer's own (maybe padded) luma height.
    DWORD contiguous = 0;
    DWORD lumaRows   = height;
    if (ok && SUCCEEDED(source2d->GetContiguousLength(&contiguous)) && contiguous >= static_cast<DWORD>(pitch) * height * 3 / 2)
        lumaRows = contiguous / static_cast<DWORD>(pitch) * 2 / 3;
    if (ok) {
        for (DWORD row = 0; row < height; ++row)
            memcpy(target + static_cast<size_t>(row) * width, scan0 + static_cast<ptrdiff_t>(row) * pitch, width);
        const BYTE* chroma = scan0 + static_cast<ptrdiff_t>(lumaRows) * pitch;
        for (DWORD row = 0; row < height / 2; ++row)
            memcpy(target + static_cast<size_t>(height + row) * width, chroma + static_cast<ptrdiff_t>(row) * pitch, width);
        buffer->Unlock();
        ok = SUCCEEDED(buffer->SetCurrentLength(size)) && SUCCEEDED(MFCreateSample(&out)) && SUCCEEDED(out->AddBuffer(buffer.Get()));
    }
    source2d->Unlock2D();
    if (!ok)
        return nullptr;
    LONGLONG duration = 0;
    if (SUCCEEDED(sample->GetSampleDuration(&duration)))
        out->SetSampleDuration(duration);
    return out;
}

// "decoder name (hardware) > processor name" of the reader's video stream, for the log.
QString Attempt::readerChain() const
{
    ComPtr<IMFSourceReaderEx> readerEx;
    if (!m_reader || FAILED(m_reader.As(&readerEx)))
        return {};
    QStringList parts;
    for (DWORD index = 0; index < 8; ++index) {
        GUID                 category = GUID_NULL;
        ComPtr<IMFTransform> transform;
        if (FAILED(readerEx->GetTransformForStream(m_streams.video, index, &category, &transform)))
            break;
        QString               name = category == MFT_CATEGORY_VIDEO_DECODER ? QString::fromLatin1("decoder") : QString::fromLatin1("processor");
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
            WCHAR* text   = nullptr;
            UINT32 length = 0;
            if (SUCCEEDED(attributes->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &text, &length)) && text) {
                name = QString::fromWCharArray(text, static_cast<int>(length));
                CoTaskMemFree(text);
            }
            if (SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)))
                name += QString::fromLatin1(" (hardware)");
            if (MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE))
                name += QString::fromLatin1(" (D3D11)");
        }
        parts << name;
    }
    QString chain = parts.join(QString::fromLatin1(" > "));
    // The graphics card the device runs on (several may be installed).
    ComPtr<IDXGIDevice>  dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC    desc = {};
    if (m_device && SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
        chain += QString::fromLatin1(" on ") + QString::fromWCharArray(desc.Description);
    return chain;
}

bool Attempt::writeVideo(IMFSample* sample, LONGLONG timestamp)
{
    if (m_base < 0) {
        m_base = timestamp;
        // Sound that came before the first frame: from the first frame on, shifted like the picture.
        const auto pending = std::exchange(m_pendingAudio, {});
        for (const auto& audio : pending) {
            if (!writeAudio(audio.second.Get(), audio.first))
                return false;
        }
    }
    const LONGLONG time = qMax<LONGLONG>(0, timestamp - m_base);
    ComPtr<IMFSample> copy;
    if (!m_hardware) {
        // The software AV1 decoder's frames say they hold 0 bytes (the picture is reachable through the
        // 2-D buffer only), which the processor's encoder refuses: such frames are copied into a plain one.
        ComPtr<IMFMediaBuffer> buffer;
        DWORD                  length = 0;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->GetCurrentLength(&length)) && length == 0) {
            copy = plainFrame(sample);
            if (!copy) {
                ++m_emptyFrames;
                return true; // nothing in it
            }
            sample = copy.Get();
            ++m_copiedFrames;
        }
    }
    if (m_decimate) {
        // Keep the frames nearest to the output rate's grid.
        if (time + m_sourceFrameHns / 2 < m_nextKeep)
            return true;
        m_nextKeep = qMax(m_nextKeep + m_frameHns, time + m_frameHns);
        sample->SetSampleDuration(m_frameHns);
    }
    sample->SetSampleTime(time);
    if (!waitForEncoder())
        return false;
    const HRESULT hr = m_writer->WriteSample(m_videoOut, sample);
    if (FAILED(hr)) {
        fail(Stage::Encoder, hr, "encode a frame");
        // What the frame looked like (graphics memory or not), for the log.
        DWORD                  buffers = 0;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer>  dxgi;
        DWORD                  length = 0;
        sample->GetBufferCount(&buffers);
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
            buffer->GetCurrentLength(&length);
            buffer.As(&dxgi);
        }
        m_result.detail += QString::fromLatin1(": %1 buffer(s), %2 bytes%3").arg(buffers).arg(length).arg(dxgi ? QString::fromLatin1(", graphics memory") : QString());
        return false;
    }
    m_lastVideo = qMax(m_lastVideo, time);
    // After the sound ended, ticks keep the writer from waiting for sound that won't come.
    if (m_withAudio && m_audioEnded && m_lastVideo - m_lastTick >= kAudioTickHns) {
        m_writer->SendStreamTick(m_audioOut, m_lastVideo);
        m_lastTick = m_lastVideo;
    }
    updateProgress(m_lastVideo);
    return true;
}

bool Attempt::writeAudio(IMFSample* sample, LONGLONG timestamp)
{
    if (m_base < 0) {
        if (m_pendingAudio.size() >= kMaxPendingAudio)
            return fail(Stage::Source, MF_E_INVALID_STREAM_DATA, "sound without pictures");
        m_pendingAudio.append({timestamp, ComPtr<IMFSample>(sample)});
        return true;
    }
    const LONGLONG time = timestamp - m_base;
    if (time < 0)
        return true; // before the first frame
    sample->SetSampleTime(time);
    const HRESULT hr = m_writer->WriteSample(m_audioOut, sample);
    if (FAILED(hr))
        return fail(Stage::Encoder, hr, "encode sound");
    return true;
}

void Attempt::updateProgress(LONGLONG written)
{
    if (!m_control || m_request.durationMs <= 0)
        return;
    const qint64 permille = qBound<qint64>(0, written / kHnsPerMs * 1000 / m_request.durationMs, 999);
    if (permille > m_control->permille.load())
        m_control->permille.store(static_cast<int>(permille));
}

// Every 2 s: where the output size is heading. Far over the target means the encoder misses its rate:
// stop now rather than after minutes.
bool Attempt::checkSize()
{
    if (m_request.abortAboveBytes == 0 || !m_control || m_sizeClock.elapsed() < kSizeCheckMs)
        return true;
    m_sizeClock.restart();
    const int permille = m_control->permille.load();
    if (permille < kSizeCheckFromPermille)
        return true;
    MF_SINK_WRITER_STATISTICS stats = {};
    stats.cb                        = sizeof(stats);
    if (FAILED(m_writer->GetStatistics(static_cast<DWORD>(MF_SINK_WRITER_ALL_STREAMS), &stats)))
        return true;
    const quint64 projected = stats.qwByteCountProcessed * 1000 / static_cast<quint64>(permille);
    if (static_cast<double>(projected) <= static_cast<double>(m_request.abortAboveBytes) * 1.03)
        return true;
    m_result.exceeded       = true;
    m_result.projectedBytes = projected;
    m_result.detail         = QString::fromLatin1("projected %1 bytes, above %2").arg(projected).arg(m_request.abortAboveBytes);
    return false;
}

bool Attempt::pump()
{
    m_sizeClock.start();
    LONGLONG      lastVideoRead = -1;
    LONGLONG      lastAudioRead = -1;
    QElapsedTimer poolWait; // graphics memory frames: how long the decoder's pool has been all in use
    while (!(m_videoEnded && m_audioEnded)) {
        if (canceled())
            return false;
        // The stream that is behind is read next, so the writer always gets both in step: the MP4 sink
        // interleaves them and takes no more pictures while it waits for sound (and the other way round).
        const bool        readAudio = m_withAudio && !m_audioEnded && (m_videoEnded || lastAudioRead <= lastVideoRead);
        DWORD             stream    = 0;
        DWORD             flags     = 0;
        LONGLONG          timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT     hr = m_reader->ReadSample(readAudio ? m_streams.audio : m_streams.video, 0, &stream, &flags, &timestamp, &sample);
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
            // Every frame of the decoder's graphics memory pool is still with the encoder: it gives
            // them back as it goes.
            if (!poolWait.isValid())
                poolWait.start();
            else if (poolWait.elapsed() > kStallMs)
                return fail(Stage::Stall, hr, "the encoder keeps every frame");
            Sleep(1);
            continue;
        }
        poolWait.invalidate();
        if (FAILED(hr))
            return fail(Stage::Source, hr, "read the source");
        if (flags & MF_SOURCE_READERF_ERROR)
            return fail(Stage::Source, MF_E_INVALID_STREAM_DATA, "the source reported an error");
        const bool video = !readAudio;
        const bool audio = readAudio;
        if (sample) {
            LONGLONG sampleDuration = 0;
            sample->GetSampleDuration(&sampleDuration);
            (video ? lastVideoRead : lastAudioRead) = timestamp + qMax<LONGLONG>(0, sampleDuration);
        }

        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && video) {
            ComPtr<IMFMediaType> type;
            UINT32               width = 0, height = 0;
            if (FAILED(m_reader->GetCurrentMediaType(m_streams.video, &type)) || FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height))
                || width != static_cast<UINT32>(m_request.frameSize.width()) || height != static_cast<UINT32>(m_request.frameSize.height()))
                return fail(Stage::Source, MF_E_INVALIDMEDIATYPE, "the picture size changed in the middle");
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) && m_base >= 0 && (video || audio))
            m_writer->SendStreamTick(video ? m_videoOut : m_audioOut, qMax<LONGLONG>(0, timestamp - m_base));

        if (sample) {
            if (video && !writeVideo(sample.Get(), timestamp))
                return false;
            if (audio && !writeAudio(sample.Get(), timestamp))
                return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (video)
                m_videoEnded = true;
            if (audio)
                m_audioEnded = true;
            if (video && m_base < 0)
                return fail(Stage::Source, MF_E_INVALID_STREAM_DATA, "no frame could be decoded");
        }
        if (!checkSize())
            return false;
    }
    return true;
}

TranscodeResult Attempt::run()
{
    m_clock.start();
    bool ok = openReader() && setReaderTypes() && measureFrameRate();
    if (ok) {
        ok = openWriter(eAVEncH264VProfile_High);
        if (!ok && m_result.stage == Stage::Encoder) {
            m_result = TranscodeResult();
            ok       = openWriter(eAVEncH264VProfile_Main); // some hardware encoders have no High profile
        }
    }
    if (ok) {
        identifyEncoder();
        const QString rate = m_result.detail;
        ok                 = pump();
        if (ok) {
            m_result.detail = rate + QString::fromLatin1("; reader: ") + readerChain();
            if (m_copiedFrames > 0)
                m_result.detail += QString::fromLatin1("; %1 frames copied to plain memory").arg(m_copiedFrames);
            if (m_emptyFrames > 0)
                m_result.detail += QString::fromLatin1("; %1 empty frames skipped").arg(m_emptyFrames);
        }
    }
    if (ok && !canceled()) {
        if (m_control)
            m_control->finishing.store(true);
        const HRESULT hr = m_writer->Finalize();
        if (FAILED(hr))
            ok = fail(Stage::Encoder, hr, "finish the file");
    }
    m_result.canceled = canceled() && !m_result.exceeded;
    m_result.ok       = ok && !m_result.canceled;
    // Released here, on the thread that made them: the writer first (it holds the encoders), then the reader.
    m_pendingAudio.clear();
    m_writer.Reset();
    m_reader.Reset();
    m_videoIn.Reset();
    m_audioIn.Reset();
    m_manager.Reset();
    m_context.Reset();
    m_device.Reset();
    m_result.elapsedMs = m_clock.elapsed();
    if (!m_result.ok)
        QFile::remove(m_request.target);
    return m_result;
}

// The output must open, have a picture of the planned size that is upright, sound if it should, and
// about the source's length. Anything else (a sideways phone video, a cut-off file) is not sent.
bool verify(const TranscodeRequest& request, TranscodeResult& result)
{
    const ProbeResult probe = mf::probe(request.target, 0);
    result.bytes            = static_cast<quint64>(qMax<qint64>(0, QFileInfo(request.target).size()));
    result.frameSize        = probe.size;
    result.durationMs       = probe.durationMs;
    const qint64 tolerance  = qMax<qint64>(kMinLengthToleranceMs, request.durationMs * 3 / 100);
    const char*  problem    = nullptr;
    if (!probe.ok || !probe.hasVideo)
        problem = "the output doesn't open";
    else if (probe.hasAudio != (request.audioKbps > 0))
        problem = "the output's sound is missing";
    else if (qAbs(probe.size.width() - request.frameSize.width()) > 2 || qAbs(probe.size.height() - request.frameSize.height()) > 2)
        problem = "the output has another size";
    else if (probe.rotation != 0)
        problem = "the output is not upright";
    else if (request.durationMs > 0 && qAbs(probe.durationMs - request.durationMs) > tolerance) {
        problem        = "the output has another length";
        result.shorter = probe.durationMs < request.durationMs;
    }
    else if (result.bytes == 0)
        problem = "the output is empty";
    if (!problem)
        return true;
    result.ok     = false;
    result.stage  = Stage::Verify;
    result.hr     = static_cast<quint32>(MF_E_INVALID_STREAM_DATA);
    result.detail = QString::fromLatin1(problem) + QString::fromLatin1(" (%1x%2, %3 ms)").arg(probe.size.width()).arg(probe.size.height()).arg(probe.durationMs);
    QFile::remove(request.target);
    return false;
}

QStringList encoderNames(UINT32 flags)
{
    QStringList            names;
    MFT_REGISTER_TYPE_INFO input  = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate**          found  = nullptr;
    UINT32                 count  = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input, &output, &found, &count)))
        return names;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name   = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(found[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name) {
            names << QString::fromWCharArray(name, static_cast<int>(length));
            CoTaskMemFree(name);
        }
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return names;
}

} // namespace

const char* stageName(TranscodeResult::Stage stage)
{
    switch (stage) {
    case Stage::None:
        return "none";
    case Stage::Source:
        return "source";
    case Stage::Encoder:
        return "encoder";
    case Stage::Disk:
        return "disk";
    case Stage::Memory:
        return "memory";
    case Stage::Stall:
        return "stall";
    case Stage::Verify:
        return "verify";
    }
    return "unknown";
}

TranscodeResult transcodeToMp4(const TranscodeRequest& request, TranscodeControl* control)
{
    TranscodeResult result;
    QElapsedTimer   clock;
    clock.start();
    if (!detail::mediaFoundationPresent()) {
        result.stage  = Stage::Source;
        result.hr     = static_cast<quint32>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        result.detail = QString::fromLatin1("Media Foundation is not available");
        return result;
    }
    if (request.frameSize.isEmpty() || request.frameSize.width() % 2 || request.frameSize.height() % 2) {
        result.stage  = Stage::Source;
        result.hr     = static_cast<quint32>(E_INVALIDARG);
        result.detail = QString::fromLatin1("bad frame size");
        return result;
    }
    detail::ComScope com(COINIT_MULTITHREADED);
    if (!com.usable()) {
        result.stage = Stage::Source;
        result.hr    = static_cast<quint32>(com.result());
        return result;
    }
    detail::PlatformScope platform;
    if (FAILED(platform.result())) {
        result.stage = Stage::Source;
        result.hr    = static_cast<quint32>(platform.result());
        return result;
    }
    ThreadPriority priority;
    QDir().mkpath(QFileInfo(request.target).absolutePath());

    const double measuredFps = compressedFrameRate(request.source);
    const auto   attempt     = [&request, control, measuredFps](bool hardware) {
        if (control) {
            control->permille.store(0);
            control->finishing.store(false);
            control->encoder.store(0);
        }
        Attempt         run(request, control, hardware, measuredFps);
        TranscodeResult r = run.run();
        if (r.ok)
            verify(request, r);
        return r;
    };

    if (request.allowHardware) {
        result = attempt(true);
        // A cut-off source gives the same short result on the processor: not worth a second run.
        const bool final = result.ok || result.canceled || result.exceeded || result.stage == Stage::Disk || result.shorter || (control && control->cancel.load());
        if (!final) {
            // Once more on the processor, from scratch: graphics drivers fail in many ways.
            const QString gpuError = QString::fromLatin1("%1: %2").arg(QString::fromLatin1(stageName(result.stage)), result.detail);
            const QString gpuName  = result.encoderName;
            result                 = attempt(false);
            result.gpuFailed       = true;
            result.gpuError        = gpuName.isEmpty() ? gpuError : gpuName + QString::fromLatin1(", ") + gpuError;
        }
    } else {
        result = attempt(false);
    }
    result.elapsedMs = clock.elapsed();
    return result;
}

double measuredFrameRate(const QString& path)
{
    if (!detail::mediaFoundationPresent())
        return 0.0;
    detail::ComScope com(COINIT_MULTITHREADED);
    if (!com.usable())
        return 0.0;
    detail::PlatformScope platform;
    if (FAILED(platform.result()))
        return 0.0;
    return compressedFrameRate(path);
}

EncoderList h264Encoders()
{
    EncoderList list;
    if (!detail::mediaFoundationPresent())
        return list;
    detail::ComScope com(COINIT_MULTITHREADED);
    if (!com.usable())
        return list;
    detail::PlatformScope platform;
    if (FAILED(platform.result()))
        return list;
    list.queried  = true;
    list.hardware = encoderNames(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER);
    list.software = encoderNames(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);
    return list;
}

} // namespace mf
