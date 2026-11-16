#pragma once

// 2.2 foundation: a group of settings that a feature adds to the settings dialog
// (SettingsDialog::addSection). The dialog loads it with the rest, asks it to check and store its values
// on OK / Apply, and marks itself changed when the section says so. Build it like the dialog's own
// groups: strings through i18n::t, style sheets with QString::fromLatin1. Its labels with the "hint" / "error" role are
// coloured by the dialog's theme code.
// 2.7 F3 (design_settings 4.2): a section is one or more cards (card()), each a column of rows: switchRow() (a switch on
// the right, its helper text under the title), fieldRow() (label and helper on the left, the control on the right; it
// turns stacked when the control is wide), stackedRow() (label, a full-width field, helper) and customRow(). A row that
// only matters while another option is on is indented under it (indent 1, 2) and greyed out (setRowEnabled), never
// hidden.

#include <QWidget>

#include "settings.h"

class QFormLayout;
class QGroupBox;
class QLabel;
class SettingRow;
class SwitchBox;

class SettingsSection : public QWidget
{
    Q_OBJECT

  public:
    explicit SettingsSection(QWidget* parent = nullptr);

    // Fills the form from s (when the dialog opens, after Apply, and for Restore defaults; see
    // restoreDefaults). Must not emit changed().
    virtual void load(const Settings& s) = 0;
    // Restore defaults: by default load(defaults). Settings that Restore defaults must keep (update
    // consent, a skipped version, per-server overrides) stay as they are in the form.
    virtual void restoreDefaults(const Settings& defaults) { load(defaults); }
    // Before saving: the widget with a problem (the dialog shows its page and focuses it; show the
    // message under the field yourself), or nullptr when everything can be saved.
    virtual QWidget* validate() { return nullptr; }
    // Writes the values that differ from what was loaded (loaded) into target. The dialog calls it for
    // the live settings and for its own copy of what is now loaded, so a value someone else saved while
    // the dialog was open is only overwritten when it was changed here. Use take().
    virtual void store(Settings& target, const Settings& loaded) const = 0;

    // The dialog's building blocks, so sections look the same.
    static QLabel*      hint(const QString& text, QWidget* parent); // secondary text under an option
    static QFormLayout* form(QWidget* parent);
    static void         addUnderField(QFormLayout* layout, QWidget* widget); // a row under the field it explains

    // 2.7 F3: cards and rows (design_settings 4.2). A card is a QGroupBox (its title above it, by the dialog's sheet).
    static QGroupBox*  card(const QString& title, QWidget* parent);
    static void        makeCard(QGroupBox* box); // a QGroupBox subclass that holds rows (Servers, Updates)
    // A switch with its helper text (also its accessible description); trailing: a control between title and switch.
    static SwitchBox*  switchRow(QGroupBox* card, const QString& text, const QString& hint, int indent = 0, QWidget* trailing = nullptr);
    // A label with its buddy and helper on the left, the field on the right (stacked when the field is wide). The label.
    static QLabel*     fieldRow(QGroupBox* card, const QString& label, QWidget* field, const QString& hint, int indent = 0);
    // A label, a full-width field (text fields), extra (e.g. an error line), the helper. The label.
    static QLabel*     stackedRow(QGroupBox* card, const QString& label, QWidget* field, const QString& hint, int indent = 0, QWidget* extra = nullptr);
    static SettingRow* customRow(QGroupBox* card, QWidget* content, int indent = 0);
    // The row a control sits in (nullptr outside rows), and the row greyed out with it (title, helper and control).
    static SettingRow* rowOf(const QWidget* control);
    static void        setRowEnabled(QWidget* control, bool enabled);
    static QLabel*     hintOf(const QWidget* control); // the helper text of the control's row (nullptr: none)

    template <typename T, typename U>
    static void take(T& target, const U& loaded, const T& value)
    {
        if (!(value == loaded))
            target = value;
    }

  signals:
    void changed();          // the user changed something (Apply becomes available)
    void showPage(int page); // 2.7 F3 (M10): a link to another page (SettingsDialog::Tab) was clicked
};
