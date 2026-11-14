#pragma once

// 2.6 chat proxy: Settings -> Privacy & updates -> Chat proxy (design 1.3.10). A short own-keys group (not a
// SettingsSection: the Settings dialog's OK, Apply, Cancel and Restore defaults never touch the chat proxy):
//  * the status row of the Chat proxy window (its texts; "No chat proxy is set up." first when nothing is saved);
//  * "Turn on/off chat pro&xy" acts at once (like the Updates group's Check now), hidden while nothing is saved;
//    "&Chat proxy settings…" opens the window, window-modal to Settings; "Use &it as the chat proxy" only while Windows
//    names only a SOCKS proxy (the window opens with the form filled).
// It follows proxy::Control::routeChanged / healthChanged. No timers.

#include <QGroupBox>
#include <QPointer>

#include "control.h"

class QLabel;
class QPushButton;

namespace proxy::ui {

class ProxyGroup : public QGroupBox
{
    Q_OBJECT

  public:
    explicit ProxyGroup(QWidget* parent = nullptr, Control* control = nullptr);

    void refresh();

    // tests
    QLabel*      statusLabel() const { return m_status; }
    QPushButton* toggleButton() const { return m_toggle; }
    QPushButton* settingsButton() const { return m_settings; }
    QPushButton* useItButton() const { return m_useIt; }
    QString      statusIcon() const { return m_iconKind; }
    QLabel*      statusIconLabel() const { return m_icon; }

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void     paintIcon(); // the status icon in the palette's colours of now (the check mark in the ok colour)
    QPalette themePalette() const; // the Settings dialog's (its role style sheet colours the status text from it)

    QPointer<Control> m_control;
    QLabel*           m_icon     = nullptr;
    QLabel*           m_status   = nullptr;
    QPushButton*      m_toggle   = nullptr;
    QPushButton*      m_settings = nullptr;
    QPushButton*      m_useIt    = nullptr;
    QString           m_iconKind;
};

} // namespace proxy::ui
