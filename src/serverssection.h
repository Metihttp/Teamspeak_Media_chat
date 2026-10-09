#pragma once

// 2.2 per-server settings: the "Servers" section of the settings dialog. Pick a server (connected
// ones first, then the ones with own settings) and give it its own data saver, upload folder,
// upload size limit and note for people without the plugin. Self-contained: SettingsDialog places it,
// feeds it the global values and the connected servers, and calls applyTo() on OK / Apply.
// Nothing here calls TeamSpeak (datasaver::connectedServers() does).

#include <QGroupBox>
#include <QVector>

#include "serversettings.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
struct Settings;

class ServersSection : public QGroupBox
{
    Q_OBJECT

  public:
    explicit ServersSection(QWidget* parent = nullptr);

    // What "Same as all servers (…)" shows: the dialog's current values for all servers, saved or not.
    void setGlobals(bool dataSaver, const QString& uploadDirectory, int uploadMaxMB, bool addRequiredNotice);

    // Lists the connected servers (the current tab first, as given) and the servers with own settings
    // in Settings::instance(). Values edited here and not applied yet are kept; everything else shows
    // what is saved now (e.g. after the Plugins menu changed the data saver).
    void reload(const QVector<ConnectedServer>& connected);

    bool hasChanges() const;
    // Writes what was changed here into settings.servers: only the values edited here (a change made
    // meanwhile through the Plugins menu stays), Forget removes the server. Then nothing is pending.
    void applyTo(Settings& settings);

    QSpinBox* limitField() const { return m_limit; } // shares the dialog's number field width

  signals:
    void changed(); // the user edited something (Apply becomes available)

  private:
    struct Row {
        QString         key;
        QString         name; // sanitised server name ("" if unknown)
        bool            connected = false;
        ServerOverrides base;   // saved values the edits are relative to
        ServerOverrides edit;   // what the form shows
        bool            forget = false; // Forget pressed: the saved values are dropped on Apply
        bool dirty() const { return forget || !edit.sameValues(base); }
    };

    void    rebuildCombo(const QString& selectKey);
    void    showRow();
    void    storeForm();
    void    updateTexts();
    void    updateState();
    Row*    currentRow();
    QString rowLabel(const Row& row) const;

    QComboBox*   m_server;
    QPushButton* m_forget;
    QComboBox*   m_downloads;
    QLineEdit*   m_folder;
    QSpinBox*    m_limit;
    QComboBox*   m_note;
    QLabel*      m_hint;
    QVector<QWidget*> m_fields; // rows 2-5 and their labels, disabled without a server

    QVector<Row> m_rows;
    int          m_current = -1;
    bool         m_loading = false;

    bool    m_globalDataSaver = false;
    QString m_globalFolder;
    int     m_globalLimitMB = 100;
    bool    m_globalNote    = true;
};
