#include "mfvideo.h"

#include <QDir>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QTransform>
#include <QUrl>

#include <windows.h>

#include <d3d10_1.h> // ID3D10Multithread; MF headers want d3d10_1.h rather than d3d10.h
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "i18n.h"
#include "mfcommon.h"

using Microsoft::WRL::ComPtr;

namespace mf {

namespace {

constexpr LONGLONG kHnsPerMs              = 10000;
constexpr int      kMaxImageSide          = 16384;
constexpr int      kMaxTextureSide        = 8192;
constexpr int      kFrameIntervalMs       = 10;
constexpr int      kAudioIntervalMs       = 50; // audio only: no frames, the timer only reports the position
constexpr int      kPositionIntervalMs    = 250;
constexpr int      kFirstFramePollMs      = 3000; // keep polling this long after load for the first frame
constexpr LONGLONG kNoFrame               = std::numeric_limits<LONGLONG>::min(); // OnVideoStreamTick: nothing presented yet
constexpr int      kPosterMaxSamples      = 300;
constexpr int      kPosterTimeoutMs       = 5000;
constexpr DWORD    kFirstVideoStream      = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD    kFirstAudioStream      = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD    kAllStreams            = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD    kMediaSource           = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

std::mutex g_platformMutex;
int        g_platformRefs = 0;

using detail::ComScope;
using detail::failureText;
using detail::Geometry;
using detail::geometryOf;
using detail::mediaFoundationPresent;
using detail::PlatformScope;
using detail::platformUnavailableText;
using detail::Wording;

LONG defaultStride(IMFMediaType* type)
{
    return static_cast<LONG>(static_cast<INT32>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, 0)));
}

// Copies 32-bit BGRX rows into image and forces alpha to opaque.
void copyRows(QImage& image, const BYTE* scan0, LONG pitch)
{
    const int width = image.width();
    for (int y = 0; y < image.height(); ++y) {
        const auto* src = reinterpret_cast<const quint32*>(scan0 + static_cast<qptrdiff>(y) * pitch);
        auto*       dst = reinterpret_cast<quint32*>(image.scanLine(y));
        for (int x = 0; x < width; ++x)
            dst[x] = src[x] | 0xFF000000u;
    }
}

QImage imageFromSample(IMFSample* sample, const QSize& frame, LONG stride)
{
    if (!sample || frame.isEmpty())
        return {};
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return {};
    QImage image(frame, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    const LONG rowBytes = frame.width() * 4;

    // Lock2D hands out the top row and a signed pitch, which covers bottom-up buffers too.
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scan0 = nullptr;
        LONG  pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
            const bool ok = std::labs(pitch) >= rowBytes;
            if (ok)
                copyRows(image, scan0, pitch);
            buffer2d->Unlock2D();
            return ok ? image : QImage();
        }
    }

    BYTE* data      = nullptr;
    DWORD maxLength = 0, length = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &length)))
        return {};
    const LONG   pitch    = stride != 0 ? stride : rowBytes;
    const qint64 required = static_cast<qint64>(std::labs(pitch)) * (frame.height() - 1) + rowBytes;
    const bool   ok       = std::labs(pitch) >= rowBytes && required <= static_cast<qint64>(length);
    if (ok)
        copyRows(image, pitch < 0 ? data + static_cast<qptrdiff>(-pitch) * (frame.height() - 1) : data, pitch);
    buffer->Unlock();
    return ok ? image : QImage();
}

