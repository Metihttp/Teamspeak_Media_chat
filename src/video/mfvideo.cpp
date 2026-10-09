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
#include <iterator>
#include <limits>
#include <mutex>
#include <string>

#include "i18n.h"

using Microsoft::WRL::ComPtr;

namespace mf {

namespace {

constexpr LONGLONG kHnsPerMs              = 10000;
constexpr int      kMaxImageSide          = 16384;
constexpr int      kMaxTextureSide        = 8192;
constexpr int      kFrameIntervalMs       = 10;
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

// The plugin delay-loads mfplat.dll / mfreadwrite.dll so it still loads on Windows N editions without
// the Media Feature Pack. Nothing may call into Media Foundation before this returned true.
bool mediaFoundationPresent()
{
    static const bool present = [] {
        for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll"}) {
            const HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                return false;
            FreeLibrary(module);
        }
        return true;
    }();
    return present;
}

// CoInitializeEx for the current thread, balanced on destruction. RPC_E_CHANGED_MODE means the thread
// already lives in the other apartment type, which Media Foundation is fine with.
class ComScope
{
  public:
    explicit ComScope(DWORD model)
        : m_hr(CoInitializeEx(nullptr, model | COINIT_DISABLE_OLE1DDE))
    {
    }
    ~ComScope()
    {
        if (SUCCEEDED(m_hr))
            CoUninitialize();
    }

    bool    usable() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return m_hr; }

  private:
    Q_DISABLE_COPY(ComScope)
    HRESULT m_hr;
};

// MFStartup/MFShutdown for the duration of a probe (Media Foundation counts these itself).
class PlatformScope
{
  public:
    PlatformScope()
        : m_hr(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))
    {
    }
    ~PlatformScope()
    {
        if (SUCCEEDED(m_hr))
            MFShutdown();
    }

    HRESULT result() const { return m_hr; }

  private:
    Q_DISABLE_COPY(PlatformScope)
    HRESULT m_hr;
};

// ---- error texts --------------------------------------------------------------------------------

struct CodecInfo {
    const GUID* subtype;
    const char* name;
    const char* storeApp; // Microsoft Store extension that provides the decoder
};

const CodecInfo kCodecs[] = {
    {&MFVideoFormat_H264, "H.264", nullptr},
    {&MFVideoFormat_HEVC, "HEVC (H.265)", "HEVC Video Extensions"},
    {&MFVideoFormat_VP90, "VP9", "VP9 Video Extensions"},
    {&MFVideoFormat_VP80, "VP8", nullptr},
    {&MFVideoFormat_AV1, "AV1", "AV1 Video Extension"},
    {&MFVideoFormat_MPEG2, "MPEG-2", "MPEG-2 Video Extension"},
    {&MFVideoFormat_MP4V, "MPEG-4 Part 2", nullptr},
    {&MFVideoFormat_WMV3, "WMV 9", nullptr},
    {&MFVideoFormat_WVC1, "VC-1", nullptr},
    {&MFVideoFormat_MJPG, "Motion JPEG", nullptr},
};

// Codecs Windows has no decoder for, which media sources still expose with their FourCC.
struct ForeignCodec {
    const char* fourCC;
    const char* name;
};

const ForeignCodec kForeignCodecs[] = {
    {"apch", "Apple ProRes"}, {"apcn", "Apple ProRes"}, {"apcs", "Apple ProRes"}, {"apco", "Apple ProRes"},
    {"ap4h", "Apple ProRes"}, {"ap4x", "Apple ProRes"}, {"avdn", "Avid DNxHD"},   {"avdh", "Avid DNxHR"},
    {"ffv1", "FFV1"},         {"theo", "Theora"},       {"cvid", "Cinepak"},      {"cfhd", "GoPro CineForm"},
};

