// TeamSpeak 3 plugin entry points. Everything heavy lives in Core / ChatIntegration;
// this file only adapts the C API and moves work onto the Qt GUI thread.

#include <QApplication>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <cstring>

#include "chatintegration.h"
#include "core.h"
#include "i18n.h"
#include "inlinemedia.h"
#include "previewrenderer.h"
#include "settings.h"
#include "settingsdialog.h"
#include "ts3api.h"
#include "version.h"
#include "video/mfvideo.h"

#define PLUGIN_API_VERSION 26
#define TS3_EXPORT extern "C" __declspec(dllexport)

namespace {

QPointer<Core>            g_core;
QPointer<ChatIntegration> g_chat;
QPointer<SettingsDialog>  g_settings;
bool                      g_mediaFoundation = false; // mf::startup() succeeded (GUI thread only)

enum MenuId { MenuSend = 1, MenuSettings, MenuCache };

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
    QObject::connect(dialog, &SettingsDialog::settingsChanged, dialog, [] {
        if (g_chat)
            g_chat->refreshAll();
    });
    g_settings = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void printHelp(uint64 sch)
{
    ts3::printInfo(sch, i18n::t(TSMEDIA_NAME " " TSMEDIA_VERSION " — commands: /tsmedia send | settings | cache | debug",
                                TSMEDIA_NAME " " TSMEDIA_VERSION " — دستورها: /tsmedia send | settings | cache | debug"));
    ts3::printInfo(sch, i18n::t("Drop files on the chat or paste a screenshot (Ctrl+V) into the chat line to send it.",
                                "برای ارسال، فایل را روی چت رها کنید یا اسکرین‌شات را با Ctrl+V در خط چت بچسبانید."));
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
    QTimer::singleShot(2000, g_core.data(), [sch, attempt] { selfTestPlay(sch, attempt + 1); });
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

    Settings::instance().load(); // also selects the UI language, before menus are created

    auto* core = new Core;
    auto* chat = new ChatIntegration(core);
    if (QThread::currentThread() != qApp->thread()) {
        core->moveToThread(qApp->thread());
        chat->moveToThread(qApp->thread());
    }
    g_core = core;
    g_chat = chat;

    QMetaObject::invokeMethod(core, [core, chat] {
        g_mediaFoundation = mf::startup();
        if (!g_mediaFoundation)
            ts3::log(QStringLiteral("Media Foundation is not available: videos can't be played inside the chat"), LogLevel_WARNING);
        core->start();
        chat->start();
        ts3::log(QStringLiteral(TSMEDIA_NAME " " TSMEDIA_VERSION " loaded"));
    }, Qt::QueuedConnection);
    return 0;
}

TS3_EXPORT void ts3plugin_shutdown()
{
    auto cleanup = [] {
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

        delete g_core.data(); // waits for probe workers

        // Every mf::VideoPlayer is gone now (viewer windows, inline players).
        if (g_mediaFoundation) {
            mf::shutdown();
            g_mediaFoundation = false;
        }
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
    } else if (cmd == QLatin1String("settings")) {
        onGuiThread([] { showSettings(nullptr); });
    } else if (cmd == QLatin1String("cache")) {
        onGuiThread([] {
            if (g_core)
                g_core->openCacheFolder();
        });
    } else if (cmd == QLatin1String("debug")) {
        onGuiThread([sch] {
            if (!g_chat)
                return;
            const QString path = ts3::dataDir() + QStringLiteral("/widget_dump.txt");
            QFile         file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                file.write(g_chat->dumpWidgetTree().toUtf8());
                ts3::printInfo(sch, i18n::t("Widget dump written to %1", "فهرست ویجت‌ها در %1 ذخیره شد").arg(path));
            }
        });
    } else {
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
        MenuId  id;
        QString text;
    };
    const Item items[] = {
        {MenuSend, i18n::t("Send file / image to chat…", "ارسال فایل یا تصویر به چت…")},
        {MenuSettings, i18n::t("Settings…", "تنظیمات…")},
        {MenuCache, i18n::t("Open media cache folder", "باز کردن پوشهٔ کش رسانه‌ها")},
    };
    constexpr size_t count = sizeof(items) / sizeof(items[0]);

