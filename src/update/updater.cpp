#include "updater.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QMainWindow>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#include <windows.h>

#include <vector>

#include "i18n.h"
#include "medialink.h" // formatSize
#include "ts3api.h"
#include "updatedialog.h"
#include "updateinstaller.h"
#include "updatepolicy.h"
#include "updateworker.h"
#include "version.h"

namespace upd {

namespace {

QPointer<Updater> g_instance;

QString latin(const char* text)
{
    return QString::fromLatin1(text);
}

void log(const QString& text, LogLevel level = LogLevel_INFO)
{
    ts3::log(latin("[update] ") + text, level);
}

constexpr const char* kCheck = "check";

QString processExePath()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD          length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return QString();
    return QDir::fromNativeSeparators(QString::fromWCharArray(buffer.data(), static_cast<int>(length)));
}

qint64 downloadBytes(const Manifest& m, Arch arch)
{
    qint64 total = 0;
    for (const UpdateFile& f : m.files) {
        if (f.kind == FileKind::Plugin || (f.kind == FileKind::Helper && f.arch == arch))
            total += f.size;
    }
    return total;
}

// A path for a dialog: native separators, and a break opportunity (zero-width space) after each one,
// so a long path wraps instead of running out of the window.
QString displayPath(const QString& path)
{
    QString text = QDir::toNativeSeparators(path);
    text.replace(QLatin1Char('\\'), QString(QLatin1Char('\\')) + QChar(0x200b));
    return text;
}

int connectedServers()
{
    int count = 0;
    for (uint64 sch : ts3::connections()) {
        if (ts3::isConnected(sch))
            ++count;
    }
    return count;
}

// Short text for Settings ("Last check failed: …") and the chat.
QString shortError(http::Error error)
{
    switch (error) {
    case http::Error::Offline:
    case http::Error::Unavailable:
        return i18n::t("couldn't reach GitHub");
    case http::Error::Timeout:
        return i18n::t("GitHub took too long to respond");
    case http::Error::Tls:
        return i18n::t("couldn't make a secure connection to GitHub");
    case http::Error::RateLimited:
    case http::Error::ServerError:
        return i18n::t("GitHub isn't available right now");
    case http::Error::Redirect:
        return i18n::t("GitHub sent the request somewhere else");
    default:
        return i18n::t("GitHub's answer wasn't valid");
    }
}

enum ErrorAction { NoAction = 0, TryAgain = 1, OpenPage = 2, ShowFolder = 3 };

struct ErrorCopy {
    QString heading;
    QString body;
    int     action;
};

ErrorCopy networkError(http::Error error)
{
    switch (error) {
    case http::Error::Timeout:
        return {i18n::t("GitHub is taking too long to respond"), i18n::t("Try again in a few minutes. Nothing was changed."), TryAgain};
    case http::Error::RateLimited:
    case http::Error::ServerError:
        return {i18n::t("GitHub isn't available right now"), i18n::t("Try again later. Nothing was changed."), TryAgain};
    case http::Error::Tls:
        return {i18n::t("Couldn't make a secure connection to GitHub"),
                i18n::t("Your Windows may be missing TLS 1.2 support, or a security program is intercepting the connection. Nothing was "
                        "changed. You can download the update from GitHub yourself."),
                OpenPage};
    case http::Error::Offline:
    case http::Error::Unavailable:
        return {i18n::t("Couldn't reach GitHub"), i18n::t("Check your internet connection and try again. Nothing was changed."), TryAgain};
    default:
        return {i18n::t("GitHub sent an unexpected answer"), i18n::t("Try again later. Nothing was changed."), TryAgain};
    }
}

ErrorCopy signatureError()
{
    return {i18n::t("The update failed the signature check"),
            i18n::t("The downloaded file was deleted and nothing was changed. The download may have been damaged or altered. Try again "
                    "later, or download the update from GitHub yourself."),
            OpenPage};
}

ErrorCopy manualInstallError(const QString& version)
{
    return {i18n::t("This update needs a manual install"),
            i18n::t("TS Media chat %1 can't be installed automatically from this version. Download it from GitHub and double-click the file.").arg(version),
            OpenPage};
}

// TeamSpeak's own Quit action (Ctrl+Q in its menu). Unverified on 3.6.2: see docs/UPDATES.md.
QAction* findQuitAction(QWidget* mainWindow)
{
    if (!mainWindow)
        return nullptr;
    const QKeySequence quit(Qt::CTRL | Qt::Key_Q);
    QAction*           byName = nullptr;
    for (QAction* action : mainWindow->findChildren<QAction*>()) {
        if (action->isSeparator() || action->menu())
            continue;
        if (action->shortcut().matches(quit) == QKeySequence::ExactMatch)
            return action;
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        const QString name = action->objectName().toLower();
        if (!byName && (name.contains(QLatin1String("quit")) || text.compare(QLatin1String("Quit"), Qt::CaseInsensitive) == 0))
            byName = action;
    }
    return byName;
}

} // namespace