QString hexCode(HRESULT hr)
{
    return QStringLiteral("0x") + QString::number(static_cast<quint32>(hr), 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

// Readable codec name of a video subtype, empty if unknown.
QString codecName(const GUID& subtype, const char** storeApp)
{
    *storeApp = nullptr;
    for (const CodecInfo& codec : kCodecs) {
        if (*codec.subtype == subtype) {
            *storeApp = codec.storeApp;
            return QString::fromLatin1(codec.name);
        }
    }
    if (subtype == GUID_NULL)
        return {};
    // Media sources are not consistent about the byte order of FourCCs they do not know: try both.
    QByteArray lowFirst, highFirst;
    for (int i = 0; i < 4; ++i) {
        lowFirst.append(static_cast<char>((subtype.Data1 >> (8 * i)) & 0xFF));
        highFirst.append(static_cast<char>((subtype.Data1 >> (8 * (3 - i))) & 0xFF));
    }
    for (const ForeignCodec& codec : kForeignCodecs) {
        if (qstricmp(lowFirst.constData(), codec.fourCC) == 0 || qstricmp(highFirst.constData(), codec.fourCC) == 0)
            return QString::fromLatin1(codec.name);
    }
    return {};
}

QString platformUnavailableText()
{
    return i18n::t("Windows video support (Media Foundation) is not available. On Windows N editions, install the Media Feature Pack.",
                   "پشتیبانی ویدیوی ویندوز (Media Foundation) در دسترس نیست. در نسخه‌های N ویندوز، Media Feature Pack را نصب کنید.");
}

QString unsupportedFormatText()
{
    return i18n::t("This file format can't be played on this computer.", "این قالب فایل روی این رایانه قابل پخش نیست.");
}

QString damagedText()
{
    return i18n::t("The video could not be decoded. The file may be damaged.", "ویدیو رمزگشایی نشد. ممکن است فایل خراب باشد.");
}

QString missingDecoderText(const GUID& subtype)
{
    const char*   storeApp = nullptr;
    const QString codec    = codecName(subtype, &storeApp);
    if (codec.isEmpty())
        return unsupportedFormatText();
    if (storeApp) {
        return i18n::t("No %1 video decoder is installed on this computer. Install “%2” from the Microsoft Store.",
                       "رمزگشای ویدیوی %1 روی این رایانه نصب نیست. «%2» را از Microsoft Store نصب کنید.")
            .arg(codec, QString::fromLatin1(storeApp));
    }
    return i18n::t("No %1 video decoder is installed on this computer.", "رمزگشای ویدیوی %1 روی این رایانه نصب نیست.").arg(codec);
}

bool isMissingDecoder(HRESULT hr)
{
    return hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TRANSFORM_TYPE_NOT_SET
        || hr == MF_E_NO_MORE_TYPES || hr == MF_E_TOPO_UNSUPPORTED || hr == MF_E_UNSUPPORTED_D3D_TYPE;
}

// Empty if hr is not about reaching the file.
QString fileErrorText(HRESULT hr)
{
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        || hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME))
        return i18n::t("The video file was not found.", "فایل ویدیو پیدا نشد.");
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
        return i18n::t("The video file is in use or access to it was denied.", "فایل ویدیو در حال استفاده است یا اجازهٔ دسترسی به آن داده نشد.");
    return {};
}

// videoSubtype (if known) names the codec when a decoder is missing.
QString failureText(HRESULT hr, const GUID& videoSubtype = GUID_NULL)
{
    const QString fileError = fileErrorText(hr);
    if (!fileError.isEmpty())
        return fileError;
    if (hr == MF_E_PLATFORM_NOT_INITIALIZED || hr == REGDB_E_CLASSNOTREG || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND))
        return platformUnavailableText();
    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_UNSUPPORTED_SCHEME || hr == MF_E_UNSUPPORTED_FORMAT
        || hr == MF_E_INVALID_FILE_FORMAT || hr == MF_E_BYTESTREAM_NOT_SEEKABLE)
        return unsupportedFormatText();
    if (isMissingDecoder(hr))
        return missingDecoderText(videoSubtype);
    if (hr == MF_E_INVALID_STREAM_DATA || hr == MF_E_INVALIDREQUEST || hr == E_UNEXPECTED)
        return damagedText();
    return i18n::t("The video could not be played (error %1).", "ویدیو پخش نشد (خطای %1).").arg(hexCode(hr));
}

// ---- source reader helpers ----------------------------------------------------------------------

