#pragma once

// 2.2 foundation: a group of settings that a feature adds to the settings dialog
// (SettingsDialog::addSection). The dialog loads it with the rest, asks it to check and store its values
// on OK / Apply, and marks itself changed when the section says so. Build it like the dialog's own
// groups: a QGroupBox per topic, visible labels, helper text from hint(), strings through i18n::t,
// style sheets with QString::fromLatin1. Its labels with the "hint" / "error" role are coloured by the
// dialog's theme code.

#include <QVector>
#include <QWidget>

#include "settings.h"

class QFormLayout;
class QLabel;

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
    // Before saving: the widget with a problem (the dialog shows its tab and focuses it; show the
    // message under the field yourself), or nullptr when everything can be saved.
    virtual QWidget* validate() { return nullptr; }
    // Writes the values that differ from what was loaded (loaded) into target. The dialog calls it for
    // the live settings and for its own copy of what is now loaded, so a value someone else saved while
    // the dialog was open is only overwritten when it was changed here. Use take().
    virtual void store(Settings& target, const Settings& loaded) const = 0;

    // Widgets lined up with the text of the checkbox above them (the dialog sets their left margin).
    const QVector<QWidget*>& indented() const { return m_indented; }

    // The dialog's building blocks, so sections look the same.
    static QLabel*      hint(const QString& text, QWidget* parent); // secondary text under an option
    static QFormLayout* form(QWidget* parent);
    static void         addUnderField(QFormLayout* layout, QWidget* widget); // a row under the field it explains

    template <typename T, typename U>
    static void take(T& target, const U& loaded, const T& value)
    {
        if (!(value == loaded))
            target = value;
    }

  signals:
    void changed(); // the user changed something (Apply becomes available)

  protected:
    void addIndented(QWidget* widget) { m_indented.append(widget); }

  private:
    QVector<QWidget*> m_indented;
};