// Decodes the frame at min(1 s, 10% of the duration) as an upright image at display aspect.
QImage decodePoster(IMFSourceReader* reader, const Geometry& native, qint64 durationMs, int maxSide, HRESULT* status)
{
    reader->SetStreamSelection(kAllStreams, FALSE);
    reader->SetStreamSelection(kFirstVideoStream, TRUE);

    ComPtr<IMFMediaType> rgb;
    HRESULT              hr = MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr))
        hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr))
        hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, rgb.Get());
    ComPtr<IMFMediaType> output;
    if (SUCCEEDED(hr))
        hr = reader->GetCurrentMediaType(kFirstVideoStream, &output);
    if (FAILED(hr)) {
        *status = hr;
        return {};
    }

    const LONGLONG target = qMin<qint64>(1000, durationMs / 10) * kHnsPerMs;
    if (target > 0) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt            = VT_I8;
        position.hVal.QuadPart = target;
        reader->SetCurrentPosition(GUID_NULL, position); // not seekable: the poster comes from the start
        PropVariantClear(&position);
    }

    // Seeking lands on the key frame before the target; decode forward until the target is reached.
    ComPtr<IMFSample>    best;
    ComPtr<IMFMediaType> bestType;
    QElapsedTimer        clock;
    clock.start();
    for (int i = 0; i < kPosterMaxSamples && clock.elapsed() < kPosterTimeoutMs; ++i) {
        DWORD             flags     = 0;
        LONGLONG          timestamp = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(kFirstVideoStream, 0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(hr))
            break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            output.Reset();
            hr = reader->GetCurrentMediaType(kFirstVideoStream, &output);
            if (FAILED(hr))
                break;
        }
        if (sample) {
            best     = sample;
            bestType = output;
            if (timestamp >= target)
                break;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
            break;
    }
    if (!best) {
        *status = FAILED(hr) ? hr : MF_E_INVALID_STREAM_DATA;
        return {};
    }

    const Geometry decoded = geometryOf(bestType.Get());
    QImage         image   = imageFromSample(best.Get(), decoded.frame, defaultStride(bestType.Get()));
    if (image.isNull()) {
        *status = E_UNEXPECTED;
        return {};
    }

    // Decoders pad frames (1920x1088 for 1080p); keep the visible area only.
    QRect visible = decoded.aperture;
    if (visible == image.rect() && !native.aperture.isEmpty() && native.aperture != image.rect() && image.rect().contains(native.aperture))
        visible = native.aperture;
    if (visible != image.rect())
        image = image.copy(visible);

    // The advanced video processor normally applies rotation (and pixel aspect ratio) itself; rotate
    // only if the output type still carries a rotation or the picture still has the stored orientation.
    int rotation = decoded.rotation;
    if (rotation == 0 && (native.rotation == 90 || native.rotation == 270) && native.aperture.width() != native.aperture.height()) {
        const bool storedLandscape  = native.aperture.width() > native.aperture.height();
        const bool decodedLandscape = image.width() > image.height();
        if (storedLandscape == decodedLandscape)
            rotation = native.rotation;
    }
    if (rotation != 0)
        image = image.transformed(QTransform().rotate(rotation));

    QSize size = native.display.isEmpty() ? image.size() : native.display;
    if (size.width() > maxSide || size.height() > maxSide)
        size.scale(maxSide, maxSide, Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(1, 1));
    if (size != image.size())
        image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    *status = S_OK;
    return image;
}

} // namespace

// ================================================================================================
// Platform
// ================================================================================================

