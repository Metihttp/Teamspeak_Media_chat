#pragma once

// The updater's choices in settings.ini, kept apart from Settings on purpose: they record consent,
// not preferences, so the settings dialog's "Restore defaults" never touches them.
//
//   updateCheck        0 = not asked yet (no requests at all), 1 = on, 2 = off; anything else is 0
//   updateConsentAsked how often the consent window was shown (0..2)
//   updateSkipVersion  a version the user skipped ("2.2.1"), validated; otherwise empty

#include <QString>

#include "updatemanifest.h"
#include "updatepolicy.h"

namespace upd {

struct UpdateSettings {
    CheckSetting check      = CheckSetting::NotAsked;
    int          timesAsked = 0;
    Version      skipped;

    // file: the plugin's settings.ini
    static UpdateSettings load(const QString& file);
    void                  save(const QString& file) const;
};

} // namespace upd
