#pragma once

// 2.2 compose: Settings → Sending → "When you drop files on the chat": open the send window (the
// default) or send right away. Holding Ctrl while dropping does the other. Available while "Send files
// dropped on the chat" is on.

#include <QPointer>

#include "settingssection.h"

class QCheckBox;
class QComboBox;

class ComposeSettingsSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit ComposeSettingsSection(QWidget* dialog);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    void followDropSwitch(); // enabled together with the dialog's "Send files dropped on the chat"

    QWidget*           m_group    = nullptr;
    QComboBox*         m_dropMode = nullptr;
    QPointer<QCheckBox> m_dropSwitch;
    bool               m_loading = false;
};