bool startup()
{
    if (!mediaFoundationPresent())
        return false;
    std::lock_guard<std::mutex> lock(g_platformMutex);
    if (g_platformRefs == 0 && FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
        return false;
    ++g_platformRefs;
    return true;
}

void shutdown()
{
    std::lock_guard<std::mutex> lock(g_platformMutex);
    if (g_platformRefs == 0)
        return;
    if (--g_platformRefs == 0)
        MFShutdown();
}

bool available()
{
    return mediaFoundationPresent();
}

// ================================================================================================
// Probe
// ================================================================================================

// ok means the container could be read (streams, duration, size). The poster may still be null when
// the video cannot be decoded on this machine; error then says why. posterMaxSide <= 0 skips the poster.
ProbeResult probe(const QString& path, int posterMaxSide)
{
    ProbeResult result;
    if (!mediaFoundationPresent()) {
        result.error = platformUnavailableText();
        return result;
    }

    ComScope com(COINIT_MULTITHREADED);
    if (!com.usable()) {
        result.error = failureText(com.result());
        return result;
    }
    PlatformScope platform;
    if (FAILED(platform.result())) {
        result.error = platformUnavailableText();
        return result;
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT               hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) // YUV -> RGB32 conversion (including 10-bit formats) inside the reader
        hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr))
        hr = detail::createSourceReader(path, attributes.Get(), &reader);
    if (FAILED(hr)) {
        result.error = failureText(hr);
        return result;
    }

    ComPtr<IMFMediaType> nativeVideo;
    ComPtr<IMFMediaType> nativeAudio;
    result.hasVideo = SUCCEEDED(reader->GetNativeMediaType(kFirstVideoStream, 0, &nativeVideo));
    result.hasAudio = SUCCEEDED(reader->GetNativeMediaType(kFirstAudioStream, 0, &nativeAudio));
    if (!result.hasVideo && !result.hasAudio) {
        result.error = detail::unsupportedFormatText(); // media sources hide tracks they cannot handle
        return result;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
        result.durationMs = static_cast<qint64>(duration.uhVal.QuadPart / kHnsPerMs);
    PropVariantClear(&duration);

    // 2.4 compress: frame rate, codec and sound layout for the compression planner.
    if (result.hasAudio) {
        result.audioChannels   = static_cast<int>(MFGetAttributeUINT32(nativeAudio.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        result.audioSampleRate = static_cast<int>(MFGetAttributeUINT32(nativeAudio.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
    }
    if (result.hasVideo) {
        UINT32 rateNum = 0, rateDen = 0;
        if (SUCCEEDED(MFGetAttributeRatio(nativeVideo.Get(), MF_MT_FRAME_RATE, &rateNum, &rateDen)) && rateNum > 0 && rateDen > 0)
            result.frameRate = static_cast<double>(rateNum) / rateDen;
        GUID subtype = GUID_NULL;
        nativeVideo->GetGUID(MF_MT_SUBTYPE, &subtype);
        result.videoCodec = detail::videoCodecName(subtype);
    }

    result.ok = true;
    if (!result.hasVideo)
        return result;

    const Geometry native = geometryOf(nativeVideo.Get());
    result.size           = native.display;
    result.rotation       = native.rotation; // 2.4 compress
    if (posterMaxSide <= 0)
        return result;

    result.poster = decodePoster(reader.Get(), native, result.durationMs, posterMaxSide, &hr);
    if (result.poster.isNull()) {
        GUID subtype = GUID_NULL;
        nativeVideo->GetGUID(MF_MT_SUBTYPE, &subtype);
        result.error = failureText(hr, subtype);
    } else if (result.size.isEmpty()) {
        result.size = result.poster.size();
    }
    return result;
}

// ================================================================================================
// VideoPlayer
// ================================================================================================

struct VideoPlayer::Private
{
    // Shared with the engine callback. The player clears it (under the mutex) before the engine is shut
    // down, so a callback running on a Media Foundation thread can never reach a destroyed player.
    struct Guard {
        std::mutex   mutex;
        VideoPlayer* player     = nullptr;
        quint64      generation = 0;
    };

    // Receives engine events on Media Foundation threads and queues them to the GUI thread.
    class Notify final : public IMFMediaEngineNotify
    {
      public:
        explicit Notify(std::shared_ptr<Guard> guard)
            : m_guard(std::move(guard))
        {
        }

        STDMETHODIMP QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
                *object = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG refs = --m_refs;
            if (refs == 0)
                delete this;
            return refs;
        }

        STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
        {
            if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
                SetEvent(reinterpret_cast<HANDLE>(param1));
                return S_OK;
            }
            std::lock_guard<std::mutex> lock(m_guard->mutex);
            VideoPlayer* player = m_guard->player;
            if (!player)
                return S_OK;
            const quint64 engineGeneration = m_guard->generation;
            QMetaObject::invokeMethod(
                player,
                [player, engineGeneration, event, param1, param2] { player->d->onEngineEvent(engineGeneration, event, param1, param2); },
                Qt::QueuedConnection);
            return S_OK;
        }

      private:
        ~Notify() = default;

        std::atomic<ULONG>     m_refs{1};
        std::shared_ptr<Guard> m_guard;
    };

    struct State {
        bool playing = false;
        bool ended   = false;
        bool muted   = false;

        bool operator!=(const State& other) const { return playing != other.playing || ended != other.ended || muted != other.muted; }
    };

    explicit Private(VideoPlayer* owner)
        : q(owner)
    {
    }

    ~Private()
    {
        release();
        if (platformStarted)
            shutdown();
    }

    bool    createEngine(QString* error);
    void    release();
    void    failLater(const QString& error);
    void    onEngineEvent(quint64 eventGeneration, DWORD event, DWORD_PTR param1, DWORD param2);
    QString engineErrorText(DWORD_PTR code, HRESULT hr) const;
    Wording wording() const { return audioOnly ? Wording::Audio : Wording::Video; }
    void    tick();
    bool    transferFrame();
    bool    ensureTextures(const QSize& size);
    QSize   nativeVideoSize() const;
    void    updateVideoSize();
    QSize   outputSize() const;
    MFVideoNormalizedRect sourceRect(const QSize& target) const;
    bool    waitingForFirstFrame() const;
    void    updateTimer();
    void    checkState();

    VideoPlayer* q;
    ComScope     com{COINIT_APARTMENTTHREADED}; // declared before the COM pointers, so destroyed after them
    bool         platformStarted = startup();

    std::shared_ptr<Guard>       guard;
    ComPtr<ID3D11Device>         device;  // none in audio-only mode
    ComPtr<ID3D11DeviceContext>  context;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    ComPtr<IMFMediaEngine>       engine;
    ComPtr<ID3D11Texture2D>      renderTarget;
    ComPtr<ID3D11Texture2D>      staging;
    QSize                        textureSize;

    QTimer        timer;
    QElapsedTimer positionClock;
    QElapsedTimer loadClock;
    QString       path;
    quint64       generation           = 0;
    bool          audioOnly            = false; // OpenMode::AudioOnly: no device, no frames
    bool          loaded               = false;
    bool          failed               = false;
    bool          refresh              = false; // transfer the current frame even if the stream tick did not change
    LONGLONG      lastPts              = kNoFrame;
    qint64        lastReportedPosition = -1;
    QSize         videoSize;
    QSize         requestedSize;
    QSize         requestedFor; // videoSize when requestedSize was set (the picture the owner sized it for)
    QImage        frame;
    bool          muted  = false;
    double        volume = 1.0;
    bool          loop   = false;
    State         lastState;
};

bool VideoPlayer::Private::createEngine(QString* error)
{
    if (!com.usable() || !platformStarted) {
        *error = platformUnavailableText(wording());
        return false;
    }

    // Audio files need no graphics device at all: the audio-only engine never renders frames.
    HRESULT hr = S_OK;
    if (!audioOnly) {
        hr = detail::createD3D11Device(true, &device, &context);
        if (SUCCEEDED(hr))
            hr = detail::createDxgiDeviceManager(device.Get(), &deviceManager);
        if (FAILED(hr)) {
            *error = i18n::t("Couldn't start the player (graphics error %1). Updating your graphics driver may help.")
                         .arg(detail::hexCode(hr));
            return false;
        }
    }

    ComPtr<IMFMediaEngineClassFactory> factory;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        *error = platformUnavailableText(wording());
        return false;
    }

    ComPtr<IMFMediaEngineNotify> notify;
    notify.Attach(new Notify(guard));

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(hr))
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(hr) && !audioOnly)
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager.Get());
    if (SUCCEEDED(hr) && !audioOnly)
        hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    // No MF_MEDIA_ENGINE_PLAYBACK_HWND: frame-server mode (or no video at all with AUDIOONLY).
    const DWORD flags = audioOnly ? static_cast<DWORD>(MF_MEDIA_ENGINE_AUDIOONLY) : 0;
    if (SUCCEEDED(hr))
        hr = factory->CreateInstance(flags, attributes.Get(), &engine);
    if (FAILED(hr)) {
        engine.Reset();
        *error = failureText(hr, GUID_NULL, wording(), path);
        return false;
    }

    engine->SetAutoPlay(FALSE);
    engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    engine->SetMuted(muted ? TRUE : FALSE);
    engine->SetVolume(volume);
    engine->SetLoop(loop ? TRUE : FALSE);
    return true;
}

