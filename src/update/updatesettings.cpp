#include "updatesettings.h"

#include <QSettings>

namespace upd {

namespace {

// Keys and values are allocated by Qt, never QStringLiteral: QSettings keeps them in a process-wide
// cache after the DLL is unloaded (see settings.cpp).
QString key(const char* name)
{
    return QString::fromLatin1(name);
}

} // namespace

UpdateSettings UpdateSettings::load(const QString& file)
{
    const QSettings s(file, QSettings::IniFormat);
    UpdateSettings  result;
    bool            ok    = false;
    const int       check = s.value(key("updateCheck")).toInt(&ok);
    result.check          = ok && (check == 1 || check == 2) ? static_cast<CheckSetting>(check) : CheckSetting::NotAsked;
    const int asked       = s.value(key("updateConsentAsked")).toInt(&ok);
    result.timesAsked     = ok ? qBound(0, asked, kMaxConsentAsks) : 0;
    result.skipped        = Version::parse(s.value(key("updateSkipVersion")).toString());
    return result;
}

void UpdateSettings::save(const QString& file) const
{
    QSettings s(file, QSettings::IniFormat);
    s.setValue(key("updateCheck"), static_cast<int>(check));
    s.setValue(key("updateConsentAsked"), timesAsked);
    // toString() builds a new string from numbers: owned by Qt, nothing of the DLL image.
    s.setValue(key("updateSkipVersion"), skipped.isValid() ? skipped.toString() : QString::fromLatin1(""));
    s.sync();
}

} // namespace upd
