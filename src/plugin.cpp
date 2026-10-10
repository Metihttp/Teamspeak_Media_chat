// TeamSpeak 3 plugin entry points. Everything heavy lives in Core / ChatIntegration;
// this file only adapts the C API and moves work onto the Qt GUI thread.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <cstdlib>
#include <cstring>

#include "accessgroup.h" // 2.2 servergroup
#include "chatintegration.h"
#include "composesettings.h" // 2.2 compose
#include "compresssettings.h" // 2.4 compress
#include "core.h"
#include "datasaver.h" // 2.2 per-server settings
#include "diagnosticscollect.h" // 2.2 diagnostics
#include "diagnosticsdialog.h"  // 2.2 diagnostics
#include "fileverify.h"         // 2.2 sha: its diagnostics section
#include "i18n.h"
#include "inlinemedia.h"
#include "ownedtimer.h"
#include "peerhub.h"        // 2.2 protocol
#include "pluginlog.h"
#include "previewrenderer.h"
#include "privacysection.h" // 2.2 protocol
#include "settings.h"
#include "settingsdialog.h"
#include "spoilersection.h" // 2.2 spoiler
#include "ts3api.h"
#include "update/updateinstaller.h" // 2.2 updater
#include "update/updater.h"         // 2.2 updater
#include "version.h"
#include "video/mfvideo.h"
#include "voicecontroller.h" // 2.2 voice
#include "voicesection.h"    // 2.2 voice

#define PLUGIN_API_VERSION 26
#define TS3_EXPORT extern "C" __declspec(dllexport)

namespace {

QPointer<Core>              g_core;
QPointer<ChatIntegration>   g_chat;
QPointer<SettingsDialog>    g_settings;
QPointer<upd::Updater>      g_updater;                 // 2.2 updater
QPointer<AccessGroup>       g_access;                  // 2.2 servergroup
QPointer<DiagnosticsDialog> g_diagnostics;             // 2.2 diagnostics
QPointer<PeerHub>           g_peers;                   // 2.2 protocol: presence and reactions
bool                        g_mediaFoundation = false; // mf::startup() succeeded (GUI thread only)

// Explicit values: each feature owns its ids, and existing ones stay stable.
enum MenuId {
    MenuSend            = 1,
    MenuSettings        = 2,
    MenuCache           = 3,
    MenuUpdate          = 4,  // 2.2 updater (official builds only)
    MenuPauseDownloads  = 5,  // 2.2 data saver
    MenuResumeDownloads = 6,  // 2.2 data saver
    MenuVoice           = 7,  // 2.2 voice: Record voice message… (enabled while connected)
    MenuGiveAccess      = 20, // 2.2 servergroup: client context menu (AccessGroup greys them)
    MenuRemoveAccess    = 21, // 2.2 servergroup
};

template <typename Fn>
void onGuiThread(Fn&& fn)
{
    if (g_core)
        QMetaObject::invokeMethod(g_core.data(), std::forward<Fn>(fn), Qt::QueuedConnection);
}

QString str(const char* s)
{
    return s ? QString::fromUtf8(s) : QString();
}

// Copies UTF-8 text into a fixed TeamSpeak buffer without cutting a multi-byte character in half.
void copyText(char* dst, size_t size, const QString& text)
{
    QByteArray utf8 = text.toUtf8();
    if (static_cast<size_t>(utf8.size()) >= size) {
        int cut = static_cast<int>(size) - 1;
        while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80)
            --cut;
        utf8.truncate(cut);
    }
    memcpy(dst, utf8.constData(), static_cast<size_t>(utf8.size()));
    dst[utf8.size()] = '\0';
}

void updateMenus();