// ---- construction ------------------------------------------------------------------------------

Updater::Updater(const QString& dataDir, Hooks hooks, QObject* parent)
    : QObject(parent)
    , m_layout(Layout::forRunningPlugin(dataDir))
    , m_settingsFile(dataDir + QLatin1String("/settings.ini"))
    , m_settings(UpdateSettings::load(m_settingsFile))
    , m_hooks(std::move(hooks))
    , m_current(Version::parse(QString::fromLatin1(TSMEDIA_VERSION)))
{
    g_instance     = this;
    m_tick         = new QTimer(this);
    m_consentTimer = new QTimer(this);
    m_noticeTimer  = new QTimer(this);
    m_tick->setSingleShot(true);
    m_consentTimer->setSingleShot(true);
    m_noticeTimer->setInterval(5000);
    connect(m_tick, &QTimer::timeout, this, [this] { evaluate(); });
    connect(m_consentTimer, &QTimer::timeout, this, [this] { tryShowConsent(); });
    connect(m_noticeTimer, &QTimer::timeout, this, [this] { flushNotices(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
            return;
        if (m_consentPending)
            tryShowConsent();
        if (m_promptPending)
            tryPrompt();
    });
}

Updater::~Updater()
{
    if (m_link) {
        QMutexLocker lock(&m_link->mutex);
        m_link->receiver = nullptr; // no result can be posted to this object any more
        m_link->cancel   = true;
    }
    waitForThreads(1000); // a job still running keeps the DLL pinned and ends on its own
    for (void* thread : qAsConst(m_threads))
        CloseHandle(static_cast<HANDLE>(thread));
    delete m_consent.data();
    delete m_dialog.data();
}

Updater* Updater::instance()
{
    return g_instance.data();
}

bool Updater::builtIn()
{
#ifdef TSMEDIA_UPDATER
    return true;
#else
    return false;
#endif
}

void Updater::pruneThreads()
{
    for (int i = m_threads.size() - 1; i >= 0; --i) {
        if (WaitForSingleObject(static_cast<HANDLE>(m_threads.at(i)), 0) == WAIT_OBJECT_0) {
            CloseHandle(static_cast<HANDLE>(m_threads.at(i)));
            m_threads.remove(i);
        }
    }
}

void Updater::waitForThreads(int ms)
{
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
    for (void* thread : qAsConst(m_threads)) {
        const ULONGLONG now = GetTickCount64();
        WaitForSingleObject(static_cast<HANDLE>(thread), now < until ? static_cast<DWORD>(until - now) : 0);
    }
}

// ---- start and schedule --------------------------------------------------------------------------

void Updater::start()
{
    // Every build settles the install state: the started marker for the helper, applied -> done.
    const StartupNotice notice = onStarted(m_layout, m_current, GetCurrentProcessId());
    if (notice.kind == StartupNotice::Updated) {
        log(latin("now running ") + notice.version.toString());
        queueNotice(i18n::t("Updated to TS Media chat %1.").arg(notice.version.toString()), false,
                    releaseNotesUrl(latin("v") + notice.version.toString()).toString(), i18n::t("What's new in %1").arg(notice.version.toString()), true);
    } else if (notice.kind == StartupNotice::RolledBack) {
        log(latin("the update to ") + notice.version.toString() + latin(" didn't start; ") + notice.restored.toString() + latin(" was restored"),
            LogLevel_WARNING);
        m_settings.skipped = notice.version; // never offered again automatically
        saveSettings();
        queueNotice(i18n::t("The update to %1 couldn't start, so TS Media chat %2 was restored. %1 won't be offered again.")
                        .arg(notice.version.toString(), m_current.toString()),
                    true, QString(), QString(), true);
    }
    // Leftovers go once a restart helper has surely finished.
    QTimer::singleShot(2 * 60 * 1000, this, [this] { cleanup(m_layout, m_current, false); });

    if (!builtIn()) {
        setPhase(Phase::Disabled);
        return;
    }
#ifdef TSMEDIA_TESTHOOKS
    // Local test server only: <data>/update_base.txt = http://127.0.0.1:<port> (refused for any other host).
    QFile base(m_layout.updateDir + latin("/../update_base.txt"));
    if (base.open(QIODevice::ReadOnly)) {
        const QString origin = QString::fromUtf8(base.readAll()).trimmed();
        log(setTestOrigin(origin) ? latin("[test] update origin ") + origin : latin("[test] update origin refused (not a loopback address)"));
    }
#endif
    setPhase(restartPending(m_layout, m_current) ? Phase::RestartPending : Phase::Idle);
    m_earliestCheck = QDateTime::currentDateTimeUtc().addMSecs(initialDelayMs(QRandomGenerator::global()->generate()));
    if (shouldAskConsent(m_settings.check, m_settings.timesAsked))
        m_consentTimer->start(kConsentDelayMs);
    schedule();
}

void Updater::schedule()
{
    m_tick->stop();
    if (!builtIn() || m_settings.check != CheckSetting::On || busy())
        return;
    const QDateTime now  = QDateTime::currentDateTimeUtc();
    const QDateTime next = qMax(sanitizeNext(StateFile(m_layout.stateFile()).timeValue(kCheck, "nextUtc"), now), m_earliestCheck);
    const qint64    wait = qBound<qint64>(1000, now.msecsTo(next), kRecheckIntervalMs);
    m_tick->start(static_cast<int>(wait));
}

void Updater::evaluate()
{
    if (m_promptPending)
        tryPrompt();
    if (!builtIn() || m_settings.check != CheckSetting::On || busy()) {
        schedule();
        return;
    }
    const QDateTime now  = QDateTime::currentDateTimeUtc();
    const QDateTime next = qMax(sanitizeNext(StateFile(m_layout.stateFile()).timeValue(kCheck, "nextUtc"), now), m_earliestCheck);
    if (now >= next)
        startCheck(Origin::Automatic);
    else
        schedule();
}

bool Updater::busy() const
{
    return m_phase == Phase::Checking || m_phase == Phase::Downloading || m_phase == Phase::Installing || m_phase == Phase::Restarting;
}

void Updater::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit statusChanged();
}

