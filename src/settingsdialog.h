#pragma once

#include <QDialog>
#include <QPointer>
#include <QVector>

#include "settings.h"
#include "settingstheme.h" // 2.7 F3: the tokens its painted widgets ask for

class Core;
class QCheckBox;
class QComboBox; // 2.5 zoom
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMessageBox;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class SettingsSection;
class ServersSection; // 2.2 per-server settings

// Plugin settings (2.7 F3, design_settings): a sidebar of pages (Chat, Emoji & GIFs, Sending, Receiving & playback, Link
// previews, Privacy, Servers, About & updates), each a scrolling page of titled cards with rows; OK, Cancel, Apply and
// Restore defaults below. Pages without content stay hidden. Every size follows TeamSpeak's font (the factor k: sharp
// and roomy at 100-200 %), every colour the skin's palette (the tokens). Closing with unsaved changes (X, Esc) asks.
// The window's objectName starts with "tsmedia" so plugin shutdown can close it.
class SettingsDialog : public QDialog, public settingstheme::TokenSource
{
    Q_OBJECT

  public:
    // 2.7 F3: the sidebar's pages, in its order (the name is kept to limit churn).
    enum class Tab { Chat, EmojiGifs, Sending, ReceivingPlayback, LinkPreviews, Privacy, Servers, About };

