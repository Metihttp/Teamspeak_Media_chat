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
class QTabWidget;
class QTimer;
class QVBoxLayout;
class SettingsSection;

// Plugin settings in tabs (2.2): General (media cache), Sending (sending, the note for people without
// the plugin), Receiving & playback, Servers, Privacy & updates. Tabs without content stay hidden.
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.
class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    enum class Tab { General, Sending, ReceivingPlayback, Servers, PrivacyUpdates };

    SettingsDialog(Core* core, QWidget* parent = nullptr);

    // 2.2: a feature's settings, added below the tab's groups in the order of the calls (the dialog
    // takes ownership and shows the tab). A SettingsSection is loaded, checked and stored with the rest
    // of the form; any other widget is only shown.
    void addSection(Tab tab, QWidget* section);

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
    void updateGifHint();
    void updateCacheLabel();
    void clearCache();
    bool checkDownloadUrl(bool normalize); // shows or hides the message under the field
    void showDownloadUrlError(const QString& text);
    void applyTheme();
    void fitToContents();
    QVBoxLayout* tabLayout(Tab tab) const;
    void         showTab(Tab tab);
    void         showTabOf(QWidget* widget); // the tab a widget is on becomes the current one

    Core* m_core;

    QTabWidget*                 m_tabs;
    QVector<QWidget*>           m_pages;    // by Tab
    QVector<SettingsSection*>   m_sections; // added by features

    // Receiving
    QCheckBox* m_inlinePreviews;
    QWidget*   m_receiveDetails; // everything that only matters with inline previews on
    QCheckBox* m_autoDownload;
    QSpinBox*  m_autoDownloadMax;
    QCheckBox* m_autoplayGifs;
    QLabel*    m_gifHint; // shown while Windows animations are off (GIFs then play on hover only)
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