Updater::Status Updater::status() const
{
    Status s;
    s.phase          = m_phase;
    s.automatic      = m_settings.check == CheckSetting::On;
    s.installed      = m_current;
    s.available      = m_haveManifest ? m_manifest.version : Version();
    s.pending        = restartPending(m_layout, m_current) ? effectiveInstalled(m_layout, m_current) : Version();
    s.skipped        = m_settings.skipped;
    s.lastSuccess    = StateFile(m_layout.stateFile()).timeValue(kCheck, "lastSuccessUtc");
    s.lastError      = m_lastError;
    s.manualUpToDate = m_manualUpToDate;
    return s;
}

void Updater::saveSettings()
{
    m_settings.save(m_settingsFile);
}

void Updater::setAutomatic(bool on)
{
    const CheckSetting wanted = on ? CheckSetting::On : CheckSetting::Off;
    if (m_settings.check == wanted)
        return;
    m_settings.check = wanted;
    saveSettings();
    log(on ? latin("automatic checks turned on") : latin("automatic checks turned off"));
    m_consentTimer->stop();
    m_consentPending = false;
    if (m_consent)
        m_consent->close();
    schedule();
    emit statusChanged();
}

void Updater::undoSkip()
{
    m_settings.skipped = Version();
    saveSettings();
    emit statusChanged();
}

// ---- consent -------------------------------------------------------------------------------------

void Updater::tryShowConsent()
{
    if (!shouldAskConsent(m_settings.check, m_settings.timesAsked) || m_consent)
        return;
    // Only while the user is in TeamSpeak and nothing modal is open; otherwise when TeamSpeak is active again.
    if (qApp->applicationState() != Qt::ApplicationActive || QApplication::activeModalWidget() || !parentWindow()) {
        m_consentPending = true;
        return;
    }
    m_consentPending = false;
    showConsent();
}

void Updater::showConsent()
{
    auto* window = new ConsentWindow(parentWindow());
    window->setAttribute(Qt::WA_DeleteOnClose);
    m_consent = window;
    ++m_settings.timesAsked;
    saveSettings();
    connect(window, &ConsentWindow::turnOn, this, [this] {
        m_settings.check = CheckSetting::On;
        saveSettings();
        log(latin("automatic checks turned on (consent window)"));
        schedule();
        emit statusChanged();
    });
    connect(window, &ConsentWindow::noThanks, this, [this] {
        m_settings.check = CheckSetting::Off;
        saveSettings();
        log(latin("automatic checks declined"));
        emit statusChanged();
    });
    window->show();
}

QWidget* Updater::parentWindow() const
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    }
    return nullptr;
}

// ---- check ---------------------------------------------------------------------------------------

void Updater::checkNow(Origin origin)
{
    if (!builtIn())
        return;
    if (busy()) {
        if (origin != Origin::Automatic && m_dialog)
            m_dialog->show();
        return;
    }
    if (m_phase == Phase::RestartPending && origin != Origin::Automatic) {
        // An update is installed already: say so instead of checking again.
        openUpdateDialog();
    }
    startCheck(origin);
}

