#include "mfmusicreader.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <propsys.h>
#include <wrl/client.h>

#include <QStringList>

#include <cmath>

#include "i18n.h"
#include "mfcommon.h"

using Microsoft::WRL::ComPtr;

namespace mf {

namespace {

constexpr DWORD kFirstAudio  = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kAllStreams  = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
constexpr int   kMaxTagChars = 400; // tags are cut here already; the caller sanitizes and cuts again

// PKEY_Title ({f29f85e0-4ff9-1068-ab91-08002b27b3d9}, 2) and PKEY_Music_Artist
// ({56a3372e-ce9c-11d2-9f0e-006097c686f6}, 2), spelled out so no GUID library or INITGUID is needed.
const PROPERTYKEY kTitle  = {{0xf29f85e0, 0x4ff9, 0x1068, {0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9}}, 2};
const PROPERTYKEY kArtist = {{0x56a3372e, 0xce9c, 0x11d2, {0x9f, 0x0e, 0x00, 0x60, 0x97, 0xc6, 0x86, 0xf6}}, 2};

QString tagText(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    QString text;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal) {
            text = QString::fromWCharArray(value.pwszVal);
        } else if (value.vt == (VT_VECTOR | VT_LPWSTR) && value.calpwstr.cElems > 0 && value.calpwstr.pElems) {
            QStringList parts; // several artists
            for (ULONG i = 0; i < value.calpwstr.cElems && i < 8; ++i) {
                if (value.calpwstr.pElems[i])
                    parts << QString::fromWCharArray(value.calpwstr.pElems[i]);
            }
            text = parts.join(QStringLiteral(", "));
        }
    }
    PropVariantClear(&value);
    return text.left(kMaxTagChars);
}

// 48 kHz stereo, as float (preferred) or 16-bit PCM.
HRESULT outputType(bool asFloat, IMFMediaType** out)
{
    ComPtr<IMFMediaType> type;
    HRESULT              hr = MFCreateMediaType(&type);
    const UINT32         bits = asFloat ? 32 : 16;
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_SUBTYPE, asFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MusicReader::kRate);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MusicReader::kChannels);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, MusicReader::kChannels * bits / 8);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, MusicReader::kRate * MusicReader::kChannels * bits / 8);
    if (SUCCEEDED(hr))
        *out = type.Detach();
    return hr;
}

// 2.3 review: the reader's output is still what read() takes: 48 kHz stereo in the sample format open()
// chose. Before, a format change mid-file was checked for rate and channels only: 16-bit data then read as
// float would have gone to the whole channel as full-scale noise.
bool isOutputType(IMFMediaType* type, bool asFloat)
{
    GUID subtype = GUID_NULL;
    return type && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == (asFloat ? MFAudioFormat_Float : MFAudioFormat_PCM)
           && MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0) == static_cast<UINT32>(MusicReader::kRate)
           && MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0) == static_cast<UINT32>(MusicReader::kChannels)
           && MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0) == (asFloat ? 32u : 16u);
}

} // namespace

// ---- ThreadScope ----------------------------------------------------------------------------------------

struct ThreadScope::Data {
    detail::ComScope                       com{COINIT_MULTITHREADED};
    std::unique_ptr<detail::PlatformScope> platform;
};

ThreadScope::ThreadScope()
    : d(std::make_unique<Data>())
{
    if (d->com.usable() && detail::mediaFoundationPresent())
        d->platform = std::make_unique<detail::PlatformScope>();
}

ThreadScope::~ThreadScope() = default;

bool ThreadScope::ok() const
{
    return d->platform && SUCCEEDED(d->platform->result());
}

// ---- MusicReader -----------------------------------------------------------------------------------------

struct MusicReader::Data {
    ComPtr<IMFSourceReader> reader;
    bool                    asFloat = true;
    bool                    ended   = false;
};

MusicReader::MusicReader()
    : d(std::make_unique<Data>())
{
}

MusicReader::~MusicReader() = default;

bool MusicReader::fail(long hr, const QString& path)
{
    m_code  = hr;
    m_error = detail::failureText(static_cast<HRESULT>(hr), GUID_NULL, detail::Wording::Audio, path);
    d->reader.Reset();
    return false;
}

