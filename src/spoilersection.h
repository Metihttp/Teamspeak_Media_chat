#pragma once

// 2.2 spoiler: the "Spoilers" group of the settings dialog's Receiving & playback tab
// (SettingsDialog::addSection). One option: Settings::revealSpoilers.

#include "settingssection.h"

class QCheckBox;

class SpoilerSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit SpoilerSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    QCheckBox* m_showAll = nullptr;
};
