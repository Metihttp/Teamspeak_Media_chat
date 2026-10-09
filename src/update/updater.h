#pragma once

// The update check, on TeamSpeak's GUI thread (docs/UPDATES.md).
//
// Off until the user agrees: 45 s after the first start the consent window asks once ("Keep TS Media
// chat up to date?"); closing it asks again on the next start, at most twice. Once on, the first check
// runs 3 to 8 minutes after the plugin starts, then about once a day (failures back off: 1, 3, 6, 12,
// 24 h). A check fetches one small signed file from GitHub; a newer version is offered in a dialog
// that waits until TeamSpeak is active and doesn't take the focus. Nothing is downloaded before the
// user clicks Update; nothing is installed unless every signature, hash and load check passes; the
// new version starts with the next TeamSpeak start ("Restart TeamSpeak now" or later).
//
// A manual check (Settings, the plugin menu, /tsmedia update) is explicit consent for that one
// request and works with automatic checks off.
//
// Only builds with TSMEDIA_UPDATER (signed releases) check anything. Other builds still settle the
// install state at start (onStarted) and show the "turned off in versions you build yourself" text.

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>
#include <memory>

#include "updatefiles.h"
#include "updatemanifest.h"
#include "updatesettings.h"

class QTimer;
class QWidget;

namespace upd {

class ConsentWindow;
class UpdateDialog;
struct CheckOutcome;
struct PrepareOutcome;
struct WorkerLink;

class Updater : public QObject
{
    Q_OBJECT

  public:
    enum class Phase { Disabled, Idle, Checking, Available, Downloading, Installing, RestartPending, Restarting };
    enum class Origin { Automatic, Settings, Chat };

    struct Status {
        Phase     phase = Phase::Disabled;
        bool      automatic = false; // updateCheck == on
        Version   installed;
        Version   available;       // Available: the offered version
        Version   pending;         // RestartPending: the installed, not yet running version
        Version   skipped;
        QDateTime lastSuccess;     // UTC
        QString   lastError;       // short text if the last check failed ("couldn't reach GitHub")
        bool      manualUpToDate = false; // the last manual check found nothing newer
    };

    struct Hooks {
        std::function<int()> runningUploads; // uploads a restart would cancel
    };

    Updater(const QString& dataDir, Hooks hooks, QObject* parent = nullptr);
    ~Updater() override;

    static Updater* instance();
    static bool     builtIn(); // this build has the update check (TSMEDIA_UPDATER)

    // End of the plugin's start: settles the install state, queues the chat lines, schedules.
    void start();

    void checkNow(Origin origin);
    void setAutomatic(bool on); // the Settings checkbox
    void undoSkip();
    void openUpdateDialog();    // Settings: "Update…"
    void restartNow();          // Settings: "Restart now"; the dialog's button
    Status status() const;

  signals:
    void statusChanged();

  private:
    void schedule();
    void evaluate();
    void startCheck(Origin origin);
    void onCheckDone(const CheckOutcome& outcome, Origin origin);
    void offer(Origin origin);
    bool tryPrompt();
    void startDownload();
    void onPrepareDone(const PrepareOutcome& outcome);
    void install(const PrepareOutcome& outcome);
    void showError(const QString& heading, const QString& body, int action);
    void showConsent();
    void tryShowConsent();
    void queueNotice(const QString& text, bool warning, const QString& linkUrl = QString(), const QString& linkText = QString(),
                     bool startupNotice = false);
    void flushNotices();
    void saveSettings();
    void setPhase(Phase phase);
    void waitForThreads(int ms);
    void pruneThreads(); // closes the handles of finished jobs
    void copyHelperLog();
    UpdateDialog* dialog(bool create);
    QWidget*      parentWindow() const;
    bool          busy() const;

    Layout          m_layout;
    QString         m_settingsFile;
    UpdateSettings  m_settings;
    Hooks           m_hooks;
    Version         m_current;
    Phase           m_phase = Phase::Disabled;
    Manifest        m_manifest; // the offered version
    bool            m_haveManifest = false;
    QDateTime       m_earliestCheck; // start + 3..8 min
    QString         m_lastError;
    QString         m_folderToShow; // "Show folder" in the error dialog
    bool            m_manualUpToDate = false;
    Origin          m_checkOrigin    = Origin::Automatic;
    bool            m_promptPending  = false;
    bool            m_consentPending = false;
    int             m_lastProgressPercent = -1;

    QTimer* m_tick;          // next due check / 30-minute re-evaluation
    QTimer* m_consentTimer;  // 45 s after start
    QTimer* m_noticeTimer;   // prints queued chat lines once a chat is connected
    QDateTime m_noticeUntil;

    struct Notice {
        QString text;
        bool    warning;
        QString linkUrl;
        QString linkText;
        bool    startupNotice;
    };
    QVector<Notice> m_notices;

    QPointer<ConsentWindow> m_consent;
    QPointer<UpdateDialog>  m_dialog;

    std::shared_ptr<WorkerLink> m_link;    // the running job's; replaced per job
    QVector<void*>              m_threads; // thread handles still to wait for
};

} // namespace upd