HRESULT createReader(const QString& path, IMFAttributes* attributes, IMFSourceReader** reader)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return MFCreateSourceReaderFromURL(native.c_str(), attributes, reader);
}

GUID videoSubtypeOf(const QString& path)
{
    GUID                    subtype = GUID_NULL;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType>    type;
    if (SUCCEEDED(createReader(path, nullptr, &reader)) && SUCCEEDED(reader->GetNativeMediaType(kFirstVideoStream, 0, &type)))
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
    return subtype;
}

QSize applyPixelAspect(const QSize& size, UINT32 num, UINT32 den)
{
    if (num == 0 || den == 0 || num == den || size.isEmpty())
        return size;
    if (num > den)
        return QSize(qBound(1, qRound(size.width() * static_cast<double>(num) / den), kMaxImageSide), size.height());
    return QSize(size.width(), qBound(1, qRound(size.height() * static_cast<double>(den) / num), kMaxImageSide));
}

struct Geometry {
    QSize frame;        // coded frame size
    QRect aperture;     // visible part of the frame
    QSize display;      // aperture with pixel aspect ratio and rotation applied
    int   rotation = 0; // clockwise degrees that turn the decoded picture upright
};

Geometry geometryOf(IMFMediaType* type)
{
    Geometry g;
    UINT32   width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) || width == 0 || height == 0
        || width > static_cast<UINT32>(kMaxImageSide) || height > static_cast<UINT32>(kMaxImageSide))
        return g;
    g.frame    = QSize(static_cast<int>(width), static_cast<int>(height));
    g.aperture = QRect(QPoint(), g.frame);

    MFVideoArea area     = {};
    UINT32      areaSize = 0;
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &areaSize))
        || SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &areaSize))) {
        const QRect visible = QRect(area.OffsetX.value, area.OffsetY.value, area.Area.cx, area.Area.cy) & g.aperture;
        if (!visible.isEmpty())
            g.aperture = visible;
    }

    UINT32 num = 1, den = 1;
    MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, &num, &den);
    g.display = applyPixelAspect(g.aperture.size(), num, den);

    // MF_MT_VIDEO_ROTATION is how far the stored picture is rotated counter-clockwise.
    const UINT32 rotation = MFGetAttributeUINT32(type, MF_MT_VIDEO_ROTATION, 0);
    if (rotation == 90 || rotation == 180 || rotation == 270)
        g.rotation = static_cast<int>(rotation);
    if (g.rotation == 90 || g.rotation == 270)
        g.display.transpose();
    return g;
}

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
        hr = createReader(path, attributes.Get(), &reader);
    if (FAILED(hr)) {
        result.error = failureText(hr);
        return result;
    }

    ComPtr<IMFMediaType> nativeVideo;
    ComPtr<IMFMediaType> nativeAudio;
    result.hasVideo = SUCCEEDED(reader->GetNativeMediaType(kFirstVideoStream, 0, &nativeVideo));
    result.hasAudio = SUCCEEDED(reader->GetNativeMediaType(kFirstAudioStream, 0, &nativeAudio));
    if (!result.hasVideo && !result.hasAudio) {
        result.error = unsupportedFormatText(); // media sources hide tracks they cannot handle
        return result;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
        result.durationMs = static_cast<qint64>(duration.uhVal.QuadPart / kHnsPerMs);
    PropVariantClear(&duration);

    result.ok = true;
    if (!result.hasVideo)
        return result;

    const Geometry native = geometryOf(nativeVideo.Get());
    result.size           = native.display;
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
    HRESULT createDevice();
    void    release();
    void    failLater(const QString& error);
    void    onEngineEvent(quint64 eventGeneration, DWORD event, DWORD_PTR param1, DWORD param2);
    QString engineErrorText(DWORD_PTR code, HRESULT hr) const;
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
    ComPtr<ID3D11Device>         device;
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

HRESULT VideoPlayer::Private::createDevice()
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    const UINT                     flags    = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const UINT                     count    = static_cast<UINT>(std::size(levels));

    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, count, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (hr == E_INVALIDARG) // runtimes without 11.1 reject the whole list
            hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, count - 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return hr;

    // The engine decodes on its own threads while frames are read back on the GUI thread.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    UINT resetToken = 0;
    hr              = MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
    if (SUCCEEDED(hr))
        hr = deviceManager->ResetDevice(device.Get(), resetToken);
    return hr;
}

