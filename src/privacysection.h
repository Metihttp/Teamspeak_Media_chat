#pragma once

// 2.2 protocol: Settings > Privacy & updates > "Privacy": reactions and presence, both on by default.

#include "settingssection.h"

class QCheckBox;

class PrivacySection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit PrivacySection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    QCheckBox* m_reactions;
    QCheckBox* m_presence;
};
