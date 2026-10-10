#include "audio/aacwriter.h"

#include <QFile>

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "audio/waveform.h"
#include "video/mfcommon.h"

using Microsoft::WRL::ComPtr;

namespace voice {

namespace {

constexpr UINT32 kAacBytesPerSecond = 12000; // 96 kbps: the encoder's lowest rate
constexpr UINT32 kAacProfileLevel   = 0x29;  // AAC-LC

AacResult failed(HRESULT hr, const char* step)
{
    AacResult r;
    r.code  = hr;
    r.error = QString::fromLatin1(step) + QStringLiteral(" failed: ") + mf::detail::hexCode(hr);
    return r;
}

HRESULT audioType(const GUID& subtype, UINT32 rate, IMFMediaType** out)
{
    ComPtr<IMFMediaType> type;
    HRESULT              hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr))
        hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    if (IsEqualGUID(subtype, MFAudioFormat_AAC)) {
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSecond);
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // raw AAC in the MPEG-4 container
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, kAacProfileLevel);
    } else {
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        if (SUCCEEDED(hr))
            hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 2);
    }
    if (SUCCEEDED(hr))
        *out = type.Detach();
    return hr;
}

AacResult encode(const QString& path, const int16_t* samples, qint64 frames, int sampleRate, double gain, const std::atomic<bool>* cancel,
                 std::atomic<qint64>* writtenMs)
{
    namespace detail = mf::detail;
    if (!detail::mediaFoundationPresent()) {
        AacResult r;
        r.error = QStringLiteral("Media Foundation is not available");
        return r;
    }
    detail::ComScope com(COINIT_MULTITHREADED);
    if (!com.usable())
        return failed(com.result(), "CoInitializeEx");
    detail::PlatformScope platform;
    if (FAILED(platform.result()))
        return failed(platform.result(), "MFStartup");

    const UINT32          rate = static_cast<UINT32>(sampleRate);
    ComPtr<IMFSinkWriter> writer;
    HRESULT               hr = detail::createMpeg4SinkWriter(path, nullptr, false, &writer);
    if (FAILED(hr))
        return failed(hr, "MFCreateSinkWriterFromURL");
    ComPtr<IMFMediaType> outType;
    ComPtr<IMFMediaType> inType;
    DWORD                stream = 0;
    hr                          = audioType(MFAudioFormat_AAC, rate, &outType);
    if (SUCCEEDED(hr))
        hr = writer->AddStream(outType.Get(), &stream);
    if (FAILED(hr))
        return failed(hr, "AddStream (AAC)");
    hr = audioType(MFAudioFormat_PCM, rate, &inType);
    if (SUCCEEDED(hr))
        hr = writer->SetInputMediaType(stream, inType.Get(), nullptr);
    if (FAILED(hr))
        return failed(hr, "SetInputMediaType (PCM)");
    hr = writer->BeginWriting();
    if (FAILED(hr))
        return failed(hr, "BeginWriting");

    const qint64 chunk = rate / 10; // 100 ms per sample
    for (qint64 at = 0; at < frames; at += chunk) {
        if (cancel && cancel->load()) {
            AacResult r;
            r.canceled = true;
            r.error    = QStringLiteral("canceled");
            return r;
        }
        const qint64           count = qMin(chunk, frames - at);
        const DWORD            bytes = static_cast<DWORD>(count * 2);
        ComPtr<IMFMediaBuffer> buffer;
        hr          = MFCreateMemoryBuffer(bytes, &buffer);
        BYTE* data  = nullptr;
        if (SUCCEEDED(hr))
            hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            auto* pcm = reinterpret_cast<int16_t*>(data);
            for (qint64 i = 0; i < count; ++i)
                pcm[i] = gain == 1.0 ? samples[at + i] : waveform::applyGain(samples[at + i], gain);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bytes);
        }
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr))
            hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
            hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr))
            hr = sample->SetSampleTime(at * 10000000LL / rate);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleDuration(count * 10000000LL / rate);
        if (SUCCEEDED(hr))
            hr = writer->WriteSample(stream, sample.Get());
        if (FAILED(hr))
            return failed(hr, "WriteSample");
        if (writtenMs)
            writtenMs->store((at + count) * 1000 / rate);
    }
    hr = writer->Finalize();
    if (FAILED(hr))
        return failed(hr, "Finalize");
    AacResult r;
    r.ok = true;
    return r;
}

} // namespace

AacResult writeAac(const QString& path, const int16_t* samples, qint64 frames, int sampleRate, double gain, const std::atomic<bool>* cancel,
                   std::atomic<qint64>* writtenMs)
{
    AacResult r;
    if (!samples || frames <= 0 || (sampleRate != 44100 && sampleRate != 48000)) {
        r.error = QStringLiteral("nothing to encode");
        return r;
    }
    QFile::remove(path);
    r = encode(path, samples, frames, sampleRate, gain, cancel, writtenMs);
    if (!r.ok)
        QFile::remove(path); // the writer is gone (released in encode): the partial file can go
    return r;
}

} // namespace voice