void Updater::startCheck(Origin origin)
{
#ifdef TSMEDIA_UPDATER
    m_checkOrigin    = origin;
    m_manualUpToDate = false;
    const Phase before = m_phase;
    setPhase(Phase::Checking);
    StateFile(m_layout.stateFile()).setTime(kCheck, "lastAttemptUtc", QDateTime::currentDateTimeUtc());
    log(origin == Origin::Automatic ? latin("checking for updates") : latin("checking for updates (asked by the user)"));

    pruneThreads();
    m_link           = std::make_shared<WorkerLink>();
    m_link->receiver = this;
    const QSet<int> revoked = StateFile(m_layout.stateFile()).revokedKeys();
    void* thread = UpdateWorker::startCheck(m_link, trustedKeys(), revoked, [this, origin](const CheckOutcome& outcome) { onCheckDone(outcome, origin); });
    if (!thread) {
        log(latin("couldn't start the update thread"), LogLevel_WARNING);
        setPhase(before == Phase::Checking ? Phase::Idle : before);
        return;
    }
    m_threads.append(thread);
#else
    Q_UNUSED(origin);
#endif
}

#ifdef TSMEDIA_UPDATER
void Updater::onCheckDone(const CheckOutcome& outcome, Origin origin)
{
    StateFile       st(m_layout.stateFile());
    const QDateTime now      = QDateTime::currentDateTimeUtc();
    const quint32   rand     = QRandomGenerator::global()->generate();
    const Phase     restPhase = restartPending(m_layout, m_current) ? Phase::RestartPending : Phase::Idle;

    if (outcome.httpError == http::Error::NotFound) {
        // The latest release has no manifest (yet): not an error, keep the normal schedule.
        log(latin("no update manifest in the latest release"));
        st.setValue(kCheck, "lastResult", latin("notFound"));
        st.setTime(kCheck, "lastSuccessUtc", now);
        st.setInt(kCheck, "failures", 0);
        st.setTime(kCheck, "nextUtc", nextAfterSuccess(now, rand));
        m_lastError.clear();
        m_manualUpToDate = origin != Origin::Automatic;
        setPhase(restPhase);
        if (origin == Origin::Chat)
            ts3::printInfo(ts3::currentConnection(), i18n::t("You have the latest version of TS Media chat (%1).").arg(m_current.toString()));
        schedule();
        emit statusChanged();
        return;
    }

    if (!outcome.ok()) {
        const int failures = st.intValue(kCheck, "failures", 0, 0, 1000) + 1;
        st.setInt(kCheck, "failures", failures);
        st.setTime(kCheck, "nextUtc", nextAfterFailure(now, failures, outcome.retryAfterSec));
        ErrorCopy copy;
        if (outcome.httpError != http::Error::None) {
            st.setValue(kCheck, "lastResult", latin("error:") + http::errorCode(outcome.httpError));
            log(latin("check failed: ") + http::errorCode(outcome.httpError) + latin(" (status ") + QString::number(outcome.httpStatus) + latin(", error ")
                + QString::number(outcome.winError) + latin(")"));
            m_lastError = shortError(outcome.httpError);
            copy        = networkError(outcome.httpError);
        } else {
            st.setValue(kCheck, "lastResult", latin("error:") + errorCode(outcome.manifestError));
            log(latin("the update manifest was rejected: ") + errorCode(outcome.manifestError)
                    + (outcome.detail.isEmpty() ? QString() : latin(" (") + outcome.detail + latin(")")),
                LogLevel_WARNING);
            if (outcome.manifestError == ManifestError::UnsupportedFormat || outcome.manifestError == ManifestError::UnknownKey) {
                m_lastError = i18n::t("the new version needs a manual install");
                copy        = {i18n::t("This update needs a manual install"),
                               i18n::t("The new version can't be installed automatically from this version. Download it from GitHub and double-click "
                                       "the file."),
                               OpenPage};
            } else {
                m_lastError = i18n::t("the update information failed the signature check");
                copy        = signatureError();
            }
        }
        setPhase(restPhase);
        if (origin == Origin::Chat)
            ts3::printWarning(ts3::currentConnection(), i18n::t("Couldn't check for updates: %1.").arg(m_lastError));
        else if (origin == Origin::Settings && m_dialog)
            showError(copy.heading, copy.body, copy.action);
        schedule();
        emit statusChanged();
        return;
    }

    // A valid, signed manifest.
    const Manifest& m = outcome.manifest;
    const QSet<int> revoke = honouredRevocations(m, trustedKeys());
    if (!revoke.isEmpty()) {
        st.addRevokedKeys(revoke); // for good
        log(latin("signing keys revoked by the release: ") + QString::number(revoke.size()), LogLevel_WARNING);
    }
    st.setTime(kCheck, "lastSuccessUtc", now);
    st.setInt(kCheck, "failures", 0);
    st.setTime(kCheck, "nextUtc", nextAfterSuccess(now, rand));
    st.setValue(kCheck, "latest", m.version.toString());
    m_lastError.clear();

    const Offer o = offerFor(m, effectiveInstalled(m_layout, m_current), m_settings.skipped, m_layout.arch);
    log(latin("latest release ") + m.version.toString() + latin(" (host ") + outcome.host + latin(")"));
    switch (o) {
    case Offer::No:
        st.setValue(kCheck, "lastResult", latin("upToDate"));
        m_haveManifest   = false;
        m_manualUpToDate = origin != Origin::Automatic;
        setPhase(restPhase);
        if (origin == Origin::Chat) {
            if (restPhase == Phase::RestartPending)
                ts3::printInfo(ts3::currentConnection(), i18n::t("TS Media chat %1 is installed. Restart TeamSpeak to start using it.")
                                                             .arg(effectiveInstalled(m_layout, m_current).toString()));
            else
                ts3::printInfo(ts3::currentConnection(), i18n::t("You have the latest version of TS Media chat (%1).").arg(m_current.toString()));
        }
        break;
    case Offer::Skipped:
        st.setValue(kCheck, "lastResult", latin("skipped"));
        m_manifest     = m;
        m_haveManifest = true;
        setPhase(restPhase);
        if (origin != Origin::Automatic)
            offer(origin); // a manual check still shows it ("you skipped it")
        break;
    case Offer::NeedsManualInstall: {
        st.setValue(kCheck, "lastResult", latin("needsManualInstall"));
        m_lastError = i18n::t("%1 needs a manual install").arg(m.version.toString());
        setPhase(restPhase);
        const ErrorCopy copy = manualInstallError(m.version.toString());
        const QDateTime remind = st.timeValue(kCheck, "remindAfterUtc");
        if (origin != Origin::Automatic || !remind.isValid() || now >= remind) {
            st.setTime(kCheck, "remindAfterUtc", now.addSecs(kRemindAfterSec));
            if (origin == Origin::Chat)
                ts3::printWarning(ts3::currentConnection(), copy.body);
            else if (origin == Origin::Settings || qApp->applicationState() == Qt::ApplicationActive)
                showError(copy.heading, copy.body, copy.action);
        }
        break;
    }
    case Offer::Yes:
        st.setValue(kCheck, "lastResult", latin("available"));
        m_manifest     = m;
        m_haveManifest = true;
        setPhase(Phase::Available);
        offer(origin);
        break;
    }
    schedule();
    emit statusChanged();
}