void showSettings(QWidget* parent)
{
    if (!g_core)
        return;
    if (g_settings) {
        g_settings->raise();
        g_settings->activateWindow();
        return;
    }
    // Heap allocated and non-blocking so plugin shutdown can always close it.
    auto* dialog = new SettingsDialog(g_core, parent ? parent : (g_chat ? g_chat->mainWindow() : nullptr));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->addSection(SettingsDialog::Tab::Sending, new ComposeSettingsSection(dialog)); // 2.2 compose
    dialog->addSection(SettingsDialog::Tab::ReceivingPlayback, new SpoilerSection(dialog)); // 2.2 spoiler
    dialog->addSection(SettingsDialog::Tab::PrivacyUpdates, new PrivacySection, true); // 2.2 protocol: Privacy above Updates
    dialog->addSection(SettingsDialog::Tab::Sending, new CompressSettingsSection(dialog, g_core)); // 2.4 compress
    dialog->addSection(SettingsDialog::Tab::General, new VoiceSection(dialog)); // 2.2 voice (General has the room: Sending is full)
    QObject::connect(dialog, &SettingsDialog::settingsChanged, dialog, [] {
        if (g_peers)
            g_peers->applySettings(); // 2.2 protocol: HELLO or BYE when presence was switched
        if (g_core) {
            g_core->applyCacheLimit();    // a lower limit counts now, not after the next download
            g_core->onDataSaverChanged(); // 2.2 data saver: may stop or release automatic downloads
            g_core->spoilerSettingChanged(); // 2.2 spoiler: an open viewer follows "without blurring" too
        }
        if (g_chat)
            g_chat->refreshAll();
        updateMenus(); // 2.2 data saver: the Pause / Resume pair
    });
    g_settings = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// 2.2 diagnostics: one "Diagnostic info" window (/tsmedia diag, Settings → Diagnostic info…). Owned
// by TeamSpeak's main window, so it stays open when the settings close.
void showDiagnostics(QWidget* parent)
{
    if (!g_core)
        return;
    if (g_diagnostics) {
        g_diagnostics->raise();
        g_diagnostics->activateWindow();
        return;
    }
    diag::Environment env;
    env.core                   = g_core;
    env.chat                   = g_chat;
    env.mediaFoundationStarted = g_mediaFoundation;
    env.pluginApi              = PLUGIN_API_VERSION;
    QWidget* owner             = g_chat && g_chat->mainWindow() ? g_chat->mainWindow() : parent;
    auto*    dialog            = new DiagnosticsDialog(env, owner); // deletes itself on close
    g_diagnostics              = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// One line per command. Only the first line carries the plugin's name (printInfo adds it); "debug"
// is for bug reports and not listed.
void printHelp(uint64 sch)
{
    ts3::printInfo(sch, i18n::t(TSMEDIA_VERSION " — commands:"));
    const char* const commands[] = {
        "[b]/tsmedia send[/b] — choose files to send to this chat (opens the send window)",
        "[b]/tsmedia voice[/b] — record a voice message for this chat", // 2.2 voice
        "[b]/tsmedia cancel[/b] — cancel all running uploads",
        "[b]/tsmedia settings[/b] — open the settings",
        "[b]/tsmedia cache[/b] — open the media cache folder",
        "[b]/tsmedia update[/b] — check for a new version", // 2.2 updater (official builds only)
        "[b]/tsmedia datasaver[/b] on | off | default — pause or resume automatic downloads on this server", // 2.2 data saver
        "[b]/tsmedia diag[/b] — copy diagnostic info for a bug report", // 2.2 diagnostics
        "[b]/tsmedia help[/b] — show this list",
    };
    for (const char* line : commands) {
        if (!upd::Updater::builtIn() && strstr(line, "/tsmedia update")) // 2.2 updater
            continue;
        ts3::print(sch, i18n::t(line));
    }
    ts3::print(sch, i18n::t("Tip: drop files on the chat to send them (hold Shift to skip), or copy files or a screenshot and press Ctrl+V in the chat input."));
}

// "/tsmedia cancel" and its hotkey: the keyboard way to stop sending (the toast never takes the focus).
void cancelUploads(uint64 sch)
{
    if (!g_core)
        return;
    // Also files that are uploaded but whose message still waits for earlier ones: once those are
    // canceled, nothing may be posted after all.
    int canceled = 0;
    for (int id : g_core->uploadIds()) {
        if (g_core->canCancelUpload(id)) {
            g_core->cancelUpload(id);
            ++canceled;
        }
    }
    const uint64 where = sch ? sch : ts3::currentConnection();
    if (canceled == 0)
        ts3::printInfo(where, i18n::t("No uploads are running."));
    else if (canceled == 1)
        ts3::printInfo(where, i18n::t("Canceled 1 upload."));
    else
        ts3::printInfo(where, i18n::t("Canceled %1 uploads.").arg(canceled));
}

// 2.2 updater: uploads a TeamSpeak restart would cancel. Posting too: the file is on the server, but
// its chat message may still wait (for earlier files, the album, or the flood governor) and would never
// be sent.
int runningUploads()
{
    int running = 0;
    if (g_core) {
        for (int id : g_core->uploadIds()) {
            const UploadJob* job = g_core->upload(id);
            if (job
                && (job->state == UploadState::Preparing || job->state == UploadState::Compressing || job->state == UploadState::Uploading
                    || job->state == UploadState::Posting))
                ++running;
        }
    }
    return running;
}

// "Send files to chat…" is only offered while the current server tab is connected.
void updateMenus()
{
    if (!ts3::funcs.setPluginMenuEnabled || ts3::pluginId.isEmpty())
        return;
    const QByteArray id = ts3::pluginId.toUtf8();
    ts3::funcs.setPluginMenuEnabled(id.constData(), MenuSend, ts3::isConnected(ts3::currentConnection()) ? 1 : 0);
    ts3::funcs.setPluginMenuEnabled(id.constData(), MenuVoice, ts3::isConnected(ts3::currentConnection()) ? 1 : 0); // 2.2 voice
    datasaver::updateMenu(MenuPauseDownloads, MenuResumeDownloads); // 2.2 data saver
}

// 2.2 per-server settings: after the Plugins menu or a command changed a server's data saver.
void serverSettingsChanged()
{
    if (g_core)
        g_core->onDataSaverChanged();
    updateMenus();
    if (g_settings)
        g_settings->reloadServers(); // rows not edited in an open dialog show the new value
}

// 2.2 voice: the recorder for the visible chat (menu, command, hotkey).
void recordVoice(VoiceController::Origin origin)
{
    if (!g_chat || !g_chat->voice())
        return;
    if (origin == VoiceController::Origin::Hotkey)
        g_chat->voice()->hotkey();
    else
        g_chat->voice()->start(origin);
}

#ifdef TSMEDIA_TESTHOOKS
bool isLocalServer(uint64 sch)
{
    QString host;
    quint16 port = 0;
    return ts3::getServerAddress(sch, &host, &port)
        && (host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1"));
}

// Reads and deletes <data dir>/<name>. False if there is no such trigger file.
bool takeTrigger(const QString& name, QString* content)
{
    QFile trigger(ts3::dataDir() + QLatin1Char('/') + name);
    if (!trigger.open(QIODevice::ReadOnly))
        return false;
    if (content)
        *content = QString::fromUtf8(trigger.readAll());
    trigger.close();
    trigger.remove();
    return true;
}

// <data dir>/selftest_upload.txt names files to upload to the channel after connecting to a
// localhost server.
void selfTestUpload(uint64 sch)
{
    QString content;
    if (!takeTrigger(QStringLiteral("selftest_upload.txt"), &content))
        return;
    if (!isLocalServer(sch)) {
        ts3::log(QStringLiteral("[test] self-test upload skipped: not a localhost server"));
        return;
    }
    QStringList paths;
    for (const QString& f : content.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        if (!f.trimmed().isEmpty())
            paths << f.trimmed();
    }
    ts3::log(QStringLiteral("[test] self-test upload of ") + paths.join(QStringLiteral(", ")));
    ChatTarget target;
    target.sch = sch;
    if (g_core)
        g_core->uploadFiles(paths, target);
}

// <data dir>/selftest_play.txt: presses play on the most recent video of this server's chat.
// Retries for a while when no video has shown up yet (e.g. the self-test upload is still running).
void selfTestPlay(uint64 sch, int attempt)
{
    if (!g_core || !g_chat || !g_chat->media())
        return;
    const QString     serverUid = ts3::serverUid(sch);
    const QStringList keys      = g_core->keys();
    for (int i = keys.size() - 1; i >= 0; --i) {
        const MediaEntry* e = g_core->entry(keys.at(i));
        if (!e || e->kind != MediaKind::Video || (!serverUid.isEmpty() && e->link.serverUid != serverUid))
            continue;
        ts3::log(QStringLiteral("[test] self-test play: %1 (%2)").arg(e->link.fileName, keys.at(i)));
        g_chat->media()->click(keys.at(i), VideoZone::Body, 0.0);
        return;
    }
    if (attempt >= 15) {
        ts3::log(QStringLiteral("[test] self-test play: no video found in the chat"));
        return;
    }
    ts3::log(QStringLiteral("[test] self-test play: waiting for a video (attempt %1)").arg(attempt + 1));
    singleShotOwned(2000, g_core.data(), [sch, attempt] { selfTestPlay(sch, attempt + 1); });
}

// 2.2 voice: <data dir>/selftest_voice.txt ("<ms>[;send][;silence][;unplug@<ms>]") records a voice
// message from generated sound (FakeCapture, never the microphone) in the visible chat.
void selfTestVoice(uint64 sch)
{
    QString content;
    if (!takeTrigger(QStringLiteral("selftest_voice.txt"), &content))
        return;
    if (!isLocalServer(sch)) {
        ts3::log("[test] self-test voice skipped: not a localhost server");
        return;
    }
    QFile fake(ts3::dataDir() + QStringLiteral("/voice_fake.txt"));
    if (fake.open(QIODevice::WriteOnly | QIODevice::Truncate))
        fake.write(content.trimmed().toUtf8());
    fake.close();
    recordVoice(VoiceController::Origin::Command);
}
#endif

} // namespace

// ============================================================================================
// Required functions
// ============================================================================================

TS3_EXPORT const char* ts3plugin_name()
{
    return TSMEDIA_NAME;
}

TS3_EXPORT const char* ts3plugin_version()
{
    return TSMEDIA_VERSION;
}

TS3_EXPORT int ts3plugin_apiVersion()
{
    return PLUGIN_API_VERSION;
}

TS3_EXPORT const char* ts3plugin_author()
{
    return TSMEDIA_AUTHOR;
}

TS3_EXPORT const char* ts3plugin_description()
{
    return TSMEDIA_DESCRIPTION;
}

TS3_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3::funcs = funcs;
}

TS3_EXPORT int ts3plugin_init()
{
    if (!qApp)
        return 1;

    plog::start(ts3::dataDir() + QStringLiteral("/logs")); // 2.2 foundation: the plugin log file, before anything logs
    // 2.2 updater: before anything else, a just-updated version whose previous start never finished
    // (a crash in init) puts the previous version back; returning 1 makes TeamSpeak unload it.
    if (upd::bootGuard(upd::Layout::forRunningPlugin(ts3::dataDir()), upd::Version::parse(QString::fromLatin1(TSMEDIA_VERSION))) == upd::BootGuard::RolledBack) {
        ts3::log(QString::fromLatin1("[update] " TSMEDIA_VERSION " didn't start last time: the previous version was restored"), LogLevel_WARNING);
        plog::shutdown(); // TeamSpeak unloads the DLL without calling ts3plugin_shutdown
        return 1;
    }
#ifdef TSMEDIA_TESTHOOKS
    // 2.2 updater rollback tests (test builds only): <data>/selftest_init_fail.txt makes this start fail
    // (init returns 1), selftest_init_crash.txt makes it crash. Each trigger is used once.
    if (takeTrigger(QString::fromLatin1("selftest_init_fail.txt"), nullptr)) {
        ts3::log(QString::fromLatin1("[test] init fails on purpose"), LogLevel_WARNING);
        plog::shutdown();
        return 1;
    }
    if (takeTrigger(QString::fromLatin1("selftest_init_crash.txt"), nullptr)) {
        ts3::log(QString::fromLatin1("[test] init crashes on purpose"), LogLevel_WARNING);
        *static_cast<volatile int*>(nullptr) = 0;
    }
#endif

    Settings::instance().load();

    auto* core = new Core;
    auto* chat = new ChatIntegration(core);
    if (QThread::currentThread() != qApp->thread()) {
        core->moveToThread(qApp->thread());
        chat->moveToThread(qApp->thread());
    }
    g_core = core;
    g_chat = chat;

    // 2.2 servergroup: Settings > Server access and the Give / Remove TS Media chat access menu items
    g_access = new AccessGroup(MenuGiveAccess, MenuRemoveAccess);
    if (QThread::currentThread() != qApp->thread())
        g_access->moveToThread(qApp->thread());

    QMetaObject::invokeMethod(core, [core, chat] {
        diag::startSessionStats(core);              // 2.2 diagnostics: counts from the start (a child of core)
        diag::setDialogOpener(&showDiagnostics);    // 2.2 diagnostics: Settings → Diagnostic info…
        // 2.2 diagnostics: recent lines from the plugin log (names marked, so they can be left out).
        // Called on the dialog's worker thread; plog is thread-safe.
        diag::setLogTailProvider([](int maxLines) {
            const QStringList all = plog::tail(100000); // at most two files of 256 KB
            return diag::pluginLogTail(all.mid(qMax(0, all.size() - maxLines)), all.size());
        });
        g_mediaFoundation = mf::startup();
        if (!g_mediaFoundation)
            ts3::log("Media Foundation is not available: videos can't be played inside the chat", LogLevel_WARNING);
        core->start();
        // 2.2 protocol: after Core (its flood governors), before the chat (reaction rows). start()
        // registers the send window's presence line (compose::setPresenceLineFactory); prepareShutdown
        // and ~PeerHub clear it again.
        auto* peers = new PeerHub(core);
        g_peers     = peers;
        peers->start();
        // 2.2 sha and protocol: their sections of the diagnostic info (counts only; called on the GUI
        // thread while it is collected, and the dialog is closed before PeerHub goes at shutdown).
        diag::addSectionProvider([]() -> diag::Section { return {fileverify::diagnosticsTitle(), fileverify::diagnosticLines()}; });
        diag::addSectionProvider([]() -> diag::Section {
            PeerHub* hub = PeerHub::instance();
            return hub ? diag::Section{PeerHub::diagnosticsTitle(), hub->diagnosticLines()} : diag::Section();
        });
        // 2.2 compression and voice: their sections too (GUI thread; both objects outlive the dialog).
        diag::addSectionProvider([]() -> diag::Section {
            return g_core ? diag::Section{Core::compressionDiagnosticsTitle(), g_core->compressionDiagnostics()} : diag::Section();
        });
        diag::addSectionProvider([]() -> diag::Section {
            VoiceController* voice = g_chat ? g_chat->voice() : nullptr;
            return voice ? diag::Section{VoiceController::diagnosticsTitle(), voice->diagnosticLines()} : diag::Section();
        });
        chat->start();
        if (chat->voice()) // 2.2 voice: "Open TS Media settings" in the recorder's errors
            QObject::connect(chat->voice(), &VoiceController::settingsRequested, chat, [] { showSettings(nullptr); });
        updateMenus();
        // 2.2 updater: this start counts as successful (started marker, applied -> done), then the
        // consent window / daily check are scheduled. Deleted first in ts3plugin_shutdown.
        g_updater = new upd::Updater(ts3::dataDir(), upd::Updater::Hooks{[] { return runningUploads(); }});
        g_updater->start();
        ts3::log(TSMEDIA_NAME " " TSMEDIA_VERSION " loaded");
    }, Qt::QueuedConnection);
    return 0;
}

TS3_EXPORT void ts3plugin_shutdown()
{
    // The order matters; each step may still use what the later ones delete:
    //   1. presence BYE and saved reactions          5. our windows (viewer, settings, send window, editor)
    //   2. a voice recording (mic given back)        6. the chat (inline players), leftovers in TeamSpeak
    //   3. compressions (transcode worker joined)    7. AccessGroup, PeerHub, then Core (its pools)
    //   4. diagnostics hooks, the updater            8. Media Foundation, then the plugin log
    auto cleanup = [] {
        // 2.2 protocol: BYE where we said hello (not waiting for an answer), reactions.json written.
        if (g_peers)
            g_peers->prepareShutdown();

        // 2.2 voice: a recording stops first: its capture or encode worker is joined and TeamSpeak's
        // microphone given back (MicGuard) while every connection is still there; its window goes too.
        if (g_chat && g_chat->voice())
            g_chat->voice()->shutdown();
        // 2.4 compress: running and queued transcodes are canceled and their worker joined before any
        // window, the chat or Core goes (~Core would do it too, last).
        if (g_core)
            g_core->stopCompressions();

        diag::setDialogOpener(nullptr); // 2.2 diagnostics (its window is closed below and waits for its worker)
        diag::setLogTailProvider(nullptr);
        // 2.2 updater: cancels a running check or download (waits at most 1 s; a job still in a network
        // read keeps the DLL pinned and ends on its own) and closes its windows.
        delete g_updater.data();

        // Our windows must be gone before the DLL is unloaded. Deleting one window can delete
        // another (owned dialogs), hence the guarded second pass.
        QList<QPointer<QWidget>> windows;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName().startsWith(QLatin1String("tsmedia")))
                windows.append(w);
        }
        for (const QPointer<QWidget>& w : windows)
            delete w.data();

        delete g_chat.data(); // destroys the inline players

        // Anything of ours still living inside TeamSpeak's widgets (e.g. the upload toast in a chat
        // browser) would outlive the DLL's code.
        QList<QPointer<QWidget>> leftovers;
        for (QWidget* w : QApplication::allWidgets()) {
            if (w->objectName().startsWith(QLatin1String("tsmedia")))
                leftovers.append(w);
        }
        for (const QPointer<QWidget>& w : leftovers)
            delete w.data();

        delete g_access.data(); // 2.2 servergroup: stops its timers and a running icon upload
        delete g_peers.data(); // 2.2 protocol: the store, the presence directory, the transport

        delete g_core.data(); // waits for probe workers

        // Every mf::VideoPlayer is gone now (viewer windows, inline players).
        if (g_mediaFoundation) {
            mf::shutdown();
            g_mediaFoundation = false;
        }

        // 2.2 foundation: last, so the steps above can still log.
        plog::shutdown();
    };
    if (QThread::currentThread() == qApp->thread())
        cleanup();
    else
        QMetaObject::invokeMethod(qApp, cleanup, Qt::BlockingQueuedConnection);
}