void VideoPlayer::Private::release()
{
    timer.stop();
    if (guard) {
        std::lock_guard<std::mutex> lock(guard->mutex);
        guard->player = nullptr;
    }
    guard.reset();
    ++generation; // events already queued for the old engine are ignored

    if (engine) {
        engine->Shutdown();
        engine.Reset();
    }
    staging.Reset();
    renderTarget.Reset();
    textureSize = QSize();
    deviceManager.Reset();
    if (context)
        context->ClearState();
    context.Reset();
    device.Reset();

    path.clear();
    audioOnly            = false;
    loaded               = false;
    failed               = false;
    refresh              = false;
    lastPts              = kNoFrame;
    lastReportedPosition = -1;
    videoSize            = QSize();
    frame                = QImage();
    positionClock.invalidate();
    loadClock.invalidate();
}

void VideoPlayer::Private::failLater(const QString& error)
{
    failed = true;
    timer.stop();
    const quint64 failedGeneration = generation;
    QMetaObject::invokeMethod(
        q,
        [this, failedGeneration, error] {
            if (generation == failedGeneration)
                emit q->failed(error);
        },
        Qt::QueuedConnection);
}

QString VideoPlayer::Private::engineErrorText(DWORD_PTR code, HRESULT hr) const
{
    if (code == 0 || hr == S_OK) {
        ComPtr<IMFMediaError> error;
        if (engine && SUCCEEDED(engine->GetError(&error)) && error) {
            if (code == 0)
                code = error->GetErrorCode();
            if (hr == S_OK)
                hr = error->GetExtendedErrorCode();
        }
    }

    switch (code) {
    case MF_MEDIA_ENGINE_ERR_ABORTED:
        return i18n::t("Playback was interrupted.");
    case MF_MEDIA_ENGINE_ERR_NETWORK:
        return i18n::t("Couldn't read the file.");
    case MF_MEDIA_ENGINE_ERR_DECODE:
        return detail::damagedText();
    case MF_MEDIA_ENGINE_ERR_ENCRYPTED:
        return audioOnly ? i18n::t("This file is copy-protected and can't be played here.")
                         : i18n::t("This video is copy-protected and can't be played here.");
    case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: {
        if (detail::isMissingDecoder(hr))
            return detail::missingDecoderText(audioOnly ? GUID_NULL : detail::videoSubtypeOf(path), wording());
        const QString fileError = detail::fileErrorText(hr);
        if (!fileError.isEmpty())
            return fileError;
        // The engine often reports a plain E_FAIL here: name the Store extension where it applies.
        return failureText(MF_E_UNSUPPORTED_BYTESTREAM_TYPE, GUID_NULL, wording(), path);
    }
    default:
        return failureText(FAILED(hr) ? hr : E_FAIL, GUID_NULL, wording(), path);
    }
}