#else
void Updater::onCheckDone(const CheckOutcome&, Origin) {}
#endif

// ---- prompt --------------------------------------------------------------------------------------

void Updater::offer(Origin origin)
{
    if (!m_haveManifest)
        return;
    if (origin != Origin::Automatic) {
        m_promptPending = false;
        openUpdateDialog();
        if (m_dialog) {
            m_dialog->raise();
            m_dialog->activateWindow(); // asked for: it may take the focus
        }
        return;
    }
    const QDateTime remind = StateFile(m_layout.stateFile()).timeValue(kCheck, "remindAfterUtc");
    if (remind.isValid() && QDateTime::currentDateTimeUtc() < remind)
        return; // "Later" was clicked less than 20 h ago
    m_promptPending = true;
    tryPrompt();
}

bool Updater::tryPrompt()
{
    if (!m_promptPending)
        return false;
    if (!m_haveManifest || m_phase != Phase::Available) {
        m_promptPending = false;
        return false;
    }
    // Wait until the user is in TeamSpeak and nothing modal is open.
    if (qApp->applicationState() != Qt::ApplicationActive || QApplication::activeModalWidget() || !parentWindow())
        return false;
    m_promptPending = false;
    openUpdateDialog();
    return true;
}

UpdateDialog* Updater::dialog(bool create)
{
    if (!m_dialog && create) {
        auto* d = new UpdateDialog(parentWindow());
        d->setAttribute(Qt::WA_DeleteOnClose);
        m_dialog = d;
        connect(d, &UpdateDialog::laterClicked, this, [this] {
            StateFile(m_layout.stateFile()).setTime(kCheck, "remindAfterUtc", QDateTime::currentDateTimeUtc().addSecs(kRemindAfterSec));
            if (m_dialog)
                m_dialog->close();
        });
        connect(d, &UpdateDialog::skipClicked, this, [this] {
            if (m_haveManifest) {
                m_settings.skipped = m_manifest.version;
                saveSettings();
                log(latin("skipped ") + m_manifest.version.toString());
            }
            if (m_phase == Phase::Available)
                setPhase(restartPending(m_layout, m_current) ? Phase::RestartPending : Phase::Idle);
            if (m_dialog)
                m_dialog->close();
            emit statusChanged();
        });
        connect(d, &UpdateDialog::updateClicked, this, [this] { startDownload(); });
        connect(d, &UpdateDialog::cancelClicked, this, [this] {
            // The worker notices between reads (at most one receive timeout) and removes its partial
            // files; the dialog goes at once.
            if (m_phase == Phase::Downloading && m_link)
                m_link->cancel = true;
            if (m_dialog)
                m_dialog->close();
        });
        connect(d, &UpdateDialog::restartNowClicked, this, [this] { restartNow(); });
        connect(d, &UpdateDialog::restartLaterClicked, this, [this] {
            const Version pending = effectiveInstalled(m_layout, m_current);
            queueNotice(i18n::t("TS Media chat %1 is installed and starts the next time you open TeamSpeak.").arg(pending.toString()), false);
            if (m_dialog)
                m_dialog->close();
        });
        connect(d, &UpdateDialog::tryAgainClicked, this, [this] {
            if (m_haveManifest && m_phase == Phase::Available)
                startDownload();
            else
                checkNow(Origin::Settings);
        });
        connect(d, &UpdateDialog::openDownloadPageClicked, this, [this] {
            QDesktopServices::openUrl(latestReleaseUrl());
            if (m_dialog)
                m_dialog->close();
        });
        connect(d, &UpdateDialog::showFolderClicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_folderToShow.isEmpty() ? m_layout.pluginsDir : m_folderToShow));
            if (m_dialog)
                m_dialog->close();
        });
        connect(d, &UpdateDialog::closeClicked, this, [this] {
            if (m_dialog)
                m_dialog->close();
        });
    }
    return m_dialog.data();
}

