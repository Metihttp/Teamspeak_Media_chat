#include "audio/capturedevice.h"

#include <QRegularExpression>

#include <cmath>
#include <cstring>

namespace voice {

namespace {

const Endpoint* byId(const EndpointList& list, const QString& id)
{
    if (id.isEmpty())
        return nullptr;
    for (const Endpoint& e : list.endpoints) {
        if (e.id.compare(id, Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

// A friendly name: exact (ignoring case) first, then the one endpoint whose name starts with it or that
// it starts with (DirectSound cuts names at 31 characters). Two candidates: no match.
const Endpoint* byName(const EndpointList& list, const QString& rawName)
{
    const QString name = rawName.trimmed();
    if (name.size() < 3)
        return nullptr;
    for (const Endpoint& e : list.endpoints) {
        if (e.name.trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return &e;
    }
    const Endpoint* found = nullptr;
    for (const Endpoint& e : list.endpoints) {
        const QString other = e.name.trimmed();
        if (other.isEmpty())
            continue;
        const bool prefix = other.startsWith(name, Qt::CaseInsensitive) || (other.size() >= 8 && name.startsWith(other, Qt::CaseInsensitive));
        if (!prefix)
            continue;
        if (found)
            return nullptr; // ambiguous
        found = &e;
    }
    return found;
}

} // namespace

QString endpointIdIn(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("\\{0\\.0\\.1\\.[0-9a-fA-F]{8}\\}\\.\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\}"));
    const QRegularExpressionMatch m = pattern.match(text);
    return m.hasMatch() ? m.captured(0) : QString();
}

DeviceChoice chooseCaptureDevice(const QString& setting, const TeamSpeakCapture& teamSpeak, const EndpointList& list)
{
    DeviceChoice choice;
    if (!setting.isEmpty()) {
        if (const Endpoint* e = byId(list, setting)) {
            choice.source     = DeviceChoice::Source::Setting;
            choice.endpointId = e->id;
            return choice;
        }
        choice.settingMissing = true;
    }
    if (!teamSpeak.known || teamSpeak.isDefault || teamSpeak.device.trimmed().isEmpty())
        return choice; // "Default" in TeamSpeak: which Windows role it means is not known

    // The current device and, from TeamSpeak's list, its (name, id) pair.
    QStringList candidates{teamSpeak.device};
    for (const auto& pair : teamSpeak.devices) {
        if (pair.first == teamSpeak.device || pair.second == teamSpeak.device)
            candidates << pair.second << pair.first;
    }
    for (const QString& c : qAsConst(candidates)) {
        const Endpoint* e = byId(list, c);
        if (!e)
            e = byId(list, endpointIdIn(c));
        if (e) {
            choice.source     = DeviceChoice::Source::TeamSpeak;
            choice.endpointId = e->id;
            return choice;
        }
    }
    for (const QString& c : qAsConst(candidates)) {
        if (const Endpoint* e = byName(list, c)) {
            choice.source     = DeviceChoice::Source::TeamSpeak;
            choice.endpointId = e->id;
            return choice;
        }
    }
    return choice;
}

bool isConvertible(const SampleLayout& layout)
{
    if (layout.channels < 1 || layout.channels > 32)
        return false;
    if (layout.isFloat)
        return layout.bitsPerSample == 32;
    return layout.bitsPerSample == 16 || layout.bitsPerSample == 24 || layout.bitsPerSample == 32;
}

void appendMono16(const void* data, int frames, const SampleLayout& layout, bool silent, std::vector<int16_t>& out)
{
    if (frames <= 0)
        return;
    if (silent || !data || !isConvertible(layout)) {
        out.insert(out.end(), static_cast<size_t>(frames), int16_t(0));
        return;
    }
    const int   channels = layout.channels;
    const auto* bytes    = static_cast<const unsigned char*>(data);
    const int   width    = layout.bitsPerSample / 8;
    out.reserve(out.size() + static_cast<size_t>(frames));
    for (int f = 0; f < frames; ++f) {
        double sum = 0.0; // in units of full scale (-1..1)
        for (int c = 0; c < channels; ++c) {
            const unsigned char* s = bytes + (static_cast<size_t>(f) * channels + c) * width;
            if (layout.isFloat) {
                float v = 0.0f;
                memcpy(&v, s, sizeof(v));
                if (!(v == v)) // NaN
                    v = 0.0f;
                sum += qBound(-1.0, static_cast<double>(v), 1.0);
            } else if (width == 2) {
                int16_t v = 0;
                memcpy(&v, s, sizeof(v));
                sum += v / 32768.0;
            } else if (width == 3) {
                const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(s[0]) << 8) | (static_cast<uint32_t>(s[1]) << 16) | (static_cast<uint32_t>(s[2]) << 24));
                sum += v / 2147483648.0;
            } else {
                int32_t v = 0;
                memcpy(&v, s, sizeof(v));
                sum += v / 2147483648.0;
            }
        }
        const double mono = sum / channels;
        out.push_back(static_cast<int16_t>(qBound(-32768L, std::lround(mono * 32768.0), 32767L)));
    }
}

} // namespace voice
