#pragma once

// 2.7 Dark mode: Settings → Chat → "Appearance", the first card of the Chat page (design_dark 3.1, 3.2):
//   &Dark mode   acts at once (like Chat text size): the switch calls the hook (plugin.cpp's setDarkMode: saved at once,
//                applied at once). Not a form value: OK, Apply, Cancel and Restore defaults never touch it.
// refresh() mirrors Settings::darkMode (the Plugins menu or the hotkey changed it while the window is open; window
// activation). Without the hook (tests, a section on its own) the switch is disabled with its current value.

#include <functional>

#include "settingssection.h"

class SwitchBox;

class AppearanceSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit AppearanceSection(QWidget* parent = nullptr, std::function<void(bool)> setDarkMode = {});

    void load(const Settings& s) override;                       // only mirrors the live value
    void store(Settings& target, const Settings& loaded) const override; // nothing: not a form value
    void restoreDefaults(const Settings& defaults) override;     // nothing: Restore defaults leaves it alone

    SwitchBox* darkModeSwitch() const { return m_switch; }

  public slots:
    void refresh();

  protected:
    bool event(QEvent* event) override;

  private:
    std::function<void(bool)> m_setDarkMode;
    SwitchBox*                m_switch = nullptr;
};