void Updater::openUpdateDialog()
{
    if (m_phase == Phase::RestartPending || m_phase == Phase::Restarting) {
        UpdateDialog* d = dialog(true);
        d->showInstalled(effectiveInstalled(m_layout, m_current).toString(), connectedServers(), m_hooks.runningUploads ? m_hooks.runningUploads() : 0);
        d->show();
        return;
    }
    if (!m_haveManifest)
        return;
    UpdateDialog*           d = dialog(true);
    UpdateDialog::Available info;
    info.version   = m_manifest.version.toString();
    info.installed = m_current.toString();
    info.published = m_manifest.published;
    info.bytes     = downloadBytes(m_manifest, m_layout.arch);
    info.notes     = m_manifest.notes;
    info.notesUrl  = releaseNotesUrl(latin("v") + info.version).toString();
    info.skipped   = m_settings.skipped == m_manifest.version;
    if (m_phase != Phase::Downloading && m_phase != Phase::Installing)
        d->showAvailable(info);
    d->show();
}

void Updater::showError(const QString& heading, const QString& body, int action)
{
    UpdateDialog*       d = dialog(true);
    UpdateDialog::Error e;
    e.heading = heading;
    e.body    = body;
    e.action  = action == TryAgain ? UpdateDialog::ErrorAction::TryAgain
              : action == OpenPage ? UpdateDialog::ErrorAction::OpenDownloadPage
              : action == ShowFolder ? UpdateDialog::ErrorAction::ShowFolder
                                     : UpdateDialog::ErrorAction::None;
    d->showError(e);
    d->show();
}

// ---- download, verify, install -----------------------------------------------------------------

void Updater::startDownload()
{
#ifdef TSMEDIA_UPDATER
    if (!m_haveManifest || busy())
        return;
    if (!m_manifest.file(FileKind::Plugin, m_layout.arch)) {
        const ErrorCopy copy = manualInstallError(m_manifest.version.toString());
        showError(copy.heading, copy.body, copy.action);
        return;
    }
    setPhase(Phase::Downloading);
    m_lastProgressPercent = -1;
    const qint64 total    = downloadBytes(m_manifest, m_layout.arch);
    if (UpdateDialog* d = dialog(true)) {
        d->showDownloading(m_manifest.version.toString(), 0, total);
        d->show();
    }
    log(latin("downloading ") + m_manifest.version.toString());
    pruneThreads();
    m_link           = std::make_shared<WorkerLink>();
    m_link->receiver = this;
    void* thread     = UpdateWorker::startPrepare(
        m_link, m_layout, m_manifest,
        [this](qint64 done, qint64 all) {
            if (m_phase != Phase::Downloading || !m_dialog)
                return;
            m_dialog->showDownloading(m_manifest.version.toString(), done, all);
        },
        [this](const PrepareOutcome& outcome) { onPrepareDone(outcome); });
    if (!thread) {
        setPhase(Phase::Available);
        showError(i18n::t("The update couldn't start"), i18n::t("Try again later. Nothing was changed."), TryAgain);
        return;
    }
    m_threads.append(thread);
#endif
}