// ============================================================================================
// Optional functions
// ============================================================================================

TS3_EXPORT int ts3plugin_offersConfigure()
{
    return PLUGIN_OFFERS_CONFIGURE_QT_THREAD;
}

TS3_EXPORT void ts3plugin_configure(void* handle, void* qParentWidget)
{
    Q_UNUSED(handle);
    showSettings(static_cast<QWidget*>(qParentWidget));
}

TS3_EXPORT void ts3plugin_registerPluginID(const char* id)
{
    ts3::pluginId = str(id);
}

TS3_EXPORT const char* ts3plugin_commandKeyword()
{
    return "tsmedia";
}

TS3_EXPORT int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command)
{
    const QString cmd = str(command).trimmed().toLower();
    const uint64  sch = serverConnectionHandlerID;

    if (cmd == QLatin1String("send")) {
        onGuiThread([] {
            if (g_chat)
                g_chat->pickAndSendFiles();
        });
    } else if (cmd == QLatin1String("voice")) { // 2.2 voice
        onGuiThread([] { recordVoice(VoiceController::Origin::Command); });
    } else if (cmd == QLatin1String("settings")) {
        onGuiThread([] { showSettings(nullptr); });
    } else if (cmd == QLatin1String("cache")) {
        onGuiThread([] {
            if (g_core)
                g_core->openCacheFolder();
        });
    } else if (cmd == QLatin1String("cancel")) {
        onGuiThread([sch] { cancelUploads(sch); });
    } else if (cmd == QLatin1String("update")) { // 2.2 updater
        onGuiThread([sch] {
            if (g_updater && upd::Updater::builtIn())
                g_updater->checkNow(upd::Updater::Origin::Chat);
            else
                ts3::printInfo(sch, i18n::t("Updates are turned off in versions you build yourself. Check "
                                            "github.com/Metihttp/Teamspeak_Media_chat/releases for new versions."));
        });
    } else if (cmd == QLatin1String("datasaver") || cmd.startsWith(QLatin1String("datasaver "))) {
        // 2.2 data saver: "/tsmedia datasaver [on|off|default]" (the argument is checked there).
        const QString arguments = cmd.mid(9).trimmed().left(40);
        onGuiThread([sch, arguments] {
            if (datasaver::runCommand(sch, arguments))
                serverSettingsChanged();
        });
    } else if (cmd == QLatin1String("diag") || cmd == QLatin1String("diagnostics")) { // 2.2 diagnostics
        onGuiThread([] { showDiagnostics(nullptr); });
    } else if (cmd == QLatin1String("debug")) {
        onGuiThread([sch] {
            if (!g_chat)
                return;
            const QString path = ts3::dataDir() + QStringLiteral("/widget_dump.txt");
            QFile         file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(g_chat->dumpWidgetTree().toUtf8()) >= 0)
                ts3::printInfo(sch, i18n::t("Diagnostics saved to %1. Attach this file to your bug report.").arg(QDir::toNativeSeparators(path)));
            else
                ts3::printWarning(sch, i18n::t("Couldn't save diagnostics to %1. Check that you can write to that folder.").arg(QDir::toNativeSeparators(path)));
        });
    } else {
        // A typo gets a hint instead of silently showing the list.
        if (!cmd.isEmpty() && cmd != QLatin1String("help"))
            ts3::printWarning(sch, i18n::t("Unknown command “%1”.").arg(str(command).trimmed().left(40)));
        printHelp(sch);
    }
    return 0;
}