bool VideoPlayer::Private::createEngine(QString* error)
{
    if (!com.usable() || !platformStarted) {
        *error = platformUnavailableText();
        return false;
    }

    HRESULT hr = createDevice();
    if (FAILED(hr)) {
        *error = i18n::t("The video player could not be started (graphics error %1).", "پخش‌کنندهٔ ویدیو راه‌اندازی نشد (خطای گرافیکی %1).")
                     .arg(hexCode(hr));
        return false;
    }

    ComPtr<IMFMediaEngineClassFactory> factory;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        *error = platformUnavailableText();
        return false;
    }

    ComPtr<IMFMediaEngineNotify> notify;
    notify.Attach(new Notify(guard));

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(hr))
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager.Get());
    if (SUCCEEDED(hr))
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    if (SUCCEEDED(hr))
        hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) // no MF_MEDIA_ENGINE_PLAYBACK_HWND: frame-server mode
        hr = factory->CreateInstance(0, attributes.Get(), &engine);
    if (FAILED(hr)) {
        engine.Reset();
        *error = failureText(hr);
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
        return i18n::t("Playback was interrupted.", "پخش متوقف شد.");
    case MF_MEDIA_ENGINE_ERR_NETWORK:
        return i18n::t("The video file could not be read.", "فایل ویدیو خوانده نشد.");
    case MF_MEDIA_ENGINE_ERR_DECODE:
        return damagedText();
    case MF_MEDIA_ENGINE_ERR_ENCRYPTED:
        return i18n::t("This video is copy-protected and can't be played here.", "این ویدیو محافظت‌شده است و اینجا پخش نمی‌شود.");
    case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED: {
        if (isMissingDecoder(hr))
            return missingDecoderText(videoSubtypeOf(path));
        const QString fileError = fileErrorText(hr);
        return fileError.isEmpty() ? unsupportedFormatText() : fileError;
    }
    default:
        return failureText(FAILED(hr) ? hr : E_FAIL);
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
        const QString message = event == MF_MEDIA_ENGINE_EVENT_ERROR
                                    ? engineErrorText(param1, static_cast<HRESULT>(param2))
                                    : i18n::t("Video playback stopped because the graphics device was reset. Open the video again.",
                                              "پخش ویدیو متوقف شد چون دستگاه گرافیکی بازنشانی شد. ویدیو را دوباره باز کنید.");
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
    if (!engine || !engine->HasVideo() || FAILED(engine->GetNativeVideoSize(&width, &height)) || width == 0 || height == 0)
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
    return loaded && lastPts == kNoFrame && loadClock.isValid() && loadClock.elapsed() < kFirstFramePollMs && engine->HasVideo();
}

void VideoPlayer::Private::tick()
{
    if (!engine || failed) {
        timer.stop();
        return;
    }

    bool newFrame = false;
    if (engine->HasVideo()) {
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

void VideoPlayer::open(const QString& path)
{
    close();
    d->path              = path;
    d->guard             = std::make_shared<Private::Guard>();
    d->guard->player     = this;
    d->guard->generation = d->generation;

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
        d->failLater(failureText(hr));
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
    d->engine->SetCurrentTime(static_cast<double>(target) / 1000.0);
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

void VideoPlayer::setFrameSize(const QSize& pixels)
{
    const QSize size   = pixels.isEmpty() ? QSize() : pixels.boundedTo(QSize(kMaxTextureSide, kMaxTextureSide));
    const QSize before = d->outputSize();
    d->requestedSize   = size;
    d->requestedFor    = d->videoSize; // also when the size is the same: the owner has seen the current picture
    if (d->outputSize() == before)
        return;
    d->refresh = true;
    d->updateTimer();
}

QImage VideoPlayer::currentFrame() const
{
    return d->frame;
}

} // namespace mf