#ifdef TSMEDIA_UPDATER
void Updater::onPrepareDone(const PrepareOutcome& outcome)
{
    using F = PrepareOutcome::Failure;
    if (outcome.ok() && m_link && m_link->cancel) {
        // Canceled just as the download finished: install nothing.
        log(latin("download canceled"));
        setPhase(Phase::Available);
        return;
    }
    if (outcome.ok()) {
        install(outcome);
        return;
    }
    setPhase(m_haveManifest ? Phase::Available : Phase::Idle);
    switch (outcome.failure) {
    case F::Canceled:
        log(latin("download canceled"));
        if (m_dialog)
            m_dialog->close();
        return;
    case F::Network: {
        log(latin("download failed: ") + http::errorCode(outcome.httpError) + latin(" (error ") + QString::number(outcome.winError) + latin(")"));
        const ErrorCopy copy = networkError(outcome.httpError);
        showError(copy.heading, copy.body, copy.action);
        return;
    }
    case F::Mismatch:
    case F::WrongMachine: {
        log(latin("a downloaded file failed its check: ") + outcome.fileName, LogLevel_WARNING);
        const ErrorCopy copy = signatureError();
        showError(copy.heading, copy.body, copy.action);
        return;
    }
    case F::Quarantined:
        log(latin("a staged file changed or vanished (antivirus?): ") + outcome.fileName, LogLevel_WARNING);
        showError(i18n::t("The update file was blocked"),
                  i18n::t("Windows Security or another antivirus program removed or locked the new plugin file. TS Media chat was not changed. "
                          "You can install the update by hand from GitHub."),
                  OpenPage);
        return;
    case F::Imports:
        log(latin("the new version imports what this TeamSpeak doesn't have: ") + outcome.missingImports.mid(0, 5).join(latin(", ")), LogLevel_WARNING);
        {
            const ErrorCopy copy = manualInstallError(m_manifest.version.toString());
            showError(copy.heading, copy.body, copy.action);
        }
        return;
    case F::DiskSpace:
        showError(i18n::t("Not enough disk space"),
                  i18n::t("The update needs about %1 of free space on %2.")
                      .arg(formatSize(outcome.neededBytes), QDir::toNativeSeparators(m_layout.updateDir.left(2) + QLatin1Char('/'))),
                  TryAgain);
        return;
    case F::Disk:
    case F::None:
        log(latin("can't write the update files (error ") + QString::number(outcome.winError) + latin(")"), LogLevel_WARNING);
        showError(i18n::t("Can't write to the plugins folder"),
                  i18n::t("TS Media chat has no permission to change %1, so it can't update itself. Install the update by hand from GitHub.")
                      .arg(displayPath(m_layout.updateDir)),
                  OpenPage);
        return;
    }
}

#else
void Updater::onPrepareDone(const PrepareOutcome&) {}
#endif

void Updater::install(const PrepareOutcome& outcome)
{
    setPhase(Phase::Installing);
    if (m_dialog)
        m_dialog->showWorking(m_manifest.version.toString(), i18n::t("Installing…"));
    const ApplyResult result = applyUpdate(m_layout, outcome.plugins, m_current, m_manifest.version);
    if (result.ok()) {
        StateFile st(m_layout.stateFile());
        if (outcome.hasHelper)
            st.setValue("install", "helper", QString::fromLatin1(outcome.helper.sha256.toHex()));
        else
            st.remove("install", "helper");
        log(latin("installed ") + m_manifest.version.toString() + latin("; it starts with the next TeamSpeak start"));
        m_haveManifest = false;
        setPhase(Phase::RestartPending);
        UpdateDialog* d = dialog(true);
        d->showInstalled(m_manifest.version.toString(), connectedServers(), m_hooks.runningUploads ? m_hooks.runningUploads() : 0);
        d->show();
        emit statusChanged();
        return;
    }

    setPhase(Phase::Available);
    log(latin("install failed (step ") + QString::number(static_cast<int>(result.error)) + latin(", error ") + QString::number(result.winError) + latin(")"),
        LogLevel_WARNING);
    if (!result.unchanged) {
        // The undo failed too: the user must act before TeamSpeak's next start.
        const QString copy = result.copyPath.isEmpty() ? m_layout.rollbackDir(m_current) : result.copyPath;
        const char*   what = "TS Media chat may not load next time. Before you restart TeamSpeak, copy %1 back to %2 and name it %3.";
        ts3::printWarning(ts3::currentConnection(), i18n::t(what).arg(QDir::toNativeSeparators(copy), QDir::toNativeSeparators(m_layout.pluginsDir), result.missingFile));
        m_folderToShow = fs::isDir(copy) ? copy : QFileInfo(copy).absolutePath();
        showError(i18n::t("The update didn't finish"), i18n::t(what).arg(displayPath(copy), displayPath(m_layout.pluginsDir), result.missingFile), ShowFolder);
        return;
    }
    switch (result.error) {
    case ApplyError::InUse:
    case ApplyError::OtherVolume:
        showError(i18n::t("The update can't be installed while TeamSpeak runs"),
                  i18n::t("Close TeamSpeak, then download the update from GitHub and double-click the file. Nothing was changed."), OpenPage);
        break;
    case ApplyError::NotWritable:
        showError(i18n::t("Can't write to the plugins folder"),
                  i18n::t("TS Media chat has no permission to change %1, so it can't update itself. Install the update by hand from GitHub.")
                      .arg(displayPath(m_layout.pluginsDir)),
                  OpenPage);
        break;
    case ApplyError::StagedChanged:
        showError(i18n::t("The update file was blocked"),
                  i18n::t("Windows Security or another antivirus program removed or locked the new plugin file. TS Media chat was not changed. "
                          "You can install the update by hand from GitHub."),
                  OpenPage);
        break;
    case ApplyError::Locked:
        showError(i18n::t("Another TeamSpeak is installing the update"), i18n::t("Try again in a moment. Nothing was changed."), TryAgain);
        break;
    default:
        showError(i18n::t("The update couldn't be installed"), i18n::t("Nothing was changed. Try again, or install the update by hand from GitHub."),
                  TryAgain);
        break;
    }
}

