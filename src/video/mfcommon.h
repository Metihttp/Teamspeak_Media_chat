#pragma once

// Internal helpers shared by the Media Foundation modules of the plugin: the player and the probe
// (mfvideo.cpp) now, the transcoder and the voice-message AAC writer later. Not part of the public
// video API: include this only from src/video/*.cpp (and developer tools).
//
// Nothing in here may run before mediaFoundationPresent() returned true: the plugin delay-loads
// mfplat.dll / mfreadwrite.dll so it still loads on Windows N editions without the Media Feature Pack.
// Unless noted, the functions are safe on any thread that has COM initialized (see ComScope).

#include <QRect>
#include <QSize>
#include <QString>

#include <windows.h>

#include <cguid.h> // GUID_NULL
#include <guiddef.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IMFAttributes;
struct IMFDXGIDeviceManager;
struct IMFMediaType;
struct IMFSinkWriter;
struct IMFSourceReader;

namespace mf::detail {

// mfplat.dll and mfreadwrite.dll can be loaded (checked once, then cached).
bool mediaFoundationPresent();

// CoInitializeEx for the current thread, balanced on destruction. RPC_E_CHANGED_MODE means the thread
// already lives in the other apartment type, which Media Foundation is fine with.
class ComScope
{
  public:
    explicit ComScope(DWORD model);
    ~ComScope();
    ComScope(const ComScope&)            = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool    usable() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return m_hr; }

  private:
    HRESULT m_hr;
};

// MFStartup/MFShutdown for the lifetime of the scope (Media Foundation counts these itself). For work
// on a worker thread (probe, transcode, encode); players use the reference-counted mf::startup().
class PlatformScope
{
  public:
    PlatformScope();
    ~PlatformScope();
    PlatformScope(const PlatformScope&)            = delete;
    PlatformScope& operator=(const PlatformScope&) = delete;

    HRESULT result() const { return m_hr; }

  private:
    HRESULT m_hr;
};

// ---- error texts (all i18n::t, safe to hand to Qt state that outlives the DLL) --------------------

// What the text talks about: a video ("No H.264 video decoder…") or an audio file.
enum class Wording { Video, Audio };

QString hexCode(HRESULT hr); // "0x80070002"
QString platformUnavailableText(Wording wording = Wording::Video);
QString unsupportedFormatText();
QString damagedText();
// videoSubtype names the codec when it is known (GUID_NULL otherwise).
QString missingDecoderText(const GUID& videoSubtype, Wording wording = Wording::Video);
bool    isMissingDecoder(HRESULT hr);
QString fileErrorText(HRESULT hr); // empty if hr is not about reaching the file
// The best explanation for a failed Media Foundation call. path (optional) lets audio texts name the
// Store extension that provides Ogg / Opus support when the format is not supported at all.
QString failureText(HRESULT hr, const GUID& videoSubtype = GUID_NULL, Wording wording = Wording::Video, const QString& path = QString());

// ---- source reader / media types ------------------------------------------------------------------

HRESULT createSourceReader(const QString& path, IMFAttributes* attributes, IMFSourceReader** reader);
GUID    videoSubtypeOf(const QString& path); // GUID_NULL if the file has no readable video stream
// 2.4 compress: "H.264", "HEVC (H.265)", "VP9", ... (the names of the decoder texts); empty if unknown.
QString videoCodecName(const GUID& subtype);

QSize applyPixelAspect(const QSize& size, UINT32 num, UINT32 den);

struct Geometry {
    QSize frame;        // coded frame size
    QRect aperture;     // visible part of the frame
    QSize display;      // aperture with pixel aspect ratio and rotation applied
    int   rotation = 0; // clockwise degrees that turn the decoded picture upright
};
Geometry geometryOf(IMFMediaType* type);

// ---- D3D11 / DXGI ---------------------------------------------------------------------------------

// A multithread-protected D3D11 device with video support. allowWarp: fall back to the WARP software
// rasterizer when there is no usable GPU (the player wants that; a hardware encoder does not).
HRESULT createD3D11Device(bool allowWarp, ID3D11Device** device, ID3D11DeviceContext** context);
// A DXGI device manager handing device to Media Foundation components.
HRESULT createDxgiDeviceManager(ID3D11Device* device, IMFDXGIDeviceManager** manager);

// ---- sink writer ----------------------------------------------------------------------------------

// An IMFSinkWriter that writes an MPEG-4 file (.mp4 / .m4a) at path (replaced if it exists), with
// MF_TRANSCODE_CONTAINERTYPE = MPEG4 so the container does not depend on the file extension. Throttling
// is off: callers pace themselves, so WriteSample never blocks (bounded cancel and plugin unload).
// manager (optional) lets encoders use that GPU; hardwareTransforms allows hardware encoders.
HRESULT createMpeg4SinkWriter(const QString& path, IMFDXGIDeviceManager* manager, bool hardwareTransforms, IMFSinkWriter** writer);

} // namespace mf::detail
