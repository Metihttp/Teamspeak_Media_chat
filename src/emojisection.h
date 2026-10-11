#pragma once

// 2.2 emoji: Settings > General > "Emoji": HD emoji in the chat, our picker on TeamSpeak's emoji button,
// jumbo emoji for messages of only emoji. All on by default.

#include "settingssection.h"

class QCheckBox;
class QLabel;

class EmojiSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit EmojiSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    void updateEnabled();

    QCheckBox* m_hd;
    QCheckBox* m_jumbo;
    QCheckBox* m_button;
    bool       m_colour = true; // the colour renderer works here
};