// ---- restart -------------------------------------------------------------------------------------

void Updater::restartNow()
{
    if (!restartPending(m_layout, m_current))
        return;
    const Version pending   = effectiveInstalled(m_layout, m_current);
    StateFile     st(m_layout.stateFile());
    const QByteArray helperSha = QByteArray::fromHex(st.value("install", "helper").toLatin1());
    const QString    helper    = m_layout.stagingDir(pending) + QLatin1Char('/') + targetFileName(FileKind::Helper, m_layout.arch) + latin(".new");
    const auto       cannot    = [this] {
        showError(i18n::t("TeamSpeak couldn't be restarted automatically"), i18n::t("The update is installed. Close TeamSpeak and start it again to finish."),
                  NoAction);
    };
    if (helperSha.size() != 32 || !fs::exists(helper)) {
        log(latin("no restart helper for this update"));
        cannot();
        return;
    }
    HelperJob job;
    job.pid    = GetCurrentProcessId();
    job.exe    = processExePath();
    job.flags  = relaunchFlags(QCoreApplication::arguments().mid(1));
    job.expect = pending;
    job.from   = m_current;
    unsigned long     error  = 0;
    const LaunchError launch = startHelper(m_layout, helper, helperSha, job, &error);
    if (launch != LaunchError::None) {
        log(latin("couldn't start the restart helper (") + QString::number(static_cast<int>(launch)) + latin(", error ") + QString::number(error) + latin(")"),
            LogLevel_WARNING);
        cannot();
        return;
    }
    log(latin("restarting TeamSpeak for ") + pending.toString());
    setPhase(Phase::Restarting);

    // Close our windows, then quit through Qt's own meta-object code (string-based, queued), so no code
    // of this DLL is on the stack when TeamSpeak unloads the plugin while quitting.
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName().startsWith(QLatin1String("tsmedia")))
            w->deleteLater();
    }
    if (QAction* quit = findQuitAction(parentWindow()))
        QMetaObject::invokeMethod(quit, "trigger", Qt::QueuedConnection);
    else
        QTimer::singleShot(0, qApp, SLOT(quit()));
}

// ---- chat lines ----------------------------------------------------------------------------------

void Updater::queueNotice(const QString& text, bool warning, const QString& linkUrl, const QString& linkText, bool startupNotice)
{
    m_notices.append(Notice{text, warning, linkUrl, linkText, startupNotice});
    m_noticeUntil = QDateTime::currentDateTimeUtc().addSecs(30 * 60);
    flushNotices();
    if (!m_notices.isEmpty())
        m_noticeTimer->start();
}

void Updater::flushNotices()
{
    const uint64 sch = ts3::currentConnection();
    if (!ts3::isConnected(sch)) {
        if (QDateTime::currentDateTimeUtc() > m_noticeUntil) {
            m_noticeTimer->stop(); // shown at the next start instead (startup notices stay "pending")
            m_notices.clear();
        }
        return;
    }
    bool startup = false;
    for (const Notice& n : qAsConst(m_notices)) {
        if (n.warning)
            ts3::printWarning(sch, n.text);
        else
            ts3::printInfo(sch, n.text);
        // The address is built from a validated version; the text is the plugin's own.
        if (!n.linkUrl.isEmpty())
            ts3::print(sch, latin("[url=") + n.linkUrl + latin("]") + n.linkText + latin("[/url]"));
        startup = startup || n.startupNotice;
    }
    m_notices.clear();
    m_noticeTimer->stop();
    if (startup)
        markNoticeShown(m_layout);
}

} // namespace upd
