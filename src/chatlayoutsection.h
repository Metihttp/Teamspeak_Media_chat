#pragma once

// Chat redesign: Settings > General > "Chat layout", at the top: Cozy (the default), Compact or TeamSpeak
// classic, and what the layout does (grouping, the action bar, avatars, mentions, collapsed events).
// Everything applies at once (OK / Apply).

#include "settingssection.h"

class QCheckBox;
class QRadioButton;

class ChatLayoutSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit ChatLayoutSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    void updateEnabled();
    int  chosen() const;

    QRadioButton* m_cozy;
    QRadioButton* m_compact;
    QRadioButton* m_classic;
    QCheckBox*    m_group;
    QCheckBox*    m_actions;
    QCheckBox*    m_avatars;  // P2
    QCheckBox*    m_mentions; // P2
    QCheckBox*    m_collapse; // P2
};
