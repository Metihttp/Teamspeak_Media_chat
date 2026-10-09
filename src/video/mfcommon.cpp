#include "mfcommon.h"

#include <QDir>
#include <QFileInfo>

#include <d3d10_1.h> // ID3D10Multithread; MF headers want d3d10_1.h rather than d3d10.h
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <iterator>
#include <string>

#include "i18n.h"

using Microsoft::WRL::ComPtr;

namespace mf::detail {

namespace {

constexpr int   kMaxImageSide     = 16384;
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

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

// Ogg and Opus come only with the Store's Web Media Extensions (no inbox byte-stream handler).
bool needsWebMediaExtensions(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("ogg") || ext == QLatin1String("oga") || ext == QLatin1String("opus");
}

} // namespace

// ---- platform -----------------------------------------------------------------------------------

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

ComScope::ComScope(DWORD model)
    : m_hr(CoInitializeEx(nullptr, model | COINIT_DISABLE_OLE1DDE))
{
}

ComScope::~ComScope()
{
    if (SUCCEEDED(m_hr))
        CoUninitialize();
}

PlatformScope::PlatformScope()
    : m_hr(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))
{
}

PlatformScope::~PlatformScope()
{
    if (SUCCEEDED(m_hr))
        MFShutdown();
}

// ---- error texts --------------------------------------------------------------------------------

QString hexCode(HRESULT hr)
{
    return QString::fromLatin1("0x") + QString::number(static_cast<quint32>(hr), 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

QString platformUnavailableText(Wording wording)
{
    if (wording == Wording::Audio)
        return i18n::t("Windows media support (Media Foundation) is not available. On Windows N editions, install the Media Feature Pack.");
    return i18n::t("Windows video support (Media Foundation) is not available. On Windows N editions, install the Media Feature Pack.");
}

QString unsupportedFormatText()
{
    return i18n::t("This file format can't be played on this computer.");
}

QString damagedText()
{
    return i18n::t("Couldn't decode this file. It may be damaged.");
}

QString missingDecoderText(const GUID& videoSubtype, Wording wording)
{
    if (wording == Wording::Audio)
        return i18n::t("No decoder for this audio format is installed on this computer.");
    const char*   storeApp = nullptr;
    const QString codec    = codecName(videoSubtype, &storeApp);
    if (codec.isEmpty())
        return unsupportedFormatText();
    if (storeApp) {
        return i18n::t("No %1 video decoder is installed on this computer. Install “%2” from the Microsoft Store.")
            .arg(codec, QString::fromLatin1(storeApp));
    }
    return i18n::t("No %1 video decoder is installed on this computer.").arg(codec);
}

bool isMissingDecoder(HRESULT hr)
{
    return hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TRANSFORM_TYPE_NOT_SET
        || hr == MF_E_NO_MORE_TYPES || hr == MF_E_TOPO_UNSUPPORTED || hr == MF_E_UNSUPPORTED_D3D_TYPE;
}

QString fileErrorText(HRESULT hr)
{
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        || hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME))
        return i18n::t("The file was not found.");
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
        return i18n::t("The file is in use by another program, or access was denied.");
    return {};
}

QString failureText(HRESULT hr, const GUID& videoSubtype, Wording wording, const QString& path)
{
    const QString fileError = fileErrorText(hr);
    if (!fileError.isEmpty())
        return fileError;
    if (hr == MF_E_PLATFORM_NOT_INITIALIZED || hr == REGDB_E_CLASSNOTREG || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND))
        return platformUnavailableText(wording);
    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_UNSUPPORTED_SCHEME || hr == MF_E_UNSUPPORTED_FORMAT
        || hr == MF_E_INVALID_FILE_FORMAT || hr == MF_E_BYTESTREAM_NOT_SEEKABLE) {
        if (wording == Wording::Audio && hr != MF_E_BYTESTREAM_NOT_SEEKABLE && needsWebMediaExtensions(path))
            return i18n::t("Ogg and Opus audio need an extension on this computer. Install “Web Media Extensions” from the Microsoft Store.");
        return unsupportedFormatText();
    }
    if (isMissingDecoder(hr))
        return missingDecoderText(videoSubtype, wording);
    if (hr == MF_E_INVALID_STREAM_DATA || hr == MF_E_INVALIDREQUEST || hr == E_UNEXPECTED)
        return damagedText();
    return i18n::t("Couldn't play this file (error %1).").arg(hexCode(hr));
}

// ---- source reader / media types ----------------------------------------------------------------

HRESULT createSourceReader(const QString& path, IMFAttributes* attributes, IMFSourceReader** reader)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return MFCreateSourceReaderFromURL(native.c_str(), attributes, reader);
}

GUID videoSubtypeOf(const QString& path)
{
    GUID                    subtype = GUID_NULL;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType>    type;
    if (SUCCEEDED(createSourceReader(path, nullptr, &reader)) && SUCCEEDED(reader->GetNativeMediaType(kFirstVideoStream, 0, &type)))
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

// ---- D3D11 / DXGI -------------------------------------------------------------------------------

HRESULT createD3D11Device(bool allowWarp, ID3D11Device** device, ID3D11DeviceContext** context)
{
    if (!device || !context)
        return E_POINTER;
    *device  = nullptr;
    *context = nullptr;

    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    const UINT                     flags    = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const UINT                     count    = static_cast<UINT>(std::size(levels));

    ComPtr<ID3D11Device>        created;
    ComPtr<ID3D11DeviceContext> immediate;
    HRESULT                     hr = E_FAIL;
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (type == D3D_DRIVER_TYPE_WARP && !allowWarp)
            break;
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, count, D3D11_SDK_VERSION, &created, nullptr, &immediate);
        if (hr == E_INVALIDARG) // runtimes without 11.1 reject the whole list
            hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, count - 1, D3D11_SDK_VERSION, &created, nullptr, &immediate);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return hr;

    // Media Foundation decodes on its own threads while the owner uses the device on another.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(created.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    *device  = created.Detach();
    *context = immediate.Detach();
    return S_OK;
}

HRESULT createDxgiDeviceManager(ID3D11Device* device, IMFDXGIDeviceManager** manager)
{
    if (!device || !manager)
        return E_POINTER;
    *manager = nullptr;
    UINT                         resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> created;
    HRESULT                      hr = MFCreateDXGIDeviceManager(&resetToken, &created);
    if (SUCCEEDED(hr))
        hr = created->ResetDevice(device, resetToken);
    if (SUCCEEDED(hr))
        *manager = created.Detach();
    return hr;
}

// ---- sink writer --------------------------------------------------------------------------------

HRESULT createMpeg4SinkWriter(const QString& path, IMFDXGIDeviceManager* manager, bool hardwareTransforms, IMFSinkWriter** writer)
{
    if (!writer)
        return E_POINTER;
    *writer = nullptr;

    ComPtr<IMFAttributes> attributes;
    HRESULT               hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr))
        hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr))
        hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED(hr))
        hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardwareTransforms ? TRUE : FALSE);
    if (SUCCEEDED(hr) && manager)
        hr = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager);
    if (FAILED(hr))
        return hr;

    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    return MFCreateSinkWriterFromURL(native.c_str(), nullptr, attributes.Get(), writer);
}

} // namespace mf::detail
