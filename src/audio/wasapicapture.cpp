#include "audio/wasapicapture.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <wrl/client.h>

#include <ks.h>
#include <ksmedia.h>

using Microsoft::WRL::ComPtr;

namespace voice {

namespace {

// PKEY_Device_FriendlyName ({a45c254e-df1c-4efd-8020-67d146a850e0}, 14), spelled out so no GUID library
// or INITGUID trick is needed.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr REFERENCE_TIME kBufferDuration = 2000000; // 200 ms in 100 ns units
constexpr HRESULT        kNotFound       = static_cast<HRESULT>(0x80070490L); // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

QString takeCoString(LPWSTR text)
{
    if (!text)
        return {};
    const QString s = QString::fromWCharArray(text);
    CoTaskMemFree(text);
    return s;
}

QString friendlyNameOf(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (!device || FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    QString name;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        name = QString::fromWCharArray(value.pwszVal);
    PropVariantClear(&value);
    return name;
}

HRESULT createEnumerator(ComPtr<IMMDeviceEnumerator>* enumerator)
{
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator->ReleaseAndGetAddressOf()));
}

SampleLayout layoutOf(const WAVEFORMATEX* format)
{
    SampleLayout layout;
    layout.channels      = format->nChannels;
    layout.bitsPerSample = format->wBitsPerSample;
    layout.isFloat       = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            layout.isFloat = true;
        else if (!IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            layout.channels = 0; // something else (compressed): not convertible
    } else if (format->wFormatTag != WAVE_FORMAT_PCM && format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        layout.channels = 0;
    }
    return layout;
}

CaptureResult failure(HRESULT hr, const char* where)
{
    CaptureResult r;
    r.error  = captureErrorFor(hr);
    r.code   = hr;
    r.detail = QString::fromLatin1(where);
    return r;
}

} // namespace

CaptureError captureErrorFor(long hr)
{
    switch (static_cast<HRESULT>(hr)) {
    case S_OK:
        return CaptureError::None;
    case E_ACCESSDENIED:
        return CaptureError::PrivacyBlocked;
    case AUDCLNT_E_DEVICE_IN_USE:
    case AUDCLNT_E_EXCLUSIVE_MODE_ONLY:
        return CaptureError::Busy;
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return CaptureError::Disconnected;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return CaptureError::UnsupportedFormat;
    case kNotFound:
        return CaptureError::NoDevice;
    default:
        return CaptureError::Generic;
    }
}

EndpointList listCaptureEndpoints()
{
    EndpointList                 list;
    ComPtr<IMMDeviceEnumerator>  enumerator;
    if (FAILED(createEnumerator(&enumerator)))
        return list;
    ComPtr<IMMDeviceCollection> devices;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, &device)))
                continue;
            LPWSTR id = nullptr;
            if (FAILED(device->GetId(&id)))
                continue;
            Endpoint e;
            e.id   = takeCoString(id);
            e.name = friendlyNameOf(device.Get());
            list.endpoints.append(e);
        }
    }
    ComPtr<IMMDevice> communications;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &communications))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(communications->GetId(&id)))
            list.defaultCommunications = takeCoString(id);
    }
    return list;
}

struct WasapiCapture::Private {
    QString                     endpointId;
    HANDLE                      packetEvent = nullptr;
    HANDLE                      wakeEvent   = nullptr;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioCaptureClient> capture;
    SampleLayout                layout;      // what GetBuffer delivers
    bool                        convert = false; // layout is the mix format (not the 16-bit mono we asked for)
    bool                        started = false;

    HRESULT activate()
    {
        client.Reset();
        return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    }
};