TS3_EXPORT void ts3plugin_freeMemory(void* data)
{
    free(data);
}

TS3_EXPORT int ts3plugin_requestAutoload()
{
    return 1;
}

TS3_EXPORT void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    struct Item {
        MenuId         id;
        QString        text;
        PluginMenuType type = PLUGIN_MENU_TYPE_GLOBAL;
    };
    const Item items[] = {
        {MenuSend, i18n::t("Send files to chat…")},
        {MenuVoice, i18n::t("Record voice message…")}, // 2.2 voice: next to the other way of sending
        // 2.2 data saver: an enable/disable pair (the SDK can't change a menu item's text or check mark).
        {MenuPauseDownloads, i18n::t("Pause automatic downloads on this server")},
        {MenuResumeDownloads, i18n::t("Resume automatic downloads on this server")},
        {MenuSettings, i18n::t("Settings…")},
        {MenuCache, i18n::t("Open media cache folder")},
        {MenuUpdate, i18n::t("Check for updates…")}, // 2.2 updater (official builds only)
        {MenuGiveAccess, i18n::t("Give TS Media chat access"), PLUGIN_MENU_TYPE_CLIENT},     // 2.2 servergroup
        {MenuRemoveAccess, i18n::t("Remove TS Media chat access"), PLUGIN_MENU_TYPE_CLIENT}, // 2.2 servergroup
    };
    QVector<const Item*> shown;
    for (const Item& item : items) {
        if (item.id != MenuUpdate || upd::Updater::builtIn())
            shown.append(&item);
    }
    const int count = shown.size();

    *menuItems = static_cast<PluginMenuItem**>(malloc(sizeof(PluginMenuItem*) * (count + 1)));
    for (int i = 0; i < count; ++i) {
        auto* item = static_cast<PluginMenuItem*>(malloc(sizeof(PluginMenuItem)));
        item->type = shown.at(i)->type;
        item->id   = shown.at(i)->id;
        copyText(item->text, PLUGIN_MENU_BUFSZ, shown.at(i)->text);
        copyText(item->icon, PLUGIN_MENU_BUFSZ, QString());
        (*menuItems)[i] = item;
    }
    (*menuItems)[count] = nullptr;
    *menuIcon           = nullptr;
}

