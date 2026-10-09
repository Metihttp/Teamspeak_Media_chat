#pragma once

// 2.4 compress: Settings → Sending → "Videos": compress videos larger than N MB, convert videos that may
// not play for others, the quality, and whether the graphics card may be used. Unavailable without Media
// Foundation (Windows N); the graphics card switch is off when this computer has no video encoder on it.

#include <QPointer>

#include "settingssection.h"

class Core;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

class CompressSettingsSection : public SettingsSection
{
    Q_OBJECT

  public:
    CompressSettingsSection(QWidget* dialog, Core* core);

    void load(const Settings& s) override;
    void store(Settings& target, const Settings& loaded) const override;

  private:
    void updateEnabled();
    void updateEncoders(); // the graphics card switch, once Core knows the encoders

    QPointer<Core> m_core;
    bool           m_available = true; // Media Foundation is there
    QCheckBox*     m_compress  = nullptr;
    QSpinBox*      m_threshold = nullptr;
    QCheckBox*     m_convert   = nullptr;
    QLabel*        m_qualityLabel = nullptr;
    QComboBox*     m_quality   = nullptr;
    QCheckBox*     m_gpu       = nullptr;
    QLabel*        m_hint      = nullptr;
    bool           m_noGpuEncoder = false;
    bool           m_loading      = false;
};
