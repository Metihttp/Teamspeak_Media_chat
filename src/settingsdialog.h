#pragma once

#include <QDialog>

class Core;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
struct Settings;

// Plugin settings: Receiving, Playback, Sending and General (language, cache).
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    SettingsDialog(Core* core, QWidget* parent = nullptr);

  signals:
    void settingsChanged();

  private:
    void load(const Settings& settings);
    bool apply(); // false (and nothing saved) if an input is invalid
    void updateEnabled();
    void updateCacheLabel();

    Core* m_core;

    // Receiving
    QCheckBox* m_inlinePreviews;
    QCheckBox* m_autoDownload;
    QSpinBox*  m_autoDownloadMax;
    QCheckBox* m_autoplayGifs;
    QSpinBox*  m_videoAutoDownload;
    QSpinBox*  m_previewWidth;
    QSpinBox*  m_previewHeight;

    // Playback
    QSlider*   m_volume;
    QLabel*    m_volumeLabel;
    QCheckBox* m_startMuted;
    QCheckBox* m_loop;

    // Sending
    QCheckBox* m_dragDrop;
    QCheckBox* m_paste;
    QCheckBox* m_jpeg;
    QCheckBox* m_previews;
    QCheckBox* m_notice;
    QLineEdit* m_downloadUrl;
    QSpinBox*  m_uploadMax;
    QLineEdit* m_uploadDir;

    // General
    QComboBox* m_language;
    QSpinBox*  m_cacheLimit;
    QLabel*    m_cacheLabel;
    quint64    m_cacheUsed = 0; // bytes, measured when the dialog opens and after clearing
};