TS3_EXPORT void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys)
{
    struct Hotkey {
        const char* keyword;
        QString     description;
    };
    const Hotkey keys[] = {
        {"tsmedia_send", i18n::t("Send files to the current chat")},
        {"tsmedia_cancel", i18n::t("Cancel all uploads")},
        {VoiceSection::kHotkeyKeyword, i18n::t("Record a voice message (press to start, press again to stop)")}, // 2.2 voice
    };
    constexpr size_t count = sizeof(keys) / sizeof(keys[0]);

    *hotkeys = static_cast<PluginHotkey**>(malloc(sizeof(PluginHotkey*) * (count + 1)));
    for (size_t i = 0; i < count; ++i) {
        auto* hk = static_cast<PluginHotkey*>(malloc(sizeof(PluginHotkey)));
        copyText(hk->keyword, PLUGIN_HOTKEY_BUFSZ, QString::fromLatin1(keys[i].keyword));
        copyText(hk->description, PLUGIN_HOTKEY_BUFSZ, keys[i].description);
        (*hotkeys)[i] = hk;
    }
    (*hotkeys)[count] = nullptr;
}

TS3_EXPORT void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID)
{
    Q_UNUSED(type);
    if (AccessGroup::handleMenuItem(serverConnectionHandlerID, menuItemID, selectedItemID)) // 2.2 servergroup
        return;
    Q_UNUSED(selectedItemID);
    const uint64 sch = serverConnectionHandlerID; // the current server tab
    switch (menuItemID) {
    case MenuPauseDownloads: // 2.2 data saver
    case MenuResumeDownloads:
        onGuiThread([sch, pause = menuItemID == MenuPauseDownloads] {
            if (datasaver::setFromMenu(sch, pause))
                serverSettingsChanged();
            else
                updateMenus(); // e.g. a stale pair after a reload
        });
        break;
    case MenuSend:
        onGuiThread([] {
            if (g_chat)
                g_chat->pickAndSendFiles();
        });
        break;
    case MenuSettings:
        onGuiThread([] { showSettings(nullptr); });
        break;
    case MenuCache:
        onGuiThread([] {
            if (g_core)
                g_core->openCacheFolder();
        });
        break;
    case MenuUpdate: // 2.2 updater: the result shows in Settings → Updates (or the update dialog)
        onGuiThread([] {
            showSettings(nullptr);
            if (g_settings) // the dialog opens on the tab used last; the result is on this one
                g_settings->showTab(SettingsDialog::Tab::PrivacyUpdates);
            if (g_updater)
                g_updater->checkNow(upd::Updater::Origin::Settings);
        });
        break;
    case MenuVoice: // 2.2 voice
        onGuiThread([] { recordVoice(VoiceController::Origin::Menu); });
        break;
    default:
        break;
    }
}