    *menuItems = static_cast<PluginMenuItem**>(malloc(sizeof(PluginMenuItem*) * (count + 1)));
    for (size_t i = 0; i < count; ++i) {
        auto* item = static_cast<PluginMenuItem*>(malloc(sizeof(PluginMenuItem)));
        item->type = PLUGIN_MENU_TYPE_GLOBAL;
        item->id   = items[i].id;
        copyText(item->text, PLUGIN_MENU_BUFSZ, items[i].text);
        copyText(item->icon, PLUGIN_MENU_BUFSZ, QString());
        (*menuItems)[i] = item;
    }
    (*menuItems)[count] = nullptr;
    *menuIcon           = nullptr;
}

TS3_EXPORT void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys)
{
    *hotkeys = static_cast<PluginHotkey**>(malloc(sizeof(PluginHotkey*) * 2));
    auto* hk = static_cast<PluginHotkey*>(malloc(sizeof(PluginHotkey)));
    copyText(hk->keyword, PLUGIN_HOTKEY_BUFSZ, QStringLiteral("tsmedia_send"));
    copyText(hk->description, PLUGIN_HOTKEY_BUFSZ, i18n::t("Send file / image to the current chat", "ارسال فایل یا تصویر به چت فعلی"));
    (*hotkeys)[0] = hk;
    (*hotkeys)[1] = nullptr;
}

TS3_EXPORT void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID)
{
    Q_UNUSED(serverConnectionHandlerID);
    Q_UNUSED(type);
    Q_UNUSED(selectedItemID);
    switch (menuItemID) {
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
    default:
        break;
    }
}

TS3_EXPORT void ts3plugin_onHotkeyEvent(const char* keyword)
{
    if (str(keyword) == QLatin1String("tsmedia_send")) {
        onGuiThread([] {
            if (g_chat)
                g_chat->pickAndSendFiles();
        });
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
            QTimer::singleShot(3000, g_core.data(), [sch] { selfTestUpload(sch); });
            QTimer::singleShot(6000, g_core.data(), [sch] {
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
    if (newStatus != STATUS_DISCONNECTED)
        return;
    onGuiThread([sch] {
        if (g_core)
            g_core->onConnectionLost(sch);
    });
}

TS3_EXPORT int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID, anyID fromID, const char* fromName, const char* fromUniqueIdentifier, const char* message, int ffIgnored)
{
    Q_UNUSED(targetMode);
    Q_UNUSED(toID);
    Q_UNUSED(fromID);
    Q_UNUSED(fromName);
    Q_UNUSED(fromUniqueIdentifier);
    if (ffIgnored)
        return 0;
    const QString text = str(message);
    if (!text.contains(QLatin1String("ts3file"), Qt::CaseInsensitive))
        return 0;
    const uint64 sch = serverConnectionHandlerID;
    onGuiThread([sch, text] {
        if (g_core)
            g_core->onTextMessage(sch, text);
    });
    return 0; // never hide the message: the link is the fallback for clients without the plugin
}

TS3_EXPORT int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage)
{
    Q_UNUSED(extraMessage);
    const QString rc = str(returnCode);
    if (rc.isEmpty() || !g_core || !g_core->isOwnReturnCode(rc))
        return 0;
    const uint64  sch = serverConnectionHandlerID;
    const QString msg = str(errorMessage);
    onGuiThread([sch, error, rc, msg] {
        if (g_core)
            g_core->onServerError(sch, error, rc, msg, false);
    });
    return 1; // handled: the plugin shows its own, friendlier message
}

TS3_EXPORT int ts3plugin_onServerPermissionErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, unsigned int failedPermissionID)
{
    Q_UNUSED(failedPermissionID);
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
    const QString msg = str(statusMessage);
    const uint64  sch = serverConnectionHandlerID;
    onGuiThread([transferID, status, msg, sch] {
        if (g_core)
            g_core->onTransferStatus(transferID, status, msg, sch);
    });
}
