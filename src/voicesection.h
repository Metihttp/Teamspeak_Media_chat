#pragma once

// 2.2 voice: the "Voice messages" group of the settings dialog's General tab (SettingsDialog::addSection):
// the microphone, muting TeamSpeak's microphone while recording, listening before sending (hotkey), the
// start / stop sounds, and the record hotkey with a button to TeamSpeak's hotkey setup.

#include "settingssection.h"

class QCheckBox;
class QComboBox;
class QLabel;

class VoiceSection : public SettingsSection
{
    Q_OBJECT

  public:
    explicit VoiceSection(QWidget* parent = nullptr);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

    static constexpr const char* kHotkeyKeyword = "tsmedia_voice";

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void fillMicrophones(const QString& selected);
    void updateHotkey();

    QComboBox* m_microphone;
    QCheckBox* m_mute;
    QCheckBox* m_review;
    QCheckBox* m_sounds;
    QLabel*    m_hotkey;
};