bool MusicReader::open(const QString& path)
{
    m_info  = MusicInfo();
    m_error.clear();
    m_code = 0;
    if (!detail::mediaFoundationPresent()) {
        m_error = detail::platformUnavailableText(detail::Wording::Audio);
        m_code  = static_cast<long>(MF_E_PLATFORM_NOT_INITIALIZED);
        return false;
    }
    HRESULT hr = detail::createSourceReader(path, nullptr, &d->reader);
    if (FAILED(hr))
        return fail(hr, path);
    hr = d->reader->SetStreamSelection(kAllStreams, FALSE);
    if (SUCCEEDED(hr))
        hr = d->reader->SetStreamSelection(kFirstAudio, TRUE);
    if (FAILED(hr)) { // no sound at all (a picture, a document)
        m_code  = hr;
        m_error = i18n::t("This file has no sound to play.");
        d->reader.Reset();
        return false;
    }
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(d->reader->GetNativeMediaType(kFirstAudio, 0, &native))) {
        m_info.sourceRate     = static_cast<int>(MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        m_info.sourceChannels = static_cast<int>(MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
    }
    // The reader decodes, resamples and mixes down to 48 kHz stereo (float; 16-bit if float is refused).
    for (const bool asFloat : {true, false}) {
        ComPtr<IMFMediaType> want;
        hr = outputType(asFloat, &want);
        if (SUCCEEDED(hr))
            hr = d->reader->SetCurrentMediaType(kFirstAudio, nullptr, want.Get());
        if (SUCCEEDED(hr)) {
            d->asFloat = asFloat;
            break;
        }
    }
    if (FAILED(hr))
        return fail(hr, path);
    ComPtr<IMFMediaType> current;
    hr = d->reader->GetCurrentMediaType(kFirstAudio, &current);
    if (FAILED(hr))
        return fail(hr, path);
    if (!isOutputType(current.Get(), d->asFloat))
        return fail(MF_E_INVALIDMEDIATYPE, path); // never played at the wrong speed

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(d->reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
        m_info.durationMs = static_cast<qint64>(duration.uhVal.QuadPart / 10000);
    PropVariantClear(&duration);

    ComPtr<IPropertyStore> tags;
    if (SUCCEEDED(d->reader->GetServiceForStream(kMediaSource, MF_PROPERTY_HANDLER_SERVICE, IID_PPV_ARGS(&tags))) && tags) {
        m_info.title  = tagText(tags.Get(), kTitle);
        m_info.artist = tagText(tags.Get(), kArtist);
    }
    d->ended = false;
    return true;
}

MusicReader::Read MusicReader::read(std::vector<float>* out, qint64* first)
{
    *first = -1;
    if (!d->reader)
        return Read::Error;
    if (d->ended)
        return Read::End;
    DWORD             stream = 0, flags = 0;
    LONGLONG          time   = 0;
    ComPtr<IMFSample> sample;
    HRESULT           hr = d->reader->ReadSample(kFirstAudio, 0, &stream, &flags, &time, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        fail(FAILED(hr) ? hr : E_FAIL);
        return Read::Error;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        ComPtr<IMFMediaType> current;
        if (FAILED(d->reader->GetCurrentMediaType(kFirstAudio, &current)) || !isOutputType(current.Get(), d->asFloat)) {
            fail(MF_E_INVALIDMEDIATYPE);
            return Read::Error;
        }
    }
    if (sample) {
        LONGLONG sampleTime = 0;
        if (SUCCEEDED(sample->GetSampleTime(&sampleTime)))
            *first = static_cast<qint64>(std::llround(static_cast<double>(sampleTime) * kRate / 10000000.0));
        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data   = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(hr))
            hr = buffer->Lock(&data, nullptr, &length);
        if (FAILED(hr)) {
            fail(hr);
            return Read::Error;
        }
        const size_t at = out->size();
        if (d->asFloat) {
            const size_t count = length / sizeof(float) / kChannels * kChannels;
            out->resize(at + count);
            const auto* src = reinterpret_cast<const float*>(data);
            for (size_t i = 0; i < count; ++i) {
                const float v = src[i];
                (*out)[at + i] = std::isfinite(v) ? v : 0.0f; // a broken decoder never reaches the mix as NaN
            }
        } else {
            const size_t count = length / sizeof(qint16) / kChannels * kChannels;
            out->resize(at + count);
            const auto* src = reinterpret_cast<const qint16*>(data);
            for (size_t i = 0; i < count; ++i)
                (*out)[at + i] = static_cast<float>(src[i]) / 32768.0f;
        }
        buffer->Unlock();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        d->ended = true;
        return sample ? Read::Data : Read::End;
    }
    return Read::Data;
}

bool MusicReader::seek(qint64 frame)
{
    if (!d->reader)
        return false;
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt             = VT_I8;
    position.hVal.QuadPart  = static_cast<LONGLONG>(std::llround(static_cast<double>(qMax<qint64>(0, frame)) * 10000000.0 / kRate));
    const HRESULT hr        = d->reader->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    if (FAILED(hr)) {
        m_code = hr; // not fatal: playback goes on where it was
        return false;
    }
    d->ended = false;
    return true;
}

} // namespace mf