WasapiCapture::WasapiCapture(const QString& endpointId)
    : d(std::make_unique<Private>())
{
    d->endpointId  = endpointId;
    d->packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d->wakeEvent   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

WasapiCapture::~WasapiCapture()
{
    close();
    if (d->packetEvent)
        CloseHandle(d->packetEvent);
    if (d->wakeEvent)
        CloseHandle(d->wakeEvent);
}

CaptureResult WasapiCapture::open(CaptureFormat* format, QString* deviceName)
{
    if (!d->packetEvent || !d->wakeEvent)
        return failure(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT                     hr = createEnumerator(&enumerator);
    if (FAILED(hr))
        return failure(hr, "MMDeviceEnumerator");
    if (d->endpointId.isEmpty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &d->device);
        if (FAILED(hr))
            return failure(hr, "GetDefaultAudioEndpoint");
    } else {
        const std::wstring id = d->endpointId.toStdWString();
        hr                    = enumerator->GetDevice(id.c_str(), &d->device);
        DWORD state           = 0;
        if (SUCCEEDED(hr))
            hr = d->device->GetState(&state);
        if (SUCCEEDED(hr) && state != DEVICE_STATE_ACTIVE)
            hr = AUDCLNT_E_DEVICE_INVALIDATED; // picked a moment ago, unplugged since
        if (FAILED(hr))
            return failure(hr == kNotFound ? AUDCLNT_E_DEVICE_INVALIDATED : hr, "GetDevice");
    }
    if (deviceName)
        *deviceName = friendlyNameOf(d->device.Get());

    // First choice: 48 kHz mono 16-bit, converted by Windows (the encoder's own format).
    WAVEFORMATEX wanted{};
    wanted.wFormatTag      = WAVE_FORMAT_PCM;
    wanted.nChannels       = 1;
    wanted.nSamplesPerSec  = 48000;
    wanted.wBitsPerSample  = 16;
    wanted.nBlockAlign     = 2;
    wanted.nAvgBytesPerSec = 48000 * 2;
    hr                     = d->activate();
    if (FAILED(hr))
        return failure(hr, "Activate");
    constexpr DWORD kConvertFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = d->client->Initialize(AUDCLNT_SHAREMODE_SHARED, kConvertFlags, kBufferDuration, 0, &wanted, nullptr);
    if (SUCCEEDED(hr)) {
        d->layout       = SampleLayout{1, 16, false};
        d->convert      = false;
        format->sampleRate = 48000;
    } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == E_INVALIDARG) {
        // The mix format, converted here (an IAudioClient can't be initialized twice: a fresh one).
        hr = d->activate();
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(hr))
            hr = d->client->GetMixFormat(&mix);
        if (FAILED(hr))
            return failure(hr, "GetMixFormat");
        const SampleLayout layout = layoutOf(mix);
        const DWORD        rate   = mix->nSamplesPerSec;
        if ((rate != 48000 && rate != 44100) || !isConvertible(layout)) {
            CoTaskMemFree(mix);
            CaptureResult r;
            r.error  = CaptureError::UnsupportedFormat;
            r.code   = static_cast<long>(rate);
            r.detail = QStringLiteral("mix format %1 Hz, %2 channels, %3 bit").arg(rate).arg(layout.channels).arg(layout.bitsPerSample);
            return r;
        }
        hr = d->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr))
            return failure(hr, "Initialize (mix format)");
        d->layout          = layout;
        d->convert         = true;
        format->sampleRate = static_cast<int>(rate);
    } else {
        return failure(hr, "Initialize");
    }
    format->channels = 1;

    hr = d->client->SetEventHandle(d->packetEvent);
    if (SUCCEEDED(hr))
        hr = d->client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(d->capture.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = d->client->Start();
    if (FAILED(hr)) {
        close();
        return failure(hr, "Start");
    }
    d->started = true;
    return {};
}

CaptureResult WasapiCapture::read(std::vector<int16_t>& out, int timeoutMs)
{
    if (!d->capture)
        return failure(AUDCLNT_E_NOT_INITIALIZED, "read");
    const HANDLE handles[2] = {d->packetEvent, d->wakeEvent};
    const DWORD  waited      = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(qMax(0, timeoutMs)));
    if (waited == WAIT_OBJECT_0 + 1)
        return {}; // interrupted: the recorder checks why
    UINT32  packet = 0;
    HRESULT hr     = d->capture->GetNextPacketSize(&packet);
    while (SUCCEEDED(hr) && packet > 0) {
        BYTE*  data   = nullptr;
        UINT32 frames = 0;
        DWORD  flags  = 0;
        hr            = d->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr))
            break;
        appendMono16(data, static_cast<int>(frames), d->layout, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, out);
        hr = d->capture->ReleaseBuffer(frames);
        if (SUCCEEDED(hr))
            hr = d->capture->GetNextPacketSize(&packet);
    }
    if (FAILED(hr))
        return failure(hr, "capture");
    return {};
}

void WasapiCapture::close()
{
    if (d->client && d->started)
        d->client->Stop();
    d->started = false;
    d->capture.Reset();
    d->client.Reset();
    d->device.Reset();
}

void WasapiCapture::interrupt()
{
    if (d->wakeEvent)
        SetEvent(d->wakeEvent);
}

} // namespace voice
