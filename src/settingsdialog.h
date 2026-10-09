#pragma once

#include <QDialog>
#include <QVector>

#include "settings.h"

class Core;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

// Plugin settings: Receiving, Playback, Media cache, Sending and the note for people without the plugin.
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    SettingsDialog(Core* core, QWidget* parent = nullptr);

  signals:
    void settingsChanged();

  protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void load(const Settings& settings); // fills the form; Apply stays as it is
    bool apply();                        // saves what changed; false (and nothing saved) if an input is invalid
    void setDirty(bool dirty);
    void updateEnabled();
    void syncVolume();
    void updateCacheLabel();
    void clearCache();
    bool checkDownloadUrl(bool normalize); // shows or hides the message under the field
    void showDownloadUrlError(const QString& text);
    void applyTheme();
    void fitToContents();

    Core* m_core;

    // Receiving
    QCheckBox* m_inlinePreviews;
    QWidget*   m_receiveDetails; // everything that only matters with inline previews on
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

    // Media cache
    QSpinBox*    m_cacheLimit;
    QLabel*      m_cacheLabel;
    QPushButton* m_clearCache;
    QTimer*      m_cacheNotice;   // keeps "Cleared ..." readable for a moment
    quint64      m_cacheUsed = 0; // bytes, measured when the dialog opens and around clearing

    // Sending
    QCheckBox* m_dragDrop;
    QCheckBox* m_paste;
    QCheckBox* m_jpeg;
    QCheckBox* m_previews;
    QSpinBox*  m_uploadMax;
    QLineEdit* m_uploadDir;

    // Note for people without the plugin
    QCheckBox* m_notice;
    QWidget*   m_noteDetails;
    QLineEdit* m_downloadUrl;
    QWidget*   m_downloadUrlError;
    QLabel*    m_downloadUrlErrorText;
    QString    m_downloadUrlHint;

    QPushButton*       m_applyButton;
    QVector<QWidget*>  m_indented;              // lined up with the text of the checkbox above
    QVector<QSpinBox*> m_numberFields;          // share one width
    Settings           m_loaded;                // what the form showed when it was loaded or last applied
    bool               m_ready         = false; // the constructor has finished
    bool               m_sized         = false; // opened at its full size once
    bool               m_loading       = false;
    bool               m_keyboardFocus = false; // focus has moved by keyboard: show focus rings
    bool               m_applyingTheme = false;
};