TS3_EXPORT void ts3plugin_onHotkeyEvent(const char* keyword)
{
    const QString key = str(keyword);
    if (key == QLatin1String("tsmedia_send")) {
        onGuiThread([] {
            if (g_chat)
                g_chat->pickAndSendFiles();
        });
    } else if (key == QLatin1String("tsmedia_cancel")) {
        onGuiThread([] { cancelUploads(0); });
    } else if (key == QLatin1String(VoiceSection::kHotkeyKeyword)) { // 2.2 voice
        onGuiThread([] { recordVoice(VoiceController::Origin::Hotkey); });
    }
}

// ============================================================================================
// Client events
// ============================================================================================

TS3_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber)
{
    Q_UNUSED(errorNumber);
    const uint64 sch = serverConnectionHandlerID;
#ifdef TSMEDIA_TESTHOOKS
    // Test builds only, and only against localhost servers (checked when the hooks fire).
    if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
        onGuiThread([sch] {
            singleShotOwned(3000, g_core.data(), [sch] {
                selfTestUpload(sch);
                selfTestVoice(sch); // 2.2 voice
            });
            singleShotOwned(6000, g_core.data(), [sch] {
                if (!takeTrigger(QStringLiteral("selftest_play.txt"), nullptr))
                    return;
                if (!isLocalServer(sch)) {
                    ts3::log(QStringLiteral("[test] self-test play skipped: not a localhost server"));
                    return;
                }
                selfTestPlay(sch, 0);
            });
        });
    }
