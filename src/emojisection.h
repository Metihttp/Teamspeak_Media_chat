#pragma once

// 2.2 emoji: Settings > General > "Emoji": HD emoji in the chat, our picker on TeamSpeak's emoji button,
// jumbo emoji for messages of only emoji. All on by default. 2.5 GIFs: "Remember GIFs I send" (the picker's GIFs tab),
// the list's count and size, and Remove all GIFs….

#include "settingssection.h"

class QCheckBox;
class QLabel;
class QPushButton;

class EmojiSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit EmojiSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

    // 2.5 GIFs: Remove all's question (none: the leftover copies; one: "the GIF"; else "all N GIFs").
    static QString removeAllQuestion(int count);

  private:
    void updateEnabled();
    void updateGifs();       // 2.5 GIFs: "GIFs: 37 · 48 MB"
    void confirmRemoveAll(); // 2.5 GIFs

    QCheckBox* m_hd;
    QCheckBox* m_jumbo;
    QCheckBox* m_button;
    QCheckBox*   m_remember  = nullptr; // 2.5 GIFs
    QLabel*      m_gifCount  = nullptr;
    QPushButton* m_removeAll = nullptr;
    bool       m_colour = true; // the colour renderer works here
};
