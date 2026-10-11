#pragma once

// 2.2 voice: which microphone a voice message is recorded from. Pure QtCore, unit-tested.
//
// Windows lists capture endpoints (IMMDevice: an id like "{0.0.1.00000000}.{guid}" and a friendly
// name); TeamSpeak names the device it captures from in its own terms (getCurrentCaptureMode /
// getCurrentCaptureDeviceName / getCaptureDeviceList: in "Windows Audio Session" mode usually the
// endpoint id, in "DirectSound" mode a name that may be cut short). chooseCaptureDevice() maps one to
// the other, conservatively: an ambiguous name is not a match. Without a match the recording uses
// Windows' default communications microphone, and the recorder window says so.

#include <QPair>
#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

namespace voice {

struct Endpoint {
    QString id;   // IMMDevice::GetId
    QString name; // PKEY_Device_FriendlyName
};

struct EndpointList {
    QVector<Endpoint> endpoints;             // active capture endpoints
    QString           defaultCommunications; // id of Windows' default communications microphone (may be empty)
};

// What TeamSpeak reports for the connection the message goes to (read on the GUI thread).
struct TeamSpeakCapture {
    bool    known     = false; // the calls succeeded
    QString mode;              // "Windows Audio Session", "DirectSound", ...
    QString device;            // getCurrentCaptureDeviceName
    bool    isDefault = false; // TeamSpeak follows Windows' default device
    QVector<QPair<QString, QString>> devices; // getCaptureDeviceList(mode): (name, id)
};

struct DeviceChoice {
    enum class Source {
        TeamSpeak,             // the one TeamSpeak captures from
        DefaultCommunications, // Windows' default communications microphone (no match / TeamSpeak uses "Default")
    };
    Source  source = Source::DefaultCommunications;
    QString endpointId;        // empty: ask Windows for the default communications endpoint
    bool    unmatched = false; // TeamSpeak named a microphone Windows doesn't list (or that fits two)
};

// 2.2.1: always TeamSpeak's own capture device; there is no microphone setting any more.
DeviceChoice chooseCaptureDevice(const TeamSpeakCapture& teamSpeak, const EndpointList& list);

// The endpoint id inside a string such as TeamSpeak's device id ("{0.0.1.00000000}.{guid}", maybe with
// a prefix); empty if there is none.
QString endpointIdIn(const QString& text);

// ---- sample conversion (what WASAPI delivers when it can't convert for us) -------------------------

struct SampleLayout {
    int  channels      = 1;
    int  bitsPerSample = 16; // container size: 16, 24 or 32
    bool isFloat       = false;
};

// Whether appendMono16 can read that layout.
bool isConvertible(const SampleLayout& layout);

// Appends `frames` frames of interleaved samples as mono 16-bit (the channels averaged, float clipped
// to full scale, 24/32-bit rounded down to 16). silent: append zeros instead (WASAPI's SILENT flag).
void appendMono16(const void* data, int frames, const SampleLayout& layout, bool silent, std::vector<int16_t>& out);

} // namespace voice