#endif
    PeerHub::onConnectStatus(sch, newStatus); // 2.2 protocol: HELLO once connected, forget on disconnect
    onGuiThread([] { updateMenus(); });
    AccessGroup::handleConnectStatus(sch, newStatus); // 2.2 servergroup
    if (newStatus == STATUS_CONNECTION_ESTABLISHED || newStatus == STATUS_DISCONNECTED) {
        // 2.2 per-server settings: the server's stored name, the data saver notice, Settings → Servers.
        onGuiThread([sch, newStatus] {
            if (newStatus == STATUS_CONNECTION_ESTABLISHED)
                datasaver::onConnectionEstablished(sch);
            if (g_settings)
                g_settings->reloadServers();
        });
    }
    if (newStatus != STATUS_DISCONNECTED)
        return;
    onGuiThread([sch] {
        if (g_core)
            g_core->onConnectionLost(sch);
    });
}

TS3_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID)
{
    onGuiThread([] { updateMenus(); }); // another server tab: its connection state counts now
    AccessGroup::handleCurrentConnectionChanged(serverConnectionHandlerID); // 2.2 servergroup
}

TS3_EXPORT int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID, anyID fromID, const char* fromName, const char* fromUniqueIdentifier, const char* message, int ffIgnored)
{
    Q_UNUSED(fromName);
    if (ffIgnored)
        return 0;
    const QString text = str(message);
    if (!text.contains(QLatin1String("ts3file"), Qt::CaseInsensitive))
        return 0;
    const uint64  sch         = serverConnectionHandlerID;
    const QString sender      = str(fromUniqueIdentifier); // 2.2 album: filled in by the server, so it can be trusted
    const bool    privateChat = targetMode == TextMessageTarget_CLIENT;
    onGuiThread([sch, text, sender, privateChat, toID, fromID] {
        if (g_core)
            g_core->onTextMessage(sch, text, sender);
        // 2.2 protocol: media of a private chat, so only that partner's private reactions count (S0: our
        // own private messages come here too, from our own id to the partner's).
        if (privateChat && g_peers) {
            const QString partner = fromID == ts3::ownClientId(sch) ? ts3::clientUid(sch, toID) : sender;
            QStringList   keys;
            for (const MediaLink& link : MediaLink::findInMessage(text))
                keys.append(link.key());
            g_peers->notePrivateMedia(sch, partner, keys);
        }
    });
    return 0; // never hide the message: the link is the fallback for clients without the plugin
}

