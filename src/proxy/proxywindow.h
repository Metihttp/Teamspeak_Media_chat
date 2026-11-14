#pragma once

// 2.6 chat proxy: the Chat proxy window (design 1.3) and its Settings group (proxygroup.h). One window at a time
// (tsmediaProxyDialog: the plugin's unload sweep deletes it with the other tsmedia* windows), deleted on close.
//
//  * The form is the saved config (proxy::Control::config()); the fields are always enabled (the saved proxy can be looked
//    at, edited and tested while it is off); the checkbox only says whether it is in use. OK validates (1.3.3), seals a
//    typed password once with the endpoint's entropy and saves (Control::save); Cancel, Esc or closing saves nothing.
//  * The saved password is never decrypted into the field; it follows its endpoint (S7): changing type, server, port or
//    user name without typing a password drops it on OK (a hint says so before).
//  * Find my VPN app, Paste from clipboard and Test (one request to www.youtube.com through the form's proxy) show their
//    result in one row; editing a field, Cancel or closing cancels a running Test or Find (its result is dropped).
//  * The status row says what is in use now (the saved state), refreshed on Control::routeChanged / healthChanged.
//
// CRASH RULE: no timers at all; the jobs' results come through their own posting links (testrun.h, findrun.h); no
// metatypes; texts through i18n::t, style sheets with fromLatin1.

#include <QDialog>
#include <QPointer>
#include <QVector>

#include <functional>

#include "control.h"
#include "linkparse.h"
#include "protect.h"
#include "proxyconfig.h"
#include "proxytexts.h"
#include "proxyui.h"
#include "testrun.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace proxy {
class FindRun;
} // namespace proxy

namespace proxy::ui {

class ProxyWindow : public QDialog
{
    Q_OBJECT

  public:
    // The Test and Find jobs (the real ones: TestRun / FindRun, owned by the window); tests put fakes in.
    struct Jobs {
        std::function<bool(const Config& form, protect::SecretBytes typed, const QByteArray& savedBlob, std::function<void(const TestResult&)> done)> test;
        std::function<bool(quint16 bridgePort, std::function<void(const QVector<quint16>&)> done)> find;
        std::function<void()> cancel; // both
    };
    ProxyWindow(Control* control, QWidget* parent = nullptr, const Jobs& jobs = Jobs());
    ~ProxyWindow() override;

    void prepare(Request request); // the form for a request (after construction, before show)

    // ---- tests ----
    QCheckBox*    useBox() const { return m_use; }
    QComboBox*    typeBox() const { return m_type; }
    QLineEdit*    serverField() const { return m_server; }
    QLineEdit*    portField() const { return m_port; }
    QLineEdit*    userField() const { return m_user; }
    QLineEdit*    passwordField() const { return m_password; }
    QPushButton*  findButton() const { return m_find; }
    QPushButton*  pasteButton() const { return m_paste; }
    QPushButton*  testButton() const { return m_test; }
    QRadioButton* systemRadio() const { return m_system; }
    QRadioButton* directRadio() const { return m_direct; }
    QPushButton*  useItButton() const { return m_useIt; }
    QLabel*       statusLabel() const { return m_statusText; }
    QLabel*       resultLabel() const { return m_resultText; }
    QWidget*      resultRow() const { return m_resultRow; }
    QLabel*       lanHint() const { return m_lanHint; }
    QLabel*       droppedHint() const { return m_droppedHint; }
    QLabel*       portsHint() const { return m_portsHint; }
    QString       errorOf(QWidget* field) const; // the message under a field ("" when none)
    bool          errorIconShown(QWidget* field) const;
    QString       resultIcon() const { return m_resultIconKind; } // "", "check", "warning"
    QString       statusIcon() const { return m_statusIconKind; }
    QLabel*       statusIconLabel() const { return m_statusIcon; }
    bool          accept_(); // OK's path (tests): true when it saved
    void          applyThemeForTests() { applyTheme(); }
    void          reject() override; // Cancel, Esc: nothing saved, a running Test or Find canceled

  protected:
    bool event(QEvent* event) override; // LayoutRequest: fitHeight()
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    struct ErrorRow {
        QWidget* row  = nullptr;
        QLabel*  icon = nullptr;
        QLabel*  text = nullptr;
        QWidget* field = nullptr;
        QString  hint; // the field's accessible description when it has no error
    };

    void      loadForm(const Config& saved);
    Config    formConfig() const;         // type, server, port, user (no password)
    Endpoint  formEndpoint() const;
    bool      endpointChanged() const;    // type, server, port or user name differ from the saved ones
    void      updatePasswordHints();
    void      updateLanHint();
    bool      validate(bool forOk, QWidget** first);
    bool      validateServer(bool required);
    bool      validatePort(bool required);
    bool      validateLogin();
    void      showError(QWidget* field, const QString& text);
    void      clearErrors();
    bool      serverLink();               // a link or host:port in the server field: filled from it (true), or its error
    void      fillFrom(const ProxyLink& link);
    void      startTest();
    void      startFind();
    void      paste();
    void      cancelJobs();
    void      testDone(const TestResult& result, const Config& tested, bool savedValues);
    void      findDone(const QVector<quint16>& found);
    void      setBusy(bool testing, bool finding);
    void      showResult(const QString& text, const QString& icon); // icon: "", "check", "warning"
    void      refreshStatus();
    void      routeChanged();
    void      useIt();
    void      touched();
    void      applyTheme();
    void      setIcon(QLabel* label, const QString& kind);
    void      fitHeight(); // grows the window when a row appears, so wrapped hints never slide under the buttons

    QPointer<Control> m_control;
    Jobs              m_jobs;
    TestRun*          m_testRun = nullptr; // the real jobs (null with fakes)
    FindRun*          m_findRun = nullptr;
    bool              m_testing = false, m_finding = false;
    bool              m_built   = false;   // the constructor finished (a skin's StyleChange comes during it)
    quint64           m_jobSeq  = 0;       // a result of an older job (edited or canceled since) is dropped

    QCheckBox*    m_use      = nullptr;
    QComboBox*    m_type     = nullptr;
    QLineEdit*    m_server   = nullptr;
    QLineEdit*    m_port     = nullptr;
    QLineEdit*    m_user     = nullptr;
    QLineEdit*    m_password = nullptr;
    QLabel*       m_portsHint = nullptr;
    QLabel*       m_lanHint   = nullptr;
    QLabel*       m_droppedHint = nullptr;
    QPushButton*  m_find  = nullptr;
    QPushButton*  m_paste = nullptr;
    QPushButton*  m_test  = nullptr;
    QWidget*      m_resultRow  = nullptr;
    QLabel*       m_resultIcon = nullptr;
    QLabel*       m_resultText = nullptr;
    QString       m_resultIconKind;
    QRadioButton* m_system = nullptr;
    QRadioButton* m_direct = nullptr;
    QButtonGroup* m_whenOff = nullptr;
    QLabel*       m_statusIcon = nullptr;
    QLabel*       m_statusText = nullptr;
    QString       m_statusIconKind;
    QPushButton*  m_useIt = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QVector<ErrorRow> m_errors;

    Config m_saved;              // the saved config the form started from
    bool   m_checkTouched = false; // the user changed the checkbox / the radios: a route change elsewhere leaves them
    bool   m_radioTouched = false;
    bool   m_loading      = false;
    bool   m_theming      = false;
};

} // namespace proxy::ui
