#pragma once

// 2.2 servergroup: the "Server access" section of the settings dialog. A self-contained box: it shows
// the state of the current server's TS Media chat group (access::buildView) and runs Create / Repair
// through AccessGroup. Server actions take effect right away; OK, Apply, Cancel and Restore defaults
// don't touch them. Requests go to the server only while the box is visible.
//
// Labels use the settings dialog's roles ("hint", "error"), so its theme code colours them.

#include <QGroupBox>
#include <QPointer>

#include "accessgroupview.h"

class AccessGroup;
class QDialog;
class QLabel;
class QPushButton;

class AccessGroupBox : public QGroupBox
{
    Q_OBJECT

  public:
    explicit AccessGroupBox(QWidget* parent = nullptr);
    ~AccessGroupBox() override;

    // Shows this state instead of the live one (render harness, tests).
    void showPreview(const access::ViewInput& view, const QString& server);
    // Opens the Details window for the state shown (also used by the harness).
    void openDetails(const access::Details& details);

  signals:
    void contentsChanged(); // the box may need more height: the dialog grows to fit

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void refreshView();
    void render(const access::View& view, const QString& server, bool connected);
    void setStatusIcon(access::View::Icon icon);
    void runPrimary();
    void showDetails();

    QPointer<AccessGroup> m_access;
    QPointer<QDialog>     m_detailsWindow;
    QLabel*               m_intro;
    QWidget*              m_serverRow;
    QLabel*               m_server;
    QLabel*               m_statusIcon;
    QLabel*               m_status;
    QLabel*               m_detail;      // role hint
    QLabel*               m_detailError; // role error, for errors and "isn't protected"
    QLabel*               m_extra;       // role hint
    QWidget*              m_buttons;
    QPushButton*          m_primary;
    QPushButton*          m_details;
    QPushButton*          m_checkAgain;
    access::View::Icon    m_icon   = access::View::Icon::None;
    access::Action        m_action = access::Action::None;
    quint64               m_sch    = 0; // the connection shown
    bool                  m_watching = false;
    bool                  m_preview  = false;
};
