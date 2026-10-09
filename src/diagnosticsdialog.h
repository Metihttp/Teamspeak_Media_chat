#pragma once

// The "Diagnostic info" window (/tsmedia diag, Settings → Diagnostic info…): shows exactly the
// plain-text report it copies for a bug report, with file names hidden unless the user asks for them.

#include <QDialog>
#include <QPushButton>
#include <QThreadPool>

#include <memory>

#include "diagnostics.h"
#include "diagnosticscollect.h"

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QTimer;

// Non-modal, deletes itself on close. The objectName starts with "tsmedia" so plugin shutdown closes
// it; the destructor waits for the worker that checks codecs and reads the log.
class DiagnosticsDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit DiagnosticsDialog(const diag::Environment& env, QWidget* parent = nullptr);
    ~DiagnosticsDialog() override;

    QString report() const { return m_report; } // empty while collecting
    bool    isCollecting() const { return !m_facts; }

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void onCollected(const diag::Facts& facts);
    void render();
    bool copy();
    void copyAndOpen();
    void showStatus(const QString& text, bool error);
    void applyTheme();

    std::unique_ptr<diag::Facts> m_facts; // null while collecting
    QString                      m_report;

    QPlainTextEdit* m_text;
    QCheckBox*      m_includeNames;
    QLabel*         m_namesHint;
    QLabel*         m_status;
    QPushButton*    m_copy;
    QPushButton*    m_copyAndOpen;
    QPushButton*    m_close;
    QTimer*         m_statusTimer;
    bool            m_ready         = false;
    bool            m_keyboardFocus = false;
    bool            m_applyingTheme = false;

    QThreadPool m_pool; // collectOnWorker; waited for in the destructor (no plugin code after unload)
};

// "Diagnostic info…" as a link-style button, for the settings window. It opens the window through the
// opener plugin.cpp registers, and stays hidden when there is none.
class DiagnosticsButton : public QPushButton
{
  public:
    explicit DiagnosticsButton(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void applyTheme();
    bool m_applyingTheme = false;
};

namespace diag {

// plugin.cpp registers its showDiagnostics() at start and clears it at shutdown (GUI thread).
using DialogOpener = void (*)(QWidget* parent);
void setDialogOpener(DialogOpener opener);
bool hasDialogOpener();
bool openDialog(QWidget* parent); // false if no opener is registered

} // namespace diag