void VideoPlayer::Private::onEngineEvent(quint64 eventGeneration, DWORD event, DWORD_PTR param1, DWORD param2)
{
    if (eventGeneration != generation || !engine || failed)
        return;

    QPointer<VideoPlayer> alive(q);
    const auto            current = [&] { return alive && generation == eventGeneration && !failed; };

    // Owners size frames from videoSize() when loaded() arrives; any later change has to reach them,
    // or they keep requesting (and the picture is cropped to) the old aspect ratio.
    const bool  announced      = loaded;
    const QSize sizeBefore     = videoSize;
    bool        justLoaded     = false;
    qint64      reportPosition = -1;
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
    case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        updateVideoSize();
        if (event != MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA)
            refresh = true; // the first frame is decoded now
        if (!loaded) {
            loaded     = true;
            justLoaded = true;
            loadClock.start();
        }
        break;
    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE: // e.g. a recording whose source window was resized
        updateVideoSize();
        refresh = true;
        break;
    case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
        refresh = true;
        break;
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
    case MF_MEDIA_ENGINE_EVENT_ENDED:
        refresh        = true;
        reportPosition = q->position();
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR:
    case MF_MEDIA_ENGINE_EVENT_RESOURCELOST:
    case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR: {
        QString message;
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
            message = engineErrorText(param1, static_cast<HRESULT>(param2));
        else if (audioOnly)
            message = i18n::t("Playback stopped unexpectedly. Play it again.");
        else
            message = i18n::t("Playback stopped because the graphics device was reset. Open the file again.");
        failed = true;
        timer.stop();
        emit q->failed(message);
        if (alive && generation == eventGeneration)
            checkState();
        return;
    }
    default:
        break;
    }

    updateTimer();
    if (justLoaded)
        emit q->loaded();
    if (announced && videoSize != sizeBefore && current())
        emit q->videoSizeChanged();
    if (reportPosition >= 0 && current() && reportPosition != lastReportedPosition) {
        lastReportedPosition = reportPosition;
        emit q->positionChanged(reportPosition);
    }
    if (current())
        checkState();
}