TS3_EXPORT int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage)
{
    // 2.2 servergroup: answers to Server access requests (TeamSpeak's own print is suppressed only for them)
    if (AccessGroup::handleServerError(serverConnectionHandlerID, errorMessage, error, returnCode, 0, false))
        return 1;
    // 2.2 protocol: answers to plugin commands (the extra message has the flood "retry in N ms").
    if (PeerHub::onServerError(serverConnectionHandlerID, error, returnCode, extraMessage, false))
        return 1;
    const QString rc = str(returnCode);
    if (rc.isEmpty() || !g_core || !g_core->isOwnReturnCode(rc))
        return 0;
    const uint64  sch   = serverConnectionHandlerID;
    const QString msg   = str(errorMessage);
    const QString extra = str(extraMessage).left(200); // 0x020c: "retry in <n>ms" (FloodGovernor)
    onGuiThread([sch, error, rc, msg, extra] {
        if (g_core)
            g_core->onServerError(sch, error, rc, msg, false, extra);
    });
    return 1; // handled: the plugin shows its own, friendlier message
}

TS3_EXPORT int ts3plugin_onServerPermissionErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, unsigned int failedPermissionID)
{
    // 2.2 servergroup: Server access requests; the failed permission marks what needs a higher admin
    if (AccessGroup::handleServerError(serverConnectionHandlerID, errorMessage, error, returnCode, failedPermissionID, true))
        return 1;
    if (PeerHub::onServerError(serverConnectionHandlerID, error, returnCode, nullptr, true)) // 2.2 protocol
        return 1;
    const QString rc = str(returnCode);
    if (rc.isEmpty() || !g_core || !g_core->isOwnReturnCode(rc))
        return 0;
    const uint64  sch = serverConnectionHandlerID;
    const QString msg = str(errorMessage);
    onGuiThread([sch, error, rc, msg] {
        if (g_core)
            g_core->onServerError(sch, error, rc, msg, true);
    });
    return 1;
}

TS3_EXPORT void ts3plugin_onFileTransferStatusEvent(anyID transferID, unsigned int status, const char* statusMessage, uint64 remotefileSize, uint64 serverConnectionHandlerID)
{
    Q_UNUSED(remotefileSize);
    AccessGroup::handleTransferStatus(transferID, status, statusMessage, serverConnectionHandlerID); // 2.2 servergroup: its icon upload
    const QString msg = str(statusMessage);
    const uint64  sch = serverConnectionHandlerID;
    onGuiThread([transferID, status, msg, sch] {
        if (g_core)
            g_core->onTransferStatus(transferID, status, msg, sch);
    });
}

// ============================================================================================
// 2.2 protocol: plugin commands and channel membership (presence, reactions)
// ============================================================================================

TS3_EXPORT void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand, anyID invokerClientID,
                                               const char* invokerName, const char* invokerUniqueIdentity)
{
    // Checked (ours, size, magic) before anything is copied; the invoker fields come from the server.
    PeerHub::onPluginCommand(serverConnectionHandlerID, pluginName, pluginCommand, invokerClientID, invokerName, invokerUniqueIdentity);
}

TS3_EXPORT void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage)
{
    Q_UNUSED(moveMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* timeoutMessage)
{
    Q_UNUSED(timeoutMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID moverID,
                                                 const char* moverName, const char* moverUniqueIdentifier, const char* moveMessage)
{
    Q_UNUSED(moverID);
    Q_UNUSED(moverName);
    Q_UNUSED(moverUniqueIdentifier);
    Q_UNUSED(moveMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID,
                                                       const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage)
{
    Q_UNUSED(kickerID);
    Q_UNUSED(kickerName);
    Q_UNUSED(kickerUniqueIdentifier);
    Q_UNUSED(kickMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID,
                                                      const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage)
{
    Q_UNUSED(kickerID);
    Q_UNUSED(kickerName);
    Q_UNUSED(kickerUniqueIdentifier);
    Q_UNUSED(kickMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientBanFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID,
                                                     const char* kickerName, const char* kickerUniqueIdentifier, uint64 time, const char* kickMessage)
{
    Q_UNUSED(kickerID);
    Q_UNUSED(kickerName);
    Q_UNUSED(kickerUniqueIdentifier);
    Q_UNUSED(time);
    Q_UNUSED(kickMessage);
    PeerHub::onClientMove(serverConnectionHandlerID, clientID, oldChannelID, newChannelID, visibility);
}

TS3_EXPORT void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID, const char* displayName, const char* uniqueClientIdentifier)
{
    Q_UNUSED(uniqueClientIdentifier);
    PeerHub::onClientRenamed(serverConnectionHandlerID, clientID, displayName);
}
