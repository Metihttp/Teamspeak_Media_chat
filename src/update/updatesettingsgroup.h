#pragma once

// Settings → Updates: a self-contained group for the settings dialog (one line there adds it, one
// line calls apply()). The checkbox follows the dialog's OK/Apply/Cancel; "Check now" and the link
// buttons act at once. Restore defaults never touches it: it records consent, not a preference.

#include <QGroupBox>

class QCheckBox;
class QLabel;
class QPushButton;

namespace upd {

class UpdateSettingsGroup : public QGroupBox
{
    Q_OBJECT

  public:
    explicit UpdateSettingsGroup(QWidget* parent = nullptr);

    // Called by the settings dialog's OK / Apply: saves the checkbox if it was changed.
    void apply();

  private:
    void refresh();

    QCheckBox*   m_automatic = nullptr;
    QLabel*      m_status    = nullptr;
    QPushButton* m_action    = nullptr; // Update… / Restart now / Undo
    QPushButton* m_checkNow  = nullptr;
    bool         m_initial   = false;   // the checkbox as loaded
    bool         m_checking  = false;   // Check now was clicked here
};

} // namespace upd
