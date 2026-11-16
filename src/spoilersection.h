#pragma once

// 2.2 spoiler: the "Spoilers" card of Settings → Receiving & playback (SettingsDialog::addSection). One option:
// Settings::revealSpoilers (2.7: it covers text spoilers too).

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