QSize VideoPlayer::Private::nativeVideoSize() const
{
    DWORD width = 0, height = 0;
    if (!engine || audioOnly || !engine->HasVideo() || FAILED(engine->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0)
        return {};
    QSize size(static_cast<int>(qMin<DWORD>(width, kMaxImageSide)), static_cast<int>(qMin<DWORD>(height, kMaxImageSide)));

    // GetVideoAspectRatio is the picture aspect ratio (16:9 for square-pixel 1280x720), not the pixel one.
    DWORD aspectX = 0, aspectY = 0;
    if (SUCCEEDED(engine->GetVideoAspectRatio(&aspectX, &aspectY)) && aspectX != 0 && aspectY != 0) {
        const double picture = static_cast<double>(aspectX) / aspectY;
        const double stored  = static_cast<double>(size.width()) / size.height();
        if (picture > stored * 1.005)
            size.setWidth(qBound(1, qRound(size.height() * picture), kMaxImageSide));
        else if (picture < stored / 1.005)
            size.setHeight(qBound(1, qRound(size.width() / picture), kMaxImageSide));
    }
    return size;
}

// A moment in which the engine cannot report a size keeps the last known one.
void VideoPlayer::Private::updateVideoSize()
{
    const QSize size = nativeVideoSize();
    if (!size.isEmpty())
        videoSize = size;
}

QSize VideoPlayer::Private::outputSize() const
{
    QSize size = requestedSize.isEmpty() ? videoSize : requestedSize;
    if (!requestedSize.isEmpty() && !videoSize.isEmpty() && videoSize != requestedFor) {
        // The request was sized for another picture (the stream changed size since). Until the owner asks
        // again, fit the picture into the requested box instead of cropping it to the old aspect ratio.
        // A change that keeps the aspect ratio keeps the request: the sides then differ by rounding only.
        const QSize fitted = videoSize.scaled(requestedSize, Qt::KeepAspectRatio);
        const QSize even(qMax(2, fitted.width() & ~1), qMax(2, fitted.height() & ~1)); // 4:2:0 friendly, like the owners
        if (qAbs(even.width() - requestedSize.width()) > 2 || qAbs(even.height() - requestedSize.height()) > 2)
            size = even;
    }
    return size.boundedTo(QSize(kMaxTextureSide, kMaxTextureSide));
}

// Part of the video that fills target without distortion (centre crop when the aspect ratios differ).
MFVideoNormalizedRect VideoPlayer::Private::sourceRect(const QSize& target) const
{
    MFVideoNormalizedRect rect = {0.f, 0.f, 1.f, 1.f};
    if (videoSize.isEmpty() || target.isEmpty())
        return rect;
    const double videoAspect  = static_cast<double>(videoSize.width()) / videoSize.height();
    const double targetAspect = static_cast<double>(target.width()) / target.height();
    if (videoAspect > targetAspect) {
        const float visible = static_cast<float>(targetAspect / videoAspect);
        rect.left           = (1.f - visible) / 2.f;
        rect.right          = rect.left + visible;
    } else if (videoAspect < targetAspect) {
        const float visible = static_cast<float>(videoAspect / targetAspect);
        rect.top            = (1.f - visible) / 2.f;
        rect.bottom         = rect.top + visible;
    }
    return rect;
}

bool VideoPlayer::Private::ensureTextures(const QSize& size)
{
    if (!device)
        return false;
    if (renderTarget && staging && textureSize == size)
        return true;
    renderTarget.Reset();
    staging.Reset();
    textureSize = QSize();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width                = static_cast<UINT>(size.width());
    desc.Height               = static_cast<UINT>(size.height());
    desc.MipLevels            = 1;
    desc.ArraySize            = 1;
    desc.Format               = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count     = 1;
    desc.Usage                = D3D11_USAGE_DEFAULT;
    desc.BindFlags            = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &renderTarget)))
        return false;

    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        renderTarget.Reset();
        return false;
    }
    textureSize = size;
    return true;
}

