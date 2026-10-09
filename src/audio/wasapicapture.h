#pragma once

// 2.2 voice: the microphone, through WASAPI in shared mode (the same way TeamSpeak and every other app
// share it; nothing exclusive). Event driven, 200 ms buffer, 48 kHz mono 16-bit with Windows doing the
// conversion; when the driver refuses that, the device's own mix format is converted here (44.1 or
// 48 kHz only, see capturedevice.h). No Media Foundation: the plugin keeps importing only the
// delay-loaded mfplat / mfreadwrite.
//
// Everything here runs on the recorder's worker thread with COM initialized, except interrupt().

#include <memory>

#include "audio/capture.h"
#include "audio/capturedevice.h"

namespace voice {

// Active capture endpoints and Windows' default communications microphone. Any thread with COM
// initialized (the settings dialog calls it on the GUI thread). Empty when there are none or COM fails.
EndpointList listCaptureEndpoints();

// The error for an HRESULT from the audio APIs (E_ACCESSDENIED -> PrivacyBlocked, ...).
CaptureError captureErrorFor(long hr);

class WasapiCapture : public CaptureBackend
{
  public:
    // endpointId: an IMMDevice id; empty = Windows' default communications microphone.
    explicit WasapiCapture(const QString& endpointId);
    ~WasapiCapture() override;

    CaptureResult open(CaptureFormat* format, QString* deviceName) override;
    CaptureResult read(std::vector<int16_t>& out, int timeoutMs) override;
    void          close() override;
    void          interrupt() override;

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace voice