    SettingsDialog(Core* core, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    // 2.2: a feature's settings, added below the page's cards in the order of the calls (the dialog
    // takes ownership and shows the page). A SettingsSection is loaded, checked and stored with the rest
    // of the form; any other widget is only shown.
    // atTop: above what the page has so far (the chat layout heads the Chat page).
    void addSection(Tab tab, QWidget* section, bool atTop = false);
    // 2.2 per-server settings: the connected servers or saved server settings changed (connection,
    // Plugins menu, command). Values edited in the dialog and not applied yet are kept.
    void reloadServers();
    void showTab(Tab tab); // e.g. About & updates after "Check for updates…" in the Plugins menu
    Tab  currentTab() const;

    // 2.7 F3 (M2): Esc and the window's X come here: with unsaved changes it asks "Save your changes?" (Save, Discard,
    // Cancel; a window of its own, never a nested event loop). The Cancel button discards without asking.
    void reject() override;
    void discardAndClose();
    bool hasUnsavedChanges() const;

    // settingstheme::TokenSource
    const settingstheme::Tokens& themeTokens() const override { return m_tokens; }
    bool                         keyboardFocusShown() const override { return m_keyboardFocus; }

    // Tests and the live-test driver.
    QListWidget*    pageList() const { return m_nav; }
    QStackedWidget* pageStack() const { return m_stack; }
    QScrollArea*    pageArea(Tab tab); // 2.7.3 (M2): the page attached first, as when it is shown
    QWidget*        page(Tab tab) const { return m_pages.value(static_cast<int>(tab)); }
    // 2.7.3 (M2): a page goes into the window when it is first shown (until then it waits, mostly unpolished, in a window
    // that is never shown: Qt's repolish for a new application sheet polishes a widget once per styled ancestor, and the
    // hidden pages made up most of the dark mode switch with the window open; review fix: a switch takes the pages not on
    // screen out again, and they come back when shown). pageAttached: it is in the window now. controls and
    // control: the window's widgets of a type (by object name), on every page (findChildren sees the attached pages only).
    // attachAllPages: tests that need every page's geometry.
    bool pageAttached(Tab tab) const;
    void attachAllPages();
    template <class T>
    QList<T*> controls() const;
    template <class T>
    T*   control(const QString& name) const;
    void showTabOf(QWidget* widget); // the page a widget is on becomes the current one
    QLabel*         unsavedNote() const { return m_unsaved; }
    // H1: the size the window opens at on a screen with this available area (900 x 640 times k, within 92 % of it).
    QSize initialSizeFor(const QSize& available) const;
    QSize minimumSizeFor(const QSize& available) const;

  signals:
    void settingsChanged();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override; // review fix (2.7.3, M2): a dark mode switch takes the pages not on screen out
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override; // 2.7 live: Tab leaves the button bar

  private:
    void load(const Settings& settings, bool sections = true); // fills the form; Apply stays as it is
    bool apply();                        // saves what changed; false (and nothing saved) if an input is invalid
    void okClicked();                    // apply (when changed), then close
    void setDirty(bool dirty);
    void updateEnabled();
    void syncVolume();
    void updateGifHint();
    void updateCacheLabel();
    void clearCache();
    bool checkDownloadUrl(bool normalize); // shows or hides the message under the field
    void showDownloadUrlError(const QString& text);
    void applyTheme();
    // 2.7.2 (D7): dark mode switched the look while the window was open: one re-theme now (the palette and the sheet both
    // in place), then each page laid out once.
    void themeChangedByDarkMode();
    void rebuildTabOrder();
    void stepPage(int direction); // Ctrl+Tab and friends: the next visible page, around
    void initialSize();
    QWidget*     addPage(Tab tab, const QString& title, const QString& description);
    QVBoxLayout* pageLayout(Tab tab) const;
    void         insertCard(Tab tab, QWidget* card, bool last = false); // last: stays below what sections add later
    void         attachPage(int index); // 2.7.3 (M2)
    void         detachPage(int index); // review fix: back into the window that holds the pages not shown (a switch)

    Core* m_core;

    QListWidget*              m_nav   = nullptr;
    QStackedWidget*           m_stack = nullptr;
    QVector<QWidget*>         m_pages;     // by Tab: the parent of its cards
    QVector<QScrollArea*>     m_pageAreas; // by Tab: the page scrolls on its own (2.7.3: nullptr until first shown)
    QVector<QWidget*>         m_lastCards; // by Tab: a card that stays last (Media cache, the note), or nullptr
    QVector<SettingsSection*> m_sections;  // added by features

    // Receiving
    QCheckBox* m_inlinePreviews;
    QGroupBox* m_receiveCard;
    QGroupBox* m_previewCard; // 2.7 F3: Preview size, a card of its own (follows "Show images ...")
    QCheckBox* m_autoDownload;
    QSpinBox*  m_autoDownloadMax;
    QCheckBox* m_autoplayGifs;
    QLabel*    m_gifHint; // shown while Windows animations are off (GIFs then play on hover only)
    QSpinBox*  m_videoAutoDownload;
    QSpinBox*  m_previewWidth;
    QSpinBox*  m_previewHeight;
    QComboBox* m_previewScale; // 2.5 zoom: Preview size
    QCheckBox* m_dataSaver; // 2.2 data saver

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
    QLineEdit* m_downloadUrl;
    QWidget*   m_downloadUrlError;
    QLabel*    m_downloadUrlErrorText;
    QString    m_downloadUrlHint;

    ServersSection* m_servers; // 2.2 per-server settings

    QDialogButtonBox*     m_restoreBox  = nullptr;
    QDialogButtonBox*     m_buttons     = nullptr;
    QPushButton*          m_applyButton = nullptr;
    QLabel*               m_unsaved     = nullptr; // "Not saved yet: OK or Apply saves your changes."
    QPointer<QMessageBox> m_question;              // M2: "Save your changes?"
    QVector<QSpinBox*>    m_numberFields;          // share one width
    Settings              m_loaded;                // what the form showed when it was loaded or last applied
    settingstheme::Tokens m_tokens;
    bool                  m_ready         = false; // the constructor has finished
    bool                  m_sized         = false; // opened at its size once
    bool                  m_loading       = false;
    bool                  m_keyboardFocus = false; // focus has moved by keyboard: show focus rings
    bool                  m_applyingTheme = false;
    int                   m_sheetSets     = 0;     // 2.7.2 (D7): the window's own sheet set (tests)
    QString               m_sheetSetUnder;         // ... and the application sheet when it was set last (tests)
    QPointer<QWidget>     m_unshown;               // 2.7.3 (M2): holds the pages not shown yet (never shown; deleted with the window)

  public:
    // 2.7.2 (D7), tests: how often the window set its own sheet, under which application sheet the last time, and the
    // style changes its pages handled (each lays its page out again).
    int     sheetSets() const { return m_sheetSets; }
    QString sheetSetUnder() const { return m_sheetSetUnder; }
    int     pageStyleChanges() const;
};

template <class T>
QList<T*> SettingsDialog::controls() const
{
    QList<T*> all = findChildren<T*>();
    if (m_unshown)
        all += m_unshown->findChildren<T*>();
    return all;
}

template <class T>
T* SettingsDialog::control(const QString& name) const
{
    if (T* found = findChild<T*>(name))
        return found;
    return m_unshown ? m_unshown->findChild<T*>(name) : nullptr;
}