bool VideoPlayer::Private::transferFrame()
{
    const QSize size = outputSize();
    if (size.isEmpty() || !ensureTextures(size))
        return false;

    const MFVideoNormalizedRect source = sourceRect(size);
    const RECT                  target = {0, 0, size.width(), size.height()};
    const MFARGB                border = {0, 0, 0, 255};
    if (FAILED(engine->TransferVideoFrame(renderTarget.Get(), &source, &target, &border)))
        return false;

    context->CopyResource(staging.Get(), renderTarget.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (!image.isNull())
        copyRows(image, static_cast<const BYTE*>(mapped.pData), static_cast<LONG>(mapped.RowPitch));
    context->Unmap(staging.Get(), 0);
    if (image.isNull())
        return false;
    frame = image;
    return true;
}

// While paused right after loading, keep polling until the engine reports its first stream tick.
bool VideoPlayer::Private::waitingForFirstFrame() const
{
    return !audioOnly && loaded && lastPts == kNoFrame && loadClock.isValid() && loadClock.elapsed() < kFirstFramePollMs && engine->HasVideo();
}

void VideoPlayer::Private::tick()
{
    if (!engine || failed) {
        timer.stop();
        return;
    }

    bool newFrame = false;
    if (!audioOnly && engine->HasVideo()) {
        // S_FALSE (no new frame) reports the frame already presented. Before the first frame the engine
        // reports LLONG_MIN, and a transfer would succeed with a black surface.
        LONGLONG      pts    = lastPts;
        const HRESULT hr     = engine->OnVideoStreamTick(&pts);
        const bool hasFrame = SUCCEEDED(hr) && pts != kNoFrame;
        if (hasFrame && (pts != lastPts || refresh)) {
            if (transferFrame()) {
                newFrame = true;
                refresh  = false;
                lastPts  = pts;
            }
        }
    } else if (loaded) {
        refresh = false; // audio only
    }

    qint64 reportPosition = -1;
    if (q->isPlaying() && (!positionClock.isValid() || positionClock.elapsed() >= kPositionIntervalMs)) {
        positionClock.start();
        reportPosition = q->position();
    }
    updateTimer();

    QPointer<VideoPlayer> alive(q);
    const quint64         tickGeneration = generation;
    const auto            current        = [&] { return alive && generation == tickGeneration && !failed; };
    if (newFrame)
        emit q->frameReady();
    if (reportPosition >= 0 && current() && reportPosition != lastReportedPosition) {
        lastReportedPosition = reportPosition;
        emit q->positionChanged(reportPosition);
    }
    if (current())
        checkState();
}

void VideoPlayer::Private::updateTimer()
{
    const bool needed = engine && !failed && (q->isPlaying() || refresh || engine->IsSeeking() || waitingForFirstFrame());
    if (needed && !timer.isActive())
        timer.start();
    else if (!needed && timer.isActive())
        timer.stop();
}

void VideoPlayer::Private::checkState()
{
    State now;
    now.playing = q->isPlaying();
    now.ended   = q->isEnded();
    now.muted   = muted;
    if (!(now != lastState))
        return;
    lastState = now;
    emit q->stateChanged();
}

VideoPlayer::VideoPlayer(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
    d->timer.setTimerType(Qt::PreciseTimer);
    d->timer.setInterval(kFrameIntervalMs);
    connect(&d->timer, &QTimer::timeout, this, [this] { d->tick(); });
}

VideoPlayer::~VideoPlayer()
{
    d->release();
}

void VideoPlayer::open(const QString& path, OpenMode mode)
{
    close();
    d->path              = path;
    d->audioOnly         = mode == OpenMode::AudioOnly;
    d->guard             = std::make_shared<Private::Guard>();
    d->guard->player     = this;
    d->guard->generation = d->generation;
    // Without frames the timer only reports the position: no need to wake up every 10 ms.
    d->timer.setTimerType(d->audioOnly ? Qt::CoarseTimer : Qt::PreciseTimer);
    d->timer.setInterval(d->audioOnly ? kAudioIntervalMs : kFrameIntervalMs);

    QString error;
    if (!d->createEngine(&error)) {
        d->failLater(error);
        return;
    }

    // SetSource expects a URL; the engine starts loading asynchronously (LOADEDMETADATA or ERROR follows).
    const QString url    = QUrl::fromLocalFile(QDir::fromNativeSeparators(path)).toString();
    BSTR          source = SysAllocString(reinterpret_cast<const OLECHAR*>(url.utf16()));
    const HRESULT hr     = source ? d->engine->SetSource(source) : E_OUTOFMEMORY;
    SysFreeString(source);
    if (FAILED(hr)) {
        d->failLater(failureText(hr, GUID_NULL, d->wording(), path));
        return;
    }
    d->updateTimer();
}

void VideoPlayer::close()
{
    d->release();
    d->checkState();
}

bool VideoPlayer::isLoaded() const
{
    return d->loaded && !d->failed;
}

bool VideoPlayer::isAudioOnly() const
{
    return d->audioOnly;
}

bool VideoPlayer::hasGraphicsDevice() const
{
    return d->device != nullptr;
}

void VideoPlayer::play()
{
    if (!d->engine || d->failed)
        return;
    if (d->engine->IsEnded())
        d->engine->SetCurrentTime(0.0);
    d->engine->Play();
    d->positionClock.invalidate();
    d->updateTimer();
    d->checkState();
}

void VideoPlayer::pause()
{
    if (!d->engine || d->failed)
        return;
    d->engine->Pause();
    d->refresh = true; // show exactly the frame playback stopped on
    d->updateTimer();

    QPointer<VideoPlayer> alive(this);
    const qint64          pos = position();
    if (pos != d->lastReportedPosition) {
        d->lastReportedPosition = pos;
        emit positionChanged(pos);
    }
    if (alive)
        d->checkState();
}

void VideoPlayer::togglePlay()
{
    if (isPlaying())
        pause();
    else
        play();
}

bool VideoPlayer::isPlaying() const
{
    return d->engine && !d->failed && !d->engine->IsPaused() && !d->engine->IsEnded();
}

bool VideoPlayer::isEnded() const
{
    return d->engine && !d->failed && d->engine->IsEnded();
}

void VideoPlayer::seek(qint64 ms)
{
    if (!d->engine || d->failed)
        return;
    const qint64 total  = duration();
    const qint64 target = qBound<qint64>(0, ms, total > 0 ? total : std::numeric_limits<qint64>::max());
    // At the end the engine is not paused, so a seek would quietly resume playback (past the owner's
    // one-at-a-time rule). Seeking never starts playback: an ended file waits at the new position.
    const bool wasEnded = d->engine->IsEnded();
    d->engine->SetCurrentTime(static_cast<double>(target) / 1000.0);
    if (wasEnded)
        d->engine->Pause();
    d->refresh = true;
    d->positionClock.start();
    d->updateTimer();

    QPointer<VideoPlayer> alive(this);
    d->lastReportedPosition = target;
    emit positionChanged(target);
    if (alive)
        d->checkState();
}

qint64 VideoPlayer::position() const
{
    if (!d->engine)
        return 0;
    const double seconds = d->engine->GetCurrentTime();
    if (!std::isfinite(seconds) || seconds <= 0)
        return 0;
    const qint64 ms    = qRound64(seconds * 1000.0);
    const qint64 total = duration();
    return total > 0 ? qMin(ms, total) : ms;
}

qint64 VideoPlayer::duration() const
{
    if (!d->engine)
        return 0;
    const double seconds = d->engine->GetDuration(); // NaN while unknown, +inf for live sources
    if (!std::isfinite(seconds) || seconds <= 0)
        return 0;
    return qRound64(seconds * 1000.0);
}

QSize VideoPlayer::videoSize() const
{
    return d->videoSize;
}

void VideoPlayer::setMuted(bool muted)
{
    d->muted = muted;
    if (d->engine)
        d->engine->SetMuted(muted ? TRUE : FALSE);
    d->checkState();
}

bool VideoPlayer::isMuted() const
{
    return d->muted;
}

void VideoPlayer::setVolume(double volume)
{
    if (std::isnan(volume))
        return; // keep the current volume rather than guessing (never jump to full volume)
    d->volume = qBound(0.0, volume, 1.0);
    if (d->engine)
        d->engine->SetVolume(d->volume);
}

double VideoPlayer::volume() const
{
    return d->volume;
}

void VideoPlayer::setLoop(bool loop)
{
    d->loop = loop;
    if (d->engine)
        d->engine->SetLoop(loop ? TRUE : FALSE);
}

bool VideoPlayer::isPlaybackRateSupported(double rate) const
{
    if (!d->engine || d->failed || !d->loaded || !std::isfinite(rate) || rate <= 0.0 || rate > 16.0)
        return false;
    ComPtr<IMFMediaEngineEx> ex;
    if (FAILED(d->engine.As(&ex)))
        return qFuzzyCompare(rate, 1.0);
    return ex->IsPlaybackRateSupported(rate) != FALSE;
}

bool VideoPlayer::setPlaybackRate(double rate)
{
    if (!isPlaybackRateSupported(rate))
        return false;
    // The default rate is what playback returns to after a pause or seek; the current one applies now.
    return SUCCEEDED(d->engine->SetDefaultPlaybackRate(rate)) && SUCCEEDED(d->engine->SetPlaybackRate(rate));
}

double VideoPlayer::playbackRate() const
{
    if (!d->engine || d->failed)
        return 1.0;
    const double rate = d->engine->GetPlaybackRate();
    return std::isfinite(rate) && rate > 0.0 ? rate : 1.0;
}

void VideoPlayer::setFrameSize(const QSize& pixels)
{
    const QSize size   = pixels.isEmpty() ? QSize() : pixels.boundedTo(QSize(kMaxTextureSide, kMaxTextureSide));
    const QSize before = d->outputSize();
    d->requestedSize   = size;
    d->requestedFor    = d->videoSize; // also when the size is the same: the owner has seen the current picture
    if (d->outputSize() == before || d->audioOnly)
        return;
    d->refresh = true;
    d->updateTimer();
}

QImage VideoPlayer::currentFrame() const
{
    return d->frame;
}

} // namespace mf
