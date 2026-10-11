#pragma once

// The updater's windows: the one-time consent window and the update dialog (available, downloading,
// checking/installing, installed, error). Views only: Updater (updater.cpp) decides what they show.
//
// Both are non-modal, parented to TeamSpeak's main window, shown without taking the focus (typing in
// the chat is never captured), and named tsmedia* so plugin shutdown deletes them. Every text comes
// from i18n::t; notes from the manifest are plain text; style sheets are built with fromLatin1.

#include <QDate>
#include <QDialog>
#include <QStringList>

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace upd {

class ProgressLine;

// Shared look: hint colour, primary button, focus rings under TeamSpeak's dark skins.
void styleUpdateWindow(QWidget* window, ProgressLine* progress = nullptr);

// "Keep TS Media chat up to date?" [No thanks] [Turn on]. Closing it (X, Esc) means "ask me later".
class ConsentWindow : public QDialog
{
    Q_OBJECT

  public:
    explicit ConsentWindow(QWidget* parent = nullptr);

  signals:
    void turnOn();
    void noThanks();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    bool m_ready = false;
};

class UpdateDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit UpdateDialog(QWidget* parent = nullptr);

    struct Available {
        QString     version;   // "2.2.1"
        QString     installed; // "2.2.0"
        QDate       published; // may be invalid
        qint64      bytes = 0; // download size
        QStringList notes;     // plain text, at most 5
        QString     notesUrl;  // https://github.com/.../releases/tag/v2.2.1
        bool        skipped = false; // a manual check found a version the user skipped
    };
    enum class ErrorAction { None, TryAgain, OpenDownloadPage, ShowFolder };
    struct Error {
        QString     heading;
        QString     body;
        ErrorAction action = ErrorAction::None;
    };

    void showAvailable(const Available& info);
    void showDownloading(const QString& version, qint64 done, qint64 total);
    void showWorking(const QString& version, const QString& status); // "Checking the files…", "Installing…"
    void showInstalled(const QString& version, int connectedServers, int runningUploads);
    void showError(const Error& error);
    // Closes the window for good: the Updater's answer to a button, Esc or the title bar's X. Never
    // close() for that: QDialog::closeEvent calls reject(), which asks the Updater again, so a close()
    // from inside that answer was ignored and the window stayed open (live test 2.2: "Restart later"
    // printed its notice twice and the window stayed).
    void dismiss();

    enum class Page { Available, Downloading, Working, Installed, Error };
    Page page() const { return m_page; }

  signals:
    void updateClicked();
    void laterClicked();
    void skipClicked();
    void cancelClicked();
    void restartNowClicked();
    void restartLaterClicked();
    void tryAgainClicked();
    void openDownloadPageClicked();
    void showFolderClicked();
    void closeClicked();

  protected:
    void reject() override; // Esc and the title bar's X: the choice that changes nothing
    void changeEvent(QEvent* event) override;

  private:
    void setPage(Page page);
    void setButtons(std::initializer_list<QPushButton*> visible, QPushButton* primary, QPushButton* defaultButton);
    void fitHeight(); // grows (never shrinks) to what the current page needs: no jumps between states
    void refreshLink();

    QString         m_notesUrl;
    bool            m_ready  = false; // the constructor has finished (style events come during it)
    int             m_height = 0;
    Page            m_page   = Page::Available;
    QLabel*         m_icon    = nullptr;
    QLabel*         m_heading = nullptr;
    QStackedWidget* m_stack   = nullptr;
    // Available
    QLabel*          m_meta        = nullptr;
    QLabel*          m_skippedHint = nullptr;
    QLabel*          m_notesTitle  = nullptr;
    QVector<QLabel*> m_notes;
    QLabel*          m_notesLink = nullptr;
    // Downloading / Working
    QLabel*       m_status   = nullptr;
    ProgressLine* m_progress = nullptr;
    // Installed / Error
    QLabel* m_body       = nullptr;
    QLabel* m_serverHint = nullptr;
    QLabel* m_uploadHint = nullptr;

    QPushButton*          m_skip         = nullptr;
    QPushButton*          m_later        = nullptr;
    QPushButton*          m_update       = nullptr;
    QPushButton*          m_cancel       = nullptr;
    QPushButton*          m_restartLater = nullptr;
    QPushButton*          m_restartNow   = nullptr;
    QPushButton*          m_close        = nullptr;
    QPushButton*          m_tryAgain     = nullptr;
    QPushButton*          m_openPage     = nullptr;
    QPushButton*          m_showFolder   = nullptr;
    QVector<QPushButton*> m_allButtons;
};

} // namespace upd
